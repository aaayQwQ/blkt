#include "BilibiliAdapter.hpp"
#include "../../core/Core.hpp"
#include "../../core/Guard.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace blkt {

BilibiliAdapter::BilibiliAdapter(Core *core, QObject *parent)
    : QObject(parent), m_core(core)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(RequestTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_reply && !m_reply->isFinished())
            m_reply->abort();
    });
}

BilibiliAdapter::~BilibiliAdapter()
{
    m_timeout.stop();
    if (m_reply && !m_reply->isFinished())
        m_reply->abort();
    clearCredentials();
}

bool BilibiliAdapter::authenticate(const QByteArray &cookie, const QString &csrf)
{
    if (!m_core || m_reply || m_authenticated
        || !Guard::validCookie(cookie) || !Guard::validCsrf(csrf)) {
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Invalid authentication state or credentials"));
        return false;
    }

    const QString cookieText = QString::fromLatin1(cookie);
    const QRegularExpression csrfPattern(
        QStringLiteral("(?:^|;\\s*)bili_jct=([^;\\s]+)"));
    const auto match = csrfPattern.match(cookieText);
    if (!match.hasMatch() || match.captured(1) != csrf) {
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("CSRF token does not match cookie"));
        return false;
    }

    m_cookie = cookie;
    m_csrf = csrf;

    QNetworkRequest request(QUrl(QStringLiteral(
        "https://api.bilibili.com/x/web-interface/nav")));
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent",
                         "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    request.setRawHeader("Referer", "https://www.bilibili.com/");
    request.setTransferTimeout(RequestTimeoutMs);

    m_reply = m_network.get(request);
    if (!m_reply) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Could not create authentication request"));
        return false;
    }

    m_reply->setReadBufferSize(MaxResponseBytes);
    connect(m_reply, &QIODevice::readyRead, this, [this] {
        if (m_reply && m_reply->bytesAvailable() > MaxResponseBytes)
            m_reply->abort();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        if (m_reply)
            finishAuthentication(m_reply);
    });
    m_timeout.start();
    return true;
}

void BilibiliAdapter::finishAuthentication(QNetworkReply *reply)
{
    if (!reply)
        return;

    m_timeout.stop();
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool responseTooLarge = body.size() > MaxResponseBytes;
    const bool transportOk = reply->error() == QNetworkReply::NoError
                             && !responseTooLarge;
    reply->deleteLater();
    m_reply.clear();

    if (!transportOk) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Bilibili authentication request failed"));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const bool apiOk = parseError.error == QJsonParseError::NoError
                       && root.value(QStringLiteral("code")).toInt(-1) == 0;
    const bool loggedIn = apiOk
                          && data.value(QStringLiteral("isLogin")).toBool(false);
    bool idOk = false;
    const quint64 mid = data.value(QStringLiteral("mid")).toVariant().toULongLong(&idOk);
    const QString name = Guard::cleanText(
        data.value(QStringLiteral("uname")).toString(), 64);

    if (!loggedIn || !idOk || mid == 0 || name.isEmpty()
        || !m_core || !m_core->login(m_cookie, m_csrf)) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Cookie is invalid, expired, or account validation failed"));
        return;
    }

    m_accountId = mid;
    m_accountName = name;
    m_authenticated = true;
    clearCredentials();

    emit authenticationFinished(true, m_accountName, m_accountId,
                                QStringLiteral("Account authenticated"));
}

void BilibiliAdapter::clearCredentials()
{
    if (!m_cookie.isEmpty())
        m_cookie.fill('\0');
    m_cookie.clear();
    m_csrf.fill(QChar(u'\0'));
    m_csrf.clear();
}

bool BilibiliAdapter::isAuthenticated() const
{
    return m_authenticated;
}

QString BilibiliAdapter::accountName() const
{
    return m_accountName;
}

quint64 BilibiliAdapter::accountId() const
{
    return m_accountId;
}

} // namespace blkt
