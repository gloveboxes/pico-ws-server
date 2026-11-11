# WebSocket Latency Design

How pico-ws-server keeps interactive traffic, such as a terminal, responsive.

## Receive

- **Direct dispatch.** A complete message is handed to the application inside
  the TCP receive callback, with no queue between the two.
- **Send from the main loop.** Record state in the callback and transmit
  after `cyw43_arch_poll()` returns. A frame sent from the receive path can
  exhaust the CYW43 driver's transmit credit while received frames are still
  queued in the chip. The driver discards the frames it reads while waiting
  for credit, so the peer stalls until TCP retransmits them.
- **Immediate ACK.** Every received segment sets `TF_ACK_NOW`. A peer that
  coalesces small writes is never held behind lwIP's 250 ms delayed-ACK timer.
- **One copy, no allocation.** Payload bytes are unmasked straight into the
  connection's message buffer, which is reused for every message.

## Send

- **TCP_NODELAY.** `setTcpNoDelay(true)` disables Nagle's algorithm so small
  frames are not held for coalescing. The default leaves Nagle enabled.
- **Atomic frames.** Each frame is one `TCP_WRITE_FLAG_COPY` write assembled in
  a buffer the server reuses. Backpressure rejects the whole frame
  (`would_block`); it never queues part of one.
- **Enqueue is not flush.** `enqueueMessage()` queues without output; call
  `flush()` once after a burst. `sendMessage()` and `sendPing()` queue and
  attempt output. A frame TCP accepted stays accepted when `tcp_output()`
  fails: the library retries the output from sent/poll callbacks and `poll()`.
  Retry only frames that returned `would_block`.

```cpp
WebSocketServer server(1);
server.setTcpNoDelay(true);
server.setMessageCallback(on_message);
server.startListening(80);

while (true) {
  cyw43_arch_poll();
  server.poll();
}
```

## Trade-offs

Disabling Nagle and acknowledging immediately send more, smaller segments. That
suits interactive use on a local network; it is not a bulk-transfer tuning.

## Evidence

The consuming Z80ROMlessSBC project's native `websocket_*` tests compile all
server sources against fake lwIP and real SDK Mbed TLS. Configure them with
`PICO_SDK_PATH`. They check exact write/output counts, write/flush errors,
chained and split pbufs, empty and fragmented messages, interleaved controls,
malformed frames, the message size limit, callbacks that send, close or abort,
wrapped close deadlines, slot reuse, HTTP backpressure/final acknowledgements
and the absence of steady-state heap allocations.

These are functional and operation-count results, not hardware latency
measurements. For hardware acceptance, record sustained bytes/second,
key-to-echo latency and the maximum network-poll gap under the workloads that
matter, and check disconnect/reconnect under load.
