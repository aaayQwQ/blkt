#include "BilibiliAdapter.hpp"
#include "../../core/Core.hpp"
#include "../../core/Guard.hpp"

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
    if (value.isString())
        return value.toString().toULongLong(ok);
    return value.toVariant().toULongLong(ok);
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

    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_reply && !m_reply->isFinished())
            m_reply->abort();
        if (m_roomInfoReply && !m_roomInfoReply->isFinished())
            m_roomInfoReply->abort();
    });
    connect(&m_reconnectTimer, &QTimer::timeout,
            this, &BilibiliAdapter::requestDanmuInfo);
    connect(&m_heartbeat, &QTimer::timeout,
            this, &BilibiliAdapter::sendHeartbeat);
    connect(&m_giftTimeout, &QTimer::timeout, this, [this] {
        if (m_giftCatalogReply && !m_giftCatalogReply->isFinished())
            m_giftCatalogReply->abort();
    });
    connect(&m_authTimeout, &QTimer::timeout, this, [this] {
        if (m_webSocket.state() != QAbstractSocket::UnconnectedState)
            m_webSocket.abort();
    });

    connect(&m_webSocket, &QWebSocket::connected,
            this, &BilibiliAdapter::sendAuthPacket);
    connect(&m_webSocket, &QWebSocket::binaryMessageReceived,
            this, &BilibiliAdapter::handleBinaryMessage);
    connect(&m_webSocket, &QWebSocket::disconnected, this, [this] {
        m_heartbeat.stop();
        m_authTimeout.stop();
        m_roomConnected = false;
        if (m_roomRequested) {
            if (m_core)
                emit m_core->logMessage(QStringLiteral("Bilibili live connection disconnected; reconnect scheduled"));
            scheduleReconnect();
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(&m_webSocket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Bilibili live WebSocket error"));
    });
#else
    connect(&m_webSocket,
            qOverload<QAbstractSocket::SocketError>(&QWebSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Bilibili live WebSocket error"));
    });
#endif
}

BilibiliAdapter::~BilibiliAdapter()
{
    disconnectRoom();
    m_timeout.stop();
    m_giftTimeout.stop();
    if (m_reply && !m_reply->isFinished())
        m_reply->abort();
    clearCredentials();
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
    request.setRawHeader("Referer", "https://www.bilibili.com/");

    m_reply = m_network.get(request);
    if (!m_reply) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Could not create authentication request"));
        return false;
    }

    m_reply->setReadBufferSize(MaxResponseBytes);
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
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool responseTooLarge = body.size() > MaxResponseBytes;
    const bool transportOk = reply->error() == QNetworkReply::NoError
                             && !responseTooLarge;
    reply->deleteLater();
    m_reply.clear();

    if (!transportOk) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Bilibili authentication request failed"));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const bool apiOk = parseError.error == QJsonParseError::NoError
                       && root.value(QStringLiteral("code")).toInt(-1) == 0;
    const bool loggedIn = apiOk
                          && data.value(QStringLiteral("isLogin")).toBool(false);
    bool idOk = false;
    const quint64 mid = jsonUnsigned(data.value(QStringLiteral("mid")), &idOk);
    const QString name = Guard::cleanText(
        data.value(QStringLiteral("uname")).toString(), 64);

    if (!loggedIn || !idOk || mid == 0 || name.isEmpty()
        || !m_core || !m_core->login(m_cookie, m_csrf)) {
        clearCredentials();
        emit authenticationFinished(false, {}, 0,
                                    QStringLiteral("Cookie is invalid, expired, or account validation failed"));
        return;
    }

    m_accountId = mid;
    m_accountName = name;
    m_wbiMixinKey = buildWbiMixinKey(data.value(QStringLiteral("wbi_img")).toObject());
    m_authenticated = true;

    emit authenticationFinished(true, m_accountName, m_accountId,
                                QStringLiteral("Account authenticated"));
}

bool BilibiliAdapter::connectRoom(const QString &roomId)
{
    if (!m_authenticated || m_roomRequested || !Guard::validRoomId(roomId))
        return false;
    if (m_buvid3.isEmpty() || m_wbiMixinKey.isEmpty()) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Live room connection requires buvid3 and WBI credentials from cookie.txt"));
        return false;
    }

    m_roomId = roomId;
    m_roomRequested = true;
    requestDanmuInfo();
    return true;
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
    if (!m_roomRequested || m_roomId.isEmpty())
        return;

    if (m_roomInfoReply && !m_roomInfoReply->isFinished())
        m_roomInfoReply->abort();
    m_roomInfoReply.clear();

    QUrl url(QStringLiteral("https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo"));
    QUrlQuery query;
    const QMap<QString, QString> parameters = signWbiParameters({
        {QStringLiteral("id"), m_roomId},
        {QStringLiteral("type"), QStringLiteral("0")}
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
    roomReply->setReadBufferSize(MaxResponseBytes);
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
    const QByteArray body = reply->read(MaxResponseBytes + 1);
    const bool transportOk = reply->error() == QNetworkReply::NoError
                             && body.size() <= MaxResponseBytes;
    reply->deleteLater();
    m_roomInfoReply.clear();

    if (!transportOk) {
        emit m_core->logMessage(QStringLiteral("Live room information request failed"));
        scheduleReconnect();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const QJsonArray hosts = data.value(QStringLiteral("host_list")).toArray();
    const QString token = data.value(QStringLiteral("token")).toString();

    if (parseError.error != QJsonParseError::NoError
        || root.value(QStringLiteral("code")).toInt(-1) != 0
        || token.isEmpty() || token.size() > 4096 || hosts.isEmpty()) {
        emit m_core->logMessage(QStringLiteral("Live room information response was invalid"));
        scheduleReconnect();
        return;
    }

    QString host;
    int port = 0;
    for (const QJsonValue &value : hosts) {
        const QJsonObject item = value.toObject();
        const QString candidate = item.value(QStringLiteral("host")).toString();
        const int candidatePort = item.value(QStringLiteral("wss_port")).toInt();
        static const QRegularExpression hostPattern(
            QStringLiteral("^[A-Za-z0-9.-]{1,255}$"));
        if (hostPattern.match(candidate).hasMatch()
            && candidatePort > 0 && candidatePort <= 65535) {
            host = candidate;
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
    socketRequest.setRawHeader("Origin", "https://live.bilibili.com");
    socketRequest.setRawHeader("User-Agent", "Mozilla/5.0 BLKT/1.0 (Qt; C++17)");
    m_authTimeout.start();
    m_webSocket.open(socketRequest);
}

void BilibiliAdapter::requestGiftCatalog()
{
    if (!m_core || m_roomId.isEmpty())
        return;
    if (m_giftCatalogReply && !m_giftCatalogReply->isFinished())
        m_giftCatalogReply->abort();
    m_giftCatalogReply.clear();

    QUrl url(QStringLiteral(
        "https://api.live.bilibili.com/xlive/web-room/v1/giftPanel/roomGiftList"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("platform"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("room_id"), m_roomId);
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
    giftReply->setReadBufferSize(MaxGiftCatalogBytes);
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
    const QByteArray body = reply->read(MaxGiftCatalogBytes + 1);
    const bool transportOk = reply->error() == QNetworkReply::NoError
                             && body.size() <= MaxGiftCatalogBytes;
    reply->deleteLater();
    m_giftCatalogReply.clear();

    if (!transportOk) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Gift catalog request failed"));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject root = document.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    QJsonArray gifts = data.value(QStringLiteral("list")).toArray();

    if (gifts.isEmpty()) {
        const QJsonObject giftConfig =
            data.value(QStringLiteral("gift_config")).toObject();
        const QJsonObject baseConfig =
            giftConfig.value(QStringLiteral("base_config")).toObject();
        gifts = baseConfig.value(QStringLiteral("list")).toArray();
    }

    if (parseError.error != QJsonParseError::NoError
        || root.value(QStringLiteral("code")).toInt(-1) != 0
        || gifts.isEmpty()) {
        if (m_core)
            emit m_core->logMessage(QStringLiteral("Gift catalog response was invalid or empty"));
        return;
    }

    QList<GiftInfo> catalog;
    catalog.reserve(qMin(gifts.size(), 10000));
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

    if (m_core) {
        m_core->replaceGiftCatalog(catalog);
        emit m_core->logMessage(
            QStringLiteral("Gift catalog loaded into memory: %1 items").arg(catalog.size()));
    }
}

void BilibiliAdapter::sendAuthPacket()
{
    if (!m_roomRequested || !m_authenticated)
        return;

    QJsonObject auth;
    auth.insert(QStringLiteral("uid"), static_cast<double>(m_accountId));
    auth.insert(QStringLiteral("roomid"), static_cast<double>(m_roomId.toULongLong()));
    auth.insert(QStringLiteral("protover"), 2);
    auth.insert(QStringLiteral("buvid"), m_buvid3);
    auth.insert(QStringLiteral("platform"), QStringLiteral("web"));
    auth.insert(QStringLiteral("type"), 2);
    auth.insert(QStringLiteral("key"), m_danmuToken);

    const QByteArray packet = makePacket(
        OperationAuth, ProtocolVersionPlain,
        QJsonDocument(auth).toJson(QJsonDocument::Compact));
    m_webSocket.sendBinaryMessage(packet);
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
        m_webSocket.abort();
        return;
    }
    handlePacketData(message, 0);
}

void BilibiliAdapter::handlePacketData(const QByteArray &data, int depth)
{
    if (depth > 3 || data.size() > MaxWebSocketPacketBytes)
        return;

    qsizetype offset = 0;
    while (offset + PacketHeaderSize <= data.size()) {
        const quint32 packetLength = readU32(data, offset);
        const quint16 headerLength = readU16(data, offset + 4);
        const quint16 version = readU16(data, offset + 6);
        const quint32 operation = readU32(data, offset + 8);

        if (packetLength < PacketHeaderSize || packetLength > MaxWebSocketPacketBytes
            || headerLength < PacketHeaderSize || headerLength > packetLength
            || offset + packetLength > data.size()) {
            return;
        }

        const QByteArray payload =
            data.mid(offset + headerLength, packetLength - headerLength);
        if (version == ProtocolVersionZlib) {
            const QByteArray expanded = decompressZlib(payload);
            if (!expanded.isEmpty())
                handlePacketData(expanded, depth + 1);
        } else if (version == ProtocolVersionPlain || version == 0) {
            if (operation == OperationAuthReply) {
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
                const bool accepted = error.error == QJsonParseError::NoError
                    && document.object().value(QStringLiteral("code")).toInt(-1) == 0;
                m_authTimeout.stop();
                if (accepted) {
                    m_roomConnected = true;
                    m_reconnectAttempt = 0;
                    m_reconnectTimer.stop();
                    m_heartbeat.start();
                    emit m_core->logMessage(QStringLiteral("Connected to Bilibili live room"));
                } else {
                    m_roomRequested = false;
                    m_webSocket.close();
                    emit m_core->logMessage(QStringLiteral("Bilibili rejected live room authentication"));
                }
            } else if (operation == OperationMessage) {
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
                if (error.error == QJsonParseError::NoError && document.isObject())
                    handleCommand(document.object());
            } else if (operation == OperationHeartbeatReply) {
                // Popularity payload is intentionally ignored by the core.
            }
        } else {
            // Brotli and unknown protocol versions are not negotiated by this client.
            return;
        }

        offset += packetLength;
    }
}

void BilibiliAdapter::handleCommand(const QJsonObject &command)
{
    const QString cmd = command.value(QStringLiteral("cmd")).toString();
    const QJsonObject data = command.value(QStringLiteral("data")).toObject();

    if (cmd.startsWith(QStringLiteral("DANMU_MSG"))) {
        const QJsonArray info = command.value(QStringLiteral("info")).toArray();
        if (info.size() < 3 || !m_core)
            return;

        const QJsonArray userInfo = info.at(2).toArray();
        if (userInfo.size() < 2)
            return;

        MessageEvent event;
        event.user = userInfo.at(1).toString();
        event.text = info.at(1).toString();
        event.timestamp = QDateTime::currentSecsSinceEpoch();
        m_core->ingestMessage(event);
        return;
    }

    if (cmd == QStringLiteral("SEND_GIFT") && m_core) {
        GiftEvent event;
        event.user = data.value(QStringLiteral("uname")).toString();
        event.giftName = data.value(QStringLiteral("giftName")).toString();
        bool giftIdOk = false;
        const quint64 giftId = jsonUnsigned(data.value(QStringLiteral("giftId")), &giftIdOk);
        bool totalCoinOk = false;
        quint64 totalCoin = jsonUnsigned(data.value(QStringLiteral("total_coin")), &totalCoinOk);
        const int count = data.value(QStringLiteral("num")).toInt();
        if (!giftIdOk || giftId == 0 || count <= 0)
            return;

        event.giftId = giftId;
        event.count = static_cast<quint32>(count);
        event.totalCoin = totalCoinOk ? totalCoin : 0;
        event.coinType = data.value(QStringLiteral("coin_type")).toString();
        event.timestamp = QDateTime::currentSecsSinceEpoch();
        m_core->ingestGift(event);
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
    m_heartbeat.stop();
    m_authTimeout.stop();
    m_giftTimeout.stop();

    if (m_roomInfoReply && !m_roomInfoReply->isFinished())
        m_roomInfoReply->abort();
    if (m_giftCatalogReply && !m_giftCatalogReply->isFinished())
        m_giftCatalogReply->abort();
    m_roomInfoReply.clear();

    if (m_webSocket.state() != QAbstractSocket::UnconnectedState)
        m_webSocket.close();

    if (m_core)
        m_core->replaceGiftCatalog({});
    m_roomId.clear();
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
