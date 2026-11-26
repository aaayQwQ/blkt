#pragma once

#include "IPlugin.hpp"

#include <QObject>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QStringList>
#include <QFileSystemWatcher>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QTimer>

namespace blkt {

class Core;

class PluginManager final : public QObject
{
    Q_OBJECT

public:
    explicit PluginManager(Core *core, const QString &directory,
                           QObject *parent = nullptr);
    ~PluginManager() override;

    // The directory must already exist. The manager never creates plugin folders/files.
    bool start();
    void stop();

    void setGrantedPermissions(const QString &pluginId, Permissions permissions);

    // Trust is volatile and must be granted again after each application restart.
    bool trustPlugin(const QString &fileName, const QByteArray &sha256);
    void revokePluginTrust(const QString &fileName);

private:
    struct Entry {
        QPointer<QProcess> process;
        QString id;
        QString canonicalPath;
        QByteArray digest;
        QByteArray inputBuffer;
        Permissions permissions{};
        bool ready = false;
        bool stopping = false;
        qint64 requestWindowStartMs = 0;
        int requestCount = 0;
        qint64 packetWindowStartMs = 0;
        int packetCount = 0;
        QTimer *readyTimer = nullptr;
    };

    void scheduleSync();
    void syncDirectory();
    bool launchPlugin(const QFileInfo &file);
    void stopPlugin(const QString &pluginId);
    void stopAll();
    bool isAllowedPluginFile(const QFileInfo &file) const;
    bool hasTrustedDigest(const QFileInfo &file) const;
    bool sendPacket(Entry *entry, const QJsonObject &packet);
    void consumeStdout(const QString &pluginId);
    void handlePacket(const QString &pluginId, const QJsonObject &packet);
    void handleRequest(Entry *entry, const QJsonObject &packet);
    QStringList permissionsFor(Permissions permissions) const;
    QString platformSuffix() const;

    Core *m_core = nullptr;
    QDir m_directory;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QHash<QString, Entry *> m_plugins;
    QHash<QString, Permissions> m_grants;
    QHash<QString, QByteArray> m_trustedHashes;
    bool m_running = false;
    QElapsedTimer m_clock;

    static constexpr qsizetype MaxPacketBytes = PluginProtocol::MaxPacketBytes;
    static constexpr int ReadyTimeoutMs = 5000;
    static constexpr int GracefulStopMs = 1000;
};

} // namespace blkt
