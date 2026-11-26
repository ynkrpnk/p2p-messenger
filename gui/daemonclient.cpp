#include "daemonclient.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <unistd.h>

DaemonClient::DaemonClient(int port, QObject* parent) : QObject(parent), m_port(port) {
    // Той самий шлях, що обчислює демон (IpcServer::default_path)
    QString rt = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!rt.isEmpty())
        m_path = QString("%1/p2pm-%2.sock").arg(rt).arg(port);
    else
        m_path = QString("/tmp/p2pm-%1-%2.sock").arg(getuid()).arg(port);

    connect(&m_socket, &QLocalSocket::connected, this, &DaemonClient::onConnected);
    connect(&m_socket, &QLocalSocket::disconnected, this, &DaemonClient::onDisconnected);
    connect(&m_socket, &QLocalSocket::readyRead, this, &DaemonClient::onReadyRead);
    connect(&m_retry, &QTimer::timeout, this, &DaemonClient::tryConnect);
    m_retry.setInterval(1000);
}

DaemonClient::~DaemonClient() {
    m_retry.stop();
    m_socket.blockSignals(true);  // при закритті сокета не повідомляємо вікно
    // Якщо демон запустили ми, коректно зупиняємо його (SIGTERM)
    if (m_daemon && m_daemon->state() != QProcess::NotRunning) {
        m_daemon->terminate();
        if (!m_daemon->waitForFinished(2000)) m_daemon->kill();
    }
}

void DaemonClient::start() {
    tryConnect();
    m_retry.start();
}

void DaemonClient::tryConnect() {
    if (m_socket.state() != QLocalSocket::UnconnectedState) return;
    m_socket.connectToServer(m_path);
    // Не вдалося з'єднатись кілька разів поспіль: демона, схоже, немає
    if (!m_socket.waitForConnected(200)) {
        ++m_failures;
        if (m_failures == 2) launchDaemon();
    }
}

void DaemonClient::launchDaemon() {
    if (m_daemon) return;
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString& cand : {dir + "/p2pd", dir + "/../p2pd"}) {
        if (!QFileInfo::exists(cand)) continue;
        m_daemon = new QProcess(this);
        m_daemon->setProcessChannelMode(QProcess::ForwardedChannels);
        m_daemon->start(cand, {QString::number(m_port)});
        return;
    }
}

void DaemonClient::onConnected() {
    m_failures = 0;
    m_buf.clear();
    emit connected();
}

void DaemonClient::onDisconnected() {
    emit disconnected();
}

void DaemonClient::send(const QStringList& fields) {
    if (!isConnected()) return;
    QStringList esc;
    for (const QString& f : fields) esc << escape(f);
    m_socket.write(esc.join('\t').toUtf8() + '\n');
}

void DaemonClient::onReadyRead() {
    m_buf += m_socket.readAll();
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_buf.left(nl));
        m_buf.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        QStringList fields;
        for (const QString& f : line.split('\t')) fields << unescape(f);
        emit received(fields);
    }
}

QString DaemonClient::escape(const QString& s) {
    QString o;
    for (QChar c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') continue;
        else o += c;
    }
    return o;
}

QString DaemonClient::unescape(const QString& s) {
    QString o;
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            o += (s[i] == 'n') ? QChar('\n') : (s[i] == 't') ? QChar('\t') : s[i];
        } else {
            o += s[i];
        }
    }
    return o;
}
