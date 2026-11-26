#include "../auth/CredentialStore.hpp"

#include <QDateTime>
#include <QTemporaryFile>

int main()
{
    const qint64 future = QDateTime::currentSecsSinceEpoch() + 86400;
    const qint64 expired = QDateTime::currentSecsSinceEpoch() - 86400;
    QByteArray data = QByteArrayLiteral("# Netscape HTTP Cookie File\n");
    auto add = [&data](const QByteArray &domain, const QByteArray &subdomains,
                       const QByteArray &path, const QByteArray &secure,
                       qint64 expiry, const QByteArray &name, const QByteArray &value) {
        data += domain + '\t' + subdomains + '\t' + path + '\t' + secure + '\t'
                + QByteArray::number(expiry) + '\t' + name + '\t' + value + '\n';
    };

    add(".bilibili.com", "TRUE", "/", "TRUE", future, "SESSDATA", "session");
    add(".bilibili.com", "TRUE", "/", "TRUE", future, "bili_jct", "csrf");
    add(".bilibili.com", "TRUE", "/", "FALSE", future, "buvid3", "device");
    add(".bilibili.com", "TRUE", "/", "TRUE", future, "unrelated", "must-not-leak");
    add(".bilibili.com", "TRUE", "/private", "TRUE", future, "SESSDATA", "wrong-path");
    add(".bilibili.com", "TRUE", "/", "MAYBE", future, "bili_jct", "invalid-secure-flag");
    add(".bilibili.com", "TRUE", "/", "TRUE", expired, "buvid3", "expired");
    add("api.bilibili.com", "FALSE", "/", "TRUE", future, "SESSDATA", "host-only");

    QTemporaryFile file;
    if (!file.open() || file.write(data) != data.size())
        return 1;
    file.flush();

    blkt::CredentialStore store;
    if (!store.loadCookieFile(file.fileName()))
        return 2;
    if (store.cookieHeader() != QByteArrayLiteral(
            "SESSDATA=session; bili_jct=csrf; buvid3=device")
        || store.csrfToken() != QStringLiteral("csrf"))
        return 3;

    const QByteArray raw = QByteArrayLiteral(
        "SESSDATA=raw-session; bili_jct=raw-csrf; buvid3=raw-device; unrelated=secret");
    QTemporaryFile rawFile;
    if (!rawFile.open() || rawFile.write(raw) != raw.size())
        return 4;
    rawFile.flush();
    if (!store.loadCookieFile(rawFile.fileName())
        || store.cookieHeader() != QByteArrayLiteral(
            "SESSDATA=raw-session; bili_jct=raw-csrf; buvid3=raw-device"))
        return 5;

    return 0;
}
