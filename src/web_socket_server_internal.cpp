#include "web_socket_server_internal.h"

#include <new>
#include <stdint.h>
#include <string.h>

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

  bool keep_connection = false;
  if (pb) {
    if (err != ERR_OK) {
      connection->setCloseReason(WebSocketServer::CloseReason::tcp_error, err);
    } else {
      // Interactive traffic: ACK now rather than on the 250 ms lwIP fast
      // timer, so a sender that coalesces small writes never holds a
      // keystroke behind an unacknowledged frame. A reply sent by a callback
      // while processing carries the ACK; otherwise tcp_input() sends it on
      // return.
      tcp_set_flags(pcb, TF_ACK_NOW);
      keep_connection = connection->process(pb);
    }
    tcp_recved(pcb, pb->tot_len);
    pbuf_free(pb);
  } else {
    connection->setCloseReason(WebSocketServer::CloseReason::peer_tcp_closed);
    DEBUG("client disconnected");
  }

  if (keep_connection)
    return ERR_OK;
  if (connection->abortRequested()) {
    DEBUG("aborting connection");
    connection->detachAndAbort();
    connection->onClose();
    return ERR_ABRT;
  }
  DEBUG("closing connection");
  return finish_connection(connection, pcb);
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
  for (auto& connection : connections)
    if (connection)
      connection->detachAndAbort();
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
  for (auto& connection : connections)
    if (connection)
      connection->setTcpNoDelay(enabled);
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

bool WebSocketServerInternal::setMaxMessageSize(size_t bytes) {
  Cyw43Guard guard;
  if (listen_pcb || !bytes || bytes > MAX_MESSAGE_BYTES) {
    DEBUG("invalid message size or listener already started");
    return false;
  }
  max_message_bytes = bytes;
  return true;
}

void WebSocketServerInternal::poll() {
  Cyw43Guard guard;
  // Slots are never added or removed, so a close callback may poll again.
  for (auto& slot : connections) {
    ClientConnection* connection = slot.get();
    if (!connection || connection->isProcessing())
      continue;
    if (!connection->shouldReap())
      connection->onPoll();
    if (connection->shouldReap()) {
      connection->markCloseTimeout();
      connection->detachAndAbort();
      connection->onClose();
    }
  }
}

uint8_t* WebSocketServerInternal::frameBuffer(size_t size) {
  if (size > frame_capacity) {
    uint8_t* grown = new (std::nothrow) uint8_t[size];
    if (!grown)
      return nullptr;
    frame_buffer.reset(grown);
    frame_capacity = size;
  }
  return frame_buffer.get();
}

WebSocketServer::SendResult WebSocketServerInternal::enqueueMessage(uint32_t conn_id, const void* payload, size_t size) {
  Cyw43Guard guard;
  ClientConnection* connection = getConnectionById(conn_id);
  return connection ? connection->enqueueMessage(WebSocketHandler::BINARY, payload, size)
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
  return connection && connection->sendMessage(WebSocketHandler::PING, payload, payload_size);
}

bool WebSocketServerInternal::sendMessage(uint32_t conn_id, const char* payload) {
  Cyw43Guard guard;
  ClientConnection* connection = getConnectionById(conn_id);
  if (!payload)
    payload = "";
  return connection && connection->sendMessage(WebSocketHandler::TEXT, payload, strlen(payload));
}

bool WebSocketServerInternal::sendMessage(uint32_t conn_id, const void* payload, size_t payload_size) {
  Cyw43Guard guard;
  ClientConnection* connection = getConnectionById(conn_id);
  return connection && connection->sendMessage(WebSocketHandler::BINARY, payload, payload_size);
}

bool WebSocketServerInternal::broadcastMessage(const char* payload) {
  if (!payload)
    payload = "";
  Cyw43Guard guard;
  bool any = false, all_success = true;
  for (auto& connection : connections) {
    if (!connection)
      continue;
    any = true;
    if (!connection->sendMessage(WebSocketHandler::TEXT, payload, strlen(payload)))
      all_success = false;
  }
  return any && all_success;
}

bool WebSocketServerInternal::broadcastMessage(const void* payload, size_t payload_size) {
  Cyw43Guard guard;
  bool any = false, all_success = true;
  for (auto& connection : connections) {
    if (!connection)
      continue;
    any = true;
    if (!connection->sendMessage(WebSocketHandler::BINARY, payload, payload_size))
      all_success = false;
  }
  return any && all_success;
}

bool WebSocketServerInternal::close(uint32_t conn_id) {
  Cyw43Guard guard;
  ClientConnection* connection = getConnectionById(conn_id);
  return connection && connection->close();
}

ClientConnection* WebSocketServerInternal::onConnect(struct tcp_pcb* pcb) {
  cyw43_arch_lwip_check();

  for (auto& slot : connections) {
    if (slot)
      continue;
    if (tcp_nodelay)
      tcp_nagle_disable(pcb);
    do {
      if (++next_connection_id > INT32_MAX)
        next_connection_id = 1;
    } while (getConnectionById(next_connection_id));
    slot.reset(new (std::nothrow) ClientConnection(*this, pcb, next_connection_id));
    return slot.get();
  }
  return nullptr;
}

void WebSocketServerInternal::onUpgrade(ClientConnection* connection) {
  cyw43_arch_lwip_check();

  if (connect_cb) {
    connect_cb(server, connection->id());
  }
}

void WebSocketServerInternal::onClose(ClientConnection* connection) {
  cyw43_arch_lwip_check();

  for (auto& slot : connections) {
    if (slot.get() != connection)
      continue;
    // Free the slot first: the callback may look the connection up or poll.
    std::unique_ptr<ClientConnection> closed = std::move(slot);
    if (closed->isUpgraded() && close_cb)
      close_cb(server, closed->id(), closed->closeReason(), closed->closeTransportError());
    return;
  }
}

void WebSocketServerInternal::onMessage(ClientConnection* connection, const void* payload, size_t size) {
  cyw43_arch_lwip_check();

  if (message_cb) {
    message_cb(server, connection->id(), payload, size);
  }
}

void WebSocketServerInternal::onPong(ClientConnection* connection, const void* payload, size_t size) {
  cyw43_arch_lwip_check();

  if (pong_cb) {
    pong_cb(server, connection->id(), payload, size);
  }
}

ClientConnection* WebSocketServerInternal::getConnectionById(uint32_t conn_id) {
  for (auto& connection : connections)
    if (connection && connection->id() == conn_id)
      return connection.get();
  return nullptr;
}
