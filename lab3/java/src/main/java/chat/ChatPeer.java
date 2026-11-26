package chat;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.PrintStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.SocketAddress;
import java.net.SocketException;
import java.net.StandardSocketOptions;
import java.nio.file.Path;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

public final class ChatPeer implements AutoCloseable {
    public static final long HEARTBEAT_INTERVAL_SECONDS = 5;
    public static final long PEER_TIMEOUT_SECONDS = 15;
    public static final long JOIN_WAIT_MILLIS = 600;
    private static final int RECEIVE_BUFFER_SIZE = 65_535;
    private static final String LOOPBACK_IPV4 = "127.0.0.1";
    private static final String LOOPBACK_BROADCAST_IPV4 = "127.255.255.255";

    private final String username;
    private final int chatPort;
    private volatile InetAddress broadcastAddress;
    private final boolean explicitBroadcastAddress;

    private final DatagramSocket broadcastSocket; // Socket 1: receives broadcasts on 0.0.0.0:<chatPort>
    private final DatagramSocket personalSocket;  // Socket 2: sends all datagrams & receives unicasts on 0.0.0.0:<ephemeral>
    private final int personalPort;
    private final InetSocketAddress selfLoopbackEndpoint;
    private final Set<InetAddress> localAddresses;

    private final Crypto crypto;
    private final DatagramLogger logger;
    private final PeerRegistry registry = new PeerRegistry();
    private final PrintStream out;

    private final Object sendLock = new Object();
    private final Object outLock = new Object();

    private final AtomicBoolean running = new AtomicBoolean(false);
    private final AtomicBoolean joined = new AtomicBoolean(false);
    private final AtomicBoolean rejected = new AtomicBoolean(false);
    private final AtomicBoolean leaveSent = new AtomicBoolean(false);
    private final AtomicBoolean receivedSelfBroadcast = new AtomicBoolean(false);
    private final CountDownLatch rejectLatch = new CountDownLatch(1);

    private ScheduledExecutorService scheduler;
    private Thread broadcastThread;
    private Thread personalThread;

    public ChatPeer(
            String username,
            int chatPort,
            String passphrase,
            InetAddress broadcastAddress,
            boolean explicitBroadcastAddress,
            Path customLogPath,
            PrintStream out
    ) throws IOException {
        this.username = username;
        this.chatPort = chatPort;
        this.broadcastAddress = broadcastAddress;
        this.explicitBroadcastAddress = explicitBroadcastAddress;
        this.out = out;
        this.crypto = new Crypto(passphrase);
        this.localAddresses = discoverLocalAddresses();

        // Socket 1: shared broadcast listener bound to 0.0.0.0:<chatPort>
        this.broadcastSocket = createBroadcastListenerSocket(chatPort);
        try {
            // Socket 2: personal socket bound to 0.0.0.0:0 with SO_BROADCAST enabled
            this.personalSocket = createPersonalSocket();
        } catch (IOException e) {
            this.broadcastSocket.close();
            throw e;
        }

        this.personalPort = personalSocket.getLocalPort();
        this.selfLoopbackEndpoint = new InetSocketAddress(InetAddress.getByName(LOOPBACK_IPV4), personalPort);

        Path logPath = customLogPath != null
                ? customLogPath
                : Path.of(String.format("chat_%s_%d.log", sanitizeFilename(username), personalPort));
        try {
            this.logger = new DatagramLogger(logPath);
        } catch (IOException e) {
            this.broadcastSocket.close();
            this.personalSocket.close();
            throw e;
        }
    }

    private static DatagramSocket createBroadcastListenerSocket(int port) throws IOException {
        DatagramSocket socket = new DatagramSocket((SocketAddress) null);
        socket.setReuseAddress(true);
        try {
            socket.setOption(StandardSocketOptions.SO_REUSEPORT, true);
        } catch (UnsupportedOperationException | IOException ignored) {
            // SO_REUSEPORT is not supported on all platforms (e.g., Windows uses SO_REUSEADDR).
        }
        socket.bind(new InetSocketAddress(InetAddress.getByName("0.0.0.0"), port));
        return socket;
    }

    private static DatagramSocket createPersonalSocket() throws IOException {
        DatagramSocket socket = new DatagramSocket((SocketAddress) null);
        socket.setBroadcast(true);
        socket.bind(new InetSocketAddress(InetAddress.getByName("0.0.0.0"), 0));
        return socket;
    }

    public int getPersonalPort() {
        return personalPort;
    }

    public Path getLogPath() {
        return logger.getLogPath();
    }

    public boolean start() throws InterruptedException {
        if (!running.compareAndSet(false, true)) {
            return joined.get();
        }

        broadcastThread = new Thread(this::broadcastReceiveLoop, "broadcast-listener-" + username);
        broadcastThread.setDaemon(true);
        broadcastThread.start();

        personalThread = new Thread(this::personalReceiveLoop, "personal-listener-" + username);
        personalThread.setDaemon(true);
        personalThread.start();

        // Announce join to all peers on the shared chat port.
        sendBroadcast(Protocol.encodeJoin(username), Protocol.TYPE_JOIN);

        // Wait briefly for existing peers to send join_ack or join_reject.
        rejectLatch.await(JOIN_WAIT_MILLIS, TimeUnit.MILLISECONDS);
        if (rejected.get()) {
            println("Error: Name '" + username + "' is already in use. Cannot join the chat.");
            closeWithoutLeave();
            return false;
        }

        joined.set(true);

        scheduler = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "peer-scheduler-" + username);
            t.setDaemon(true);
            return t;
        });
        scheduler.scheduleAtFixedRate(
                () -> sendBroadcast(Protocol.encodeHeartbeat(username), Protocol.TYPE_HEARTBEAT),
                HEARTBEAT_INTERVAL_SECONDS,
                HEARTBEAT_INTERVAL_SECONDS,
                TimeUnit.SECONDS
        );
        scheduler.scheduleAtFixedRate(
                this::expireSilentPeers,
                1,
                1,
                TimeUnit.SECONDS
        );

        println(String.format(
                "Joined chat as '%s' (chat port: %d, personal port: %d, broadcast: %s, log: %s)",
                username, chatPort, personalPort, broadcastAddress.getHostAddress(), logger.getLogPath()
        ));
        if (!receivedSelfBroadcast.get()) {
            println("Warning: Did not receive our own broadcast on "
                    + broadcastAddress.getHostAddress() + ":" + chatPort
                    + " (likely blocked by local firewall).");
            println("  - For local loopback testing on this machine, pass 127.255.255.255 as the 4th argument.");
            println("  - For LAN chat across machines, open UDP port " + chatPort
                    + " (and peer ephemeral ports) in firewalld.");
        }
        printOnlineUsers();
        return true;
    }

    public void runConsole(BufferedReader reader) throws IOException {
        String line;
        while (running.get() && (line = reader.readLine()) != null) {
            if (!handleUserInput(line)) {
                break;
            }
        }
    }

    /**
     * Processes one line of user input. Returns false if the user requested /quit.
     */
    public boolean handleUserInput(String rawLine) {
        if (rawLine == null) {
            return false;
        }
        String trimmed = rawLine.trim();
        if (trimmed.isEmpty()) {
            return true;
        }

        if (!trimmed.startsWith("/")) {
            sendPublicMessage(rawLine);
            return true;
        }

        if (trimmed.equals("/quit")) {
            leaveAndClose();
            return false;
        }

        if (trimmed.equals("/users")) {
            printOnlineUsers();
            return true;
        }

        if (trimmed.equals("/msg") || trimmed.startsWith("/msg ")) {
            handlePrivateCommand(trimmed);
            return true;
        }

        println("Unknown command. Commands: <text>, /msg <name> <text>, /users, /quit");
        return true;
    }

    private void handlePrivateCommand(String commandLine) {
        String rest = commandLine.length() > 4 ? commandLine.substring(4).trim() : "";
        int firstSpace = findFirstWhitespace(rest);
        if (firstSpace <= 0) {
            println("Usage: /msg <name> <text>");
            return;
        }
        String targetName = rest.substring(0, firstSpace);
        String text = rest.substring(firstSpace).stripLeading();
        if (text.isEmpty()) {
            println("Usage: /msg <name> <text>");
            return;
        }
        sendPrivateMessage(targetName, text);
    }

    private static int findFirstWhitespace(String s) {
        for (int i = 0; i < s.length(); i++) {
            if (Character.isWhitespace(s.charAt(i))) {
                return i;
            }
        }
        return -1;
    }

    public void sendPublicMessage(String text) {
        if (text.length() > Protocol.MAX_MESSAGE_CHARS) {
            println("Error: Message exceeds " + Protocol.MAX_MESSAGE_CHARS
                    + " characters (" + text.length() + "/" + Protocol.MAX_MESSAGE_CHARS + ").");
            return;
        }
        byte[] payload = Protocol.encodePublicChat(username, text);
        if (sendBroadcast(payload, Protocol.TYPE_CHAT_PUBLIC)) {
            println("[general] " + username + ": " + text);
        }
    }

    public void sendPrivateMessage(String targetName, String text) {
        if (text.length() > Protocol.MAX_MESSAGE_CHARS) {
            println("Error: Message exceeds " + Protocol.MAX_MESSAGE_CHARS
                    + " characters (" + text.length() + "/" + Protocol.MAX_MESSAGE_CHARS + ").");
            return;
        }

        boolean toSelf = targetName.equals(username);
        InetSocketAddress targetEndpoint;
        if (toSelf) {
            // Send directly to our own personal socket via IPv4 loopback (127.0.0.1:<personalPort>).
            targetEndpoint = selfLoopbackEndpoint;
        } else {
            Optional<InetSocketAddress> resolved = registry.getEndpoint(targetName);
            if (resolved.isEmpty()) {
                println("Error: User '" + targetName + "' is not online.");
                return;
            }
            targetEndpoint = resolved.get();
        }

        Crypto.EncryptedPayload encrypted;
        try {
            encrypted = crypto.encrypt(text);
        } catch (Crypto.CryptoException e) {
            println("Error: Could not encrypt private message: " + e.getMessage());
            return;
        }

        byte[] datagram = Protocol.encodePrivateChat(username, encrypted.payloadBase64(), encrypted.ivBase64());
        if (sendUnicast(targetEndpoint, datagram, Protocol.TYPE_CHAT_PRIVATE)) {
            // For messages sent to self over loopback, Socket 2 will receive, decrypt, and print it once.
            if (!toSelf) {
                println("[private -> " + targetName + "] " + username + ": " + text);
            }
        }
    }

    public void printOnlineUsers() {
        List<PeerRegistry.PeerEntry> peers = registry.snapshot();
        StringBuilder sb = new StringBuilder();
        sb.append("Online users (").append(peers.size() + 1).append("):\n");
        sb.append("  - ").append(username).append(" (you) [")
                .append(selfLoopbackEndpoint.getAddress().getHostAddress())
                .append(":").append(selfLoopbackEndpoint.getPort()).append("]");
        for (PeerRegistry.PeerEntry peer : peers) {
            sb.append("\n  - ").append(peer.name()).append(" [")
                    .append(peer.endpoint().getAddress().getHostAddress())
                    .append(":").append(peer.endpoint().getPort()).append("]");
        }
        println(sb.toString());
    }

    private void broadcastReceiveLoop() {
        byte[] buffer = new byte[RECEIVE_BUFFER_SIZE];
        while (running.get()) {
            DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
            try {
                broadcastSocket.receive(packet);
            } catch (IOException e) {
                if (!running.get()) {
                    break;
                }
                continue;
            }
            handleBroadcastPacket(packet);
        }
    }

    private void personalReceiveLoop() {
        byte[] buffer = new byte[RECEIVE_BUFFER_SIZE];
        while (running.get()) {
            DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
            try {
                personalSocket.receive(packet);
            } catch (IOException e) {
                if (!running.get()) {
                    break;
                }
                continue;
            }
            handlePersonalPacket(packet);
        }
    }

    private void handleBroadcastPacket(DatagramPacket packet) {
        InetSocketAddress source = new InetSocketAddress(packet.getAddress(), packet.getPort());
        int length = packet.getLength();

        Protocol.ParseResult parsed = Protocol.parse(packet.getData(), length, true);
        if (parsed instanceof Protocol.ParseResult.Invalid invalid) {
            logger.logDrop(source, invalid.reason(), length);
            return;
        }

        Protocol.Message message = ((Protocol.ParseResult.Ok) parsed).message();
        if (isSelfEndpoint(source)) {
            // Recognize our own reflected broadcast by matching our unique personal port on a local IP.
            receivedSelfBroadcast.set(true);
            logger.logRecv(source, message.type() + " (self)", length);
            return;
        }

        logger.logRecv(source, message.type(), length);
        long now = System.currentTimeMillis();

        switch (message) {
            case Protocol.Join(String sender) -> {
                if (sender.equals(username)) {
                    sendUnicast(source, Protocol.encodeJoinReject(username), Protocol.TYPE_JOIN_REJECT);
                    return;
                }
                PeerRegistry.JoinResult joinResult = registry.registerOnJoin(sender, source, now);
                if (joinResult == PeerRegistry.JoinResult.ADDED) {
                    println("* " + sender + " joined the chat");
                    sendUnicast(source, Protocol.encodeJoinAck(username), Protocol.TYPE_JOIN_ACK);
                } else if (joinResult == PeerRegistry.JoinResult.REFRESHED) {
                    sendUnicast(source, Protocol.encodeJoinAck(username), Protocol.TYPE_JOIN_ACK);
                }
            }
            case Protocol.Heartbeat(String sender) -> {
                if (sender.equals(username)) {
                    return;
                }
                boolean added = registry.refreshFromHeartbeat(sender, source, now);
                if (added) {
                    println("* " + sender + " joined the chat");
                }
            }
            case Protocol.Leave(String sender) -> {
                if (registry.removeIfEndpointMatches(sender, source)) {
                    println("* " + sender + " left the chat");
                }
            }
            case Protocol.ChatPublic(String sender, String text) -> {
                if (!sender.equals(username)) {
                    PeerRegistry.JoinResult joinResult = registry.registerOnJoin(sender, source, now);
                    if (joinResult == PeerRegistry.JoinResult.ADDED) {
                        println("* " + sender + " joined the chat");
                    }
                }
                println("[general] " + sender + ": " + text);
            }
            case Protocol.JoinAck _, Protocol.JoinReject _, Protocol.ChatPrivate _ -> {
                // Already filtered by Protocol.parse(..., fromBroadcastSocket = true).
            }
        }
    }

    private void handlePersonalPacket(DatagramPacket packet) {
        InetSocketAddress source = new InetSocketAddress(packet.getAddress(), packet.getPort());
        int length = packet.getLength();

        Protocol.ParseResult parsed = Protocol.parse(packet.getData(), length, false);
        if (parsed instanceof Protocol.ParseResult.Invalid invalid) {
            logger.logDrop(source, invalid.reason(), length);
            return;
        }

        Protocol.Message message = ((Protocol.ParseResult.Ok) parsed).message();
        boolean fromSelf = isSelfEndpoint(source);
        long now = System.currentTimeMillis();

        switch (message) {
            case Protocol.JoinAck(String sender) -> {
                logger.logRecv(source, fromSelf ? "join_ack (self)" : Protocol.TYPE_JOIN_ACK, length);
                if (!fromSelf && !sender.equals(username)) {
                    registry.putOrUpdateFromAck(sender, source, now);
                }
            }
            case Protocol.JoinReject(String sender) -> {
                logger.logRecv(source, fromSelf ? "join_reject (self)" : Protocol.TYPE_JOIN_REJECT, length);
                if (!fromSelf && !joined.get()) {
                    rejected.set(true);
                    rejectLatch.countDown();
                }
            }
            case Protocol.ChatPrivate(String sender, String payload, String iv) -> {
                String plaintext;
                try {
                    plaintext = crypto.decrypt(payload, iv);
                } catch (Crypto.CryptoException e) {
                    logger.logDrop(source, "decrypt_failed (" + e.getMessage() + ")", length);
                    return;
                }

                if (plaintext.length() > Protocol.MAX_MESSAGE_CHARS) {
                    logger.logDrop(source, "too_long (decrypted > " + Protocol.MAX_MESSAGE_CHARS + " chars)", length);
                    return;
                }

                logger.logRecv(source, fromSelf ? "chat_private (self)" : Protocol.TYPE_CHAT_PRIVATE, length);
                if (!fromSelf && !sender.equals(username)) {
                    registry.refreshIfEndpointMatches(sender, source, now);
                }
                println("[private] " + sender + ": " + plaintext);
            }
            case Protocol.Join _, Protocol.Heartbeat _, Protocol.Leave _, Protocol.ChatPublic _ -> {
                // Already filtered by Protocol.parse(..., fromBroadcastSocket = false).
            }
        }
    }

    private void expireSilentPeers() {
        long now = System.currentTimeMillis();
        List<String> expired = registry.removeExpired(now, TimeUnit.SECONDS.toMillis(PEER_TIMEOUT_SECONDS));
        for (String peerName : expired) {
            println("* " + peerName + " left the chat");
        }
    }

    private boolean sendBroadcast(byte[] data, String kind) {
        InetSocketAddress destination = new InetSocketAddress(broadcastAddress, chatPort);
        try {
            sendRaw(destination, data, kind);
            return true;
        } catch (IOException e) {
            // If default 255.255.255.255 fails because no external route exists, fall back to loopback broadcast.
            if (!explicitBroadcastAddress && !LOOPBACK_BROADCAST_IPV4.equals(broadcastAddress.getHostAddress())) {
                try {
                    InetAddress fallback = InetAddress.getByName(LOOPBACK_BROADCAST_IPV4);
                    broadcastAddress = fallback;
                    println("Broadcast to " + destination.getAddress().getHostAddress()
                            + " failed (" + e.getMessage() + "); falling back to loopback broadcast "
                            + LOOPBACK_BROADCAST_IPV4);
                    sendRaw(new InetSocketAddress(fallback, chatPort), data, kind);
                    return true;
                } catch (IOException fallbackErr) {
                    println("Error sending " + kind + ": " + fallbackErr.getMessage());
                    return false;
                }
            }
            if (running.get()) {
                println("Error sending " + kind + ": " + e.getMessage());
            }
            return false;
        }
    }

    private boolean sendUnicast(InetSocketAddress destination, byte[] data, String kind) {
        try {
            sendRaw(destination, data, kind);
            return true;
        } catch (IOException e) {
            if (running.get()) {
                println("Error sending " + kind + " to " + destination.getAddress().getHostAddress()
                        + ":" + destination.getPort() + ": " + e.getMessage());
            }
            return false;
        }
    }

    private void sendRaw(InetSocketAddress destination, byte[] data, String kind) throws IOException {
        DatagramPacket packet = new DatagramPacket(data, data.length, destination);
        synchronized (sendLock) {
            personalSocket.send(packet);
            logger.logSend(destination, kind, data.length);
        }
    }

    private boolean isSelfEndpoint(InetSocketAddress source) {
        if (source.getPort() != personalPort) {
            return false;
        }
        InetAddress addr = source.getAddress();
        if (addr.isLoopbackAddress() || addr.isAnyLocalAddress() || localAddresses.contains(addr)) {
            return true;
        }
        try {
            return NetworkInterface.getByInetAddress(addr) != null;
        } catch (SocketException e) {
            return false;
        }
    }

    private static Set<InetAddress> discoverLocalAddresses() {
        Set<InetAddress> addresses = new HashSet<>();
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            if (interfaces != null) {
                for (NetworkInterface nif : Collections.list(interfaces)) {
                    addresses.addAll(Collections.list(nif.getInetAddresses()));
                }
            }
        } catch (SocketException ignored) {
            // Fall back to loopback and dynamic NetworkInterface.getByInetAddress check.
        }
        return addresses;
    }

    public void leaveAndClose() {
        if (joined.get() && leaveSent.compareAndSet(false, true) && !personalSocket.isClosed()) {
            sendBroadcast(Protocol.encodeLeave(username), Protocol.TYPE_LEAVE);
        }
        closeWithoutLeave();
    }

    private void closeWithoutLeave() {
        running.set(false);
        if (scheduler != null) {
            scheduler.shutdownNow();
        }
        broadcastSocket.close();
        personalSocket.close();
        logger.close();
    }

    @Override
    public void close() {
        leaveAndClose();
    }

    private void println(String text) {
        synchronized (outLock) {
            out.println(text);
            out.flush();
        }
    }

    private static String sanitizeFilename(String name) {
        return name.replaceAll("[^a-zA-Z0-9._-]", "_");
    }
}
