#include "../core/Core.hpp"
#include "../plugin/ScriptPluginManager.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

namespace {
bool writeScript(const QString &path, const QByteArray &source)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Text)
        && file.write(source) == source.size();
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;

    const QByteArray deniedSource = QByteArrayLiteral(
        "var accepted = BLKT.openAIChat({"
        "endpoint:'https://api.openai.com/v1/chat/completions',"
        "api_key:'unit-test-secret',model:'test-model',"
        "messages:[{role:'system',content:'test'}]"
        "}, function() {});"
        "BLKT.log('OPENAI_PERMISSION_DENIED:' + (!accepted));"
    );
    const QByteArray allowedSource = QByteArrayLiteral(
        "var httpAccepted = BLKT.openAIChat({"
        "endpoint:'http://api.openai.com/v1/chat/completions',"
        "api_key:'unit-test-secret',model:'test-model',"
        "messages:[{role:'system',content:'test'}]"
        "}, function() {});"
        "var localAccepted = BLKT.openAIChat({"
        "endpoint:'https://localhost/v1/chat/completions',"
        "api_key:'unit-test-secret',model:'test-model',"
        "messages:[{role:'system',content:'test'}]"
        "}, function() {});"
        "BLKT.log('OPENAI_ENDPOINTS_REJECTED:' + (!httpAccepted && !localAccepted));"
    );

    if (!writeScript(directory.filePath(QStringLiteral("denied.js")), deniedSource)
        || !writeScript(directory.filePath(QStringLiteral("allowed.js")), allowedSource))
        return 2;

    blkt::Core core;
    QStringList logs;
    QObject::connect(&core, &blkt::Core::logMessage, &app,
                     [&logs](const QString &message) { logs.append(message); });

    blkt::ScriptPluginManager manager(
        &core, directory.path(),
        {QStringLiteral("denied"), QStringLiteral("allowed")},
        {}, {QStringLiteral("allowed")});
    if (!manager.start())
        return 3;

    if (!logs.contains(QStringLiteral("Plugin denied: OPENAI_PERMISSION_DENIED:true"))
        || !logs.contains(QStringLiteral("Plugin allowed: OPENAI_ENDPOINTS_REJECTED:true"))
        || logs.contains(QStringLiteral("unit-test-secret"))
        || !logs.contains(QStringLiteral("Community JavaScript plugin loaded: denied"))
        || !logs.contains(QStringLiteral("Community JavaScript plugin loaded: allowed"))) {
        manager.stop();
        return 4;
    }

    manager.stop();
    return 0;
}
