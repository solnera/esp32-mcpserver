# Vision

"Modern smart devices are rented, not owned—reliant on constant connections to centralized vendor clouds.

We are changing this with MCP-native hardware. By standardizing device communication through the Model Context Protocol, we empower users to bring their own intelligence (LLMs) to their devices. This is the first step toward a future of local-first AI and genuinely decentralized hardware ecosystems."


# ESP32 MCP Server

A lightweight Model Context Protocol (MCP) server implementation for the ESP32 microcontroller. This framework allows your ESP32 device to expose tools and resources to Large Language Models (LLMs) via the MCP standard, enabling direct interaction between AI agents and hardware.

## Features

- **Protocol Support**: MCP JSON-RPC 2.0 over Streamable HTTP, with version negotiation across `2025-11-25` (default), `2025-06-18`, and `2025-03-26`.
- **Tool System**: Easy-to-use API for defining and registering custom tools with JSON Schema validation.
- **Blocking handlers are safe**: `tools/call` runs on a dedicated worker task, so a handler that waits on a sensor or on I/O never stalls the async TCP task that services every connection on the device.
- **Stateless transport**: no session identifier is issued or required.
- **Asynchronous**: Built on `ESPAsyncWebServer` for non-blocking operation.
- **Schema Validation**: robust input/output schema definition using a fluent C++ API.
- **Discovery**: advertises itself over mDNS as `_mcp._tcp`.
- **Hardened request path**: body-size cap, `Origin` validation, and strict JSON-RPC envelope checks (see [Request handling](#request-handling)).

## Supported MCP Methods

The server currently supports the following MCP methods:
- `initialize`: Server handshake and capability negotiation.
- `ping`: Liveness probe; answers with an empty result.
- `notifications/initialized`: Client acknowledgment.
- `tools/list`: Discovery of available tools.
- `tools/call`: Execution of tool logic.

Tool results are returned both as text content and, when the handler returns a
JSON object, as `structuredContent`. Define `MCP_OMIT_TEXT_WHEN_STRUCTURED=1` to
send only the structured form and keep the payload off the wire twice.

## Prerequisites

- **Hardware**: ESP32 development board.
- **Software**: 
  - [PlatformIO](https://platformio.org/) (recommended) or Arduino IDE.
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
    solnera/ESP32-MCPServer@^0.2.0
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

Open `examples/echo/src/main.ino` and configure your WiFi credentials:

```cpp
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
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

// Attach handler
echoTool.handler = std::make_shared<EchoHandler>();

// Register with server
mcpServer->RegisterTool(echoTool);
```

### 4. Initialization

Construct the server with a port, name, version, and optional system
instructions, then start it with `begin()` **after** every tool is registered
and **after** WiFi is connected:

```cpp
mcpServer = new MCPServer(3000, "echo service", "1.0.0",
                          "You are an intelligent device that supports the MCP protocol.");

mcpServer->RegisterTool(std::move(echoTool));

if (!mcpServer->begin()) {
    Serial.println("Failed to start MCP server!");
}
```

`begin()` is required: the constructor no longer starts listening on its own.
Registering tools first keeps request handlers from racing mutations of the tool
registry, and `begin()` reads `WiFi.localIP()` to publish the mDNS endpoint.

## API Reference

The server exposes a single endpoint for MCP traffic:

- **Endpoint**: `POST /mcp` — `GET` and `DELETE` answer `405` with an `Allow: POST` header.
- **Body**: JSON-RPC 2.0 Request
- **Request headers**:
  - `Content-Type: application/json` (required; anything else is rejected with `415`)
  - `MCP-Protocol-Version`: (Optional) rejected with `400` if it names an unsupported version
- **Response headers**:
  - `MCP-Protocol-Version`: the version this build speaks

### Request handling

| Condition | Response |
| --- | --- |
| Body larger than `MCP_HTTP_MAX_BODY_SIZE` | `413`, rejected before any buffer is allocated |
| No `Content-Length` (chunked upload) | `411` |
| `Content-Type` is not `application/json` | `415` |
| `Origin` present but not targeting the device IP or advertised `<server-name>.local` hostname | `403` (DNS-rebinding guard) |
| Fewer body bytes than declared | `400` |
| Notification (no `id`) | `202` with no body |
| Tool-call queue full | `200` with JSON-RPC error `-32000` |

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

`pio test -e native-san` runs the same suite under AddressSanitizer and UBSan,
and `scripts/coverage.sh` reports line coverage over `src/` and `include/`.

# Contact Us


- 📧 **Email**: [liuyf1117@hotmail.com](mailto:liuyf1117@hotmail.com)
- 💬 **Slack**: [Join our community](https://join.slack.com/t/solnera/shared_invite/zt-3m2oat3br-~2JsOdxNW7XPnEgb5bjUdg)
