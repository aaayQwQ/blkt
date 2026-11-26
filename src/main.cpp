#include "core/Core.hpp"
#include "plugin/PluginManager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("BLKT"));
    QCoreApplication::setOrganizationName(QStringLiteral("BLKT"));

    const QString pluginPath =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugins"));

    blkt::Core core;
    blkt::PluginManager plugins(&core, pluginPath);

    QObject::connect(&core, &blkt::Core::logMessage, &app,
                     [](const QString &message) {
                         QTextStream stream(stderr);
                         stream << message << Qt::endl;
                     });

    if (!plugins.start()) {
        qCritical("BLKT: plugin manager initialization failed");
        return 1;
    }

    return app.exec();
}
