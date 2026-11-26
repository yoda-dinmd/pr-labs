package chat;

import java.io.BufferedWriter;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;

public final class DatagramLogger implements AutoCloseable {
    private static final DateTimeFormatter TIMESTAMP_FORMAT =
            DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSS");

    private final Path logPath;
    private final BufferedWriter writer;
    private boolean closed;

    public DatagramLogger(Path logPath) throws IOException {
        this.logPath = logPath;
        Path parent = logPath.toAbsolutePath().getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        this.writer = Files.newBufferedWriter(
                logPath,
                StandardCharsets.UTF_8,
                StandardOpenOption.CREATE,
                StandardOpenOption.APPEND
        );
    }

    public Path getLogPath() {
        return logPath;
    }

    public void logSend(InetSocketAddress destination, String kind, int sizeBytes) {
        writeEntry("SEND", destination, kind, sizeBytes);
    }

    public void logRecv(InetSocketAddress source, String kind, int sizeBytes) {
        writeEntry("RECV", source, kind, sizeBytes);
    }

    public void logDrop(InetSocketAddress source, String kind, int sizeBytes) {
        writeEntry("DROP", source, kind, sizeBytes);
    }

    private synchronized void writeEntry(String action, InetSocketAddress endpoint, String kind, int sizeBytes) {
        if (closed) {
            return;
        }
        String timestamp = LocalDateTime.now().format(TIMESTAMP_FORMAT);
        String endpointStr = endpoint.getAddress().getHostAddress() + ":" + endpoint.getPort();
        String line = String.format("%s | %-4s | %s | %s | %d bytes",
                timestamp, action, endpointStr, kind, sizeBytes);
        try {
            writer.write(line);
            writer.newLine();
            writer.flush();
        } catch (IOException ignored) {
            // Logging errors must never crash the chat.
        }
    }

    @Override
    public synchronized void close() {
        if (closed) {
            return;
        }
        closed = true;
        try {
            writer.flush();
            writer.close();
        } catch (IOException ignored) {
            // Best-effort close on shutdown.
        }
    }
}
