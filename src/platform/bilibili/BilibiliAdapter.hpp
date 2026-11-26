#pragma once

#include <QObject>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

namespace blkt {

class Core;

class BilibiliAdapter final : public QObject
{
    Q_OBJECT

public:
    explicit BilibiliAdapter(Core *core, QObject *parent = nullptr);
    ~BilibiliAdapter() override;

    bool authenticate(const QByteArray &cookie, const QString &csrf);
    bool isAuthenticated() const;
    QString accountName() const;
    quint64 accountId() const;

signals:
    void authenticationFinished(bool success, const QString &accountName,
                                quint64 accountId, const QString &reason);

private:
    void finishAuthentication(QNetworkReply *reply);
    void clearCredentials();

    Core *m_core = nullptr;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timeout;
    QByteArray m_cookie;
    QString m_csrf;
    QString m_accountName;
    quint64 m_accountId = 0;
    bool m_authenticated = false;

    static constexpr qsizetype MaxResponseBytes = 64 * 1024;
    static constexpr int RequestTimeoutMs = 15000;
};

} // namespace blkt
