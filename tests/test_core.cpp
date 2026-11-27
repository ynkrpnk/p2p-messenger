// Модульні тести протоколу та шифрування. Запуск: ./build/p2p-tests (або ctest)
#include <cstdio>
#include <string>

#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include "config/config.h"
#include "crypto/crypto.h"
#include "protocol/packet.h"

static int g_failed = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) { printf("  ПРОВАЛ: %s (рядок %d)\n", #cond, __LINE__); ++g_failed; } \
    } while (0)

static void test_packet_roundtrip() {
    printf("packet: кодування і розбір\n");
    std::string buf = encode_packet(PacketType::Text, "привіт");
    Packet p;
    CHECK(try_decode(buf, p) == ParseResult::Ok);
    CHECK(p.type == PacketType::Text && p.body == "привіт" && buf.empty());
}

static void test_packet_fragmentation() {
    printf("packet: фрагментація і склеювання TCP-потоку\n");
    std::string wire = encode_packet(PacketType::Text, "one") + encode_packet(PacketType::Ping, "");
    std::string buf;
    int got = 0;
    for (char c : wire) {  // подаємо по одному байту
        buf.push_back(c);
        Packet p;
        while (try_decode(buf, p) == ParseResult::Ok) ++got;
    }
    CHECK(got == 2);

    std::string glued = encode_packet(PacketType::Text, "a") + encode_packet(PacketType::Text, "b");
    Packet p;
    CHECK(try_decode(glued, p) == ParseResult::Ok && p.body == "a");
    CHECK(try_decode(glued, p) == ParseResult::Ok && p.body == "b");
    CHECK(try_decode(glued, p) == ParseResult::NeedMore);
}

static void test_packet_too_big() {
    printf("packet: захист від завеликої довжини\n");
    std::string buf = encode_packet(PacketType::Text, "x");
    buf[1] = buf[2] = buf[3] = buf[4] = '\xff';  // довжина 4 ГБ
    Packet p;
    CHECK(try_decode(buf, p) == ParseResult::Error);
}

struct Pair {
    crypto::KeyPair a = crypto::generate(), b = crypto::generate();
    crypto::Salt sa = crypto::random_salt(), sb = crypto::random_salt();
    crypto::Session A, B;
    Pair() {
        CHECK(A.setup(a, b.pk, sa, sb));
        CHECK(B.setup(b, a.pk, sb, sa));
    }
};

static void test_crypto_roundtrip() {
    printf("crypto: шифрування в обидва боки\n");
    Pair t;
    std::string out;
    CHECK(t.B.open(t.A.seal("привіт, Боб"), out) && out == "привіт, Боб");
    CHECK(t.A.open(t.B.seal("привіт, Аліса"), out) && out == "привіт, Аліса");
    CHECK(t.B.open(t.A.seal(""), out) && out.empty());
    std::string sealed = t.A.seal("secret-text");
    CHECK(sealed.find("secret-text") == std::string::npos);  // у каналі немає відкритого тексту
}

static void test_crypto_tamper_replay() {
    printf("crypto: підміна, повтор, чужий ключ\n");
    Pair t;
    std::string out;
    std::string m1 = t.A.seal("one");
    std::string bad = m1;
    bad.back() ^= 1;
    CHECK(!t.B.open(bad, out));          // змінений шифротекст
    CHECK(t.B.open(m1, out));            // оригінал проходить
    CHECK(!t.B.open(m1, out));           // повтор відхиляється
    std::string m2 = t.A.seal("two"), m3 = t.A.seal("three");
    CHECK(!t.B.open(m3, out));           // порушення порядку
    CHECK(t.B.open(m2, out));

    crypto::KeyPair eve = crypto::generate();
    crypto::Session E;
    CHECK(E.setup(eve, t.a.pk, crypto::random_salt(), t.sa));
    CHECK(!E.open(t.A.seal("x"), out));  // третя сторона не може розшифрувати
}

static void test_crypto_sessions_independent() {
    printf("crypto: різні сеанси мають різні ключі\n");
    crypto::KeyPair a = crypto::generate(), b = crypto::generate();
    crypto::Session A1, B1, B2;
    crypto::Salt s1 = crypto::random_salt(), s2 = crypto::random_salt(), s3 = crypto::random_salt();
    CHECK(A1.setup(a, b.pk, s1, s2));
    CHECK(B1.setup(b, a.pk, s2, s1));
    CHECK(B2.setup(b, a.pk, s3, s1));  // новий сеанс з іншою сіллю
    std::string out, m = A1.seal("hello");
    CHECK(!B2.open(m, out));           // запис старого сеансу не діє в новому
    CHECK(B1.open(m, out));
}

static void test_hex() {
    printf("crypto: hex-перетворення\n");
    crypto::KeyPair k = crypto::generate();
    std::string h = crypto::to_hex(k.pk.data(), k.pk.size());
    crypto::Key back;
    CHECK(h.size() == 64 && crypto::from_hex(h, back.data(), back.size()) && back == k.pk);
    CHECK(!crypto::from_hex("zz", back.data(), back.size()));
}

static bool write_and_load(const std::string& text, Config& c, std::string& err) {
    char path[] = "/tmp/p2pm-test-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    { std::ofstream(path) << text; }
    bool ok = load_config(path, c, err);
    unlink(path);
    return ok;
}

static void test_config() {
    printf("config: розбір і перевірка файлу\n");
    Config c;
    std::string err;
    CHECK(write_and_load("# коментар\nname = Аліса\nping_interval=2\npeer_timeout=10\nbeacon_interval=3\n", c, err));
    CHECK(c.name == "Аліса" && c.ping_interval == 2 && c.peer_timeout == 10 && c.beacon_interval == 3);
    CHECK(!write_and_load("ping_interval=abc\n", c, err));        // не число
    CHECK(!write_and_load("ping_interval=0\n", c, err));          // поза діапазоном
    CHECK(!write_and_load("something=1\n", c, err));              // невідомий ключ
    CHECK(!write_and_load("ping_interval=10\npeer_timeout=12\n", c, err));  // timeout < 2*ping
    CHECK(!write_and_load("name=a|b\n", c, err));                 // заборонений символ
    CHECK(write_and_load("", c, err) && c.ping_interval == 5);     // порожній файл = типові значення
}

int main() {
    if (!crypto::init()) { printf("sodium_init не вдалося\n"); return 2; }
    test_packet_roundtrip();
    test_packet_fragmentation();
    test_packet_too_big();
    test_crypto_roundtrip();
    test_crypto_tamper_replay();
    test_crypto_sessions_independent();
    test_hex();
    test_config();
    printf(g_failed ? "\nПРОВАЛЕНО перевірок: %d\n" : "\nУсі тести пройдено\n", g_failed);
    return g_failed ? 1 : 0;
}
