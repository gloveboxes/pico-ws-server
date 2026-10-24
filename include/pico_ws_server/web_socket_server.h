#ifndef __PICO_WS_SERVER_WEB_SOCKET_SERVER_H__
#define __PICO_WS_SERVER_WEB_SOCKET_SERVER_H__

#include <cstddef>
#include <memory>
#include <stdint.h>

class WebSocketServerInternal;

// Not multicore safe
class WebSocketServer {
 public:
  enum class SendResult { queued, would_block, closed, invalid, too_large, no_memory, error };
  struct ReceiveLimits {
    size_t messageBytes = 64000;
    size_t queuedBytes = 64000;
    size_t queuedMessages = 16;
    size_t dispatchMessages = 8;
  };
  struct ReceiveQueueStats {
    size_t queuedMessages = 0;
    size_t queuedBytes = 0;
    size_t peakQueuedMessages = 0;
    size_t peakQueuedBytes = 0;
    size_t messageLimit = 0;
    size_t byteLimit = 0;
  };
  enum class CloseReason : uint8_t {
    unknown,
    peer_tcp_closed,
    peer_websocket_close,
    invalid_websocket_input,
    receive_queue_message_limit,
    receive_queue_byte_limit,
    pong_backpressure,
    tcp_write_error,
    tcp_output_error,
    tcp_error,
    close_timeout,
    server_abort,
    local_close,
  };
  typedef void (*ConnectCallback)(WebSocketServer& server, uint32_t conn_id);
  // Note: data can be treated as a null-terminated string if expecting TEXT messages (an extra NULL byte is allocated)
  typedef void (*MessageCallback)(WebSocketServer& server, uint32_t conn_id, const void *data, size_t len);
  typedef void (*CloseCallback)(WebSocketServer& server, uint32_t conn_id);
  typedef void (*CloseDetailsCallback)(WebSocketServer& server, uint32_t conn_id,
                                       CloseReason reason, int transport_error,
                                       size_t queued_messages, size_t queued_bytes,
                                       size_t peak_queued_messages,
                                       size_t peak_queued_bytes,
                                       size_t queue_message_limit,
                                       size_t queue_byte_limit);
  typedef void (*PongCallback)(WebSocketServer& server, uint32_t conn_id, const void *data, size_t len);

  WebSocketServer(uint32_t max_connections = 1);
  ~WebSocketServer();

  // Warning: connect/close callbacks may be called from cyw43 ISR context, use caution with shared data
  // and avoid acquiring mutexes (ideally, only access data that can rely on cyw43 context for exclusion)
  void setConnectCallback(ConnectCallback cb);
  void setCloseCallback(CloseCallback cb);
  // Optional diagnostic callback, invoked immediately before CloseCallback.
  void setCloseDetailsCallback(CloseDetailsCallback cb);
  // Note: unlike connect/close, the message callback will not be called from an ISR, but still holds
  // the cyw43 context lock
  void setMessageCallback(MessageCallback cb);
  // PONG callback runs in the same context as message callback (cyw43 lock held, not ISR)
  void setPongCallback(PongCallback cb);
  void setCallbackExtra(void* arg);
  void* getCallbackExtra();

  bool startListening(uint16_t port);
  // Must be called routinely to process incoming messages (triggers message callback)
  void popMessages();

  // Set TCP_NODELAY option to disable Nagle's algorithm for lower latency.
  // Call this before startListening() or after connections are established.
  // Default is false (Nagle's algorithm enabled).
  void setTcpNoDelay(bool enabled);
  // Set before listening. Oversize messages or queue overflow disconnect the
  // peer rather than silently losing bytes. Control frames retain RFC limits.
  bool setReceiveLimits(const ReceiveLimits& limits);
  // Snapshot current and peak queued application-message usage.
  bool getReceiveQueueStats(uint32_t conn_id, ReceiveQueueStats& stats);

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
  // Request unconditional teardown at the next popMessages(), safe in callbacks.
  bool abort(uint32_t conn_id);

 private:
  std::unique_ptr<WebSocketServerInternal> internal;
  void* callback_extra = nullptr;
};

#endif
