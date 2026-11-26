#include "auth/CredentialStore.hpp"
#include "core/Core.hpp"
#include "plugin/PluginManager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("BLKT"));
    QCoreApplication::setOrganizationName(QStringLiteral("BLKT"));

    // Portable mode: read the pre-provisioned cookie file beside the executable.
    // No configuration, database, cache, or log files are created by this entry point.
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QString cookiePath = QDir(applicationDirectory).filePath(QStringLiteral("cookie.txt"));

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

    if (!core.login(credentials.cookieHeader(), credentials.csrfToken())) {
        credentials.clear();
        QTextStream stream(stderr);
        stream << "BLKT: cookie validation failed" << Qt::endl;
        return 3;
    }
    credentials.clear();

    const QString pluginPath =
        QDir(applicationDirectory).filePath(QStringLiteral("plugins"));
    blkt::PluginManager plugins(&core, pluginPath);

    if (!plugins.start()) {
        core.logout();
        qCritical("BLKT: plugin manager initialization failed");
        return 4;
    }

    return app.exec();
}
