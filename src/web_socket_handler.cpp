#include "web_socket_handler.h"

#include <cstddef>

#include "lwip/pbuf.h"

#include "client_connection.h"
#include "debug.h"
#include "web_socket_message.h"

bool WebSocketHandler::process(struct pbuf* pb) {
  if (is_closing)
    return false;
  for (const pbuf* span = pb; span; span = span->next) {
    if (!process(static_cast<const uint8_t*>(span->payload), span->len))
      return false;
  }
  return true;
}

bool WebSocketHandler::process(const uint8_t* data, size_t size) {
  if (is_closing)
    return false;
  if (message_builder.process(data, size))
    return true;
  close();
  return false;
}

bool WebSocketHandler::sendRaw(const void* data, size_t size) {
  return connection.sendRaw(data, size);
}

WebSocketServer::SendResult WebSocketHandler::enqueueRaw(const void* data, size_t size) {
  return connection.enqueueRaw(data, size);
}

bool WebSocketHandler::flushSend() {
  return connection.flushSend();
}

size_t WebSocketHandler::maxMessageBytes() const {
  return connection.maxMessageBytes();
}

bool WebSocketHandler::processMessage(WebSocketMessage&& message) {
  switch (message.getType()) {
  case WebSocketMessage::TEXT:
  case WebSocketMessage::BINARY:
    return connection.processWebSocketMessage(std::move(message));

  case WebSocketMessage::CLOSE:
    if (!is_closing) {
      DEBUG("CLOSE requested");
      sendMessage(message);
      is_closing = true;
    }
    return false;

  case WebSocketMessage::PING:
    DEBUG("PING requested");
    return connection.sendWebSocketPong(message.getPayload(), message.getPayloadSize());

  case WebSocketMessage::PONG:
    DEBUG("PONG received");
    return connection.processWebSocketMessage(std::move(message));

  default:
    // Ignore unknown message types
    DEBUG("unknown message type");
    return true;
  }
}

bool WebSocketHandler::sendMessage(const WebSocketMessage& message) {
  if (is_closing) {
    return false;
  }

  return message_builder.sendMessage(message);
}

WebSocketServer::SendResult WebSocketHandler::enqueueMessage(const WebSocketMessage& message) {
  if (is_closing)
    return WebSocketServer::SendResult::closed;
  return message_builder.enqueueMessage(message);
}

bool WebSocketHandler::close() {
  if (is_closing) {
    return true;
  }

  bool queued = message_builder.sendMessage(WebSocketMessage(WebSocketMessage::CLOSE, nullptr, 0));
  is_closing = true;
  return queued;
}
