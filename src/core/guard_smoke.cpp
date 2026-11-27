#include "../core/Guard.hpp"

#include <QByteArray>
#include <QString>

int main()
{
    using blkt::Guard;

    if (!Guard::validRoomId(QStringLiteral("123456")))
        return 1;
    if (!Guard::validRoomId(QStringLiteral("9007199254740991"))
        || Guard::validRoomId(QStringLiteral("9007199254740992")))
        return 2;

    if (!Guard::validBilibiliWebSocketHost(QStringLiteral("broadcastlv.chat.bilibili.com"))
        || !Guard::validBilibiliWebSocketHost(QStringLiteral("tx-gz-live-comet-01.chat.bilibili.com"))
        || Guard::validBilibiliWebSocketHost(QStringLiteral("evil.example.com"))
        || Guard::validBilibiliWebSocketHost(QStringLiteral("bilibili.com.evil.example")))
        return 3;

    if (Guard::validRoomId(QStringLiteral("0"))
        || Guard::validRoomId(QStringLiteral("room"))
        || Guard::validRoomId(QStringLiteral("123456789012345678901"))
        || Guard::validRoomId(QStringLiteral("+1"))
        || Guard::validRoomId(QStringLiteral("1 ")))
        return 4;

    if (!Guard::validCsrf(QStringLiteral("0123456789abcdefABCDEF_-")))
        return 5;
    if (Guard::validCsrf(QStringLiteral("invalid;csrf"))
        || Guard::validCsrf(QString()))
        return 6;

    if (!Guard::validCookie(QByteArrayLiteral("SESSDATA=abc; bili_jct=123")))
        return 7;
    if (Guard::validCookie(QByteArray("SESSDATA=abc\n", 13)))
        return 8;

    const QString cleaned =
        Guard::cleanText(QString::fromUtf8("a\0b\x7f", 4), 16);
    if (cleaned != QStringLiteral("ab"))
        return 9;

    const QString safeLog = Guard::cleanLogText(
        QStringLiteral("first\nsecond\r\x1b[31mred\x1b[0m"), 64);
    if (safeLog.contains(QLatin1Char('\n')) || safeLog.contains(QLatin1Char('\r'))
        || safeLog.contains(QChar(0x001b)))
        return 10;

    const QString redacted =
        Guard::redact(QStringLiteral("Cookie: secret-value csrf_token=another-secret"));
    if (redacted.contains(QStringLiteral("secret-value"))
        || redacted.contains(QStringLiteral("another-secret"))
        || !redacted.contains(QStringLiteral("[REDACTED]")))
        return 11;

    const QString cookieRedacted = Guard::redact(
        QStringLiteral("SESSDATA=session-value; bili_jct=csrf-value; buvid3=device-value"));
    if (cookieRedacted.contains(QStringLiteral("session-value"))
        || cookieRedacted.contains(QStringLiteral("csrf-value"))
        || cookieRedacted.contains(QStringLiteral("device-value")))
        return 12;

    return 0;
}
