#include "app/MainWindow.hpp"
#include "auth/CredentialStore.hpp"
#include "core/Core.hpp"
#include "platform/bilibili/BilibiliAdapter.hpp"
#include "plugin/PluginManager.hpp"

#include <QApplication>
#include <QDir>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
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
        QMessageBox::critical(nullptr, QStringLiteral("BLKT - Cookie"),
                              credentialError);
        return 2;
    }

    blkt::Core core;
    const QString pluginPath =
        QDir(applicationDirectory).filePath(QStringLiteral("plugins"));
    blkt::PluginManager plugins(&core, pluginPath);
    if (!plugins.start()) {
        QMessageBox::critical(nullptr, QStringLiteral("BLKT"),
                              QStringLiteral("Plugin manager initialization failed."));
        credentials.clear();
        return 3;
    }

    blkt::BilibiliAdapter bilibili(&core);
    blkt::MainWindow window(&core, &bilibili, &plugins, pluginPath);

    QObject::connect(&core, &blkt::Core::logMessage,
                     &window, &blkt::MainWindow::appendLog);
    QObject::connect(&bilibili, &blkt::BilibiliAdapter::authenticationFinished,
                     &window, [&window](bool success, const QString &accountName,
                                        quint64, const QString &reason) {
        window.setAuthenticationState(success, accountName, reason);
    });

    window.show();

    const bool started = bilibili.authenticate(
        credentials.cookieHeader(), credentials.csrfToken());
    credentials.clear();

    if (!started) {
        window.setAuthenticationState(false, {},
                                       QStringLiteral("Authentication request could not be started"));
    }

    return app.exec();
}
