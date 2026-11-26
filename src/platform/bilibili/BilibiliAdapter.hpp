#pragma once

#include <QObject>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QWebSocket>
#include <QPointer>
#include <QMap>
#include <QTimer>

class QNetworkReply;
class QJsonObject;

namespace blkt {

class Core;

class BilibiliAdapter final : public QObject
{
    Q_OBJECT

public:
    explicit BilibiliAdapter(Core *core, QObject *parent = nullptr);
    ~BilibiliAdapter() override;

    bool authenticate(const QByteArray &cookie, const QString &csrf);
    bool connectRoom(const QString &roomId);
    void disconnectRoom();
    bool isRoomConnected() const;
    bool isRoomRequested() const;
    bool isAuthenticated() const;
    QString accountName() const;
    quint64 accountId() const;

signals:
    void authenticationFinished(bool success, const QString &accountName,
                                quint64 accountId, const QString &reason);

private:
    void finishAuthentication(QNetworkReply *reply);
    void clearCredentials();
    void requestRoomInit();
    void handleRoomInit(QNetworkReply *reply);
    void requestDanmuInfo();
    QString buildWbiMixinKey(const QJsonObject &wbiImage) const;
    QMap<QString, QString> signWbiParameters(QMap<QString, QString> parameters) const;
    void scheduleReconnect();
    void handleDanmuInfo(QNetworkReply *reply);
    void requestGiftCatalog();
    void handleRoomDetails(QNetworkReply *reply);
    void fetchGiftCatalog(int areaId, int parentAreaId);
    void handleGiftCatalog(QNetworkReply *reply);
    void sendAuthPacket();
    void sendHeartbeat();
    void handleBinaryMessage(const QByteArray &message);
    void handlePacketData(const QByteArray &data, int depth);
    void handleCommand(const QJsonObject &command);

    Core *m_core = nullptr;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QPointer<QNetworkReply> m_roomInfoReply;
    QPointer<QNetworkReply> m_giftCatalogReply;
    QPointer<QNetworkReply> m_roomDetailsReply;
    QWebSocket m_webSocket;
    QTimer m_heartbeat;
    QTimer m_authTimeout;
    QTimer m_reconnectTimer;
    QTimer m_giftTimeout;
    QTimer m_roomDetailsTimeout;
    QTimer m_timeout;
    QByteArray m_cookie;
    QString m_csrf;
    QString m_accountName;
    quint64 m_accountId = 0;
    QString m_roomId;
    QString m_danmuToken;
    QString m_wbiMixinKey;
    QString m_buvid3;
    bool m_roomConnected = false;
    bool m_roomRequested = false;
    bool m_roomResolved = false;
    int m_reconnectAttempt = 0;
    bool m_authenticated = false;

    static constexpr qsizetype MaxResponseBytes = 64 * 1024;
    static constexpr qsizetype MaxGiftCatalogBytes = 512 * 1024;
    static constexpr int RequestTimeoutMs = 15000;
    static constexpr int HeartbeatIntervalMs = 30000;
    static constexpr int MaxWebSocketPacketBytes = 4 * 1024 * 1024;
};

} // namespace blkt
