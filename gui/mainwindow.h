#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QPushButton>
#include <QTextBrowser>

#include "daemonclient.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(int port, QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onConnected();
    void onDisconnected();
    void onMessage(const QStringList& f);
    void onPeerSelected();
    void sendCurrent();

private:
    struct PeerInfo {
        QString name;
        bool online = false;
        int unread = 0;
    };

    void refreshList();
    void updateInputState();
    void showPlaceholder();
    void appendMessage(const QString& id, bool outgoing, qint64 ts, const QString& text);
    void appendSystem(const QString& text);
    void appendBlock(const QString& html, Qt::Alignment align);

    DaemonClient m_client;
    QListWidget* m_list;
    QTextBrowser* m_chat;
    QLineEdit* m_input;
    QPushButton* m_send;
    QLabel* m_header;
    QLabel* m_status;

    QMap<QString, PeerInfo> m_peers;
    QString m_current;           // id вибраного вузла
    bool m_historyPending = false;
    QString m_myName;
};
