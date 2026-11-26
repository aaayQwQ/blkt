#pragma once

#include <QFlags>
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
Q_DECLARE_FLAGS(Permissions, Permission)
Q_DECLARE_OPERATORS_FOR_FLAGS(Permissions)

constexpr Permissions permissionFlag(Permission permission) noexcept
{
    return Permissions(permission);
}

inline bool hasPermission(Permissions permissions, Permission permission) noexcept
{
    return permissions.testFlag(permission);
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
