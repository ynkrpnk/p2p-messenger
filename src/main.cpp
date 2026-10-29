#include "protocol/packet.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>

// Виведення повідомлення про помилку та завершення програми
static void die(const char* msg) {
    perror(msg);
    exit(1);
}

// Переведення сокета в неблокуючий режим
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        die("fcntl");
}

// Додавання сокета до списку відстеження подій epoll
static void epoll_add(int ep, int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0)
        die("epoll_ctl");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Використання: %s <порт> [ip порт_пира]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

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

    int ep = epoll_create1(0);
    if (ep < 0) die("epoll_create1");
    epoll_add(ep, listen_fd);
    epoll_add(ep, STDIN_FILENO);

    // Використовуємо map замість vector: дескриптор сокета -> буфер приймання
    std::map<int, std::string> peers;

    if (argc >= 4) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(atoi(argv[3]));
        inet_pton(AF_INET, argv[2], &peer.sin_addr);
        if (connect(fd, (sockaddr*)&peer, sizeof(peer)) < 0) die("connect");
        set_nonblocking(fd);
        epoll_add(ep, fd);

        peers[fd] = ""; // Створюємо порожній буфер для приймання даних
        printf("[+] підключилися до %s:%s\n", argv[2], argv[3]);

        // Одразу надсилаємо вітальний пакет
        std::string hello_pkt = encode_packet(PacketType::Hello, "Node " + std::to_string(port));
        send(fd, hello_pkt.data(), hello_pkt.size(), MSG_NOSIGNAL);
    }

    printf("Вузол прослуховує порт %d. Вводь повідомлення та натискай Enter.\n", port);

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
                peers[c] = ""; // Створюємо порожній буфер для нового клієнта
                printf("[+] нове підключення (fd=%d)\n", c);

            } else if (fd == STDIN_FILENO) {
                char buf[1024];
                ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
                if (r <= 0) return 0;

                // Перетворюємо введений текст на мережевий пакет
                std::string text(buf, r);
                std::string pkt = encode_packet(PacketType::Text, text);

                // Надсилаємо пакет усім підключеним вузлам
                for (auto& [p_fd, _] : peers) {
                    send(p_fd, pkt.data(), pkt.size(), MSG_NOSIGNAL);
                }

            } else {
                bool disconnected = false;

                // Читаємо дані в циклі до появи EAGAIN (для роботи з неблокуючими сокетами)
                while (true) {
                    char tmp[4096];
                    ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
                    if (r > 0) {
                        peers[fd].append(tmp, r); // Додаємо отримані дані до буфера
                    } else if (r == 0) {
                        disconnected = true;
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        disconnected = true;
                        break;
                    }
                }

                if (disconnected) {
                    printf("[-] вузол відключився (fd=%d)\n", fd);
                    epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    peers.erase(fd);
                } else {
                    // Розбираємо всі повністю отримані пакети, які накопичилися в буфері
                    Packet p;
                    ParseResult res;
                    while ((res = try_decode(peers[fd], p)) == ParseResult::Ok) {
                        if (p.type == PacketType::Text) {
                            printf("[fd=%d] ПОВІДОМЛЕННЯ: %s", fd, p.body.c_str());
                        } else if (p.type == PacketType::Hello) {
                            printf("[fd=%d] ПРИВІТАННЯ: %s\n", fd, p.body.c_str());
                        }
                    }

                    // Якщо отримано некоректні дані (розмір перевищує 1 МБ), розриваємо з'єднання
                    if (res == ParseResult::Error) {
                        printf("[-] помилка протоколу, відключення (fd=%d)\n", fd);
                        epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        peers.erase(fd);
                    }
                }
            }
        }
    }
}
