#include "auth/CredentialStore.hpp"
#include "core/Core.hpp"
#include "core/Guard.hpp"
#include "platform/bilibili/BilibiliAdapter.hpp"
#include "plugin/ScriptPluginManager.hpp"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <csignal>

namespace {
volatile std::sig_atomic_t g_stopRequested = 0;
void requestStop(int) { g_stopRequested = 1; }

void writeLog(const QString &message)
{
    QTextStream out(stdout);
    const QString safeMessage = blkt::Guard::cleanLogText(message, 2048);
    out << QDateTime::currentDateTime().toString(Qt::ISODate)
        << " " << safeMessage << Qt::endl;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("BLKT"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("BLKT headless Bilibili live server"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption roomOption(
        {QStringLiteral("r"), QStringLiteral("room")},
        QStringLiteral("Live room ID to connect after account authentication."),
        QStringLiteral("room-id"));
    const QCommandLineOption pluginOption(
        QStringLiteral("allow-plugin"),
        QStringLiteral("Allow a trusted community JavaScript plugin by filename; repeatable."),
        QStringLiteral("plugin-name"));
    const QCommandLineOption pluginSendOption(
        QStringLiteral("plugin-send-message"),
        QStringLiteral("Grant the named allowed plugin permission to send messages."),
        QStringLiteral("plugin-name"));

    parser.addOption(roomOption);
    parser.addOption(pluginOption);
    parser.addOption(pluginSendOption);
    parser.process(app);

    const QString roomId = parser.value(roomOption).trimmed();
    if (!roomId.isEmpty()) {
        if (!blkt::Guard::validRoomId(roomId)) {
            QTextStream(stderr) << "Invalid room ID.\n";
            return 2;
        }
    }

    const QStringList allowedPlugins = parser.values(pluginOption);
    const QStringList sendMessagePlugins = parser.values(pluginSendOption);
    static const QRegularExpression pluginPattern(QStringLiteral("^[A-Za-z0-9_-]{1,48}$"));
    for (const QString &id : allowedPlugins) {
        if (!pluginPattern.match(id).hasMatch()) {
            QTextStream(stderr) << "Invalid plugin name: " << id << Qt::endl;
            return 2;
        }
    }
    for (const QString &id : sendMessagePlugins) {
        if (!pluginPattern.match(id).hasMatch() || !allowedPlugins.contains(id)) {
            QTextStream(stderr) << "--plugin-send-message requires the same name in --allow-plugin.\n";
            return 2;
        }
    }

    const QString appDirectory = QCoreApplication::applicationDirPath();
    blkt::CredentialStore credentials;
    QString credentialError;
    if (!credentials.loadCookieFile(
            QDir(appDirectory).filePath(QStringLiteral("cookie.txt")), &credentialError)) {
        QTextStream(stderr) << "Cookie error: " << credentialError << Qt::endl;
        return 2;
    }

    blkt::Core core;
    QObject::connect(&core, &blkt::Core::logMessage, &app,
                     [](const QString &message) { writeLog(message); });
    const QString pluginDirectory = QDir(appDirectory).filePath(QStringLiteral("plugins"));
    blkt::ScriptPluginManager plugins(
        &core, pluginDirectory, allowedPlugins, sendMessagePlugins);
    if (!plugins.start()) {
        QTextStream(stderr) << "Plugin manager initialization failed.\n";
        credentials.clear();
        return 3;
    }

    blkt::BilibiliAdapter bilibili(&core);
    QObject::connect(&bilibili, &blkt::BilibiliAdapter::authenticationFinished,
                     &app, [&](bool success, const QString &accountName,
                               quint64 accountId, const QString &reason) {
        writeLog(reason);
        if (!success) {
            app.exit(4);
            return;
        }
        writeLog(QStringLiteral("Authenticated account: %1 (UID %2)")
                     .arg(accountName, QString::number(accountId)));
        if (!roomId.isEmpty() && !bilibili.connectRoom(roomId))
            writeLog(QStringLiteral("Room connection could not be started; verify room ID and credentials"));
    });

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    QTimer signalPoll;
    QObject::connect(&signalPoll, &QTimer::timeout, &app, [&] {
        if (g_stopRequested) {
            writeLog(QStringLiteral("Shutdown requested"));
            app.quit();
        }
    });
    signalPoll.start(250);

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
        writeLog(QStringLiteral("Stopping BLKT"));
        bilibili.disconnectRoom();
        plugins.stop();
        core.logout();
        credentials.clear();
    });

    writeLog(QStringLiteral("BLKT headless server starting"));
    writeLog(QStringLiteral("BLKT does not persist logs, cache, configuration, database or temporary files"));
    if (roomId.isEmpty())
        writeLog(QStringLiteral("No room selected; use --room <room-id> to connect a live room"));

    const bool started = bilibili.authenticate(credentials.cookieHeader(), credentials.csrfToken());
    credentials.clear();
    if (!started) {
        QTextStream(stderr) << "Authentication request could not be started." << Qt::endl;
        plugins.stop();
        return 4;
    }
    return app.exec();
}
