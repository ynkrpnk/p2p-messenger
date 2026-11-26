#include "ipc/ipc.h"
#include "protocol/packet.h"
#include "storage/storage.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

// ---------- Параметри ----------
constexpr int DISCOVERY_PORT = 45454;  // UDP-порт для виявлення вузлів
constexpr int BEACON_INTERVAL = 5;     // як часто розсилаємо "я тут" (секунди)
constexpr int PING_INTERVAL = 5;       // як часто надсилаємо Ping (секунди)
constexpr int PEER_TIMEOUT = 15;       // скільки чекаємо відповіді до розриву (секунди)
constexpr int KNOWN_TTL = 20;          // через скільки секунд забуваємо вузол зі списку

// ---------- Дані вузла ----------
// Стан одного TCP-з'єднання з вузлом
struct Peer {
    std::string id;       // унікальний ідентифікатор вузла (з Hello)
    std::string name;     // ім'я вузла (з Hello)
    std::string inbuf;    // накопичені прийняті байти
    std::string outbuf;   // байти, які ще не вдалося відправити
    bool connecting = false;  // true, поки неблокуючий connect() не завершився
    bool want_out = false;    // чи підписані ми на EPOLLOUT
    time_t last_rx = 0;       // коли востаннє щось отримали
    time_t last_ping = 0;     // коли востаннє надсилали Ping
};

// Вузол, знайдений через UDP-розсилку
struct Known {
    std::string name;
    std::string ip;
    int port = 0;
    time_t last_seen = 0;
};

static int g_ep = -1;
static bool g_running = true;
static std::string g_id;    // наш ідентифікатор
static std::string g_name;  // наше ім'я
static int g_port = 0;
static int g_udp_fd = -1;
static std::map<int, Peer> g_peers;
static std::map<std::string, Known> g_known;
static std::unique_ptr<Storage> g_db;  // локальна база історії
static IpcServer g_ipc;                // сокет для GUI-клієнта

// ---------- Допоміжні функції ----------
static void die(const char* msg) {
    perror(msg);
    exit(1);
}

static time_t now_sec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) die("fcntl");
}

static std::string make_node_id() {
    std::random_device rd;
    uint64_t v = (static_cast<uint64_t>(rd()) << 32) | rd();
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

static void trim_eol(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

static std::string label(const Peer& p, int fd) {
    return p.name.empty() ? "fd=" + std::to_string(fd) : p.name;
}

// ---------- epoll ----------
static uint32_t wanted_events(const Peer& p) {
    return EPOLLIN | ((p.connecting || !p.outbuf.empty()) ? static_cast<uint32_t>(EPOLLOUT) : 0u);
}

static void epoll_add(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(g_ep, EPOLL_CTL_ADD, fd, &ev) < 0) die("epoll_ctl ADD");
}

// Оновлюємо підписку на EPOLLOUT, лише якщо вона змінилася
static void update_events(int fd, Peer& p) {
    bool need = (wanted_events(p) & EPOLLOUT) != 0;
    if (need == p.want_out) return;
    p.want_out = need;
    epoll_event ev{};
    ev.events = wanted_events(p);
    ev.data.fd = fd;
    epoll_ctl(g_ep, EPOLL_CTL_MOD, fd, &ev);
}

// ---------- Робота з вузлами ----------
static void close_peer(int fd) {
    auto it = g_peers.find(fd);
    if (it == g_peers.end()) return;
    printf("[-] %s відключився\n", label(it->second, fd).c_str());
    if (!it->second.id.empty()) g_ipc.broadcast({"PEER_DOWN", it->second.id});
    epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    g_peers.erase(it);
}

// Відправляємо накопичене. false = фатальна помилка з'єднання.
static bool flush_peer(int fd, Peer& p) {
    if (p.connecting) return true;
    while (!p.outbuf.empty()) {
        ssize_t n = send(fd, p.outbuf.data(), p.outbuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            p.outbuf.erase(0, n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // сокет переповнений, продовжимо по EPOLLOUT
        } else {
            return false;
        }
    }
    update_events(fd, p);
    return true;
}

static bool send_packet(int fd, Peer& p, PacketType type, const std::string& body) {
    p.outbuf += encode_packet(type, body);
    return flush_peer(fd, p);
}

// Реєструємо нове з'єднання і ставимо в чергу Hello
static void add_peer(int fd, bool connecting) {
    Peer& p = g_peers[fd];
    p.connecting = connecting;
    p.last_rx = p.last_ping = now_sec();
    p.outbuf += encode_packet(PacketType::Hello, g_id + "|" + g_name);
    p.want_out = (wanted_events(p) & EPOLLOUT) != 0;
    epoll_add(fd, wanted_events(p));
}

static bool is_known_peer(const std::string& id) {
    for (auto& [fd, p] : g_peers)
        if (p.id == id) return true;
    return false;
}

// Неблокуючий connect: не зависаємо, якщо вузол недоступний
static bool start_connect(const std::string& ip, int port, const std::string& id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    set_nonblocking(fd);

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        close(fd);
        return false;
    }
    if (connect(fd, (sockaddr*)&a, sizeof(a)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    add_peer(fd, true);
    g_peers[fd].id = id;
    printf("[*] підключаюсь до %s:%d ...\n", ip.c_str(), port);
    return true;
}

static void handle_packet(int fd, Peer& p, const Packet& pk) {
    switch (pk.type) {
        case PacketType::Hello: {
            size_t bar = pk.body.find('|');
            if (bar == std::string::npos) {
                p.name = pk.body;
            } else {
                p.id = pk.body.substr(0, bar);
                p.name = pk.body.substr(bar + 1);
            }
            printf("[*] представився: %s\n", p.name.c_str());
            g_db->touch_peer(p.id, p.name);
            if (!p.id.empty()) g_ipc.broadcast({"PEER_UP", p.id, p.name});
            break;
        }
        case PacketType::Text: {
            std::string t = pk.body;
            trim_eol(t);
            printf("[%s] %s\n", label(p, fd).c_str(), t.c_str());
            g_db->save_message(p.id, false, t);
            g_ipc.broadcast({"MSG", p.id, "0", std::to_string(time(nullptr)), t});
            break;
        }
        case PacketType::Ping:
            send_packet(fd, p, PacketType::Pong, "");
            break;
        case PacketType::Pong:
            break;  // last_rx уже оновлено
    }
}

static void on_readable(int fd) {
    Peer& p = g_peers[fd];
    bool closed = false;

    while (true) {
        char tmp[4096];
        ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
        if (r > 0) {
            p.inbuf.append(tmp, r);
        } else if (r == 0) {
            closed = true;
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) closed = true;
            break;
        }
    }
    if (!p.inbuf.empty()) p.last_rx = now_sec();

    // Спочатку розбираємо все отримане, і лише потім закриваємо з'єднання
    Packet pk;
    ParseResult res;
    while ((res = try_decode(p.inbuf, pk)) == ParseResult::Ok) handle_packet(fd, p, pk);

    if (res == ParseResult::Error) {
        printf("[-] помилка протоколу від %s\n", label(p, fd).c_str());
        closed = true;
    }
    if (closed) close_peer(fd);
}

static void on_writable(int fd) {
    Peer& p = g_peers[fd];
    if (p.connecting) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            printf("[!] не вдалося підключитись: %s\n", strerror(err));
            close_peer(fd);
            return;
        }
        p.connecting = false;
        printf("[+] з'єднання встановлено (fd=%d)\n", fd);
    }
    if (!flush_peer(fd, p)) close_peer(fd);
}

// ---------- Виявлення вузлів (UDP broadcast) ----------
static void setup_discovery() {
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) die("socket udp");
    int yes = 1;
    // REUSEADDR дозволяє кільком вузлам на одній машині слухати один порт
    setsockopt(g_udp_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(g_udp_fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(DISCOVERY_PORT);
    if (bind(g_udp_fd, (sockaddr*)&a, sizeof(a)) < 0) die("bind udp");
    set_nonblocking(g_udp_fd);
    epoll_add(g_udp_fd, EPOLLIN);
}

// Формат: P2PM1|<id>|<tcp-порт>|<ім'я>
static void send_beacon() {
    std::string msg = "P2PM1|" + g_id + "|" + std::to_string(g_port) + "|" + g_name;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(DISCOVERY_PORT);
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(g_udp_fd, msg.data(), msg.size(), 0, (sockaddr*)&a, sizeof(a));
}

static bool parse_beacon(const std::string& s, std::string& id, int& port, std::string& name) {
    if (s.compare(0, 6, "P2PM1|") != 0) return false;
    size_t b = s.find('|', 6);
    if (b == std::string::npos) return false;
    size_t c = s.find('|', b + 1);
    if (c == std::string::npos) return false;
    id = s.substr(6, b - 6);
    port = atoi(s.substr(b + 1, c - b - 1).c_str());
    name = s.substr(c + 1);
    return !id.empty() && port > 0 && port < 65536;
}

static void on_beacon() {
    while (true) {
        char buf[512];
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(g_udp_fd, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (r < 0) break;  // EAGAIN: датаграм більше немає

        std::string id, name;
        int port;
        if (!parse_beacon(std::string(buf, r), id, port, name)) continue;
        if (id == g_id) continue;  // це ми самі

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

        bool fresh = g_known.find(id) == g_known.end();
        g_known[id] = Known{name, ip, port, now_sec()};
        if (fresh) printf("[?] знайдено вузол %s (%s:%d)\n", name.c_str(), ip, port);

        // Щоб обидва вузли не з'єднувались одночасно, ініціатор той, чий id менший
        if (!is_known_peer(id) && g_id < id) start_connect(ip, port, id);
    }
}

// ---------- Таймер: heartbeat ----------
static int g_tick = 0;

static void on_timer(int tfd) {
    uint64_t exp;
    if (read(tfd, &exp, sizeof(exp)) < 0) return;  // скидаємо лічильник таймера
    time_t now = now_sec();
    ++g_tick;

    if (g_tick % BEACON_INTERVAL == 0) send_beacon();

    std::vector<int> dead;
    for (auto& [fd, p] : g_peers) {
        if (now - p.last_rx > PEER_TIMEOUT) {
            dead.push_back(fd);
        } else if (!p.connecting && now - p.last_ping >= PING_INTERVAL) {
            p.last_ping = now;
            if (!send_packet(fd, p, PacketType::Ping, "")) dead.push_back(fd);
        }
    }
    for (int fd : dead) close_peer(fd);

    for (auto it = g_known.begin(); it != g_known.end();) {
        if (now - it->second.last_seen > KNOWN_TTL) it = g_known.erase(it);
        else ++it;
    }
}

// ---------- Ввід користувача ----------
static void print_peers() {
    printf("--- Підключені вузли ---\n");
    for (auto& [fd, p] : g_peers)
        printf("  %s%s\n", label(p, fd).c_str(), p.connecting ? " (підключення...)" : "");
    printf("--- Знайдені в мережі ---\n");
    for (auto& [id, k] : g_known)
        printf("  %s  %s:%d  %s\n", k.name.c_str(), k.ip.c_str(), k.port,
               is_known_peer(id) ? "[з'єднано]" : "");
}

// Надсилає текст вузлу, зберігає в історію і повідомляє GUI
static bool send_text(int fd, Peer& p, const std::string& text) {
    if (!send_packet(fd, p, PacketType::Text, text)) return false;
    g_db->save_message(p.id, true, text);
    g_ipc.broadcast({"MSG", p.id, "1", std::to_string(time(nullptr)), text});
    return true;
}

static int find_peer_fd(const std::string& id) {
    for (auto& [fd, p] : g_peers)
        if (!p.connecting && p.id == id) return fd;
    return -1;
}

// Привітання для щойно підключеного GUI: хто ми і хто зараз у мережі
static void on_ipc_connect(int client) {
    g_ipc.send(client, {"INFO", g_id, g_name});
    for (auto& [fd, p] : g_peers)
        if (!p.connecting && !p.id.empty()) g_ipc.send(client, {"PEER_UP", p.id, p.name});
}

// Команди від GUI:
//   LIST                    -> PEER <id> <ім'я> <1|0> ..., LISTEND
//   SEND <id> <текст>       -> (подія MSG) або ERR
//   HISTORY <id> [кількість]-> HMSG <id> <1|0> <час> <текст> ..., HISTEND <id>
static void on_ipc_command(int client, const std::vector<std::string>& a) {
    const std::string& cmd = a[0];
    if (cmd == "LIST") {
        std::map<std::string, bool> online;
        for (auto& [fd, p] : g_peers)
            if (!p.connecting && !p.id.empty()) {
                online[p.id] = true;
                g_ipc.send(client, {"PEER", p.id, p.name, "1"});
            }
        for (auto& sp : g_db->peers())
            if (!online.count(sp.id)) g_ipc.send(client, {"PEER", sp.id, sp.name, "0"});
        g_ipc.send(client, {"LISTEND"});
    } else if (cmd == "SEND" && a.size() >= 3) {
        int fd = find_peer_fd(a[1]);
        if (fd < 0) {
            g_ipc.send(client, {"ERR", a[1], "вузол не в мережі"});
        } else if (!send_text(fd, g_peers[fd], a[2])) {
            close_peer(fd);
            g_ipc.send(client, {"ERR", a[1], "помилка відправки"});
        }
    } else if (cmd == "HISTORY" && a.size() >= 2) {
        int limit = a.size() >= 3 ? atoi(a[2].c_str()) : 50;
        if (limit <= 0 || limit > 500) limit = 50;
        for (auto& m : g_db->history(a[1], limit))
            g_ipc.send(client, {"HMSG", a[1], m.outgoing ? "1" : "0", std::to_string(m.ts), m.body});
        g_ipc.send(client, {"HISTEND", a[1]});
    } else {
        g_ipc.send(client, {"ERR", "", "невідома команда: " + cmd});
    }
}

static std::string fmt_time(time_t t) {
    char buf[32];
    tm lt;
    localtime_r(&t, &lt);
    strftime(buf, sizeof(buf), "%d.%m %H:%M", &lt);
    return buf;
}

// /history без аргументу — список переписок; /history <частина імені> — остання історія
static void print_history(const std::string& query) {
    auto peers = g_db->peers();
    if (query.empty()) {
        printf("--- Переписки (%s) ---\n", g_db->path().c_str());
        for (auto& p : peers)
            printf("  %s  [%d повідомлень, востаннє %s]\n", p.name.c_str(), p.messages,
                   fmt_time(p.last_seen).c_str());
        if (peers.empty()) printf("  (порожньо)\n");
        return;
    }
    for (auto& p : peers) {
        if (p.name.find(query) == std::string::npos) continue;
        printf("--- Історія з %s ---\n", p.name.c_str());
        for (auto& m : g_db->history(p.id, 20))
            printf("  %s  %s: %s\n", fmt_time(m.ts).c_str(), m.outgoing ? "я" : p.name.c_str(), m.body.c_str());
        return;
    }
    printf("[!] переписку з '%s' не знайдено\n", query.c_str());
}

static void on_stdin() {
    char buf[1024];
    ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
    if (r <= 0) {
        // stdin закрито (демон запущено без терміналу): просто не слухаємо його далі
        epoll_ctl(g_ep, EPOLL_CTL_DEL, STDIN_FILENO, nullptr);
        return;
    }
    std::string text(buf, r);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        trim_eol(line);
        if (line.empty()) continue;

        if (line == "/quit") {
            g_running = false;
        } else if (line == "/peers") {
            print_peers();
        } else if (line.rfind("/history", 0) == 0) {
            print_history(line.size() > 9 ? line.substr(9) : "");
        } else {
            std::vector<int> dead;
            int sent = 0;
            for (auto& [fd, p] : g_peers) {
                if (p.connecting || p.id.empty()) continue;
                if (send_text(fd, p, line)) {
                    ++sent;
                } else {
                    dead.push_back(fd);
                }
            }
            for (int fd : dead) close_peer(fd);
            if (sent == 0) printf("[!] немає підключених вузлів, повідомлення нікому не надіслано\n");
        }
    }
}

// ---------- Точка входу ----------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Використання: %s <порт> [ip порт_пира] [-n ім'я]\n", argv[0]);
        return 1;
    }
    g_port = atoi(argv[1]);

    std::vector<std::string> pos;
    std::string custom_name;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "-n" && i + 1 < argc) custom_name = argv[++i];
        else pos.push_back(argv[i]);
    }

    g_db = std::make_unique<Storage>(g_port);
    if (g_db->ok()) printf("[*] база історії: %s\n", g_db->path().c_str());
    // id зберігається в базі, тому не змінюється між запусками
    g_id = g_db->get_or_set("node_id", make_node_id());
    if (!custom_name.empty()) {
        g_name = custom_name;
    } else {
        passwd* pw = getpwuid(getuid());
        g_name = std::string(pw ? pw->pw_name : "user") + ":" + std::to_string(g_port);
    }

    // TCP: слухаючий сокет
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket");
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_port);
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(listen_fd, 16) < 0) die("listen");
    set_nonblocking(listen_fd);

    g_ep = epoll_create1(0);
    if (g_ep < 0) die("epoll_create1");
    epoll_add(listen_fd, EPOLLIN);
    {   // stdin може бути недоступний для epoll (наприклад, файл), тому без die
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = STDIN_FILENO;
        epoll_ctl(g_ep, EPOLL_CTL_ADD, STDIN_FILENO, &ev);
    }

    g_ipc.on_command = on_ipc_command;
    g_ipc.on_connect = on_ipc_connect;
    if (g_ipc.start(g_ep, IpcServer::default_path(g_port)))
        printf("[*] IPC-сокет для GUI: %s\n", g_ipc.path().c_str());
    else
        fprintf(stderr, "[!] не вдалося створити IPC-сокет, GUI не зможе підключитись\n");

    // Сигнали через файловий дескриптор: блокуємо звичайну обробку і читаємо з signalfd
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (sig_fd < 0) die("signalfd");
    epoll_add(sig_fd, EPOLLIN);

    // Таймер на 1 секунду (heartbeat, розсилка, очищення списків)
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (timer_fd < 0) die("timerfd_create");
    itimerspec its{};
    its.it_interval.tv_sec = 1;
    its.it_value.tv_sec = 1;
    if (timerfd_settime(timer_fd, 0, &its, nullptr) < 0) die("timerfd_settime");
    epoll_add(timer_fd, EPOLLIN);

    setup_discovery();

    // Ручне підключення, якщо вказано ip і порт
    if (pos.size() >= 2) {
        if (!start_connect(pos[0], atoi(pos[1].c_str()), "")) {
            fprintf(stderr, "Не вдалося почати підключення до %s:%s\n", pos[0].c_str(), pos[1].c_str());
            return 1;
        }
    }

    printf("Вузол '%s' (id %s) слухає порт %d.\n", g_name.c_str(), g_id.c_str(), g_port);
    printf("Команди: /peers — вузли, /history [ім'я] — історія, /quit — вихід. Усе інше надсилається всім.\n");
    send_beacon();

    epoll_event events[32];
    while (g_running) {
        int n = epoll_wait(g_ep, events, 32, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait");
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd) {
                int c = accept(listen_fd, nullptr, nullptr);
                if (c >= 0) {
                    set_nonblocking(c);
                    add_peer(c, false);
                    printf("[+] нове підключення (fd=%d)\n", c);
                    flush_peer(c, g_peers[c]);
                }
            } else if (fd == STDIN_FILENO) {
                on_stdin();
            } else if (g_ipc.owns(fd)) {
                g_ipc.handle_event(fd, ev);
            } else if (fd == g_udp_fd) {
                on_beacon();
            } else if (fd == timer_fd) {
                on_timer(timer_fd);
            } else if (fd == sig_fd) {
                signalfd_siginfo si;
                if (read(sig_fd, &si, sizeof(si)) > 0) {
                    printf("\n[!] отримано сигнал %u, завершення роботи...\n", si.ssi_signo);
                    g_running = false;
                }
            } else {
                if (g_peers.find(fd) == g_peers.end()) continue;  // уже закрито
                if (ev & EPOLLOUT) {
                    on_writable(fd);
                    if (g_peers.find(fd) == g_peers.end()) continue;
                }
                if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR)) on_readable(fd);
            }
        }
    }

    // Коректне завершення: закриваємо всі з'єднання та дескриптори
    while (!g_peers.empty()) close_peer(g_peers.begin()->first);
    g_ipc.stop();
    close(listen_fd);
    close(g_udp_fd);
    close(timer_fd);
    close(sig_fd);
    close(g_ep);
    printf("Вузол зупинено.\n");
    return 0;
}
