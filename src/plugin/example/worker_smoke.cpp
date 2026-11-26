#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>

int main(int argc, char *argv[])
{
    if (argc != 2)
        return 2;

    QProcess worker;
    worker.setProcessChannelMode(QProcess::SeparateChannels);
    worker.start(QString::fromLocal8Bit(argv[1]),
                 {QStringLiteral("--blkt-plugin-worker"),
                  QStringLiteral("--blkt-protocol=1")});

    if (!worker.waitForStarted(5000))
        return 3;

    QJsonObject initialize;
    initialize.insert(QStringLiteral("type"), QStringLiteral("initialize"));
    initialize.insert(QStringLiteral("protocol"), 1);
    initialize.insert(QStringLiteral("plugin_id"), QStringLiteral("echo"));
    initialize.insert(QStringLiteral("permissions"), QJsonArray{});

    QByteArray packet = QJsonDocument(initialize).toJson(QJsonDocument::Compact);
    packet.append('\n');
    if (worker.write(packet) != packet.size() || !worker.waitForBytesWritten(5000)) {
        worker.kill();
        worker.waitForFinished(1000);
        return 4;
    }

    if (!worker.waitForReadyRead(5000)) {
        worker.kill();
        worker.waitForFinished(1000);
        return 5;
    }

    QByteArray response = worker.readLine(64 * 1024 + 1);
    if (response.size() > 64 * 1024 || !response.endsWith('\n')) {
        worker.kill();
        worker.waitForFinished(1000);
        return 6;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response.trimmed(), &parseError);
    const QJsonObject ready = document.object();
    if (parseError.error != QJsonParseError::NoError
        || ready.value(QStringLiteral("type")).toString() != QStringLiteral("ready")
        || ready.value(QStringLiteral("protocol")).toInt(-1) != 1
        || ready.value(QStringLiteral("plugin_id")).toString() != QStringLiteral("echo")) {
        worker.kill();
        worker.waitForFinished(1000);
        return 7;
    }

    QJsonObject shutdown;
    shutdown.insert(QStringLiteral("type"), QStringLiteral("shutdown"));
    packet = QJsonDocument(shutdown).toJson(QJsonDocument::Compact);
    packet.append('\n');
    if (worker.write(packet) != packet.size() || !worker.waitForBytesWritten(5000)) {
        worker.kill();
        worker.waitForFinished(1000);
        return 8;
    }

    if (!worker.waitForFinished(3000)) {
        worker.kill();
        worker.waitForFinished(1000);
        return 9;
    }

    return worker.exitStatus() == QProcess::NormalExit && worker.exitCode() == 0
               ? 0
               : 10;
}
