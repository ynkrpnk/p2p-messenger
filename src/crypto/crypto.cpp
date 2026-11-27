#include "crypto/crypto.h"

#include <sodium.h>

#include <cstring>

namespace crypto {

bool init() { return sodium_init() >= 0; }

KeyPair from_secret(const Key& sk) {
    KeyPair kp;
    kp.sk = sk;
    crypto_scalarmult_base(kp.pk.data(), kp.sk.data());
    return kp;
}

KeyPair generate() {
    Key sk;
    randombytes_buf(sk.data(), sk.size());
    return from_secret(sk);
}

Salt random_salt() {
    Salt s;
    randombytes_buf(s.data(), s.size());
    return s;
}

std::string to_hex(const uint8_t* data, size_t n) {
    std::string s(n * 2 + 1, '\0');
    sodium_bin2hex(&s[0], s.size(), data, n);
    s.resize(n * 2);
    return s;
}

bool from_hex(const std::string& hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2) return false;
    size_t len = 0;
    return sodium_hex2bin(out, n, hex.c_str(), hex.size(), nullptr, &len, nullptr) == 0 && len == n;
}

static void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(v & 0xff); v >>= 8; }
}

static uint64_t get_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

Session::~Session() { sodium_memzero(key_.data(), key_.size()); }

bool Session::setup(const KeyPair& me, const Key& peer_pk, const Salt& my_salt, const Salt& peer_salt) {
    if (me.pk == peer_pk) return false;  // з'єднання із самим собою

    Key shared;
    if (crypto_box_beforenm(shared.data(), peer_pk.data(), me.sk.data()) != 0) return false;

    // Обидві сторони впорядковують солі однаково: спершу сіль вузла з меншим ключем
    const bool i_am_low = std::memcmp(me.pk.data(), peer_pk.data(), KEY_SIZE) < 0;
    const Salt& lo = i_am_low ? my_salt : peer_salt;
    const Salt& hi = i_am_low ? peer_salt : my_salt;

    static const char label[] = "p2pm-session-v1";
    uint8_t input[sizeof(label) - 1 + 2 * SALT_SIZE];
    std::memcpy(input, label, sizeof(label) - 1);
    std::memcpy(input + sizeof(label) - 1, lo.data(), SALT_SIZE);
    std::memcpy(input + sizeof(label) - 1 + SALT_SIZE, hi.data(), SALT_SIZE);
    crypto_generichash(key_.data(), key_.size(), input, sizeof(input), shared.data(), shared.size());
    sodium_memzero(shared.data(), shared.size());

    my_dir_ = i_am_low ? 0 : 1;  // напрямок входить у nonce, щоб сторони не повторювали nonce
    send_ctr_ = recv_ctr_ = 0;
    ready_ = true;
    return true;
}

static void make_nonce(uint8_t nonce[crypto_secretbox_NONCEBYTES], uint8_t dir, uint64_t ctr) {
    std::memset(nonce, 0, crypto_secretbox_NONCEBYTES);
    nonce[0] = dir;
    put_be64(nonce + 16, ctr);
}

std::string Session::seal(const std::string& plain) {
    uint8_t nonce[crypto_secretbox_NONCEBYTES];
    make_nonce(nonce, my_dir_, send_ctr_);

    std::string out(8 + crypto_secretbox_MACBYTES + plain.size(), '\0');
    put_be64(reinterpret_cast<uint8_t*>(&out[0]), send_ctr_);
    crypto_secretbox_easy(reinterpret_cast<uint8_t*>(&out[8]),
                          reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), nonce, key_.data());
    ++send_ctr_;
    return out;
}

bool Session::open(const std::string& data, std::string& plain) {
    if (!ready_ || data.size() < 8 + crypto_secretbox_MACBYTES) return false;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(data.data());

    // Лічильник має йти строго по порядку: повтор або пропуск = помилка
    const uint64_t ctr = get_be64(d);
    if (ctr != recv_ctr_) return false;

    uint8_t nonce[crypto_secretbox_NONCEBYTES];
    make_nonce(nonce, 1 - my_dir_, ctr);

    plain.assign(data.size() - 8 - crypto_secretbox_MACBYTES, '\0');
    if (crypto_secretbox_open_easy(reinterpret_cast<uint8_t*>(&plain[0]), d + 8, data.size() - 8,
                                   nonce, key_.data()) != 0) {
        plain.clear();
        return false;
    }
    ++recv_ctr_;
    return true;
}

}  // namespace crypto
