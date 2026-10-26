#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void die(const char* msg) {
    perror(msg);
    exit(1);
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        die("fcntl");
}

static void epoll_add(int ep, int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0)
        die("epoll_ctl");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Использование: %s <порт> [ip порт_пира]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

    // 1. Слушающий TCP-сокет
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket");

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(listen_fd, 16) < 0) die("listen");
    set_nonblocking(listen_fd);

    // 2. epoll: следим за listen-сокетом и stdin
    int ep = epoll_create1(0);
    if (ep < 0) die("epoll_create1");
    epoll_add(ep, listen_fd);
    epoll_add(ep, STDIN_FILENO);

    std::vector<int> peers;

    // 3. Если указан пир, подключаемся к нему
    if (argc >= 4) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(atoi(argv[3]));
        inet_pton(AF_INET, argv[2], &peer.sin_addr);
        if (connect(fd, (sockaddr*)&peer, sizeof(peer)) < 0) die("connect");
        set_nonblocking(fd);
        epoll_add(ep, fd);
        peers.push_back(fd);
        printf("[+] подключились к %s:%s\n", argv[2], argv[3]);
    }

    printf("Узел слушает порт %d. Пиши сообщения и жми Enter.\n", port);

    // 4. Главный цикл событий
    epoll_event events[32];
    while (true) {
        int n = epoll_wait(ep, events, 32, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait");
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                int c = accept(listen_fd, nullptr, nullptr);
                if (c < 0) continue;
                set_nonblocking(c);
                epoll_add(ep, c);
                peers.push_back(c);
                printf("[+] новое подключение (fd=%d)\n", c);

            } else if (fd == STDIN_FILENO) {
                char buf[1024];
                ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
                if (r <= 0) return 0;
                for (int p : peers) send(p, buf, r, MSG_NOSIGNAL);

            } else {
                char buf[1024];
                ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
                if (r <= 0) {
                    printf("[-] пир отключился (fd=%d)\n", fd);
                    epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    peers.erase(std::remove(peers.begin(), peers.end(), fd), peers.end());
                } else {
                    buf[r] = '\0';
                    printf("[fd=%d] %s", fd, buf);
                }
            }
        }
    }
}
