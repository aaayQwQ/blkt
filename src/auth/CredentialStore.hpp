#pragma once

#include <QByteArray>
#include <QString>

namespace blkt {

// Loads a pre-existing cookie export. This class never creates or modifies files.
// Credentials are retained only in process memory and cleared on request.
class CredentialStore final
{
public:
    CredentialStore() = default;
    ~CredentialStore();

    CredentialStore(const CredentialStore &) = delete;
    CredentialStore &operator=(const CredentialStore &) = delete;

    bool loadCookieFile(const QString &filePath, QString *error = nullptr);
    bool isReady() const;
    QByteArray cookieHeader() const;
    QString csrfToken() const;
    void clear();

private:
    bool parseNetscapeCookieFile(const QByteArray &data);
    bool parseCookieHeader(const QByteArray &data);

    QByteArray m_cookie;
    QString m_csrf;
};

} // namespace blkt
