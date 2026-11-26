#pragma once
#include <ctime>
#include <string>
#include <vector>

struct sqlite3;  // щоб не підключати sqlite3.h у кожному файлі

// Повідомлення з історії
struct StoredMessage {
    bool outgoing;     // true = ми відправили, false = нам надіслали
    std::string body;
    time_t ts;         // час (unix time)
};

// Вузол, з яким колись спілкувалися
struct StoredPeer {
    std::string id;
    std::string name;
    time_t last_seen;
    int messages;      // скільки повідомлень у переписці
};

// Локальне сховище на SQLite. Один файл на вузол:
// ~/.local/share/p2pm/node-<порт>.db
// Якщо базу відкрити не вдалося, усі методи просто нічого не роблять.
class Storage {
public:
    explicit Storage(int port);
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    bool ok() const { return db_ != nullptr; }
    const std::string& path() const { return path_; }

    // Значення з таблиці meta; якщо ключа немає, записує і повертає def
    std::string get_or_set(const std::string& key, const std::string& def);

    void touch_peer(const std::string& id, const std::string& name);
    void save_message(const std::string& peer_id, bool outgoing, const std::string& body);
    std::vector<StoredMessage> history(const std::string& peer_id, int limit = 20);
    std::vector<StoredPeer> peers();

private:
    bool exec(const char* sql);
    sqlite3* db_ = nullptr;
    std::string path_;
};
