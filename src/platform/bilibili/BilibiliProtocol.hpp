#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QtGlobal>
#include <initializer_list>
#include "../../plugin/IPlugin.hpp"

#include <cmath>
#include <limits>

namespace blkt {
namespace BilibiliProtocol {


struct PacketHeader {
    quint32 packetLength = 0;
    quint16 headerLength = 0;
    quint16 version = 0;
    quint32 operation = 0;
    quint32 sequence = 0;
};

inline bool readPacketHeader(const QByteArray &data, qsizetype offset,
                             quint32 maxPacketBytes, PacketHeader *header)
{
    constexpr qsizetype HeaderBytes = 16;
    if (!header || offset < 0 || offset > data.size()
        || data.size() - offset < HeaderBytes) {
        return false;
    }

    const auto *bytes = reinterpret_cast<const uchar *>(data.constData() + offset);
    const quint32 packetLength =
        (static_cast<quint32>(bytes[0]) << 24)
        | (static_cast<quint32>(bytes[1]) << 16)
        | (static_cast<quint32>(bytes[2]) << 8)
        | static_cast<quint32>(bytes[3]);
    const quint16 headerLength =
        static_cast<quint16>((static_cast<quint16>(bytes[4]) << 8)
                             | static_cast<quint16>(bytes[5]));
    const quint16 version =
        static_cast<quint16>((static_cast<quint16>(bytes[6]) << 8)
                             | static_cast<quint16>(bytes[7]));
    const quint32 operation =
        (static_cast<quint32>(bytes[8]) << 24)
        | (static_cast<quint32>(bytes[9]) << 16)
        | (static_cast<quint32>(bytes[10]) << 8)
        | static_cast<quint32>(bytes[11]);
    const quint32 sequence =
        (static_cast<quint32>(bytes[12]) << 24)
        | (static_cast<quint32>(bytes[13]) << 16)
        | (static_cast<quint32>(bytes[14]) << 8)
        | static_cast<quint32>(bytes[15]);

    const qsizetype remaining = data.size() - offset;
    if (packetLength < static_cast<quint32>(HeaderBytes)
        || packetLength > maxPacketBytes
        || headerLength < static_cast<quint16>(HeaderBytes)
        || headerLength > packetLength
        || static_cast<quint64>(packetLength) > static_cast<quint64>(remaining)) {
        return false;
    }

    PacketHeader parsed;
    parsed.packetLength = packetLength;
    parsed.headerLength = headerLength;
    parsed.version = version;
    parsed.operation = operation;
    parsed.sequence = sequence;
    *header = parsed;
    return true;
}

inline int reconnectDelayMs(int attempt, int jitter)
{
    const int safeAttempt = attempt < 0 ? 0 : (attempt > 5 ? 5 : attempt);
    const int baseDelay = qMin(1000 * (1 << safeAttempt), 30000);
    const int safeJitter = jitter < 0 ? 0 : (jitter > 999 ? 999 : jitter);
    return baseDelay + safeJitter;
}

// JSON numbers are IEEE-754 doubles. Reject values outside the exact integer
// range instead of silently rounding large remote identifiers.
inline bool toUnsigned(const QJsonValue &value, quint64 *result)
{
    if (!result)
        return false;

    if (value.isString()) {
        bool ok = false;
        const quint64 parsed = value.toString().toULongLong(&ok, 10);
        if (!ok)
            return false;
        *result = parsed;
        return true;
    }

    if (!value.isDouble())
        return false;

    const double number = value.toDouble();
    constexpr double MaxSafeInteger = 9007199254740991.0;
    if (!std::isfinite(number) || number < 0.0
        || std::floor(number) != number || number > MaxSafeInteger)
        return false;

    *result = static_cast<quint64>(number);
    return true;
}

inline bool extractDanmaku(const QJsonObject &command,
                           QString *user, QString *text)
{
    if (!user || !text)
        return false;

    const QString commandName = command.value(QStringLiteral("cmd")).toString();
    if (!commandName.startsWith(QStringLiteral("DANMU_MSG")))
        return false;

    QJsonArray info = command.value(QStringLiteral("info")).toArray();
    const QJsonObject data = command.value(QStringLiteral("data")).toObject();
    if (info.isEmpty())
        info = data.value(QStringLiteral("info")).toArray();

    QString parsedUser;
    QString parsedText;

    if (info.size() >= 3) {
        parsedText = info.at(1).toString();
        const QJsonArray userInfo = info.at(2).toArray();
        if (userInfo.size() > 1)
            parsedUser = userInfo.at(1).toString();

        // Extended web payloads carry the display name in info[2][15].
        if (parsedUser.isEmpty() && userInfo.size() > 15) {
            const QJsonObject extension = userInfo.at(15).toObject();
            const QJsonObject userObject =
                extension.value(QStringLiteral("user")).toObject();
            const QJsonObject base =
                userObject.value(QStringLiteral("base")).toObject();
            parsedUser = base.value(QStringLiteral("name")).toString();
        }
    }

    if (parsedUser.isEmpty())
        parsedUser = data.value(QStringLiteral("uname")).toString();
    if (parsedText.isEmpty())
        parsedText = data.value(QStringLiteral("content")).toString();
    if (parsedText.isEmpty())
        parsedText = data.value(QStringLiteral("msg")).toString();

    if (parsedUser.trimmed().isEmpty() || parsedText.trimmed().isEmpty())
        return false;

    *user = parsedUser;
    *text = parsedText;
    return true;
}

inline QString scalarString(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    quint64 number = 0;
    return toUnsigned(value, &number) ? QString::number(number) : QString();
}

inline QString firstScalar(const QJsonObject &object,
                           std::initializer_list<QString> keys)
{
    for (const QString &key : keys) {
        const QString value = scalarString(object.value(key));
        if (!value.isEmpty())
            return value;
    }
    return {};
}

inline QString userName(const QJsonObject &data)
{
    QString name = firstScalar(data, {
        QStringLiteral("uname"), QStringLiteral("username"),
        QStringLiteral("user_name")
    });
    if (!name.isEmpty())
        return name;

    QJsonObject user = data.value(QStringLiteral("user_info")).toObject();
    if (user.isEmpty())
        user = data.value(QStringLiteral("user")).toObject();
    name = firstScalar(user, {
        QStringLiteral("uname"), QStringLiteral("username"),
        QStringLiteral("name")
    });
    if (!name.isEmpty())
        return name;

    const QJsonObject base = user.value(QStringLiteral("base")).toObject();
    return firstScalar(base, {
        QStringLiteral("name"), QStringLiteral("uname")
    });
}

inline QString unsignedScalar(const QJsonValue &value)
{
    quint64 number = 0;
    return toUnsigned(value, &number) ? QString::number(number) : QString();
}

inline QString firstUnsigned(const QJsonObject &object,
                             std::initializer_list<QString> keys)
{
    for (const QString &key : keys) {
        const QString value = unsignedScalar(object.value(key));
        if (!value.isEmpty())
            return value;
    }
    return {};
}

inline QString userId(const QJsonObject &data)
{
    QString id = firstUnsigned(data, {
        QStringLiteral("uid"), QStringLiteral("mid")
    });
    if (!id.isEmpty())
        return id;
    QJsonObject user = data.value(QStringLiteral("user_info")).toObject();
    if (user.isEmpty())
        user = data.value(QStringLiteral("user")).toObject();
    return firstUnsigned(user, {
        QStringLiteral("uid"), QStringLiteral("mid")
    });
}


inline bool extractGift(const QJsonObject &command,
                        const QString &roomId,
                        quint64 timestamp,
                        GiftEvent *event)
{
    if (!event || command.value(QStringLiteral("cmd")).toString()
                      != QStringLiteral("SEND_GIFT")) {
        return false;
    }

    const QJsonObject data = command.value(QStringLiteral("data")).toObject();
    const QString name = userName(data);
    const QString giftName = firstScalar(data, {
        QStringLiteral("giftName"), QStringLiteral("gift_name")
    });
    quint64 giftId = 0;
    quint64 count = 0;
    quint64 totalCoin = 0;
    const bool idOk = toUnsigned(data.value(QStringLiteral("giftId")).isUndefined()
                                     ? data.value(QStringLiteral("gift_id"))
                                     : data.value(QStringLiteral("giftId")), &giftId);
    const bool countOk = toUnsigned(data.value(QStringLiteral("num")), &count);
    const QJsonValue coinValue = data.value(QStringLiteral("total_coin"));
    const bool coinOk = coinValue.isUndefined() || coinValue.isNull()
        || toUnsigned(coinValue, &totalCoin);

    if (name.isEmpty() || giftName.isEmpty() || !idOk || giftId == 0
        || !countOk || count == 0
        || count > static_cast<quint64>(std::numeric_limits<quint32>::max())
        || !coinOk) {
        return false;
    }

    GiftEvent parsed;
    parsed.roomId = roomId;
    parsed.user = name;
    parsed.giftName = giftName;
    parsed.giftId = giftId;
    parsed.count = static_cast<quint32>(count);
    parsed.totalCoin = totalCoin;
    parsed.coinType = firstScalar(data, {QStringLiteral("coin_type")});
    parsed.timestamp = timestamp;
    *event = parsed;
    return true;
}

inline bool extractPlatformEvent(const QJsonObject &command,
                                const QString &roomId,
                                quint64 timestamp,
                                PlatformEvent *event)
{
    if (!event)
        return false;

    const QString cmd = command.value(QStringLiteral("cmd")).toString();
    QJsonObject data = command.value(QStringLiteral("data")).toObject();
    // NOTICE_MSG payloads are sometimes emitted directly on the command root.
    if (data.isEmpty())
        data = command;
    PlatformEvent parsed;
    parsed.roomId = roomId;
    parsed.timestamp = timestamp;

    auto setType = [&parsed](const QString &type) { parsed.type = type; };
    auto put = [&parsed](const QString &key, const QString &value) {
        if (!value.isEmpty())
            parsed.data.insert(key, value);
    };
    const QString name = userName(data);
    const QString uid = userId(data);

    if (cmd == QStringLiteral("INTERACT_WORD")
        || cmd == QStringLiteral("INTERACT_WORD_V2")) {
        setType(QStringLiteral("interaction"));
        put(QStringLiteral("user"), name);
        put(QStringLiteral("uid"), uid);
        put(QStringLiteral("subtype"), firstScalar(data, {
            QStringLiteral("msg_type"), QStringLiteral("type")
        }));
        put(QStringLiteral("text"), firstScalar(data, {
            QStringLiteral("msg"), QStringLiteral("message")
        }));
    } else if (cmd == QStringLiteral("ENTRY_EFFECT")
               || cmd == QStringLiteral("WELCOME")
               || cmd == QStringLiteral("WELCOME_GUARD")) {
        setType(QStringLiteral("entry"));
        put(QStringLiteral("user"), name);
        put(QStringLiteral("uid"), uid);
        put(QStringLiteral("text"), firstScalar(data, {
            QStringLiteral("copy_writing"), QStringLiteral("msg"),
            QStringLiteral("message")
        }));
        put(QStringLiteral("subtype"), cmd);
        put(QStringLiteral("level"), firstUnsigned(data, {
            QStringLiteral("guard_level"), QStringLiteral("privilege_type")
        }));
    } else if (cmd == QStringLiteral("LIKE_INFO_V3_CLICK")
               || cmd == QStringLiteral("LIKE_INFO_V3_UPDATE")) {
        setType(QStringLiteral("like"));
        put(QStringLiteral("count"), firstUnsigned(data, {
            QStringLiteral("click_count"), QStringLiteral("like_count"),
            QStringLiteral("count")
        }));
        put(QStringLiteral("total"), firstUnsigned(data, {
            QStringLiteral("total_like"), QStringLiteral("total")
        }));
        put(QStringLiteral("subtype"), cmd);
    } else if (cmd == QStringLiteral("WATCHED_CHANGE")) {
        setType(QStringLiteral("watched"));
        put(QStringLiteral("count"), firstUnsigned(data, {
            QStringLiteral("num"), QStringLiteral("count")
        }));
        put(QStringLiteral("text"), firstScalar(data, {
            QStringLiteral("text"), QStringLiteral("show_text")
        }));
    } else if (cmd == QStringLiteral("NOTICE_MSG")) {
        setType(QStringLiteral("notice"));
        put(QStringLiteral("text"), firstScalar(data, {
            QStringLiteral("msg_common"), QStringLiteral("msg_self"),
            QStringLiteral("msg")
        }));
        put(QStringLiteral("subtype"), firstScalar(data, {
            QStringLiteral("msg_type"), QStringLiteral("type")
        }));
    } else if (cmd == QStringLiteral("SUPER_CHAT_MESSAGE")
               || cmd == QStringLiteral("SUPER_CHAT_MESSAGE_JPN")) {
        setType(QStringLiteral("superChat"));
        put(QStringLiteral("user"), name);
        put(QStringLiteral("uid"), uid);
        put(QStringLiteral("text"), firstScalar(data, {
            QStringLiteral("message"), QStringLiteral("msg")
        }));
        put(QStringLiteral("price"), firstUnsigned(data, {
            QStringLiteral("price")
        }));
        put(QStringLiteral("messageId"), firstUnsigned(data, {
            QStringLiteral("message_id"), QStringLiteral("id")
        }));
    } else if (cmd == QStringLiteral("GUARD_BUY")) {
        setType(QStringLiteral("guard"));
        put(QStringLiteral("user"), name);
        put(QStringLiteral("uid"), uid);
        put(QStringLiteral("giftName"), firstScalar(data, {
            QStringLiteral("gift_name"), QStringLiteral("giftName")
        }));
        put(QStringLiteral("count"), firstUnsigned(data, {
            QStringLiteral("num"), QStringLiteral("count")
        }));
        put(QStringLiteral("price"), firstUnsigned(data, {
            QStringLiteral("price")
        }));
        put(QStringLiteral("level"), firstUnsigned(data, {
            QStringLiteral("guard_level"), QStringLiteral("level")
        }));
    } else if (cmd == QStringLiteral("COMBO_SEND")) {
        setType(QStringLiteral("comboGift"));
        put(QStringLiteral("user"), name);
        put(QStringLiteral("uid"), uid);
        put(QStringLiteral("giftName"), firstScalar(data, {
            QStringLiteral("gift_name"), QStringLiteral("giftName")
        }));
        put(QStringLiteral("giftId"), firstUnsigned(data, {
            QStringLiteral("gift_id"), QStringLiteral("giftId")
        }));
        put(QStringLiteral("count"), firstUnsigned(data, {
            QStringLiteral("combo_num"), QStringLiteral("total_num"),
            QStringLiteral("num")
        }));
        put(QStringLiteral("total"), firstUnsigned(data, {
            QStringLiteral("combo_total_coin"), QStringLiteral("total_coin")
        }));
        put(QStringLiteral("coinType"), firstScalar(data, {
            QStringLiteral("coin_type")
        }));
    } else if (cmd == QStringLiteral("ONLINE_RANK_COUNT")) {
        setType(QStringLiteral("online"));
        put(QStringLiteral("count"), firstUnsigned(data, {
            QStringLiteral("count"), QStringLiteral("num")
        }));
    } else if (cmd == QStringLiteral("ROOM_REAL_TIME_MESSAGE_UPDATE")) {
        setType(QStringLiteral("roomStats"));
        put(QStringLiteral("online"), firstUnsigned(data, {
            QStringLiteral("online")
        }));
        const QJsonObject watched = data.value(QStringLiteral("watched_show")).toObject();
        put(QStringLiteral("watched"), firstUnsigned(watched, {
            QStringLiteral("num")
        }));
        put(QStringLiteral("roomScore"), firstUnsigned(data, {
            QStringLiteral("room_score")
        }));
    } else {
        return false;
    }

    if (parsed.data.isEmpty())
        return false;
    *event = parsed;
    return true;
}

} // namespace BilibiliProtocol
} // namespace blkt
