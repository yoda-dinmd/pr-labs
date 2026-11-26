#include "Crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>
#include <stdexcept>

namespace chat {

namespace {

constexpr std::string_view kBase64Alphabet{
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
};

constexpr int decode_base64_char(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

using EvpCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

} // namespace

Crypto::Crypto(std::string_view passphrase)
    : key_{derive_key(passphrase)} {}

const std::array<std::uint8_t, kAesKeyBytes>& Crypto::key() const noexcept {
    return key_;
}

std::array<std::uint8_t, kAesKeyBytes> Crypto::derive_key(std::string_view passphrase) {
    std::array<std::uint8_t, kAesKeyBytes> derived{};
    const int ok{PKCS5_PBKDF2_HMAC(
        passphrase.data(),
        static_cast<int>(passphrase.size()),
        reinterpret_cast<const unsigned char*>(kKdfSalt.data()),
        static_cast<int>(kKdfSalt.size()),
        kPbkdf2Iterations,
        EVP_sha256(),
        static_cast<int>(derived.size()),
        derived.data()
    )};
    if (ok != 1) {
        throw std::runtime_error{"PBKDF2 key derivation failed"};
    }
    return derived;
}

std::optional<EncryptedPayload> Crypto::encrypt(std::string_view plaintext) const {
    std::array<std::uint8_t, kGcmIvBytes> iv{};
    if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) {
        return std::nullopt;
    }

    EvpCtxPtr ctx{EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free};
    if (!ctx) {
        return std::nullopt;
    }

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return std::nullopt;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1) {
        return std::nullopt;
    }
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key_.data(), iv.data()) != 1) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> output{};
    output.resize(plaintext.size() + kGcmTagBytes);

    int out_len{0};
    if (!plaintext.empty()) {
        if (EVP_EncryptUpdate(
                ctx.get(),
                output.data(),
                &out_len,
                reinterpret_cast<const unsigned char*>(plaintext.data()),
                static_cast<int>(plaintext.size())) != 1) {
            return std::nullopt;
        }
    }

    int final_len{0};
    if (EVP_EncryptFinal_ex(ctx.get(), output.data() + out_len, &final_len) != 1) {
        return std::nullopt;
    }

    const std::size_t ciphertext_len{static_cast<std::size_t>(out_len + final_len)};
    if (EVP_CIPHER_CTX_ctrl(
            ctx.get(),
            EVP_CTRL_GCM_GET_TAG,
            static_cast<int>(kGcmTagBytes),
            output.data() + ciphertext_len) != 1) {
        return std::nullopt;
    }

    output.resize(ciphertext_len + kGcmTagBytes);

    return EncryptedPayload{
        base64_encode(output),
        base64_encode(iv)
    };
}

std::optional<std::string> Crypto::decrypt(std::string_view payload_b64,
                                           std::string_view iv_b64) const {
    const auto iv_bytes{base64_decode(iv_b64)};
    if (!iv_bytes || iv_bytes->size() != kGcmIvBytes) {
        return std::nullopt;
    }

    const auto payload_bytes{base64_decode(payload_b64)};
    if (!payload_bytes || payload_bytes->size() < kGcmTagBytes) {
        return std::nullopt;
    }

    const std::size_t ciphertext_len{payload_bytes->size() - kGcmTagBytes};
    const std::uint8_t* tag_ptr{payload_bytes->data() + ciphertext_len};

    EvpCtxPtr ctx{EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free};
    if (!ctx) {
        return std::nullopt;
    }

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return std::nullopt;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kGcmIvBytes), nullptr) != 1) {
        return std::nullopt;
    }
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key_.data(), iv_bytes->data()) != 1) {
        return std::nullopt;
    }

    std::string plaintext{};
    plaintext.resize(ciphertext_len);

    int out_len{0};
    if (ciphertext_len > 0) {
        if (EVP_DecryptUpdate(
                ctx.get(),
                reinterpret_cast<unsigned char*>(plaintext.data()),
                &out_len,
                payload_bytes->data(),
                static_cast<int>(ciphertext_len)) != 1) {
            return std::nullopt;
        }
    }

    if (EVP_CIPHER_CTX_ctrl(
            ctx.get(),
            EVP_CTRL_GCM_SET_TAG,
            static_cast<int>(kGcmTagBytes),
            const_cast<std::uint8_t*>(tag_ptr)) != 1) {
        return std::nullopt;
    }

    int final_len{0};
    if (EVP_DecryptFinal_ex(
            ctx.get(),
            reinterpret_cast<unsigned char*>(plaintext.data()) + out_len,
            &final_len) <= 0) {
        return std::nullopt;
    }

    plaintext.resize(static_cast<std::size_t>(out_len + final_len));
    return plaintext;
}

std::string Crypto::base64_encode(std::span<const std::uint8_t> data) {
    if (data.empty()) {
        return {};
    }

    std::string encoded{};
    encoded.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i{0};
    while (i + 3 <= data.size()) {
        const std::uint32_t triple{
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8) |
            static_cast<std::uint32_t>(data[i + 2])
        };
        encoded.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
        encoded.push_back(kBase64Alphabet[triple & 0x3F]);
        i += 3;
    }

    const std::size_t remaining{data.size() - i};
    if (remaining == 1) {
        const std::uint32_t triple{static_cast<std::uint32_t>(data[i]) << 16};
        encoded.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        encoded.push_back('=');
        encoded.push_back('=');
    } else if (remaining == 2) {
        const std::uint32_t triple{
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8)
        };
        encoded.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
        encoded.push_back('=');
    }

    return encoded;
}

std::optional<std::vector<std::uint8_t>> Crypto::base64_decode(std::string_view encoded) {
    if (encoded.empty()) {
        return std::vector<std::uint8_t>{};
    }
    if (encoded.size() % 4 != 0) {
        return std::nullopt;
    }

    std::size_t padding{0};
    if (encoded[encoded.size() - 1] == '=') {
        ++padding;
        if (encoded[encoded.size() - 2] == '=') {
            ++padding;
        }
    }

    std::vector<std::uint8_t> decoded{};
    decoded.reserve((encoded.size() / 4) * 3 - padding);

    for (std::size_t i{0}; i < encoded.size(); i += 4) {
        const bool is_last_block{i + 4 == encoded.size()};
        const int v0{decode_base64_char(encoded[i])};
        const int v1{decode_base64_char(encoded[i + 1])};
        if (v0 < 0 || v1 < 0) {
            return std::nullopt;
        }

        const char c2{encoded[i + 2]};
        const char c3{encoded[i + 3]};

        if (c2 == '=') {
            if (!is_last_block || c3 != '=' || (v1 & 0x0F) != 0) {
                return std::nullopt;
            }
            const std::uint32_t triple{
                (static_cast<std::uint32_t>(v0) << 18) |
                (static_cast<std::uint32_t>(v1) << 12)
            };
            decoded.push_back(static_cast<std::uint8_t>((triple >> 16) & 0xFF));
            break;
        }

        const int v2{decode_base64_char(c2)};
        if (v2 < 0) {
            return std::nullopt;
        }

        if (c3 == '=') {
            if (!is_last_block || (v2 & 0x03) != 0) {
                return std::nullopt;
            }
            const std::uint32_t triple{
                (static_cast<std::uint32_t>(v0) << 18) |
                (static_cast<std::uint32_t>(v1) << 12) |
                (static_cast<std::uint32_t>(v2) << 6)
            };
            decoded.push_back(static_cast<std::uint8_t>((triple >> 16) & 0xFF));
            decoded.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xFF));
            break;
        }

        const int v3{decode_base64_char(c3)};
        if (v3 < 0) {
            return std::nullopt;
        }

        const std::uint32_t triple{
            (static_cast<std::uint32_t>(v0) << 18) |
            (static_cast<std::uint32_t>(v1) << 12) |
            (static_cast<std::uint32_t>(v2) << 6) |
            static_cast<std::uint32_t>(v3)
        };
        decoded.push_back(static_cast<std::uint8_t>((triple >> 16) & 0xFF));
        decoded.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xFF));
        decoded.push_back(static_cast<std::uint8_t>(triple & 0xFF));
    }

    return decoded;
}

} // namespace chat
