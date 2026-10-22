#ifndef __WEB_SOCKET_MESSAGE_H__
#define __WEB_SOCKET_MESSAGE_H__

#include <cstddef>
#include <stdint.h>
#include <vector>

#include "web_socket_frame.h"

class WebSocketMessage {
 public:
  enum Type : uint8_t {
    UNKNOWN,
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0A,
  };

  WebSocketMessage(Type type, const void* payload, size_t payload_size)
      : type(type), payload_size(payload_size), const_payload(static_cast<const uint8_t*>(payload)) {}

  WebSocketMessage(WebSocketMessage&& from)
      : type(from.type),
        payload_size(from.payload_size),
        payload(std::move(from.payload)),
        const_payload(from.const_payload) {}

  WebSocketMessage& operator=(WebSocketMessage&& from) {
    type = from.type;
    payload_size = from.payload_size;
    payload = std::move(from.payload);
    const_payload = from.const_payload;
    return *this;
  }

  WebSocketMessage(Type type, std::vector<uint8_t>&& bytes)
      : type(type), payload_size(bytes.size()), payload(std::move(bytes)) {
    payload.push_back(0);
  }

  Type getType() const {
    return type;
  }

  size_t getPayloadSize() const {
    return payload_size;
  }
  const uint8_t* getPayload() const {
    if (const_payload) {
      return const_payload;
    }
    return payload.data();
  }

 private:
  Type type = UNKNOWN;
  size_t payload_size = 0;
  std::vector<uint8_t> payload;
  const uint8_t* const_payload = nullptr;

};

#endif
