#pragma once

#include "IPlugin.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QJSEngine>
#include <QJSValue>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <QElapsedTimer>

namespace blkt {
class Core;

class ScriptPluginApi final : public QObject
{
    Q_OBJECT
public:
    ScriptPluginApi(Core *core, QJSEngine *engine, QString pluginId,
                    Permissions permissions, QObject *parent = nullptr);

    Q_INVOKABLE bool on(const QString &eventName, const QJSValue &callback);
    Q_INVOKABLE bool sendMessage(const QString &text);
    Q_INVOKABLE QVariantMap giftInfo(const QString &giftId) const;
    Q_INVOKABLE void log(const QString &message);

    bool dispatch(const QString &eventName, const QVariantMap &event,
                  QElapsedTimer &eventTimer, int &callbacksRemaining);

signals:
    void logMessage(const QString &message);

private:
    Core *m_core = nullptr;
    QJSEngine *m_engine = nullptr;
    QString m_pluginId;
    Permissions m_permissions{};
    QHash<QString, QList<QJSValue>> m_handlers;
    int m_callbackFailures = 0;
    bool m_disabled = false;
};

class ScriptPluginManager final : public QObject
{
    Q_OBJECT
public:
    ScriptPluginManager(Core *core, const QString &directory,
                        const QStringList &allowedPluginIds,
                        const QStringList &sendMessagePluginIds,
                        QObject *parent = nullptr);
    ~ScriptPluginManager() override;

    bool start();
    void stop();

private:
    struct Entry {
        QString id;
        QJSEngine *engine = nullptr;
        ScriptPluginApi *api = nullptr;
    };

    bool loadPlugin(const QFileInfo &scriptInfo);
    void unloadPlugin(const QString &id);
    void dispatchMessage(const MessageEvent &event);
    void dispatchGift(const GiftEvent &event);
    void dispatchPlatformEvent(const PlatformEvent &event);

    Core *m_core = nullptr;
    QDir m_directory;
    QSet<QString> m_allowedIds;
    QSet<QString> m_sendMessageIds;
    QMap<QString, Entry *> m_plugins;
    bool m_started = false;
};
}
