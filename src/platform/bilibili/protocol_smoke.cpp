#include "BilibiliProtocol.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

int main()
{
    using blkt::BilibiliProtocol::extractDanmaku;
    using blkt::BilibiliProtocol::toUnsigned;

    quint64 number = 0;
    if (!toUnsigned(QJsonValue(QStringLiteral("18446744073709551615")), &number)
        || number != std::numeric_limits<quint64>::max())
        return 1;
    if (!toUnsigned(QJsonValue(42.0), &number) || number != 42)
        return 2;
    if (toUnsigned(QJsonValue(9007199254740992.0), &number)
        || toUnsigned(QJsonValue(-1.0), &number)
        || toUnsigned(QJsonValue(1.5), &number)
        || toUnsigned(QJsonValue(QStringLiteral("not-a-number")), &number))
        return 3;

    QString user;
    QString text;
    QJsonArray userInfo;
    userInfo.append(123);
    userInfo.append(QStringLiteral("LegacyUser"));
    QJsonArray info;
    info.append(QJsonArray{});
    info.append(QStringLiteral("hello from legacy payload"));
    info.append(userInfo);
    QJsonObject legacy;
    legacy.insert(QStringLiteral("cmd"), QStringLiteral("DANMU_MSG:4:0:2:2:2:0"));
    legacy.insert(QStringLiteral("info"), info);
    if (!extractDanmaku(legacy, &user, &text)
        || user != QStringLiteral("LegacyUser")
        || text != QStringLiteral("hello from legacy payload"))
        return 4;

    QJsonArray extendedUser;
    for (int i = 0; i < 16; ++i)
        extendedUser.append(QJsonValue());
    QJsonObject base;
    base.insert(QStringLiteral("name"), QStringLiteral("ExtendedUser"));
    QJsonObject userObject;
    userObject.insert(QStringLiteral("base"), base);
    QJsonObject extension;
    extension.insert(QStringLiteral("user"), userObject);
    extendedUser[15] = extension;
    QJsonArray extendedInfo;
    extendedInfo.append(QJsonArray{});
    extendedInfo.append(QStringLiteral("hello from extended payload"));
    extendedInfo.append(extendedUser);
    QJsonObject extended;
    extended.insert(QStringLiteral("cmd"), QStringLiteral("DANMU_MSG"));
    extended.insert(QStringLiteral("info"), extendedInfo);
    if (!extractDanmaku(extended, &user, &text)
        || user != QStringLiteral("ExtendedUser")
        || text != QStringLiteral("hello from extended payload"))
        return 5;

    QJsonObject fallbackData;
    fallbackData.insert(QStringLiteral("uname"), QStringLiteral("FallbackUser"));
    fallbackData.insert(QStringLiteral("content"), QStringLiteral("fallback text"));
    QJsonObject fallback;
    fallback.insert(QStringLiteral("cmd"), QStringLiteral("DANMU_MSG:4"));
    fallback.insert(QStringLiteral("data"), fallbackData);
    if (!extractDanmaku(fallback, &user, &text)
        || user != QStringLiteral("FallbackUser")
        || text != QStringLiteral("fallback text"))
        return 6;

    QJsonObject incomplete;
    incomplete.insert(QStringLiteral("cmd"), QStringLiteral("DANMU_MSG"));
    if (extractDanmaku(incomplete, &user, &text))
        return 7;
    incomplete.insert(QStringLiteral("cmd"), QStringLiteral("SEND_GIFT"));
    if (extractDanmaku(incomplete, &user, &text))
        return 8;

    return 0;
}
