#include "../IPlugin.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <iostream>
#include <string>

namespace {

constexpr int ProtocolVersion = blkt::PluginProtocol::Version;
constexpr qsizetype MaxPacketBytes = blkt::PluginProtocol::MaxPacketBytes;

bool readPacket(QJsonObject &packet)
{
    std::string line;
    if (!std::getline(std::cin, line))
        return false;
    if (line.empty() || static_cast<qsizetype>(line.size()) > MaxPacketBytes)
        return false;

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;

    packet = document.object();
    return true;
}

bool writePacket(const QJsonObject &packet)
{
    const QByteArray line = QJsonDocument(packet).toJson(QJsonDocument::Compact);
    if (line.isEmpty() || line.size() > MaxPacketBytes)
        return false;

    std::cout.write(line.constData(), line.size());
    std::cout.put('\n');
    std::cout.flush();
    return static_cast<bool>(std::cout);
}

bool hasPermission(const QJsonArray &permissions, const QString &name)
{
    for (const QJsonValue &value : permissions) {
        if (value.toString() == name)
            return true;
    }
    return false;
}

} // namespace

int main()
{
    QJsonObject initialize;
    if (!readPacket(initialize)
        || initialize.value(QStringLiteral("type")).toString()
               != QStringLiteral("initialize")
        || initialize.value(QStringLiteral("protocol")).toInt(-1) != ProtocolVersion)
        return 2;

    const QString pluginId =
        initialize.value(QStringLiteral("plugin_id")).toString();
    if (pluginId.isEmpty() || pluginId.size() > 128)
        return 2;

    QJsonArray permissions =
        initialize.value(QStringLiteral("permissions")).toArray();

    QJsonObject ready;
    ready.insert(QStringLiteral("type"), QStringLiteral("ready"));
    ready.insert(QStringLiteral("protocol"), ProtocolVersion);
    ready.insert(QStringLiteral("plugin_id"), pluginId);
    if (!writePacket(ready))
        return 3;

    QJsonObject packet;
    while (readPacket(packet)) {
        const QString type = packet.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("shutdown"))
            return 0;

        if (type == QStringLiteral("permissions")) {
            permissions = packet.value(QStringLiteral("permissions")).toArray();
            continue;
        }

        if (type != QStringLiteral("event"))
            continue;

        const QString event = packet.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("message")
            && hasPermission(permissions, QStringLiteral("receive_message"))) {
            QJsonObject log;
            log.insert(QStringLiteral("type"), QStringLiteral("log"));
            log.insert(QStringLiteral("message"), QStringLiteral("danmaku received"));
            if (!writePacket(log))
                return 3;
        } else if (event == QStringLiteral("gift")
                   && hasPermission(permissions, QStringLiteral("receive_gift"))) {
            QJsonObject log;
            log.insert(QStringLiteral("type"), QStringLiteral("log"));
            log.insert(QStringLiteral("message"), QStringLiteral("gift received"));
            if (!writePacket(log))
                return 3;
        }
    }

    return 0;
}
