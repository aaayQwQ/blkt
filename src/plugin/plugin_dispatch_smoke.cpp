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
        "});\n");
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

    manager.stop();
    core.logout();
    return 0;
}
