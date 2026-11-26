#include "ChatPeer.h"
#include "ConsoleUI.h"
#include "Crypto.h"
#include "Logger.h"
#include "PeerRegistry.h"
#include "Protocol.h"
#include "Types.h"
#include "UdpSocket.h"

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int g_tests_run{0};
int g_tests_passed{0};
int g_tests_failed{0};

void fail(std::string_view expr, int line, std::string_view detail = {}) {
    std::ostringstream oss{};
    oss << "Assertion failed at line " << line << ": " << expr;
    if (!detail.empty()) {
        oss << " (" << detail << ")";
    }
    throw std::runtime_error{oss.str()};
}

template <typename Lhs, typename Rhs>
void check_equal(const Lhs& lhs, const Rhs& rhs, std::string_view expr, int line) {
    if (!(lhs == rhs)) {
        fail(expr, line);
    }
}

#define CHECK(expr)                                    \
    do {                                               \
        if (!(expr)) {                                 \
            fail(#expr, __LINE__);                     \
        }                                              \
    } while (false)

#define CHECK_EQ(lhs, rhs)                             \
    check_equal((lhs), (rhs), #lhs " == " #rhs, __LINE__)

void run_test(std::string_view name, const std::function<void()>& fn) {
    ++g_tests_run;
    try {
        fn();
        ++g_tests_passed;
        std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& ex) {
        ++g_tests_failed;
        std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
    }
}

struct TempFile {
    std::string path{};

    explicit TempFile(std::string suffix) {
        static std::atomic<int> counter{0};
        const int id{counter.fetch_add(1)};
        const auto base{std::filesystem::temp_directory_path()};
        path = (base / ("lanchat_test_" + std::to_string(::getpid()) + "_" +
                        std::to_string(id) + "_" + suffix)).string();
    }

    ~TempFile() {
        std::error_code ec{};
        std::filesystem::remove(path, ec);
    }
};

std::string read_file_contents(const std::string& path) {
    std::ifstream ifs{path};
    std::ostringstream oss{};
    oss << ifs.rdbuf();
    return oss.str();
}

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count{0};
    std::size_t pos{0};
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string bytes_to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view kHex{"0123456789abcdef"};
    std::string out{};
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::uint16_t pick_free_udp_port() {
    chat::UdpSocket sock{};
    CHECK(sock.open());
    CHECK(sock.bind_any(0));
    const auto port{sock.local_port()};
    CHECK(port.has_value());
    return *port;
}

void pump_peers(std::vector<chat::ChatPeer*>& peers, int rounds = 10, int poll_ms = 10) {
    for (int i{0}; i < rounds; ++i) {
        for (chat::ChatPeer* peer : peers) {
            bool quit{false};
            peer->poll_once(poll_ms, quit, false);
        }
    }
}

chat::ChatConfig make_test_config(std::string name,
                                  std::uint16_t port,
                                  std::string passphrase,
                                  std::string log_path,
                                  std::string broadcast_ip = std::string{chat::kLoopbackBroadcastIp}) {
    chat::ChatConfig cfg{};
    cfg.username = std::move(name);
    cfg.chat_port = port;
    cfg.passphrase = std::move(passphrase);
    cfg.broadcast_ip = std::move(broadcast_ip);
    cfg.log_file_path = std::move(log_path);
    cfg.join_window = std::chrono::milliseconds{150};
    cfg.heartbeat_interval = std::chrono::milliseconds{5000};
    cfg.peer_timeout = std::chrono::milliseconds{15000};
    return cfg;
}

// ---------- Unit Tests: Crypto & Base64 ----------

void test_crypto_pbkdf2_reference_vector() {
    const chat::Crypto crypto{"secret123"};
    const std::string hex{bytes_to_hex(crypto.key())};
    // Verified independently with Python hashlib.pbkdf2_hmac('sha256', b'secret123', b'NetworkChatLab3_Salt', 100000, 32)
    CHECK_EQ(hex, std::string{"105185e2166e5323a17bafad18e6d8206a36157aec5c063af6d68e3b3fdf75dd"});

    const chat::Crypto other{"different_passphrase"};
    CHECK(crypto.key() != other.key());
}

void test_base64_roundtrip_and_validation() {
    const std::vector<std::uint8_t> empty{};
    CHECK_EQ(chat::Crypto::base64_encode(empty), std::string{""});
    const auto dec_empty{chat::Crypto::base64_decode("")};
    CHECK(dec_empty.has_value() && dec_empty->empty());

    // All 256 byte values + padding variants (len % 3 == 0, 1, 2)
    for (std::size_t len : {1U, 2U, 3U, 4U, 5U, 12U, 16U, 256U}) {
        std::vector<std::uint8_t> input{};
        input.reserve(len);
        for (std::size_t i{0}; i < len; ++i) {
            input.push_back(static_cast<std::uint8_t>(i & 0xFFU));
        }
        const std::string enc{chat::Crypto::base64_encode(input)};
        CHECK_EQ(enc.size() % 4U, 0U);
        const auto dec{chat::Crypto::base64_decode(enc)};
        CHECK(dec.has_value());
        CHECK(*dec == input);
    }

    // Reject invalid Base64 strings
    CHECK(!chat::Crypto::base64_decode("abc").has_value());       // invalid length
    CHECK(!chat::Crypto::base64_decode("ab!d").has_value());      // invalid char
    CHECK(!chat::Crypto::base64_decode("ab\nc").has_value());     // newline
    CHECK(!chat::Crypto::base64_decode("=AAA").has_value());      // leading padding
    CHECK(!chat::Crypto::base64_decode("AA=A").has_value());      // mid padding
    CHECK(!chat::Crypto::base64_decode("AA==AAAA").has_value());  // non-terminal padding
    CHECK(!chat::Crypto::base64_decode("AB==").has_value());      // non-zero trailing bits
    CHECK(!chat::Crypto::base64_decode("AAB=").has_value());      // non-zero trailing bits
}

void test_aes_gcm_encrypt_decrypt_and_tampering() {
    const chat::Crypto crypto{"lab3_pass"};
    const chat::Crypto wrong_crypto{"wrong_pass"};

    const std::vector<std::string> samples{
        "",
        "hello",
        "See you at five!",
        "UTF-8 test: bună ziua, €100, 🚀🔥",
        std::string(chat::kMaxMessageChars, 'x')
    };

    for (const auto& plain : samples) {
        const auto enc1{crypto.encrypt(plain)};
        const auto enc2{crypto.encrypt(plain)};
        CHECK(enc1.has_value());
        CHECK(enc2.has_value());

        // Unique 12-byte IV every time
        CHECK(enc1->iv_b64 != enc2->iv_b64);
        const auto iv_raw{chat::Crypto::base64_decode(enc1->iv_b64)};
        CHECK(iv_raw.has_value());
        CHECK_EQ(iv_raw->size(), chat::kGcmIvBytes);

        // Payload is ciphertext + 16-byte GCM tag
        const auto payload_raw{chat::Crypto::base64_decode(enc1->payload_b64)};
        CHECK(payload_raw.has_value());
        CHECK_EQ(payload_raw->size(), plain.size() + chat::kGcmTagBytes);

        const auto dec{crypto.decrypt(enc1->payload_b64, enc1->iv_b64)};
        CHECK(dec.has_value());
        CHECK_EQ(*dec, plain);

        // Wrong passphrase must fail auth tag check
        CHECK(!wrong_crypto.decrypt(enc1->payload_b64, enc1->iv_b64).has_value());
    }

    // Tampering tests (flip 1 bit in ciphertext, tag, and IV)
    const auto enc{crypto.encrypt("top secret message")};
    CHECK(enc.has_value());

    auto tampered_ct{*chat::Crypto::base64_decode(enc->payload_b64)};
    tampered_ct[0] ^= 0x01U;
    CHECK(!crypto.decrypt(chat::Crypto::base64_encode(tampered_ct), enc->iv_b64).has_value());

    auto tampered_tag{*chat::Crypto::base64_decode(enc->payload_b64)};
    tampered_tag.back() ^= 0x80U;
    CHECK(!crypto.decrypt(chat::Crypto::base64_encode(tampered_tag), enc->iv_b64).has_value());

    auto tampered_iv{*chat::Crypto::base64_decode(enc->iv_b64)};
    tampered_iv[5] ^= 0x04U;
    CHECK(!crypto.decrypt(enc->payload_b64, chat::Crypto::base64_encode(tampered_iv)).has_value());

    // Truncated payload (< 16 bytes) and wrong IV lengths (11, 13 bytes)
    const std::vector<std::uint8_t> short_payload{1, 2, 3, 4, 5};
    CHECK(!crypto.decrypt(chat::Crypto::base64_encode(short_payload), enc->iv_b64).has_value());

    std::vector<std::uint8_t> short_iv{};
    short_iv.resize(11);
    CHECK(!crypto.decrypt(enc->payload_b64, chat::Crypto::base64_encode(short_iv)).has_value());

    std::vector<std::uint8_t> long_iv{};
    long_iv.resize(13);
    CHECK(!crypto.decrypt(enc->payload_b64, chat::Crypto::base64_encode(long_iv)).has_value());
}

// ---------- Unit Tests: Protocol & UTF-8 ----------

void test_utf8_and_username_validation() {
    CHECK_EQ(*chat::utf8_char_count(""), 0U);
    CHECK_EQ(*chat::utf8_char_count("hello"), 5U);
    CHECK_EQ(*chat::utf8_char_count("ăâîșț"), 5U);
    CHECK_EQ(*chat::utf8_char_count("€€€"), 3U);
    CHECK_EQ(*chat::utf8_char_count("😀😁"), 2U);

    // Invalid UTF-8 sequences
    CHECK(!chat::is_valid_utf8(std::string{"\xFF\xFE"}));
    CHECK(!chat::is_valid_utf8(std::string{"\xC0\xAF"}));      // overlong 2-byte
    CHECK(!chat::is_valid_utf8(std::string{"\xED\xA0\x80"}));  // UTF-16 surrogate
    CHECK(!chat::is_valid_utf8(std::string{"\xF4\x90\x80\x80"})); // > U+10FFFF
    CHECK(!chat::is_valid_utf8(std::string{"\xE2\x82"}));      // truncated 3-byte

    CHECK(chat::is_valid_username("Ana"));
    CHECK(chat::is_valid_username("user_123-ok"));
    CHECK(chat::is_valid_username(std::string(chat::kMaxUsernameChars, 'a')));
    CHECK(!chat::is_valid_username(""));
    CHECK(!chat::is_valid_username("Ana Maria"));
    CHECK(!chat::is_valid_username("Ana\tIon"));
    CHECK(!chat::is_valid_username("bad/name"));
    CHECK(!chat::is_valid_username("bad\\name"));
    CHECK(!chat::is_valid_username(std::string(chat::kMaxUsernameChars + 1, 'a')));
}

void test_protocol_roundtrip_and_edge_cases() {
    const std::vector<chat::ProtocolMessage> valid_messages{
        chat::ProtocolMessage{chat::MessageType::Join, "Ana", {}, {}, {}},
        chat::ProtocolMessage{chat::MessageType::JoinAck, "Ion", {}, {}, {}},
        chat::ProtocolMessage{chat::MessageType::JoinReject, "Ana", {}, {}, {}},
        chat::ProtocolMessage{chat::MessageType::Heartbeat, "Maria", {}, {}, {}},
        chat::ProtocolMessage{chat::MessageType::Leave, "Ion", {}, {}, {}},
        chat::ProtocolMessage{chat::MessageType::ChatPublic, "Ana", "hello everyone", {}, {}},
        chat::ProtocolMessage{chat::MessageType::ChatPrivate, "Ion", {}, "U2FsdGVkX1==", "aB39kL0123456789"},
    };

    for (const auto& orig : valid_messages) {
        const std::string wire{chat::serialize_message(orig)};
        CHECK(!wire.empty() && wire.front() == '{' && wire.back() == '}');

        const auto res{chat::deserialize_message(wire)};
        CHECK(res.message.has_value());
        CHECK(res.message->type == orig.type);
        CHECK_EQ(res.message->sender, orig.sender);
        CHECK_EQ(res.message->text, orig.text);
        CHECK_EQ(res.message->payload, orig.payload);
        CHECK_EQ(res.message->iv, orig.iv);
    }

    // 500-char exact boundary (ASCII and multibyte UTF-8)
    const chat::ProtocolMessage exact_500{
        chat::MessageType::ChatPublic, "Ana", std::string(500, 'a'), {}, {}
    };
    CHECK(chat::deserialize_message(chat::serialize_message(exact_500)).message.has_value());

    const chat::ProtocolMessage over_500{
        chat::MessageType::ChatPublic, "Ana", std::string(501, 'a'), {}, {}
    };
    CHECK(!chat::deserialize_message(chat::serialize_message(over_500)).message.has_value());

    std::string utf8_500{};
    for (std::size_t i{0}; i < 500; ++i) {
        utf8_500 += "ă"; // 2 bytes per code point -> 1000 bytes (< 1400)
    }
    const chat::ProtocolMessage exact_utf8_500{chat::MessageType::ChatPublic, "Ana", utf8_500, {}, {}};
    CHECK(chat::deserialize_message(chat::serialize_message(exact_utf8_500)).message.has_value());

    utf8_500 += "ă"; // 501 code points
    const chat::ProtocolMessage over_utf8_500{chat::MessageType::ChatPublic, "Ana", utf8_500, {}, {}};
    CHECK(!chat::deserialize_message(chat::serialize_message(over_utf8_500)).message.has_value());

    // Extra harmless field allowed for cross-language interop
    CHECK(chat::deserialize_message(R"({"type":"join","sender":"Ana","version":1})").message.has_value());

    // Malformed / invalid datagrams
    const std::vector<std::string> invalid_inputs{
        "",
        "not json at all",
        std::string{"\x00\x01\x02\xFF", 4},
        "[]",
        "\"just a string\"",
        "123",
        R"({"sender":"Ana"})",
        R"({"type":"join"})",
        R"({"type":123,"sender":"Ana"})",
        R"({"type":"join","sender":42})",
        R"({"type":"unknown_type","sender":"Ana"})",
        R"({"type":"join","sender":"bad name"})",
        R"({"type":"chat_public","sender":"Ana"})",
        R"({"type":"chat_public","sender":"Ana","text":99})",
        R"({"type":"chat_private","sender":"Ana","payload":"abc"})",
        R"({"type":"chat_private","sender":"Ana","iv":"abc"})",
        R"({"type":"chat_private","sender":"Ana","payload":"","iv":"abc"})",
        std::string(chat::kMaxDatagramBytes + 1, 'a')
    };

    for (const auto& bad : invalid_inputs) {
        CHECK(!chat::deserialize_message(bad).message.has_value());
    }
}

// ---------- Unit Tests: PeerRegistry, Logger, UdpSocket, ConsoleUI ----------

void test_peer_registry() {
    chat::PeerRegistry reg{};
    const auto t0{chat::PeerRegistry::Clock::now()};

    CHECK(reg.upsert("Ion", chat::Endpoint{"192.168.1.2", 40001}, t0));
    CHECK(reg.upsert("Ana", chat::Endpoint{"192.168.1.3", 40002}, t0));
    CHECK_EQ(reg.size(), 2U);

    // Sorted order
    const auto list{reg.list_peers()};
    CHECK_EQ(list.size(), 2U);
    CHECK_EQ(list[0].first, std::string{"Ana"});
    CHECK_EQ(list[1].first, std::string{"Ion"});

    // Protected upsert (overwrite_different_endpoint = false) must keep original endpoint
    CHECK(!reg.upsert("Ana", chat::Endpoint{"192.168.1.9", 55555}, t0 + std::chrono::seconds{2}, false));
    CHECK(*reg.find_endpoint("Ana") == (chat::Endpoint{"192.168.1.3", 40002}));

    // Explicit overwrite updates endpoint (e.g. after heartbeat from restarted peer)
    CHECK(!reg.upsert("Ana", chat::Endpoint{"192.168.1.3", 40099}, t0 + std::chrono::seconds{5}, true));
    CHECK(*reg.find_endpoint("Ana") == (chat::Endpoint{"192.168.1.3", 40099}));

    // Expiry: at t0 + 15s, Ion expires while Ana (refreshed at t0 + 5s) stays alive
    const auto expired{reg.remove_expired(t0 + std::chrono::seconds{15}, std::chrono::seconds{15})};
    CHECK_EQ(expired.size(), 1U);
    CHECK_EQ(expired[0], std::string{"Ion"});
    CHECK(!reg.contains("Ion"));
    CHECK(reg.contains("Ana"));

    CHECK(reg.remove("Ana"));
    CHECK(!reg.remove("Ana"));
    CHECK_EQ(reg.size(), 0U);
}

void test_logger() {
    TempFile log_file{"logger.log"};
    chat::Logger logger{log_file.path};
    CHECK(logger.is_open());

    logger.log_datagram(chat::DatagramDirection::Send, chat::Endpoint{"127.255.255.255", 9000}, "join", 32);
    logger.log_datagram(chat::DatagramDirection::Recv, chat::Endpoint{"127.0.0.1", 51234}, "join_ack", 37);
    logger.log_datagram(chat::DatagramDirection::Drop, chat::Endpoint{"10.0.0.5", 1234}, "invalid (malformed JSON)", 9);

    const std::string contents{read_file_contents(log_file.path)};
    CHECK(contents.find("| SEND | 127.255.255.255:9000 | join | 32 bytes") != std::string::npos);
    CHECK(contents.find("| RECV | 127.0.0.1:51234 | join_ack | 37 bytes") != std::string::npos);
    CHECK(contents.find("| DROP | 10.0.0.5:1234 | invalid (malformed JSON) | 9 bytes") != std::string::npos);
}

void test_udp_socket_broadcast_unicast_and_truncation() {
    const std::uint16_t shared_port{pick_free_udp_port()};

    chat::UdpSocket s1{};
    chat::UdpSocket s2{};
    CHECK(s1.open() && s1.enable_reuse_addr() && s1.enable_reuse_port() && s1.bind_any(shared_port));
    CHECK(s2.open() && s2.enable_reuse_addr() && s2.enable_reuse_port() && s2.bind_any(shared_port));

    chat::UdpSocket tx{};
    CHECK(tx.open() && tx.enable_broadcast() && tx.bind_any(0));
    const auto tx_port{tx.local_port()};
    CHECK(tx_port.has_value());

    // Broadcast reaches BOTH sockets bound to shared_port
    CHECK(tx.send_to("broadcast_msg", chat::Endpoint{std::string{chat::kLoopbackBroadcastIp}, shared_port}));
    const auto r1{s1.recv_from(1024)};
    const auto r2{s2.recv_from(1024)};
    CHECK(r1.has_value() && r1->data == "broadcast_msg" && r1->sender.port == *tx_port && !r1->truncated);
    CHECK(r2.has_value() && r2->data == "broadcast_msg" && r2->sender.port == *tx_port && !r2->truncated);

    // Oversized datagram truncation detection (MSG_TRUNC)
    const std::string big_payload(2048, 'Z');
    CHECK(tx.send_to(big_payload, chat::Endpoint{"127.0.0.1", *tx_port}));
    const auto r_big{tx.recv_from(chat::kMaxDatagramBytes)};
    CHECK(r_big.has_value());
    CHECK(r_big->truncated);
    CHECK_EQ(r_big->actual_size, 2048U);

    // Local IPv4 addresses includes loopback
    const auto ips{chat::UdpSocket::local_ipv4_addresses()};
    CHECK(ips.contains("127.0.0.1"));

    CHECK(chat::UdpSocket::is_valid_ipv4("255.255.255.255"));
    CHECK(chat::UdpSocket::is_valid_ipv4("127.255.255.255"));
    CHECK(!chat::UdpSocket::is_valid_ipv4("999.0.0.1"));
    CHECK(!chat::UdpSocket::is_valid_ipv4("not-an-ip"));
}

void test_console_ui_pipe_mode() {
    std::array<int, 2> pipe_fds{-1, -1};
    CHECK(::pipe(pipe_fds.data()) == 0);

    std::ostringstream out{};
    chat::ConsoleUI ui{pipe_fds[0], out, false};
    CHECK(!ui.is_interactive());

    const std::string chunk1{"first line\r\nsecond "};
    CHECK(::write(pipe_fds[1], chunk1.data(), chunk1.size()) == static_cast<ssize_t>(chunk1.size()));
    const auto res1{ui.read_available()};
    CHECK(!res1.eof);
    CHECK_EQ(res1.lines.size(), 1U);
    CHECK_EQ(res1.lines[0], std::string{"first line"});

    const std::string chunk2{"line\ntrailing"};
    CHECK(::write(pipe_fds[1], chunk2.data(), chunk2.size()) == static_cast<ssize_t>(chunk2.size()));
    const auto res2{ui.read_available()};
    CHECK(!res2.eof);
    CHECK_EQ(res2.lines.size(), 1U);
    CHECK_EQ(res2.lines[0], std::string{"second line"});

    ::close(pipe_fds[1]);
    const auto res3{ui.read_available()};
    CHECK(res3.eof);
    CHECK_EQ(res3.lines.size(), 1U);
    CHECK_EQ(res3.lines[0], std::string{"trailing"});

    ::close(pipe_fds[0]);
}

// ---------- Integration Tests: Multi-Peer Live UDP Chat ----------

void test_live_discovery_general_private_and_self_messaging() {
    const std::uint16_t port{pick_free_udp_port()};
    TempFile log_ana{"ana.log"};
    TempFile log_ion{"ion.log"};
    TempFile log_maria{"maria.log"};

    std::ostringstream out_ana{};
    std::ostringstream out_ion{};
    std::ostringstream out_maria{};

    chat::ChatPeer ana{make_test_config("Ana", port, "shared_secret", log_ana.path), -1, out_ana, false};
    chat::ChatPeer ion{make_test_config("Ion", port, "shared_secret", log_ion.path), -1, out_ion, false};
    chat::ChatPeer maria{make_test_config("Maria", port, "shared_secret", log_maria.path), -1, out_maria, false};

    CHECK(ana.initialize());
    CHECK(ana.join_network() == chat::RunStatus::ExitedNormally);

    // Ion joins while Ana pumps events so Ana replies with join_ack
    CHECK(ion.initialize());
    std::thread ana_pump{[&ana]() {
        for (int i{0}; i < 25; ++i) {
            bool quit{false};
            ana.poll_once(10, quit, false);
        }
    }};
    CHECK(ion.join_network() == chat::RunStatus::ExitedNormally);
    ana_pump.join();

    // Both know each other with exact personal ports!
    CHECK(ana.peers().contains("Ion"));
    CHECK(ion.peers().contains("Ana"));
    CHECK_EQ(ana.peers().find_endpoint("Ion")->port, ion.personal_port());
    CHECK_EQ(ion.peers().find_endpoint("Ana")->port, ana.personal_port());
    CHECK(out_ana.str().find("* Ion joined the chat") != std::string::npos);

    // Maria joins while Ana & Ion pump
    CHECK(maria.initialize());
    std::thread both_pump{[&ana, &ion]() {
        for (int i{0}; i < 25; ++i) {
            bool quit{false};
            ana.poll_once(10, quit, false);
            ion.poll_once(10, quit, false);
        }
    }};
    CHECK(maria.join_network() == chat::RunStatus::ExitedNormally);
    both_pump.join();

    CHECK(maria.peers().contains("Ana") && maria.peers().contains("Ion"));
    CHECK(ana.peers().contains("Maria") && ion.peers().contains("Maria"));

    std::vector<chat::ChatPeer*> all_peers{&ana, &ion, &maria};

    // Task 1: General message shown to everybody, including author EXACTLY ONCE
    bool quit{false};
    ana.process_user_line("hello everyone", quit);
    CHECK(!quit);
    pump_peers(all_peers);

    CHECK_EQ(count_occurrences(out_ana.str(), "[general] Ana: hello everyone"), 1U);
    CHECK_EQ(count_occurrences(out_ion.str(), "[general] Ana: hello everyone"), 1U);
    CHECK_EQ(count_occurrences(out_maria.str(), "[general] Ana: hello everyone"), 1U);

    // Task 3 & 4: Private encrypted message reaches ONLY receiver, NOT Maria on same machine!
    ana.process_user_line("/msg Ion see you at five", quit);
    pump_peers(all_peers);

    CHECK_EQ(count_occurrences(out_ion.str(), "[private] Ana: see you at five"), 1U);
    CHECK_EQ(count_occurrences(out_maria.str(), "[private] Ana: see you at five"), 0U);
    CHECK_EQ(count_occurrences(out_ana.str(), "[private] Ana: see you at five"), 0U);

    // Self-messaging requirement: /msg <my_name> <text> works through real encrypted UDP socket!
    ana.process_user_line("/msg Ana note to myself", quit);
    pump_peers(all_peers);

    CHECK_EQ(count_occurrences(out_ana.str(), "[private] Ana: note to myself"), 1U);
    CHECK_EQ(count_occurrences(out_ion.str(), "[private] Ana: note to myself"), 0U);
    CHECK_EQ(count_occurrences(out_maria.str(), "[private] Ana: note to myself"), 0U);

    // Verify self-messaging logged both SEND and RECV of chat_private to Ana's own personal port
    const std::string ana_log_text{read_file_contents(log_ana.path)};
    const std::string self_ep{"127.0.0.1:" + std::to_string(ana.personal_port())};
    CHECK(ana_log_text.find("| SEND | " + self_ep + " | chat_private |") != std::string::npos);
    CHECK(ana_log_text.find("| RECV | " + self_ep + " | chat_private |") != std::string::npos);

    // /users command lists self and all online peers
    ana.process_user_line("/users", quit);
    CHECK(out_ana.str().find("Ana (you)") != std::string::npos);
    CHECK(out_ana.str().find("Ion [") != std::string::npos);
    CHECK(out_ana.str().find("Maria [") != std::string::npos);

    // Command error edge cases
    ana.process_user_line("/msg Ghost hello", quit);
    CHECK(out_ana.str().find("Error: user \"Ghost\" is not online") != std::string::npos);

    ana.process_user_line("/msg", quit);
    ana.process_user_line("/msg Ion", quit);
    ana.process_user_line("/msg Ion   ", quit);
    CHECK_EQ(count_occurrences(out_ana.str(), "Usage: /msg <name> <text>"), 3U);

    ana.process_user_line("/unknowncmd", quit);
    CHECK(out_ana.str().find("Unknown command.") != std::string::npos);

    ana.process_user_line(std::string(501, 'A'), quit);
    CHECK(out_ana.str().find("Error: message exceeds 500 characters") != std::string::npos);

    ana.process_user_line("/msg Ana " + std::string(501, 'B'), quit);
    CHECK_EQ(count_occurrences(out_ana.str(), "Error: message exceeds 500 characters"), 2U);

    // Exact 500-char private self-message succeeds!
    const std::string exact_500(500, 'M');
    ana.process_user_line("/msg Ana " + exact_500, quit);
    pump_peers(all_peers);
    CHECK_EQ(count_occurrences(out_ana.str(), "[private] Ana: " + exact_500), 1U);

    // Graceful /quit broadcasts leave and removes peer from others
    ion.process_user_line("/quit", quit);
    CHECK(quit);
    ion.send_leave();
    pump_peers(all_peers);

    CHECK(!ana.peers().contains("Ion"));
    CHECK(!maria.peers().contains("Ion"));
    CHECK(out_ana.str().find("* Ion left the chat") != std::string::npos);
    CHECK(out_maria.str().find("* Ion left the chat") != std::string::npos);

    ana.send_leave();
    maria.send_leave();
}

void test_duplicate_username_rejection_and_existing_peer_protection() {
    const std::uint16_t port{pick_free_udp_port()};
    TempFile log_ana1{"ana1.log"};
    TempFile log_ion{"ion_dup.log"};
    TempFile log_ana2{"ana2.log"};

    std::ostringstream out_ana1{};
    std::ostringstream out_ion{};
    std::ostringstream out_ana2{};

    chat::ChatPeer ana1{make_test_config("Ana", port, "secret", log_ana1.path), -1, out_ana1, false};
    chat::ChatPeer ion{make_test_config("Ion", port, "secret", log_ion.path), -1, out_ion, false};

    CHECK(ana1.initialize());
    CHECK(ana1.join_network() == chat::RunStatus::ExitedNormally);

    CHECK(ion.initialize());
    std::thread p1{[&ana1]() {
        for (int i{0}; i < 25; ++i) {
            bool q{false};
            ana1.poll_once(10, q, false);
        }
    }};
    CHECK(ion.join_network() == chat::RunStatus::ExitedNormally);
    p1.join();

    const std::uint16_t original_ana_port{ana1.personal_port()};
    CHECK(ion.peers().contains("Ana"));
    CHECK_EQ(ion.peers().find_endpoint("Ana")->port, original_ana_port);

    // Second peer tries to join as "Ana" -> must be rejected and NOT disrupt Ion's route to original Ana
    chat::ChatPeer ana2{make_test_config("Ana", port, "secret", log_ana2.path), -1, out_ana2, false};
    CHECK(ana2.initialize());

    std::thread p2{[&ana1, &ion]() {
        for (int i{0}; i < 25; ++i) {
            bool q{false};
            ana1.poll_once(10, q, false);
            ion.poll_once(10, q, false);
        }
    }};
    CHECK(ana2.join_network() == chat::RunStatus::NameTaken);
    p2.join();

    CHECK(out_ana2.str().find("already in use") != std::string::npos);
    // Original Ana's endpoint in Ion's table must be untouched!
    CHECK(ion.peers().contains("Ana"));
    CHECK_EQ(ion.peers().find_endpoint("Ana")->port, original_ana_port);

    // Ion can still send private messages to the real Ana
    bool quit{false};
    ion.process_user_line("/msg Ana still reachable", quit);
    std::vector<chat::ChatPeer*> live{&ana1, &ion};
    pump_peers(live);
    CHECK(out_ana1.str().find("[private] Ion: still reachable") != std::string::npos);

    ana1.send_leave();
    ion.send_leave();
}

void test_simultaneous_duplicate_join() {
    const std::uint16_t port{pick_free_udp_port()};
    TempFile log1{"twin1.log"};
    TempFile log2{"twin2.log"};

    std::ostringstream out1{};
    std::ostringstream out2{};

    chat::ChatPeer twin1{make_test_config("Twin", port, "secret", log1.path), -1, out1, false};
    chat::ChatPeer twin2{make_test_config("Twin", port, "secret", log2.path), -1, out2, false};
    CHECK(twin1.initialize());
    CHECK(twin2.initialize());

    chat::RunStatus s1{chat::RunStatus::InitError};
    chat::RunStatus s2{chat::RunStatus::InitError};

    std::thread t1{[&]() { s1 = twin1.join_network(); }};
    std::thread t2{[&]() { s2 = twin2.join_network(); }};
    t1.join();
    t2.join();

    // At least one (in practice both) detects the collision and rejects
    CHECK(s1 == chat::RunStatus::NameTaken || s2 == chat::RunStatus::NameTaken);
}

void test_wrong_passphrase_and_malformed_datagrams_dropped_and_logged() {
    const std::uint16_t port{pick_free_udp_port()};
    TempFile log_ana{"sec_ana.log"};
    TempFile log_eve{"sec_eve.log"};

    std::ostringstream out_ana{};
    std::ostringstream out_eve{};

    chat::ChatPeer ana{make_test_config("Ana", port, "correct_pass", log_ana.path), -1, out_ana, false};
    chat::ChatPeer eve{make_test_config("Eve", port, "wrong_pass", log_eve.path), -1, out_eve, false};

    CHECK(ana.initialize());
    CHECK(ana.join_network() == chat::RunStatus::ExitedNormally);

    CHECK(eve.initialize());
    std::thread p{[&ana]() {
        for (int i{0}; i < 25; ++i) {
            bool q{false};
            ana.poll_once(10, q, false);
        }
    }};
    CHECK(eve.join_network() == chat::RunStatus::ExitedNormally);
    p.join();

    // Eve sends private message encrypted with wrong_pass -> Ana must drop & log, NOT print
    bool quit{false};
    eve.process_user_line("/msg Ana can you read this?", quit);

    // Blast malformed / random / oversized / wrong-channel datagrams at BOTH Socket 1 and Socket 2
    chat::UdpSocket raw{};
    CHECK(raw.open() && raw.enable_broadcast() && raw.bind_any(0));

    const chat::Endpoint bcast_ep{std::string{chat::kLoopbackBroadcastIp}, port};
    const chat::Endpoint ana_personal_ep{"127.0.0.1", ana.personal_port()};

    // 1. Random binary garbage
    CHECK(raw.send_to(std::string{"\xDE\xAD\xBE\xEF\x00\xFF", 6}, bcast_ep));
    CHECK(raw.send_to(std::string{"\xDE\xAD\xBE\xEF\x00\xFF", 6}, ana_personal_ep));
    // 2. Malformed JSON
    CHECK(raw.send_to("{broken json", bcast_ep));
    CHECK(raw.send_to("{broken json", ana_personal_ep));
    // 3. Oversized UDP datagram (> 1400 bytes)
    const std::string huge(3000, 'X');
    CHECK(raw.send_to(huge, bcast_ep));
    CHECK(raw.send_to(huge, ana_personal_ep));
    // 4. Wrong channel: chat_private sent to broadcast port, chat_public sent to personal port
    CHECK(raw.send_to(R"({"type":"chat_private","sender":"Eve","payload":"AAAA","iv":"BBBB"})", bcast_ep));
    CHECK(raw.send_to(R"({"type":"chat_public","sender":"Eve","text":"wrong socket"})", ana_personal_ep));
    // 5. > 500 char chat_public
    const std::string over_500_json{
        R"({"type":"chat_public","sender":"Eve","text":")" + std::string(501, 'a') + R"("})"
    };
    CHECK(raw.send_to(over_500_json, bcast_ep));
    // 6. Tampered valid-passphrase private message
    const chat::Crypto real_crypto{"correct_pass"};
    const auto enc{real_crypto.encrypt("tamper test")};
    CHECK(enc.has_value());
    auto tampered_bytes{*chat::Crypto::base64_decode(enc->payload_b64)};
    tampered_bytes[0] ^= 0xFFU;
    const chat::ProtocolMessage tampered_msg{
        chat::MessageType::ChatPrivate,
        "Eve",
        {},
        chat::Crypto::base64_encode(tampered_bytes),
        enc->iv_b64
    };
    CHECK(raw.send_to(chat::serialize_message(tampered_msg), ana_personal_ep));

    std::vector<chat::ChatPeer*> peers{&ana, &eve};
    pump_peers(peers, 20, 10);

    // None of the bad/wrong-pass/tampered messages should be displayed
    CHECK(out_ana.str().find("can you read this?") == std::string::npos);
    CHECK(out_ana.str().find("tamper test") == std::string::npos);
    CHECK(out_ana.str().find("wrong socket") == std::string::npos);

    // Every bad datagram must be logged as DROP
    const std::string log_text{read_file_contents(log_ana.path)};
    CHECK(log_text.find("| DROP |") != std::string::npos);
    CHECK(log_text.find("decryption failed") != std::string::npos);
    CHECK(log_text.find("datagram too long") != std::string::npos);
    CHECK(log_text.find("malformed JSON") != std::string::npos);
    CHECK(log_text.find("unicast type on broadcast socket") != std::string::npos);
    CHECK(log_text.find("unexpected message type on personal socket") != std::string::npos);
    CHECK(count_occurrences(log_text, "| DROP |") >= 10U);

    // Ana is still completely healthy and can receive valid public & private messages!
    eve.process_user_line("public still works", quit);
    ana.process_user_line("/msg Ana still alive", quit);
    pump_peers(peers, 10, 10);

    CHECK(out_ana.str().find("[general] Eve: public still works") != std::string::npos);
    CHECK(out_ana.str().find("[private] Ana: still alive") != std::string::npos);

    ana.send_leave();
    eve.send_leave();
}

void test_heartbeat_keeps_alive_and_silent_peer_times_out() {
    const std::uint16_t port{pick_free_udp_port()};
    TempFile log_a{"hb_a.log"};
    TempFile log_b{"hb_b.log"};
    TempFile log_c{"hb_c.log"};

    std::ostringstream out_a{};
    std::ostringstream out_b{};
    std::ostringstream out_c{};

    auto cfg_a{make_test_config("Alice", port, "secret", log_a.path)};
    cfg_a.heartbeat_interval = std::chrono::milliseconds{100};
    cfg_a.peer_timeout = std::chrono::milliseconds{350};

    auto cfg_b{make_test_config("Bob", port, "secret", log_b.path)};
    cfg_b.heartbeat_interval = std::chrono::milliseconds{100};
    cfg_b.peer_timeout = std::chrono::milliseconds{350};

    auto cfg_c{make_test_config("Silent", port, "secret", log_c.path)};
    cfg_c.heartbeat_interval = std::chrono::milliseconds{5000};
    cfg_c.peer_timeout = std::chrono::milliseconds{350};

    chat::ChatPeer alice{std::move(cfg_a), -1, out_a, false};
    chat::ChatPeer bob{std::move(cfg_b), -1, out_b, false};
    chat::ChatPeer silent{std::move(cfg_c), -1, out_c, false};

    CHECK(alice.initialize());
    CHECK(alice.join_network() == chat::RunStatus::ExitedNormally);

    CHECK(bob.initialize());
    CHECK(silent.initialize());
    std::thread pump{[&]() {
        for (int i{0}; i < 30; ++i) {
            bool q{false};
            alice.poll_once(10, q, false);
        }
    }};
    CHECK(bob.join_network() == chat::RunStatus::ExitedNormally);
    CHECK(silent.join_network() == chat::RunStatus::ExitedNormally);
    pump.join();

    CHECK(alice.peers().contains("Bob"));
    CHECK(alice.peers().contains("Silent"));

    // Run Alice & Bob for ~500ms while Silent does NOT send heartbeats (simulating SIGKILL crash)
    for (int i{0}; i < 25; ++i) {
        bool q{false};
        alice.poll_once(20, q, false);
        bob.poll_once(20, q, false);
        alice.tick_maintenance();
        bob.tick_maintenance();
    }

    // Bob stayed alive via heartbeats; Silent timed out and was removed!
    CHECK(alice.peers().contains("Bob"));
    CHECK(!alice.peers().contains("Silent"));
    CHECK(out_a.str().find("* Silent left the chat") != std::string::npos);
    CHECK(out_a.str().find("* Bob left the chat") == std::string::npos);

    alice.send_leave();
    bob.send_leave();
}

void test_default_broadcast_fallback_in_loopback_environment() {
    const std::uint16_t port{pick_free_udp_port()};
    TempFile log_a{"fb_a.log"};
    std::ostringstream out_a{};

    chat::ChatPeer peer{
        make_test_config("FallbackPeer", port, "secret", log_a.path, std::string{chat::kDefaultBroadcastIp}),
        -1,
        out_a,
        false
    };
    CHECK(peer.initialize());
    CHECK(peer.join_network() == chat::RunStatus::ExitedNormally);
    peer.send_leave();
}

} // namespace

int main() {
    run_test("Crypto PBKDF2-HMAC-SHA256 reference vector", test_crypto_pbkdf2_reference_vector);
    run_test("Base64 roundtrip and strict validation", test_base64_roundtrip_and_validation);
    run_test("AES-256-GCM encrypt/decrypt, IV uniqueness, and tamper detection", test_aes_gcm_encrypt_decrypt_and_tampering);
    run_test("UTF-8 character counting and username validation", test_utf8_and_username_validation);
    run_test("Protocol JSON roundtrip, 500-char limit, and malformed inputs", test_protocol_roundtrip_and_edge_cases);
    run_test("PeerRegistry upsert protection, sorting, and expiry", test_peer_registry);
    run_test("Logger SEND/RECV/DROP formatting", test_logger);
    run_test("UdpSocket broadcast fan-out, unicast, and MSG_TRUNC detection", test_udp_socket_broadcast_unicast_and_truncation);
    run_test("ConsoleUI pipe-mode buffered line reading and EOF", test_console_ui_pipe_mode);
    run_test("Live multi-peer discovery, general, private, and self-messaging (/msg self)", test_live_discovery_general_private_and_self_messaging);
    run_test("Live duplicate username rejection and live-peer protection", test_duplicate_username_rejection_and_existing_peer_protection);
    run_test("Live simultaneous duplicate join collision", test_simultaneous_duplicate_join);
    run_test("Live wrong passphrase, tampering, and malformed/oversized datagrams", test_wrong_passphrase_and_malformed_datagrams_dropped_and_logged);
    run_test("Live heartbeats keep peers alive while crashed peers time out", test_heartbeat_keeps_alive_and_silent_peer_times_out);
    run_test("Default 255.255.255.255 broadcast initialization and loopback fallback", test_default_broadcast_fallback_in_loopback_environment);

    std::cout << "\nSummary: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0) {
        std::cout << " (" << g_tests_failed << " failed)";
    }
    std::cout << '\n';

    return g_tests_failed == 0 ? 0 : 1;
}
