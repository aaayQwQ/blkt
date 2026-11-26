#pragma once

#include <QString>
#include <QtGlobal>

namespace blkt {

enum class Permission : quint32 {
    None           = 0,
    ReceiveMessage = 1u << 0,
    SendMessage    = 1u << 1,
    ReceiveGift    = 1u << 2,
    GiftInfo       = 1u << 3
};

using Permissions = quint32;

constexpr Permissions permissionFlag(Permission permission)
{
    return static_cast<Permissions>(permission);
}

constexpr bool hasPermission(Permissions permissions, Permission permission)
{
    return (permissions & permissionFlag(permission)) != 0;
}

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

} // namespace blkt
