#ifndef __PICO_WS_SERVER_WEB_SOCKET_SERVER_H__
#define __PICO_WS_SERVER_WEB_SOCKET_SERVER_H__

#include <cstddef>
#include <memory>
#include <stdint.h>

class WebSocketServerInternal;

// Not multicore safe: use only from the core and context that run lwIP.
class WebSocketServer {
 public:
  enum class SendResult { queued, would_block, closed, invalid, too_large, no_memory, error };
  enum class CloseReason : uint8_t {
    unknown,
    peer_tcp_closed,
    peer_websocket_close,
    invalid_websocket_input,
    pong_backpressure,
    tcp_write_error,
    tcp_output_error,
    tcp_error,
    close_timeout,
    server_abort,
    local_close,
  };
  // Callbacks run in the lwIP context. Connect, message and PONG callbacks run
  // inside the TCP receive callback that delivered them; the close callback
  // runs there, from a TCP timer or from poll(). Every callback may send to,
  // close() or abort() any connection; teardown waits until it returns.
  // Prefer sending from the main loop: on the CYW43 radio a frame sent from
  // the receive path can cost received frames still queued in the chip.
  typedef void (*ConnectCallback)(WebSocketServer& server, uint32_t conn_id);
  // data is valid only during the callback and is followed by a NUL byte, so
  // a TEXT message can be read as a string.
  typedef void (*MessageCallback)(WebSocketServer& server, uint32_t conn_id, const void *data, size_t len);
  // transport_error is the lwIP error when one caused the close, otherwise 0.
  typedef void (*CloseCallback)(WebSocketServer& server, uint32_t conn_id,
                                CloseReason reason, int transport_error);
  typedef void (*PongCallback)(WebSocketServer& server, uint32_t conn_id, const void *data, size_t len);

  WebSocketServer(uint32_t max_connections = 1);
  ~WebSocketServer();

  void setConnectCallback(ConnectCallback cb);
  void setCloseCallback(CloseCallback cb);
  void setMessageCallback(MessageCallback cb);
  void setPongCallback(PongCallback cb);
  void setCallbackExtra(void* arg);
  void* getCallbackExtra();

  bool startListening(uint16_t port);
  // Call routinely: retries blocked output and reclaims aborted or timed-out
  // connections. Received messages do not wait for it.
  void poll();

  // Set TCP_NODELAY option to disable Nagle's algorithm for lower latency.
  // Call this before startListening() or after connections are established.
  // Default is false (Nagle's algorithm enabled).
  void setTcpNoDelay(bool enabled);
  // Set before listening. A larger message disconnects the peer rather than
  // losing bytes. Control frames keep the RFC 125-byte limit.
  bool setMaxMessageSize(size_t bytes);

  // Immediate send attempts a flush and returns TCP enqueue success, not peer
  // delivery. Accepted bytes are retried internally after a flush failure.
  // Send a TEXT message, payload must be a null-terminated string.
  bool sendMessage(uint32_t conn_id, const char* payload);
  // Send a BINARY message
  bool sendMessage(uint32_t conn_id, const void* payload, size_t payload_size);
  // Queue one complete binary frame without flushing. Only queued transfers
  // ownership to TCP; retry would_block after servicing the network.
  SendResult enqueueMessage(uint32_t conn_id, const void* payload, size_t payload_size);
  bool flush(uint32_t conn_id);
  // Send a PING control frame, optional payload echoed back in PONG (up to 125 bytes per RFC)
  bool sendPing(uint32_t conn_id, const void* payload = nullptr, size_t payload_size = 0);

  // Send a TEXT message to all connections, payload must be a null-terminated string
  bool broadcastMessage(const char* payload);
  // Send a BINARY message to all connections
  bool broadcastMessage(const void* payload, size_t payload_size);

  // Begin closing the specified connection.
  // No further application messages are dispatched or sent. Cleanup occurs
  // after a one-second grace period even if the close frame cannot be queued.
  bool close(uint32_t conn_id);
  // Request unconditional teardown, safe in callbacks. It happens when the
  // connection's receive callback returns, or at the next poll().
  bool abort(uint32_t conn_id);

 private:
  std::unique_ptr<WebSocketServerInternal> internal;
  void* callback_extra = nullptr;
};

#endif
