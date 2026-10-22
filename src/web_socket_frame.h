#ifndef __WEB_SOCKET_FRAME_H__
#define __WEB_SOCKET_FRAME_H__

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

class WebSocketFrame {
 public:
  WebSocketFrame() = default;

  void reset(uint8_t new_opcode, bool new_final, size_t capacity) {
    opcode = new_opcode;
    final = new_final;
    payload.clear();
    payload.reserve(capacity + 1);
  }

  uint8_t getOpcode() {
    return opcode;
  }
  bool isFinal() {
    return final;
  }

  void append(const uint8_t* data, size_t size, const uint8_t* mask) {
    size_t offset = payload.size();
    payload.resize(offset + size);
    for (size_t i = 0; i < size; ++i)
      payload[offset + i] = data[i] ^ mask[(offset + i) & 3];
  }

  std::vector<uint8_t> takePayload() { return std::move(payload); }
  size_t getPayloadSize() {
    return payload.size();
  }
  const uint8_t* getPayload() {
    return payload.data();
  }

 private:
  uint8_t opcode = 0;
  bool final = false;
  std::vector<uint8_t> payload;
};

#endif
