#ifndef __CLIENT_CONNECTION_H__
#define __CLIENT_CONNECTION_H__

#include <array>
#include <cstddef>
#include <cstdint>

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "http_handler.h"
#include "web_socket_handler.h"

class WebSocketServerInternal;

// Only access from lwIP context
class ClientConnection {
 public:
  ClientConnection(WebSocketServerInternal& server, struct tcp_pcb* pcb, uint32_t id)
      : server(server), pcb(pcb), connection_id(id), http_handler(*this), ws_handler(*this) {}

  uint32_t id() const { return connection_id; }
  bool isUpgraded() { return http_handler.isUpgraded(); }

  // Parses received data and runs the application callbacks for it. Returns
  // false when the caller must tear the connection down.
  bool process(struct pbuf* pb);
  // Teardown waits while a callback for this connection is on the stack.
  bool isProcessing() const { return processing; }

  // onClose tears down this connection, the reference is no longer safe to use
  void onClose();
  // The first reason wins; a transport error may still replace a local close.
  void setCloseReason(WebSocketServer::CloseReason reason, int transport_error = 0) {
    if (close_reason == WebSocketServer::CloseReason::unknown ||
        (close_reason == WebSocketServer::CloseReason::local_close && transport_error))
      close_reason = reason;
    if (transport_error)
      close_transport_error = transport_error;
  }
  WebSocketServer::CloseReason closeReason() const { return close_reason; }
  int closeTransportError() const { return close_transport_error; }

  bool close();
  void abort() {
    if (close_reason == WebSocketServer::CloseReason::local_close)
      close_reason = WebSocketServer::CloseReason::server_abort;
    else
      setCloseReason(WebSocketServer::CloseReason::server_abort);
    abort_requested = true;
  }
  bool abortRequested() const { return abort_requested; }
  bool shouldReap() const;
  void markCloseTimeout();
  void detachAndAbort();

  // Queue one WebSocket frame; sendMessage also attempts a flush.
  WebSocketServer::SendResult enqueueMessage(WebSocketHandler::Opcode opcode,
                                             const void* payload, size_t size);
  bool sendMessage(WebSocketHandler::Opcode opcode, const void* payload, size_t size);
  bool sendPong(const void* payload, size_t size);

  WebSocketServer::SendResult enqueueRaw(const void* data, size_t size);
  bool sendRaw(const void* data, size_t size);
  bool flushSend();
  bool needsSentCallback();
  bool onSent(uint16_t len);
  void onPoll();

  void deliverMessage(const void* payload, size_t size);
  void deliverPong(const void* payload, size_t size);
  size_t maxMessageBytes() const;
  uint8_t* frameBuffer(size_t size);

  void setTcpNoDelay(bool enabled) {
    if (enabled)
      tcp_nagle_disable(pcb);
    else
      tcp_nagle_enable(pcb);
  }

 private:
  WebSocketServerInternal& server;
  struct tcp_pcb* pcb;
  const uint32_t connection_id;
  HTTPHandler http_handler;
  WebSocketHandler ws_handler;

  std::array<uint8_t, 125> pong_payload;
  size_t pong_size = 0;
  bool pong_pending = false;
  bool flush_pending = false;
  bool abort_requested = false;
  bool processing = false;
  bool close_started = false;
  uint32_t close_deadline = 0;
  WebSocketServer::CloseReason close_reason = WebSocketServer::CloseReason::unknown;
  int close_transport_error = 0;
  bool retryPong();
  bool processData(struct pbuf* pb);
};

#endif
