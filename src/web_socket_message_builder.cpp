#include "web_socket_message_builder.h"

#include <memory>
#include <new>
#include <cstring>
#include <stdint.h>

#include "debug.h"
#include "lwip/tcp.h"
#include "web_socket_frame.h"
#include "web_socket_frame_builder.h"
#include "web_socket_handler.h"

bool WebSocketMessageBuilder::process(uint8_t byte) {
  return frame_builder.process(byte);
}

bool WebSocketMessageBuilder::acceptFrame(uint8_t opcode, bool final, size_t size) const {
  if (opcode >= WebSocketMessage::CLOSE) {
    return (opcode == WebSocketMessage::CLOSE || opcode == WebSocketMessage::PING ||
            opcode == WebSocketMessage::PONG) && final && size <= 125 &&
           !(opcode == WebSocketMessage::CLOSE && size == 1);
  }
  if (opcode == 0)
    return fragmented_type != WebSocketMessage::UNKNOWN &&
           fragment_count < MAX_MESSAGE_FRAMES &&
           size <= handler.maxMessageBytes() - fragmented_payload.size();
  return (opcode == WebSocketMessage::TEXT || opcode == WebSocketMessage::BINARY) &&
         fragmented_type == WebSocketMessage::UNKNOWN && size <= handler.maxMessageBytes();
}

bool WebSocketMessageBuilder::processFrame(WebSocketFrame& frame) {
  uint8_t opcode = frame.getOpcode();
  if (opcode != 0) {
    auto type = static_cast<WebSocketMessage::Type>(opcode);
    if (frame.isFinal())
      return handler.processMessage(WebSocketMessage(type, frame.takePayload()));
    fragmented_type = type;
    fragmented_payload = frame.takePayload();
    fragment_count = 1;
    return true;
  }
  fragmented_payload.insert(fragmented_payload.end(), frame.getPayload(),
                            frame.getPayload() + frame.getPayloadSize());
  ++fragment_count;
  if (frame.isFinal()) {
    WebSocketMessage message(fragmented_type, std::move(fragmented_payload));
    fragmented_type = WebSocketMessage::UNKNOWN;
    fragment_count = 0;
    return handler.processMessage(std::move(message));
  }
  return true;
}

bool WebSocketMessageBuilder::sendMessage(const WebSocketMessage& message) {
  if (enqueueMessage(message) != WebSocketServer::SendResult::queued)
    return false;
  // TCP already owns the bytes even if output fails. flushSend retries without
  // enqueueing another copy from sent/poll callbacks.
  handler.flushSend();
  return true;
}

WebSocketServer::SendResult WebSocketMessageBuilder::enqueueMessage(const WebSocketMessage& message) {
  const size_t payload_size = message.getPayloadSize();
  if (payload_size > 64000)
    return WebSocketServer::SendResult::too_large;
  if (payload_size && message.getPayload() == nullptr)
    return WebSocketServer::SendResult::invalid;
  if (message.getType() >= WebSocketMessage::CLOSE && payload_size > 125)
    return WebSocketServer::SendResult::too_large;

  uint8_t header[WebSocketFrameBuilder::MAX_HEADER_SIZE];
  const size_t header_len = frame_builder.makeHeader(/*final=*/true, message.getType(), payload_size, header);
  const size_t frame_size = header_len + payload_size;
  if (frame_size > TCP_SND_BUF)
    return WebSocketServer::SendResult::too_large;

  const uint8_t* payload = message.getPayload();

  // Pack header + payload contiguously so tcp_write sees one frame and avoids partial message errors.
  uint8_t stack_buffer[WebSocketFrameBuilder::MAX_HEADER_SIZE + 256];
  uint8_t* frame_data = stack_buffer;
  std::unique_ptr<uint8_t[]> heap_buffer;
  if (frame_size > sizeof(stack_buffer)) {
    heap_buffer.reset(new (std::nothrow) uint8_t[frame_size]);
    if (!heap_buffer) {
      return WebSocketServer::SendResult::no_memory;
    }
    frame_data = heap_buffer.get();
  }

  std::memcpy(frame_data, header, header_len);
  if (payload_size > 0 && payload) {
    std::memcpy(frame_data + header_len, payload, payload_size);
  }

  return handler.enqueueRaw(frame_data, frame_size);
}
