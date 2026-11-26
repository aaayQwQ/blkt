#include "PluginManager.hpp"
#include "../core/Core.hpp"
#include "../core/Guard.hpp"

#include <QFileInfo>
#include <QLibrary>
#include <QRegularExpression>
#include <QSet>
#include <QMetaObject>

namespace blkt {

PluginManager::PluginManager(Core *core, const QString &directory, QObject *parent)
    : QObject(parent), m_core(core), m_directory(directory)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(350);

    connect(&m_debounce, &QTimer::timeout, this, &PluginManager::syncDirectory);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &PluginManager::scheduleSync);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &PluginManager::scheduleSync);

    if (m_core) {
        connect(m_core, &Core::messageReceived, this, [this](const MessageEvent &event) {
            const auto entries = m_plugins;
            for (const auto &entry : entries) {
                if (!entry || !entry->active || !entry->plugin
                    || !entry->permissions.testFlag(Permission::ReceiveMessage))
                    continue;
                try {
                    entry->plugin->onMessage(event);
                } catch (...) {
                    const QString id = entry->context.pluginId;
                    QMetaObject::invokeMethod(this, [this, id] { unloadPlugin(id); },
                                              Qt::QueuedConnection);
                }
            }
        });
        connect(m_core, &Core::giftReceived, this, [this](const GiftEvent &event) {
            const auto entries = m_plugins;
            for (const auto &entry : entries) {
                if (!entry || !entry->active || !entry->plugin
                    || !entry->permissions.testFlag(Permission::ReceiveGift))
                    continue;
                try {
                    entry->plugin->onGift(event);
                } catch (...) {
                    const QString id = entry->context.pluginId;
                    QMetaObject::invokeMethod(this, [this, id] { unloadPlugin(id); },
                                              Qt::QueuedConnection);
                }
            }
        });
    }
}

PluginManager::~PluginManager()
{
    stop();
    m_lifetime.clear();
}

bool PluginManager::start()
{
    if (m_running || !m_core)
        return false;

    if (m_lifetime.isNull())
        m_lifetime = QSharedPointer<int>::create(1);

    if (!m_directory.exists() && !QDir().mkpath(m_directory.absolutePath()))
        return false;
    m_directory = QDir(m_directory.canonicalPath());
    if (!m_directory.exists())
        return false;

    m_running = true;
    m_watcher.addPath(m_directory.absolutePath());
    syncDirectory();
    return true;
}

void PluginManager::stop()
{
    if (!m_running)
        return;

    m_running = false;
    m_lifetime.clear();
    m_debounce.stop();
    m_watcher.removePaths(m_watcher.files());
    m_watcher.removePaths(m_watcher.directories());
    unloadAll();
}

void PluginManager::setGrantedPermissions(const QString &pluginId, Permissions permissions)
{
    static const QRegularExpression idPattern(
        QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}$"));
    if (!idPattern.match(pluginId).hasMatch())
        return;

    m_grants.insert(pluginId, permissions);
    const auto it = m_plugins.find(pluginId);
    if (it != m_plugins.end())
        it.value()->permissions = permissions;
}

void PluginManager::scheduleSync()
{
    if (m_running)
        m_debounce.start();
}

QString PluginManager::platformSuffix() const
{
#if defined(Q_OS_WIN)
    return QStringLiteral("dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("dylib");
#else
    return QStringLiteral("so");
#endif
}

bool PluginManager::isTrustedPluginPath(const QFileInfo &file) const
{
    if (!file.exists() || !file.isFile() || file.isSymLink())
        return false;

    const QString root = QDir::cleanPath(m_directory.canonicalPath()) + QDir::separator();
    const QString path = QDir::cleanPath(file.canonicalFilePath());
    return !root.isEmpty() && path.startsWith(root, Qt::CaseSensitive)
           && file.suffix().compare(platformSuffix(), Qt::CaseInsensitive) == 0;
}

void PluginManager::syncDirectory()
{
    if (!m_running)
        return;

    QHash<QString, QFileInfo> candidates;
    const QFileInfoList files = m_directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &file : files) {
        if (!isTrustedPluginPath(file))
            continue;

        candidates.insert(file.canonicalFilePath(), file);
        if (!m_watcher.files().contains(file.absoluteFilePath()))
            m_watcher.addPath(file.absoluteFilePath());
    }

    QStringList toRemove;
    for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it) {
        const Entry *entry = it.value().data();
        if (!entry || !candidates.contains(entry->canonicalPath)) {
            toRemove.append(it.key());
            continue;
        }

        const QFileInfo current = candidates.value(entry->canonicalPath);
        if (current.lastModified().toMSecsSinceEpoch() != entry->modifiedMs
            || current.size() != entry->fileSize)
            toRemove.append(it.key());
    }

    for (const QString &id : toRemove)
        unloadPlugin(id);

    QSet<QString> loadedPaths;
    for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it)
        loadedPaths.insert(it.value()->canonicalPath);

    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
        if (!loadedPaths.contains(it.key()))
            loadPlugin(it.value());
    }
}

bool PluginManager::loadPlugin(const QFileInfo &file)
{
    if (!m_running || !isTrustedPluginPath(file))
        return false;

    auto entry = QSharedPointer<Entry>::create();
    entry->loader = std::make_unique<QPluginLoader>(file.absoluteFilePath());
    entry->loader->setLoadHints(QLibrary::LoadHints{});

    QObject *instance = entry->loader->instance();
    if (!instance) {
        emit m_core->logMessage(Guard::redact(
            QStringLiteral("Plugin load failed: %1").arg(entry->loader->errorString())));
        return false;
    }

    IPlugin *plugin = qobject_cast<IPlugin *>(instance);
    if (!plugin) {
        entry->loader->unload();
        return false;
    }

    const QString id = Guard::cleanText(plugin->id(), 128);
    static const QRegularExpression idPattern(
        QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}$"));
    if (!idPattern.match(id).hasMatch() || m_plugins.contains(id)) {
        entry->loader->unload();
        return false;
    }

    entry->plugin = plugin;
    entry->canonicalPath = file.canonicalFilePath();
    entry->modifiedMs = file.lastModified().toMSecsSinceEpoch();
    entry->fileSize = file.size();
    entry->permissions = m_grants.value(id, Permissions(Permission::None));
    entry->active = true;
    entry->context.pluginId = id;

    const QString pluginId = id;
    const QWeakPointer<int> lifetime = m_lifetime;
    entry->context.sendMessage = [this, pluginId, lifetime](
                                     const QString &roomId, const QString &text) {
        const auto alive = lifetime.toStrongRef();
        if (!alive)
            return false;
        const auto it = m_plugins.constFind(pluginId);
        if (!m_running || it == m_plugins.cend() || !it.value()->active
            || !it.value()->permissions.testFlag(Permission::SendMessage))
            return false;
        return m_core && m_core->sendMessage(roomId, text);
    };
    entry->context.giftInfo = [this, pluginId, lifetime](
                                  quint64 giftId) -> std::optional<GiftInfo> {
        const auto alive = lifetime.toStrongRef();
        if (!alive)
            return std::nullopt;
        const auto it = m_plugins.constFind(pluginId);
        if (!m_running || it == m_plugins.cend() || !it.value()->active
            || !it.value()->permissions.testFlag(Permission::GiftInfo))
            return std::nullopt;
        return m_core ? m_core->giftInfo(giftId) : std::nullopt;
    };

    m_plugins.insert(id, entry);
    try {
        plugin->onLoad(entry->context);
    } catch (...) {
        unloadPlugin(id);
        return false;
    }

    emit m_core->logMessage(QStringLiteral("Plugin loaded: %1").arg(id));
    return true;
}

void PluginManager::unloadPlugin(const QString &pluginId)
{
    auto it = m_plugins.find(pluginId);
    if (it == m_plugins.end())
        return;

    const QSharedPointer<Entry> entry = it.value();
    entry->active = false;
    entry->context.sendMessage = {};
    entry->context.giftInfo = {};

    if (entry->plugin) {
        try {
            entry->plugin->onUnload();
        } catch (...) {
            // A plugin must not prevent host cleanup.
        }
        entry->plugin = nullptr;
    }

    if (entry->loader && entry->loader->isLoaded())
        entry->loader->unload();

    m_plugins.erase(it);
    if (m_core)
        emit m_core->logMessage(QStringLiteral("Plugin unloaded: %1").arg(pluginId));
}

void PluginManager::unloadAll()
{
    const QStringList ids = m_plugins.keys();
    for (const QString &id : ids)
        unloadPlugin(id);
}

} // namespace blkt
