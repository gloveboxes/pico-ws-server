#ifndef __WEB_SOCKET_MESSAGE_BUILDER_H__
#define __WEB_SOCKET_MESSAGE_BUILDER_H__

#include <cstddef>
#include <stdint.h>

#include "web_socket_frame.h"
#include "web_socket_frame_builder.h"
#include "web_socket_message.h"
#include "pico_ws_server/web_socket_server.h"

class WebSocketHandler;

class WebSocketMessageBuilder {
 public:
  WebSocketMessageBuilder(WebSocketHandler& handler)
      : handler(handler), frame_builder(*this) {}

  bool process(uint8_t byte);
  bool process(const uint8_t* data, size_t size) { return frame_builder.process(data, size); }

  bool acceptFrame(uint8_t opcode, bool final, size_t size) const;
  bool processFrame(WebSocketFrame& frame);

  bool sendMessage(const WebSocketMessage& message);
  WebSocketServer::SendResult enqueueMessage(const WebSocketMessage& message);

 private:
  static constexpr auto MAX_MESSAGE_FRAMES = 1024;

  WebSocketHandler& handler;
  WebSocketFrameBuilder frame_builder;
  std::vector<uint8_t> fragmented_payload;
  WebSocketMessage::Type fragmented_type = WebSocketMessage::UNKNOWN;
  size_t fragment_count = 0;
};

#endif
