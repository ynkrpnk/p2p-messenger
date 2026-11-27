#pragma once
#include <string>

// Налаштування вузла з файлу ~/.config/p2pm/node-<порт>.conf (формат ключ=значення).
// Файл відстежується через inotify, тому зміни застосовуються без перезапуску.
struct Config {
    std::string name;         // ім'я вузла (порожнє = типове "користувач:порт")
    int ping_interval = 5;    // секунди між Ping
    int peer_timeout = 15;    // скільки секунд мовчання до розриву з'єднання
    int beacon_interval = 5;  // секунди між UDP-розсилками "я тут"
};

std::string config_path(int port);

// Створює каталог (mkdir) і файл із типовими значеннями, якщо їх ще немає
void ensure_default_config(const std::string& path);

// Читає та перевіряє файл. false = конфігурацію не прийнято, причини в errors.
bool load_config(const std::string& path, Config& out, std::string& errors);

// Спостереження за каталогом конфігурації через inotify
class ConfigWatcher {
public:
    ~ConfigWatcher();
    bool start(int epoll_fd, const std::string& path);
    int fd() const { return fd_; }
    // Читає накопичені події; true = наш файл змінився
    bool handle_event();

private:
    int fd_ = -1;
    std::string name_;  // ім'я файлу без каталогу
};
