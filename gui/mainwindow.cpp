#include "mainwindow.h"

#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTimer>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>

MainWindow::MainWindow(int port, QWidget* parent) : QMainWindow(parent), m_client(port) {
    setWindowTitle(QString("P2P Месенджер (порт %1)").arg(port));
    resize(900, 600);

    // Ліва панель: список вузлів
    m_list = new QListWidget;
    m_list->setMinimumWidth(200);
    m_list->setUniformItemSizes(true);

    // Права панель: заголовок, чат, поле вводу
    m_header = new QLabel;
    QFont hf = m_header->font();
    hf.setBold(true);
    hf.setPointSize(hf.pointSize() + 2);
    m_header->setFont(hf);

    m_chat = new QTextBrowser;
    m_chat->setOpenLinks(false);

    m_input = new QLineEdit;
    m_send = new QPushButton("Надіслати");
    auto* inputRow = new QHBoxLayout;
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_send);

    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->addWidget(m_header);
    rl->addWidget(m_chat, 1);
    rl->addLayout(inputRow);

    auto* split = new QSplitter;
    split->addWidget(m_list);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({260, 640});
    setCentralWidget(split);

    m_status = new QLabel;
    statusBar()->addWidget(m_status);

    connect(m_list, &QListWidget::itemSelectionChanged, this, &MainWindow::onPeerSelected);
    connect(m_send, &QPushButton::clicked, this, &MainWindow::sendCurrent);
    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::sendCurrent);
    connect(&m_client, &DaemonClient::connected, this, &MainWindow::onConnected);
    connect(&m_client, &DaemonClient::disconnected, this, &MainWindow::onDisconnected);
    connect(&m_client, &DaemonClient::received, this, &MainWindow::onMessage);

    showPlaceholder();
    updateInputState();
    m_status->setText("○ Немає зв'язку з демоном, підключаюсь…");
    m_client.start();
}

MainWindow::~MainWindow() {
    // Клієнт вилучається раніше за віджети: не даємо йому слати сигнали напівзнищеному вікну
    m_client.disconnect(this);
}

// ---------- Зв'язок з демоном ----------
void MainWindow::onConnected() {
    m_status->setText("● Підключено до демона");
    m_client.send({"LIST"});
    // Повторно завантажуємо історію вибраного вузла (після перепідключення)
    if (!m_current.isEmpty()) {
        m_chat->clear();
        m_historyPending = true;
        m_client.send({"HISTORY", m_current, "100"});
    }
    updateInputState();
}

void MainWindow::onDisconnected() {
    m_status->setText("○ Немає зв'язку з демоном, підключаюсь…");
    for (auto& p : m_peers) p.online = false;
    refreshList();
    updateInputState();
}

void MainWindow::onMessage(const QStringList& f) {
    if (f.isEmpty()) return;
    const QString& cmd = f[0];

    if (cmd == "INFO" && f.size() >= 3) {
        m_myName = f[2];
        setWindowTitle(QString("P2P Месенджер: %1").arg(m_myName));
    } else if (cmd == "PEER" && f.size() >= 4) {
        PeerInfo& p = m_peers[f[1]];
        p.name = f[2];
        p.online = (f[3] == "1");
    } else if (cmd == "LISTEND") {
        refreshList();
    } else if (cmd == "PEER_UP" && f.size() >= 3) {
        PeerInfo& p = m_peers[f[1]];
        p.name = f[2];
        p.online = true;
        refreshList();
        updateInputState();
    } else if (cmd == "PEER_DOWN" && f.size() >= 2) {
        if (m_peers.contains(f[1])) m_peers[f[1]].online = false;
        refreshList();
        updateInputState();
    } else if (cmd == "MSG" && f.size() >= 5) {
        const QString& id = f[1];
        bool outgoing = (f[2] == "1");
        if (id == m_current) {
            // Поки йде завантаження історії, нові події вже входять до неї
            if (!m_historyPending) appendMessage(id, outgoing, f[3].toLongLong(), f[4]);
        } else if (!outgoing) {
            m_peers[id].unread++;
            refreshList();
        }
        if (!outgoing && !isActiveWindow()) QApplication::alert(this);
    } else if (cmd == "HMSG" && f.size() >= 5) {
        if (f[1] == m_current) appendMessage(f[1], f[2] == "1", f[3].toLongLong(), f[4]);
    } else if (cmd == "HISTEND") {
        if (f.size() >= 2 && f[1] == m_current) {
            m_historyPending = false;
            m_chat->verticalScrollBar()->setValue(m_chat->verticalScrollBar()->maximum());
        }
    } else if (cmd == "ERR" && f.size() >= 3) {
        appendSystem("⚠ " + f[2]);
    }
}

// ---------- Список вузлів ----------
void MainWindow::refreshList() {
    QList<QString> ids = m_peers.keys();
    std::sort(ids.begin(), ids.end(), [this](const QString& a, const QString& b) {
        const PeerInfo& pa = m_peers[a];
        const PeerInfo& pb = m_peers[b];
        if (pa.online != pb.online) return pa.online;  // спочатку ті, хто в мережі
        return pa.name.toLower() < pb.name.toLower();
    });

    m_list->blockSignals(true);
    m_list->clear();
    int selected = -1;
    for (int i = 0; i < ids.size(); ++i) {
        const PeerInfo& p = m_peers[ids[i]];
        QString text = QString("%1  %2").arg(p.online ? "●" : "○", p.name);
        if (p.unread > 0) text += QString("  (%1)").arg(p.unread);
        auto* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, ids[i]);
        if (!p.online) item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        if (p.unread > 0) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
        m_list->addItem(item);
        if (ids[i] == m_current) selected = i;
    }
    if (selected >= 0) m_list->setCurrentRow(selected);
    m_list->blockSignals(false);
}

void MainWindow::onPeerSelected() {
    auto items = m_list->selectedItems();
    if (items.isEmpty()) return;
    m_current = items[0]->data(Qt::UserRole).toString();
    m_peers[m_current].unread = 0;
    // Список перебудовуємо після повернення з обробника його власного сигналу
    QTimer::singleShot(0, this, &MainWindow::refreshList);

    m_chat->clear();
    m_historyPending = true;
    m_client.send({"HISTORY", m_current, "100"});
    updateInputState();
    m_input->setFocus();
}

void MainWindow::updateInputState() {
    const bool known = !m_current.isEmpty() && m_peers.contains(m_current);
    const bool online = known && m_peers[m_current].online && m_client.isConnected();
    m_header->setText(known ? m_peers[m_current].name : "Оберіть вузол зліва");
    m_input->setEnabled(online);
    m_send->setEnabled(online);
    m_input->setPlaceholderText(!known ? "" : online ? "Введіть повідомлення і натисніть Enter"
                                                      : "Вузол не в мережі, надсилання недоступне");
}

// ---------- Чат ----------
void MainWindow::showPlaceholder() {
    m_chat->setHtml("<p style='color:gray'>Оберіть вузол зі списку ліворуч, щоб почати спілкування.</p>");
}

void MainWindow::appendMessage(const QString& id, bool outgoing, qint64 ts, const QString& text) {
    const QString who = outgoing ? "Ви" : m_peers.value(id).name;
    const QString time = QDateTime::fromSecsSinceEpoch(ts).toString("dd.MM HH:mm");
    QString body = text.toHtmlEscaped();
    body.replace('\n', "<br>");
    const QString html = QString("<span style='color:gray'>%1</span> <b>%2</b><br>%3")
                             .arg(time, who.toHtmlEscaped(), body);
    appendBlock(html, outgoing ? Qt::AlignRight : Qt::AlignLeft);
    if (!m_historyPending)
        m_chat->verticalScrollBar()->setValue(m_chat->verticalScrollBar()->maximum());
}

void MainWindow::appendSystem(const QString& text) {
    appendBlock(QString("<span style='color:#c0392b'>%1</span>").arg(text.toHtmlEscaped()), Qt::AlignHCenter);
}

// Додає абзац у кінець чату з потрібним вирівнюванням
void MainWindow::appendBlock(const QString& html, Qt::Alignment align) {
    QTextCursor c(m_chat->document());
    c.movePosition(QTextCursor::End);
    if (!m_chat->document()->isEmpty()) c.insertBlock();
    c.insertHtml(html);
    QTextBlockFormat bf;
    bf.setAlignment(align);
    bf.setTopMargin(6);
    bf.setBottomMargin(6);
    c.setBlockFormat(bf);
}

void MainWindow::sendCurrent() {
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || m_current.isEmpty()) return;
    m_client.send({"SEND", m_current, text});  // повідомлення з'явиться, коли демон надішле подію MSG
    m_input->clear();
}
