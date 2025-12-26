# Vision

"Modern smart devices are rented, not owned—reliant on constant connections to centralized vendor clouds.

We are changing this with MCP-native hardware. By standardizing device communication through the Model Context Protocol, we empower users to bring their own intelligence (LLMs) to their devices. This is the first step toward a future of local-first AI and genuinely decentralized hardware ecosystems."

If you have ideas or want to collaborate, feel free to reach out by [email](mailto:liuyf1117@hotmail.com).

# ESP32 MCP Server

A lightweight Model Context Protocol (MCP) server implementation for the ESP32 microcontroller. This framework allows your ESP32 device to expose tools and resources to Large Language Models (LLMs) via the MCP standard, enabling direct interaction between AI agents and hardware.

## Features

- **Protocol Support**: Full implementation of the MCP JSON-RPC 2.0 protocol over HTTP.
- **Tool System**: Easy-to-use API for defining and registering custom tools with JSON Schema validation.
- **Session Management**: Built-in handling of MCP session IDs.
- **Asynchronous**: Built on `ESPAsyncWebServer` for non-blocking operation.
- **Schema Validation**: robust input/output schema definition using a fluent C++ API.

## Supported MCP Methods

The server currently supports the following MCP methods:
- `initialize`: Server handshake and capability negotiation.
- `notifications/initialized`: Client acknowledgment.
- `tools/list`: Discovery of available tools.
- `tools/call`: Execution of tool logic.

## Prerequisites

- **Hardware**: ESP32 development board.
- **Software**: 
  - [PlatformIO](https://platformio.org/) (recommended) or Arduino IDE.
  - Required Libraries:
    - `ArduinoJson`
    - `ESPAsyncWebServer`
    - `AsyncTCP` (for ESP32)

## Installation

1. Clone this repository:
   ```bash
   git clone https://github.com/yourusername/esp32-mcpserver.git
   ```
2. Open the project in PlatformIO.
3. Install the required dependencies (PlatformIO should handle this automatically via `platformio.ini`).

## Usage

`src/main.cpp` is an example sketch that demonstrates connecting to WiFi, starting the MCP server, and registering the `echo` tool.

### 1. Configuration

Open `src/main.cpp` and configure your WiFi credentials:

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
Tool myTool;
myTool.name = "echo";
myTool.description = "Echo back the input text";

// Define input schema
Properties textProperty;
textProperty.type = "string";
textProperty.description = "Text to echo back";

myTool.inputSchema.type = "object";
myTool.inputSchema.description = "Echo tool parameters";
myTool.inputSchema.properties["text"] = textProperty;
myTool.inputSchema.required.push_back("text");

// Attach handler
myTool.handler = std::make_shared<EchoHandler>();

// Register with server
mcpServer->RegisterTool(myTool);
```

### 4. Initialization

Initialize the server with a port, name, version, and optional system instructions:

```cpp
mcpServer = new MCPServer(3000, "echo service", "1.0.0",
                          "You are an intelligent device that supports the MCP protocol.");
```

## API Reference

The server exposes a single endpoint for MCP traffic:

- **Endpoint**: `POST /mcp`
- **Body**: JSON-RPC 2.0 Request
- **Headers**: 
  - `Content-Type: application/json`
  - `mcp-session-id`: (Optional) Session identifier
