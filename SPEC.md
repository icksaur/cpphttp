# Http Library Specs

Always follow and refer to code-quality.md

## requirements

Simple implementation.
HTTP only (no https)
arbitrary port
auto 404
No internationalization requirements
Binary and text request/response capability
one function per verb in library
addStage for "middleware"
colon-prepended slug-format path variables - see README
C++20, CMake, Linux sockets

## implementation

SEE README!

### types

- `QueryParameterKeyValue` - key/value struct for query params
- `Header` - name/value struct for HTTP headers
- `Response` - statusCode, body, contentType
- `Context` - verb, route, routeVariables, queryParameters, requestHeaders, requestBody, response (Response)
- `HttpException` - statusCode + message, derives from std::runtime_error
- `ParsedRequest` - verb, path, queryString, headers, body (internal parsing result)
- `RouteHandler` = std::function<Response(Context&)>
- `NextFunction` = std::function<void(Context&)>
- `StageHandler` = std::function<void(Context&, NextFunction)>

### WebSocket types

- `WebSocketHandle` = uint64_t — lightweight, copyable connection identifier
- `WebSocketMessage` - opcode (uint8_t: 0x1 text, 0x2 binary), data (string)
- `WebSocketHandler` - onOpen/onMessage/onClose callbacks taking WebSocketHandle
- `WebSocketFrame` (internal) - fin (bool), opcode (uint8_t), payload (string)
- `WebSocketConnectionInfo` (internal) - socket fd, write mutex (shared_ptr), route variables, fragmentation buffer

All types must be value-initialized. Structs returned from parsing functions must have deterministic default values on all fields, not indeterminate values from default construction.

### Server class

- Constructor takes port
- `get/post/put/patch/del` - one function per verb, registers route handler
- `addStage` - registers middleware stage
- `ws(path, WebSocketHandler)` - registers WebSocket route
- `send(handle, string) → bool` - send text message to WebSocket client, returns false if handle invalid
- `send(handle, vector<uint8_t>) → bool` - send binary message, returns false if handle invalid
- `broadcast(path, string)` - send text message to all WebSocket connections whose route pattern matches `path`
- `broadcast(path, vector<uint8_t>)` - binary broadcast overload
- `closeConnection(handle)` - server-initiated WebSocket close
- `getRouteVariables(handle) → vector<string>` - route variables captured at WebSocket handshake
- `start` - binds socket, listens, spawns accept thread
- `stop` - signals stop, closes WebSocket connections (with close frames), closes server socket, joins thread, waits for active connections to drain
- Destructor calls stop if running

### free functions (exposed for testing)

- `Ok(body, contentType)` - returns 200 Response
- `Ok(vector<uint8_t>, contentType)` - returns 200 binary Response
- `Fail(statusCode, body, contentType)` - returns Response with caller-chosen status code
- `parseQueryString(str)` - splits on & and =
- `matchRoute(pattern, path, outVars)` - segment matching with :variable capture
- `parseRequest(raw)` - parses HTTP/1.1 request text
- `formatResponse(code, body, contentType)` - builds HTTP/1.1 response text
- `normalizePath(path)` - ensures leading /
- `isWebSocketUpgrade(ParsedRequest)` - checks Upgrade and Connection headers (case-insensitive)
- `buildWebSocketAcceptKey(clientKey)` - SHA-1 + Base64 of key + magic GUID
- `parseWebSocketFrame(raw, bytesConsumed)` - decodes a single WebSocket frame, sets bytesConsumed to total frame size. All frame boundary logic lives in this one function — no separate frame-size calculation.
- `buildWebSocketFrame(opcode, payload)` - encodes unmasked frame (server→client)
- `sha1(input)` - raw SHA-1 hash (20 bytes)
- `base64Encode(input)` - Base64 encoding

### route matching

- Split pattern and path on /
- Segments must match count
- Literal segments must match exactly
- `:name` segments match anything, captured value stored in routeVariables (positional)
- Root "/" is empty-segments match
- Same matching used for both HTTP and WebSocket routes

### pipeline execution

- Innermost function: matched route handler (or 404 handler if no match)
- Each stage wraps the next function: stages_[last] wraps innermost, stages_[0] is outermost
- Stages call next(context) to proceed (or skip to short-circuit)
- Exceptions propagate through stages; handleConnection catches anything uncaught
- Stages do not apply to WebSocket connections (different paradigm)

### failing requests

Two mechanisms for returning error responses:

1. **Return `Fail()`** - handler returns a Response with the desired status code. Clean, no stack unwinding. Preferred for expected failures (validation, not-found, forbidden).

2. **Throw `HttpException`** - propagates through middleware stages. Stages can catch, log, or transform the error. Uncaught HttpException is handled by the connection handler. Preferred when middleware needs to observe the failure.

Behavior:
- `Fail(statusCode)` returns response with empty body, text/plain
- `Fail(statusCode, body)` returns response with body, text/plain
- `Fail(statusCode, body, contentType)` returns response with body and content type
- `HttpException(statusCode, message)` thrown from handler or stage, propagates up the pipeline
- Unmatched routes automatically return 404 with "Not Found" body
- Verb mismatch on a matched path returns 405 with "Method Not Allowed" body
- Uncaught `std::exception` returns 500 with "Internal Server Error" body

### WebSocket protocol

- Handshake: client sends GET with Upgrade/Connection/Sec-WebSocket-Key/Version headers, server responds 101 with Sec-WebSocket-Accept
- Frames: fin bit, opcode, mask bit, variable-length payload
- Fragmented message reassembly: accumulate continuation frames until fin=1, deliver complete message to onMessage. Max assembled size 1MB.
- Ping/pong: server responds to client pings automatically
- Close handshake: close frame → close response → TCP close. 5-second timeout.

### WebSocket ownership and API

- Server owns all connections via internal registry (unordered_map<WebSocketHandle, ConnectionInfo>)
- Callers hold WebSocketHandle (uint64_t) — lightweight, copyable, no lifetime concerns
- `send()` returns bool (false if handle stale) — disconnects are expected, not exceptional
- `getRouteVariables(handle)` returns route variables captured at handshake time

### broadcast

- `broadcast(path, message)` sends to all WebSocket connections registered on the route pattern `path`
- The argument is the route pattern string (same string passed to `ws()`), not a concrete path. `broadcast("/chat", msg)` sends to all `/chat` connections. `broadcast("/room/:id", msg)` sends to all `/room/:id` connections regardless of their `:id` value.
- Matching is string equality against the stored route pattern (after normalization)
- Thread-safe: takes a snapshot of matching handles under lock, then sends outside the lock
- Disconnected/errored clients during broadcast are silently skipped (send returns false)
- `broadcastExcept` omitted for now — not needed for htmx push use case, easy to add later

### decisions

- `del()` for DELETE verb — `delete` is a C++ keyword
- Single `Context` type for all verbs — simpler than per-verb subtypes; body is empty for GET/DELETE
- `Context` embeds `Response` — one representation, stages and handlers both write `ctx.response`
- Route matching prefers most specific match (fewest :variables) regardless of registration order
- 405 Method Not Allowed when path matches but verb doesn't (not 404)
- First-added stage is outermost in pipeline execution
- Connection tracking via atomic counter — `stop()` waits for active connections to drain
- Parsing functions exposed as free functions for direct unit testing
- `handleConnection` decomposed into `readFullRequest`, `findRoute`, `buildPipeline`
- Single parse — reads full request (headers + body) before parsing once
- Max request size (1MB) to prevent unbounded memory consumption
- No URL decoding (no internationalization requirement)
- Connection: close on every response (no keep-alive)
- **Socket cleanup via RAII** — `handleConnection` uses a `SocketGuard` (destructor calls `close(fd)`). No manual `close(clientSocket)` calls. Socket is closed before `activeConnectionCount_` is decremented (SocketGuard destructs before the detached thread's decrement runs).
- **Handle-based WebSocket API** — Server owns connections internally; callers hold `WebSocketHandle` (uint64_t)
- **`send()` returns bool** — closed connections are expected, not exceptional
- **`send()` on Server, not on connection object** — Server owns the socket, the mutex, the registry. Centralizes thread safety.
- **Two-level locking protocol** — Registry mutex held only for lookups, never during I/O. To write: (1) lock registry, (2) copy fd + shared_ptr<mutex>, (3) unlock registry, (4) lock write mutex, (5) write, (6) unlock write mutex. All socket write paths follow this: `send()`, `closeConnection()`, `stop()`, and message loop pong/close responses.
- **Fragmented message reassembly** — frames arrive in TCP order. Accumulate until fin=1. Max assembled size 1MB.
- **Connection registry** — `unordered_map<WebSocketHandle, ConnectionInfo>` with mutex. ConnectionInfo holds fd, write mutex (shared_ptr<mutex>), route variables, fragmentation buffer. Monotonically increasing handle counter.
- Inline SHA-1 and Base64 — zero external dependencies
- `ws()` routes checked before HTTP routes — upgrade requests never reach HTTP pipeline
- **WebSocket receive timeout** — WebSocket connections must set a receive timeout to prevent stale clients from holding threads indefinitely. Same timeout approach as HTTP connections.
- **`extractContentLength` returns `size_t`** — consistent with unsigned size types used elsewhere
- **Broadcast matches on route pattern string** — the broadcast argument is the exact route pattern registered with `ws()`, matched by string equality. No `matchRoute` ambiguity. Per-room targeting is caller-side via `getRouteVariables`.

### socket handling

- Linux sockets: socket, bind, listen, accept, recv, send, close
- SO_REUSEADDR for quick restart
- Accept loop in dedicated thread
- Each connection handled in detached thread with atomic connection counter
- Counter incremented before thread spawn, decremented after handleConnection returns
- `stop()` waits for active connections to drain before returning
- `readFullRequest()` reads until \r\n\r\n, extracts Content-Length, reads body in one pass
- Max request size enforced (1MB) — drops oversized requests
- Connection: close after each response
- 405 returned when path matches but verb doesn't

## File server (fileserver.h / fileserver.cpp)

A separate, optional compilation unit. Not part of core http library. Users include it when they want to serve static files.

### overview

Serves a hardcoded list of files registered at startup. Files are loaded from disk into memory at registration time and served from memory thereafter. No directory walking, no runtime filesystem access.

### use case

htmx-based browser UI: serve index.html, htmx.min.js, ui.css, and similar assets alongside the C++ API server.

### API

```cpp
#include "fileserver.h"

Http::Server http(53000);
Http::FileServer files(http, "public");
files.add("index.html");       // GET /index.html, auto-registers GET / for index.html
files.add("htmx.min.js");      // GET /htmx.min.js
files.add("ui.css");           // GET /ui.css
// OR batch: files.add({"index.html", "htmx.min.js", "ui.css"});
```

### types

- `FileServer` class in `Http` namespace — takes `Server&` and base directory path
- No new public types beyond `FileServer` itself

### FileServer class

- Constructor: `FileServer(Server& server, const std::string& baseDir)`
- `add(filename)` — loads `baseDir/filename` from disk, registers `GET /filename` route on the server. If filename is `index.html`, also registers `GET /`. Throws `std::runtime_error` if file doesn't exist on disk at registration time.
- `add(initializer_list<string>)` — batch overload, calls single-file `add` for each
- Files are flat — no `/` or `..` in filenames. `add` rejects filenames containing `/` or `..` with an exception.

### content type mapping

Hardcoded map from extension to MIME type:

| Extension | Content-Type |
|---|---|
| .html | text/html |
| .js | text/javascript |
| .css | text/css |
| .png | image/png |
| .svg | image/svg+xml |
| .json | application/json |
| .ico | image/x-icon |
| (other) | application/octet-stream |

### file server decisions

- **Files loaded at startup, served from memory** — no runtime filesystem access, no TOCTOU issues
- **Flat filenames only** — path traversal attacks are impossible by design. No `..`, no `/` in filenames.
- **Fail loudly at registration** — if a file doesn't exist on disk when `add()` is called, throw immediately. Don't silently serve 404.
- **index.html auto-registers `/`** — `add("index.html")` registers both `GET /index.html` and `GET /` serving the same content. No separate `addIndex` method needed.
- **Separate compilation unit** — fileserver.h/fileserver.cpp are optional. Users who don't serve static files don't compile or link them.
- **Content-Type from extension** — small hardcoded map, falls back to `application/octet-stream`
- **Binary-safe** — files read in binary mode, stored as `std::string` (which holds arbitrary bytes)

### build integration

CMakeLists.txt adds `fileserver.cpp` as a separate static library. Tests and example link both `http` and `fileserver`.
