#include "storage/storage.h"

#include <pwd.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

// Створюємо каталог через системний виклик mkdir (EEXIST — не помилка)
static void make_dir(const std::string& p) {
    if (mkdir(p.c_str(), 0700) < 0 && errno != EEXIST) perror(("mkdir " + p).c_str());
}

static std::string data_dir() {
    std::string base;
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = getenv("HOME");
        if (!home || !*home) {
            passwd* pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        base = std::string(home) + "/.local";
        make_dir(base);
        base += "/share";
    }
    make_dir(base);
    base += "/p2pm";
    make_dir(base);
    return base;
}

Storage::Storage(int port) {
    path_ = data_dir() + "/node-" + std::to_string(port) + ".db";
    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        fprintf(stderr, "[!] не вдалося відкрити базу %s: %s\n", path_.c_str(),
                db_ ? sqlite3_errmsg(db_) : "?");
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    sqlite3_busy_timeout(db_, 2000);
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("CREATE TABLE IF NOT EXISTS meta ("
         " key TEXT PRIMARY KEY, value TEXT NOT NULL);");
    exec("CREATE TABLE IF NOT EXISTS peers ("
         " id TEXT PRIMARY KEY, name TEXT, last_seen INTEGER);");
    exec("CREATE TABLE IF NOT EXISTS messages ("
         " id INTEGER PRIMARY KEY AUTOINCREMENT,"
         " peer_id TEXT NOT NULL, outgoing INTEGER NOT NULL,"
         " body TEXT NOT NULL, ts INTEGER NOT NULL);");
    exec("CREATE INDEX IF NOT EXISTS idx_msg_peer ON messages(peer_id, id);");
}

Storage::~Storage() {
    if (db_) sqlite3_close(db_);
}

bool Storage::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        fprintf(stderr, "[!] SQLite: %s\n", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

std::string Storage::get_or_set(const std::string& key, const std::string& def) {
    if (!db_) return def;
    sqlite3_stmt* st = nullptr;
    std::string result;
    bool found = false;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            result = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            found = true;
        }
    }
    sqlite3_finalize(st);
    if (found) return result;

    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, def.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return def;
}

void Storage::touch_peer(const std::string& id, const std::string& name) {
    if (!db_ || id.empty()) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO peers(id, name, last_seen) VALUES(?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET name=excluded.name, last_seen=excluded.last_seen",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(time(nullptr)));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

void Storage::save_message(const std::string& peer_id, bool outgoing, const std::string& body) {
    if (!db_ || peer_id.empty()) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO messages(peer_id, outgoing, body, ts) VALUES(?, ?, ?, ?)",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, peer_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, outgoing ? 1 : 0);
        sqlite3_bind_text(st, 3, body.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, static_cast<sqlite3_int64>(time(nullptr)));
        if (sqlite3_step(st) != SQLITE_DONE) fprintf(stderr, "[!] SQLite: %s\n", sqlite3_errmsg(db_));
    }
    sqlite3_finalize(st);
}

std::vector<StoredMessage> Storage::history(const std::string& peer_id, int limit) {
    std::vector<StoredMessage> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT outgoing, body, ts FROM messages WHERE peer_id=? ORDER BY id DESC LIMIT ?",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, peer_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            StoredMessage m;
            m.outgoing = sqlite3_column_int(st, 0) != 0;
            m.body = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            m.ts = static_cast<time_t>(sqlite3_column_int64(st, 2));
            out.push_back(std::move(m));
        }
    }
    sqlite3_finalize(st);
    std::reverse(out.begin(), out.end());  // від старих до нових
    return out;
}

std::vector<StoredPeer> Storage::peers() {
    std::vector<StoredPeer> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT p.id, p.name, p.last_seen, "
            "(SELECT COUNT(*) FROM messages m WHERE m.peer_id = p.id) "
            "FROM peers p ORDER BY p.last_seen DESC",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            StoredPeer p;
            p.id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            const unsigned char* nm = sqlite3_column_text(st, 1);
            p.name = nm ? reinterpret_cast<const char*>(nm) : "";
            p.last_seen = static_cast<time_t>(sqlite3_column_int64(st, 2));
            p.messages = sqlite3_column_int(st, 3);
            out.push_back(std::move(p));
        }
    }
    sqlite3_finalize(st);
    return out;
}
