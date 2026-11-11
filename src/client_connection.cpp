#include "client_connection.h"

#include <cstddef>
#include <string.h>

#include "cyw43_config.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/sys.h"

#include "web_socket_server_internal.h"
#include "debug.h"

void ClientConnection::onPoll() {
  retryPong();
  flushSend();
}

bool ClientConnection::process(struct pbuf* pb) {
  processing = true;
  bool result = processData(pb);
  processing = false;
  if (!result)
    setCloseReason(WebSocketServer::CloseReason::invalid_websocket_input);
  return result;
}

bool ClientConnection::processData(struct pbuf* pb) {
  if (abort_requested)
    return false;
  // WebSocket frames may share a segment with the upgrade request.
  size_t consumed = 0;
  if (!http_handler.isUpgraded()) {
    if (!http_handler.process(pb, consumed))
      return false;
    if (!http_handler.isUpgraded())
      return true;
    server.onUpgrade(this);
  }
  for (const pbuf* span = pb; span; span = span->next) {
    if (consumed >= span->len) {
      consumed -= span->len;
      continue;
    }
    if (!ws_handler.process(static_cast<const uint8_t*>(span->payload) + consumed,
                            span->len - consumed))
      return false;
    consumed = 0;
  }
  return !abort_requested && !ws_handler.isClosing();
}

bool ClientConnection::sendRaw(const void* data, size_t size) {
  return enqueueRaw(data, size) == WebSocketServer::SendResult::queued;
}

WebSocketServer::SendResult ClientConnection::enqueueRaw(const void* data, size_t size) {
  cyw43_arch_lwip_check();
  using Result = WebSocketServer::SendResult;
  if (abort_requested || pcb == nullptr)
    return Result::closed;
  if (size > UINT16_MAX || size > TCP_SND_BUF)
    return Result::too_large;
  if (size && data == nullptr)
    return Result::invalid;
  if (!size)
    return Result::queued;
  err_t result = tcp_write(pcb, data, (u16_t)size, TCP_WRITE_FLAG_COPY);
  if (result == ERR_MEM)
    return Result::would_block;
  if (result != ERR_OK) {
    setCloseReason(WebSocketServer::CloseReason::tcp_write_error, result);
    abort_requested = true;
    return Result::error;
  }
  flush_pending = true;
  return Result::queued;
}

bool ClientConnection::flushSend() {
  cyw43_arch_lwip_check();

  if (abort_requested || pcb == nullptr)
    return false;
  if (!flush_pending)
    return true;
  // lwIP ignores output requested inside its receive callback and sends when
  // the callback returns. Keep the flush pending so poll() confirms it.
  if (processing)
    return true;
  err_t result = tcp_output(pcb);
  if (result == ERR_OK)
    flush_pending = false;
  else if (result != ERR_MEM && result != ERR_BUF && result != ERR_RTE) {
    setCloseReason(WebSocketServer::CloseReason::tcp_output_error, result);
    abort_requested = true;
  }
  return result == ERR_OK;
}

bool ClientConnection::needsSentCallback() {
  return flush_pending || pong_pending || http_handler.needsSentCallback();
}

bool ClientConnection::onSent(uint16_t len) {
  retryPong();
  flushSend();
  return !abort_requested && http_handler.onSent(len);
}

void ClientConnection::onClose() {
  abort_requested = true;
  server.onClose(this);
}

void ClientConnection::deliverMessage(const void* payload, size_t size) {
  server.onMessage(this, payload, size);
}

void ClientConnection::deliverPong(const void* payload, size_t size) {
  server.onPong(this, payload, size);
}

size_t ClientConnection::maxMessageBytes() const {
  return server.maxMessageBytes();
}

uint8_t* ClientConnection::frameBuffer(size_t size) {
  return server.frameBuffer(size);
}

bool ClientConnection::sendPong(const void* payload, size_t size) {
  if (pong_pending) {
    setCloseReason(WebSocketServer::CloseReason::pong_backpressure);
    DEBUG("WebSocket pending PONG limit exceeded");
    return false;
  }
  memcpy(pong_payload.data(), payload, size);
  pong_size = size;
  pong_pending = true;
  bool result = retryPong();
  flushSend();
  return result && !abort_requested;
}

bool ClientConnection::retryPong() {
  if (!pong_pending)
    return true;
  auto result = ws_handler.enqueue(WebSocketHandler::PONG, pong_payload.data(), pong_size);
  if (result == WebSocketServer::SendResult::would_block)
    return true;
  if (result != WebSocketServer::SendResult::queued) {
    setCloseReason(WebSocketServer::CloseReason::pong_backpressure);
    DEBUG("WebSocket PONG enqueue failed");
    abort_requested = true;
    return false;
  }
  pong_pending = false;
  return !abort_requested;
}

WebSocketServer::SendResult ClientConnection::enqueueMessage(WebSocketHandler::Opcode opcode,
                                                             const void* payload, size_t size) {
  if (!http_handler.isUpgraded() || abort_requested)
    return WebSocketServer::SendResult::closed;
  return ws_handler.enqueue(opcode, payload, size);
}

bool ClientConnection::sendMessage(WebSocketHandler::Opcode opcode, const void* payload, size_t size) {
  if (enqueueMessage(opcode, payload, size) != WebSocketServer::SendResult::queued)
    return false;
  // TCP already owns the bytes even if output fails. flushSend retries without
  // enqueueing another copy from sent/poll callbacks.
  flushSend();
  return true;
}

bool ClientConnection::close() {
  if (!http_handler.isUpgraded()) {
    return false;
  }

  if (!close_started) {
    setCloseReason(WebSocketServer::CloseReason::local_close);
    close_started = true;
    close_deadline = sys_now() + 1000;
  }
  return ws_handler.close();
}

bool ClientConnection::shouldReap() const {
  return abort_requested ||
         (close_started && static_cast<int32_t>(sys_now() - close_deadline) >= 0);
}

void ClientConnection::markCloseTimeout() {
  if (!abort_requested && close_started &&
      static_cast<int32_t>(sys_now() - close_deadline) >= 0 &&
      (close_reason == WebSocketServer::CloseReason::local_close ||
       close_reason == WebSocketServer::CloseReason::unknown))
    close_reason = WebSocketServer::CloseReason::close_timeout;
}

void ClientConnection::detachAndAbort() {
  tcp_arg(pcb, nullptr);
  tcp_recv(pcb, nullptr);
  tcp_sent(pcb, nullptr);
  tcp_err(pcb, nullptr);
  tcp_poll(pcb, nullptr, 0);
  tcp_abort(pcb);
  pcb = nullptr;
}
