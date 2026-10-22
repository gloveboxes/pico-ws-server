#ifndef __WEB_SOCKET_HANDLER_H__
#define __WEB_SOCKET_HANDLER_H__

#include <cstddef>

#include "lwip/pbuf.h"

#include "web_socket_message.h"
#include "web_socket_message_builder.h"

class ClientConnection;

// Only access from lwIP context
class WebSocketHandler {
 public:
  explicit WebSocketHandler(ClientConnection& connection)
      : connection(connection), message_builder(*this) {}

  // Methods below must be called from lwIP-safe context

  bool process(struct pbuf* pb);
  bool process(const uint8_t* data, size_t size);
  size_t maxMessageBytes() const;
  bool sendRaw(const void* data, size_t size);
  WebSocketServer::SendResult enqueueRaw(const void* data, size_t size);
  bool flushSend();

  bool processMessage(WebSocketMessage&& message);

  bool sendMessage(const WebSocketMessage& message);
  WebSocketServer::SendResult enqueueMessage(const WebSocketMessage& message);
  bool close();

  bool isClosing() { return is_closing; }

 private:
  ClientConnection& connection;
  WebSocketMessageBuilder message_builder;
  bool is_closing = false;
};

#endif
