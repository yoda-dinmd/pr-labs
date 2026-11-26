# Local Network Peer-to-Peer UDP Chat (C++23)

Serverless peer-to-peer LAN chat written in modern C++23 using the POSIX UDP socket API directly (`socket`, `setsockopt`, `bind`, `getsockname`, `sendto`, `recvfrom`, `poll`, `close`).

## Features

- **General Channel (Broadcast)**: One UDP datagram per message delivered to all peers on the shared chat port (including multiple peers on the same computer).
- **Peer Discovery & Presence**: Serverless discovery via `join` / `join_ack`, duplicate username rejection via `join_reject`, 5-second `heartbeat` broadcasts, graceful `leave`, and 15-second inactivity timeout.
- **Direct Private Messages**: Unicast UDP datagrams sent directly to the receiver's personal ephemeral socket (`IP:PersonalPort`). Supports messaging yourself via `/msg <your_name> <text>`.
- **Authenticated Encryption**: Private messages are encrypted with **AES-256-GCM** using a 256-bit key derived from the shared passphrase via **PBKDF2-HMAC-SHA256** (salt `"NetworkChatLab3_Salt"`, 100,000 iterations) and a fresh 12-byte random IV per message.
- **Datagram Logging & Robustness**: Every sent, received, or dropped datagram is logged (`timestamp | direction | endpoint | kind | size`) to `chat_<username>_<personal_port>.log`. Malformed, oversized (> 1400 bytes or > 500 characters), or tampered datagrams are dropped and logged without crashing.
- **Non-blocking Console UI**: Single-threaded `poll()` event loop displays incoming messages immediately without corrupting the line currently being typed.

## Project Layout

```text
cpp/
├── CMakeLists.txt
├── include/
│   ├── Types.h         # Shared structs, config, and protocol constants
│   ├── Logger.h        # Datagram file logger (SEND / RECV / DROP)
│   ├── Crypto.h        # PBKDF2-HMAC-SHA256, AES-256-GCM, strict Base64
│   ├── Protocol.h      # JSON schema serialization, validation, UTF-8 counting
│   ├── UdpSocket.h     # Direct POSIX UDP socket RAII wrapper
│   ├── PeerRegistry.h  # Online peer map and timeout expiry
│   ├── ConsoleUI.h     # Interactive TTY & piped stdin/stdout handling
│   └── ChatPeer.h      # Two-socket architecture & poll() event loop
├── src/
│   ├── Logger.cpp
│   ├── Crypto.cpp
│   ├── Protocol.cpp
│   ├── UdpSocket.cpp
│   ├── PeerRegistry.cpp
│   ├── ConsoleUI.cpp
│   ├── ChatPeer.cpp
│   └── main.cpp
└── tests/
    └── test_main.cpp   # Unit, integration, and edge-case test suite
```

## Dependencies

- C++23 compiler (e.g., GCC 13+ / Clang 17+)
- CMake 3.20+
- OpenSSL (`libcrypto` development headers)
- `nlohmann/json` (automatically found if installed, otherwise fetched via CMake `FetchContent`)

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/chat <username> <port> <passphrase> [broadcast_ip]
```

Examples:

```bash
# Peer 1
./build/chat Ana 9000 secret123

# Peer 2 (same computer or another computer on the LAN)
./build/chat Ion 9000 secret123

# Optional: specify a subnet or loopback broadcast address if needed
./build/chat Maria 9000 secret123 192.168.1.255
```

> **Note:** If a Wi-Fi network blocks `255.255.255.255`, pass your subnet broadcast address as the optional 4th argument or use a mobile hotspot. When no LAN route exists (loopback-only testing), the default `255.255.255.255` automatically falls back to `127.255.255.255`.

## Commands

| Command | Description |
|---|---|
| `text` | Send `text` to the general channel (max 500 characters) |
| `/msg name text` | Send an encrypted private message to `name` (including yourself) |
| `/users` | List online users and their endpoints |
| `/quit` | Broadcast `leave` and exit gracefully |

## Run Tests

```bash
ctest --test-dir build --output-on-failure
```
