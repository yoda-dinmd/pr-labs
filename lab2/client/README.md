# Coffee Shop TCP Client

Interactive CLI client for the Lab 2 Coffee Shop TCP protocol. Connects to the server over a TCP socket, sends JSON-encoded requests, and listens asynchronously for server responses and notifications.

## Requirements

- **JDK 27** (`maven.compiler.source` / `target` set to `27` in `pom.xml`)
- **Maven** (to resolve `tools.jackson.core:jackson-databind:3.2.4`)

## Building and Running

From `lab2/client/`:

### Using Maven

```bash
mvn compile
mvn exec:java -Dexec.mainClass="Main" -Dexec.args="<host> <port> <client|provider>"
```

Example:

```bash
mvn exec:java -Dexec.mainClass="Main" -Dexec.args="localhost 8080 client"
```

### Using `java` directly

Build the classpath once and run `target/classes`:

```bash
mvn compile dependency:build-classpath -Dmdep.outputFile=target/cp.txt
java -cp "target/classes:$(cat target/cp.txt)" Main <host> <port> <client|provider>
```

### From IntelliJ IDEA

Open `lab2/client/pom.xml` as a Maven project and run `Main` with program arguments:

```text
<host> <port> <client|provider>
```

## Roles

- `client`: Standard customer session. Can request the menu and place orders.
- `provider`: Supplier session. Automatically sends `{"type":"PROVIDER_IDENTIFY"}` on connect so the server recognizes it for deliveries and `LOW_STOCK` notifications.

## Interactive Commands

Once connected, type a command and press Enter:

| Command | Description |
| --- | --- |
| `menu` | Sends `MENU_REQUEST` to fetch current product availability. |
| `order` | Prompts for `Coffee`, `Tea`, and `Croissant` quantities and sends `ORDER_REQUEST`. |
| `deliver` | Prompts for `Coffee`, `Tea`, and `Croissant` quantities and sends `DELIVERY` (for `provider` sessions). |
| `raw` | Prompts for a raw line and sends it verbatim over the socket (useful for testing invalid payloads). |
| `exit` | Closes the connection and exits. |

For `order` and `deliver`, each quantity must be a non-negative integer and at least one item must be greater than `0`.

## Server Messages

A background listener thread prints the raw JSON from the server (`[SERVER]: ...`) followed by a formatted summary for:

- `MENU_RESPONSE` — available `coffee`, `tea`, and `croissant` counts
- `ORDER_ACCEPTED` — assigned `orderId` and `preparationTime` (in seconds)
- `ORDER_REFUSED` — refusal `reason`
- `ORDER_READY` — completed `orderId`
- `LOW_STOCK` — `product` name and `remaining` count
- `ERROR` — `code` and `message`
