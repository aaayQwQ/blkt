#include "../core/Core.hpp"
#include "../plugin/ScriptPluginManager.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <QStringList>
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
    QStringList logs;
    QObject::connect(&core, &blkt::Core::logMessage, &app,
                     [&logs](const QString &message) { logs.append(message); });
    if (!core.login(QByteArrayLiteral("SESSDATA=session; bili_jct=csrf"),
                    QStringLiteral("csrf"))) {
        qCritical() << "Test setup failed: Core login rejected the test session";
        return 4;
    }
    core.setRoomConnection(QStringLiteral("1"), true);

    blkt::ScriptPluginManager manager(
        &core, directory.path(), {QStringLiteral("stall")}, {});
    if (!manager.start()) {
        qCritical() << "Test setup failed: plugin manager did not start";
        return 5;
    }
    if (!logs.contains(QStringLiteral("Community JavaScript plugin loaded: stall"))) {
        qCritical() << "Test setup failed: stall.js was not loaded";
        for (const QString &message : logs)
            qCritical().noquote() << message;
        return 6;
    }

    const blkt::MessageEvent event{
        QStringLiteral("1"), QStringLiteral("user"),
        QStringLiteral("message"), 1};

    QElapsedTimer timer;
    timer.start();
    core.ingestMessage(event);
    const qint64 firstDispatchMs = timer.elapsed();
    if (firstDispatchMs > 2000 || firstDispatchMs < 100) {
        qCritical() << "Event budget test failed; first dispatch took"
                    << firstDispatchMs << "ms";
        for (const QString &message : logs)
            qCritical().noquote() << message;
        return 7;
    }

    timer.restart();
    core.ingestMessage(event);
    if (timer.elapsed() > 100) {
        qCritical() << "Timed-out plugin was not disabled; second dispatch took"
                    << timer.elapsed() << "ms";
        return 8;
    }

    manager.stop();
    core.logout();
    return 0;
}
