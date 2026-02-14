# HTTP-CPP

A modern C++ library that opens a given port and serves HTTP/1.1 GET/PUT/POST/PATCH/DELETE.

This is situational software intended only for the author's use, and does not need or intend to fulfil general purpose requirements for widespread usefulness.

## tech

- C++17
- CMake
- Linux sockets

## build

```bash
cmake -B build && cmake --build build
./build/http_tests     # run tests
./build/http_example   # run example server on port 53000
```

## usage

```cpp
#include "http.h"

int main() {
    Http::Server http(53000);

    http.get("/", [](Http::Context& ctx) {
        return Http::Ok("Hello, world!");
    });

    http.get("/:slug", [](Http::Context& ctx) {
        return Http::Ok("You asked for: " + ctx.routeVariables[0]);
    });

    http.get("/params", [](Http::Context& ctx) {
        std::string response;
        for (const auto& kv : ctx.queryParameters) {
            response += kv.key + "=" + kv.value + "\n";
        }
        return Http::Ok(response);
    });

    http.post("/images", [](Http::Context& ctx) {
        return Http::Ok("Received " + std::to_string(ctx.requestBody.size()) + " bytes");
    });

    http.put("/items/:id", [](Http::Context& ctx) {
        return Http::Ok("Updated item " + ctx.routeVariables[0]);
    });

    http.patch("/items/:id", [](Http::Context& ctx) {
        return Http::Ok("Patched item " + ctx.routeVariables[0]);
    });

    http.del("/items/:id", [](Http::Context& ctx) {
        return Http::Ok("Deleted item " + ctx.routeVariables[0]);
    });

    // Logging middleware
    http.addStage([](Http::Context& ctx, Http::NextFunction next) {
        next(ctx);
        std::cout << ctx.verb << " " << ctx.route << " " << ctx.response.statusCode << std::endl;
    });

    http.start();
    std::cin.get();
    http.stop();
    return 0;
}
```

## failing requests

Return an error from a handler with `Fail()`:

```cpp
http.get("/protected", [](Http::Context& ctx) {
    return Http::Fail(403, "Forbidden");
});

http.post("/data", [](Http::Context& ctx) {
    if (ctx.requestBody.empty()) {
        return Http::Fail(400, "Body required");
    }
    return Http::Ok("accepted");
});
```

Or throw `HttpException` for errors that propagate through middleware stages:

```cpp
http.get("/item/:id", [](Http::Context& ctx) {
    if (ctx.routeVariables[0] == "0") {
        throw Http::HttpException(404, "Item not found");
    }
    return Http::Ok("item " + ctx.routeVariables[0]);
});
```

Unmatched routes automatically return 404. Verb mismatch on a matched path returns 405. Uncaught `std::exception` returns 500.

## route matching

- Paths are matched segment-by-segment (split on `/`)
- Literal segments must match exactly
- `:name` segments capture any value into `ctx.routeVariables` (positional order)
- When multiple patterns match, the most specific (fewest variables) wins
- Leading `/` is optional when registering routes

## context reference

```
Context {
    std::string verb;                                  // "GET", "POST", etc.
    std::string route;                                 // request path, e.g. "/items/42"
    std::vector<std::string> routeVariables;           // captured :variable values
    std::vector<QueryParameterKeyValue> queryParameters; // parsed ?key=value pairs
    std::vector<Header> requestHeaders;                // request headers
    std::string requestBody;                           // request body (empty for GET/DELETE)
    Response response;                                 // {statusCode, body, contentType}
}

Response { int statusCode; std::string body; std::string contentType; }
QueryParameterKeyValue { std::string key; std::string value; }
Header { std::string name; std::string value; }
```

## response helpers

| Function | Returns |
|---|---|
| `Ok(body)` | 200, text/plain |
| `Ok(body, contentType)` | 200, custom content type |
| `Ok(vector<uint8_t>, contentType)` | 200, binary body |
| `Fail(statusCode)` | status code, empty body |
| `Fail(statusCode, body)` | status code, text/plain |
| `Fail(statusCode, body, contentType)` | status code, custom content type |

## pipeline stages

Stages wrap the handler in registration order. First-added stage is outermost:

```
addStage(A)  →  addStage(B)  →  handler

Execution: A → B → handler → B → A
```

A stage can short-circuit by not calling `next`, or catch exceptions thrown by inner stages/handler.

Stages interact with the response via `ctx.response`:

```cpp
http.addStage([](Http::Context& ctx, Http::NextFunction next) {
    try {
        next(ctx);
    } catch (Http::HttpException& ex) {
        ctx.response = {ex.statusCode, ex.what(), "text/plain"};
    }
});
```