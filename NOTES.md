# notes

## design decisions

- Single `Context` type for all verbs (simpler than per-verb subtypes, body is just empty for GET/DELETE)
- `Context` embeds `Response` — one representation, eliminates dual Response/field-mutation pattern
- Route matching prefers most specific route (fewest variables) so exact paths beat wildcards regardless of registration order
- 405 Method Not Allowed when path matches but verb doesn't (not 404)
- `del()` instead of `delete()` since delete is a C++ keyword
- Pipeline stages wrap in registration order: first-added stage is outermost (A → B → handler → B → A)
- Connection tracking via atomic counter — `stop()` waits for active connections to drain before returning
- `handleConnection` decomposed into `readFullRequest`, `findRoute`, `buildPipeline`
- Single parse — full request (headers + body) read once, parsed once
- Max request size (1MB) to prevent unbounded memory consumption
- Parsing functions exposed as free functions in `Http` namespace for direct unit testing
- `std::string` used for binary bodies (can hold arbitrary bytes); `Ok(vector<uint8_t>, contentType)` overload for convenience
- No URL decoding (no internationalization requirement)
- Connection: close on every response (no keep-alive)

## build

```
cmake . && cmake --build .
./http_tests
./http_example
```

## test coverage (104 assertions)

- Unit: normalizePath, parseQueryString, matchRoute, parseRequest, formatResponse
- Integration: all 5 verbs, route variables, query params, auto 404, wrong-verb 405, HttpException, std::exception, binary response
- Failures: Fail(400), Fail(404), Fail(418 arbitrary), Fail(503 no body), HttpException from handler, Fail() unit tests (defaults + custom contentType)
- Pipeline: no stages, single stage, multiple stage ordering, short-circuit, exception catching