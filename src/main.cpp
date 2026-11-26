#include "auth/CredentialStore.hpp"
#include "core/Core.hpp"
#include "platform/bilibili/BilibiliAdapter.hpp"
#include "plugin/PluginManager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("BLKT"));
    QCoreApplication::setOrganizationName(QStringLiteral("BLKT"));

    // Portable mode: cookie.txt is provisioned by the user before launch.
    // The application reads it only; it never creates or modifies it.
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QString cookiePath =
        QDir(applicationDirectory).filePath(QStringLiteral("cookie.txt"));

    blkt::CredentialStore credentials;
    QString credentialError;
    if (!credentials.loadCookieFile(cookiePath, &credentialError)) {
        QTextStream stream(stderr);
        stream << "BLKT: " << credentialError << Qt::endl;
        return 2;
    }

    blkt::Core core;
    QObject::connect(&core, &blkt::Core::logMessage, &app,
                     [](const QString &message) {
                         QTextStream stream(stderr);
                         stream << message << Qt::endl;
                     });

    const QString pluginPath =
        QDir(applicationDirectory).filePath(QStringLiteral("plugins"));
    blkt::PluginManager plugins(&core, pluginPath);
    if (!plugins.start()) {
        credentials.clear();
        qCritical("BLKT: plugin manager initialization failed");
        return 3;
    }

    blkt::BilibiliAdapter bilibili(&core);
    QObject::connect(&bilibili, &blkt::BilibiliAdapter::authenticationFinished,
                     &app, [&app](bool success, const QString &accountName,
                                  quint64, const QString &reason) {
        QTextStream stream(success ? stdout : stderr);
        if (success)
            stream << "BLKT: authenticated as " << accountName << Qt::endl;
        else
            stream << "BLKT: authentication failed: " << reason << Qt::endl;
        if (!success)
            app.exit(4);
    });

    const bool started = bilibili.authenticate(
        credentials.cookieHeader(), credentials.csrfToken());
    credentials.clear();

    if (!started) {
        plugins.stop();
        qCritical("BLKT: authentication request could not be started");
        return 4;
    }

    return app.exec();
}
