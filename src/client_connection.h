#ifndef __CLIENT_CONNECTION_H__
#define __CLIENT_CONNECTION_H__

#include <cstddef>
#include <queue>
#include <array>

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "http_handler.h"
#include "web_socket_handler.h"
#include "web_socket_message.h"

class WebSocketServerInternal;

// Only access from lwIP context
class ClientConnection {
 public:
  ClientConnection(WebSocketServerInternal& server, struct tcp_pcb* pcb,
                   const WebSocketServer::ReceiveLimits& limits)
      : server(server), pcb(pcb), http_handler(*this), ws_handler(*this), limits(limits) {}

  // onClose tears down this connection, the reference is no longer safe to use
  void onClose();
  bool isClosing();

  bool processWebSocketMessage(WebSocketMessage&& message);
  void processWebSocketPong(const void* payload, size_t size);

  bool sendWebSocketTextMessage(const char* payload);
  bool sendWebSocketBinaryMessage(const void* payload, size_t size);
  bool sendWebSocketPing(const void* payload, size_t size);
  bool sendWebSocketPong(const void* payload, size_t size);

  bool sendWebSocketMessage(const char* payload);
  bool sendWebSocketMessage(const void* payload, size_t size);

  bool close();
  void abort() { abort_requested = true; }
  bool abortRequested() const { return abort_requested; }
  bool shouldReap() const;
  bool isProcessing() const { return processing; }
  void detachAndAbort();
  WebSocketServer::SendResult enqueueWebSocketMessage(const void* payload, size_t size);
  WebSocketServer::SendResult enqueueRaw(const void* data, size_t size);

  void popMessages();
  bool process(struct pbuf* pb);
  bool sendRaw(const void* data, size_t size);
  bool flushSend();
  bool needsSentCallback();
  bool onSent(uint16_t len);
  void onPoll();
  size_t maxMessageBytes() const { return limits.messageBytes; }
  void setTcpNoDelay(bool enabled) {
    if (enabled)
      tcp_nagle_disable(pcb);
    else
      tcp_nagle_enable(pcb);
  }

 private:
  WebSocketServerInternal& server;
  struct tcp_pcb* pcb;
  HTTPHandler http_handler;
  WebSocketHandler ws_handler;

  std::queue<WebSocketMessage> message_queue;
  const WebSocketServer::ReceiveLimits limits;
  size_t queued_bytes = 0;
  std::array<uint8_t, 125> pong_payload;
  size_t pong_size = 0;
  bool pong_pending = false;
  bool flush_pending = false;
  bool abort_requested = false;
  bool processing = false;
  bool close_started = false;
  uint32_t close_deadline = 0;
  bool retryPong();
  bool processData(struct pbuf* pb);
};

#endif
