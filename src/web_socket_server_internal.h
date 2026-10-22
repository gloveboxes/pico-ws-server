#ifndef __WEB_SOCKET_SERVER_INTERNAL_H__
#define __WEB_SOCKET_SERVER_INTERNAL_H__

#include <cstddef>
#include <memory>
#include <unordered_map>

#include "lwip/tcp.h"

#include "pico_ws_server/web_socket_server.h"
#include "client_connection.h"

// Not multicore safe
class WebSocketServerInternal {
 public:
  WebSocketServerInternal(WebSocketServer& server, uint32_t max_connections)
      : server(server), max_connections(max_connections) {}
  ~WebSocketServerInternal();

  void setConnectCallback(WebSocketServer::ConnectCallback cb) { connect_cb = cb; }
  void setCloseCallback(WebSocketServer::CloseCallback cb) { close_cb = cb; }
  void setMessageCallback(WebSocketServer::MessageCallback cb) { message_cb = cb; }
  void setPongCallback(WebSocketServer::PongCallback cb) { pong_cb = cb; }
  void setTcpNoDelay(bool enabled);
  bool setReceiveLimits(const WebSocketServer::ReceiveLimits& limits);

  bool startListening(uint16_t port);
  void popMessages();

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
  void onClose(ClientConnection* connection, bool is_upgraded);

  void onMessage(ClientConnection* connection, const void* payload, size_t size);
  void onPong(ClientConnection* connection, const void* payload, size_t size);

 private:
  WebSocketServer& server;

  uint32_t max_connections;
  uint32_t next_connection_id = 0;
  WebSocketServer::ReceiveLimits receive_limits;
  bool tcp_nodelay = false;
  bool dispatching = false;
  WebSocketServer::ConnectCallback connect_cb = nullptr;
  WebSocketServer::MessageCallback message_cb = nullptr;
  WebSocketServer::CloseCallback close_cb = nullptr;
  WebSocketServer::PongCallback pong_cb = nullptr;

  struct tcp_pcb* listen_pcb = nullptr;
  std::unordered_map<uint32_t, std::unique_ptr<ClientConnection>> connection_by_id;

  uint32_t getConnectionId(ClientConnection* connection);
  ClientConnection* getConnectionById(uint32_t conn_id);
};

#endif
