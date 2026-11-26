#include "PluginManager.hpp"
#include "../core/Core.hpp"
#include "../core/Guard.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QProcessEnvironment>
#include <QTextStream>

namespace blkt {
namespace {

constexpr qint64 MaxPluginBytes = 256LL * 1024LL * 1024LL;

QString normalizedId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}$"));
    return pattern.match(value).hasMatch() ? value : QString();
}

QJsonObject messageToJson(const MessageEvent &event)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), QStringLiteral("event"));
    object.insert(QStringLiteral("event"), QStringLiteral("message"));
    object.insert(QStringLiteral("user"), event.user);
    object.insert(QStringLiteral("text"), event.text);
    object.insert(QStringLiteral("timestamp"), static_cast<double>(event.timestamp));
    return object;
}

QJsonObject giftToJson(const GiftEvent &event)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), QStringLiteral("event"));
    object.insert(QStringLiteral("event"), QStringLiteral("gift"));
    object.insert(QStringLiteral("user"), event.user);
    object.insert(QStringLiteral("gift_name"), event.giftName);
    object.insert(QStringLiteral("gift_id"), QString::number(event.giftId));
    object.insert(QStringLiteral("count"), static_cast<int>(event.count));
    object.insert(QStringLiteral("total_coin"), QString::number(event.totalCoin));
    object.insert(QStringLiteral("coin_type"), event.coinType);
    object.insert(QStringLiteral("timestamp"), static_cast<double>(event.timestamp));
    return object;
}

} // namespace

PluginManager::PluginManager(Core *core, const QString &directory, QObject *parent)
    : QObject(parent), m_core(core), m_directory(directory)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &PluginManager::syncDirectory);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &PluginManager::scheduleSync);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &PluginManager::scheduleSync);

    if (m_core) {
        connect(m_core, &Core::messageReceived, this, [this](const MessageEvent &event) {
            const QJsonObject packet = messageToJson(event);
            const auto entries = m_plugins;
            for (Entry *entry : entries) {
                if (entry && entry->ready
                    && hasPermission(entry->permissions, Permission::ReceiveMessage))
                    sendPacket(entry, packet);
            }
        });
        connect(m_core, &Core::giftReceived, this, [this](const GiftEvent &event) {
            const QJsonObject packet = giftToJson(event);
            const auto entries = m_plugins;
            for (Entry *entry : entries) {
                if (entry && entry->ready
                    && hasPermission(entry->permissions, Permission::ReceiveGift))
                    sendPacket(entry, packet);
            }
        });
    }
}

PluginManager::~PluginManager()
{
    stop();
}

bool PluginManager::start()
{
    if (m_running || !m_core)
        return false;

    m_running = true;
    if (!m_directory.exists()) {
        emit m_core->logMessage(QStringLiteral(
            "Plugin directory is absent; no plugin files were created"));
        return true;
    }

    const QString canonical = m_directory.canonicalPath();
    if (canonical.isEmpty()) {
        m_running = false;
        return false;
    }
    m_directory = QDir(canonical);
    m_watcher.addPath(m_directory.absolutePath());
    syncDirectory();
    return true;
}

void PluginManager::stop()
{
    if (!m_running)
        return;

    m_running = false;
    m_debounce.stop();
    m_watcher.removePaths(m_watcher.files());
    m_watcher.removePaths(m_watcher.directories());
    stopAll();
}

void PluginManager::setGrantedPermissions(const QString &pluginId, Permissions permissions)
{
    const QString id = normalizedId(pluginId);
    if (id.isEmpty())
        return;

    constexpr Permissions known = permissionFlag(Permission::ReceiveMessage)
        | permissionFlag(Permission::SendMessage)
        | permissionFlag(Permission::ReceiveGift)
        | permissionFlag(Permission::GiftInfo);
    permissions &= known;
    m_grants.insert(id, permissions);

    Entry *entry = m_plugins.value(id, nullptr);
    if (entry) {
        entry->permissions = permissions;
        QJsonObject packet;
        packet.insert(QStringLiteral("type"), QStringLiteral("permissions"));
        packet.insert(QStringLiteral("permissions"), permissionsFor(permissions));
        sendPacket(entry, packet);
    }
}

bool PluginManager::trustPlugin(const QString &fileName, const QByteArray &sha256)
{
    const QFileInfo nameInfo(fileName);
    static const QRegularExpression digestPattern(QStringLiteral("^[A-Fa-f0-9]{64}$"));
    if (fileName.isEmpty() || nameInfo.fileName() != fileName
        || fileName == QStringLiteral(".") || fileName == QStringLiteral("..")
        || fileName.contains(QLatin1Char('/')) || fileName.contains(QLatin1Char('\\'))
        || !digestPattern.match(QString::fromLatin1(sha256)).hasMatch())
        return false;

    m_trustedHashes.insert(fileName, sha256.toLower());
    scheduleSync();
    return true;
}

void PluginManager::revokePluginTrust(const QString &fileName)
{
    m_trustedHashes.remove(fileName);
    QStringList remove;
    for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it) {
        if (QFileInfo(it.value()->canonicalPath).fileName() == fileName)
            remove.append(it.key());
    }
    for (const QString &id : remove)
        stopPlugin(id);
    scheduleSync();
}

QString PluginManager::platformSuffix() const
{
#if defined(Q_OS_WIN)
    return QStringLiteral("exe");
#else
    return QStringLiteral("blkt-plugin");
#endif
}

bool PluginManager::isAllowedPluginFile(const QFileInfo &file) const
{
    if (!file.exists() || !file.isFile() || file.isSymLink()
        || file.suffix().compare(platformSuffix(), Qt::CaseInsensitive) != 0)
        return false;

#if !defined(Q_OS_WIN)
    if (!file.isExecutable())
        return false;
#endif

    const QString root = QDir::cleanPath(m_directory.canonicalPath()) + QDir::separator();
    const QString path = QDir::cleanPath(file.canonicalFilePath());
    return !root.isEmpty() && path.startsWith(root, Qt::CaseSensitive);
}

bool PluginManager::hasTrustedDigest(const QFileInfo &file) const
{
    const auto expected = m_trustedHashes.constFind(file.fileName());
    if (expected == m_trustedHashes.cend() || file.size() <= 0
        || file.size() > MaxPluginBytes)
        return false;

    QFile input(file.canonicalFilePath());
    if (!input.open(QIODevice::ReadOnly))
        return false;

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&input))
        return false;
    return hash.result().toHex() == expected.value();
}

void PluginManager::scheduleSync()
{
    if (m_running && m_directory.exists())
        m_debounce.start();
}

void PluginManager::syncDirectory()
{
    if (!m_running || !m_directory.exists())
        return;

    QHash<QString, QFileInfo> candidates;
    const QFileInfoList files =
        m_directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &file : files) {
        if (!isAllowedPluginFile(file) || !hasTrustedDigest(file))
            continue;
        candidates.insert(file.canonicalFilePath(), file);
        if (!m_watcher.files().contains(file.absoluteFilePath()))
            m_watcher.addPath(file.absoluteFilePath());
    }

    QStringList stopIds;
    for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it) {
        Entry *entry = it.value();
        if (!entry || !candidates.contains(entry->canonicalPath)) {
            stopIds.append(it.key());
            continue;
        }
        const QFileInfo current = candidates.value(entry->canonicalPath);
        if (!hasTrustedDigest(current))
            stopIds.append(it.key());
    }
    for (const QString &id : stopIds)
        stopPlugin(id);

    QSet<QString> runningPaths;
    for (Entry *entry : m_plugins)
        if (entry)
            runningPaths.insert(entry->canonicalPath);

    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
        if (!runningPaths.contains(it.key()))
            launchPlugin(it.value());
    }
}

bool PluginManager::launchPlugin(const QFileInfo &file)
{
    if (!m_running || !isAllowedPluginFile(file) || !hasTrustedDigest(file))
        return false;

    QString id = file.fileName();
    const QString suffix = QLatin1Char('.') + platformSuffix();
    if (id.endsWith(suffix, Qt::CaseInsensitive))
        id.chop(suffix.size());
    id = normalizedId(id);
    if (id.isEmpty() || m_plugins.contains(id))
        return false;

    auto *entry = new Entry;
    entry->id = id;
    entry->canonicalPath = file.canonicalFilePath();
    entry->permissions = m_grants.value(id, 0);
    entry->process = new QProcess(this);
    entry->readyTimer = new QTimer(entry->process);
    entry->readyTimer->setSingleShot(true);

    QProcess *process = entry->process;
    m_plugins.insert(id, entry);

    connect(process, &QProcess::started, this, [this, entry] {
        QJsonObject init;
        init.insert(QStringLiteral("type"), QStringLiteral("initialize"));
        init.insert(QStringLiteral("protocol"), 1);
        init.insert(QStringLiteral("plugin_id"), entry->id);
        init.insert(QStringLiteral("permissions"), permissionsFor(entry->permissions));
        if (!sendPacket(entry, init))
            stopPlugin(entry->id);
    });
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, id] { consumeStdout(id); });
    connect(process, &QProcess::readyReadStandardError, this, [this, id, process] {
        const QByteArray ignored = process->readAllStandardError();
        if (!ignored.isEmpty() && m_core)
            emit m_core->logMessage(QStringLiteral("Plugin %1 wrote to stderr (content suppressed)").arg(id));
    });
    connect(entry->readyTimer, &QTimer::timeout, this, [this, id] {
        Entry *current = m_plugins.value(id, nullptr);
        if (current && !current->ready) {
            if (m_core)
                emit m_core->logMessage(QStringLiteral("Plugin startup handshake timed out: %1").arg(id));
            stopPlugin(id);
        }
    });
    connect(process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this, id, entry](int, QProcess::ExitStatus) {
        if (m_plugins.value(id, nullptr) == entry)
            m_plugins.remove(id);
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Plugin process exited: %1").arg(id));
        entry->process->deleteLater();
        delete entry;
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, id](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            if (m_core)
                emit m_core->logMessage(QStringLiteral("Plugin process could not start: %1").arg(id));
            stopPlugin(id);
        }
    });

    process->setProcessChannelMode(QProcess::SeparateChannels);

    // Do not leak host environment variables (tokens, proxy credentials, etc.)
    // into a third-party worker.
    QProcessEnvironment environment;
#if defined(Q_OS_WIN)
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    if (!systemRoot.isEmpty()) {
        environment.insert(QStringLiteral("SystemRoot"), systemRoot);
        environment.insert(QStringLiteral("WINDIR"), systemRoot);
        environment.insert(QStringLiteral("PATH"),
                           QDir(systemRoot).filePath(QStringLiteral("System32")));
    }
#else
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    const QString lang = qEnvironmentVariable("LANG");
    if (!lang.isEmpty())
        environment.insert(QStringLiteral("LANG"), lang);
#endif
    process->setProcessEnvironment(environment);
    process->start(file.canonicalFilePath(),
                   {QStringLiteral("--blkt-plugin-worker"),
                    QStringLiteral("--blkt-protocol=1")});
    entry->readyTimer->start(ReadyTimeoutMs);
    return true;
}

bool PluginManager::sendPacket(Entry *entry, const QJsonObject &packet)
{
    if (!entry || !entry->process || entry->process->state() == QProcess::NotRunning)
        return false;

    const QByteArray bytes = QJsonDocument(packet).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > MaxPacketBytes
        || entry->process->bytesToWrite() + bytes.size() > 256 * 1024)
        return false;
    return entry->process->write(bytes) == bytes.size();
}

void PluginManager::consumeStdout(const QString &pluginId)
{
    Entry *entry = m_plugins.value(pluginId, nullptr);
    if (!entry || !entry->process)
        return;

    entry->inputBuffer.append(entry->process->readAllStandardOutput());
    if (entry->inputBuffer.size() > MaxPacketBytes
        && !entry->inputBuffer.contains('\n')) {
        stopPlugin(pluginId);
        return;
    }

    while (true) {
        const qsizetype newline = entry->inputBuffer.indexOf('\n');
        if (newline < 0)
            break;
        if (newline > MaxPacketBytes) {
            stopPlugin(pluginId);
            return;
        }

        const QByteArray line = entry->inputBuffer.left(newline).trimmed();
        entry->inputBuffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            stopPlugin(pluginId);
            return;
        }
        handlePacket(pluginId, document.object());
        if (!m_plugins.contains(pluginId))
            return;
    }
}

void PluginManager::handlePacket(const QString &pluginId, const QJsonObject &packet)
{
    Entry *entry = m_plugins.value(pluginId, nullptr);
    if (!entry)
        return;

    const QString type = packet.value(QStringLiteral("type")).toString();
    if (!entry->ready) {
        const bool valid = type == QStringLiteral("ready")
            && packet.value(QStringLiteral("protocol")).toInt(-1) == 1
            && packet.value(QStringLiteral("plugin_id")).toString() == pluginId;
        if (!valid) {
            stopPlugin(pluginId);
            return;
        }
        entry->ready = true;
        entry->readyTimer->stop();
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Plugin worker ready: %1").arg(pluginId));
        return;
    }

    if (type == QStringLiteral("request"))
        handleRequest(entry, packet);
    else if (type == QStringLiteral("log")) {
        // Plugin log content is intentionally not forwarded: it may contain secrets.
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Plugin %1 emitted a log event").arg(pluginId));
    } else {
        stopPlugin(pluginId);
    }
}

void PluginManager::handleRequest(Entry *entry, const QJsonObject &packet)
{
    if (!entry || !entry->ready)
        return;

    const QString requestId = packet.value(QStringLiteral("id")).toString();
    const QString operation = packet.value(QStringLiteral("operation")).toString();
    const QJsonObject payload = packet.value(QStringLiteral("payload")).toObject();
    if (requestId.isEmpty() || requestId.size() > 128)
        return;

    QJsonObject response;
    response.insert(QStringLiteral("type"), QStringLiteral("response"));
    response.insert(QStringLiteral("id"), requestId);
    bool ok = false;

    if (operation == QStringLiteral("send_message")
        && hasPermission(entry->permissions, Permission::SendMessage)) {
        const QString roomId = payload.value(QStringLiteral("room_id")).toString();
        const QString text = payload.value(QStringLiteral("text")).toString();
        ok = m_core && m_core->sendMessage(roomId, text);
    } else if (operation == QStringLiteral("gift_info")
               && hasPermission(entry->permissions, Permission::GiftInfo)) {
        bool idOk = false;
        const quint64 giftId = payload.value(QStringLiteral("gift_id")).toString().toULongLong(&idOk);
        if (idOk && m_core) {
            const auto info = m_core->giftInfo(giftId);
            if (info) {
                QJsonObject gift;
                gift.insert(QStringLiteral("id"), QString::number(info->id));
                gift.insert(QStringLiteral("name"), info->name);
                gift.insert(QStringLiteral("price"), QString::number(info->price));
                gift.insert(QStringLiteral("coin_type"), info->coinType);
                response.insert(QStringLiteral("result"), gift);
                ok = true;
            }
        }
    }

    response.insert(QStringLiteral("ok"), ok);
    sendPacket(entry, response);
}

QStringList PluginManager::permissionsFor(Permissions permissions) const
{
    QStringList result;
    if (hasPermission(permissions, Permission::ReceiveMessage))
        result.append(QStringLiteral("receive_message"));
    if (hasPermission(permissions, Permission::SendMessage))
        result.append(QStringLiteral("send_message"));
    if (hasPermission(permissions, Permission::ReceiveGift))
        result.append(QStringLiteral("receive_gift"));
    if (hasPermission(permissions, Permission::GiftInfo))
        result.append(QStringLiteral("gift_info"));
    return result;
}

void PluginManager::stopPlugin(const QString &pluginId)
{
    Entry *entry = m_plugins.take(pluginId);
    if (!entry)
        return;

    entry->stopping = true;
    entry->ready = false;
    QProcess *process = entry->process.data();

    if (process) {
        // Prevent the finished callback from deleting Entry while this
        // synchronous, bounded shutdown path is still using it.
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            QJsonObject shutdown;
            shutdown.insert(QStringLiteral("type"), QStringLiteral("shutdown"));
            const QByteArray packet =
                QJsonDocument(shutdown).toJson(QJsonDocument::Compact) + '\\n';
            process->write(packet);
            process->waitForBytesWritten(100);
            if (!process->waitForFinished(GracefulStopMs)) {
                process->terminate();
                if (!process->waitForFinished(250)) {
                    process->kill();
                    process->waitForFinished(500);
                }
            }
        }
        process->deleteLater();
    }

    delete entry;
}

