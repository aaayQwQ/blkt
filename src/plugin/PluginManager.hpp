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

#include <memory>

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

    // Explicitly trust a plugin binary by file name and its expected SHA-256 digest.
    // The plugin directory must not be writable by untrusted users.
    bool trustPlugin(const QString &fileName, const QByteArray &sha256);
    void revokePluginTrust(const QString &fileName);

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
    bool hasTrustedDigest(const QFileInfo &file) const;
    QString platformSuffix() const;

    Core *m_core = nullptr;
    QDir m_directory;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QHash<QString, QSharedPointer<Entry>> m_plugins;
    QHash<QString, Permissions> m_grants;
    QHash<QString, QByteArray> m_trustedHashes;
    QSharedPointer<int> m_lifetime = QSharedPointer<int>::create(1);
    bool m_running = false;
};

} // namespace blkt
