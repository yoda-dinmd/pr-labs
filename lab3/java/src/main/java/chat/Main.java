package chat;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketException;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;

public final class Main {
    private static final String GLOBAL_BROADCAST = "255.255.255.255";
    private static final String LOOPBACK_BROADCAST = "127.255.255.255";

    private Main() {}

    public static void main(String[] args) {
        // Force IPv4 sockets before any java.net class initializes so 0.0.0.0 binding and
        // IPv4 broadcasts interoperate cleanly with peers in other languages.
        System.setProperty("java.net.preferIPv4Stack", "true");

        Path customLogPath = null;
        List<String> positional = new ArrayList<>();
        for (int i = 0; i < args.length; i++) {
            if ("--log".equals(args[i]) && i + 1 < args.length) {
                customLogPath = Path.of(args[++i]);
            } else {
                positional.add(args[i]);
            }
        }

        if (positional.size() < 3 || positional.size() > 4) {
            printUsage();
            System.exit(1);
        }

        String username = positional.get(0);
        if (!Protocol.isValidUsername(username)) {
            System.err.println("Error: Username must be 1-" + Protocol.MAX_SENDER_CHARS
                    + " characters with no whitespace or control characters.");
            System.exit(1);
        }

        int port;
        try {
            port = Integer.parseInt(positional.get(1));
            if (port < 1 || port > 65535) {
                throw new NumberFormatException("Out of range");
            }
        } catch (NumberFormatException e) {
            System.err.println("Error: Port must be an integer between 1 and 65535.");
            System.exit(1);
            return;
        }

        String passphrase = positional.get(2);
        if (passphrase.isEmpty()) {
            System.err.println("Error: Passphrase must not be empty.");
            System.exit(1);
        }

        boolean explicitBroadcast = positional.size() == 4;
        InetAddress broadcastAddress;
        try {
            if (explicitBroadcast) {
                broadcastAddress = InetAddress.getByName(positional.get(3));
                if (!(broadcastAddress instanceof Inet4Address)) {
                    System.err.println("Error: Broadcast address must be an IPv4 address.");
                    System.exit(1);
                    return;
                }
                if ("127.0.0.1".equals(broadcastAddress.getHostAddress())) {
                    System.out.println("Note: 127.0.0.1 is loopback unicast; use 127.255.255.255 to broadcast to multiple peers on this machine.");
                }
            } else {
                broadcastAddress = detectDefaultBroadcastAddress();
            }
        } catch (UnknownHostException e) {
            System.err.println("Error: Invalid broadcast IP address: " + positional.get(3));
            System.exit(1);
            return;
        }

        try (ChatPeer peer = new ChatPeer(
                username,
                port,
                passphrase,
                broadcastAddress,
                explicitBroadcast,
                customLogPath,
                System.out
        )) {
            Runtime.getRuntime().addShutdownHook(new Thread(peer::leaveAndClose, "chat-shutdown-hook"));

            if (!peer.start()) {
                System.exit(1);
            }

            BufferedReader stdIn = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
            peer.runConsole(stdIn);
        } catch (IOException e) {
            System.err.println("Network error: " + e.getMessage());
            System.exit(1);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static InetAddress detectDefaultBroadcastAddress() throws UnknownHostException {
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            if (interfaces != null) {
                for (NetworkInterface nif : Collections.list(interfaces)) {
                    if (!nif.isUp() || nif.isLoopback() || nif.isPointToPoint()) {
                        continue;
                    }
                    for (InterfaceAddress ia : nif.getInterfaceAddresses()) {
                        if (ia.getAddress() instanceof Inet4Address && ia.getBroadcast() != null) {
                            return InetAddress.getByName(GLOBAL_BROADCAST);
                        }
                    }
                }
            }
        } catch (SocketException ignored) {
            // Fall through to loopback broadcast.
        }
        return InetAddress.getByName(LOOPBACK_BROADCAST);
    }

    private static void printUsage() {
        System.out.println("Usage: java -jar target/udp-chat-1.0-SNAPSHOT.jar <name> <port> <passphrase> [broadcast_ip] [--log <file>]");
        System.out.println("  name          Unique username (no spaces)");
        System.out.println("  port          Shared UDP chat port (1-65535)");
        System.out.println("  passphrase    Shared secret for AES-256-GCM private messages");
        System.out.println("  broadcast_ip  Optional IPv4 broadcast address (default: 255.255.255.255, or 127.255.255.255 when loopback-only)");
        System.out.println("  --log <file>  Optional custom datagram log file path");
    }
}
