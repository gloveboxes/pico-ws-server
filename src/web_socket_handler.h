#ifndef __WEB_SOCKET_HANDLER_H__
#define __WEB_SOCKET_HANDLER_H__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pico_ws_server/web_socket_server.h"

class ClientConnection;

// Parses received frames and frames outgoing messages for one connection.
// Only access from lwIP context.
class WebSocketHandler {
 public:
  enum Opcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT = 0x1,
    BINARY = 0x2,
    CLOSE = 0x8,
    PING = 0x9,
    PONG = 0xA,
  };

  explicit WebSocketHandler(ClientConnection& connection) : connection(connection) {}

  // Delivers each complete message to the connection as it is parsed.
  // Returns false once the connection must stop receiving.
  bool process(const uint8_t* data, size_t size);

  // Queues one complete frame with a single TCP write.
  WebSocketServer::SendResult enqueue(Opcode opcode, const void* payload, size_t size);
  // Queues the close frame; returns whether TCP accepted it.
  bool close();

  bool isClosing() const { return is_closing; }

 private:
  static constexpr size_t MIN_HEADER_SIZE = 2;
  static constexpr size_t MAX_HEADER_SIZE = 14;
  static constexpr size_t MAX_CONTROL_SIZE = 125;
  static constexpr size_t MAX_PAYLOAD_SIZE = 64000;
  static constexpr size_t MAX_MESSAGE_FRAMES = 1024;

  ClientConnection& connection;
  bool is_closing = false;

  uint8_t header[MAX_HEADER_SIZE];
  size_t header_bytes = 0;
  size_t header_size = MIN_HEADER_SIZE;
  bool reading_payload = false;
  uint8_t opcode = 0;
  bool final = false;
  uint8_t mask[4] = {};
  size_t payload_size = 0;
  size_t payload_received = 0;
  // Where the current frame's unmasked payload is written.
  uint8_t* target = nullptr;

  // The data message being assembled, all fragments, then a NUL terminator.
  // Its capacity is kept between messages.
  std::vector<uint8_t> message;
  size_t message_size = 0;
  size_t fragment_count = 0;
  // Control frames may arrive between the fragments of a data message.
  uint8_t control[MAX_CONTROL_SIZE];

  // Consumes input up to the end of one frame; false rejects the input.
  bool parse(const uint8_t*& data, size_t& size);
  bool beginFrame();
  bool finishFrame();
  WebSocketServer::SendResult enqueueFrame(Opcode opcode, const void* payload, size_t size);
};

#endif
