#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cmath>
#include <limits>

namespace blkt {
namespace BilibiliProtocol {

// JSON numbers are IEEE-754 doubles. Reject values outside the exact integer
// range instead of silently rounding large remote identifiers.
inline bool toUnsigned(const QJsonValue &value, quint64 *result)
{
    if (!result)
        return false;

    if (value.isString()) {
        bool ok = false;
        const quint64 parsed = value.toString().toULongLong(&ok, 10);
        if (!ok)
            return false;
        *result = parsed;
        return true;
    }

    if (!value.isDouble())
        return false;

    const double number = value.toDouble();
    constexpr double MaxSafeInteger = 9007199254740991.0;
    if (!std::isfinite(number) || number < 0.0
        || std::floor(number) != number || number > MaxSafeInteger)
        return false;

    *result = static_cast<quint64>(number);
    return true;
}

inline bool extractDanmaku(const QJsonObject &command,
                           QString *user, QString *text)
{
    if (!user || !text)
        return false;

    const QString commandName = command.value(QStringLiteral("cmd")).toString();
    if (!commandName.startsWith(QStringLiteral("DANMU_MSG")))
        return false;

    QJsonArray info = command.value(QStringLiteral("info")).toArray();
    const QJsonObject data = command.value(QStringLiteral("data")).toObject();
    if (info.isEmpty())
        info = data.value(QStringLiteral("info")).toArray();

    QString parsedUser;
    QString parsedText;

    if (info.size() >= 3) {
        parsedText = info.at(1).toString();
        const QJsonArray userInfo = info.at(2).toArray();
        if (userInfo.size() > 1)
            parsedUser = userInfo.at(1).toString();

        // Extended web payloads carry the display name in info[2][15].
        if (parsedUser.isEmpty() && userInfo.size() > 15) {
            const QJsonObject extension = userInfo.at(15).toObject();
            const QJsonObject userObject =
                extension.value(QStringLiteral("user")).toObject();
            const QJsonObject base =
                userObject.value(QStringLiteral("base")).toObject();
            parsedUser = base.value(QStringLiteral("name")).toString();
        }
    }

    if (parsedUser.isEmpty())
        parsedUser = data.value(QStringLiteral("uname")).toString();
    if (parsedText.isEmpty())
        parsedText = data.value(QStringLiteral("content")).toString();
    if (parsedText.isEmpty())
        parsedText = data.value(QStringLiteral("msg")).toString();

    if (parsedUser.trimmed().isEmpty() || parsedText.trimmed().isEmpty())
        return false;

    *user = parsedUser;
    *text = parsedText;
    return true;
}

} // namespace BilibiliProtocol
} // namespace blkt
