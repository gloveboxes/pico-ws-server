# WebSocket Latency Optimizations

## Overview
This document describes optimizations made to the pico-ws-server library to minimize transmission latency for real-time applications like terminal emulators.

## Changes Made

### 1. TCP_NODELAY Support (Optional Nagle Disable)

**Files Modified:**
- `include/pico_ws_server/web_socket_server.h`
- `src/web_socket_server.cpp`
- `src/web_socket_server_internal.h`
- `src/web_socket_server_internal.cpp`

**Change:**
Added `setTcpNoDelay(bool enabled)` method to allow applications to disable Nagle's algorithm on WebSocket connections.

**API:**
```cpp
WebSocketServer server(1);
server.setTcpNoDelay(true);  // Disable Nagle's algorithm
server.startListening(8088);
```

**Why:**
Nagle's algorithm buffers small packets to improve network efficiency, but causes delays (up to 200ms) in interactive applications. Disabling it ensures data is sent immediately.

**Implementation:**
- Added `tcp_nodelay` flag to `WebSocketServerInternal`
- Calls `tcp_nagle_disable(pcb)` on new connections when enabled
- Default is `false` (Nagle enabled) for backward compatibility

---

### 2. Separate TCP Enqueue from Flush

**Files Modified:**
- `src/web_socket_message_builder.cpp`

**Change:**
`enqueueMessage()` returns an explicit result. A complete accepted frame belongs
to TCP, even if a subsequent `tcp_output()` fails. `sendMessage()` and `sendPing()`
return enqueue success and attempt immediate output. They no longer report an
ambiguous failure that could prompt the caller to enqueue a duplicate.

For bursts, enqueue all accepted frames and call `flush()` once. Retry rejected
`would_block` frames only; never resubmit accepted frames merely because output
failed. Pending flushes are retried from sent/poll callbacks and `popMessages()`.
Fatal errors schedule teardown. Peer consumption still requires application ACKs.

---

### 3. Broadcast Error Checking

**Files Modified:**
- `src/web_socket_server_internal.cpp`

**Change:**
Modified `broadcastMessage()` to track and return success/failure of individual connection sends.

**Before:**
```cpp
for (const auto& [_, connection] : connection_by_id) {
    connection->sendWebSocketMessage(payload, payload_size);
}
return true;  // Always returns true
```

**After:**
```cpp
bool all_success = true;
for (const auto& [_, connection] : connection_by_id) {
    if (!connection->sendWebSocketMessage(payload, payload_size)) {
        all_success = false;
    }
}
return all_success;
```

**Why:**
Applications need to know if broadcast failed so they can handle data loss appropriately (log, retry, or drop).

---

### 4. Improved TCP Write Behavior

**Files Modified:**
- `src/client_connection.cpp`

**Change:**
Enhanced documentation about `TCP_WRITE_FLAG_COPY` behavior and PSH flag handling.

**Code:**
```cpp
// Use TCP_WRITE_FLAG_COPY to copy data, and omit TCP_WRITE_FLAG_MORE to signal this is complete data
// that should be pushed immediately (sets PSH flag)
return tcp_write(pcb, data, size, TCP_WRITE_FLAG_COPY) == ERR_OK;
```

**Why:**
Each WebSocket frame uses one contiguous `TCP_WRITE_FLAG_COPY` write, preserving
atomic enqueue and temporary-buffer lifetime safety. Omitting
`TCP_WRITE_FLAG_MORE` supplies a push hint; it does not guarantee immediate peer
delivery or override congestion/window limits. A 261-byte terminal application
message plus its four-byte WebSocket header fits the existing 270-byte stack
buffer, without a temporary heap frame.

### 5. Bounded, Lower-Copy Receive and Teardown

HTTP and WebSocket paths walk pbuf spans once rather than rescanning the chain
for every byte. Header decode is cached, and payloads are unmasked while copying.
Unfragmented payload vectors move into the dispatch queue, including their
reserved NUL terminator, without per-frame heap objects or list nodes.
Fragmented messages retain protocol validation and interleaved control frames.

Receive message/queue limits and per-connection dispatch budgets bound service
work; overflow disconnects explicitly. PING backpressure retains one bounded
PONG, retried without duplicate enqueue. `abort()` defers teardown to a safe
boundary, while graceful close has a one-second reclamation deadline and a
roughly 500 ms TCP polling fallback. HTTP content remains alive through its
final ACK and tolerates backpressure before the first body chunk.

---

## Performance Impact

### Verified Host Behaviour
- Four terminal output frames produce four 265-byte TCP writes and one flush.
- Accepted frames survive transient output failures without duplicate enqueue.
- Ordinary receive payload ownership transfers without a second payload copy.
- Queue bounds, dispatch fairness and callback-safe slot reclamation are tested.

These are functional/operation-count results, not hardware latency measurements.
No physical Wi-Fi throughput or key-to-echo latency improvement is claimed.

### When to Use
These optimizations are ideal for:
- Terminal emulators
- Real-time chat applications
- Game state updates
- Live data streaming
- Any application where latency matters more than bandwidth efficiency

### Trade-offs
- **Increased packet overhead:** More small packets means more TCP/IP headers
- **Potential network congestion:** More frequent transmissions
- **Battery impact:** More radio activity on mobile devices

For most interactive applications on local networks, these trade-offs are acceptable.

---

## Usage Example

```cpp
#include "pico_ws_server/web_socket_server.h"

// Create server
WebSocketServer server(1);

// Enable low-latency mode
server.setTcpNoDelay(true);

// Set up callbacks
server.setMessageCallback(on_message);
server.setConnectCallback(on_connect);

// Start listening
if (!server.startListening(8088)) {
    printf("Failed to start server\n");
    return;
}

// In your main loop
while (true) {
    cyw43_arch_poll();
    server.popMessages();
    
    // Send data
    if (!server.broadcastMessage(data, len)) {
        printf("Send failed - network congestion or buffer full\n");
        // Handle error: log, retry, or drop data
    }
}
```

---

## Testing

To verify latency improvements:

The consuming Z80ROMlessSBC project's native `websocket_*` tests compile all
server sources against fake lwIP and real SDK Mbed TLS. Configure native tests
with `PICO_SDK_PATH` to enable them. They exercise exact write/output counts,
write/flush errors, chained and split pbufs, empty and fragmented messages,
interleaved controls, malformed frames, limits, callback reentrancy, wrapped
close deadlines, slot reuse and HTTP backpressure/final acknowledgements.

For hardware acceptance, record sustained terminal bytes/second, key-to-echo
latency and maximum network-poll gap. Compare idle, disk reads, cached disk writes
and flash commits separately. Check disconnect/reconnect under load and watch
for explicit overflow/timeout diagnostics. Do not infer a disk bottleneck or
latency target from host tests alone.

---

## Future Considerations

1. Add hardware-backed measurements before changing lwIP pools or window sizes.
2. Keep application consumption ACKs separate from TCP enqueue/output state.
3. Consider additional counters if live measurements show unexplained stalls;
   avoid speculative zero-copy buffers and their lifetime complexity.

---

## References

- [lwIP TCP Options](https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html)
- [RFC 896: Nagle's Algorithm](https://www.rfc-editor.org/rfc/rfc896)
- [TCP_NODELAY and Small Buffers](https://www.extrahop.com/company/blog/2016/tcp-nodelay-nagle-quickack-best-practices/)
