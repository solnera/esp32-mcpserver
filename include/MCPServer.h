#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <map>
#include <memory>
#include <vector>

// MCP protocol version
const char* const PROTOCOL_VERSION = "2024-11-05";
const char* const DEFAULT_SERVER_NAME = "ESP32-MCP-Server";
const char* const DEFAULT_SERVER_VERSION = "1.0.0";

struct MCPRequest {
    std::string method;
    JsonDocument idDoc;  // Store id as JsonDocument to preserve type (string, number, or null)
    JsonDocument paramsDoc;

    MCPRequest() : method("") {}

    // Get JsonVariant reference of params
    JsonVariantConst params() const {
        return paramsDoc.as<JsonVariantConst>();
    }

    // Get JsonVariant reference of id
    JsonVariantConst id() const {
        return idDoc.as<JsonVariantConst>();
    }

    // Check if params is empty
    bool hasParams() const {
        return !paramsDoc.isNull();
    }
};

struct MCPResponse {
    JsonDocument idDoc;  // Store id as JsonDocument to preserve type
    JsonDocument resultDoc;
    JsonDocument errorDoc;
    int code;  // http status code

    MCPResponse() : code(200) {}
    MCPResponse(int code, const JsonVariantConst& id) : code(code) {
        idDoc.set(id);
    }

    // Get JsonVariant reference of id, result and error
    JsonVariantConst id() const {
        return idDoc.as<JsonVariantConst>();
    }
    JsonVariantConst result() const {
        return resultDoc.as<JsonVariantConst>();
    }
    JsonVariantConst error() const {
        return errorDoc.as<JsonVariantConst>();
    }

    // Check if result or error is empty
    bool hasResult() const {
        return !resultDoc.isNull();
    }
    bool hasError() const {
        return !errorDoc.isNull();
    }
};

// JSON-RPC error codes
enum class ErrorCode {
    SERVER_ERROR = -32000,
    INVALID_REQUEST = -32600,
    METHOD_NOT_FOUND = -32601,
    INVALID_PARAMS = -32602,
    INTERNAL_ERROR = -32603,
    PARSE_ERROR = -32700
};

class ToolHandler {
   public:
    virtual ~ToolHandler() = default;
    virtual JsonDocument call(JsonDocument params) = 0;
};

class Properties {
   public:
    Properties() = default;

    // Enable deep copy semantics to support usage in STL containers and assignment
    Properties(const Properties& other) {
        type = other.type;
        title = other.title;
        description = other.description;
        properties = other.properties;
        required = other.required;
        additionalProperties = other.additionalProperties;
        hasAdditionalProperties = other.hasAdditionalProperties;
        if (other.items) {
            items.reset(new Properties(*other.items));
        } else {
            items.reset(nullptr);
        }
        enumValues = other.enumValues;
        oneOf = other.oneOf;
        anyOf = other.anyOf;
        allOf = other.allOf;
        format = other.format;
        defaultValue = other.defaultValue;
    }

    Properties& operator=(const Properties& other) {
        if (this != &other) {
            type = other.type;
            title = other.title;
            description = other.description;
            properties = other.properties;
            required = other.required;
            additionalProperties = other.additionalProperties;
            hasAdditionalProperties = other.hasAdditionalProperties;
            if (other.items) {
                items.reset(new Properties(*other.items));
            } else {
                items.reset(nullptr);
            }
            enumValues = other.enumValues;
            oneOf = other.oneOf;
            anyOf = other.anyOf;
            allOf = other.allOf;
            format = other.format;
            defaultValue = other.defaultValue;
        }
        return *this;
    }

    Properties(Properties&&) = default;
    Properties& operator=(Properties&&) = default;

    // JSON Schema basic information: type field supporting object, array, string, number, integer, boolean, null
    String type;
    // Optional JSON Schema title providing a human-readable short name
    String title;
    // Optional JSON Schema description explaining the purpose of the field or structure
    String description;
    // Used when type is object to represent the set of object properties
    std::map<String, Properties> properties;
    // Used when type is object to represent the list of required property names
    std::vector<String> required;

    // Controls whether the object allows additional properties
    // The additionalProperties keyword is serialized only if hasAdditionalProperties is true
    bool additionalProperties = true;
    bool hasAdditionalProperties = false;

    // Used when type is array to define the schema of array elements
    std::unique_ptr<Properties> items;

    // Corresponds to the JSON Schema enum keyword, representing the set of allowed values (stored as strings)
    std::vector<String> enumValues;

    // Corresponds to the JSON Schema oneOf keyword, representing one of several mutually exclusive schemas
    std::vector<Properties> oneOf;
    // Corresponds to the JSON Schema anyOf keyword, where matching any single schema is sufficient
    std::vector<Properties> anyOf;
    // Corresponds to the JSON Schema allOf keyword, requiring all schemas to be satisfied
    std::vector<Properties> allOf;

    // Aligns with the JSON Schema format keyword, such as "uri" or "date-time"
    String format;

    // Optional default value aligned with the JSON Schema default keyword (stored as a string)
    String defaultValue;

    String toString() const;
    void toJson(JsonObject& obj) const;
};

// Tool definition
class Tool {
   public:
    Tool() = default;
    Tool(const Tool&) = default;
    Tool& operator=(const Tool&) = default;
    Tool(Tool&&) = default;
    Tool& operator=(Tool&&) = default;

    // Tool name corresponding to tool.name in the MCP specification, must be unique and follow naming constraints
    String name;
    // Tool description corresponding to tool.description in the MCP specification, describing tool behavior
    String description;
    // Input parameter schema corresponding to tool.inputSchema in the MCP specification
    Properties inputSchema;
    // Output result schema corresponding to tool.outputSchema in the MCP specification
    Properties outputSchema;
    // Tool handler implementing the actual business logic
    std::shared_ptr<ToolHandler> handler;

    String toString() const;
};

class MCPServer {
   public:
    MCPServer(uint16_t port, const String& name = DEFAULT_SERVER_NAME, const String& version = DEFAULT_SERVER_VERSION,
              const String& instructions = "");
    ~MCPServer();
    void RegisterTool(const Tool& tool);

   private:
    void setupWebServer();
    std::string generateSessionId();
    std::string serializeResponse(const MCPResponse& response);
    std::string createHTTPResponse(bool success, const std::string& message, const JsonVariant& data);
    std::string createJSONRPCResponse(const JsonVariant& id, const JsonVariant& result);

    MCPRequest parseRequest(const std::string& json);

    MCPResponse createJSONRPCError(int httpCode, int code, const JsonVariantConst& id, const std::string& message);
    MCPResponse handle(MCPRequest& request);
    MCPResponse handleInitialize(MCPRequest& request);
    MCPResponse handleInitialized(MCPRequest& request);
    MCPResponse handleToolsList(MCPRequest& request);
    MCPResponse handleFunctionCalls(MCPRequest& request);

   private:
    std::map<String, Tool> tools;
    AsyncWebServer* server;
    String serverName;
    String serverVersion;
    String serverInstructions;
};
#endif  // MCP_SERVER_H
