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

    blkt::PlatformEvent event;
    QJsonObject interactionData;
    interactionData.insert(QStringLiteral("uid"), QStringLiteral("12345"));
    interactionData.insert(QStringLiteral("uname"), QStringLiteral("RoomUser"));
    interactionData.insert(QStringLiteral("msg_type"), 2);
    QJsonObject interaction;
    interaction.insert(QStringLiteral("cmd"), QStringLiteral("INTERACT_WORD_V2"));
    interaction.insert(QStringLiteral("data"), interactionData);
    if (!blkt::BilibiliProtocol::extractPlatformEvent(
            interaction, QStringLiteral("123"), 100, &event)
        || event.type != QStringLiteral("interaction")
        || event.data.value(QStringLiteral("user")).toString() != QStringLiteral("RoomUser")
        || event.data.value(QStringLiteral("uid")).toString() != QStringLiteral("12345"))
        return 9;
    interactionData.insert(QStringLiteral("uid"), QStringLiteral("not-a-number"));
    interaction.insert(QStringLiteral("data"), interactionData);
    if (blkt::BilibiliProtocol::extractPlatformEvent(
            interaction, QStringLiteral("123"), 100, &event)
        && event.data.contains(QStringLiteral("uid")))
        return 18;

    QJsonObject likeData;
    likeData.insert(QStringLiteral("click_count"), 7);
    likeData.insert(QStringLiteral("total_like"), 99);
    QJsonObject like;
    like.insert(QStringLiteral("cmd"), QStringLiteral("LIKE_INFO_V3_CLICK"));
    like.insert(QStringLiteral("data"), likeData);
    if (!blkt::BilibiliProtocol::extractPlatformEvent(
            like, QStringLiteral("123"), 101, &event)
        || event.type != QStringLiteral("like")
        || event.data.value(QStringLiteral("count")).toString() != QStringLiteral("7")
        || event.data.value(QStringLiteral("total")).toString() != QStringLiteral("99"))
        return 10;

    QJsonObject watchedData;
    watchedData.insert(QStringLiteral("num"), 321);
    watchedData.insert(QStringLiteral("text"), QStringLiteral("321 viewers"));
    QJsonObject watched;
    watched.insert(QStringLiteral("cmd"), QStringLiteral("WATCHED_CHANGE"));
    watched.insert(QStringLiteral("data"), watchedData);
    if (!blkt::BilibiliProtocol::extractPlatformEvent(
            watched, QStringLiteral("123"), 102, &event)
        || event.type != QStringLiteral("watched")
        || event.data.value(QStringLiteral("count")).toString() != QStringLiteral("321"))
        return 11;

    QJsonObject noticeData;
    noticeData.insert(QStringLiteral("msg_common"), QStringLiteral("Room notice"));
    noticeData.insert(QStringLiteral("msg_type"), 3);
    QJsonObject notice;
    notice.insert(QStringLiteral("cmd"), QStringLiteral("NOTICE_MSG"));
    notice.insert(QStringLiteral("data"), noticeData);
    if (!blkt::BilibiliProtocol::extractPlatformEvent(
            notice, QStringLiteral("123"), 103, &event)
        || event.type != QStringLiteral("notice")
        || event.data.value(QStringLiteral("text")).toString() != QStringLiteral("Room notice"))
        return 12;

    QJsonObject scUser;
    scUser.insert(QStringLiteral("uname"), QStringLiteral("Supporter"));
    scUser.insert(QStringLiteral("uid"), QStringLiteral("987654"));
    QJsonObject scData;
    scData.insert(QStringLiteral("user_info"), scUser);
    scData.insert(QStringLiteral("message"), QStringLiteral("Thanks for the stream"));
    scData.insert(QStringLiteral("price"), 30);
    scData.insert(QStringLiteral("message_id"), QStringLiteral("112233"));
    QJsonObject superChat;
    superChat.insert(QStringLiteral("cmd"), QStringLiteral("SUPER_CHAT_MESSAGE"));
    superChat.insert(QStringLiteral("data"), scData);
    if (!blkt::BilibiliProtocol::extractPlatformEvent(
            superChat, QStringLiteral("123"), 104, &event)
        || event.type != QStringLiteral("superChat")
        || event.data.value(QStringLiteral("user")).toString() != QStringLiteral("Supporter")
        || event.data.value(QStringLiteral("price")).toString() != QStringLiteral("30"))
        return 13;

    QJsonObject guardData;
    guardData.insert(QStringLiteral("username"), QStringLiteral("Captain"));
    guardData.insert(QStringLiteral("uid"), QStringLiteral("222"));
    guardData.insert(QStringLiteral("gift_name"), QStringLiteral("Captain"));
    guardData.insert(QStringLiteral("num"), 1);
    guardData.insert(QStringLiteral("price"), 198000);
    guardData.insert(QStringLiteral("guard_level"), 3);
    QJsonObject guardCommand;
    guardCommand.insert(QStringLiteral("cmd"), QStringLiteral("GUARD_BUY"));
    guardCommand.insert(QStringLiteral("data"), guardData);
    if (!blkt::BilibiliProtocol::extractPlatformEvent(
            guardCommand, QStringLiteral("123"), 105, &event)
        || event.type != QStringLiteral("guard")
        || event.data.value(QStringLiteral("level")).toString() != QStringLiteral("3"))
        return 14;

    QJsonObject comboData;
    comboData.insert(QStringLiteral("uname"), QStringLiteral("Gifter"));
    comboData.insert(QStringLiteral("gift_name"), QStringLiteral("Snack"));
    comboData.insert(QStringLiteral("gift_id"), QStringLiteral("1"));
    comboData.insert(QStringLiteral("combo_num"), 10);
    comboData.insert(QStringLiteral("combo_total_coin"), 1000);
    QJsonObject combo;
    combo.insert(QStringLiteral("cmd"), QStringLiteral("COMBO_SEND"));
    combo.insert(QStringLiteral("data"), comboData);
    if (!blkt::BilibiliProtocol::extractPlatformEvent(
            combo, QStringLiteral("123"), 106, &event)
        || event.type != QStringLiteral("comboGift")
        || event.data.value(QStringLiteral("count")).toString() != QStringLiteral("10"))
        return 15;

    QJsonObject statsData;
    statsData.insert(QStringLiteral("online"), 55);
    statsData.insert(QStringLiteral("room_score"), 1234);
    QJsonObject watchedShow;
    watchedShow.insert(QStringLiteral("num"), QStringLiteral("888"));
    statsData.insert(QStringLiteral("watched_show"), watchedShow);
    QJsonObject stats;
    stats.insert(QStringLiteral("cmd"), QStringLiteral("ROOM_REAL_TIME_MESSAGE_UPDATE"));
    stats.insert(QStringLiteral("data"), statsData);
    if (!blkt::BilibiliProtocol::extractPlatformEvent(
            stats, QStringLiteral("123"), 107, &event)
        || event.type != QStringLiteral("roomStats")
        || event.data.value(QStringLiteral("watched")).toString() != QStringLiteral("888"))
        return 16;

    QJsonObject unknown;
    unknown.insert(QStringLiteral("cmd"), QStringLiteral("UNRECOGNIZED_PRIVATE_COMMAND"));
    if (blkt::BilibiliProtocol::extractPlatformEvent(
            unknown, QStringLiteral("123"), 108, &event))
        return 17;

    return 0;
}
