#include "../core/Core.hpp"
#include "../plugin/ScriptPluginManager.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;

    QFile allowed(directory.filePath(QStringLiteral("echo.js")));
    if (!allowed.open(QIODevice::WriteOnly | QIODevice::Text))
        return 2;
    const QByteArray script = QByteArrayLiteral(
        "BLKT.on('message', function(event) { "
        "BLKT.log('DISPATCH:' + event.user + ':' + event.text); "
        "});\n"
        "BLKT.on('gift', function(event) { BLKT.log('GIFT:' + event.user + ':' + event.giftName + ':' + event.count); });\n"
        "BLKT.on('interaction', function(event) { BLKT.log('INTERACTION:' + event.user + ':' + event.uid); });\n"
        "BLKT.on('superChat', function(event) { BLKT.log('SUPERCHAT:' + event.user + ':' + event.price); });\n"
        "BLKT.on('comboGift', function(event) { BLKT.log('COMBO:' + event.giftName + ':' + event.count); });\n"
        "BLKT.on('roomStats', function(event) { BLKT.log('ROOMSTATS:' + event.online + ':' + event.watched); });\n"
        "BLKT.on('entry', function(event) { BLKT.log('ENTRY:' + event.user + ':' + event.text); });\n"
        "BLKT.on('like', function(event) { BLKT.log('LIKE:' + event.count + ':' + event.total); });\n"
        "BLKT.on('watched', function(event) { BLKT.log('WATCHED:' + event.count); });\n"
        "BLKT.on('notice', function(event) { BLKT.log('NOTICE:' + event.text); });\n"
        "BLKT.on('guard', function(event) { BLKT.log('GUARD:' + event.user + ':' + event.level); });\n"
        "BLKT.on('online', function(event) { BLKT.log('ONLINE:' + event.count); });\n");
    if (allowed.write(script) != script.size())
        return 3;
    allowed.close();

    QFile denied(directory.filePath(QStringLiteral("unlisted.js")));
    if (!denied.open(QIODevice::WriteOnly | QIODevice::Text))
        return 4;
    const QByteArray deniedScript = QByteArrayLiteral(
        "BLKT.on('message', function(event) { BLKT.log('SHOULD_NOT_RUN'); });\n");
    if (denied.write(deniedScript) != deniedScript.size())
        return 5;
    denied.close();

    blkt::Core core;
    QStringList logs;
    QObject::connect(&core, &blkt::Core::logMessage, &app,
                     [&logs](const QString &message) { logs.append(message); });
    if (!core.login(QByteArrayLiteral("SESSDATA=session; bili_jct=csrf"),
                    QStringLiteral("csrf")))
        return 6;
    core.setRoomConnection(QStringLiteral("123"), true);

    blkt::ScriptPluginManager manager(
        &core, directory.path(), {QStringLiteral("echo")}, {});
    if (!manager.start())
        return 7;
    if (!logs.contains(QStringLiteral("Community JavaScript plugin loaded: echo"))
        || logs.contains(QStringLiteral("Community JavaScript plugin loaded: unlisted")))
        return 8;

    core.ingestMessage({QStringLiteral("123"), QStringLiteral("TestUser"),
                        QStringLiteral("hello"), 1});
    bool dispatched = false;
    for (const QString &entry : logs) {
        if (entry.contains(QStringLiteral("DISPATCH:TestUser:hello"))) {
            dispatched = true;
            break;
        }
    }
    if (!dispatched)
        return 9;

    core.ingestGift({QStringLiteral("123"), QStringLiteral("Gifter"),
                      QStringLiteral("GiftName"), 7, 2, 20,
                      QStringLiteral("gold"), 2});

    blkt::PlatformEvent interactionEvent;
    interactionEvent.roomId = QStringLiteral("123");
    interactionEvent.type = QStringLiteral("interaction");
    interactionEvent.timestamp = 2;
    interactionEvent.data = {
        {QStringLiteral("user"), QStringLiteral("Visitor")},
        {QStringLiteral("uid"), QStringLiteral("42")}
    };
    core.ingestPlatformEvent(interactionEvent);

    blkt::PlatformEvent superChatEvent;
    superChatEvent.roomId = QStringLiteral("123");
    superChatEvent.type = QStringLiteral("superChat");
    superChatEvent.timestamp = 3;
    superChatEvent.data = {
        {QStringLiteral("user"), QStringLiteral("Supporter")},
        {QStringLiteral("price"), QStringLiteral("30")}
    };
    core.ingestPlatformEvent(superChatEvent);

    blkt::PlatformEvent comboEvent;
    comboEvent.roomId = QStringLiteral("123");
    comboEvent.type = QStringLiteral("comboGift");
    comboEvent.timestamp = 4;
    comboEvent.data = {
        {QStringLiteral("giftName"), QStringLiteral("Snack")},
        {QStringLiteral("count"), QStringLiteral("10")}
    };
    core.ingestPlatformEvent(comboEvent);

    blkt::PlatformEvent statsEvent;
    statsEvent.roomId = QStringLiteral("123");
    statsEvent.type = QStringLiteral("roomStats");
    statsEvent.timestamp = 5;
    statsEvent.data = {
        {QStringLiteral("online"), QStringLiteral("55")},
        {QStringLiteral("watched"), QStringLiteral("888")}
    };
    core.ingestPlatformEvent(statsEvent);

    blkt::PlatformEvent entryEvent;
    entryEvent.roomId = QStringLiteral("123");
    entryEvent.type = QStringLiteral("entry");
    entryEvent.timestamp = 6;
    entryEvent.data = {
        {QStringLiteral("user"), QStringLiteral("NewVisitor")},
        {QStringLiteral("text"), QStringLiteral("Welcome")}
    };
    core.ingestPlatformEvent(entryEvent);

    blkt::PlatformEvent likeEvent;
    likeEvent.roomId = QStringLiteral("123");
    likeEvent.type = QStringLiteral("like");
    likeEvent.timestamp = 7;
    likeEvent.data = {
        {QStringLiteral("count"), QStringLiteral("5")},
        {QStringLiteral("total"), QStringLiteral("500")}
    };
    core.ingestPlatformEvent(likeEvent);

    blkt::PlatformEvent watchedEvent;
    watchedEvent.roomId = QStringLiteral("123");
    watchedEvent.type = QStringLiteral("watched");
    watchedEvent.timestamp = 8;
    watchedEvent.data = {{QStringLiteral("count"), QStringLiteral("700")}};
    core.ingestPlatformEvent(watchedEvent);

    blkt::PlatformEvent noticeEvent;
    noticeEvent.roomId = QStringLiteral("123");
    noticeEvent.type = QStringLiteral("notice");
    noticeEvent.timestamp = 9;
    noticeEvent.data = {{QStringLiteral("text"), QStringLiteral("System notice")}};
    core.ingestPlatformEvent(noticeEvent);

    blkt::PlatformEvent guardEvent;
    guardEvent.roomId = QStringLiteral("123");
    guardEvent.type = QStringLiteral("guard");
    guardEvent.timestamp = 10;
    guardEvent.data = {
        {QStringLiteral("user"), QStringLiteral("Captain")},
        {QStringLiteral("level"), QStringLiteral("3")}
    };
    core.ingestPlatformEvent(guardEvent);

    blkt::PlatformEvent onlineEvent;
    onlineEvent.roomId = QStringLiteral("123");
    onlineEvent.type = QStringLiteral("online");
    onlineEvent.timestamp = 11;
    onlineEvent.data = {{QStringLiteral("count"), QStringLiteral("66")}};
    core.ingestPlatformEvent(onlineEvent);

    blkt::PlatformEvent wrongRoom = interactionEvent;
    wrongRoom.roomId = QStringLiteral("999");
    core.ingestPlatformEvent(wrongRoom);
    blkt::PlatformEvent unknownType = interactionEvent;
    unknownType.type = QStringLiteral("rawJson");
    core.ingestPlatformEvent(unknownType);

    if (!logs.contains(QStringLiteral("Plugin echo: GIFT:Gifter:GiftName:2"))
        || !logs.contains(QStringLiteral("Plugin echo: INTERACTION:Visitor:42"))
        || !logs.contains(QStringLiteral("Plugin echo: SUPERCHAT:Supporter:30"))
        || !logs.contains(QStringLiteral("Plugin echo: COMBO:Snack:10"))
        || !logs.contains(QStringLiteral("Plugin echo: ROOMSTATS:55:888"))
        || !logs.contains(QStringLiteral("Plugin echo: ENTRY:NewVisitor:Welcome"))
        || !logs.contains(QStringLiteral("Plugin echo: LIKE:5:500"))
        || !logs.contains(QStringLiteral("Plugin echo: WATCHED:700"))
        || !logs.contains(QStringLiteral("Plugin echo: NOTICE:System notice"))
        || !logs.contains(QStringLiteral("Plugin echo: GUARD:Captain:3"))
        || !logs.contains(QStringLiteral("Plugin echo: ONLINE:66")))
        return 10;
    for (const QString &entry : logs) {
        if (entry.contains(QStringLiteral("rawJson"))
            || entry.contains(QStringLiteral("999")))
            return 11;
    }

    manager.stop();
    core.logout();
    return 0;
}
