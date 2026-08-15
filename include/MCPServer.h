#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// MCP protocol versions supported by this library. PROTOCOL_VERSION is the
// default, used when the client does not request a supported legacy version.
extern const char* const PROTOCOL_VERSION;
extern const char* const PROTOCOL_VERSION_2025_06_18;
extern const char* const PROTOCOL_VERSION_2025_03_26;
extern const char* const DEFAULT_SERVER_NAME;
extern const char* const DEFAULT_SERVER_VERSION;

// Upper bound on an accepted POST body. Larger requests are rejected with
// HTTP 413 before any buffer is allocated, so a hostile or buggy client cannot
// exhaust the heap by declaring a huge Content-Length.
#ifndef MCP_HTTP_MAX_BODY_SIZE
#define MCP_HTTP_MAX_BODY_SIZE 8192
#endif

// Depth of the tools/call hand-off queue between the async_tcp task and the
// worker task. A full queue answers new tool calls immediately with JSON-RPC
// -32000 ("server busy") instead of buffering without bound.
#ifndef MCP_HTTP_JOB_QUEUE_DEPTH
#define MCP_HTTP_JOB_QUEUE_DEPTH 4
#endif

// Stack size of the worker task that runs tool handlers.
#ifndef MCP_HTTP_WORKER_STACK_SIZE
#define MCP_HTTP_WORKER_STACK_SIZE 8192
#endif

// How long a tools/call may be waited on inline before falling back to the
// deferred chunked reply. The deferred path can only be written when the
// connection next polls (one lwIP coarse tick, ~500 ms), so without a short
// wait even a 2 ms tool answers in half a second. The cost is blocking
// async_tcp for at most this long. Set to 0 to always defer.
#ifndef MCP_HTTP_FAST_PATH_WAIT_MS
#define MCP_HTTP_FAST_PATH_WAIT_MS 20
#endif

#ifdef MCP_HTTP_TEST_HOOKS
/* Test-only: force the next `n` deferred-job allocations to fail (simulate
 * OOM). Compiled out of production builds. */
void mcp_http_test_fail_next_job_alloc(int n);
#endif

struct MCPRequest {
    std::string method;

    /* The parse result is retained whole and params() is a view into it, so a
     * tools/call payload is never deep-copied out into a second document. The
     * id keeps its own (scalar-sized) copy: parseRequest deliberately leaves it
     * null for a malformed id so the reply carries id:null, which a view into
     * doc could not express. */
    JsonDocument doc;
    JsonDocument idDoc;
    bool hasIdField;
    bool parseError;
    bool invalidRequest;
    // Set only once params has passed validation, so the early-return paths
    // expose no params at all.
    bool paramsChecked;

    MCPRequest()
        : method(""), hasIdField(false), parseError(false), invalidRequest(false), paramsChecked(false) {}

    JsonVariantConst params() const {
        return paramsChecked ? doc["params"] : JsonVariantConst();
    }

    JsonVariantConst id() const {
        return idDoc.as<JsonVariantConst>();
    }

    bool hasParams() const {
        return paramsChecked && !doc["params"].isNull();
    }

    bool isNotification() const {
        return !parseError && !hasIdField;
    }
};

struct MCPResponse {
    JsonDocument idDoc;
    JsonDocument resultDoc;
    JsonDocument errorDoc;

    /* Already-serialized result JSON, spliced straight into the reply. Lets a
     * handler hand back a cached body (tools/list) without rebuilding the tree
     * and without a document-to-document deep copy. Takes precedence over
     * resultDoc when non-empty. */
    std::string rawResult;

    int code;
    bool body;

    MCPResponse() : code(200), body(true) {}
    MCPResponse(const JsonVariantConst& id) : code(200), body(true) {
        idDoc.set(id);
    }
    MCPResponse(int code, const JsonVariantConst& id) : code(code), body(true) {
        idDoc.set(id);
    }
    MCPResponse(int code, bool body) : code(code), body(body) {}

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
        return !rawResult.empty() || !resultDoc.isNull();
    }
    bool hasError() const {
        return !errorDoc.isNull();
    }
    bool hasBody() const {
        return body;
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

    /* Error-reporting variant; this is what the dispatcher invokes. The default
     * runs call() and reports success, so existing single-method handlers keep
     * working unchanged. To signal an execution failure (surfaced to the client
     * as result.isError = true, per MCP), override BOTH overloads and set
     * isError here; call(params) can simply delegate:
     *   bool ignored; return call(std::move(params), ignored); */
    virtual JsonDocument call(JsonDocument params, bool& isError) {
        isError = false;
        return call(std::move(params));
    }
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

class MCPServer {
public:
    MCPServer(uint16_t port, const String& name = DEFAULT_SERVER_NAME,
              const String& version = DEFAULT_SERVER_VERSION,
              const String& instructions = "");
    ~MCPServer();

    void RegisterTool(const Tool& tool);
    // Overload for callers that can give up ownership: skips the deep copy of
    // the schema documents. Equivalent in every other respect.
    void RegisterTool(Tool&& tool);

    /* Starts the HTTP listener. Register all tools first: doing so afterwards
     * would race the request handlers against mutations of the tool registry.
     * Call only once WiFi is connected — setupMDNS() reads WiFi.localIP() to
     * publish the endpoint TXT record. Returns false if the server could not
     * be created; a worker-task failure is not fatal (tool calls then run
     * inline) and still returns true. */
    bool begin();

private:
    void setupWebServer();
    void setupMDNS();
    void handlePostComplete(AsyncWebServerRequest* request);
    void handleJsonBody(AsyncWebServerRequest* request, const char* body);
    void deferToolCall(AsyncWebServerRequest* request, MCPRequest&& mcpRequest);
    void sendJSONRPCError(AsyncWebServerRequest* request, int httpCode, ErrorCode rpcCode, const char* message);
    void sendMCPResponse(AsyncWebServerRequest* request, const MCPResponse& response);
    bool validateProtocolVersionHeader(AsyncWebServerRequest* request);
    bool validateOriginHeader(AsyncWebServerRequest* request);
    std::string generateUUID();
    std::string mdnsHostname() const;

    bool startWorker();
    void stopWorker();
    static void workerEntry(void* ctx);

    /* Protocol layer. Protected rather than private so the native test suite
     * can subclass and drive it without going through the HTTP transport. */
protected:
    MCPRequest parseRequest(const char* json);
    MCPRequest parseRequest(const std::string& json);
    std::string serializeResponse(const MCPResponse& response);
    MCPResponse createJSONRPCError(int httpCode, int rpcCode, const JsonVariantConst& id,
                                   const std::string& message);
    MCPResponse handle(MCPRequest& request);
    MCPResponse handleInitialize(MCPRequest& request);
    MCPResponse handlePing(MCPRequest& request);
    MCPResponse handleToolsList(MCPRequest& request);
    MCPResponse handleFunctionCalls(MCPRequest& request);
    bool isSupportedProtocolVersion(const char* version) const;
    const char* negotiateProtocolVersion(JsonVariantConst params) const;

    /* Serialized tools/list body, rebuilt only after the tool set changes.
     * Capabilities advertise listChanged:false, so between registrations this
     * is a constant — and clients ask for it on every session start. Guarded by
     * toolsMutex along with the registry itself, since tools/list is served on
     * async_tcp while tools/call runs on the worker task. */
    const std::string& toolsListJson();

    /* Keyed on std::string rather than Arduino String so a lookup key built
     * from the request costs no heap: short-string optimization keeps tool
     * names of ~15 characters entirely on the stack, whereas String always
     * allocates. */
    std::map<std::string, Tool> tools;
    std::string toolsListCache;
    bool toolsListDirty = true;
    std::mutex toolsMutex;

    AsyncWebServer* server;
    uint16_t port;
    bool started = false;

    String serverName;
    String serverVersion;
    String serverInstructions;

    /* tools/call handlers run on this worker task, fed through job_queue, so a
     * slow or blocking handler never stalls the async_tcp task (which services
     * every TCP connection in the firmware). Null when startWorker() failed —
     * tools/call then degrades to inline execution. */
    QueueHandle_t job_queue = nullptr;
    TaskHandle_t worker_handle = nullptr;
    SemaphoreHandle_t worker_done = nullptr;
    std::atomic<bool> worker_exit{false};
};

#endif  // MCP_SERVER_H
