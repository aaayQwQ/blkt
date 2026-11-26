#pragma once

#include <QString>
#include <QByteArray>

namespace blkt {

class Guard final
{
public:
    Guard() = delete;

    static QString cleanText(const QString &value, qsizetype maxLength);
    static QString cleanLogText(const QString &value, qsizetype maxLength);
    static bool validBilibiliWebSocketHost(const QString &host);
    static QString redact(const QString &value);
    static bool validRoomId(const QString &roomId);
    static bool validCookie(const QByteArray &cookie);
    static bool validCsrf(const QString &csrf);
};

} // namespace blkt
