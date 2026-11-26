#include "CredentialStore.hpp"

#include <QFile>
#include <QHash>
#include <QFileInfo>
#include <QDateTime>
#include <QList>

namespace blkt {
namespace {

constexpr qint64 MaxCookieFileBytes = 64 * 1024;

bool safeCookiePart(const QByteArray &value)
{
    if (value.isEmpty())
        return false;
    for (const char ch : value) {
        const uchar c = static_cast<uchar>(ch);
        if (c <= 0x20 || c >= 0x7f || c == ';' || c == ',')
            return false;
    }
    return true;
}

bool isSharedBilibiliDomain(const QByteArray &field)
{
    const QByteArray domain = field.trimmed().toLower();
    return domain == QByteArrayLiteral("bilibili.com")
           || domain == QByteArrayLiteral(".bilibili.com");
}

bool allowedSessionCookie(const QByteArray &name)
{
    return name == QByteArrayLiteral("SESSDATA")
           || name == QByteArrayLiteral("bili_jct")
           || name == QByteArrayLiteral("buvid3");
}

QByteArray buildSessionHeader(const QHash<QByteArray, QByteArray> &cookies)
{
    if (!cookies.contains(QByteArrayLiteral("SESSDATA"))
        || !cookies.contains(QByteArrayLiteral("bili_jct")))
        return {};

    QByteArray header;
    const QList<QByteArray> names{
        QByteArrayLiteral("SESSDATA"),
        QByteArrayLiteral("bili_jct"),
        QByteArrayLiteral("buvid3")
    };
    for (const QByteArray &name : names) {
        const auto it = cookies.constFind(name);
        if (it == cookies.cend())
            continue;
        if (!header.isEmpty())
            header.append("; ");
        header.append(name);
        header.append('=');
        header.append(it.value());
    }
    return header.size() <= 8192 ? header : QByteArray();
}

} // namespace

CredentialStore::~CredentialStore()
{
    clear();
}

bool CredentialStore::loadCookieFile(const QString &filePath, QString *error)
{
    clear();

    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        if (error)
            *error = QStringLiteral("cookie.txt was not found beside the application");
        return false;
    }
    if (!fileInfo.isFile() || fileInfo.isSymLink()) {
        if (error)
            *error = QStringLiteral("cookie.txt must be a regular file, not a symbolic link");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cookie.txt could not be opened for reading");
        return false;
    }
    if (file.size() <= 0 || file.size() > MaxCookieFileBytes) {
        if (error)
            *error = QStringLiteral("cookie.txt is empty or exceeds the 64 KiB limit");
        return false;
    }

    QByteArray data = file.read(MaxCookieFileBytes + 1);
    const bool readOk = file.error() == QFileDevice::NoError;
    file.close();
    if (!readOk) {
        data.fill('\0');
        if (error)
            *error = QStringLiteral("cookie.txt could not be read completely");
        return false;
    }
    if (data.size() > MaxCookieFileBytes) {
        data.fill('\0');
        if (error)
            *error = QStringLiteral("cookie.txt exceeds the 64 KiB limit");
        return false;
    }

    bool ok = false;
    // Browser extensions commonly export Netscape cookies.txt. A raw Cookie
    // header is also accepted for convenient portable deployments.
    if (data.contains('\t') || data.startsWith("# Netscape"))
        ok = parseNetscapeCookieFile(data);
    else
        ok = parseCookieHeader(data);

    data.fill('\0');
    if (!ok) {
        clear();
        if (error)
            *error = QStringLiteral("cookie.txt contains no valid Bilibili session (SESSDATA and bili_jct are required)");
        return false;
    }

    return true;
}

bool CredentialStore::parseNetscapeCookieFile(const QByteArray &data)
{
    QHash<QByteArray, QByteArray> cookies;
    const QList<QByteArray> lines = data.split('\n');
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith("#HttpOnly_"))
            line.remove(0, 10);
        else if (line.startsWith('#'))
            continue;

        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() != 7)
            continue;

        const QByteArray domain = fields.at(0).trimmed();
        const QByteArray includeSubdomains = fields.at(1).trimmed().toUpper();
        const QByteArray path = fields.at(2).trimmed();
        const QByteArray secure = fields.at(3).trimmed().toUpper();
        bool expiryOk = false;
        const qint64 expiry = fields.at(4).trimmed().toLongLong(&expiryOk);
        const QByteArray name = fields.at(5).trimmed();
        const QByteArray value = fields.at(6).trimmed();

        // Only shared, root-path cookies are reused across the fixed Bilibili
        // API hosts. Both Secure values are valid for HTTPS requests; every
        // cookie-bearing endpoint in this client uses HTTPS/WSS. Host-only
        // and path-scoped cookies are deliberately excluded.
        if (!isSharedBilibiliDomain(domain)
            || includeSubdomains != QByteArrayLiteral("TRUE")
            || path != QByteArrayLiteral("/")
            || (secure != QByteArrayLiteral("TRUE")
                && secure != QByteArrayLiteral("FALSE"))
            || !expiryOk || (expiry != 0 && expiry <= now)
            || !allowedSessionCookie(name)
            || !safeCookiePart(value))
            continue;

        cookies.insert(name, value);
    }

    const QByteArray header = buildSessionHeader(cookies);
    if (header.isEmpty())
        return false;

    m_cookie = header;
    m_csrf = QString::fromLatin1(cookies.value(QByteArrayLiteral("bili_jct")));
    return true;
}

bool CredentialStore::parseCookieHeader(const QByteArray &data)
{
    QByteArray line = data.trimmed();
    if (line.left(7).compare(QByteArrayLiteral("Cookie:"), Qt::CaseInsensitive) == 0)
        line = line.mid(7).trimmed();
    if (line.isEmpty() || line.size() > 8192)
        return false;

    QHash<QByteArray, QByteArray> cookies;
    const QList<QByteArray> parts = line.split(';');
    for (QByteArray part : parts) {
        part = part.trimmed();
        const qsizetype separator = part.indexOf('=');
        if (separator <= 0)
            continue;

        const QByteArray name = part.left(separator).trimmed();
        const QByteArray value = part.mid(separator + 1).trimmed();
        if (allowedSessionCookie(name) && safeCookiePart(value))
            cookies.insert(name, value);
    }

    const QByteArray header = buildSessionHeader(cookies);
    if (header.isEmpty())
        return false;

    m_cookie = header;
    m_csrf = QString::fromLatin1(cookies.value(QByteArrayLiteral("bili_jct")));
    return true;
}

bool CredentialStore::isReady() const
{
    return !m_cookie.isEmpty() && !m_csrf.isEmpty();
}

QByteArray CredentialStore::cookieHeader() const
{
    return m_cookie;
}

QString CredentialStore::csrfToken() const
{
    return m_csrf;
}

void CredentialStore::clear()
{
    if (!m_cookie.isEmpty())
        m_cookie.fill('\0');
    m_cookie.clear();
    m_csrf.fill(QChar(u'\0'));
    m_csrf.clear();
}

} // namespace blkt
