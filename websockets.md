# WebSocket Spec

Always follow and refer to code-quality.md

## overview

Add WebSocket support to the HTTP library. WebSocket connections begin as an HTTP/1.1 upgrade handshake, then switch to a persistent, bidirectional frame-based protocol. This fits naturally into the existing server — the upgrade request arrives as a normal HTTP request, and only diverges after the handshake response is sent.

The primary use case is **server→client push**: the server holds handles to connected clients and pushes messages to them at any time. Client→server messaging is supported via callbacks but is secondary.

## use cases

1. **Server→client push** (primary) — server pushes notifications, live data, state updates to connected clients on its own schedule
2. Bidirectional messaging — client sends messages that trigger server responses
3. Long-lived connections with low overhead (no repeated HTTP headers)

## goals

- Simple API consistent with existing library style
- Handle-based: Server owns connections, callers hold lightweight handles
- `Server::send(handle, message)` for server→client push from any thread
- Callback-based onOpen/onMessage/onClose for client→server events
- Register WebSocket handlers per route (same path pattern system)
- Support text and binary messages
- Fragmented message reassembly (frames arrive in order over TCP)
- Ping/pong for keepalive
- Clean shutdown integration with existing `Server::stop()`
- Testable: parsing functions exposed as free functions
- Full test coverage of all code paths

## non-goals

- No TLS/WSS (consistent with no HTTPS)
- No per-message compression (extensions negotiation)
- No subprotocol negotiation
- No multiplexing or HTTP/2

## considerations

### WebSocket protocol (RFC 6455)

**Handshake:** Client sends HTTP GET with `Upgrade: websocket`, `Connection: Upgrade`, `Sec-WebSocket-Key`, and `Sec-WebSocket-Version: 13`. Server responds 101 Switching Protocols with `Sec-WebSocket-Accept` (SHA-1 hash of key + magic GUID, base64-encoded).

**Frames:** After handshake, data flows as frames:
- 2+ byte header: fin bit, opcode (4 bits), mask bit, payload length (7 bits, extended to 16 or 64 bits)
- Client-to-server frames are always masked (4-byte mask key)
- Opcodes: 0x1 text, 0x2 binary, 0x8 close, 0x9 ping, 0xA pong
- Server-to-client frames are never masked

**Fragmentation:** A message can be split across multiple frames. First frame has opcode (text/binary) with fin=0. Continuation frames have opcode=0x0 with fin=0. Final frame has opcode=0x0 with fin=1. Frames arrive in order (TCP guarantees this). Server must reassemble before delivering to onMessage.

**Close handshake:** Either side sends close frame (opcode 0x8) with optional status code. Other side responds with close frame, then TCP connection closes.

### ownership model

Server owns all WebSocket connections internally. Callbacks and external code interact via `WebSocketHandle` — a lightweight, copyable value (uint64_t). This avoids lifetime issues: callers never hold pointers or references to internal connection objects. If a handle becomes stale (client disconnected), `send()` returns false.

### integration with existing architecture

The current connection model is: accept → read request → route → pipeline → send response → close socket. WebSocket connections diverge after the handshake: the socket stays open and enters a message loop instead of closing.

Key change: `handleConnection` needs a branch point after parsing the request. If it's a WebSocket upgrade, perform the handshake and enter the WebSocket loop. If not, proceed with normal HTTP handling.

WebSocket connections must be tracked for clean shutdown — `stop()` must close all active WebSocket connections (send close frames) and wait for them to drain, same as it waits for HTTP connections today via `activeConnectionCount_`.

### SHA-1 and Base64

The handshake requires SHA-1 hashing and Base64 encoding. These are small, self-contained algorithms (~50 lines each). Implement inline rather than adding an external dependency — consistent with the project's zero-dependency philosophy.

## code analysis

### files to modify

- **http.h** — new types, new Server methods (`ws`, `send`, `closeConnection`), WebSocket-related free function declarations
- **http.cpp** — handshake, frame parsing/building, WebSocket message loop, SHA-1, Base64, `handleConnection` branching, connection registry

### files to add

- **tests/test_http.cpp** — additional test cases for WebSocket functionality

### impact on existing code

- `handleConnection` — add branch for upgrade requests before HTTP pipeline
- `Server` class — new `ws()`, `send()`, `closeConnection()` methods, WebSocket route storage, connection registry
- `stop()` — close active WebSocket connections before waiting for drain
- No changes to existing HTTP types, parsing, routing, or pipeline logic

## types

```
WebSocketHandle = uint64_t
  - Lightweight, copyable value identifying a connection
  - Obtained in onOpen callback, used for server→client sends
  - Becomes invalid after onClose (send() returns false)

WebSocketMessage
  - opcode (uint8_t) — 0x1 text, 0x2 binary
  - data (std::string)

WebSocketHandler
  - onOpen: std::function<void(WebSocketHandle handle)>
  - onMessage: std::function<void(WebSocketHandle handle, WebSocketMessage message)>
  - onClose: std::function<void(WebSocketHandle handle)>

WebSocketFrame (internal)
  - fin (bool)
  - opcode (uint8_t)
  - payload (std::string)
```

Server internally manages a connection registry mapping WebSocketHandle → socket fd + mutex + route variables. The registry is protected by a mutex for thread-safe access from any thread calling `send()`.

## API design

### server→client push (primary use case)

```cpp
Http::Server http(53000);

std::vector<Http::WebSocketHandle> clients;
std::mutex clientsMutex;

http.ws("/feed", {
    .onOpen = [&](Http::WebSocketHandle handle) {
        std::lock_guard lock(clientsMutex);
        clients.push_back(handle);
    },
    .onMessage = [&](Http::WebSocketHandle handle, Http::WebSocketMessage msg) {
        // handle client→server messages if needed
    },
    .onClose = [&](Http::WebSocketHandle handle) {
        std::lock_guard lock(clientsMutex);
        std::erase(clients, handle);
    }
});

http.start();

// Push data to all connected clients from any thread
while (running) {
    auto data = getLatestData();
    std::lock_guard lock(clientsMutex);
    for (auto handle : clients) {
        http.send(handle, data);  // returns false if connection gone
    }
}
```

### multiple WebSocket groups

```cpp
std::map<std::string, std::vector<Http::WebSocketHandle>> rooms;
std::mutex roomsMutex;

http.ws("/chat/:room", {
    .onOpen = [&](Http::WebSocketHandle handle) {
        auto vars = http.getRouteVariables(handle);
        std::lock_guard lock(roomsMutex);
        rooms[vars[0]].push_back(handle);
    },
    .onMessage = [&](Http::WebSocketHandle handle, Http::WebSocketMessage msg) {
        auto vars = http.getRouteVariables(handle);
        std::lock_guard lock(roomsMutex);
        for (auto peer : rooms[vars[0]]) {
            if (peer != handle) {
                http.send(peer, msg.data);
            }
        }
    },
    .onClose = [&](Http::WebSocketHandle handle) {
        auto vars = http.getRouteVariables(handle);
        std::lock_guard lock(roomsMutex);
        std::erase(rooms[vars[0]], handle);
    }
});
```

### echo server (simplest case)

```cpp
http.ws("/echo", {
    .onOpen = [&](Http::WebSocketHandle handle) {
        http.send(handle, "connected");
    },
    .onMessage = [&](Http::WebSocketHandle handle, Http::WebSocketMessage msg) {
        http.send(handle, msg.data);
    },
    .onClose = [](Http::WebSocketHandle) {}
});
```

### Server methods

```
ws(path, WebSocketHandler) — register WebSocket route
send(handle, string) → bool — send text message, returns false if handle invalid
send(handle, vector<uint8_t>) → bool — send binary message, returns false if handle invalid
closeConnection(handle) → void — server-initiated close (sends close frame)
getRouteVariables(handle) → vector<string> — route variables captured at handshake
```

`send()` returns bool rather than throwing: a closed connection is expected behavior (clients disconnect), not exceptional. Consistent with code-quality.md ("simple is best", avoid side effects). Caller can check return value and clean up stale handles, or ignore it.

### route matching

WebSocket routes use the same `matchRoute` and path variable system as HTTP routes. `ws("/chat/:room", ...)` captures `:room` the same way `get("/items/:id", ...)` does today. Route variables are stored in the connection registry at handshake time and accessible via `getRouteVariables(handle)`.

## free functions (exposed for testing)

- `isWebSocketUpgrade(ParsedRequest)` — checks Upgrade and Connection headers
- `buildWebSocketAcceptKey(string clientKey)` — SHA-1 + Base64 of key + magic GUID
- `parseWebSocketFrame(string raw, size_t& bytesConsumed)` — decodes a single WebSocket frame from raw bytes, sets bytesConsumed to total frame size (header + payload). Returns the parsed frame. The message loop uses bytesConsumed to advance through the receive buffer — all frame boundary logic lives in this one function.
- `buildWebSocketFrame(uint8_t opcode, string payload)` — encodes a frame (unmasked, server→client)
- `sha1(string input)` — raw SHA-1 hash (20 bytes)
- `base64Encode(string input)` — Base64 encoding

## risks and mitigations

| Risk | Mitigation |
|------|-----------|
| WebSocket connections are long-lived; leaked sockets | Connection registry in Server; `stop()` sends close frames and waits for drain |
| SHA-1/Base64 bugs break handshake | Unit test with known test vectors from RFC |
| Frame parsing edge cases (large payloads, fragmentation) | Enforce max message size (1MB assembled); unit test fragmented reassembly |
| Masking bugs corrupt data | Unit test with known masked/unmasked frame pairs |
| `stop()` hangs waiting for slow WS clients | Close timeout — force-close socket after 5 seconds if close handshake doesn't complete |
| Thread safety of `send()` from multiple threads | Two-level locking: registry mutex for lookup only, per-connection write mutex for I/O (see locking protocol in decisions) |
| Stale handles used after onClose | `send()` returns false; handle lookup fails cleanly |
| Fragmented message exceeds max size | Track assembled size; close connection if it exceeds 1MB |

## decisions

- **Handle-based API** — Server owns connections internally; callers hold `WebSocketHandle` (uint64_t). Avoids lifetime/ownership issues. Handles are comparable, copyable, and storable in any container.
- **`send()` returns bool** — closed connections are expected, not exceptional. Returning false is simple and lets callers decide how to react. No exception overhead for normal disconnects.
- **`send()` on Server, not on connection object** — Server owns the socket, the mutex, the registry. Centralizes thread safety. Callers don't need to worry about connection object lifetime.
- **onClose receives handle** — callers need the handle to remove it from their tracking structures (vectors, maps, sets).
- **`getRouteVariables(handle)`** — route variables set once at handshake, immutable for connection lifetime. Accessed via Server method since Server owns the data.
- **Fragmented message reassembly** — frames arrive in TCP order (guaranteed). Accumulate continuation frames until fin=1, then deliver complete message to onMessage. Enforced max assembled size (1MB).
- **Connection registry** — `std::unordered_map<WebSocketHandle, ConnectionInfo>` protected by `std::mutex`. ConnectionInfo holds socket fd, write mutex (shared_ptr<mutex>), route variables, fragmentation buffer. Monotonically increasing handle counter ensures uniqueness. Write mutex is a shared_ptr so it can be copied out of the registry while the registry lock is released.
- **Two-level locking protocol** — Registry mutex (`wsConnectionsMutex_`) is held only for lookups and modifications to the map, never during I/O. To write to a socket: (1) lock registry mutex, (2) find connection, copy socket fd and shared_ptr to write mutex, (3) unlock registry mutex, (4) lock write mutex, (5) write to socket, (6) unlock write mutex. This prevents a blocking socket write from stalling the entire registry. All code paths that write to a WebSocket socket must follow this protocol: `send()`, `closeConnection()`, `stop()`, and the message loop's pong responses.
- Designated initializer syntax for `WebSocketHandler` — clean API, no builder pattern needed
- Same `matchRoute` system for WS paths — no new routing code, consistent with HTTP
- Inline SHA-1 and Base64 — zero external dependencies, ~50 lines each, exposed for testing
- Close handshake timeout (5 seconds) — matches existing HTTP recv timeout
- **Socket cleanup via RAII** — `handleConnection` constructs a `SocketGuard` (destructor calls `close(fd)`) at the top. All manual `close(clientSocket)` calls are removed. This guarantees the socket is closed on every exit path — normal return, early return, exception — without tracking individual code paths. The detached thread in `acceptLoop` decrements `activeConnectionCount_` after `handleConnection` returns, which is after the `SocketGuard` destructor has already closed the socket. This ensures `stop()`'s drain loop doesn't return while sockets are still open.
- Ping/pong handled automatically — server responds to client pings; no application-level API needed
- `ws()` routes checked before HTTP routes in `handleConnection` — upgrade requests never reach HTTP pipeline
- Stages (middleware) do not apply to WebSocket connections — stages are request/response middleware; WebSocket is a different paradigm. If logging is needed, do it in onOpen/onMessage/onClose callbacks.
- **Message loop pong/close writes must use two-level locking** — the message loop writes pong and close-response frames directly to the socket. These writes must acquire the per-connection write mutex to avoid corrupting frames if a concurrent `send()` is in progress on another thread. The message loop thread already knows the socket fd, so it only needs to look up the write mutex from the registry (or hold it locally).

## plan

### 1. Add SHA-1 and Base64 implementations to http.cpp

Add `sha1(const std::string&)` and `base64Encode(const std::string&)` as free functions in the `Http` namespace. Declare in http.h. Implement inline in http.cpp (~50 lines each). These are the only cryptographic operations needed for the WebSocket handshake.

### 2. Add WebSocket types to http.h

Add `WebSocketHandle` alias, `WebSocketMessage`, `WebSocketHandler` (with onOpen/onMessage/onClose taking handles), and `WebSocketFrame` (internal). Declare free functions: `isWebSocketUpgrade`, `buildWebSocketAcceptKey`, `parseWebSocketFrame`, `buildWebSocketFrame`.

### 3. Add Server methods and connection registry

Add to Server class:
- `ws(const std::string& path, WebSocketHandler handler)` — register WS route
- `bool send(WebSocketHandle handle, const std::string& message)` — send text
- `bool send(WebSocketHandle handle, const std::vector<uint8_t>& message)` — send binary
- `void closeConnection(WebSocketHandle handle)` — server-initiated close
- `std::vector<std::string> getRouteVariables(WebSocketHandle handle)` — access route vars

Add private members:
- `wsRoutes_` vector (path pattern + WebSocketHandler)
- Connection registry: `std::unordered_map<WebSocketHandle, ConnectionInfo>` with mutex
- `nextWsHandle_` atomic counter
- `ConnectionInfo` struct: socket fd, write mutex (unique_ptr<mutex>), route variables, fragmentation buffer

### 4. Implement WebSocket frame parsing and building in http.cpp

Implement `parseWebSocketFrame` — reads header bytes, extracts opcode, fin bit, payload length (7-bit / 16-bit / 64-bit), unmasks payload if masked. Sets `bytesConsumed` output parameter to total frame size so the caller can advance through the receive buffer without re-parsing the header. The message loop must use `bytesConsumed` for frame boundary detection — no duplicate header parsing.

Implement `buildWebSocketFrame` — constructs unmasked frame with correct length encoding. Both functions handle text, binary, close, ping, pong opcodes and continuation frames (opcode 0x0).

### 5. Implement handshake and upgrade detection

Implement `isWebSocketUpgrade` — checks for `Upgrade: websocket` header and `Connection: Upgrade`. Implement `buildWebSocketAcceptKey` — concatenates client key with magic GUID `258EAFA5-E914-47DA-95CA-5AB9AA63D255`, SHA-1 hashes, Base64 encodes.

### 6. Implement Server::send, closeConnection, getRouteVariables

`send()` — lock registry mutex, look up handle, copy socket fd and shared_ptr to write mutex, unlock registry mutex. Lock write mutex, build frame, write to socket, unlock write mutex. Return false if handle not found. Never hold registry mutex during I/O.

`closeConnection()` — same locking protocol as `send()`: look up under registry lock, copy fd + write mutex, release registry lock, then lock write mutex and send close frame.

`getRouteVariables()` — lock registry mutex, look up handle, return copy of variables vector (empty if not found). No I/O, so registry lock is sufficient.

### 7. Implement WebSocket message loop in handleConnection

Modify `handleConnection`: after parsing request, check `isWebSocketUpgrade`. If true:
1. Find matching WS route (using existing `matchRoute`)
2. Send 101 handshake response
3. Allocate handle, register in connection registry with socket fd + route variables
4. Call `onOpen(handle)`
5. Enter recv loop reading frames:
   - Text/binary with fin=1: deliver to `onMessage(handle, message)`
   - Continuation frames (fin=0): accumulate in fragmentation buffer, deliver assembled message when fin=1
   - Ping: respond with pong automatically
   - Close: respond with close frame, exit loop
6. Remove from registry, call `onClose(handle)`

If no matching WS route, fall through to normal HTTP handling (will 404 naturally).

### 8. Integrate WebSocket shutdown into stop()

When `stop()` is called: lock registry mutex, copy all socket fds and write mutex shared_ptrs, unlock registry mutex. Then for each connection, lock its write mutex and send a close frame — same two-level locking protocol as `send()`. Close sockets after sending close frames. The existing `activeConnectionCount_` drain loop handles the rest — WebSocket message loops will exit when their socket is closed. Force-close after 5-second timeout if close handshake doesn't complete.

### 9. Add tests

Test isolation: WebSocket integration tests that each create their own server must use unique ports to avoid `TIME_WAIT` port conflicts. Stage tests that reuse the same port (19877) are sequential but depend on correct connection counter ordering (step 7/decision above) to avoid flaky bind failures on rapid restart.

Add WebSocket tests to tests/test_http.cpp:

**Unit tests:**
- `sha1` with known test vectors
- `base64Encode` with known test vectors
- `buildWebSocketAcceptKey` with RFC example key
- `isWebSocketUpgrade` — positive case, missing headers, wrong values
- `parseWebSocketFrame` — text frame, binary frame, masked frame, close frame, ping frame, pong frame, 16-bit length, 64-bit length, continuation frame (fin=0)
- `buildWebSocketFrame` — text, binary, close, ping, pong, various payload sizes

**Integration tests:**
- Complete handshake (send upgrade request, verify 101 response with correct accept key)
- Server→client: server sends message via `http.send(handle, msg)`, client receives it
- Client→server: client sends text message, onMessage callback fires
- Send binary message, receive binary message
- Fragmented message reassembly (send multi-frame message, receive complete message)
- Ping/pong
- Client-initiated close handshake
- Server-initiated close via `http.closeConnection(handle)`
- `http.send()` returns false for invalid/stale handle
- Server stop closes WebSocket connections
- WebSocket route with path variables, verify `getRouteVariables()`
- Non-WebSocket GET to a WS-only route returns 404
- Upgrade request to non-existent route returns 404
- Multiple concurrent WebSocket connections

### 10. Update example/main.cpp

Add a WebSocket echo endpoint and a push endpoint to the example server demonstrating both use cases:
```cpp
// Echo
http.ws("/echo", {
    .onOpen = [&](Http::WebSocketHandle handle) {
        http.send(handle, "connected");
    },
    .onMessage = [&](Http::WebSocketHandle handle, Http::WebSocketMessage msg) {
        http.send(handle, msg.data);
    },
    .onClose = [](Http::WebSocketHandle) {}
});

// Server push (broadcast to all connected clients)
std::vector<Http::WebSocketHandle> subscribers;
std::mutex subMutex;

http.ws("/notifications", {
    .onOpen = [&](Http::WebSocketHandle handle) {
        std::lock_guard lock(subMutex);
        subscribers.push_back(handle);
    },
    .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
    .onClose = [&](Http::WebSocketHandle handle) {
        std::lock_guard lock(subMutex);
        std::erase(subscribers, handle);
    }
});

// Trigger broadcast via HTTP POST
http.post("/notify", [&](Http::Context& ctx) {
    std::lock_guard lock(subMutex);
    for (auto handle : subscribers) {
        http.send(handle, ctx.requestBody);
    }
    return Http::Ok("sent to " + std::to_string(subscribers.size()) + " clients");
});
```

### 11. Update documentation

Update README.md with WebSocket usage section. Update SPEC.md with WebSocket types, methods, and decisions.
