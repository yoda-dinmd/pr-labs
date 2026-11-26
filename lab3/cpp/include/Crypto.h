#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chat {

inline constexpr std::string_view kKdfSalt{"NetworkChatLab3_Salt"};
inline constexpr int kPbkdf2Iterations{100'000};
inline constexpr std::size_t kAesKeyBytes{32};
inline constexpr std::size_t kGcmIvBytes{12};
inline constexpr std::size_t kGcmTagBytes{16};

struct EncryptedPayload {
    std::string payload_b64{};
    std::string iv_b64{};
};

class Crypto {
public:
    explicit Crypto(std::string_view passphrase);

    [[nodiscard]] const std::array<std::uint8_t, kAesKeyBytes>& key() const noexcept;

    [[nodiscard]] std::optional<EncryptedPayload> encrypt(std::string_view plaintext) const;
    [[nodiscard]] std::optional<std::string> decrypt(std::string_view payload_b64,
                                                     std::string_view iv_b64) const;

    [[nodiscard]] static std::array<std::uint8_t, kAesKeyBytes> derive_key(std::string_view passphrase);
    [[nodiscard]] static std::string base64_encode(std::span<const std::uint8_t> data);
    [[nodiscard]] static std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view encoded);

private:
    std::array<std::uint8_t, kAesKeyBytes> key_{};
};

} // namespace chat
