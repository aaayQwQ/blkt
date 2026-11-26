#include "ScriptPluginManager.hpp"
#include "../core/Core.hpp"
#include "../core/Guard.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace blkt {
namespace {
constexpr qint64 MaxManifestBytes = 64 * 1024;
constexpr qint64 MaxScriptBytes = 512 * 1024;
bool safePluginId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,48}$"));
    return re.match(id).hasMatch();
}

QJSValue runWithScriptDeadline(QJSEngine *engine,
                               const std::function<QJSValue()> &operation)
{
    if (!engine)
        return QJSValue();

    std::mutex mutex;
    std::condition_variable finishedCondition;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!finishedCondition.wait_for(lock, std::chrono::milliseconds(250),
                                        [&] { return finished; })) {
            engine->setInterrupted(true);
        }
    });

    const QJSValue result = operation();
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }
    finishedCondition.notify_one();
    watchdog.join();
    engine->setInterrupted(false);
    return result;
}
Permissions parsePermissions(const QJsonArray &items)
{
    Permissions permissions{};
    for (const QJsonValue &value : items) {
        const QString p = value.toString();
        if (p == QStringLiteral("receive_message")) permissions |= permissionFlag(Permission::ReceiveMessage);
        else if (p == QStringLiteral("send_message")) permissions |= permissionFlag(Permission::SendMessage);
        else if (p == QStringLiteral("receive_gift")) permissions |= permissionFlag(Permission::ReceiveGift);
        else if (p == QStringLiteral("gift_info")) permissions |= permissionFlag(Permission::GiftInfo);
    }
    return permissions;
}
}

ScriptPluginApi::ScriptPluginApi(Core *core, QJSEngine *engine, QString pluginId,
                                 Permissions permissions, QObject *parent)
    : QObject(parent), m_core(core), m_engine(engine),
      m_pluginId(std::move(pluginId)), m_permissions(permissions)
{
}

bool ScriptPluginApi::on(const QString &eventName, const QJSValue &callback)
{
    const bool permitted =
        (eventName == QStringLiteral("message") && hasPermission(m_permissions, Permission::ReceiveMessage))
        || (eventName == QStringLiteral("gift") && hasPermission(m_permissions, Permission::ReceiveGift));
    if (m_disabled || !permitted || !callback.isCallable() || !m_engine)
        return false;
    auto &handlers = m_handlers[eventName];
    if (handlers.size() >= 32)
        return false;
    handlers.append(callback);
    return true;
}

bool ScriptPluginApi::sendMessage(const QString &roomId, const QString &text)
{
    return hasPermission(m_permissions, Permission::SendMessage)
        && m_core && m_core->sendMessage(roomId, text);
}

QVariantMap ScriptPluginApi::giftInfo(const QString &giftId) const
{
    if (!hasPermission(m_permissions, Permission::GiftInfo) || !m_core)
        return {};
    bool ok = false;
    const quint64 id = giftId.toULongLong(&ok);
    if (!ok)
        return {};
    const auto gift = m_core->giftInfo(id);
    if (!gift)
        return {};
    return {{QStringLiteral("id"), QString::number(gift->id)},
            {QStringLiteral("name"), gift->name},
            {QStringLiteral("price"), QString::number(gift->price)},
            {QStringLiteral("coin_type"), gift->coinType}};
}

void ScriptPluginApi::log(const QString &message)
{
    const QString safe = Guard::cleanText(message, 512);
    if (!safe.isEmpty())
        emit logMessage(QStringLiteral("Plugin %1: %2").arg(m_pluginId, safe));
}

void ScriptPluginApi::dispatch(const QString &eventName, const QVariantMap &event)
{
    if (!m_engine || m_disabled)
        return;
    const QJSValue argument = m_engine->toScriptValue(event);
    const auto handlers = m_handlers.value(eventName);
    for (const QJSValue &handler : handlers) {
        const QJSValue result = runWithScriptDeadline(
            m_engine, [&handler, &argument] { return handler.call({argument}); });
        if (result.isError()) {
            ++m_callbackFailures;
            emit logMessage(QStringLiteral("Plugin %1 callback error (%2/3): %3")
                .arg(m_pluginId).arg(m_callbackFailures).arg(result.toString().left(200)));
            if (m_callbackFailures >= 3) {
                m_disabled = true;
                m_handlers.clear();
                emit logMessage(QStringLiteral("Plugin %1 disabled after repeated script failures")
                                    .arg(m_pluginId));
                return;
            }
        } else {
            m_callbackFailures = 0;
        }
    }
}

ScriptPluginManager::ScriptPluginManager(Core *core, const QString &directory,
                                         const QStringList &allowedPluginIds,
                                         QObject *parent)
    : QObject(parent), m_core(core), m_directory(QDir(directory))
{
    for (const QString &id : allowedPluginIds)
        if (safePluginId(id)) m_allowedIds.insert(id);
    if (m_core) {
        connect(m_core, &Core::messageReceived, this, &ScriptPluginManager::dispatchMessage);
        connect(m_core, &Core::giftReceived, this, &ScriptPluginManager::dispatchGift);
    }
}
ScriptPluginManager::~ScriptPluginManager() { stop(); }

bool ScriptPluginManager::start()
{
    if (m_started || !m_core) return false;
    m_started = true;
    if (!m_directory.exists()) {
        emit m_core->logMessage(QStringLiteral("Plugin directory missing; script plugins disabled"));
        return true;
    }
    const QString canonical = m_directory.canonicalPath();
    if (canonical.isEmpty()) return false;
    m_directory = QDir(canonical);
    const QFileInfoList dirs = m_directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &dir : dirs) loadPlugin(dir);
    emit m_core->logMessage(QStringLiteral("Script plugin scan complete: %1 loaded").arg(m_plugins.size()));
    return true;
}

bool ScriptPluginManager::loadPlugin(const QFileInfo &dirInfo)
{
    if (!dirInfo.isDir() || dirInfo.isSymLink()) return false;
    const QString id = dirInfo.fileName();
    if (!safePluginId(id) || !m_allowedIds.contains(id) || m_plugins.contains(id) || m_plugins.size() >= 16) return false;
    const QString root = QDir::cleanPath(m_directory.canonicalPath()) + QDir::separator();
    const QString path = QDir::cleanPath(dirInfo.canonicalFilePath());
    if (path.isEmpty() || !path.startsWith(root, Qt::CaseSensitive)) return false;

    const QFileInfo manifestInfo(QDir(path).filePath(QStringLiteral("manifest.json")));
    const QFileInfo scriptInfo(QDir(path).filePath(QStringLiteral("main.js")));
    if (!manifestInfo.isFile() || manifestInfo.isSymLink() || manifestInfo.size() <= 0
        || manifestInfo.size() > MaxManifestBytes || !scriptInfo.isFile()
        || scriptInfo.isSymLink() || scriptInfo.size() <= 0 || scriptInfo.size() > MaxScriptBytes) {
        emit m_core->logMessage(QStringLiteral("Plugin %1 rejected: invalid manifest or script file").arg(id));
        return false;
    }
    QFile mf(manifestInfo.canonicalFilePath()), sf(scriptInfo.canonicalFilePath());
    if (!mf.open(QIODevice::ReadOnly) || !sf.open(QIODevice::ReadOnly)) return false;
    const QByteArray manifestBytes = mf.read(MaxManifestBytes + 1);
    const QByteArray scriptBytes = sf.read(MaxScriptBytes + 1);
    if (manifestBytes.size() > MaxManifestBytes || scriptBytes.size() > MaxScriptBytes) return false;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject manifest = doc.object();
    if (manifest.value(QStringLiteral("id")).toString() != id
        || manifest.value(QStringLiteral("api_version")).toInt(-1) != 1
        || manifest.value(QStringLiteral("entry")).toString() != QStringLiteral("main.js")) {
        emit m_core->logMessage(QStringLiteral("Plugin %1 rejected: manifest identity/API mismatch").arg(id));
        return false;
    }

    auto *entry = new Entry;
    entry->id = id;
    entry->path = path;
    entry->digest = QCryptographicHash::hash(manifestBytes + scriptBytes, QCryptographicHash::Sha256).toHex();
    entry->engine = new QJSEngine(this);
    entry->api = new ScriptPluginApi(m_core, entry->engine, id,
        parsePermissions(manifest.value(QStringLiteral("permissions")).toArray()), entry->engine);
    connect(entry->api, &ScriptPluginApi::logMessage, m_core, &Core::logMessage);
    entry->engine->globalObject().setProperty(QStringLiteral("BLKT"), entry->engine->newQObject(entry->api));
    const QJSValue result = runWithScriptDeadline(entry->engine, [&entry, &scriptBytes] {
        return entry->engine->evaluate(QString::fromUtf8(scriptBytes), QStringLiteral("main.js"));
    });
    if (result.isError()) {
        emit m_core->logMessage(QStringLiteral("Plugin %1 failed at line %2: %3")
            .arg(id).arg(result.property(QStringLiteral("lineNumber")).toInt())
            .arg(result.toString().left(256)));
        delete entry->engine;
        delete entry;
        return false;
    }
    m_plugins.insert(id, entry);
    emit m_core->logMessage(QStringLiteral("Script plugin loaded: %1 (SHA-256 %2)")
        .arg(id, QString::fromLatin1(entry->digest.left(16))));
    return true;
}

void ScriptPluginManager::dispatchMessage(const MessageEvent &e)
{
    const QVariantMap event{{QStringLiteral("room_id"), e.roomId},
        {QStringLiteral("user"), e.user}, {QStringLiteral("text"), e.text},
        {QStringLiteral("timestamp"), static_cast<qlonglong>(e.timestamp)}};
    for (Entry *entry : m_plugins) if (entry && entry->api) entry->api->dispatch(QStringLiteral("message"), event);
}
void ScriptPluginManager::dispatchGift(const GiftEvent &e)
{
    const QVariantMap event{{QStringLiteral("room_id"), e.roomId},
        {QStringLiteral("user"), e.user}, {QStringLiteral("gift_name"), e.giftName},
        {QStringLiteral("gift_id"), QString::number(e.giftId)},
        {QStringLiteral("count"), static_cast<uint>(e.count)},
        {QStringLiteral("total_coin"), QString::number(e.totalCoin)},
        {QStringLiteral("coin_type"), e.coinType},
        {QStringLiteral("timestamp"), static_cast<qlonglong>(e.timestamp)}};
    for (Entry *entry : m_plugins) if (entry && entry->api) entry->api->dispatch(QStringLiteral("gift"), event);
}
void ScriptPluginManager::unloadPlugin(const QString &id)
{
    Entry *entry = m_plugins.take(id);
    if (!entry) return;
    delete entry->engine;
    delete entry;
}
void ScriptPluginManager::stop()
{
    if (!m_started) return;
    m_started = false;
    const QStringList ids = m_plugins.keys();
    for (const QString &id : ids) unloadPlugin(id);
}
}
