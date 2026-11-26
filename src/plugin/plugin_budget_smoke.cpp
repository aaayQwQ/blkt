#include "../core/Core.hpp"
#include "../plugin/ScriptPluginManager.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;

    QFile script(directory.filePath(QStringLiteral("stall.js")));
    if (!script.open(QIODevice::WriteOnly | QIODevice::Text))
        return 2;
    const QByteArray source = QByteArrayLiteral(
        "BLKT.on('message', function(event) { while (true) {} });\n");
    if (script.write(source) != source.size())
        return 3;
    script.close();

    blkt::Core core;
    if (!core.login(QByteArrayLiteral("SESSDATA=session; bili_jct=csrf"),
                    QStringLiteral("csrf")))
        return 4;
    core.setRoomConnection(QStringLiteral("1"), true);

    blkt::ScriptPluginManager manager(
        &core, directory.path(), {QStringLiteral("stall")}, {});
    if (!manager.start())
        return 5;

    const blkt::MessageEvent event{
        QStringLiteral("1"), QStringLiteral("user"),
        QStringLiteral("message"), 1};

    QElapsedTimer timer;
    timer.start();
    core.ingestMessage(event);
    const qint64 firstDispatchMs = timer.elapsed();
    if (firstDispatchMs > 2000 || firstDispatchMs < 100)
        return 6;

    timer.restart();
    core.ingestMessage(event);
    if (timer.elapsed() > 100)
        return 7;

    manager.stop();
    core.logout();
    return 0;
}
