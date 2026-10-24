#include "client_connection.h"

#include <algorithm>
#include <cstddef>
#include <string.h>
#include <queue>

#include "cyw43_config.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/sys.h"

#include "web_socket_message.h"
#include "web_socket_server_internal.h"
#include "debug.h"

void ClientConnection::popMessages() {
  onPoll();
  for (size_t count = 0; count < limits.dispatchMessages &&
       !message_queue.empty() && !isClosing(); ++count) {
    WebSocketMessage message(std::move(message_queue.front()));
    message_queue.pop();
    queued_bytes -= message.getPayloadSize();
    if (message.getType() == WebSocketMessage::PONG)
      server.onPong(this, message.getPayload(), message.getPayloadSize());
    else
      server.onMessage(this, message.getPayload(), message.getPayloadSize());
  }
}

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
  bool result;
  if (http_handler.isUpgraded()) {
    result = ws_handler.process(pb);
  } else {
    size_t consumed = 0;
    result = http_handler.process(pb, consumed);
    if (result && http_handler.isUpgraded()) {
      server.onUpgrade(this);
      if (abort_requested)
        return true;
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
    }
  }

  return result;
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
  server.onClose(this, http_handler.isUpgraded());
}

bool ClientConnection::isClosing() {
  return abort_requested || http_handler.isClosing() || ws_handler.isClosing();
}

bool ClientConnection::processWebSocketMessage(WebSocketMessage&& message) {
  const size_t next_messages = message_queue.size() + 1;
  const size_t next_bytes = queued_bytes + message.getPayloadSize();
  peak_queued_messages = std::max(peak_queued_messages, next_messages);
  peak_queued_bytes = std::max(peak_queued_bytes, next_bytes);
  if (next_messages > limits.queuedMessages) {
    setCloseReason(WebSocketServer::CloseReason::receive_queue_message_limit);
    DEBUG("WebSocket receive queue message limit exceeded: %u/%u",
          (unsigned)next_messages, (unsigned)limits.queuedMessages);
    return false;
  }
  if (next_bytes > limits.queuedBytes) {
    setCloseReason(WebSocketServer::CloseReason::receive_queue_byte_limit);
    DEBUG("WebSocket receive queue byte limit exceeded: %u/%u",
          (unsigned)next_bytes, (unsigned)limits.queuedBytes);
    return false;
  }
  queued_bytes += message.getPayloadSize();
  message_queue.push(std::move(message));
  return true;
}

bool ClientConnection::sendWebSocketPong(const void* payload, size_t size) {
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
  auto result = ws_handler.enqueueMessage(
      WebSocketMessage(WebSocketMessage::PONG, pong_payload.data(), pong_size));
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

void ClientConnection::processWebSocketPong(const void* payload, size_t size) {
  server.onPong(this, payload, size);
}

bool ClientConnection::sendWebSocketTextMessage(const char* payload) {
  if (!http_handler.isUpgraded()) {
    return false;
  }

  if (!payload) {
    payload = "";
  }

  return ws_handler.sendMessage(WebSocketMessage(WebSocketMessage::TEXT, payload, strlen(payload)));
}

bool ClientConnection::sendWebSocketBinaryMessage(const void* payload, size_t size) {
  if (!http_handler.isUpgraded()) {
    return false;
  }

  return ws_handler.sendMessage(WebSocketMessage(WebSocketMessage::BINARY, payload, size));
}

bool ClientConnection::sendWebSocketPing(const void* payload, size_t size) {
  if (!http_handler.isUpgraded()) {
    return false;
  }

  return ws_handler.sendMessage(WebSocketMessage(WebSocketMessage::PING, payload, size));
}

bool ClientConnection::sendWebSocketMessage(const char* payload) {
  return sendWebSocketTextMessage(payload);
}

bool ClientConnection::sendWebSocketMessage(const void* payload, size_t size) {
  return sendWebSocketBinaryMessage(payload, size);
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

WebSocketServer::SendResult ClientConnection::enqueueWebSocketMessage(const void* payload, size_t size) {
  if (!http_handler.isUpgraded() || abort_requested)
    return WebSocketServer::SendResult::closed;
  return ws_handler.enqueueMessage(WebSocketMessage(WebSocketMessage::BINARY, payload, size));
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
