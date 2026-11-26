#pragma once

#include <QtGlobal>
#include <QFlags>
#include <QString>

namespace blkt {

enum class Permission : quint32 {
    None           = 0,
    ReceiveMessage = 1u << 0,
    SendMessage    = 1u << 1,
    ReceiveGift    = 1u << 2,
    GiftInfo       = 1u << 3
};
Q_DECLARE_FLAGS(Permissions, Permission)
Q_DECLARE_OPERATORS_FOR_FLAGS(Permissions)

struct MessageEvent {
    QString roomId;
    QString user;
    QString text;
    quint64 timestamp = 0;
};

struct GiftEvent {
    QString roomId;
    QString user;
    QString giftName;
    quint64 giftId = 0;
    quint32 count = 0;
    quint64 totalCoin = 0;
    QString coinType;
    quint64 timestamp = 0;
};

struct GiftInfo {
    quint64 id = 0;
    QString name;
    quint64 price = 0;
    QString coinType;
};

namespace PluginProtocol {
inline constexpr int Version = 1;
inline constexpr qsizetype MaxPacketBytes = 64 * 1024;
inline constexpr int MaxRequestsPerSecond = 30;
inline constexpr int MaxPacketsPerSecond = 100;
}

} // namespace blkt
