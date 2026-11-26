#include "Core.hpp"
#include "Guard.hpp"

#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

namespace blkt {

Core::Core(QObject *parent)
    : QObject(parent), m_network()
{
    qRegisterMetaType<blkt::MessageEvent>();
    qRegisterMetaType<blkt::GiftEvent>();
    m_clock.start();
}

Core::~Core()
{
    logout();
}

// The trusted platform login adapter must validate the remote session before calling this.
bool Core::login(const QByteArray &cookie, const QString &csrf)
{
    if (!Guard::validCookie(cookie) || !Guard::validCsrf(csrf)) {
        emit logMessage(QStringLiteral("Authentication rejected: invalid session input"));
        return false;
    }

    const QString cookieText = QString::fromLatin1(cookie);
    const QRegularExpression csrfPattern(
        QStringLiteral("(?:^|;\\s*)bili_jct=([^;\\s]+)"));
    const auto match = csrfPattern.match(cookieText);
    if (!match.hasMatch() || match.captured(1) != csrf) {
        emit logMessage(QStringLiteral("Authentication rejected: CSRF cookie mismatch"));
        return false;
    }

    // Replacing a session cancels requests created with the previous credentials.
    clearSession();
    m_cookie = cookie;
    m_csrf = csrf;
    m_loggedIn = true;
    emit loginStateChanged(true);
    emit logMessage(QStringLiteral("Bilibili account authentication succeeded"));
    return true;
}

void Core::abortActiveRequests()
{
    const auto replies = m_activeReplies;
    for (QNetworkReply *reply : replies) {
        if (reply && !reply->isFinished())
            reply->abort();
    }
    m_activeReplies.clear();
}

void Core::clearSession()
{
    ++m_sessionGeneration;
    abortActiveRequests();
    if (!m_cookie.isEmpty())
        m_cookie.fill('\0');
    m_cookie.clear();
    m_csrf.fill(QChar(u'\0'));
    m_csrf.clear();
    m_loggedIn = false;
    m_activeRoomId.clear();
    m_roomConnected = false;
    m_lastSendMs = -100000;
    m_gifts.clear();
}

void Core::logout()
{
    const bool wasLoggedIn = m_loggedIn;
    clearSession();
    if (wasLoggedIn)
        emit loginStateChanged(false);
}

bool Core::isLoggedIn() const
{
    return m_loggedIn;
}

void Core::setRoomConnection(const QString &roomId, bool connected)
{
    if (roomId.isEmpty()) {
        m_activeRoomId.clear();
        m_roomConnected = false;
        return;
    }
    if (!Guard::validRoomId(roomId))
        return;

    m_activeRoomId = roomId;
    m_roomConnected = connected;
    if (!connected)
        m_gifts.clear();
}

bool Core::sendMessage(const QString &roomId, const QString &text)
{
    if (!m_loggedIn || !m_roomConnected || roomId != m_activeRoomId
        || m_activeReplies.size() >= MaxActiveRequests || !Guard::validRoomId(roomId))
        return false;

    QString safeText = Guard::cleanText(text, 500);
    safeText.replace(QChar(u'\n'), QChar(u' '));
    safeText.replace(QChar(u'\t'), QChar(u' '));
    if (safeText.isEmpty() || safeText.size() > 500)
        return false;

    const qint64 now = m_clock.elapsed();
    if (now - m_lastSendMs < SendIntervalMs)
        return false;
    m_lastSendMs = now;

    QUrl url(QStringLiteral("https://api.live.bilibili.com/msg/send"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("roomid"), roomId);
    query.addQueryItem(QStringLiteral("msg"), safeText);
    query.addQueryItem(QStringLiteral("csrf"), m_csrf);
    query.addQueryItem(QStringLiteral("csrf_token"), m_csrf);
    query.addQueryItem(QStringLiteral("color"), QStringLiteral("16777215"));
    query.addQueryItem(QStringLiteral("fontsize"), QStringLiteral("25"));
    query.addQueryItem(QStringLiteral("mode"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("bubble"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("rnd"),
                       QString::number(QDateTime::currentSecsSinceEpoch()));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent", "BLKT/0.1 Qt");

    QNetworkReply *reply = m_network.post(request, query.toString(QUrl::FullyEncoded).toUtf8());
    if (!reply) {
        emit logMessage(QStringLiteral("Danmaku request could not be created"));
        return false;
    }
    m_activeReplies.insert(reply);
    reply->setReadBufferSize(MaxReplyBytes);

    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    timeout->setInterval(15000);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, timeout, &QTimer::stop);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > MaxReplyBytes)
            reply->abort();
    });
    const quint64 requestGeneration = m_sessionGeneration;
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestGeneration] {
        consumeReply(reply, requestGeneration);
    });
    timeout->start();

    emit logMessage(QStringLiteral("Danmaku request queued"));
    return true;
}

void Core::consumeReply(QNetworkReply *reply, quint64 generation)
{
    if (!reply)
        return;

    m_activeReplies.remove(reply);
    if (generation != m_sessionGeneration) {
        reply->deleteLater();
        return;
    }

    const QByteArray body = reply->read(MaxReplyBytes + 1);
    const bool tooLarge = body.size() > MaxReplyBytes;
    const bool transportOk = m_loggedIn
                             && reply->error() == QNetworkReply::NoError
                             && !tooLarge;

    if (transportOk) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        const QJsonObject object = document.object();
        const bool apiOk = parseError.error == QJsonParseError::NoError
                           && object.value(QStringLiteral("code")).toInt(-1) == 0;
        emit logMessage(apiOk ? QStringLiteral("Danmaku accepted by server")
                              : QStringLiteral("Danmaku rejected by server"));
    } else if (m_loggedIn) {
        emit logMessage(QStringLiteral("Danmaku request failed"));
    }

    reply->deleteLater();
}

std::optional<GiftInfo> Core::giftInfo(quint64 giftId) const
{
    if (!m_loggedIn || !m_roomConnected)
        return std::nullopt;
    const auto it = m_gifts.constFind(giftId);
    if (it == m_gifts.cend())
        return std::nullopt;
    return it.value();
}

void Core::replaceGiftCatalog(const QList<GiftInfo> &items)
{
    QHash<quint64, GiftInfo> next;
    qsizetype accepted = 0;
    for (const GiftInfo &item : items) {
        if (accepted >= MaxGiftCatalogItems)
            break;
        if (item.id == 0 || item.name.isEmpty() || item.name.size() > 128)
            continue;

        GiftInfo safe = item;
        safe.name = Guard::cleanText(safe.name, 128);
        safe.coinType = Guard::cleanText(safe.coinType, 16);
        if (safe.name.isEmpty())
            continue;

        next.insert(safe.id, safe);
        ++accepted;
    }
    m_gifts.swap(next);
}

void Core::ingestMessage(const MessageEvent &event)
{
    if (!m_loggedIn || !m_roomConnected || event.roomId != m_activeRoomId)
        return;
    MessageEvent safe = event;
    safe.roomId = Guard::cleanText(safe.roomId, 20);
    safe.user = Guard::cleanText(safe.user, 64);
    safe.text = Guard::cleanText(safe.text, 500);
    if (safe.user.isEmpty() || safe.text.isEmpty())
        return;
    emit messageReceived(safe);
}

void Core::ingestGift(const GiftEvent &event)
{
    if (!m_loggedIn || !m_roomConnected || event.roomId != m_activeRoomId
        || event.giftId == 0 || event.count == 0)
        return;
    GiftEvent safe = event;
    safe.roomId = Guard::cleanText(safe.roomId, 20);
    safe.user = Guard::cleanText(safe.user, 64);
    safe.giftName = Guard::cleanText(safe.giftName, 128);
    safe.coinType = Guard::cleanText(safe.coinType, 16);
    if (safe.user.isEmpty() || safe.giftName.isEmpty())
        return;
    emit giftReceived(safe);
}

} // namespace blkt
