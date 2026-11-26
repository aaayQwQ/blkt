#pragma once

#include <QMainWindow>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace blkt {

class Core;
class BilibiliAdapter;
class PluginManager;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Core *core, BilibiliAdapter *bilibili,
                        PluginManager *plugins, const QString &pluginDirectory,
                        QWidget *parent = nullptr);

    void setAuthenticationState(bool authenticated, const QString &accountName,
                                const QString &message);
    void appendLog(const QString &message);

private:
    void connectRoom();
    void disconnectRoom();
    void choosePlugin();
    void trustSelectedPlugin();
    void revokeSelectedPlugin();
    Permissions selectedPermissions() const;

    Core *m_core = nullptr;
    BilibiliAdapter *m_bilibili = nullptr;
    PluginManager *m_plugins = nullptr;
    QString m_pluginDirectory;
    QString m_selectedPluginPath;

    QLabel *m_accountLabel = nullptr;
    QLabel *m_roomStatusLabel = nullptr;
    QLineEdit *m_roomIdEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;
    QLineEdit *m_pluginPathEdit = nullptr;
    QLabel *m_pluginStatusLabel = nullptr;
    QCheckBox *m_receiveMessage = nullptr;
    QCheckBox *m_sendMessage = nullptr;
    QCheckBox *m_receiveGift = nullptr;
    QCheckBox *m_giftInfo = nullptr;
    QPlainTextEdit *m_logView = nullptr;
};

} // namespace blkt
