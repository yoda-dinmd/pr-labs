package chat;

import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;

public final class PeerRegistry {

    public record PeerEntry(String name, InetSocketAddress endpoint, long lastSeenMillis) {}

    public enum JoinResult {
        ADDED,
        REFRESHED,
        CONFLICT_EXISTING_ENDPOINT
    }

    private final ConcurrentHashMap<String, PeerEntry> peers = new ConcurrentHashMap<>();

    public JoinResult registerOnJoin(String name, InetSocketAddress endpoint, long nowMillis) {
        JoinResult[] result = new JoinResult[1];
        peers.compute(name, (k, existing) -> {
            if (existing == null) {
                result[0] = JoinResult.ADDED;
                return new PeerEntry(name, endpoint, nowMillis);
            }
            if (existing.endpoint().equals(endpoint)) {
                result[0] = JoinResult.REFRESHED;
                return new PeerEntry(name, endpoint, nowMillis);
            }
            // Keep existing active peer to prevent a rejected duplicate joiner from poisoning routing.
            result[0] = JoinResult.CONFLICT_EXISTING_ENDPOINT;
            return existing;
        });
        return result[0];
    }

    public void putOrUpdateFromAck(String name, InetSocketAddress endpoint, long nowMillis) {
        peers.put(name, new PeerEntry(name, endpoint, nowMillis));
    }

    public boolean refreshFromHeartbeat(String name, InetSocketAddress endpoint, long nowMillis) {
        boolean[] added = new boolean[1];
        peers.compute(name, (k, existing) -> {
            if (existing == null) {
                added[0] = true;
            }
            return new PeerEntry(name, endpoint, nowMillis);
        });
        return added[0];
    }

    public void refreshIfEndpointMatches(String name, InetSocketAddress endpoint, long nowMillis) {
        peers.computeIfPresent(name, (k, existing) ->
                existing.endpoint().equals(endpoint)
                        ? new PeerEntry(name, endpoint, nowMillis)
                        : existing
        );
    }

    public boolean removeIfEndpointMatches(String name, InetSocketAddress endpoint) {
        boolean[] removed = new boolean[1];
        peers.computeIfPresent(name, (k, existing) -> {
            if (existing.endpoint().equals(endpoint)) {
                removed[0] = true;
                return null;
            }
            return existing;
        });
        return removed[0];
    }

    public List<String> removeExpired(long nowMillis, long timeoutMillis) {
        List<String> expired = new ArrayList<>();
        for (PeerEntry entry : peers.values()) {
            if (nowMillis - entry.lastSeenMillis() >= timeoutMillis) {
                if (peers.remove(entry.name(), entry)) {
                    expired.add(entry.name());
                }
            }
        }
        expired.sort(String::compareTo);
        return expired;
    }

    public Optional<InetSocketAddress> getEndpoint(String name) {
        PeerEntry entry = peers.get(name);
        return entry == null ? Optional.empty() : Optional.of(entry.endpoint());
    }

    public List<PeerEntry> snapshot() {
        List<PeerEntry> list = new ArrayList<>(peers.values());
        list.sort(Comparator.comparing(PeerEntry::name));
        return list;
    }
}
