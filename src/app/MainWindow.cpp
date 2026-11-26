#include "MainWindow.hpp"

#include "../core/Core.hpp"
#include "../platform/bilibili/BilibiliAdapter.hpp"
#include "../plugin/IPlugin.hpp"
#include "../plugin/PluginManager.hpp"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QWidget>
#include <QTimer>

namespace blkt {

MainWindow::MainWindow(Core *core, BilibiliAdapter *bilibili,
                       PluginManager *plugins, const QString &pluginDirectory,
                       QWidget *parent)
    : QMainWindow(parent),
      m_core(core),
      m_bilibili(bilibili),
      m_plugins(plugins),
      m_pluginDirectory(QDir::cleanPath(pluginDirectory))
{
    setWindowTitle(QStringLiteral("BLKT"));
    resize(900, 640);

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);

    auto *accountGroup = new QGroupBox(QStringLiteral("Account"), central);
    auto *accountLayout = new QVBoxLayout(accountGroup);
    m_accountLabel = new QLabel(QStringLiteral("Authenticating..."), accountGroup);
    m_roomStatusLabel = new QLabel(QStringLiteral("Live room: disconnected"), accountGroup);
    accountLayout->addWidget(m_accountLabel);
    accountLayout->addWidget(m_roomStatusLabel);

    auto *roomRow = new QHBoxLayout;
    m_roomIdEdit = new QLineEdit(accountGroup);
    m_roomIdEdit->setPlaceholderText(QStringLiteral("Bilibili live room ID"));
    m_connectButton = new QPushButton(QStringLiteral("Connect"), accountGroup);
    m_disconnectButton = new QPushButton(QStringLiteral("Disconnect"), accountGroup);
    m_disconnectButton->setEnabled(false);
    m_connectButton->setEnabled(false);
    roomRow->addWidget(m_roomIdEdit, 1);
    roomRow->addWidget(m_connectButton);
    roomRow->addWidget(m_disconnectButton);
    accountLayout->addLayout(roomRow);
    root->addWidget(accountGroup);

    auto *pluginGroup = new QGroupBox(QStringLiteral("Third-party plugins"), central);
    auto *pluginLayout = new QVBoxLayout(pluginGroup);
    auto *pluginFileRow = new QHBoxLayout;
    m_pluginPathEdit = new QLineEdit(pluginGroup);
    m_pluginPathEdit->setReadOnly(true);
    m_pluginPathEdit->setPlaceholderText(QStringLiteral("Select a plugin executable from the plugins directory"));
    auto *browseButton = new QPushButton(QStringLiteral("Browse..."), pluginGroup);
    pluginFileRow->addWidget(m_pluginPathEdit, 1);
    pluginFileRow->addWidget(browseButton);
    pluginLayout->addLayout(pluginFileRow);

    auto *permissionRow = new QHBoxLayout;
    m_receiveMessage = new QCheckBox(QStringLiteral("Receive danmaku"), pluginGroup);
    m_sendMessage = new QCheckBox(QStringLiteral("Send danmaku"), pluginGroup);
    m_receiveGift = new QCheckBox(QStringLiteral("Receive gifts"), pluginGroup);
    m_giftInfo = new QCheckBox(QStringLiteral("Read gift catalog"), pluginGroup);
    permissionRow->addWidget(m_receiveMessage);
    permissionRow->addWidget(m_sendMessage);
    permissionRow->addWidget(m_receiveGift);
    permissionRow->addWidget(m_giftInfo);
    pluginLayout->addLayout(permissionRow);

    auto *pluginButtonRow = new QHBoxLayout;
    auto *trustButton = new QPushButton(QStringLiteral("Trust and load"), pluginGroup);
    auto *revokeButton = new QPushButton(QStringLiteral("Revoke trust"), pluginGroup);
    pluginButtonRow->addWidget(trustButton);
    pluginButtonRow->addWidget(revokeButton);
    pluginButtonRow->addStretch(1);
    pluginLayout->addLayout(pluginButtonRow);

    m_pluginStatusLabel = new QLabel(
        QStringLiteral("Plugins run as separate processes, but are not yet OS-sandboxed. Permissions gate BLKT API calls only; trusted plugin code may still access files and networks available to your OS user. Do not load untrusted code."),
        pluginGroup);
    m_pluginStatusLabel->setWordWrap(true);
    pluginLayout->addWidget(m_pluginStatusLabel);
    root->addWidget(pluginGroup);

    auto *logGroup = new QGroupBox(QStringLiteral("Runtime log (memory only)"), central);
    auto *logLayout = new QVBoxLayout(logGroup);
    m_logView = new QPlainTextEdit(logGroup);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(2000);
    logLayout->addWidget(m_logView);
    root->addWidget(logGroup, 1);

    setCentralWidget(central);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(500);
    connect(m_statusTimer, &QTimer::timeout, this, [this] {
        if (!m_bilibili)
            return;
        if (m_bilibili->isRoomConnected()) {
            m_roomStatusLabel->setText(QStringLiteral("Live room: connected"));
            m_connectButton->setEnabled(false);
            m_disconnectButton->setEnabled(true);
        } else if (m_bilibili->isRoomRequested()) {
            m_roomStatusLabel->setText(QStringLiteral("Live room: connecting / reconnecting"));
            m_connectButton->setEnabled(false);
            m_disconnectButton->setEnabled(true);
        } else {
            m_roomStatusLabel->setText(QStringLiteral("Live room: disconnected"));
            m_connectButton->setEnabled(m_bilibili->isAuthenticated());
            m_disconnectButton->setEnabled(false);
        }
    });
    m_statusTimer->start();

    connect(m_connectButton, &QPushButton::clicked,
            this, &MainWindow::connectRoom);
    connect(m_disconnectButton, &QPushButton::clicked,
            this, &MainWindow::disconnectRoom);
    connect(browseButton, &QPushButton::clicked,
            this, &MainWindow::choosePlugin);
    connect(trustButton, &QPushButton::clicked,
            this, &MainWindow::trustSelectedPlugin);
    connect(revokeButton, &QPushButton::clicked,
            this, &MainWindow::revokeSelectedPlugin);

    if (!QDir(m_pluginDirectory).exists()) {
        m_pluginStatusLabel->setText(
            QStringLiteral("The plugins directory does not exist. Create it manually beside the application; BLKT will not create folders or files."));
    }
}

void MainWindow::setAuthenticationState(bool authenticated,
                                        const QString &accountName,
                                        const QString &message)
{
    if (authenticated) {
        m_accountLabel->setText(
            QStringLiteral("Authenticated: %1").arg(accountName));
        m_connectButton->setEnabled(true);
    } else {
        m_accountLabel->setText(QStringLiteral("Account unavailable: %1").arg(message));
        m_connectButton->setEnabled(false);
    }
    appendLog(message);
}

void MainWindow::appendLog(const QString &message)
{
    const QString safe = message.left(2048);
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_logView->appendPlainText(QStringLiteral("[%1] %2").arg(timestamp, safe));
}

void MainWindow::connectRoom()
{
    if (!m_bilibili || !m_bilibili->isAuthenticated()) {
        QMessageBox::warning(this, QStringLiteral("BLKT"),
                             QStringLiteral("The Bilibili account has not been authenticated."));
        return;
    }

    const QString roomId = m_roomIdEdit->text().trimmed();
    static const QRegularExpression roomPattern(QStringLiteral("^[0-9]{1,20}$"));
    if (!roomPattern.match(roomId).hasMatch()) {
        QMessageBox::warning(this, QStringLiteral("Invalid room ID"),
                             QStringLiteral("Enter a numeric Bilibili live room ID."));
        return;
    }

    if (!m_bilibili->connectRoom(roomId)) {
        QMessageBox::warning(this, QStringLiteral("BLKT"),
                             QStringLiteral("The live room connection could not be started."));
        return;
    }

    m_roomStatusLabel->setText(QStringLiteral("Live room: connecting (%1)").arg(roomId));
    m_connectButton->setEnabled(false);
    m_disconnectButton->setEnabled(true);
}

void MainWindow::disconnectRoom()
{
    if (m_bilibili)
        m_bilibili->disconnectRoom();
    m_roomStatusLabel->setText(QStringLiteral("Live room: disconnected"));
    m_connectButton->setEnabled(m_bilibili && m_bilibili->isAuthenticated());
    m_disconnectButton->setEnabled(false);
}

void MainWindow::choosePlugin()
{
#if defined(Q_OS_WIN)
    const QString filter = QStringLiteral("Plugin executables (*.exe)");
#else
    const QString filter = QStringLiteral("BLKT plugin executables (*.blkt-plugin)");
#endif
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select plugin"), m_pluginDirectory, filter);
    if (path.isEmpty())
        return;

    const QFileInfo info(path);
    const QString selectedParent = QDir::cleanPath(info.canonicalPath());
    const QString expectedParent = QDir::cleanPath(QFileInfo(m_pluginDirectory).canonicalFilePath());
    if (selectedParent.isEmpty() || expectedParent.isEmpty()
        || selectedParent != expectedParent || info.isSymLink()) {
        QMessageBox::warning(this, QStringLiteral("Invalid plugin location"),
                             QStringLiteral("Select a regular plugin executable directly inside the application's plugins directory."));
        return;
    }

    m_selectedPluginPath = info.canonicalFilePath();
    m_pluginPathEdit->setText(info.fileName());
    m_pluginStatusLabel->setText(
        QStringLiteral("Selected %1. Choose only the capabilities this plugin needs.")
            .arg(info.fileName()));
}

Permissions MainWindow::selectedPermissions() const
{
    Permissions permissions = 0;
    if (m_receiveMessage->isChecked())
        permissions |= permissionFlag(Permission::ReceiveMessage);
    if (m_sendMessage->isChecked())
        permissions |= permissionFlag(Permission::SendMessage);
    if (m_receiveGift->isChecked())
        permissions |= permissionFlag(Permission::ReceiveGift);
    if (m_giftInfo->isChecked())
        permissions |= permissionFlag(Permission::GiftInfo);
    return permissions;
}

void MainWindow::trustSelectedPlugin()
{
    if (!m_plugins || m_selectedPluginPath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("BLKT"),
                                 QStringLiteral("Select a plugin first."));
        return;
    }

    const QFileInfo info(m_selectedPluginPath);
    if (!info.exists() || !info.isFile() || info.isSymLink()
        || info.size() <= 0 || info.size() > 256LL * 1024LL * 1024LL) {
        QMessageBox::warning(this, QStringLiteral("Invalid plugin"),
                             QStringLiteral("The plugin file is missing or exceeds the 256 MiB limit."));
        return;
    }

    QFile file(m_selectedPluginPath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Plugin read failed"),
                             QStringLiteral("BLKT could not read the selected plugin."));
        return;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        QMessageBox::warning(this, QStringLiteral("Plugin hash failed"),
                             QStringLiteral("BLKT could not calculate the plugin fingerprint."));
        return;
    }

    const QByteArray digest = hash.result().toHex();
    const QString pluginId = info.completeBaseName();
    if (!m_plugins->trustPlugin(info.fileName(), digest)) {
        QMessageBox::warning(this, QStringLiteral("Plugin rejected"),
                             QStringLiteral("The plugin could not be trusted. Check its filename and platform format."));
        return;
    }

    m_plugins->setGrantedPermissions(pluginId, selectedPermissions());
    m_pluginStatusLabel->setText(
        QStringLiteral("Trusted %1 (SHA-256: %2…). Trust and permissions are memory-only and reset when BLKT exits.")
            .arg(info.fileName(), QString::fromLatin1(digest.left(16))));
    appendLog(QStringLiteral("Plugin trust granted: %1").arg(pluginId));
}

void MainWindow::revokeSelectedPlugin()
{
    if (!m_plugins || m_selectedPluginPath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("BLKT"),
                                 QStringLiteral("Select a plugin first."));
        return;
    }

    const QString fileName = QFileInfo(m_selectedPluginPath).fileName();
    m_plugins->revokePluginTrust(fileName);
    m_pluginStatusLabel->setText(
        QStringLiteral("Trust revoked for %1.").arg(fileName));
    appendLog(QStringLiteral("Plugin trust revoked: %1").arg(fileName));
}

} // namespace blkt
