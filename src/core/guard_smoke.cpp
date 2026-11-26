#include "../core/Guard.hpp"

#include <QByteArray>
#include <QString>

int main()
{
    using blkt::Guard;

    if (!Guard::validRoomId(QStringLiteral("123456")))
        return 1;
    if (Guard::validRoomId(QStringLiteral("0"))
        || Guard::validRoomId(QStringLiteral("room"))
        || Guard::validRoomId(QStringLiteral("123456789012345678901")))
        return 2;

    if (!Guard::validCsrf(QStringLiteral("0123456789abcdefABCDEF_-")))
        return 3;
    if (Guard::validCsrf(QStringLiteral("invalid;csrf"))
        || Guard::validCsrf(QString()))
        return 4;

    if (!Guard::validCookie(QByteArrayLiteral("SESSDATA=abc; bili_jct=123")))
        return 5;
    if (Guard::validCookie(QByteArray("SESSDATA=abc\n", 13)))
        return 6;

    const QString cleaned =
        Guard::cleanText(QString::fromUtf8("a\0b\x7f", 4), 16);
    if (cleaned != QStringLiteral("ab"))
        return 7;

    const QString redacted =
        Guard::redact(QStringLiteral("Cookie: secret-value csrf_token=another-secret"));
    if (redacted.contains(QStringLiteral("secret-value"))
        || redacted.contains(QStringLiteral("another-secret"))
        || !redacted.contains(QStringLiteral("[REDACTED]")))
        return 8;

    return 0;
}
