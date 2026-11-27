#include "BilibiliAdapter.hpp"
#include "../../core/Core.hpp"
#include "../../core/Guard.hpp"
#include "BilibiliProtocol.hpp"

#include <QAbstractSocket>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QStringList>
#include <QVariant>
#include <QUrl>
#include <QUrlQuery>

namespace blkt {
namespace {

constexpr quint16 PacketHeaderSize = 16;
constexpr quint32 MaxDecodedPacketBytes = 4u * 1024u * 1024u;
constexpr quint16 ProtocolVersionPlain = 1;
constexpr quint16 ProtocolVersionZlib = 2;
constexpr quint32 OperationHeartbeat = 2;
constexpr quint32 OperationHeartbeatReply = 3;
constexpr quint32 OperationMessage = 5;
constexpr quint32 OperationAuth = 7;
constexpr quint32 OperationAuthReply = 8;

void appendU16(QByteArray &buffer, quint16 value)
{
    buffer.append(static_cast<char>((value >> 8) & 0xff));
    buffer.append(static_cast<char>(value & 0xff));
}

void appendU32(QByteArray &buffer, quint32 value)
{
    buffer.append(static_cast<char>((value >> 24) & 0xff));
    buffer.append(static_cast<char>((value >> 16) & 0xff));
    buffer.append(static_cast<char>((value >> 8) & 0xff));
    buffer.append(static_cast<char>(value & 0xff));
}

quint16 readU16(const QByteArray &buffer, qsizetype offset)
{
    const auto *bytes = reinterpret_cast<const uchar *>(buffer.constData() + offset);
    return static_cast<quint16>((static_cast<quint16>(bytes[0]) << 8)
                                | static_cast<quint16>(bytes[1]));
}

quint32 readU32(const QByteArray &buffer, qsizetype offset)
{
    const auto *bytes = reinterpret_cast<const uchar *>(buffer.constData() + offset);
    return (static_cast<quint32>(bytes[0]) << 24)
           | (static_cast<quint32>(bytes[1]) << 16)
           | (static_cast<quint32>(bytes[2]) << 8)
           | static_cast<quint32>(bytes[3]);
}

QByteArray makePacket(quint32 operation, quint16 version, const QByteArray &payload)
{
    QByteArray packet;
    packet.reserve(PacketHeaderSize + payload.size());
    appendU32(packet, static_cast<quint32>(PacketHeaderSize + payload.size()));
    appendU16(packet, PacketHeaderSize);
    appendU16(packet, version);
    appendU32(packet, operation);
    appendU32(packet, 1);
    packet.append(payload);
    return packet;
}

QByteArray decompressZlib(const QByteArray &compressed)
{
    // Bilibili's zlib payload omits Qt's qCompress length prefix. Try bounded
    // output capacities and let qUncompress validate the zlib stream.
    for (quint32 capacity = 64 * 1024;
         capacity <= MaxDecodedPacketBytes;
         capacity *= 2) {
        QByteArray framed(4, '\0');
        framed[0] = static_cast<char>((capacity >> 24) & 0xff);
        framed[1] = static_cast<char>((capacity >> 16) & 0xff);
        framed[2] = static_cast<char>((capacity >> 8) & 0xff);
        framed[3] = static_cast<char>(capacity & 0xff);
        framed.append(compressed);
        const QByteArray result = qUncompress(framed);
        if (!result.isEmpty())
            return result;
    }
    return {};
}

quint64 jsonUnsigned(const QJsonValue &value, bool *ok = nullptr)
{
    quint64 result = 0;
    const bool valid = BilibiliProtocol::toUnsigned(value, &result);
    if (ok)
        *ok = valid;
    return valid ? result : 0;
}

} // namespace

BilibiliAdapter::BilibiliAdapter(Core *core, QObject *parent)
    : QObject(parent), m_core(core)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(RequestTimeoutMs);
    m_heartbeat.setInterval(HeartbeatIntervalMs);
    m_authTimeout.setSingleShot(true);
    m_authTimeout.setInterval(RequestTimeoutMs);
    m_reconnectTimer.setSingleShot(true);
    m_giftTimeout.setSingleShot(true);
    m_giftTimeout.setInterval(RequestTimeoutMs);
    m_roomDetailsTimeout.setSingleShot(true);
    m_roomDetailsTimeout.setInterval(RequestTimeoutMs);
    m_viewerHeartbeatTimer.setSingleShot(true);
    m_viewerEntryRetryTimer.setSingleShot(true);
    m_viewerEntryRetryTimer.setInterval(5000);

    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_reply && !m_reply->isFinished())
            m_reply->abort();
        if (m_roomInfoReply && !m_roomInfoReply->isFinished())
            m_roomInfoReply->abort();
    });
    connect(&m_reconnectTimer, &QTimer::timeout,
            this, [this] {
        if (m_roomResolved)
            requestDanmuInfo();
        else
            requestRoomInit();
    });
    connect(&m_heartbeat, &QTimer::timeout,
            this, &BilibiliAdapter::sendHeartbeat);
    connect(&m_roomDetailsTimeout, &QTimer::timeout, this, [this] {
        if (m_roomDetailsReply && !m_roomDetailsReply->isFinished())
            m_roomDetailsReply->abort();
    });
    connect(&m_viewerHeartbeatTimer, &QTimer::timeout,
            this, &BilibiliAdapter::sendViewerHeartbeat);
    connect(&m_viewerEntryRetryTimer, &QTimer::timeout,
            this, &BilibiliAdapter::reportRoomEntry);
    connect(&m_giftTimeout, &QTimer::timeout, this, [this] {
        if (m_giftCatalogReply && !m_giftCatalogReply->isFinished())
            m_giftCatalogReply->abort();
    });
    connect(&m_authTimeout, &QTimer::timeout, this, [this] {
        if (m_webSocket.state() != QAbstractSocket::UnconnectedState) {
            if (m_core)
                emit m_core->logMessage(QStringLiteral(
                    "Bilibili WebSocket handshake/authentication timed out; aborting connection"));
            m_webSocket.abort();
        }
    });

    connect(&m_webSocket, &QWebSocket::connected, this, [this] {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili WebSocket transport connected; sending room authentication"));
        sendAuthPacket();
    });
    connect(&m_webSocket, &QWebSocket::binaryMessageReceived,
            this, &BilibiliAdapter::handleBinaryMessage);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
        Q_UNUSED(message);
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili WebSocket received an unexpected text frame"));
    });
    connect(&m_webSocket, &QWebSocket::disconnected, this, [this] {
        m_heartbeat.stop();
        m_authTimeout.stop();
        const bool wasRoomAuthenticated = m_roomConnected;
        m_roomConnected = false;
        if (m_core && !m_roomId.isEmpty())
            m_core->setRoomConnection(m_roomId, false);

        if (m_core) {
            const QString closeReason = Guard::cleanLogText(m_webSocket.closeReason(), 160);
            const int closeCode = static_cast<int>(m_webSocket.closeCode());
            QString detail = QStringLiteral(
                "Bilibili WebSocket disconnected: room %1, close code %2, room-authenticated %3")
                .arg(m_roomId.isEmpty() ? QStringLiteral("unknown") : m_roomId)
                .arg(closeCode)
                .arg(wasRoomAuthenticated ? QStringLiteral("yes") : QStringLiteral("no"));
            if (!closeReason.isEmpty())
                detail += QStringLiteral(", reason '%1'").arg(closeReason);
            if (m_webSocket.error() != QAbstractSocket::UnknownSocketError) {
                const QString errorText = Guard::cleanLogText(m_webSocket.errorString(), 160);
                if (!errorText.isEmpty())
                    detail += QStringLiteral(", socket error '%1'").arg(errorText);
            }
            emit m_core->logMessage(detail);
        }

        if (m_roomRequested) {
            if (m_core)
                emit m_core->logMessage(QStringLiteral("Bilibili live connection disconnected; reconnect scheduled"));
            scheduleReconnect();
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(&m_webSocket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError error) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Bilibili WebSocket error %1: %2")
                .arg(static_cast<int>(error)).arg(Guard::cleanLogText(m_webSocket.errorString(), 160)));
    });
#else
    connect(&m_webSocket,
            qOverload<QAbstractSocket::SocketError>(&QWebSocket::error),
            this, [this](QAbstractSocket::SocketError error) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Bilibili WebSocket error %1: %2")
                .arg(static_cast<int>(error)).arg(Guard::cleanLogText(m_webSocket.errorString(), 160)));
    });
#endif
}

BilibiliAdapter::~BilibiliAdapter()
{
    disconnectRoom();
    m_timeout.stop();
    m_giftTimeout.stop();
    m_roomDetailsTimeout.stop();
    QPointer<QNetworkReply> authReply = m_reply;
    m_reply.clear();
    if (authReply) {
        disconnect(authReply, nullptr, this, nullptr);
        if (!authReply->isFinished())
            authReply->abort();
    }
    clearCredentials();
}

void BilibiliAdapter::armReplyTimeout(QNetworkReply *reply, const QString &requestName)
{
    if (!reply)
        return;

    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(RequestTimeoutMs);
    const QPointer<QNetworkReply> guardedReply(reply);
    connect(timer, &QTimer::timeout, this, [this, guardedReply, requestName] {
        if (!guardedReply || guardedReply->isFinished())
            return;
        if (m_core)
            emit m_core->logMessage(QStringLiteral("%1 request timed out").arg(requestName));
        guardedReply->abort();
    });
    connect(reply, &QNetworkReply::finished, timer, &QTimer::stop);
    timer->start();
}

bool BilibiliAdapter::authenticate(const QByteArray &cookie, const QString &csrf)
{
    if (!m_core || m_reply || m_authenticated
        || !Guard::validCookie(cookie) || !Guard::validCsrf(csrf)) {
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Invalid authentication state or credentials"));
        return false;
    }

    const QString cookieText = QString::fromLatin1(cookie);
    const QRegularExpression csrfPattern(
        QStringLiteral("(?:^|;\\s*)bili_jct=([^;\\s]+)"));
    const auto match = csrfPattern.match(cookieText);
    if (!match.hasMatch() || match.captured(1) != csrf) {
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("CSRF token does not match cookie"));
        return false;
    }

    m_cookie = cookie;
    m_csrf = csrf;
    const QRegularExpression buvidPattern(
        QStringLiteral("(?:^|;\\s*)buvid3=([^;\\s]+)"));
    const auto buvidMatch = buvidPattern.match(cookieText);
    m_buvid3 = buvidMatch.hasMatch() ? buvidMatch.captured(1) : QString();

    QNetworkRequest request(QUrl(QStringLiteral(
        "https://api.bilibili.com/x/web-interface/nav")));
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent",
                         "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    request.setRawHeader("Accept", "application/json, text/plain, */*");
    request.setRawHeader("Origin", "https://www.bilibili.com");
    request.setRawHeader("Referer", "https://www.bilibili.com/");

    m_reply = m_network.get(request);
    if (!m_reply) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Could not create authentication request"));
        return false;
    }

    m_reply->setReadBufferSize(MaxResponseBytes + 1);
    QNetworkReply *authReply = m_reply;
    connect(authReply, &QIODevice::readyRead, this, [authReply] {
        if (authReply->bytesAvailable() > MaxResponseBytes)
            authReply->abort();
    });
    connect(authReply, &QNetworkReply::finished, this, [this, authReply] {
        if (m_reply == authReply)
            finishAuthentication(authReply);
        else
            authReply->deleteLater();
    });
    m_timeout.start();
    return true;
}

void BilibiliAdapter::finishAuthentication(QNetworkReply *reply)
{
    if (!reply)
        return;

    if (m_reply != reply) {
        reply->deleteLater();
        return;
    }
    m_timeout.stop();
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = Guard::cleanLogText(reply->errorString(), 160);
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool responseTooLarge = body.size() > MaxResponseBytes;
    const bool transportOk = networkError == QNetworkReply::NoError
                             && !responseTooLarge
                             && (httpStatus == 0 || (httpStatus >= 200 && httpStatus < 300));
    reply->deleteLater();
    m_reply.clear();

    if (!transportOk) {
        clearCredentials();
        QString reason = QStringLiteral(
            "Bilibili authentication transport failed (network error %1, HTTP %2)")
            .arg(static_cast<int>(networkError))
            .arg(httpStatus > 0 ? QString::number(httpStatus) : QStringLiteral("unknown"));
        if (!networkErrorText.isEmpty())
            reason += QStringLiteral(": %1").arg(networkErrorText);
        if (responseTooLarge)
            reason += QStringLiteral(" (response exceeded size limit)");
        emit authenticationFinished(false, {}, 0, reason);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
            QStringLiteral("Bilibili authentication returned invalid JSON (HTTP %1)")
                .arg(httpStatus > 0 ? QString::number(httpStatus) : QStringLiteral("unknown")));
        return;
    }

    const QJsonObject root = document.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const int apiCode = root.value(QStringLiteral("code")).toInt(-1);
    if (apiCode != 0) {
        const QString apiMessage = Guard::cleanLogText(
            root.value(QStringLiteral("message")).toString(
                root.value(QStringLiteral("msg")).toString()), 160);
        QString reason = QStringLiteral("Bilibili rejected the login session (API code %1)")
                             .arg(apiCode);
        if (!apiMessage.isEmpty())
            reason += QStringLiteral(": %1").arg(apiMessage);
        clearCredentials();
        emit authenticationFinished(false, {}, 0, reason);
        return;
    }

    if (!data.value(QStringLiteral("isLogin")).toBool(false)) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
            QStringLiteral("Bilibili reports isLogin=false; refresh cookie.txt and export the current account session"));
        return;
    }

    bool idOk = false;
    const quint64 mid = jsonUnsigned(data.value(QStringLiteral("mid")), &idOk);
    const QString name = Guard::cleanText(
        data.value(QStringLiteral("uname")).toString(), 64);
    if (!idOk || mid == 0 || name.isEmpty()) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
            QStringLiteral("Bilibili login response is missing a valid account ID or name"));
        return;
    }

    if (!m_core || !m_core->login(m_cookie, m_csrf)) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
            QStringLiteral("Bilibili session was validated remotely but the local core rejected it"));
        return;
    }

    m_accountId = mid;
    m_accountName = name;
    m_wbiMixinKey = buildWbiMixinKey(data.value(QStringLiteral("wbi_img")).toObject());
    m_authenticated = true;

    if (m_wbiMixinKey.isEmpty() && m_core)
        emit m_core->logMessage(QStringLiteral(
            "Bilibili account login succeeded, but WBI credentials are missing; live-room connection is unavailable until refreshed"));
    emit authenticationFinished(true, m_accountName, m_accountId,
                                QStringLiteral("Account authenticated"));
}

bool BilibiliAdapter::connectRoom(const QString &roomId)
{
    if (!m_authenticated || m_roomRequested
        || m_webSocket.state() != QAbstractSocket::UnconnectedState
        || !Guard::validRoomId(roomId))
        return false;
    if (m_buvid3.isEmpty() || m_wbiMixinKey.isEmpty()) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Live room connection requires buvid3 and WBI credentials from cookie.txt"));
        return false;
    }

    m_reconnectTimer.stop();
    m_reconnectAttempt = 0;
    m_roomId = roomId;
    m_roomRequested = true;
    m_roomResolved = false;
    m_core->setRoomConnection(m_roomId, false);
    requestRoomInit();
    return true;
}

void BilibiliAdapter::requestRoomInit()
{
    if (!m_roomRequested || m_roomId.isEmpty())
        return;

    QPointer<QNetworkReply> previousReply = m_roomInfoReply;
    m_roomInfoReply.clear();
    if (previousReply && !previousReply->isFinished())
        previousReply->abort();

    QUrl url(QStringLiteral("https://api.live.bilibili.com/room/v1/Room/room_init"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), m_roomId);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent", "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    request.setRawHeader("Referer", "https://live.bilibili.com/");

    m_roomInfoReply = m_network.get(request);
    if (!m_roomInfoReply) {
        emit m_core->logMessage(QStringLiteral("Room ID resolution request could not be created"));
        scheduleReconnect();
        return;
    }

    QNetworkReply *reply = m_roomInfoReply;
    reply->setReadBufferSize(MaxResponseBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > MaxResponseBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_roomInfoReply == reply)
            handleRoomInit(reply);
        else
            reply->deleteLater();
    });
    m_timeout.start();
}

void BilibiliAdapter::handleRoomInit(QNetworkReply *reply)
{
    if (!reply)
        return;
    if (m_roomInfoReply != reply) {
        reply->deleteLater();
        return;
    }

    m_timeout.stop();
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool transportOk = reply->error() == QNetworkReply::NoError
                             && body.size() <= MaxResponseBytes;
    reply->deleteLater();
    m_roomInfoReply.clear();

    if (!transportOk) {
        emit m_core->logMessage(QStringLiteral("Room ID resolution failed"));
        scheduleReconnect();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    bool roomIdOk = false;
    const quint64 realRoomId =
        jsonUnsigned(data.value(QStringLiteral("room_id")), &roomIdOk);

    if (parseError.error != QJsonParseError::NoError
        || root.value(QStringLiteral("code")).toInt(-1) != 0
        || !roomIdOk || realRoomId == 0
        || !Guard::validRoomId(QString::number(realRoomId))) {
        emit m_core->logMessage(QStringLiteral("Room ID resolution response was invalid"));
        scheduleReconnect();
        return;
    }

    const QString resolvedRoomId = QString::number(realRoomId);
    if (m_roomId != resolvedRoomId)
        m_giftCatalogLoaded = false;
    m_roomId = resolvedRoomId;
    m_roomResolved = true;
    reportRoomEntry();
    // Move the core's room scope to the canonical room ID before the gift
    // catalog request can complete, so a later auth-success transition does
    // not clear a catalog that was just loaded.
    if (m_core)
        m_core->setRoomConnection(m_roomId, false);
    requestDanmuInfo();
}

QString BilibiliAdapter::buildWbiMixinKey(const QJsonObject &wbiImage) const
{
    const QString imageUrl = wbiImage.value(QStringLiteral("img_url")).toString();
    const QString subUrl = wbiImage.value(QStringLiteral("sub_url")).toString();
    const QString imageKey = QFileInfo(QUrl(imageUrl).path()).completeBaseName();
    const QString subKey = QFileInfo(QUrl(subUrl).path()).completeBaseName();
    if (imageKey.size() != 32 || subKey.size() != 32)
        return {};

    static constexpr int permutation[64] = {
        46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
        27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
        37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
        22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52
    };

    const QString raw = imageKey + subKey;
    QString mixed;
    mixed.reserve(32);
    for (int i = 0; i < 32; ++i)
        mixed.append(raw.at(permutation[i]));
    return mixed;
}

QMap<QString, QString> BilibiliAdapter::signWbiParameters(
    QMap<QString, QString> parameters) const
{
    if (m_wbiMixinKey.isEmpty())
        return {};

    parameters.insert(QStringLiteral("wts"),
                      QString::number(QDateTime::currentSecsSinceEpoch()));

    QStringList encoded;
    encoded.reserve(parameters.size());
    static const QRegularExpression forbidden(QStringLiteral("[!'()*]"));
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it) {
        QString value = it.value();
        value.remove(forbidden);
        const QByteArray key = QUrl::toPercentEncoding(it.key());
        const QByteArray encodedValue = QUrl::toPercentEncoding(value);
        encoded.append(QString::fromLatin1(key) + QLatin1Char('=')
                       + QString::fromLatin1(encodedValue));
    }

    const QByteArray source = encoded.join(QLatin1Char('&')).toUtf8()
                              + m_wbiMixinKey.toUtf8();
    parameters.insert(QStringLiteral("w_rid"),
                      QString::fromLatin1(
                          QCryptographicHash::hash(source, QCryptographicHash::Md5).toHex()));
    return parameters;
}

void BilibiliAdapter::requestDanmuInfo()
{
    if (!m_roomRequested || !m_roomResolved || m_roomId.isEmpty())
        return;

    QPointer<QNetworkReply> previousRoomReply = m_roomInfoReply;
    m_roomInfoReply.clear();
    if (previousRoomReply && !previousRoomReply->isFinished())
        previousRoomReply->abort();

    QUrl url(QStringLiteral("https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo"));
    QUrlQuery query;
    const QMap<QString, QString> parameters = signWbiParameters({
        {QStringLiteral("id"), m_roomId},
        {QStringLiteral("type"), QStringLiteral("0")},
        {QStringLiteral("web_location"), QStringLiteral("444.8")}
    });
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it)
        query.addQueryItem(it.key(), it.value());
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent", "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    request.setRawHeader("Referer", "https://live.bilibili.com/");

    m_roomInfoReply = m_network.get(request);
    if (!m_roomInfoReply) {
        emit m_core->logMessage(QStringLiteral("Could not request live room connection information"));
        scheduleReconnect();
        return;
    }

    QNetworkReply *roomReply = m_roomInfoReply;
    roomReply->setReadBufferSize(MaxResponseBytes + 1);
    connect(roomReply, &QIODevice::readyRead, this, [roomReply] {
        if (roomReply->bytesAvailable() > MaxResponseBytes)
            roomReply->abort();
    });
    connect(roomReply, &QNetworkReply::finished, this, [this, roomReply] {
        if (m_roomInfoReply == roomReply)
            handleDanmuInfo(roomReply);
        else
            roomReply->deleteLater();
    });
    m_timeout.start();
}

void BilibiliAdapter::handleDanmuInfo(QNetworkReply *reply)
{
    if (!reply)
        return;
    if (m_roomInfoReply != reply) {
        reply->deleteLater();
        return;
    }

    m_timeout.stop();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = Guard::cleanLogText(reply->errorString(), 160);
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool transportOk = networkError == QNetworkReply::NoError
                             && body.size() <= MaxResponseBytes;
    reply->deleteLater();
    m_roomInfoReply.clear();

    if (!transportOk) {
        emit m_core->logMessage(QStringLiteral(
            "Live room information transport failure: HTTP %1, Qt error %2 (%3), bytes %4")
            .arg(httpStatus).arg(static_cast<int>(networkError))
            .arg(networkErrorText.left(160)).arg(body.size()));
        scheduleReconnect();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const QJsonArray hosts = data.value(QStringLiteral("host_list")).toArray();
    const QString token = data.value(QStringLiteral("token")).toString();
    const int apiCode = root.value(QStringLiteral("code")).toInt(-1);

    if (parseError.error != QJsonParseError::NoError || apiCode != 0
        || token.isEmpty() || token.size() > 4096 || hosts.isEmpty()) {
        const QString apiMessage = Guard::cleanLogText(root.value(QStringLiteral("message")).toString(
            root.value(QStringLiteral("msg")).toString(QStringLiteral("no API message"))), 160);
        emit m_core->logMessage(QStringLiteral(
            "Live room information rejected: HTTP %1, API code %2, message '%3', JSON error '%4', host count %5")
            .arg(httpStatus).arg(apiCode).arg(apiMessage.left(160))
            .arg(parseError.errorString().left(120)).arg(hosts.size()));
        scheduleReconnect();
        return;
    }

    QString host;
    int port = 0;
    for (const QJsonValue &value : hosts) {
        const QJsonObject item = value.toObject();
        const QString candidate = item.value(QStringLiteral("host")).toString();
        const int candidatePort = item.value(QStringLiteral("wss_port")).toInt();
        const QString normalizedHost = candidate.trimmed().toLower();
        if (Guard::validBilibiliWebSocketHost(normalizedHost)
            && candidatePort > 0 && candidatePort <= 65535) {
            host = normalizedHost;
            port = candidatePort;
            break;
        }
    }

    if (host.isEmpty()) {
        emit m_core->logMessage(QStringLiteral("No secure live WebSocket endpoint was returned"));
        scheduleReconnect();
        return;
    }

    m_danmuToken = token;
    requestGiftCatalog();
    QUrl socketUrl;
    socketUrl.setScheme(QStringLiteral("wss"));
    socketUrl.setHost(host);
    socketUrl.setPort(port);
    socketUrl.setPath(QStringLiteral("/sub"));

    QNetworkRequest socketRequest(socketUrl);
    // Match the authenticated browser session used to obtain the WBI danmu token.
    // Bilibili's live gateway applies additional risk checks to the WebSocket
    // handshake; omitting the session Cookie can cause an immediate close even
    // when the REST authentication and getDanmuInfo requests succeeded.
    socketRequest.setRawHeader("Origin", "https://live.bilibili.com");
    socketRequest.setRawHeader("Referer", "https://live.bilibili.com/");
    socketRequest.setRawHeader(
        "User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/131.0.0.0 Safari/537.36");
    socketRequest.setRawHeader("Cookie", m_cookie);
    socketRequest.setRawHeader("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
    socketRequest.setRawHeader("Cache-Control", "no-cache");
    socketRequest.setRawHeader("Pragma", "no-cache");
    if (m_core)
        emit m_core->logMessage(QStringLiteral(
            "Opening authenticated Bilibili WebSocket: host %1, port %2")
            .arg(host).arg(port));
    m_authTimeout.start();
    m_webSocket.open(socketRequest);
}

void BilibiliAdapter::reportRoomEntry()
{
    if (!m_roomRequested || !m_roomResolved || m_roomId.isEmpty()
        || m_viewerEntryReported || m_viewerEntryReply) {
        return;
    }

    const QUrl url(QStringLiteral(
        "https://api.live.bilibili.com/xlive/web-room/v1/index/roomEntryAction"));
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("room_id"), m_roomId);
    form.addQueryItem(QStringLiteral("platform"), QStringLiteral("pc"));
    form.addQueryItem(QStringLiteral("csrf"), m_csrf);
    form.addQueryItem(QStringLiteral("csrf_token"), m_csrf);

    QNetworkRequest request(url);
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent", "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    request.setRawHeader("Referer", (QStringLiteral("https://live.bilibili.com/") + m_roomId).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));

    m_viewerEntryReply = m_network.post(request, form.query(QUrl::FullyEncoded).toUtf8());
    if (!m_viewerEntryReply) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili viewer entry report could not be created"));
        m_viewerEntryRetryTimer.start();
        return;
    }

    armReplyTimeout(m_viewerEntryReply, QStringLiteral("Bilibili viewer entry"));
    QNetworkReply *reply = m_viewerEntryReply;
    reply->setReadBufferSize(MaxResponseBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > MaxResponseBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_viewerEntryReply == reply)
            handleRoomEntry(reply);
        else
            reply->deleteLater();
    });
    if (m_core)
        emit m_core->logMessage(QStringLiteral(
            "Reporting authenticated viewer entry for room %1").arg(m_roomId));
}

void BilibiliAdapter::handleRoomEntry(QNetworkReply *reply)
{
    if (!reply)
        return;
    if (m_viewerEntryReply != reply) {
        reply->deleteLater();
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool transportOk = networkError == QNetworkReply::NoError
                             && body.size() <= MaxResponseBytes;
    const QString errorText = Guard::cleanLogText(reply->errorString(), 120);
    reply->deleteLater();
    m_viewerEntryReply.clear();

    if (!m_roomRequested || !m_roomResolved)
        return;

    if (!transportOk) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili viewer entry report transport failure: HTTP %1, error %2 (%3)")
                .arg(httpStatus).arg(static_cast<int>(networkError)).arg(errorText));
        m_viewerEntryRetryTimer.start();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const int apiCode = root.value(QStringLiteral("code")).toInt(-1);
    const QString apiMessage = Guard::cleanLogText(
        root.value(QStringLiteral("message")).toString(
            root.value(QStringLiteral("msg")).toString(QStringLiteral("no API message"))), 120);
    if (parseError.error != QJsonParseError::NoError || apiCode != 0) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili viewer entry report rejected: HTTP %1, API code %2, message '%3'")
                .arg(httpStatus).arg(apiCode).arg(apiMessage));
        m_viewerEntryRetryTimer.start();
        return;
    }

    m_viewerEntryRetryTimer.stop();
    m_viewerEntryReported = true;
    if (m_core)
        emit m_core->logMessage(QStringLiteral(
            "Bilibili authenticated viewer entry report accepted for room %1").arg(m_roomId));
    sendViewerHeartbeat();
}

void BilibiliAdapter::sendViewerHeartbeat()
{
    if (!m_roomRequested || !m_viewerEntryReported || m_roomId.isEmpty()
        || m_viewerHeartbeatReply) {
        return;
    }

    const QString heartbeat = QStringLiteral("%1|%2|1|0")
                                  .arg(m_viewerHeartbeatIntervalSeconds)
                                  .arg(m_roomId);
    const QByteArray encodedHeartbeat = heartbeat.toUtf8().toBase64();
    QUrl url(QStringLiteral(
        "https://live-trace.bilibili.com/xlive/rdata-interface/v1/heartbeat/webHeartBeat"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("hb"), QString::fromLatin1(encodedHeartbeat));
    query.addQueryItem(QStringLiteral("pf"), QStringLiteral("web"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent", "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    request.setRawHeader("Referer", (QStringLiteral("https://live.bilibili.com/") + m_roomId).toUtf8());

    m_viewerHeartbeatReply = m_network.get(request);
    if (!m_viewerHeartbeatReply) {
        m_viewerHeartbeatTimer.start(60000);
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili viewer heartbeat request could not be created"));
        return;
    }

    armReplyTimeout(m_viewerHeartbeatReply, QStringLiteral("Bilibili viewer heartbeat"));
    QNetworkReply *reply = m_viewerHeartbeatReply;
    reply->setReadBufferSize(MaxResponseBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > MaxResponseBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_viewerHeartbeatReply == reply)
            handleViewerHeartbeat(reply);
        else
            reply->deleteLater();
    });
}

void BilibiliAdapter::handleViewerHeartbeat(QNetworkReply *reply)
{
    if (!reply)
        return;
    if (m_viewerHeartbeatReply != reply) {
        reply->deleteLater();
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool transportOk = networkError == QNetworkReply::NoError
                             && body.size() <= MaxResponseBytes;
    const QString errorText = Guard::cleanLogText(reply->errorString(), 120);
    reply->deleteLater();
    m_viewerHeartbeatReply.clear();

    if (!m_roomRequested)
        return;

    if (!transportOk) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili viewer heartbeat transport failure: HTTP %1, error %2 (%3)")
                .arg(httpStatus).arg(static_cast<int>(networkError)).arg(errorText));
        m_viewerHeartbeatTimer.start(60000);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const int apiCode = root.value(QStringLiteral("code")).toInt(-1);
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const int nextInterval = data.value(QStringLiteral("next_interval")).toInt(60);
    if (parseError.error != QJsonParseError::NoError || apiCode != 0) {
        const QString apiMessage = Guard::cleanLogText(
            root.value(QStringLiteral("message")).toString(
                root.value(QStringLiteral("msg")).toString(QStringLiteral("no API message"))), 120);
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili viewer heartbeat rejected: HTTP %1, API code %2, message '%3'")
                .arg(httpStatus).arg(apiCode).arg(apiMessage));
        m_viewerHeartbeatTimer.start(60000);
        return;
    }

    m_viewerHeartbeatIntervalSeconds = qBound(30, nextInterval, 300);
    m_viewerHeartbeatTimer.start(m_viewerHeartbeatIntervalSeconds * 1000);
    if (m_core)
        emit m_core->logMessage(QStringLiteral(
            "Bilibili viewer heartbeat accepted; next report in %1 seconds")
            .arg(m_viewerHeartbeatIntervalSeconds));
}

void BilibiliAdapter::requestGiftCatalog()
{
    if (!m_roomRequested || m_roomId.isEmpty() || m_giftCatalogLoaded)
        return;

    QPointer<QNetworkReply> previousReply = m_roomDetailsReply;
    m_roomDetailsReply.clear();
    if (previousReply && !previousReply->isFinished())
        previousReply->abort();

    QUrl url(QStringLiteral("https://api.live.bilibili.com/room/v1/Room/get_info"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("room_id"), m_roomId);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent", "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    request.setRawHeader("Referer", "https://live.bilibili.com/");

    m_roomDetailsReply = m_network.get(request);
    if (!m_roomDetailsReply) {
        fetchGiftCatalog(0, 0);
        return;
    }

    QNetworkReply *reply = m_roomDetailsReply;
    reply->setReadBufferSize(MaxResponseBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > MaxResponseBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_roomDetailsReply == reply)
            handleRoomDetails(reply);
        else
            reply->deleteLater();
    });
    m_roomDetailsTimeout.start();
}

void BilibiliAdapter::handleRoomDetails(QNetworkReply *reply)
{
    if (!reply)
        return;
    if (m_roomDetailsReply != reply) {
        reply->deleteLater();
        return;
    }

    m_roomDetailsTimeout.stop();
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool transportOk = reply->error() == QNetworkReply::NoError
                             && body.size() <= MaxResponseBytes;
    reply->deleteLater();
    m_roomDetailsReply.clear();

    if (!m_roomRequested)
        return;

    int areaId = 0;
    int parentAreaId = 0;
    if (transportOk) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        const QJsonObject root = document.object();
        const QJsonObject data = root.value(QStringLiteral("data")).toObject();
        if (parseError.error == QJsonParseError::NoError
            && root.value(QStringLiteral("code")).toInt(-1) == 0) {
            areaId = qBound(0, data.value(QStringLiteral("area_id")).toInt(), 1000000);
            parentAreaId = qBound(
                0, data.value(QStringLiteral("parent_area_id")).toInt(), 1000000);
        }
    }

    fetchGiftCatalog(areaId, parentAreaId);
}

void BilibiliAdapter::fetchGiftCatalog(int areaId, int parentAreaId)
{
    if (!m_core || m_roomId.isEmpty())
        return;
    QPointer<QNetworkReply> previousGiftReply = m_giftCatalogReply;
    m_giftCatalogReply.clear();
    if (previousGiftReply && !previousGiftReply->isFinished())
        previousGiftReply->abort();

    QUrl url(QStringLiteral(
        "https://api.live.bilibili.com/xlive/web-room/v1/giftPanel/giftConfig"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("platform"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("room_id"), m_roomId);
    if (areaId > 0)
        query.addQueryItem(QStringLiteral("area_id"), QString::number(areaId));
    if (parentAreaId > 0)
        query.addQueryItem(QStringLiteral("area_parent_id"), QString::number(parentAreaId));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Cookie", m_cookie);
    request.setRawHeader("User-Agent", "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    request.setRawHeader("Referer", "https://live.bilibili.com/");

    m_giftCatalogReply = m_network.get(request);
    if (!m_giftCatalogReply) {
        emit m_core->logMessage(QStringLiteral("Gift catalog request could not be created"));
        return;
    }

    QNetworkReply *giftReply = m_giftCatalogReply;
    giftReply->setReadBufferSize(MaxGiftCatalogBytes + 1);
    connect(giftReply, &QIODevice::readyRead, this, [giftReply] {
        if (giftReply->bytesAvailable() > MaxGiftCatalogBytes)
            giftReply->abort();
    });
    connect(giftReply, &QNetworkReply::finished, this, [this, giftReply] {
        if (m_giftCatalogReply == giftReply)
            handleGiftCatalog(giftReply);
        else
            giftReply->deleteLater();
    });
    m_giftTimeout.start();
}

void BilibiliAdapter::handleGiftCatalog(QNetworkReply *reply)
{
    if (!reply)
        return;

    if (m_giftCatalogReply != reply) {
        reply->deleteLater();
        return;
    }
    m_giftTimeout.stop();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = Guard::cleanLogText(reply->errorString(), 160);
    const QByteArray body = reply->read(MaxGiftCatalogBytes + 1);
    const bool transportOk = networkError == QNetworkReply::NoError
                             && body.size() <= MaxGiftCatalogBytes;
    reply->deleteLater();
    m_giftCatalogReply.clear();

    if (!m_roomRequested || m_roomId.isEmpty())
        return;

    if (!transportOk) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Gift catalog transport failure: HTTP %1, Qt network error %2 (%3), bytes %4")
                .arg(httpStatus).arg(static_cast<int>(networkError))
                .arg(networkErrorText.left(160)).arg(body.size()));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    auto readList = [](const QJsonValue &value) -> QJsonArray {
        if (value.isArray())
            return value.toArray();
        if (value.isObject())
            return value.toObject().value(QStringLiteral("list")).toArray();
        return {};
    };

    QJsonArray gifts = readList(data.value(QStringLiteral("list")));
    if (gifts.isEmpty()) {
        const QJsonObject giftConfig = data.value(QStringLiteral("gift_config")).toObject();
        gifts = readList(giftConfig.value(QStringLiteral("base_config")));
    }
    if (gifts.isEmpty()) {
        const QJsonObject giftData = data.value(QStringLiteral("gift_data")).toObject();
        gifts = readList(giftData.value(QStringLiteral("room_gift_list")));
        if (gifts.isEmpty())
            gifts = readList(giftData.value(QStringLiteral("gift_list")));
        if (gifts.isEmpty())
            gifts = readList(giftData.value(QStringLiteral("list")));
    }
    if (gifts.isEmpty())
        gifts = readList(data.value(QStringLiteral("gift_list")));
    if (gifts.isEmpty())
        gifts = readList(data.value(QStringLiteral("gifts")));

    const int apiCode = root.value(QStringLiteral("code")).toInt(-1);
    if (parseError.error != QJsonParseError::NoError || apiCode != 0) {
        QStringList dataKeys = data.keys();
        if (dataKeys.size() > 16)
            dataKeys = dataKeys.mid(0, 16);
        const QString apiMessage = Guard::cleanLogText(root.value(QStringLiteral("message")).toString(
            root.value(QStringLiteral("msg")).toString(QStringLiteral("no API message"))), 160);
        if (m_core) {
            emit m_core->logMessage(QStringLiteral(
                "Gift catalog request failed: HTTP %1, API code %2, message '%3', JSON error '%4', data keys [%5], body bytes %6")
                .arg(httpStatus).arg(apiCode).arg(apiMessage.left(160))
                .arg(parseError.errorString().left(120))
                .arg(Guard::cleanLogText(dataKeys.join(QStringLiteral(", ")), 256)).arg(body.size()));
        }
        return;
    }

    if (gifts.isEmpty()) {
        const QJsonObject giftData = data.value(QStringLiteral("gift_data")).toObject();
        const QString roomGiftState = giftData.value(QStringLiteral("room_gift_list")).isNull()
            ? QStringLiteral("null")
            : QStringLiteral("present but empty or unsupported");
        if (m_core) {
            emit m_core->logMessage(QStringLiteral(
                "Gift catalog unavailable: Bilibili returned no gift list (gift_config=%1, gift_data.room_gift_list=%2); keeping current in-memory catalog")
                .arg(data.value(QStringLiteral("gift_config")).isNull()
                         ? QStringLiteral("null") : QStringLiteral("present"),
                     roomGiftState));
        }
        return;
    }

    QList<GiftInfo> catalog;
    catalog.reserve(qMin<qsizetype>(gifts.size(), 10000));
    for (const QJsonValue &value : gifts) {
        if (catalog.size() >= 10000)
            break;
        const QJsonObject item = value.toObject();
        bool idOk = false;
        const quint64 id = jsonUnsigned(item.value(QStringLiteral("id")), &idOk);
        bool priceOk = false;
        const quint64 price = jsonUnsigned(item.value(QStringLiteral("price")), &priceOk);
        const QString name = Guard::cleanText(
            item.value(QStringLiteral("name")).toString(), 128);
        if (!idOk || id == 0 || name.isEmpty())
            continue;

        GiftInfo gift;
        gift.id = id;
        gift.name = name;
        gift.price = priceOk ? price : 0;
        gift.coinType = Guard::cleanText(
            item.value(QStringLiteral("coin_type")).toString(), 16);
        catalog.append(gift);
    }

    if (catalog.isEmpty()) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Gift catalog contained no valid gift entries; keeping current in-memory catalog"));
        return;
    }

    if (m_core) {
        m_core->replaceGiftCatalog(catalog);
        m_giftCatalogLoaded = true;
        emit m_core->logMessage(
            QStringLiteral("Gift catalog loaded into memory: %1 items").arg(catalog.size()));
    }
}

void BilibiliAdapter::sendAuthPacket()
{
    if (!m_roomRequested || !m_authenticated)
        return;

    if (m_danmuToken.isEmpty() || m_accountId == 0 || m_buvid3.isEmpty()) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Cannot authenticate Bilibili live WebSocket: required session fields are missing"));
        m_webSocket.abort();
        return;
    }

    QJsonObject auth;
    // Bilibili's current web live gateway accepts the danmu token in
    // guest-mode WebSocket sessions (uid=0). Supplying the account MID here
    // caused the gateway to close the socket before operation 8 was returned.
    // Keep the authenticated Cookie for REST requests, but do not claim a
    // viewer-entry session that this WebSocket-only client has not established.
    auth.insert(QStringLiteral("uid"), 0);
    auth.insert(QStringLiteral("roomid"), static_cast<double>(m_roomId.toULongLong()));
    // Protocol v2 keeps the decoder dependency-free (zlib via Qt). Protocol v3
    // requires Brotli, which is deliberately not introduced as a new runtime DLL.
    auth.insert(QStringLiteral("protover"), 2);
    auth.insert(QStringLiteral("buvid"), m_buvid3);
    auth.insert(QStringLiteral("support_ack"), true);
    const quint32 queueId = QRandomGenerator::global()->generate();
    auth.insert(QStringLiteral("queue_uuid"),
                QString::number(queueId, 16).rightJustified(8, QLatin1Char('0')));
    auth.insert(QStringLiteral("scene"), QString());
    auth.insert(QStringLiteral("platform"), QStringLiteral("web"));
    auth.insert(QStringLiteral("type"), 2);
    auth.insert(QStringLiteral("key"), m_danmuToken);

    const QByteArray packet = makePacket(
        OperationAuth, ProtocolVersionPlain,
        QJsonDocument(auth).toJson(QJsonDocument::Compact));
    const qint64 queuedBytes = m_webSocket.sendBinaryMessage(packet);
    if (m_core)
        emit m_core->logMessage(QStringLiteral(
            "Bilibili WebSocket auth packet queued: %1/%2 bytes")
            .arg(queuedBytes).arg(packet.size()));
    if (queuedBytes != packet.size()) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili WebSocket auth packet could not be queued completely"));
        m_webSocket.abort();
        return;
    }
    m_authTimeout.start();
}

void BilibiliAdapter::sendHeartbeat()
{
    if (m_roomConnected && m_webSocket.state() == QAbstractSocket::ConnectedState)
        m_webSocket.sendBinaryMessage(makePacket(OperationHeartbeat,
                                                 ProtocolVersionPlain, {}));
}

void BilibiliAdapter::handleBinaryMessage(const QByteArray &message)
{
    if (message.size() > MaxWebSocketPacketBytes) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili WebSocket incoming frame rejected: %1 bytes exceeds limit")
                .arg(message.size()));
        m_webSocket.abort();
        return;
    }
    if (message.size() >= PacketHeaderSize && m_core) {
        emit m_core->logMessage(QStringLiteral(
            "Bilibili WebSocket received frame: %1 bytes, protocol %2, operation %3, header %4, packet %5")
            .arg(message.size())
            .arg(readU16(message, 6))
            .arg(readU32(message, 8))
            .arg(readU16(message, 4))
            .arg(readU32(message, 0)));
    } else if (m_core) {
        emit m_core->logMessage(QStringLiteral(
            "Bilibili WebSocket received undersized binary frame: %1 bytes")
            .arg(message.size()));
    }
    handlePacketData(message, 0);
}

void BilibiliAdapter::handlePacketData(const QByteArray &data, int depth)
{
    if (depth > 3 || data.size() > MaxWebSocketPacketBytes) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral(
                "Bilibili packet rejected: nesting depth or decoded size limit exceeded"));
        m_webSocket.abort();
        return;
    }

    // Some gateway deployments deliver a plain JSON body after decompression,
    // while others wrap one or more JSON bodies in protocol packets. Accept
    // both representations without weakening packet-size or nesting limits.
    const QByteArray trimmed = data.trimmed();
    if (!trimmed.isEmpty() && (trimmed.startsWith('{') || trimmed.startsWith('['))) {
        QJsonParseError jsonError;
        const QJsonDocument json = QJsonDocument::fromJson(trimmed, &jsonError);
        if (jsonError.error != QJsonParseError::NoError) {
            if (m_core)
                emit m_core->logMessage(QStringLiteral(
                    "Bilibili message JSON decode failed: %1, bytes %2")
                    .arg(jsonError.errorString()).arg(trimmed.size()));
            return;
        }
        if (json.isObject()) {
            handleCommand(json.object());
        } else if (json.isArray()) {
            const QJsonArray commands = json.array();
            for (const QJsonValue &value : commands) {
                if (value.isObject())
                    handleCommand(value.toObject());
            }
        }
        return;
    }

    qsizetype offset = 0;
    while (offset + PacketHeaderSize <= data.size()) {
        const quint32 packetLength = readU32(data, offset);
        const quint16 headerLength = readU16(data, offset + 4);
        const quint16 version = readU16(data, offset + 6);
        const quint32 operation = readU32(data, offset + 8);

        if (packetLength < PacketHeaderSize || packetLength > MaxWebSocketPacketBytes
            || headerLength < PacketHeaderSize || headerLength > packetLength
            || offset + packetLength > data.size()) {
            m_webSocket.abort();
            return;
        }

        const QByteArray payload =
            data.mid(offset + headerLength, packetLength - headerLength);
        if (version == ProtocolVersionZlib) {
            const QByteArray expanded = decompressZlib(payload);
            if (expanded.isEmpty()) {
                if (m_core)
                    emit m_core->logMessage(QStringLiteral(
                        "Bilibili zlib payload decompression failed: compressed bytes %1, depth %2")
                        .arg(payload.size()).arg(depth));
                m_webSocket.abort();
                return;
            }
            if (m_core)
                emit m_core->logMessage(QStringLiteral(
                    "Bilibili zlib payload decoded: %1 bytes at depth %2")
                    .arg(expanded.size()).arg(depth));
            handlePacketData(expanded, depth + 1);
        } else if (version == ProtocolVersionPlain || version == 0) {
            if (operation == OperationAuthReply) {
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
                const QJsonObject authResult = document.object();
                const int authCode = authResult.value(QStringLiteral("code")).toInt(-1);
                const QString authMessage = authResult.value(QStringLiteral("message")).toString(
                    authResult.value(QStringLiteral("msg")).toString(QStringLiteral("no server message")));
                const bool accepted = error.error == QJsonParseError::NoError && authCode == 0;
                if (m_core)
                    emit m_core->logMessage(QStringLiteral(
                        "Bilibili WebSocket authentication reply: code %1, message '%2', JSON status %3")
                        .arg(authCode)
                        .arg(Guard::cleanLogText(authMessage, 160))
                        .arg(error.error == QJsonParseError::NoError
                                 ? QStringLiteral("valid") : error.errorString()));
                m_authTimeout.stop();
                if (accepted) {
                    m_roomConnected = true;
                    m_core->setRoomConnection(m_roomId, true);
                    m_reconnectAttempt = 0;
                    m_reconnectTimer.stop();
                    m_heartbeat.start();
                    emit m_core->logMessage(QStringLiteral("Connected to Bilibili live room"));
                } else {
                    m_roomRequested = false;
                    m_roomConnected = false;
                    m_core->setRoomConnection(m_roomId, false);
                    m_webSocket.close();
                    emit m_core->logMessage(QStringLiteral("Bilibili rejected live room authentication: code %1, message '%2'")
                        .arg(authCode).arg(authMessage.left(160)));
                }
            } else if (operation == OperationMessage) {
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
                if (error.error != QJsonParseError::NoError) {
                    if (m_core)
                        emit m_core->logMessage(QStringLiteral(
                            "Bilibili operation 5 JSON parse failed: %1, bytes %2")
                            .arg(error.errorString()).arg(payload.size()));
                } else if (document.isObject()) {
                    handleCommand(document.object());
                } else if (document.isArray()) {
                    const QJsonArray commands = document.array();
                    for (const QJsonValue &value : commands) {
                        if (value.isObject())
                            handleCommand(value.toObject());
                    }
                } else if (m_core) {
                    emit m_core->logMessage(QStringLiteral(
                        "Bilibili operation 5 contained unsupported JSON root, bytes %1")
                        .arg(payload.size()));
                }
            } else if (operation == OperationHeartbeatReply) {
                // Popularity payload is intentionally ignored by the core.
            }
        } else {
            // Brotli and unknown protocol versions are not negotiated by this client.
            m_webSocket.abort();
            return;
        }

        offset += packetLength;
    }
    if (offset != data.size())
        m_webSocket.abort();
}

void BilibiliAdapter::handleCommand(const QJsonObject &command)
{
    const QString cmd = command.value(QStringLiteral("cmd")).toString();
    if (m_core && !cmd.isEmpty() && m_commandDiagnosticCount < 16) {
        ++m_commandDiagnosticCount;
        emit m_core->logMessage(QStringLiteral("Bilibili live command decoded: %1").arg(
            Guard::cleanLogText(cmd, 96)));
    }

    if (cmd.startsWith(QStringLiteral("DANMU_MSG"))) {
        if (!m_core)
            return;

        QString user;
        QString text;
        if (!BilibiliProtocol::extractDanmaku(command, &user, &text)) {
            emit m_core->logMessage(QStringLiteral(
                "Bilibili DANMU_MSG ignored: unsupported or incomplete payload"));
            return;
        }

        MessageEvent event;
        event.roomId = m_roomId;
        event.user = user;
        event.text = text;
        event.timestamp = static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
        m_core->ingestMessage(event);
        return;
    }
    if (cmd == QStringLiteral("SEND_GIFT") && m_core) {
        GiftEvent event;
        if (BilibiliProtocol::extractGift(
                command, m_roomId,
                static_cast<quint64>(QDateTime::currentSecsSinceEpoch()), &event)) {
            m_core->ingestGift(event);
        }
        return;
    }

    if (m_core) {
        PlatformEvent event;
        if (BilibiliProtocol::extractPlatformEvent(
                command, m_roomId,
                static_cast<quint64>(QDateTime::currentSecsSinceEpoch()), &event)) {
            m_core->ingestPlatformEvent(event);
        }
    }
}

void BilibiliAdapter::scheduleReconnect()
{
    if (!m_roomRequested || !m_authenticated || m_reconnectTimer.isActive())
        return;
    const int exponent = qMin(m_reconnectAttempt, 5);
    const int baseDelay = qMin(1000 * (1 << exponent), 30000);
    const int jitter = QRandomGenerator::global()->bounded(1000);
    ++m_reconnectAttempt;
    m_reconnectTimer.start(baseDelay + jitter);
}

void BilibiliAdapter::disconnectRoom()
{
    m_roomRequested = false;
    m_reconnectTimer.stop();
    m_reconnectAttempt = 0;
    m_roomConnected = false;
    if (m_core)
        m_core->setRoomConnection({}, false);
    m_heartbeat.stop();
    m_viewerHeartbeatTimer.stop();
    m_viewerEntryRetryTimer.stop();
    m_viewerEntryReported = false;
    m_viewerHeartbeatIntervalSeconds = 60;
    m_authTimeout.stop();
    m_timeout.stop();
    m_giftTimeout.stop();
    m_roomDetailsTimeout.stop();

    QPointer<QNetworkReply> roomReply = m_roomInfoReply;
    QPointer<QNetworkReply> giftReply = m_giftCatalogReply;
    QPointer<QNetworkReply> detailsReply = m_roomDetailsReply;
    QPointer<QNetworkReply> viewerEntryReply = m_viewerEntryReply;
    QPointer<QNetworkReply> viewerHeartbeatReply = m_viewerHeartbeatReply;
    m_roomInfoReply.clear();
    m_giftCatalogReply.clear();
    m_roomDetailsReply.clear();
    m_viewerEntryReply.clear();
    m_viewerHeartbeatReply.clear();
    if (roomReply && !roomReply->isFinished())
        roomReply->abort();
    if (giftReply && !giftReply->isFinished())
        giftReply->abort();
    if (detailsReply && !detailsReply->isFinished())
        detailsReply->abort();
    if (viewerEntryReply && !viewerEntryReply->isFinished())
        viewerEntryReply->abort();
    if (viewerHeartbeatReply && !viewerHeartbeatReply->isFinished())
        viewerHeartbeatReply->abort();

    if (m_webSocket.state() != QAbstractSocket::UnconnectedState)
        m_webSocket.close();

    if (m_core)
        m_core->replaceGiftCatalog({});
    m_giftCatalogLoaded = false;
    m_roomId.clear();
    m_roomResolved = false;
    m_danmuToken.fill(QChar(u'\0'));
    m_danmuToken.clear();
}

void BilibiliAdapter::clearCredentials()
{
    if (!m_cookie.isEmpty())
        m_cookie.fill('\0');
    m_cookie.clear();
    m_csrf.fill(QChar(u'\0'));
    m_csrf.clear();
    m_buvid3.fill(QChar(u'\0'));
    m_buvid3.clear();
    m_wbiMixinKey.fill(QChar(u'\0'));
    m_wbiMixinKey.clear();
    m_giftCatalogLoaded = false;
}

bool BilibiliAdapter::isAuthenticated() const
{
    return m_authenticated;
}

bool BilibiliAdapter::isRoomConnected() const
{
    return m_roomConnected;
}

bool BilibiliAdapter::isRoomRequested() const
{
    return m_roomRequested;
}

QString BilibiliAdapter::accountName() const
{
    return m_accountName;
}

quint64 BilibiliAdapter::accountId() const
{
    return m_accountId;
}

} // namespace blkt
