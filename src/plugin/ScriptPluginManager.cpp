#include "ScriptPluginManager.hpp"

#include "../core/Core.hpp"
#include "../core/Guard.hpp"

#include <QFile>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace blkt {
namespace {
constexpr qint64 MaxScriptBytes = 512 * 1024;
constexpr int MaxPlugins = 16;
constexpr int MaxHandlersPerEvent = 8;
constexpr int MaxCallbacksPerEvent = 64;
constexpr int EventExecutionBudgetMs = 250;
constexpr int ScriptDeadlineMs = 250;

bool safePluginId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,48}$"));
    return pattern.match(id).hasMatch();
}

QJSValue runWithScriptDeadline(QJSEngine *engine, int timeoutMs,
                               const std::function<QJSValue()> &operation)
{
    if (!engine)
        return QJSValue();

    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::milliseconds(qMax(1, timeoutMs)),
                                [&] { return finished; })) {
            engine->setInterrupted(true);
        }
    });

    const QJSValue result = operation();
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }
    condition.notify_one();
    watchdog.join();
    engine->setInterrupted(false);
    return result;
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
        (eventName == QStringLiteral("message")
         && hasPermission(m_permissions, Permission::ReceiveMessage))
        || (eventName == QStringLiteral("gift")
            && hasPermission(m_permissions, Permission::ReceiveGift));
    if (m_disabled || !permitted || !callback.isCallable() || !m_engine)
        return false;

    auto &handlers = m_handlers[eventName];
    if (handlers.size() >= MaxHandlersPerEvent)
        return false;
    handlers.append(callback);
    return true;
}

bool ScriptPluginApi::sendMessage(const QString &text)
{
    if (m_disabled || !hasPermission(m_permissions, Permission::SendMessage)
        || !m_core || !m_core->isRoomConnected())
        return false;
    return m_core->sendMessage(m_core->activeRoomId(), text);
}

QVariantMap ScriptPluginApi::giftInfo(const QString &giftId) const
{
    if (m_disabled || !hasPermission(m_permissions, Permission::GiftInfo) || !m_core)
        return {};
    bool ok = false;
    const quint64 id = giftId.toULongLong(&ok);
    if (!ok || id == 0)
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
    if (m_disabled)
        return;
    const QString safe = Guard::cleanLogText(message, 512);
    if (!safe.isEmpty())
        emit logMessage(QStringLiteral("Plugin %1: %2").arg(m_pluginId, safe));
}

bool ScriptPluginApi::dispatch(const QString &eventName, const QVariantMap &event,
                                QElapsedTimer &eventTimer, int &callbacksRemaining)
{
    if (!m_engine || m_disabled)
        return true;

    const QJSValue argument = m_engine->toScriptValue(event);
    const auto handlers = m_handlers.value(eventName);
    for (const QJSValue &handler : handlers) {
        const qint64 elapsed = eventTimer.elapsed();
        if (callbacksRemaining <= 0 || elapsed >= EventExecutionBudgetMs) {
            emit logMessage(QStringLiteral(
                "Plugin event dispatch budget exhausted; remaining callbacks skipped"));
            return false;
        }

        --callbacksRemaining;
        const int remainingMs = EventExecutionBudgetMs - static_cast<int>(elapsed);
        const QJSValue result = runWithScriptDeadline(
            m_engine, qMin(ScriptDeadlineMs, remainingMs),
            [&handler, &argument] { return handler.call({argument}); });

        if (eventTimer.elapsed() >= EventExecutionBudgetMs) {
            m_disabled = true;
            m_handlers.clear();
            emit logMessage(QStringLiteral(
                "Plugin %1 disabled after exhausting the event execution budget")
                .arg(m_pluginId));
            return false;
        }

        if (result.isError()) {
            ++m_callbackFailures;
            const QString error = Guard::cleanLogText(result.toString(), 200);
            emit logMessage(QStringLiteral("Plugin %1 callback error (%2/3): %3")
                .arg(m_pluginId).arg(m_callbackFailures).arg(error));
            if (m_callbackFailures >= 3) {
                m_disabled = true;
                m_handlers.clear();
                emit logMessage(QStringLiteral("Plugin %1 disabled after repeated script failures")
                                    .arg(m_pluginId));
                return true;
            }
        } else {
            m_callbackFailures = 0;
        }
    }
    return true;
}

ScriptPluginManager::ScriptPluginManager(Core *core, const QString &directory,
                                         const QStringList &allowedPluginIds,
                                         const QStringList &sendMessagePluginIds,
                                         QObject *parent)
    : QObject(parent), m_core(core), m_directory(QDir(directory))
{
    for (const QString &id : allowedPluginIds)
        if (safePluginId(id))
            m_allowedIds.insert(id);
    for (const QString &id : sendMessagePluginIds)
        if (safePluginId(id) && m_allowedIds.contains(id))
            m_sendMessageIds.insert(id);

    if (m_core) {
        connect(m_core, &Core::messageReceived, this, &ScriptPluginManager::dispatchMessage);
        connect(m_core, &Core::giftReceived, this, &ScriptPluginManager::dispatchGift);
    }
}

ScriptPluginManager::~ScriptPluginManager()
{
    stop();
}

bool ScriptPluginManager::start()
{
    if (m_started || !m_core)
        return false;
    m_started = true;

    if (!m_directory.exists()) {
        emit m_core->logMessage(QStringLiteral(
            "Community plugin directory missing; no plugins loaded"));
        return true;
    }

    const QString canonical = m_directory.canonicalPath();
    if (canonical.isEmpty())
        return false;
    m_directory = QDir(canonical);

    const QFileInfoList scripts = m_directory.entryInfoList(
        {QStringLiteral("*.js")}, QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &script : scripts)
        loadPlugin(script);

    emit m_core->logMessage(QStringLiteral("Community script scan complete: %1 loaded")
                                .arg(m_plugins.size()));
    return true;
}

bool ScriptPluginManager::loadPlugin(const QFileInfo &scriptInfo)
{
    if (!scriptInfo.isFile() || scriptInfo.isSymLink()
        || m_plugins.size() >= MaxPlugins)
        return false;

    const QString id = scriptInfo.completeBaseName();
    if (!safePluginId(id) || !m_allowedIds.contains(id) || m_plugins.contains(id))
        return false;

    // QDir::cleanPath() normalizes separators to '/', including on Windows.
    // Normalize both canonical paths before the containment check.
    const QString root =
        QDir::cleanPath(QDir::fromNativeSeparators(m_directory.canonicalPath()))
        + QLatin1Char('/');
    const QString path =
        QDir::cleanPath(QDir::fromNativeSeparators(scriptInfo.canonicalFilePath()));
#if defined(Q_OS_WIN)
    constexpr Qt::CaseSensitivity pathCaseSensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCaseSensitivity = Qt::CaseSensitive;
#endif
    if (path.isEmpty() || !path.startsWith(root, pathCaseSensitivity)
        || scriptInfo.size() <= 0 || scriptInfo.size() > MaxScriptBytes) {
        emit m_core->logMessage(QStringLiteral("Plugin %1 rejected: invalid path or size").arg(id));
        return false;
    }

    QFile script(path);
    if (!script.open(QIODevice::ReadOnly))
        return false;
    const QByteArray source = script.read(MaxScriptBytes + 1);
    if (source.isEmpty() || source.size() > MaxScriptBytes
        || source.contains('\0')) {
        emit m_core->logMessage(QStringLiteral("Plugin %1 rejected: invalid JavaScript file").arg(id));
        return false;
    }
    const QString sourceText = QString::fromUtf8(source);
    if (sourceText.toUtf8() != source) {
        emit m_core->logMessage(QStringLiteral("Plugin %1 rejected: script is not valid UTF-8").arg(id));
        return false;
    }

    auto *entry = new Entry;
    entry->id = id;
    entry->engine = new QJSEngine(this);

    Permissions permissions = permissionFlag(Permission::ReceiveMessage)
        | permissionFlag(Permission::ReceiveGift)
        | permissionFlag(Permission::GiftInfo);
    if (m_sendMessageIds.contains(id))
        permissions |= permissionFlag(Permission::SendMessage);

    entry->api = new ScriptPluginApi(m_core, entry->engine, id, permissions, entry->engine);
    connect(entry->api, &ScriptPluginApi::logMessage, m_core, &Core::logMessage);
    entry->engine->globalObject().setProperty(
        QStringLiteral("BLKT"), entry->engine->newQObject(entry->api));

    const QJSValue result = runWithScriptDeadline(entry->engine, ScriptDeadlineMs, [&] {
        return entry->engine->evaluate(QStringLiteral("\"use strict\";\n") + sourceText, scriptInfo.fileName());
    });
    if (result.isError()) {
        emit m_core->logMessage(QStringLiteral("Plugin %1 failed at line %2: %3")
            .arg(id).arg(result.property(QStringLiteral("lineNumber")).toInt())
            .arg(Guard::cleanLogText(result.toString(), 256)));
        delete entry->engine;
        delete entry;
        return false;
    }

    m_plugins.insert(id, entry);
    emit m_core->logMessage(QStringLiteral("Community JavaScript plugin loaded: %1").arg(id));
    return true;
}

void ScriptPluginManager::dispatchMessage(const MessageEvent &event)
{
    const QVariantMap payload{
        {QStringLiteral("roomId"), event.roomId},
        {QStringLiteral("user"), event.user},
        {QStringLiteral("text"), event.text},
        {QStringLiteral("timestamp"), static_cast<qlonglong>(event.timestamp)}};
    QElapsedTimer eventTimer;
    eventTimer.start();
    int callbacksRemaining = MaxCallbacksPerEvent;
    for (Entry *entry : m_plugins) {
        if (eventTimer.elapsed() >= EventExecutionBudgetMs || callbacksRemaining <= 0)
            break;
        if (entry && entry->api
            && !entry->api->dispatch(QStringLiteral("message"), payload,
                                     eventTimer, callbacksRemaining))
            break;
    }
}

void ScriptPluginManager::dispatchGift(const GiftEvent &event)
{
    const QVariantMap payload{
        {QStringLiteral("roomId"), event.roomId},
        {QStringLiteral("user"), event.user},
        {QStringLiteral("giftName"), event.giftName},
        {QStringLiteral("giftId"), QString::number(event.giftId)},
        {QStringLiteral("count"), static_cast<uint>(event.count)},
        {QStringLiteral("totalCoin"), QString::number(event.totalCoin)},
        {QStringLiteral("coinType"), event.coinType},
        {QStringLiteral("timestamp"), static_cast<qlonglong>(event.timestamp)}};
    QElapsedTimer eventTimer;
    eventTimer.start();
    int callbacksRemaining = MaxCallbacksPerEvent;
    for (Entry *entry : m_plugins) {
        if (eventTimer.elapsed() >= EventExecutionBudgetMs || callbacksRemaining <= 0)
            break;
        if (entry && entry->api
            && !entry->api->dispatch(QStringLiteral("gift"), payload,
                                     eventTimer, callbacksRemaining))
            break;
    }
}

void ScriptPluginManager::unloadPlugin(const QString &id)
{
    Entry *entry = m_plugins.take(id);
    if (!entry)
        return;
    delete entry->engine;
    delete entry;
}

void ScriptPluginManager::stop()
{
    if (!m_started)
        return;
    m_started = false;
    const QStringList ids = m_plugins.keys();
    for (const QString &id : ids)
        unloadPlugin(id);
}
}
