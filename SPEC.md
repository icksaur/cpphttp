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

### Server class

- Constructor takes port
- `get/post/put/patch/del` - one function per verb, registers route handler
- `addStage` - registers middleware stage
- `start` - binds socket, listens, spawns accept thread
- `stop` - signals stop, closes socket, joins thread, waits for active connections
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

### route matching

- Split pattern and path on /
- Segments must match count
- Literal segments must match exactly
- `:name` segments match anything, captured value stored in routeVariables (positional)
- Root "/" is empty-segments match

### pipeline execution

- Innermost function: matched route handler (or 404 handler if no match)
- Each stage wraps the next function: stages_[last] wraps innermost, stages_[0] is outermost
- Stages call next(context) to proceed (or skip to short-circuit)
- Exceptions propagate through stages; handleConnection catches anything uncaught

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

### socket handling

- Linux sockets: socket, bind, listen, accept, recv, send, close
- SO_REUSEADDR for quick restart
- Accept loop in dedicated thread
- Each connection handled in detached thread with atomic connection counter
- `stop()` waits for active connections to drain before returning
- `readFullRequest()` reads until \r\n\r\n, extracts Content-Length, reads body in one pass
- Max request size enforced (1MB) — drops oversized requests
- Connection: close after each response
- 405 returned when path matches but verb doesn't

## goal

fully code coverage of all paths and pipeline stage test

## steps to achieve goal

1. Create http.h with all types, free function declarations, and Server class
2. Create http.cpp with parsing, routing, pipeline, and socket implementation
3. Create CMakeLists.txt for building library, tests, and example
4. Create tests/test_http.cpp covering:
   - parseQueryString (empty, single, multiple, no-value)
   - matchRoute (root, exact, variable, multi-variable, mismatch, segment-count mismatch)
   - parseRequest (GET, POST with body, query string, headers)
   - formatResponse (200, 404)
   - normalizePath (with/without leading slash)
   - Integration: all verbs, route variables, query params, auto 404
   - Pipeline: no stages, single stage, multiple stages, short-circuit, exception handling
   - Binary response
   - Failures: Fail(400), Fail(404), Fail(418 arbitrary), Fail(503 no body), HttpException from handler, Fail() unit tests
5. Create example/main.cpp demonstrating library usage
6. Build and verify all tests pass
