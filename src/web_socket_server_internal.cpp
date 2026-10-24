#include "web_socket_server_internal.h"

#include <stdint.h>

#include "cyw43_config.h"
#include "lwip/tcp.h"

#include "client_connection.h"
#include "debug.h"
#include "pico_ws_server/cyw43_guard.h"

namespace {

constexpr auto POLL_TIMER_COARSE = 1; // around 500 ms

err_t finish_connection(ClientConnection* connection, tcp_pcb* pcb) {
  tcp_arg(pcb, nullptr);
  tcp_recv(pcb, nullptr);
  tcp_sent(pcb, nullptr);
  tcp_err(pcb, nullptr);
  tcp_poll(pcb, nullptr, 0);
  connection->onClose();
  if (tcp_close(pcb) == ERR_OK)
    return ERR_OK;
  tcp_abort(pcb);
  return ERR_ABRT;
}

err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* pb, err_t err) {
  cyw43_arch_lwip_check();

  if (!arg) {
    if (pb) {
      DEBUG("pbuf with null arg");
      pbuf_free(pb);
    }
    return ERR_OK;
  }

  ClientConnection* connection = (ClientConnection*)arg;

  bool keep_connection;
  if (pb) {
    if (err != ERR_OK) {
      connection->setCloseReason(WebSocketServer::CloseReason::tcp_error, err);
      keep_connection = false;
    } else {
      keep_connection = connection->process(pb);
    }
    tcp_recved(pcb, pb->tot_len);
    pbuf_free(pb);
  } else {
    keep_connection = false;
    connection->setCloseReason(WebSocketServer::CloseReason::peer_tcp_closed);
    DEBUG("client disconnected");
  }

  if (!keep_connection) {
    DEBUG("closing connection");

    return finish_connection(connection, pcb);
  }

  return ERR_OK;
}

err_t on_sent(void* arg, struct tcp_pcb* pcb, u16_t len) {
  cyw43_arch_lwip_check();

  if (!arg) {
    return ERR_OK;
  }

  ClientConnection* connection = (ClientConnection*)arg;
  if (!connection->needsSentCallback()) {
    return ERR_OK;
  }

  bool keep_connection = connection->onSent(len);
  if (!keep_connection) {
    connection->setCloseReason(WebSocketServer::CloseReason::tcp_output_error);
    return finish_connection(connection, pcb);
  }

  return ERR_OK;
}

err_t on_poll(void* arg, struct tcp_pcb* pcb) {
  cyw43_arch_lwip_check();

  if (!arg) {
    DEBUG("aborting inactive connection with null arg");
    tcp_abort(pcb);
    return ERR_ABRT;
  }

  ClientConnection* connection = (ClientConnection*)arg;
  connection->onPoll();
  if (connection->shouldReap()) {
    connection->markCloseTimeout();
    connection->detachAndAbort();
    connection->onClose();
    DEBUG("aborting inactive connection after close request");
    return ERR_ABRT;
  }

  return ERR_OK;
}

void on_error(void* arg, [[maybe_unused]] err_t err) {
  cyw43_arch_lwip_check();

  DEBUG("connection errored %s: %d", arg ? "with arg" : "without arg", (int)err);

  if (arg) {
    ClientConnection* connection = (ClientConnection*)arg;
    connection->setCloseReason(WebSocketServer::CloseReason::tcp_error, err);
    connection->onClose();
  }
}

err_t on_connect(void* arg, struct tcp_pcb* new_pcb, err_t err) {
  cyw43_arch_lwip_check();

  if (!new_pcb) {
    // Connection error
    DEBUG("null pcb");
    return ERR_ARG;
  }
  if (!arg || err != ERR_OK) {
    // Unexpected error
    DEBUG("aborting %s", arg ? "with arg" : "without arg");
    tcp_abort(new_pcb);
    return ERR_ABRT;
  }

  ClientConnection* connection = ((WebSocketServerInternal*)arg)->onConnect(new_pcb);
  if (!connection) {
    // Unable to accept connection
    DEBUG("aborting due to server rejection");
    tcp_abort(new_pcb);
    return ERR_ABRT;
  }

  tcp_arg(new_pcb, connection);
  tcp_err(new_pcb, on_error);
  tcp_recv(new_pcb, on_recv);
  tcp_sent(new_pcb, on_sent);
  tcp_poll(new_pcb, on_poll, POLL_TIMER_COARSE);

  DEBUG("success");

  return ERR_OK;
}

struct tcp_pcb* init_listen_pcb(uint16_t port, void* arg) {
  Cyw43Guard guard;

  struct tcp_pcb* temp_pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
  if (!temp_pcb) {
    DEBUG("failed to create temp pcb");
    return NULL;
  }

  tcp_arg(temp_pcb, arg);

  if (tcp_bind(temp_pcb, IP_ADDR_ANY, port) != ERR_OK) {
    DEBUG("failed to bind");
    tcp_abort(temp_pcb);
    return NULL;
  }

  struct tcp_pcb* listen_pcb = tcp_listen(temp_pcb);
  if (!listen_pcb) {
    DEBUG("failed to create listen pcb");
    tcp_abort(temp_pcb);
  }

  // temp_pcb has already been freed
  return listen_pcb;
}

} // namespace

WebSocketServerInternal::~WebSocketServerInternal() {
  Cyw43Guard guard;
  for (auto& entry : connection_by_id)
    entry.second->detachAndAbort();
  if (listen_pcb) {
    tcp_arg(listen_pcb, nullptr);
    tcp_accept(listen_pcb, nullptr);
    if (tcp_close(listen_pcb) != ERR_OK)
      tcp_abort(listen_pcb);
  }
}

void WebSocketServerInternal::setTcpNoDelay(bool enabled) {
  Cyw43Guard guard;
  tcp_nodelay = enabled;
  for (auto& entry : connection_by_id)
    entry.second->setTcpNoDelay(enabled);
}

bool WebSocketServerInternal::startListening(uint16_t port) {
  Cyw43Guard guard;

  if (listen_pcb) {
    // Already listening
    return false;
  }

  listen_pcb = init_listen_pcb(port, this);
  if (listen_pcb) {
    tcp_accept(listen_pcb, on_connect);
  }

  return listen_pcb != nullptr;
}

bool WebSocketServerInternal::setReceiveLimits(const WebSocketServer::ReceiveLimits& limits) {
  Cyw43Guard guard;
  if (listen_pcb || !limits.messageBytes || limits.messageBytes > 64000 ||
      limits.queuedBytes < limits.messageBytes || !limits.queuedMessages ||
      !limits.dispatchMessages) {
    DEBUG("invalid receive limits or listener already started");
    return false;
  }
  receive_limits = limits;
  return true;
}

bool WebSocketServerInternal::getReceiveQueueStats(
    uint32_t conn_id, WebSocketServer::ReceiveQueueStats& stats) {
  Cyw43Guard guard;
  auto iter = connection_by_id.find(conn_id);
  if (iter == connection_by_id.end())
    return false;
  const ClientConnection& connection = *iter->second;
  stats.queuedMessages = connection.queuedMessages();
  stats.queuedBytes = connection.queuedBytes();
  stats.peakQueuedMessages = connection.peakQueuedMessages();
  stats.peakQueuedBytes = connection.peakQueuedBytes();
  stats.messageLimit = connection.queueMessageLimit();
  stats.byteLimit = connection.queueByteLimit();
  return true;
}

void WebSocketServerInternal::popMessages() {
  Cyw43Guard guard;
  if (dispatching)
    return;
  dispatching = true;

  for (auto it = connection_by_id.begin(); it != connection_by_id.end();) {
    ClientConnection* connection = (it++)->second.get();
    if (connection->isProcessing())
      continue;
    if (!connection->shouldReap())
      connection->popMessages();
    if (connection->shouldReap()) {
      connection->markCloseTimeout();
      connection->detachAndAbort();
      connection->onClose();
    }
  }
  dispatching = false;
}

WebSocketServer::SendResult WebSocketServerInternal::enqueueMessage(uint32_t conn_id, const void* payload, size_t size) {
  Cyw43Guard guard;
  ClientConnection* connection = getConnectionById(conn_id);
  return connection ? connection->enqueueWebSocketMessage(payload, size)
                    : WebSocketServer::SendResult::closed;
}

bool WebSocketServerInternal::flush(uint32_t conn_id) {
  Cyw43Guard guard;
  ClientConnection* connection = getConnectionById(conn_id);
  return connection && connection->flushSend();
}

bool WebSocketServerInternal::abort(uint32_t conn_id) {
  Cyw43Guard guard;
  ClientConnection* connection = getConnectionById(conn_id);
  if (!connection)
    return false;
  connection->abort();
  return true;
}

bool WebSocketServerInternal::sendPing(uint32_t conn_id, const void* payload, size_t payload_size) {
  Cyw43Guard guard;

  ClientConnection* connection = getConnectionById(conn_id);
  if (!connection) {
    DEBUG("connection not found");
    return false;
  }

  // Per RFC 6455, control frames max payload is 125 bytes; enforcement left to caller.
  bool result = connection->sendWebSocketPing(payload, payload_size);

  return result;
}

bool WebSocketServerInternal::sendMessage(uint32_t conn_id, const char* payload) {
  Cyw43Guard guard;

  ClientConnection* connection = getConnectionById(conn_id);
  if (!connection) {
    DEBUG("connection not found");
    return false;
  }

  bool result = connection->sendWebSocketMessage(payload);

  return result;
}

bool WebSocketServerInternal::sendMessage(uint32_t conn_id, const void* payload, size_t payload_size) {
  Cyw43Guard guard;

  ClientConnection* connection = getConnectionById(conn_id);
  if (!connection) {
    DEBUG("connection not found");
    return false;
  }

  bool result = connection->sendWebSocketMessage(payload, payload_size);

  return result;
}

bool WebSocketServerInternal::broadcastMessage(const char* payload) {
  Cyw43Guard guard;

  if (connection_by_id.size() == 0) {
    DEBUG("connection map is empty");
    return false;
  }

  bool all_success = true;
  for (const auto& [_, connection] : connection_by_id)
    if (!connection->sendWebSocketMessage(payload))
      all_success = false;
  return all_success;
}

bool WebSocketServerInternal::broadcastMessage(const void* payload, size_t payload_size) {
  Cyw43Guard guard;

  if (connection_by_id.size() == 0) {
    DEBUG("connection map is empty");
    return false;
  }

  bool all_success = true;
  for (const auto& [_, connection] : connection_by_id) {
    if (!connection->sendWebSocketMessage(payload, payload_size)) {
      all_success = false;
    }
  }

  return all_success;
}

bool WebSocketServerInternal::close(uint32_t conn_id) {
  Cyw43Guard guard;

  ClientConnection* connection = getConnectionById(conn_id);
  if (!connection) {
    DEBUG("connection not found");
    return false;
  }

  bool result = connection->close();

  return result;
}

ClientConnection* WebSocketServerInternal::onConnect(struct tcp_pcb* pcb) {
  cyw43_arch_lwip_check();

  if (connection_by_id.size() >= max_connections) {
    return nullptr;
  }

  // Apply TCP_NODELAY setting if enabled
  if (tcp_nodelay) {
    tcp_nagle_disable(pcb);
  }

  auto connection = std::make_unique<ClientConnection>(*this, pcb, receive_limits);
  ClientConnection* connection_ptr = connection.get();
  do {
    if (++next_connection_id > INT32_MAX)
      next_connection_id = 1;
  } while (connection_by_id.count(next_connection_id));
  uint32_t conn_id = next_connection_id;

  connection_by_id[conn_id] = std::move(connection);

  return connection_ptr;
}

void WebSocketServerInternal::onUpgrade(ClientConnection* connection) {
  cyw43_arch_lwip_check();

  if (connect_cb) {
    connect_cb(server, getConnectionId(connection));
  }
}

void WebSocketServerInternal::onClose(ClientConnection* connection, bool is_upgraded) {
  cyw43_arch_lwip_check();

  uint32_t conn_id = getConnectionId(connection);
  auto reason = connection->closeReason();
  int transport_error = connection->closeTransportError();
  auto owned_connection = connection_by_id.extract(conn_id);

  if (is_upgraded) {
    if (close_details_cb)
      close_details_cb(server, conn_id, reason, transport_error,
                       connection->queuedMessages(), connection->queuedBytes(),
                       connection->peakQueuedMessages(), connection->peakQueuedBytes(),
                       connection->queueMessageLimit(), connection->queueByteLimit());
    if (close_cb)
      close_cb(server, conn_id);
  }
}

void WebSocketServerInternal::onMessage(ClientConnection* connection, const void* payload, size_t size) {
  cyw43_arch_lwip_check();

  if (message_cb) {
    message_cb(server, getConnectionId(connection), payload, size);
  }
}

void WebSocketServerInternal::onPong(ClientConnection* connection, const void* payload, size_t size) {
  cyw43_arch_lwip_check();

  if (pong_cb) {
    pong_cb(server, getConnectionId(connection), payload, size);
  }
}

uint32_t WebSocketServerInternal::getConnectionId(ClientConnection* connection) {
  for (const auto& entry : connection_by_id)
    if (entry.second.get() == connection)
      return entry.first;
  return 0;
}

ClientConnection* WebSocketServerInternal::getConnectionById(uint32_t conn_id) {
  Cyw43Guard guard;

  auto iter = connection_by_id.find(conn_id);
  if (iter == connection_by_id.end()) {
    return nullptr;
  }

  return iter->second.get();
}
