# Vision

"Modern smart devices are rented, not owned—reliant on constant connections to centralized vendor clouds.

We are changing this with MCP-native hardware. By standardizing device communication through the Model Context Protocol, we empower users to bring their own intelligence (LLMs) to their devices. This is the first step toward a future of local-first AI and genuinely decentralized hardware ecosystems."


# ESP32 MCP Server

A lightweight Model Context Protocol (MCP) server implementation for the ESP32 microcontroller. This framework allows your ESP32 device to expose tools to Large Language Models (LLMs) via the MCP standard, enabling direct interaction between AI agents and hardware. Tools are the only MCP primitive implemented today — resources and prompts are not served.

## Features

- **Protocol Support**: MCP JSON-RPC 2.0 over Streamable HTTP, with version negotiation across `2025-11-25` (default), `2025-06-18`, and `2025-03-26`.
- **Tool System**: Easy-to-use API for defining and registering custom tools.
- **Blocking handlers are safe**: `tools/call` runs on a dedicated worker task, so a handler that waits on a sensor or on I/O costs the async TCP task — which services every connection on the device — a short bounded wait instead of the handler's full duration. See [Where a tool call actually runs](#where-a-tool-call-actually-runs) for the two cases that still occupy the async TCP task.
- **Stateless transport**: no session identifier is issued or required.
- **Asynchronous**: Built on `ESPAsyncWebServer` for non-blocking operation.
- **Schema builder**: fluent C++ API for declaring input and output JSON Schemas. The schemas are published to clients through `tools/list`; the server does **not** validate arguments against them, so a handler must check its own inputs (see [Validating arguments](#validating-arguments)).
- **Discovery**: advertises itself over mDNS as `_mcp._tcp`.
- **Hardened request path**: body-size cap, `Origin` validation, and strict JSON-RPC envelope checks (see [Request handling](#request-handling)).

## Supported MCP Methods

The server currently supports the following MCP methods:
- `initialize`: Server handshake and capability negotiation.
- `ping`: Liveness probe; answers with an empty result.
- `notifications/initialized`: Client acknowledgment; must be sent as a notification, so a copy carrying an `id` is rejected.
- `tools/list`: Discovery of available tools.
- `tools/call`: Execution of tool logic.

A `tools/call` result always carries an `isError` flag (see
[Reporting a tool failure](#reporting-a-tool-failure)). The handler's return value
is serialized as text content, and — only when the call succeeded *and* the
returned document is a JSON object — attached again as `structuredContent`.
Define `MCP_OMIT_TEXT_WHEN_STRUCTURED=1` to send only the structured form and keep
the payload off the wire twice.

Batches (a JSON array of requests) are not supported; a request body must be a
single JSON-RPC object.

## Prerequisites

- **Hardware**: ESP32 development board.
- **Software**:
  - [PlatformIO](https://platformio.org/). The library ships a `library.json` and
    a `src/` + `include/` layout; there is no `library.properties`, so the
    Arduino IDE cannot consume it as-is.
  - Required Libraries:
    - `bblanchon/ArduinoJson@^7.0.0`
    - `ESP32Async/ESPAsyncWebServer@^3.6.0`
    - `ESP32Async/AsyncTCP@^3.3.2`

  > The `ESPAsyncWebServer` / `AsyncTCP` forks maintained by the
  > [ESP32Async](https://github.com/ESP32Async) organization are the
  > actively maintained successors to the unmaintained `me-no-dev` originals.

## Installation

Add the library to your project's `platformio.ini`:

```ini
lib_deps =
    solnera/ESP32-MCPServer@^0.2.1
    ESP32Async/ESPAsyncWebServer@^3.6.0
    ESP32Async/AsyncTCP@^3.3.2
```

To work on the library itself instead, clone it and open the repository in
PlatformIO; `platformio.ini` here configures the native test environments.

```bash
git clone https://github.com/solnera/esp32-mcpserver.git
```

## Usage

[`examples/echo`](examples/echo) is a complete sketch that connects to WiFi,
starts the MCP server, and registers the `echo` tool.

### 1. Configuration

Open `examples/echo/src/main.ino` and fill in your WiFi credentials, which ship
empty:

```cpp
const char* ssid = "";
const char* password = "";
```

### 2. Defining a Tool

Create a class that inherits from `ToolHandler` and implements the `call` method:

```cpp
class EchoHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        JsonDocument result;

        JsonVariantConst paramsVariant = params.as<JsonVariantConst>();
        String text = paramsVariant["text"].as<String>();

        result["echo"] = text;
        result["length"] = text.length();
        result["timestamp"] = millis();

        return result;
    }
};
```

`params` holds the `arguments` object of the `tools/call` request, or a null
document when the client sent none.

#### Reporting a tool failure

The dispatcher invokes the two-argument overload `call(JsonDocument, bool&)`. Its
default implementation runs the single-argument `call` and reports success, so a
handler that cannot fail needs nothing more. To signal an execution failure —
surfaced to the client as `result.isError = true`, per MCP — override **both**
overloads and let the single-argument one delegate:

```cpp
class ReadTempHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params, bool& isError) override {
        JsonDocument result;
        float celsius = readSensor();
        if (isnan(celsius)) {
            isError = true;
            result["message"] = "sensor did not respond";
            return result;
        }
        isError = false;
        result["celsius"] = celsius;
        return result;
    }

    JsonDocument call(JsonDocument params) override {
        bool ignored;
        return call(std::move(params), ignored);
    }
};
```

A failure reported this way is a successful JSON-RPC call whose result carries
`isError: true`; the payload is sent as text content only, never as
`structuredContent`. An exception escaping a handler is caught and converted into
a JSON-RPC `-32603` error instead.

#### Validating arguments

The server checks only that `arguments` is a JSON object; it does not match the
payload against the tool's `inputSchema`. Required fields, types, and ranges are
the handler's responsibility, and a rejection is reported the same way as any
other failure:

```cpp
JsonDocument call(JsonDocument params, bool& isError) override {
    JsonDocument result;
    JsonVariantConst args = params.as<JsonVariantConst>();

    if (!args["text"].is<const char*>()) {
        isError = true;
        result["message"] = "'text' is required and must be a string";
        return result;
    }
    // ...
}
```

### 3. Registering the Tool

In your `setup()` function, define the tool metadata and schema, then register it:

```cpp
// Create tool definition
Tool echoTool;
echoTool.name = "echo";
echoTool.description = "Echo back the input text";

// Define input schema using fluent Schema builder
echoTool.inputSchema = Schema::object()
    .description("Echo tool parameters")
    .property("text", Schema::string().description("Text to echo back"))
    .required({"text"})
    .build();

// Optional: declare the shape of the structured result
echoTool.outputSchema = Schema::object()
    .property("echo", Schema::string())
    .property("length", Schema::integer())
    .build();

// Attach handler
echoTool.handler = std::make_shared<EchoHandler>();

// Register with server; the rvalue overload skips a deep copy of the schemas
mcpServer->RegisterTool(std::move(echoTool));
```

`outputSchema` is optional and is omitted from `tools/list` when left unset. Like
`inputSchema`, it is advertised to clients but not enforced by the server.

### 4. Initialization

Construct the server with a port, name, version, and optional system
instructions, then start it with `begin()` **after** every tool is registered
and **after** WiFi is connected:

```cpp
// The 4th argument is optional; when non-empty it is returned to clients as the
// `instructions` field of `initialize`. examples/echo omits it.
mcpServer = new MCPServer(3000, "echo service", "1.0.0",
                          "You are an intelligent device that supports the MCP protocol.");

// ... RegisterTool() calls go here ...

if (!mcpServer->begin()) {
    Serial.println("Failed to start MCP server!");
}
```

`begin()` is required: the constructor no longer starts listening on its own.
Registering tools first keeps request handlers from racing mutations of the tool
registry, and `begin()` reads `WiFi.localIP()` to publish the mDNS endpoint.
`begin()` returns `false` only when the underlying server could not be created;
a failure to start the worker task is not fatal, and `tools/call` then degrades
to inline execution (see [Where a tool call actually runs](#where-a-tool-call-actually-runs)).

The server name also determines the mDNS hostname, which is a slug of it: the
name is lowercased, each run of non-alphanumeric characters becomes a single `-`,
leading and trailing separators are dropped, and the result is capped at 63
characters. `"echo service"` is published as `echo-service.local`; a name with no
alphanumeric character at all falls back to `esp32-mcp`.

## API Reference

The server exposes a single endpoint for MCP traffic:

- **Endpoint**: `POST /mcp` — `GET` and `DELETE` answer `405` with an `Allow: POST` header. There is no SSE stream.
- **Body**: a single JSON-RPC 2.0 request object (no batches)
- **Request headers**:
  - `Content-Type: application/json` (required; anything else is rejected with `415`)
  - `MCP-Protocol-Version`: (Optional) rejected with `400` if it names an unsupported version
- **Response headers**:
  - `MCP-Protocol-Version`: the version this build speaks, on every JSON-RPC response. Two replies do not carry it: the `405` for `GET`/`DELETE`, whose only server-added header is `Allow: POST`, and the `202` for a notification, which has no body. Both still get whatever transport headers `ESPAsyncWebServer` emits on its own.

### Request handling

| Condition | Response |
| --- | --- |
| Body larger than `MCP_HTTP_MAX_BODY_SIZE` | `413`, rejected before a body-sized buffer is allocated |
| No `Content-Length` (chunked upload) | `411` |
| `Content-Type` is not `application/json` | `415` |
| `Origin` fails the rebinding guard (below) | `403` |
| Fewer body bytes than declared | `400` |
| Any other path, or any method on `/mcp` besides `POST`/`GET`/`DELETE` | `404` |
| Body empty or not valid JSON | `400` with JSON-RPC error `-32700` |
| Envelope invalid: not a JSON-RPC 2.0 object, `id` not a string/number/null, or `params` not an object or array | `400` with JSON-RPC error `-32600` |
| Notification (no `id`) | `202` with no body |
| `tools/call` over HTTP/1.0, while the worker task is running | `505` (the deferred reply needs chunked framing) |
| Tool-call queue full | `200` with JSON-RPC error `-32000` |
| Body buffer or job allocation failed | `500` with JSON-RPC error `-32603` |

Once a request parses as a valid envelope, protocol-level failures — unknown
method, bad params, unknown tool, a handler throwing — are reported as HTTP `200`
carrying a JSON-RPC `error` object, which is what MCP SDKs expect. A malformed
`id` is answered with `id: null`, per JSON-RPC.

### DNS-rebinding guard

A request without an `Origin` header is accepted unconditionally — native MCP
clients do not send one. When `Origin` **is** present, all of the following must
hold, otherwise the request is answered with `403`:

1. A `Host` header is present.
2. `Origin` starts with `http://` (this server offers no TLS) and its authority
   contains no path, query, or fragment.
3. The `Origin` authority and `Host` are equal, case-insensitively.
4. That authority is the device's current `WiFi.localIP()` or its mDNS
   `<slug>.local` name, with `:<port>` appended — or bare, when the server
   listens on port 80.

Matching `Origin` against `Host` alone would not stop rebinding, since after a
rebind both still carry the attacker's domain; pinning to the IP or mDNS name is
what closes it.

### Where a tool call actually runs

`tools/call` normally executes on the worker task, and the HTTP reply is deferred
until the handler returns — that is what keeps a slow handler off the async TCP
task. Two cases still run the handler, or wait for it, on the async TCP task:

- **The fast path.** The tool still runs on the worker, but after queueing the
  job the async TCP task waits up to `MCP_HTTP_FAST_PATH_WAIT_MS` (20 ms by
  default) for it to finish, and replies inline if it does. A deferred reply can
  only be written when the connection next polls — roughly every 500 ms — so
  without this a 2 ms tool would still answer in half a second.
  The wait blocks the async TCP task for at most that window, no matter how long
  the handler runs; set the macro to `0` to opt out and always defer.
- **No worker.** If the worker task, its queue, or its semaphore could not be
  created during `begin()`, `tools/call` runs inline on the async TCP task for the
  handler's full duration. `begin()` still returns `true`, and the failure is
  reported on `Serial`.

Deferred replies use chunked transfer encoding, so they require HTTP/1.1. While a
worker exists, the version check comes first: **every** non-notification
`tools/call` from an HTTP/1.0 client is answered with `505` before the job is
queued, so the fast path cannot rescue it however quick the tool is. The other
methods (`initialize`, `tools/list`, `ping`) answer HTTP/1.0 normally — and so
does `tools/call` when there is no worker and it runs inline.

### Compile-time options

| Macro | Default | Effect |
| --- | --- | --- |
| `MCP_HTTP_MAX_BODY_SIZE` | `8192` | Largest accepted POST body, in bytes |
| `MCP_HTTP_JOB_QUEUE_DEPTH` | `4` | Queued `tools/call` jobs before new ones are answered "server busy" |
| `MCP_HTTP_WORKER_STACK_SIZE` | `8192` | Stack of the task that runs tool handlers |
| `MCP_HTTP_FAST_PATH_WAIT_MS` | `20` | Inline wait for a quick tool before falling back to a deferred reply; `0` always defers |
| `MCP_OMIT_TEXT_WHEN_STRUCTURED` | `0` | When `1`, an object result is sent only as `structuredContent` |

## Testing

The library has a native (host) test suite; no hardware required:

```bash
pio test -e native
```

Other environments defined in `platformio.ini`:

| Environment | Purpose |
| --- | --- |
| `native-san` | The same suite under AddressSanitizer and UBSan |
| `native-omit-text` | The protocol suite built with `MCP_OMIT_TEXT_WHEN_STRUCTURED=1` |
| `native-cov` | Instrumented build; run it through `scripts/coverage.sh`, which reports line coverage over `src/` and `include/` |

```bash
ASAN_OPTIONS=alloc_dealloc_mismatch=1:detect_leaks=0 pio test -e native-san
```

`alloc_dealloc_mismatch` catches a `new`/`delete` object handed to the
`free()`-based teardown that `ESPAsyncWebServer` applies to `_tempObject`. Leak
detection is off because the FreeRTOS task mock leaks by design: `workerEntry`
ends in `vTaskDelete(NULL)`, which the mock cannot map back to a `std::thread`.

# Contact Us


- 📧 **Email**: [liuyf1117@hotmail.com](mailto:liuyf1117@hotmail.com)
- 💬 **Slack**: [Join our community](https://join.slack.com/t/solnera/shared_invite/zt-3m2oat3br-~2JsOdxNW7XPnEgb5bjUdg)
