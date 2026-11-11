#ifndef __WEB_SOCKET_SERVER_INTERNAL_H__
#define __WEB_SOCKET_SERVER_INTERNAL_H__

#include <cstddef>
#include <memory>
#include <vector>

#include "lwip/tcp.h"

#include "pico_ws_server/web_socket_server.h"
#include "client_connection.h"

// Not multicore safe
class WebSocketServerInternal {
 public:
  WebSocketServerInternal(WebSocketServer& server, uint32_t max_connections)
      : server(server), connections(max_connections) {}
  ~WebSocketServerInternal();

  void setConnectCallback(WebSocketServer::ConnectCallback cb) { connect_cb = cb; }
  void setCloseCallback(WebSocketServer::CloseCallback cb) { close_cb = cb; }
  void setMessageCallback(WebSocketServer::MessageCallback cb) { message_cb = cb; }
  void setPongCallback(WebSocketServer::PongCallback cb) { pong_cb = cb; }
  void setTcpNoDelay(bool enabled);
  bool setMaxMessageSize(size_t bytes);
  size_t maxMessageBytes() const { return max_message_bytes; }

  bool startListening(uint16_t port);
  void poll();

  bool sendMessage(uint32_t conn_id, const char* payload);
  bool sendMessage(uint32_t conn_id, const void* payload, size_t payload_size);
  WebSocketServer::SendResult enqueueMessage(uint32_t conn_id, const void* payload, size_t size);
  bool flush(uint32_t conn_id);
  bool sendPing(uint32_t conn_id, const void* payload, size_t payload_size);
  bool broadcastMessage(const char* payload);
  bool broadcastMessage(const void* payload, size_t payload_size);

  bool close(uint32_t conn_id);
  bool abort(uint32_t conn_id);

  ClientConnection* onConnect(struct tcp_pcb* pcb);
  void onUpgrade(ClientConnection* connection);
  void onClose(ClientConnection* connection);

  void onMessage(ClientConnection* connection, const void* payload, size_t size);
  void onPong(ClientConnection* connection, const void* payload, size_t size);

  // One outgoing frame is assembled here before its single TCP write. The
  // buffer grows to the largest frame sent and is shared by all connections.
  uint8_t* frameBuffer(size_t size);

 private:
  static constexpr size_t MAX_MESSAGE_BYTES = 64000;

  WebSocketServer& server;

  uint32_t next_connection_id = 0;
  size_t max_message_bytes = MAX_MESSAGE_BYTES;
  bool tcp_nodelay = false;
  WebSocketServer::ConnectCallback connect_cb = nullptr;
  WebSocketServer::MessageCallback message_cb = nullptr;
  WebSocketServer::CloseCallback close_cb = nullptr;
  WebSocketServer::PongCallback pong_cb = nullptr;

  struct tcp_pcb* listen_pcb = nullptr;
  // One slot per permitted connection; an empty slot is free.
  std::vector<std::unique_ptr<ClientConnection>> connections;
  std::unique_ptr<uint8_t[]> frame_buffer;
  size_t frame_capacity = 0;

  ClientConnection* getConnectionById(uint32_t conn_id);
};

#endif
