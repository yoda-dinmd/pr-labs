# Local Network UDP P2P Chat (Java 27)

Serverless peer-to-peer LAN chat implemented in **Java 27** directly on `java.net.DatagramSocket`, adhering to the cross-language Lab 3 UDP chat contract.

## Requirements

- **JDK 27** (`maven.compiler.source` / `target` set to `27`)
- **Maven** (resolves `tools.jackson.core:jackson-databind:3.2.4` and builds an executable shaded JAR)

## Building

From `lab3/java/`:

```bash
mvn package
```

This creates an executable fat JAR at `target/udp-chat-1.0-SNAPSHOT.jar`.

## Running

```bash
java -jar target/udp-chat-1.0-SNAPSHOT.jar <name> <port> <passphrase> [broadcast_ip] [--log <file>]
```

### Parameters

| Parameter | Required | Description |
| --- | --- | --- |
| `<name>` | Yes | Unique username (1–64 chars, no whitespace). |
| `<port>` | Yes | Shared UDP chat port (`1–65535`); all peers in a chat use the same port. |
| `<passphrase>` | Yes | Shared secret used to derive the AES-256 key via PBKDF2-HMAC-SHA256. |
| `[broadcast_ip]` | No | IPv4 broadcast address for the general channel. Defaults to `255.255.255.255` when an active LAN interface is present, or `127.255.255.255` when loopback-only. |
| `--log <file>` | No | Custom datagram log path. Defaults to `chat_<name>_<personalPort>.log`. |

### Examples

#### LAN / Phone Hotspot (multiple computers or multiple peers on one computer)

```bash
# Terminal 1
java -jar target/udp-chat-1.0-SNAPSHOT.jar Ana 5000 mySecretPass

# Terminal 2
java -jar target/udp-chat-1.0-SNAPSHOT.jar Ion 5000 mySecretPass
```

If your OS routes broadcasts to the wrong interface (e.g., VPN or Docker bridge), pass your subnet broadcast IP explicitly (such as `192.168.43.255`):

```bash
java -jar target/udp-chat-1.0-SNAPSHOT.jar Ana 5000 mySecretPass 192.168.43.255
```

#### Local Loopback Testing (single or multiple peers on one machine)

- **Send private messages to yourself via loopback:** Run `/msg <your_name> <text>`. The peer encrypts the message with AES-256-GCM and sends a real UDP datagram over `127.0.0.1:<personalPort>` to its own personal socket, decrypts it, logs both `SEND` and `RECV`, and displays it once.
- **Run multiple peers strictly over loopback:** Pass `127.255.255.255` as `[broadcast_ip]` (or run offline, where `127.255.255.255` is chosen automatically):

```bash
java -jar target/udp-chat-1.0-SNAPSHOT.jar Ana 5000 mySecretPass 127.255.255.255
java -jar target/udp-chat-1.0-SNAPSHOT.jar Ion 5000 mySecretPass 127.255.255.255
```

> **Linux (`firewalld`) note:** On distributions like openSUSE where `firewalld` blocks unsolicited incoming UDP on Wi-Fi/Ethernet (`wlp1s0`) by default, even local broadcasts to `255.255.255.255` are dropped by the firewall. Use `127.255.255.255` for same-machine loopback testing, or allow UDP chat traffic in `firewalld` for LAN testing across computers.

## Commands

| Command | Action |
| --- | --- |
| `<text>` | Broadcasts a plaintext message (max 500 chars) to the general channel. |
| `/msg <name> <text>` | Sends an AES-256-GCM encrypted unicast message (max 500 chars) to `<name>` only (including yourself via loopback `127.0.0.1`). |
| `/users` | Lists yourself and all currently online peers with their `IP:PersonalPort` endpoints. |
| `/quit` | Broadcasts a `leave` datagram and exits cleanly. |

## Architecture Overview

- **Two-Socket Model (`ChatPeer.java`):**
  - **Socket 1 (Broadcast Listener):** Bound to `0.0.0.0:<chatPort>` with `SO_REUSEADDR` and `SO_REUSEPORT` so multiple local peers share the general channel port.
  - **Socket 2 (Personal Socket):** Bound to `0.0.0.0:0` (ephemeral OS-assigned port) with `SO_BROADCAST`. Sends all outgoing datagrams and receives incoming unicast messages (`join_ack`, `join_reject`, `chat_private`).
- **Peer Discovery & Liveness (`PeerRegistry.java`, `ChatPeer.java`):**
  - Broadcasts `join` on startup and waits briefly for `join_ack` / `join_reject` (enforcing unique usernames).
  - Broadcasts `heartbeat` every 5 seconds; removes peers silent for 15 seconds.
- **Private Message Cryptography (`Crypto.java`):**
  - Key derivation: `PBKDF2WithHmacSHA256`, salt `"NetworkChatLab3_Salt"`, `100,000` iterations, 256-bit (32-byte) AES key.
  - Encryption: `AES/GCM/NoPadding` with a fresh random 12-byte IV per message and 16-byte authentication tag concatenated as `ciphertext + tag` (Base64-encoded into `payload` and `iv`).
- **Validation & Logging (`Protocol.java`, `DatagramLogger.java`):**
  - Every sent, received, and dropped datagram is logged with ISO timestamp, direction (`SEND` / `RECV` / `DROP`), `ip:port`, message kind/reason, and byte size.
  - Invalid UTF-8, malformed JSON, unknown types, wrong-socket datagrams, messages over 500 characters / 4096 bytes, and failed GCM decryptions are dropped and logged without crashing.
