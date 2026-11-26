#include "ipc/ipc.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') continue;
        else o += c;
    }
    return o;
}

static std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            o += (s[i] == 'n') ? '\n' : (s[i] == 't') ? '\t' : s[i];
        } else {
            o += s[i];
        }
    }
    return o;
}

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

std::string IpcServer::default_path(int port) {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) return std::string(rt) + "/p2pm-" + std::to_string(port) + ".sock";
    return "/tmp/p2pm-" + std::to_string(getuid()) + "-" + std::to_string(port) + ".sock";
}

bool IpcServer::start(int epoll_fd, const std::string& path) {
    sockaddr_un a{};
    if (path.size() >= sizeof(a.sun_path)) return false;

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);

    unlink(path.c_str());  // прибираємо залишки після аварійного завершення
    if (bind(listen_fd_, (sockaddr*)&a, sizeof(a)) < 0 || listen(listen_fd_, 8) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    chmod(path.c_str(), 0600);  // доступ лише власнику
    set_nonblocking(listen_fd_);

    ep_ = epoll_fd;
    path_ = path;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    epoll_ctl(ep_, EPOLL_CTL_ADD, listen_fd_, &ev);
    return true;
}

void IpcServer::stop() {
    while (!clients_.empty()) drop_client(clients_.begin()->first);
    if (listen_fd_ >= 0) {
        epoll_ctl(ep_, EPOLL_CTL_DEL, listen_fd_, nullptr);
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(path_.c_str());
    }
}

void IpcServer::drop_client(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    clients_.erase(it);
}

void IpcServer::accept_clients() {
    while (true) {
        int c = accept(listen_fd_, nullptr, nullptr);
        if (c < 0) break;
        set_nonblocking(c);
        clients_[c];
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = c;
        epoll_ctl(ep_, EPOLL_CTL_ADD, c, &ev);
        if (on_connect) on_connect(c);
    }
}

bool IpcServer::flush_client(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return false;
    Client& c = it->second;
    while (!c.out.empty()) {
        ssize_t n = ::send(fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) c.out.erase(0, n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else return false;
    }
    bool need = !c.out.empty();
    if (need != c.want_out) {
        c.want_out = need;
        epoll_event ev{};
        ev.events = EPOLLIN | (need ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.fd = fd;
        epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
    }
    return true;
}

void IpcServer::send(int client, const std::vector<std::string>& fields) {
    auto it = clients_.find(client);
    if (it == clients_.end()) return;
    std::string line;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) line += '\t';
        line += esc(fields[i]);
    }
    line += '\n';
    it->second.out += line;
    if (!flush_client(client)) drop_client(client);
}

void IpcServer::broadcast(const std::vector<std::string>& fields) {
    std::vector<int> fds;
    for (auto& [fd, c] : clients_) fds.push_back(fd);
    for (int fd : fds) send(fd, fields);
}

void IpcServer::read_client(int fd) {
    bool closed = false;
    while (true) {
        char tmp[4096];
        ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
        auto it = clients_.find(fd);
        if (it == clients_.end()) return;
        if (r > 0) it->second.in.append(tmp, r);
        else if (r == 0) { closed = true; break; }
        else if (errno == EINTR) continue;
        else { if (errno != EAGAIN && errno != EWOULDBLOCK) closed = true; break; }
    }

    // Обробляємо всі повні рядки; після кожного колбека клієнт міг зникнути
    while (true) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) return;
        if (it->second.in.size() > (1u << 20)) { closed = true; break; }
        size_t nl = it->second.in.find('\n');
        if (nl == std::string::npos) break;
        std::string line = it->second.in.substr(0, nl);
        it->second.in.erase(0, nl + 1);
        if (line.empty()) continue;

        std::vector<std::string> args;
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            args.push_back(unesc(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (on_command) on_command(fd, args);
    }
    if (closed) drop_client(fd);
}

void IpcServer::handle_event(int fd, uint32_t events) {
    if (fd == listen_fd_) { accept_clients(); return; }
    if (events & EPOLLOUT) {
        if (!flush_client(fd)) { drop_client(fd); return; }
    }
    if (events & (EPOLLIN | EPOLLHUP | EPOLLERR)) read_client(fd);
}
