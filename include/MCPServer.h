#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

// MCP protocol version
constexpr const char* PROTOCOL_VERSION = "2024-11-05";
constexpr const char* DEFAULT_SERVER_NAME = "ESP32-MCP-Server";
constexpr const char* DEFAULT_SERVER_VERSION = "1.0.0";

struct MCPRequest {
    std::string method;
    JsonDocument idDoc;
    JsonDocument paramsDoc;

    MCPRequest() : method("") {}

    JsonVariantConst params() const {
        return paramsDoc.as<JsonVariantConst>();
    }

    JsonVariantConst id() const {
        return idDoc.as<JsonVariantConst>();
    }

    bool hasParams() const {
        return !paramsDoc.isNull();
    }
};

struct MCPResponse {
    JsonDocument idDoc;
    JsonDocument resultDoc;
    JsonDocument errorDoc;
    int code;

    MCPResponse() : code(200) {}
    MCPResponse(int code, const JsonVariantConst& id) : code(code) {
        idDoc.set(id);
    }

    JsonVariantConst id() const {
        return idDoc.as<JsonVariantConst>();
    }
    JsonVariantConst result() const {
        return resultDoc.as<JsonVariantConst>();
    }
    JsonVariantConst error() const {
        return errorDoc.as<JsonVariantConst>();
    }

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

// Fluent JSON Schema builder
class Schema {
public:
    Schema() = default;

    // Type factory methods
    static Schema object()  { Schema s; s.doc_["type"] = "object"; s.doc_["properties"].to<JsonObject>(); return s; }
    static Schema string()  { Schema s; s.doc_["type"] = "string";  return s; }
    static Schema integer() { Schema s; s.doc_["type"] = "integer"; return s; }
    static Schema number()  { Schema s; s.doc_["type"] = "number";  return s; }
    static Schema boolean() { Schema s; s.doc_["type"] = "boolean"; return s; }
    static Schema array()   { Schema s; s.doc_["type"] = "array";   return s; }
    static Schema null()    { Schema s; s.doc_["type"] = "null";    return s; }

    // Common modifiers
    Schema& description(const char* desc) { doc_["description"] = desc; return *this; }
    Schema& title(const char* t)          { doc_["title"] = t;         return *this; }
    Schema& format(const char* f)         { doc_["format"] = f;        return *this; }

    // Default value (supports any JSON type)
    template <typename T>
    Schema& defaultValue(T value) { doc_["default"] = value; return *this; }

    // Object modifiers
    Schema& property(const char* name, Schema prop) {
        doc_["properties"][name].set(prop.doc_);
        return *this;
    }
    Schema& required(std::initializer_list<const char*> fields) {
        for (auto& f : fields) doc_["required"].add(f);
        return *this;
    }
    Schema& additionalProperties(bool v) {
        doc_["additionalProperties"] = v;
        return *this;
    }

    // Array modifiers
    Schema& items(Schema item) {
        doc_["items"].set(item.doc_);
        return *this;
    }
    Schema& minItems(int n) { doc_["minItems"] = n; return *this; }
    Schema& maxItems(int n) { doc_["maxItems"] = n; return *this; }

    // Number/integer modifiers
    Schema& minimum(double v)          { doc_["minimum"] = v;          return *this; }
    Schema& maximum(double v)          { doc_["maximum"] = v;          return *this; }
    Schema& exclusiveMinimum(double v) { doc_["exclusiveMinimum"] = v; return *this; }
    Schema& exclusiveMaximum(double v) { doc_["exclusiveMaximum"] = v; return *this; }
    Schema& multipleOf(double v)       { doc_["multipleOf"] = v;       return *this; }

    // String modifiers
    Schema& minLength(int n) { doc_["minLength"] = n; return *this; }
    Schema& maxLength(int n) { doc_["maxLength"] = n; return *this; }
    Schema& pattern(const char* p) { doc_["pattern"] = p; return *this; }

    // Enum
    Schema& enumValues(std::initializer_list<const char*> vals) {
        for (auto& v : vals) doc_["enum"].add(v);
        return *this;
    }
    // Enum for numeric values
    template <typename T>
    Schema& enumValues(std::initializer_list<T> vals) {
        for (auto& v : vals) doc_["enum"].add(v);
        return *this;
    }

    // Composition
    Schema& oneOf(std::initializer_list<Schema> schemas) {
        for (auto& s : schemas) doc_["oneOf"].add(s.doc_);
        return *this;
    }
    Schema& anyOf(std::initializer_list<Schema> schemas) {
        for (auto& s : schemas) doc_["anyOf"].add(s.doc_);
        return *this;
    }
    Schema& allOf(std::initializer_list<Schema> schemas) {
        for (auto& s : schemas) doc_["allOf"].add(s.doc_);
        return *this;
    }

    // Build to JsonDocument
    JsonDocument build() { return std::move(doc_); }

private:
    JsonDocument doc_;
};

class ToolHandler {
public:
    virtual ~ToolHandler() = default;
    virtual JsonDocument call(JsonDocument params) = 0;
};

// Tool definition
class Tool {
public:
    Tool() = default;

    String name;
    String description;
    JsonDocument inputSchema;
    JsonDocument outputSchema;
    std::shared_ptr<ToolHandler> handler;
};

// Flexible array body buffer, allocated with a single malloc() so that
// ESPAsyncWebServer's free(_tempObject) cleanly releases everything.
struct BodyBuffer {
    size_t length;
    size_t capacity;
    char data[];  // C99 flexible array member (supported by GCC/Clang)

    static BodyBuffer* create(size_t capacity) {
        auto* buf = static_cast<BodyBuffer*>(malloc(sizeof(BodyBuffer) + capacity + 1));
        if (buf) {
            buf->length = 0;
            buf->capacity = capacity;
            buf->data[0] = '\0';
        }
        return buf;
    }
};

class MCPServer {
public:
    MCPServer(uint16_t port, const String& name = DEFAULT_SERVER_NAME,
              const String& version = DEFAULT_SERVER_VERSION,
              const String& instructions = "");
    ~MCPServer();
    void RegisterTool(const Tool& tool);

private:
    void setupWebServer();
    std::string generateSessionId();
    std::string serializeResponse(const MCPResponse& response);

    MCPRequest parseRequest(const std::string& json);

    MCPResponse createJSONRPCError(int httpCode, int code, const JsonVariantConst& id,
                                   const std::string& message);
    MCPResponse handle(MCPRequest& request);
    MCPResponse handleInitialize(MCPRequest& request);
    MCPResponse handleInitialized(MCPRequest& request);
    MCPResponse handleToolsList(MCPRequest& request);
    MCPResponse handleFunctionCalls(MCPRequest& request);

private:
    std::map<String, Tool> tools;
    std::mutex toolsMutex;
    AsyncWebServer* server;
    String serverName;
    String serverVersion;
    String serverInstructions;
};

#endif  // MCP_SERVER_H
