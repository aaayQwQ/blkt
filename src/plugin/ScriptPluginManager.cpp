#include "ScriptPluginManager.hpp"

#include "../core/Core.hpp"
#include "../core/Guard.hpp"

#include <QFile>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <cmath>
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
constexpr qsizetype MaxOpenAIRequestBytes = 64 * 1024;
constexpr qint64 MaxOpenAIResponseBytes = 1024 * 1024;
constexpr qsizetype MaxOpenAIMessages = 24;
constexpr qsizetype MaxOpenAIMessageBytes = 16 * 1024;
constexpr qsizetype MaxOpenAIKeyBytes = 4096;
constexpr qsizetype MaxOpenAIContentChars = 16384;
constexpr int OpenAITimeoutMs = 45000;

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
      m_aiNetwork(new QNetworkAccessManager(this)),
      m_pluginId(std::move(pluginId)), m_permissions(permissions)
{
}

ScriptPluginApi::~ScriptPluginApi()
{
    m_shuttingDown = true;
    const auto replies = m_pendingAiCallbacks.keys();
    m_pendingAiCallbacks.clear();
    for (QNetworkReply *reply : replies) {
        if (!reply)
            continue;
        disconnect(reply, nullptr, this, nullptr);
        if (!reply->isFinished())
            reply->abort();
        reply->deleteLater();
    }
}

bool ScriptPluginApi::on(const QString &eventName, const QJSValue &callback)
{
    bool permitted = false;
    if (eventName == QStringLiteral("message"))
        permitted = hasPermission(m_permissions, Permission::ReceiveMessage);
    else if (eventName == QStringLiteral("gift"))
        permitted = hasPermission(m_permissions, Permission::ReceiveGift);
    else if (eventName == QStringLiteral("interaction"))
        permitted = hasPermission(m_permissions, Permission::ReceiveInteraction);
    else if (eventName == QStringLiteral("entry"))
        permitted = hasPermission(m_permissions, Permission::ReceiveEntry);
    else if (eventName == QStringLiteral("like"))
        permitted = hasPermission(m_permissions, Permission::ReceiveLike);
    else if (eventName == QStringLiteral("watched"))
        permitted = hasPermission(m_permissions, Permission::ReceiveWatched);
    else if (eventName == QStringLiteral("notice"))
        permitted = hasPermission(m_permissions, Permission::ReceiveNotice);
    else if (eventName == QStringLiteral("superChat"))
        permitted = hasPermission(m_permissions, Permission::ReceiveSuperChat);
    else if (eventName == QStringLiteral("guard"))
        permitted = hasPermission(m_permissions, Permission::ReceiveGuard);
    else if (eventName == QStringLiteral("comboGift"))
        permitted = hasPermission(m_permissions, Permission::ReceiveComboGift);
    else if (eventName == QStringLiteral("online"))
        permitted = hasPermission(m_permissions, Permission::ReceiveOnline);
    else if (eventName == QStringLiteral("roomStats"))
        permitted = hasPermission(m_permissions, Permission::ReceiveRoomStats);
    if (m_disabled || !permitted || !callback.isCallable() || !m_engine) {
        emit logMessage(QStringLiteral("Plugin %1 rejected event handler registration for '%2'")
                            .arg(m_pluginId, eventName));
        return false;
    }

    auto &handlers = m_handlers[eventName];
    if (handlers.size() >= MaxHandlersPerEvent) {
        emit logMessage(QStringLiteral("Plugin %1 reached the handler limit for '%2'")
                            .arg(m_pluginId, eventName));
        return false;
    }
    handlers.append(callback);
    emit logMessage(QStringLiteral("Plugin %1 registered '%2' event handler")
                        .arg(m_pluginId, eventName));
    return true;
}

bool ScriptPluginApi::openAIChat(const QJSValue &requestValue,
                                 const QJSValue &callback)
{
    if (m_disabled || m_shuttingDown || !m_engine
        || !hasPermission(m_permissions, Permission::OpenAIRequest)) {
        emit logMessage(QStringLiteral(
            "Plugin %1 OpenAI request denied: --plugin-openai permission is required")
            .arg(m_pluginId));
        return false;
    }
    if (!requestValue.isObject() || !callback.isCallable()) {
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid arguments")
                            .arg(m_pluginId));
        return false;
    }
    if (!m_pendingAiCallbacks.isEmpty()) {
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: request already active")
                            .arg(m_pluginId));
        return false;
    }
    if (!QSslSocket::supportsSsl()) {
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: Qt TLS backend unavailable")
                            .arg(m_pluginId));
        return false;
    }

    const QJSValue endpointValue = requestValue.property(QStringLiteral("endpoint"));
    const QJSValue keyValue = requestValue.property(QStringLiteral("api_key"));
    const QJSValue modelValue = requestValue.property(QStringLiteral("model"));
    const QJSValue messagesValue = requestValue.property(QStringLiteral("messages"));
    if (!endpointValue.isString() || !keyValue.isString()
        || !modelValue.isString() || !messagesValue.isArray()) {
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: missing request fields")
                            .arg(m_pluginId));
        return false;
    }

    const QString endpointText = endpointValue.toString().trimmed();
    const QUrl endpoint(endpointText, QUrl::StrictMode);
    const QString host = endpoint.host().toLower();
    QHostAddress literalAddress;
    const bool localSuffix =
        host == QStringLiteral("localhost")
        || host.endsWith(QStringLiteral(".localhost"))
        || host.endsWith(QStringLiteral(".local"))
        || host.endsWith(QStringLiteral(".internal"))
        || host.endsWith(QStringLiteral(".test"))
        || host.endsWith(QStringLiteral(".home.arpa"));
    if (endpointText.size() > 2048 || !endpoint.isValid()
        || endpoint.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || host.isEmpty() || !host.contains(QLatin1Char('.'))
        || host.endsWith(QLatin1Char('.')) || localSuffix
        || literalAddress.setAddress(host)
        || !endpoint.userInfo().isEmpty() || !endpoint.fragment().isEmpty()
        || endpoint.path().isEmpty()) {
        emit logMessage(QStringLiteral(
            "Plugin %1 OpenAI request rejected: endpoint must be a public HTTPS URL")
            .arg(m_pluginId));
        return false;
    }

    QString apiKey = keyValue.toString();
    QByteArray apiKeyBytes = apiKey.toUtf8();
    bool invalidKey = apiKeyBytes.isEmpty() || apiKeyBytes.size() > MaxOpenAIKeyBytes;
    for (const QChar ch : apiKey) {
        if (ch.isSpace() || ch.isNull() || ch.unicode() < 0x20
            || ch.unicode() == 0x7f) {
            invalidKey = true;
            break;
        }
    }
    if (invalidKey) {
        apiKeyBytes.fill('\0');
        apiKey.fill(QChar(u'\0'));
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid API key")
                            .arg(m_pluginId));
        return false;
    }

    const QString model = modelValue.toString().trimmed();
    static const QRegularExpression modelPattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}$"));
    if (!modelPattern.match(model).hasMatch()) {
        apiKeyBytes.fill('\0');
        apiKey.fill(QChar(u'\0'));
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid model")
                            .arg(m_pluginId));
        return false;
    }

    const int messageCount = messagesValue.property(QStringLiteral("length")).toInt(-1);
    if (messageCount < 1 || messageCount > static_cast<int>(MaxOpenAIMessages)) {
        apiKeyBytes.fill('\0');
        apiKey.fill(QChar(u'\0'));
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid message count")
                            .arg(m_pluginId));
        return false;
    }

    QJsonArray messages;
    qsizetype messageBytes = 0;
    for (int i = 0; i < messageCount; ++i) {
        const QJSValue item = messagesValue.property(i);
        if (!item.isObject()) {
            apiKeyBytes.fill('\0');
            apiKey.fill(QChar(u'\0'));
            emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid message")
                                .arg(m_pluginId));
            return false;
        }

        const QJSValue roleValue = item.property(QStringLiteral("role"));
        const QJSValue contentValue = item.property(QStringLiteral("content"));
        if (!roleValue.isString() || !contentValue.isString()) {
            apiKeyBytes.fill('\0');
            apiKey.fill(QChar(u'\0'));
            emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid message fields")
                                .arg(m_pluginId));
            return false;
        }

        const QString role = roleValue.toString();
        const QString content = contentValue.toString();
        if ((role != QStringLiteral("system")
             && role != QStringLiteral("user")
             && role != QStringLiteral("assistant"))
            || content.isEmpty() || content.size() > MaxOpenAIMessageBytes
            || content.contains(QChar(u'\0'))) {
            apiKeyBytes.fill('\0');
            apiKey.fill(QChar(u'\0'));
            emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: unsupported message role or size")
                                .arg(m_pluginId));
            return false;
        }

        const QByteArray encodedContent = content.toUtf8();
        messageBytes += encodedContent.size();
        if (messageBytes > MaxOpenAIRequestBytes) {
            apiKeyBytes.fill('\0');
            apiKey.fill(QChar(u'\0'));
            emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: messages too large")
                                .arg(m_pluginId));
            return false;
        }

        messages.append(QJsonObject{
            {QStringLiteral("role"), role},
            {QStringLiteral("content"), content}
        });
    }

    const QJSValue maxTokensValue = requestValue.property(QStringLiteral("max_tokens"));
    const int maxTokens = maxTokensValue.isUndefined() ? 512 : maxTokensValue.toInt(-1);
    if (maxTokens < 1 || maxTokens > 4096) {
        apiKeyBytes.fill('\0');
        apiKey.fill(QChar(u'\0'));
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: max_tokens out of range")
                            .arg(m_pluginId));
        return false;
    }

    const QJSValue temperatureValue = requestValue.property(QStringLiteral("temperature"));
    const double temperature = temperatureValue.isUndefined()
        ? 0.7 : temperatureValue.toNumber();
    if ((!temperatureValue.isUndefined() && !temperatureValue.isNumber())
        || !std::isfinite(temperature) || temperature < 0.0 || temperature > 2.0) {
        apiKeyBytes.fill('\0');
        apiKey.fill(QChar(u'\0'));
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid temperature")
                            .arg(m_pluginId));
        return false;
    }

    QJsonObject payload{
        {QStringLiteral("model"), model},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("max_tokens"), maxTokens},
        {QStringLiteral("temperature"), temperature}
    };

    const QJSValue reasoningValue =
        requestValue.property(QStringLiteral("reasoning_effort"));
    if (!reasoningValue.isUndefined()) {
        if (!reasoningValue.isString()) {
            apiKeyBytes.fill('\0');
            apiKey.fill(QChar(u'\0'));
            emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid reasoning_effort")
                                .arg(m_pluginId));
            return false;
        }
        const QString effort = reasoningValue.toString();
        if (effort != QStringLiteral("low")
            && effort != QStringLiteral("medium")
            && effort != QStringLiteral("high")) {
            apiKeyBytes.fill('\0');
            apiKey.fill(QChar(u'\0'));
            emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: unsupported reasoning_effort")
                                .arg(m_pluginId));
            return false;
        }
        payload.insert(QStringLiteral("reasoning_effort"), effort);
    }

    const QJSValue thinkingValue =
        requestValue.property(QStringLiteral("enable_thinking"));
    if (!thinkingValue.isUndefined()) {
        if (!thinkingValue.isBool()) {
            apiKeyBytes.fill('\0');
            apiKey.fill(QChar(u'\0'));
            emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: invalid enable_thinking")
                                .arg(m_pluginId));
            return false;
        }
        payload.insert(QStringLiteral("enable_thinking"), thinkingValue.toBool());
    }

    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (body.isEmpty() || body.size() > MaxOpenAIRequestBytes) {
        apiKeyBytes.fill('\0');
        apiKey.fill(QChar(u'\0'));
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request rejected: encoded request too large")
                            .arg(m_pluginId));
        return false;
    }

    QSslConfiguration sslConfiguration = QSslConfiguration::defaultConfiguration();
    sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    sslConfiguration.setProtocol(QSsl::TlsV1_2OrLater);

    QNetworkRequest networkRequest(endpoint);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                             QStringLiteral("application/json"));
    networkRequest.setRawHeader("Accept", "application/json");
    networkRequest.setRawHeader("User-Agent", "BLKT/1.0 OpenAI-compatible client");
    networkRequest.setRawHeader("Authorization",
                                QByteArrayLiteral("Bearer ") + apiKeyBytes);
    networkRequest.setSslConfiguration(sslConfiguration);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::ManualRedirectPolicy);

    apiKeyBytes.fill('\0');
    apiKey.fill(QChar(u'\0'));
    if (!m_aiNetwork)
        m_aiNetwork = new QNetworkAccessManager(this);

    QNetworkReply *reply = m_aiNetwork->post(networkRequest, body);
    if (!reply) {
        emit logMessage(QStringLiteral("Plugin %1 OpenAI request could not be created")
                            .arg(m_pluginId));
        return false;
    }

    m_pendingAiCallbacks.insert(reply, callback);
    reply->setReadBufferSize(MaxOpenAIResponseBytes + 1);

    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    timeout->setInterval(OpenAITimeoutMs);
    connect(timeout, &QTimer::timeout, reply, [reply] {
        reply->setProperty("blkt_openai_timeout", true);
        reply->abort();
    });
    connect(reply, &QIODevice::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > MaxOpenAIResponseBytes) {
            reply->setProperty("blkt_openai_too_large", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::sslErrors, reply,
            [reply](const QList<QSslError> &) {
        reply->setProperty("blkt_openai_tls_error", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, timeout, &QTimer::stop);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { finishOpenAIRequest(reply); });
    timeout->start();

    emit logMessage(QStringLiteral("Plugin %1 OpenAI HTTPS request queued").arg(m_pluginId));
    return true;
}

void ScriptPluginApi::finishOpenAIRequest(QNetworkReply *reply)
{
    if (!reply)
        return;

    const QJSValue callback = m_pendingAiCallbacks.take(reply);
    QJSValue result;
    if (!m_shuttingDown && !m_disabled && m_engine && callback.isCallable()) {
        result = m_engine->newObject();
        result.setProperty(QStringLiteral("ok"), false);

        const auto fail = [&result](const QString &message) {
            result.setProperty(QStringLiteral("error"), message);
        };

        if (reply->property("blkt_openai_timeout").toBool()) {
            fail(QStringLiteral("Request timed out"));
        } else if (reply->property("blkt_openai_tls_error").toBool()) {
            fail(QStringLiteral("TLS certificate verification failed"));
        } else if (reply->property("blkt_openai_too_large").toBool()) {
            fail(QStringLiteral("Response exceeded the 1 MiB limit"));
        } else if (reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
            fail(QStringLiteral("Redirects are disabled for credential safety"));
        } else {
            const int statusCode =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError) {
                fail(statusCode > 0
                    ? QStringLiteral("HTTPS request failed (HTTP %1)").arg(statusCode)
                    : QStringLiteral("HTTPS transport failed"));
            } else if (statusCode < 200 || statusCode >= 300) {
                fail(QStringLiteral("API returned HTTP %1").arg(statusCode));
            } else {
                const QByteArray responseBody = reply->read(MaxOpenAIResponseBytes + 1);
                if (responseBody.size() > MaxOpenAIResponseBytes) {
                    fail(QStringLiteral("Response exceeded the 1 MiB limit"));
                } else {
                    QJsonParseError parseError;
                    const QJsonDocument document =
                        QJsonDocument::fromJson(responseBody, &parseError);
                    if (parseError.error != QJsonParseError::NoError
                        || !document.isObject()) {
                        fail(QStringLiteral("API returned invalid JSON"));
                    } else {
                        const QJsonArray choices =
                            document.object().value(QStringLiteral("choices")).toArray();
                        const QJsonObject firstChoice =
                            choices.isEmpty() ? QJsonObject{} : choices.first().toObject();
                        const QJsonObject message =
                            firstChoice.value(QStringLiteral("message")).toObject();
                        const QJsonValue contentValue =
                            message.value(QStringLiteral("content"));
                        if (!contentValue.isString()) {
                            fail(QStringLiteral("API response has no text content"));
                        } else {
                            const QString content = contentValue.toString();
                            if (content.isEmpty() || content.size() > MaxOpenAIContentChars) {
                                fail(QStringLiteral("Model response is empty or too large"));
                            } else {
                                result.setProperty(QStringLiteral("ok"), true);
                                result.setProperty(QStringLiteral("content"), content);
                                result.setProperty(QStringLiteral("error"), QString());
                            }
                        }
                    }
                }
            }
        }

        const QJSValue callbackResult = callback.call({result});
        if (callbackResult.isError()) {
            const QString error = Guard::cleanLogText(callbackResult.toString(), 200);
            emit logMessage(QStringLiteral("Plugin %1 OpenAI callback error: %2")
                                .arg(m_pluginId, error));
        }
    }

    reply->deleteLater();
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
                                         const QStringList &openAIPluginIds,
                                         QObject *parent)
    : QObject(parent), m_core(core), m_directory(QDir(directory))
{
    for (const QString &id : allowedPluginIds)
        if (safePluginId(id))
            m_allowedIds.insert(id);
    for (const QString &id : sendMessagePluginIds)
        if (safePluginId(id) && m_allowedIds.contains(id))
            m_sendMessageIds.insert(id);
    for (const QString &id : openAIPluginIds)
        if (safePluginId(id) && m_allowedIds.contains(id))
            m_openAIIds.insert(id);

    if (m_core) {
        connect(m_core, &Core::messageReceived, this, &ScriptPluginManager::dispatchMessage);
        connect(m_core, &Core::giftReceived, this, &ScriptPluginManager::dispatchGift);
        connect(m_core, &Core::platformEventReceived,
                this, &ScriptPluginManager::dispatchPlatformEvent);
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
        | permissionFlag(Permission::GiftInfo)
        | permissionFlag(Permission::ReceiveInteraction)
        | permissionFlag(Permission::ReceiveEntry)
        | permissionFlag(Permission::ReceiveLike)
        | permissionFlag(Permission::ReceiveWatched)
        | permissionFlag(Permission::ReceiveNotice)
        | permissionFlag(Permission::ReceiveSuperChat)
        | permissionFlag(Permission::ReceiveGuard)
        | permissionFlag(Permission::ReceiveComboGift)
        | permissionFlag(Permission::ReceiveOnline)
        | permissionFlag(Permission::ReceiveRoomStats);
    if (m_sendMessageIds.contains(id))
        permissions |= permissionFlag(Permission::SendMessage);
    if (m_openAIIds.contains(id))
        permissions |= permissionFlag(Permission::OpenAIRequest);

    entry->api = new ScriptPluginApi(m_core, entry->engine, id, permissions, entry->engine);
    connect(entry->api, &ScriptPluginApi::logMessage, m_core, &Core::logMessage);
    entry->engine->globalObject().setProperty(
        QStringLiteral("BLKT"), entry->engine->newQObject(entry->api));

    const QJSValue apiTypes = entry->engine->evaluate(
        QStringLiteral("[typeof BLKT, typeof BLKT.on, typeof BLKT.sendMessage, typeof BLKT.log, typeof BLKT.openAIChat].join(',')"));
    emit m_core->logMessage(QStringLiteral("Plugin %1 JavaScript API types: %2")
                                .arg(id, Guard::cleanLogText(apiTypes.toString(), 128)));

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

void ScriptPluginManager::dispatchPlatformEvent(const PlatformEvent &event)
{
    QVariantMap payload = event.data;
    payload.insert(QStringLiteral("roomId"), event.roomId);
    payload.insert(QStringLiteral("type"), event.type);
    payload.insert(QStringLiteral("timestamp"), static_cast<qlonglong>(event.timestamp));

    QElapsedTimer eventTimer;
    eventTimer.start();
    int callbacksRemaining = MaxCallbacksPerEvent;
    for (Entry *entry : m_plugins) {
        if (eventTimer.elapsed() >= EventExecutionBudgetMs || callbacksRemaining <= 0)
            break;
        if (entry && entry->api
            && !entry->api->dispatch(event.type, payload,
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
