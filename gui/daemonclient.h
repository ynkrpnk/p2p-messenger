#pragma once
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

// Зв'язок GUI з демоном p2pd через UNIX domain socket.
// Протокол: один рядок = одне повідомлення, поля розділені табуляцією,
// символи \ \n \t в полях екрановані (\\ \n \t).
// Якщо демон не запущено, клієнт сам запускає p2pd із тієї ж папки, що й GUI.
class DaemonClient : public QObject {
    Q_OBJECT
public:
    explicit DaemonClient(int port, QObject* parent = nullptr);
    ~DaemonClient() override;

    void start();
    bool isConnected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    void send(const QStringList& fields);

signals:
    void connected();
    void disconnected();
    void received(const QStringList& fields);  // одне повідомлення від демона

private slots:
    void tryConnect();
    void onReadyRead();
    void onConnected();
    void onDisconnected();

private:
    static QString escape(const QString& s);
    static QString unescape(const QString& s);
    void launchDaemon();

    int m_port;
    QString m_path;
    QLocalSocket m_socket;
    QTimer m_retry;
    QProcess* m_daemon = nullptr;
    QByteArray m_buf;
    int m_failures = 0;
};
