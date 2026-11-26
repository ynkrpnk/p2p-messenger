#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// IPC-сервер демона: UNIX domain socket, текстовий протокол.
// Одне повідомлення = один рядок, поля розділені символом табуляції.
// У полях \ \n \t екрануються як \\ \n \t.
class IpcServer {
public:
    using CommandHandler = std::function<void(int client, const std::vector<std::string>& args)>;
    using ConnectHandler = std::function<void(int client)>;

    ~IpcServer() { stop(); }

    // Створює сокет, реєструє його в epoll. false = не вдалося.
    bool start(int epoll_fd, const std::string& path);
    void stop();

    bool owns(int fd) const { return fd == listen_fd_ || clients_.count(fd) > 0; }
    void handle_event(int fd, uint32_t events);

    void send(int client, const std::vector<std::string>& fields);
    void broadcast(const std::vector<std::string>& fields);

    const std::string& path() const { return path_; }
    static std::string default_path(int port);

    CommandHandler on_command;
    ConnectHandler on_connect;

private:
    struct Client {
        std::string in, out;
        bool want_out = false;
    };
    void accept_clients();
    void read_client(int fd);
    bool flush_client(int fd);
    void drop_client(int fd);

    int ep_ = -1;
    int listen_fd_ = -1;
    std::string path_;
    std::map<int, Client> clients_;
};
