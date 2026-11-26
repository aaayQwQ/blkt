#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QtPlugin>

#include <functional>
#include <optional>

namespace blkt {

enum class Permission : quint32 {
    None          = 0,
    ReceiveMessage= 1u << 0,
    SendMessage   = 1u << 1,
    ReceiveGift   = 1u << 2,
    GiftInfo      = 1u << 3
};
Q_DECLARE_FLAGS(Permissions, Permission)
Q_DECLARE_OPERATORS_FOR_FLAGS(Permissions)

struct MessageEvent {
    QString user;
    QString text;
    quint64 timestamp = 0;
};

struct GiftEvent {
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

struct PluginContext {
    QString pluginId;
    std::function<bool(const QString &, const QString &)> sendMessage;
    std::function<std::optional<GiftInfo>(quint64)> giftInfo;
};

class IPlugin
{
public:
    virtual ~IPlugin() = default;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual void onLoad(const PluginContext &context) = 0;
    virtual void onUnload() = 0;
    virtual void onMessage(const MessageEvent &) {}
    virtual void onGift(const GiftEvent &) {}
};

} // namespace blkt

#define BLKT_PLUGIN_IID "org.blkt.Plugin/1.0"
Q_DECLARE_INTERFACE(blkt::IPlugin, BLKT_PLUGIN_IID)
