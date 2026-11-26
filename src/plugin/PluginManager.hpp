#pragma once

#include "IPlugin.hpp"

#include <QObject>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QHash>
#include <QPluginLoader>
#include <QSharedPointer>

namespace blkt {

class Core;

class PluginManager final : public QObject
{
    Q_OBJECT

public:
    explicit PluginManager(Core *core, const QString &directory,
                           QObject *parent = nullptr);
    ~PluginManager() override;

    bool start();
    void stop();
    void setGrantedPermissions(const QString &pluginId, Permissions permissions);

private:
    struct Entry {
        std::unique_ptr<QPluginLoader> loader;
        IPlugin *plugin = nullptr;
        PluginContext context;
        Permissions permissions;
        QString canonicalPath;
        qint64 modifiedMs = 0;
        qint64 fileSize = 0;
        bool active = false;
    };

    void scheduleSync();
    void syncDirectory();
    bool loadPlugin(const QFileInfo &file);
    void unloadPlugin(const QString &pluginId);
    void unloadAll();
    bool isTrustedPluginPath(const QFileInfo &file) const;
    QString platformSuffix() const;

    Core *m_core = nullptr;
    QDir m_directory;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QHash<QString, QSharedPointer<Entry>> m_plugins;
    QHash<QString, Permissions> m_grants;
    QSharedPointer<int> m_lifetime = QSharedPointer<int>::create(1);
    bool m_running = false;
};

} // namespace blkt
