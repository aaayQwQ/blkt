#include "CredentialStore.hpp"

#include <QFile>
#include <QHash>
#include <QFileInfo>

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

bool isBilibiliDomain(const QString &domain)
{
    const QString normalized = domain.trimmed().toLower();
    return normalized == QStringLiteral("bilibili.com")
           || normalized.endsWith(QStringLiteral(".bilibili.com"));
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

    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        bool httpOnly = false;
        if (line.startsWith("#HttpOnly_")) {
            line.remove(0, 10);
            httpOnly = true;
        } else if (line.startsWith('#')) {
            continue;
        }

        Q_UNUSED(httpOnly);
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() != 7)
            continue;

        const QString domain = QString::fromLatin1(fields.at(0));
        const QByteArray name = fields.at(5).trimmed();
        const QByteArray value = fields.at(6).trimmed();
        if (!isBilibiliDomain(domain) || !safeCookiePart(name)
            || !safeCookiePart(value))
            continue;

        cookies.insert(name, value);
    }

    if (!cookies.contains("SESSDATA") || !cookies.contains("bili_jct"))
        return false;

    QByteArray header;
    for (auto it = cookies.cbegin(); it != cookies.cend(); ++it) {
        if (!header.isEmpty())
            header.append("; ");
        header.append(it.key());
        header.append('=');
        header.append(it.value());
    }

    const QByteArray csrf = cookies.value("bili_jct");
    if (!safeCookiePart(csrf) || header.size() > 8192)
        return false;

    m_cookie = header;
    m_csrf = QString::fromLatin1(csrf);
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
        if (safeCookiePart(name) && safeCookiePart(value))
            cookies.insert(name, value);
    }

    if (!cookies.contains("SESSDATA") || !cookies.contains("bili_jct"))
        return false;

    QByteArray header;
    for (auto it = cookies.cbegin(); it != cookies.cend(); ++it) {
        if (!header.isEmpty())
            header.append("; ");
        header.append(it.key());
        header.append('=');
        header.append(it.value());
    }

    const QByteArray csrf = cookies.value("bili_jct");
    if (!safeCookiePart(csrf) || header.size() > 8192)
        return false;

    m_cookie = header;
    m_csrf = QString::fromLatin1(csrf);
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
