#pragma once
#include <array>
#include <cstdint>
#include <string>

// Шифрування на libsodium.
//  * Ідентифікатор вузла = публічний ключ X25519 (hex). Підробити чужий id не можна:
//    без відповідного секретного ключа повідомлення не пройдуть перевірку.
//  * Для кожного з'єднання виводиться сеансовий ключ: спільний секрет X25519
//    (crypto_box_beforenm), змішаний із випадковими "солями" обох сторін (BLAKE2b).
//    Тому перехоплений трафік одного сеансу не можна повторити в іншому.
//  * Повідомлення шифруються XSalsa20-Poly1305 (crypto_secretbox) із лічильником
//    у nonce: підміна, повтор і перестановка повідомлень виявляються.
namespace crypto {

constexpr size_t KEY_SIZE = 32;
constexpr size_t SALT_SIZE = 16;
using Key = std::array<uint8_t, KEY_SIZE>;
using Salt = std::array<uint8_t, SALT_SIZE>;

struct KeyPair {
    Key pk{};  // публічний ключ (він же id вузла)
    Key sk{};  // секретний ключ
};

bool init();                       // sodium_init(); false = бібліотека не працює
KeyPair generate();                // нова випадкова пара ключів
KeyPair from_secret(const Key& sk);  // відновлює пару за секретним ключем
Salt random_salt();

std::string to_hex(const uint8_t* data, size_t n);
bool from_hex(const std::string& hex, uint8_t* out, size_t n);

// Захищений канал з одним вузлом
class Session {
public:
    Session() = default;
    ~Session();

    // Викликається, коли відомі ключі та солі обох сторін. false = недійсний ключ.
    bool setup(const KeyPair& me, const Key& peer_pk, const Salt& my_salt, const Salt& peer_salt);
    bool ready() const { return ready_; }

    // Формат: [8 байт: лічильник][MAC 16 байт + шифротекст]
    std::string seal(const std::string& plain);
    bool open(const std::string& data, std::string& plain);

private:
    Key key_{};
    uint8_t my_dir_ = 0;
    uint64_t send_ctr_ = 0;
    uint64_t recv_ctr_ = 0;
    bool ready_ = false;
};

}  // namespace crypto
