#include "config/config.h"

#include <pwd.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>

static void make_dir(const std::string& p) {
    if (mkdir(p.c_str(), 0700) < 0 && errno != EEXIST) perror(("mkdir " + p).c_str());
}

std::string config_path(int port) {
    std::string base;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = getenv("HOME");
        if (!home || !*home) {
            passwd* pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        base = std::string(home) + "/.config";
    }
    return base + "/p2pm/node-" + std::to_string(port) + ".conf";
}

static std::string dir_of(const std::string& path) { return path.substr(0, path.rfind('/')); }

void ensure_default_config(const std::string& path) {
    std::string dir = dir_of(path);
    make_dir(dir.substr(0, dir.rfind('/')));  // ~/.config
    make_dir(dir);
    if (access(path.c_str(), F_OK) == 0) return;
    std::ofstream f(path);
    f << "# Конфігурація вузла p2pd. Зміни застосовуються автоматично, без перезапуску.\n"
         "# name=Ім'я вузла\n"
         "ping_interval=5\n"
         "peer_timeout=15\n"
         "beacon_interval=5\n";
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parse_int(const std::string& v, int lo, int hi, int& out) {
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    if (v.empty() || *end != '\0' || n < lo || n > hi) return false;
    out = static_cast<int>(n);
    return true;
}

bool load_config(const std::string& path, Config& out, std::string& errors) {
    std::ifstream f(path);
    if (!f) {
        errors = "не вдалося відкрити " + path;
        return false;
    }
    Config c;
    errors.clear();
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            errors += "рядок " + std::to_string(lineno) + ": очікується ключ=значення; ";
            continue;
        }
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        bool ok = true;
        if (key == "name") {
            ok = val.size() <= 64 && val.find('|') == std::string::npos;
            if (ok) c.name = val;
        } else if (key == "ping_interval") ok = parse_int(val, 1, 60, c.ping_interval);
        else if (key == "peer_timeout") ok = parse_int(val, 3, 300, c.peer_timeout);
        else if (key == "beacon_interval") ok = parse_int(val, 1, 60, c.beacon_interval);
        else {
            errors += "рядок " + std::to_string(lineno) + ": невідомий ключ '" + key + "'; ";
            continue;
        }
        if (!ok) errors += "рядок " + std::to_string(lineno) + ": недопустиме значення '" + key + "'; ";
    }
    if (c.peer_timeout < 2 * c.ping_interval)
        errors += "peer_timeout має бути не менше ніж 2*ping_interval; ";
    if (!errors.empty()) return false;
    out = c;
    return true;
}

// ---------- inotify ----------
ConfigWatcher::~ConfigWatcher() {
    if (fd_ >= 0) close(fd_);
}

bool ConfigWatcher::start(int epoll_fd, const std::string& path) {
    // Стежимо за каталогом, а не за файлом: редактори часто замінюють файл через rename
    fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0) return false;
    if (inotify_add_watch(fd_, dir_of(path).c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    name_ = path.substr(path.rfind('/') + 1);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd_;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd_, &ev);
    return true;
}

bool ConfigWatcher::handle_event() {
    bool changed = false;
    alignas(inotify_event) char buf[4096];
    while (true) {
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            if (ev->len > 0 && name_ == ev->name) changed = true;
            p += sizeof(inotify_event) + ev->len;
        }
    }
    return changed;
}
