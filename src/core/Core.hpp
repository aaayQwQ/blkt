#pragma once

#include "../plugin/IPlugin.hpp"

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QHash>
#include <QElapsedTimer>
#include <QTimer>
#include <QSet>
#include <optional>

namespace blkt {

class Core final : public QObject
{
    Q_OBJECT

public:
    explicit Core(QObject *parent = nullptr);
    ~Core() override;

    bool login(const QByteArray &cookie, const QString &csrf);
    void logout();
    bool isLoggedIn() const;
    void setRoomConnection(const QString &roomId, bool connected);

    bool sendMessage(const QString &roomId, const QString &text);
    std::optional<GiftInfo> giftInfo(quint64 giftId) const;

    // Called only by the trusted platform adapter after protocol validation.
    void ingestMessage(const MessageEvent &event);
    void ingestGift(const GiftEvent &event);
    void replaceGiftCatalog(const QList<GiftInfo> &items);

signals:
    void messageReceived(const blkt::MessageEvent &event);
    void giftReceived(const blkt::GiftEvent &event);
    void logMessage(const QString &message);
    void loginStateChanged(bool loggedIn);

private:
    void clearSession();
    void abortActiveRequests();
    void consumeReply(QNetworkReply *reply, quint64 generation);

    QNetworkAccessManager m_network;
    QSet<QNetworkReply *> m_activeReplies;
    QByteArray m_cookie;
    QString m_csrf;
    QHash<quint64, GiftInfo> m_gifts;
    QElapsedTimer m_clock;
    qint64 m_lastSendMs = -100000;
    quint64 m_sessionGeneration = 0;
    QString m_activeRoomId;
    bool m_roomConnected = false;
    bool m_loggedIn = false;
    static constexpr qint64 SendIntervalMs = 1000;
    static constexpr qsizetype MaxActiveRequests = 8;
    static constexpr qsizetype MaxGiftCatalogItems = 10000;
    static constexpr qsizetype MaxReplyBytes = 64 * 1024;
};

} // namespace blkt

Q_DECLARE_METATYPE(blkt::MessageEvent)
Q_DECLARE_METATYPE(blkt::GiftEvent)
