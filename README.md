# pico-ws-server
WebSockets server implementation for Raspberry Pi Pico W on top of raw lwIP

## Building
Users of this library must provide a CMake library named `lwipopts_provider` which provides the `lwipopts.h` header for lwIP configuration.

Users must also link this library with an implementation of `pico_cyw43_arch` (e.g. `pico_cyw43_arch_lwip_poll`).

Warning: the `pico_cyw43_arch` implementation must allow standard library functions (including `malloc`/`free`) to be called from network workers. Since `pico_cyw43_arch_lwip_threadsafe_background` executes workers within ISRs, it is typically not safe unless you have added a critical section
wrapper around `malloc` and friends.

All application callbacks run in the lwIP context that delivered the event. With `pico_cyw43_arch_lwip_poll` that is the thread calling `cyw43_arch_poll()`; with `pico_cyw43_arch_lwip_threadsafe_background` it is the interrupt-level network worker, so callbacks must then be interrupt-safe.

## Important Usage Warnings

⚠️ **Core Affinity**: All WebSocket server operations (initialization, polling, message sending) must execute on the **same core** where the WiFi/CYW43 driver was initialized. The CYW43 driver maintains core-specific state and is not thread-safe across cores. Violating this requirement will cause undefined behavior, crashes, or data corruption.

⚠️ **Onboard LED**: **Do not use the onboard LED** (`CYW43_WL_GPIO_LED_PIN` / `PICO_DEFAULT_LED_PIN`) while running the WebSocket server. On Pico W boards, the LED is controlled by the CYW43 wireless chip and sharing access with application code can cause WiFi instability, packet loss, or disconnections. Use an external LED on a GPIO pin instead.

## WebSocket Features

### PING/PONG Support (RFC 6455)
The server implements WebSocket PING (opcode 0x9) and PONG (opcode 0xA) control frames per RFC 6455:
- **Automatic PONG responses**: The server automatically replies to client PING frames with PONG frames, echoing the payload
- **Application PING API**: Applications can send PING frames using `sendPing(conn_id, payload, size)` to monitor client liveness
- **PONG notifications**: Register a callback with `setPongCallback()` to receive notifications when PONG frames arrive

See the [pingpong example](example/pingpong.cpp) for a complete demonstration of heartbeat/liveness tracking.

## Security
This server does not currently support HTTPS/WSS

## HTTP Notes
This does not aim to be a general-purpose HTTP server, and only includes minimal HTTP support for the WebSocket handshake. Only `GET / HTTP/1.1` requests are allowed, all other requests will return status `400`/`404`/`405`.

Valid requests which do not request a WebSocket upgrade will be served a static HTML blob. This is defined at compile time via CMake, using the specified file ([see example](example/CMakeLists.txt)). Define `STATIC_HTML_PATH` to point at the directory containing the static HTML, and `STATIC_HTML_FILENAME` as the static HTML filename. Changes to the HTML file will get added at compile time.

**Recent Enhancement**: The server now supports **chunked transfer encoding** for serving larger static HTML files, enabling efficient delivery of content that exceeds single-packet buffers.

### Updating Static HTML
To regenerate the static HTML header from your source HTML file, use the provided script:
```bash
cd example/static_html_generator
./update_static_html.sh
```
This compresses the HTML with gzip and generates a C header file with the embedded content. The script requires `gzip` and `xxd` to be installed. The script runs on macOS, Linux, and Windows with WSL.

## Quick Start

### Basic Setup Pattern
**Complete example:** [example/example.cpp](example/example.cpp)

```cpp
#include "pico_ws_server/web_socket_server.h"

// 1. Define callback functions
void on_connect(WebSocketServer& server, uint32_t conn_id) {
  printf("Client connected: %u\n", conn_id);
}

void on_disconnect(WebSocketServer& server, uint32_t conn_id,
                   WebSocketServer::CloseReason reason, int transport_error) {
  printf("Client disconnected: %u\n", conn_id);
}

void on_message(WebSocketServer& server, uint32_t conn_id, 
                const void* data, size_t len) {
  // Echo the message back
  server.sendMessage(conn_id, data, len);
}

int main() {
  // 2. Create server instance (max 1 connection by default)
  WebSocketServer server(1);
  
  // 3. Register callbacks (all optional, register only what you need)
  server.setConnectCallback(on_connect);
  server.setCloseCallback(on_disconnect);
  server.setMessageCallback(on_message);
  
  // 4. Optional: Configure TCP options
  server.setTcpNoDelay(true);  // Disable Nagle's algorithm for lower latency
  
  // 5. Start listening
  if (!server.startListening(80)) {
    printf("Failed to start server\n");
    return 1;
  }
  
  // 6. Main loop for pico_cyw43_arch_lwip_poll: call both regularly
  while (1) {
    cyw43_arch_poll();  // Delivers messages to the callbacks
    server.poll();      // Retries blocked output, reclaims closed connections
  }
}
```
**Complete example:** [example/pingpong.cpp](example/pingpong.cpp)


### With PING/PONG Liveness Tracking
```cpp
void on_pong(WebSocketServer& server, uint32_t conn_id, 
             const void* data, size_t len) {
  printf("Received PONG from client %u\n", conn_id);
  // Reset liveness counters, update last-seen timestamp, etc.
}

int main() {
  WebSocketServer server(1);
  
  // Register all callbacks including PONG
  server.setConnectCallback(on_connect);
  server.setCloseCallback(on_disconnect);
  server.setMessageCallback(on_message);
  server.setPongCallback(on_pong);  // Track PONG responses
  
  server.startListening(8088);
  
  while (1) {
    cyw43_arch_poll();
    server.poll();
    
    // Periodically send PING to check client liveness
    if (should_send_ping()) {
      server.sendPing(conn_id, nullptr, 0);
    }
  }
}
```

## API Reference

### Server Initialization
- **`WebSocketServer(uint32_t max_connections = 1)`**  
  Constructor. Creates a WebSocket server instance supporting up to `max_connections` simultaneous connections.

- **`bool startListening(uint16_t port)`**  
  Starts the server listening on the specified port. Returns `true` on success.

### Housekeeping
- **`void poll()`**  
  Call routinely in your main loop. It retries output that TCP could not take earlier and reclaims connections that were aborted or whose close grace period expired. Received messages do not wait for it: they reach the callbacks as the data arrives.

### Callbacks
All callbacks receive a `WebSocketServer&` reference and `conn_id` to identify the connection. Use `setCallbackExtra()` to pass custom application state.

Connect, message and PONG callbacks run inside the TCP receive callback that delivered them; the close callback runs there, from a TCP timer or from `poll()`. Every callback may send to, `close()` or `abort()` any connection, including its own: teardown waits until the callback returns.

Prefer to record state in a callback and send from the main loop. On the CYW43 radio, a frame sent from the receive path can exhaust the driver's transmit credit while received frames are still queued in the chip; the driver discards the frames it reads while waiting for credit, and the peer stalls until TCP retransmits them.

#### Connection Lifecycle
- **`void setConnectCallback(ConnectCallback cb)`**  
  Callback signature: `void callback(WebSocketServer& server, uint32_t conn_id)`  
  Called when a new WebSocket connection is established.

- **`void setCloseCallback(CloseCallback cb)`**  
  Callback signature: `void callback(WebSocketServer& server, uint32_t conn_id, CloseReason reason, int transport_error)`  
  Called once when a connection closes. `reason` is the library's cause (peer TCP close, peer WebSocket close, invalid input, PONG backpressure, TCP write/output/other error, close timeout, server abort or local close); `transport_error` is the lwIP error code when one caused the close, otherwise 0. The connection is already gone: sends to it return `closed`.

#### Message Handling
- **`void setMessageCallback(MessageCallback cb)`**  
  Callback signature: `void callback(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len)`  
  Called when a complete message is received. `data` is valid only during the callback and is followed by a null terminator, allowing a TEXT message to be treated as a C string.

#### PING/PONG Monitoring
- **`void setPongCallback(PongCallback cb)`**  
  Callback signature: `void callback(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len)`  
  Called when a PONG frame is received (typically in response to `sendPing()`). If no callback is registered, PONG frames are silently ignored.

#### Custom Application State
- **`void setCallbackExtra(void* arg)`**  
  Store a pointer to custom application data. Retrieve it in callbacks using `getCallbackExtra()`.

- **`void* getCallbackExtra()`**  
  Retrieve the custom application data pointer set with `setCallbackExtra()`.

### Sending Messages

`sendMessage`, `sendPing` and broadcasts return **TCP enqueue success**, not
delivery or flush success. Once `tcp_write` accepts a complete frame, a failed
`tcp_output` leaves it owned by TCP. The library retries output from sent/poll
callbacks and `poll()` without enqueueing another copy.
Application-level consumption acknowledgements remain the application's job.

For bursts, use `enqueueMessage(id, data, size)` for each binary frame, then
`flush(id)` once. Interactive `sendMessage` and control traffic attempt an
immediate flush. Enqueue returns `SendResult::queued`, `would_block`, `closed`,
`invalid`, `too_large`, `no_memory`, or `error`. Only `queued` transfers bytes
to TCP; only `would_block` indicates retryable write backpressure. A failed
flush never authorises resubmitting accepted frames. Fatal transport errors
schedule teardown. Each frame is one contiguous `tcp_write`, assembled in a
buffer the server allocates once and reuses, so backpressure never leaves part
of a frame queued. Frames exceeding the configured `TCP_SND_BUF` cannot be
queued atomically.

#### Unicast (Single Connection)
- **`bool sendMessage(uint32_t conn_id, const char* payload)`**  
  Send a TEXT message. `payload` must be a null-terminated string. Returns `true` on success.

- **`bool sendMessage(uint32_t conn_id, const void* payload, size_t payload_size)`**  
  Send a BINARY message with explicit size. Returns `true` on success.

- **`bool sendPing(uint32_t conn_id, const void* payload = nullptr, size_t payload_size = 0)`**  
  Send a PING control frame with optional payload (up to 125 bytes per RFC 6455). Client should respond with a PONG frame echoing the payload. Returns `true` on success.

#### Broadcast (All Connections)
- **`bool broadcastMessage(const char* payload)`**  
  Send a TEXT message to all connected clients. `payload` must be a null-terminated string. Returns `true` on success.

- **`bool broadcastMessage(const void* payload, size_t payload_size)`**  
  Send a BINARY message to all connected clients with explicit size. Returns `true` on success.

### Connection Management
- **`bool close(uint32_t conn_id)`**  
  Begin graceful shutdown, stopping application dispatch and sends. Returns whether
  the close frame was accepted (or closing already began). Closing state is set
  even if the write fails. From a callback for the same connection, the TCP
  connection closes as soon as the callback returns. Otherwise a one-second grace
  deadline bounds reclamation, checked by `poll()` and the approximately 500 ms
  TCP poll callback.

- **`bool abort(uint32_t conn_id)`**
  Request unconditional teardown. Safe from every callback: a connection is never
  deleted while its own callback is running. Its teardown happens when its receive
  callback returns, or at the next `poll()`/TCP poll. Close notification occurs once
  and the slot is reusable. Destruction detaches all callbacks and releases
  clients/listener without user notifications; destroy the server only outside
  callbacks while its CYW43 context remains usable.

### Message Size Limit

Call `setMaxMessageSize(bytes)` before listening; it returns `false` for zero,
more than 64,000 bytes, or after listening has started. The default is 64,000
bytes. The limit covers a whole message including all its fragments; control
frames keep the RFC 125-byte limit. A larger message closes the connection
rather than silently dropping application bytes. Each connection keeps one
message buffer that grows to the largest message it has received.

PING replies are immediate when possible. One pending PONG (up to 125 bytes)
survives TCP backpressure and is retried without duplicate enqueue; another PING
before that pending reply fits closes the overloaded connection.

### TCP Options
- **`void setTcpNoDelay(bool enabled)`**  
  Enable/disable TCP_NODELAY to control Nagle's algorithm. When enabled (`true`), small packets are sent immediately for lower latency. When disabled (`false`, default), the TCP stack may buffer small writes to reduce network overhead.  
  Call this before `startListening()` or after connections are established.

## Performance Notes
HTTP and WebSocket receive paths traverse pbuf spans once. Frame headers are
decoded once; masking is applied while the payload is copied straight into the
connection's message buffer, where fragments are assembled in place. The
application callback reads that buffer directly, so a received message costs one
payload copy and no heap allocation. Fragmented messages support interleaved
PING/PONG controls and enforce continuation ordering. Connections live in a
fixed set of slots sized by `max_connections`.
HTTP content is flushed once per available burst and remains alive until its last
byte is acknowledged, including backpressure before the first body chunk.

Host regressions in the consuming Z80ROMlessSBC project compile all server sources
against fake lwIP and real Mbed TLS. They verify exact TCP write/flush counts,
split/chained input, the message size limit, callbacks that send, close or abort,
teardown, and that steady-state traffic makes no heap allocations.
These tests do not establish physical Wi-Fi throughput or latency; see
[latency design](LATENCY_OPTIMIZATIONS.md).
