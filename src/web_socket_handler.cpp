#include "web_socket_handler.h"

#include <cstddef>
#include <cstring>

#include "lwip/tcp.h"

#include "client_connection.h"
#include "debug.h"

namespace {

constexpr uint8_t FIN_BIT = 0x80;
constexpr uint8_t RESERVED_BITS = 0x70;
constexpr uint8_t OPCODE_MASK = 0x0f;
constexpr uint8_t MASK_BIT = 0x80;
constexpr uint8_t LENGTH_MASK = 0x7f;
constexpr uint8_t LENGTH_16 = 126;
constexpr uint8_t LENGTH_64 = 127;

} // namespace

bool WebSocketHandler::process(const uint8_t* data, size_t size) {
  bool valid = true;
  // A callback for an earlier message may have closed the connection.
  while (valid && size && !is_closing && !connection.abortRequested())
    valid = parse(data, size);
  if (valid && !is_closing && !connection.abortRequested())
    return true;
  connection.setCloseReason(WebSocketServer::CloseReason::invalid_websocket_input);
  close();
  return false;
}

bool WebSocketHandler::parse(const uint8_t*& data, size_t& size) {
  while (!reading_payload) {
    if (!size)
      return true;
    header[header_bytes++] = *data++;
    --size;
    if (header_bytes == MIN_HEADER_SIZE) {
      if ((header[0] & RESERVED_BITS) || !(header[1] & MASK_BIT))
        return false;
      uint8_t length = header[1] & LENGTH_MASK;
      header_size = MIN_HEADER_SIZE + sizeof(mask) +
                    (length == LENGTH_16 ? 2 : length == LENGTH_64 ? 8 : 0);
    }
    if (header_bytes == header_size) {
      if (!beginFrame())
        return false;
      reading_payload = true;
    }
  }
  size_t count = payload_size - payload_received;
  if (count > size)
    count = size;
  for (size_t i = 0; i < count; ++i)
    target[payload_received + i] = data[i] ^ mask[(payload_received + i) & 3];
  payload_received += count;
  data += count;
  size -= count;
  if (payload_received < payload_size)
    return true;
  header_bytes = 0;
  header_size = MIN_HEADER_SIZE;
  reading_payload = false;
  return finishFrame();
}

bool WebSocketHandler::beginFrame() {
  uint64_t length = header[1] & LENGTH_MASK;
  if (length == LENGTH_16) {
    length = (uint64_t)header[2] << 8 | header[3];
    if (length < LENGTH_16)
      return false;
  } else if (length == LENGTH_64) {
    length = 0;
    for (size_t i = 0; i < 8; ++i)
      length = length << 8 | header[2 + i];
    if (length <= UINT16_MAX)
      return false;
  }
  if (length > MAX_PAYLOAD_SIZE)
    return false;

  opcode = header[0] & OPCODE_MASK;
  final = header[0] & FIN_BIT;
  if (opcode >= CLOSE) {
    if ((opcode != CLOSE && opcode != PING && opcode != PONG) || !final ||
        length > MAX_CONTROL_SIZE || (opcode == CLOSE && length == 1))
      return false;
    target = control;
  } else {
    const size_t limit = connection.maxMessageBytes();
    if (opcode == CONTINUATION) {
      if (!fragment_count || fragment_count >= MAX_MESSAGE_FRAMES ||
          length > limit - message_size)
        return false;
    } else if ((opcode != TEXT && opcode != BINARY) || fragment_count || length > limit) {
      return false;
    }
    message.resize(message_size + length + 1);
    target = message.data() + message_size;
  }
  memcpy(mask, header + header_size - sizeof(mask), sizeof(mask));
  payload_size = length;
  payload_received = 0;
  return true;
}

bool WebSocketHandler::finishFrame() {
  switch (opcode) {
  case CLOSE:
    connection.setCloseReason(WebSocketServer::CloseReason::peer_websocket_close);
    DEBUG("CLOSE requested");
    enqueueFrame(CLOSE, control, payload_size);
    connection.flushSend();
    is_closing = true;
    return false;

  case PING:
    DEBUG("PING requested");
    return connection.sendPong(control, payload_size);

  case PONG:
    DEBUG("PONG received");
    connection.deliverPong(control, payload_size);
    return true;

  default:
    break;
  }

  message_size += payload_size;
  if (!final) {
    ++fragment_count;
    return true;
  }
  const size_t size = message_size;
  message_size = 0;
  fragment_count = 0;
  message[size] = 0;
  connection.deliverMessage(message.data(), size);
  return true;
}

WebSocketServer::SendResult WebSocketHandler::enqueue(Opcode opcode, const void* payload, size_t size) {
  if (is_closing)
    return WebSocketServer::SendResult::closed;
  return enqueueFrame(opcode, payload, size);
}

WebSocketServer::SendResult WebSocketHandler::enqueueFrame(Opcode opcode, const void* payload, size_t size) {
  // Every permitted payload fits the 16-bit extended length.
  static_assert(MAX_PAYLOAD_SIZE <= UINT16_MAX);
  if (size > MAX_PAYLOAD_SIZE || (opcode >= CLOSE && size > MAX_CONTROL_SIZE))
    return WebSocketServer::SendResult::too_large;
  if (size && payload == nullptr)
    return WebSocketServer::SendResult::invalid;
  const size_t header_len = size > MAX_CONTROL_SIZE ? 4 : 2;
  const size_t frame_size = header_len + size;
  if (frame_size > TCP_SND_BUF)
    return WebSocketServer::SendResult::too_large;

  // Header and payload go to TCP as one write, so backpressure can never
  // leave part of a frame queued.
  uint8_t* frame = connection.frameBuffer(frame_size);
  if (!frame)
    return WebSocketServer::SendResult::no_memory;
  frame[0] = FIN_BIT | opcode;
  if (header_len == 4) {
    frame[1] = LENGTH_16;
    frame[2] = (uint8_t)(size >> 8);
    frame[3] = (uint8_t)size;
  } else {
    frame[1] = (uint8_t)size;
  }
  if (size)
    memcpy(frame + header_len, payload, size);
  return connection.enqueueRaw(frame, frame_size);
}

bool WebSocketHandler::close() {
  if (is_closing)
    return true;
  bool queued = enqueueFrame(CLOSE, nullptr, 0) == WebSocketServer::SendResult::queued;
  connection.flushSend();
  is_closing = true;
  return queued;
}
