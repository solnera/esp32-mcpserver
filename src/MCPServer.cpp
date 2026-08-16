#include "MCPServer.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <utility>

const char* const PROTOCOL_VERSION = "2025-11-25";
const char* const PROTOCOL_VERSION_2025_06_18 = "2025-06-18";
const char* const PROTOCOL_VERSION_2025_03_26 = "2025-03-26";
const char* const DEFAULT_SERVER_NAME = "ESP32-MCP-Server";
const char* const DEFAULT_SERVER_VERSION = "1.0.0";

namespace {

/* Accumulated POST body, stored in request->_tempObject. ESPAsyncWebServer
 * releases _tempObject with free() when a request dies (including aborted
 * uploads), so this must be one malloc() block with no destructor — anything
 * else either leaks or corrupts the heap on disconnect. */
struct BodyBuffer {
    size_t received;
    uint8_t status;
    char data[1];  // over-allocated to hold the body plus a NUL terminator
};

enum : uint8_t {
    BODY_OK = 0,
    BODY_TOO_LARGE = 1,
    BODY_NO_LENGTH = 2,
};

bool isJsonContentType(const String& value) {
    static const char expected[] = "application/json";
    const char* cursor = value.c_str();
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }
    for (size_t i = 0; expected[i] != '\0'; ++i, ++cursor) {
        if (std::tolower(static_cast<unsigned char>(*cursor)) != expected[i]) {
            return false;
        }
    }
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }
    return *cursor == '\0' || *cursor == ';';
}

/* One deferred tools/call. Shared between the async_tcp task (the chunked
 * response filler) and the worker task. Reference-counted intrusively and
 * allocated with new (std::nothrow) so that running out of memory on the
 * request path is a graceful HTTP 500 in BOTH exception modes — make_shared
 * would abort the device under -fno-exceptions. One reference belongs to the
 * queue/worker hand-off, one to the response filler; whichever drops last
 * frees the job. */
struct HttpToolJob {
    MCPRequest request;
    std::string response;  // serialized JSON-RPC; valid once done is set
    std::atomic<bool> done{false};
    std::atomic<int> refs{1};

    static void release(HttpToolJob* job) {
        if (job && job->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete job;
        }
    }
};

/* Copyable job handle for the filler std::function (which requires copyable
 * captures): every copy owns one reference. */
class JobRef {
public:
    explicit JobRef(HttpToolJob* job) : job_(job) {}  // adopts an existing reference
    JobRef(const JobRef& other) : job_(other.job_) {
        if (job_) {
            job_->refs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    JobRef(JobRef&& other) noexcept : job_(other.job_) { other.job_ = nullptr; }
    JobRef& operator=(const JobRef&) = delete;
    ~JobRef() { HttpToolJob::release(job_); }
    HttpToolJob* operator->() const { return job_; }

private:
    HttpToolJob* job_;
};

/* Routes the MCP endpoint by path alone.
 *
 * server->on() cannot be used here. MCP's Streamable HTTP transport requires
 * every client POST to carry `Accept: application/json, text/event-stream`, and
 * ESPAsyncWebServer reads any Accept containing text/event-stream as an SSE
 * subscription: it marks the request RCT_EVENT, after which
 * AsyncCallbackWebHandler::canHandle() declines it (that check insists on
 * isHTTP()) and the request falls through to the catch-all. The result is a 404
 * for every spec-conformant MCP client, on both POST and the GET stream probe,
 * while a hand-written curl without the header works fine.
 *
 * Matching on the path and dispatching on the method ourselves keeps the
 * connection type out of the routing decision. Nothing downstream consults it:
 * RCT_EVENT only ever gates handler selection. */
class MCPEndpointHandler : public AsyncWebHandler {
public:
    using RequestFn = std::function<void(AsyncWebServerRequest*)>;
    using BodyFn = std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)>;

    MCPEndpointHandler(const char* uri, RequestFn onRequest, BodyFn onBody)
        : uri_(uri), onRequest_(std::move(onRequest)), onBody_(std::move(onBody)) {}

    bool canHandle(AsyncWebServerRequest* request) const override {
        return request->url() == uri_;
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        onRequest_(request);
    }

    void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index,
                    size_t total) override {
        onBody_(request, data, len, index, total);
    }

    // A "trivial" handler never has the request body delivered to it.
    bool isRequestHandlerTrivial() const override {
        return false;
    }

private:
    String uri_;
    RequestFn onRequest_;
    BodyFn onBody_;
};

/* Appending sink for serializeJson. ArduinoJson's std::string writer clears the
 * destination in its constructor, so emitting several documents into one buffer
 * needs a writer that only ever appends; the default Writer template picks this
 * up by duck-typing on write(). */
struct StringAppender {
    std::string* out;

    size_t write(uint8_t c) {
        out->push_back(static_cast<char>(c));
        return 1;
    }

    size_t write(const uint8_t* s, size_t n) {
        out->append(reinterpret_cast<const char*>(s), n);
        return n;
    }
};

}  // namespace

#ifdef MCP_HTTP_TEST_HOOKS
static int s_fail_next_job_alloc = 0;
void mcp_http_test_fail_next_job_alloc(int n) {
    s_fail_next_job_alloc = n;
}
#endif

MCPServer::MCPServer(uint16_t port, const String& name, const String& version, const String& instructions)
    : server(nullptr), port(port), serverName(name), serverVersion(version), serverInstructions(instructions) {
    server = new AsyncWebServer(port);
}

MCPServer::~MCPServer() {
    /* Delete the server first so async_tcp stops feeding job_queue, then join
     * the worker. Like the AsyncWebServer it wraps, destruction is only safe
     * once no request is in flight. */
    if (server) {
        delete server;
        server = nullptr;
    }
    stopWorker();
}

bool MCPServer::begin() {
    if (started) {
        return true;
    }
    if (!server) {
        return false;
    }

    // Best-effort: on failure tools/call degrades to inline execution.
    startWorker();
    if (!setupWebServer()) {
        stopWorker();  // nothing will ever feed it
        return false;
    }
    setupMDNS();
    started = true;
    return true;
}

// ---------------------------------------------------------------------------
// Worker task
// ---------------------------------------------------------------------------

bool MCPServer::startWorker() {
    job_queue = xQueueCreate(MCP_HTTP_JOB_QUEUE_DEPTH, sizeof(HttpToolJob*));
    if (!job_queue) {
        Serial.println("[MCP] Failed to create job queue; tool calls run inline");
        return false;
    }
    worker_done = xSemaphoreCreateBinary();
    if (!worker_done) {
        Serial.println("[MCP] Failed to create worker semaphore; tool calls run inline");
        vQueueDelete(job_queue);
        job_queue = nullptr;
        return false;
    }
    worker_exit.store(false, std::memory_order_relaxed);
    if (xTaskCreate(MCPServer::workerEntry, "mcp_http_worker", MCP_HTTP_WORKER_STACK_SIZE, this, 1,
                    &worker_handle) != pdPASS) {
        Serial.println("[MCP] Failed to create worker task; tool calls run inline");
        vSemaphoreDelete(worker_done);
        worker_done = nullptr;
        vQueueDelete(job_queue);
        job_queue = nullptr;
        worker_handle = nullptr;
        return false;
    }
    return true;
}

void MCPServer::stopWorker() {
    if (worker_handle) {
        /* Raise the flag, then wake the task. A 0-tick send into a full queue
         * is fine — a full queue means the worker has work queued and
         * re-checks worker_exit per dequeue. */
        worker_exit.store(true, std::memory_order_release);
        HttpToolJob* sentinel = nullptr;
        if (job_queue) {
            xQueueSend(job_queue, &sentinel, 0);
        }
        if (worker_done) {
            xSemaphoreTake(worker_done, portMAX_DELAY);
        }
        worker_handle = nullptr;
    }

    if (job_queue) {
        HttpToolJob* job = nullptr;
        while (xQueueReceive(job_queue, &job, 0) == pdTRUE) {
            HttpToolJob::release(job);  // undelivered job; any live response filler still owns its ref
        }
        vQueueDelete(job_queue);
        job_queue = nullptr;
    }

    if (worker_done) {
        vSemaphoreDelete(worker_done);
        worker_done = nullptr;
    }
}

void MCPServer::workerEntry(void* ctx) {
    auto* self = static_cast<MCPServer*>(ctx);
    HttpToolJob* job = nullptr;
    for (;;) {
        if (xQueueReceive(self->job_queue, &job, portMAX_DELAY) == pdTRUE) {
            if (self->worker_exit.load(std::memory_order_acquire)) {
                HttpToolJob::release(job);  // sentinel (null) or an undelivered job
                break;
            }
            if (job) {
                MCPResponse mcpResponse = self->handle(job->request);
                job->response = self->serializeResponse(mcpResponse);
                job->done.store(true, std::memory_order_release);
                HttpToolJob::release(job);
                job = nullptr;
            }
        }
    }
    if (self->worker_done) {
        xSemaphoreGive(self->worker_done);
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// HTTP transport
// ---------------------------------------------------------------------------

bool MCPServer::setupWebServer() {
    /* The body callback only accumulates; the response is always sent from the
     * request callback, which runs once the request is complete — including
     * when there is no body at all. */
    auto* endpoint = new (std::nothrow) MCPEndpointHandler(
        "/mcp", [this](AsyncWebServerRequest* request) { handleEndpointRequest(request); },
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            BodyBuffer* body = static_cast<BodyBuffer*>(request->_tempObject);
            if (!body) {
                if (index != 0) {
                    return;  // first-chunk allocation failed; drop the rest
                }
                uint8_t status = BODY_OK;
                size_t cap = total;
                if (total == 0) {
                    // Chunked upload with no Content-Length; we can't size the buffer.
                    status = BODY_NO_LENGTH;
                    cap = 0;
                } else if (total > MCP_HTTP_MAX_BODY_SIZE) {
                    status = BODY_TOO_LARGE;
                    cap = 0;
                }
                body = static_cast<BodyBuffer*>(malloc(sizeof(BodyBuffer) + cap));
                if (!body) {
                    return;  // handlePostComplete reports the OOM
                }
                body->received = 0;
                body->status = status;
                request->_tempObject = body;
            }

            if (body->status != BODY_OK || index >= total) {
                return;
            }
            if (index + len > total) {
                len = total - index;  // clamp clients that overshoot their declared Content-Length
            }
            memcpy(body->data + index, data, len);
            body->received = index + len;
        });
    if (!endpoint) {
        return false;
    }
    server->addHandler(endpoint);  // the server owns it from here

    server->onNotFound([this](AsyncWebServerRequest* request) {
        if (!validateOriginHeader(request)) {
            sendJSONRPCError(request, 403, ErrorCode::INVALID_REQUEST, "Forbidden Origin");
            return;
        }
        sendJSONRPCError(request, 404, ErrorCode::INVALID_REQUEST, "Path Not Found");
    });

    server->begin();
    return true;
}

/* Every method reaching /mcp lands here, since the endpoint handler matches on
 * path alone. POST carries the JSON-RPC traffic; everything else — including the
 * GET the MCP client uses to probe for a server-initiated SSE stream, which this
 * server does not offer — is answered with the 405 the spec prescribes. */
void MCPServer::handleEndpointRequest(AsyncWebServerRequest* request) {
    if (request->method() == HTTP_POST) {
        handlePostComplete(request);
        return;
    }

    if (!validateOriginHeader(request)) {
        sendJSONRPCError(request, 403, ErrorCode::INVALID_REQUEST, "Forbidden Origin");
        return;
    }
    AsyncWebServerResponse* response =
        request->beginResponse(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
    response->addHeader("Allow", "POST");
    request->send(response);
}

std::string MCPServer::generateUUID() {
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[16];
    for (uint8_t i = 0; i < sizeof(bytes); i += 4) {
        uint32_t value = esp_random();
        bytes[i] = (value >> 24) & 0xFF;
        bytes[i + 1] = (value >> 16) & 0xFF;
        bytes[i + 2] = (value >> 8) & 0xFF;
        bytes[i + 3] = value & 0xFF;
    }
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char buf[37];
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            buf[pos++] = '-';
        }
        buf[pos++] = hex[(bytes[i] >> 4) & 0x0F];
        buf[pos++] = hex[bytes[i] & 0x0F];
    }
    buf[pos] = '\0';
    return std::string(buf, pos);
}

void MCPServer::setupMDNS() {
    const std::string hostname = mdnsHostname();
    if (!MDNS.begin(hostname.c_str())) {
        return;
    }

    std::string usn = "uuid:" + generateUUID() + "::mcp:device";
    std::string endpoint =
        "http://" + std::string(WiFi.localIP().toString().c_str()) + ":" + std::to_string(port) + "/mcp";

    MDNS.addService("_mcp", "_tcp", port);
    MDNS.addServiceTxt("_mcp", "_tcp", "path", "/mcp");
    MDNS.addServiceTxt("_mcp", "_tcp", "endpoint", endpoint.c_str());
    MDNS.addServiceTxt("_mcp", "_tcp", "device_type", "mcp:device");
    MDNS.addServiceTxt("_mcp", "_tcp", "usn", usn.c_str());
}

std::string MCPServer::mdnsHostname() const {
    std::string hostname;
    hostname.reserve(serverName.length());
    bool lastWasDash = false;
    for (const char* p = serverName.c_str(); *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c)) {
            hostname.push_back(static_cast<char>(std::tolower(c)));
            lastWasDash = false;
        } else if (!hostname.empty() && !lastWasDash) {
            hostname.push_back('-');
            lastWasDash = true;
        }
    }
    while (!hostname.empty() && hostname.back() == '-') {
        hostname.pop_back();
    }
    if (hostname.empty()) {
        hostname = "esp32-mcp";
    }
    if (hostname.size() > 63) {
        hostname.resize(63);
        while (!hostname.empty() && hostname.back() == '-') {
            hostname.pop_back();
        }
    }
    return hostname;
}

/* For transport-level rejections where no request id is known (pre-parse
 * paths only) — the JSON-RPC id is null, as the spec requires when the id
 * could not be detected. Once a request has been parsed, build the error
 * with createJSONRPCError(request.id()) instead so the id is echoed. */
void MCPServer::sendJSONRPCError(AsyncWebServerRequest* request, int httpCode, ErrorCode rpcCode,
                                 const char* message) {
    JsonDocument nullId;
    nullId.set(nullptr);
    MCPResponse error =
        createJSONRPCError(httpCode, static_cast<int>(rpcCode), nullId.as<JsonVariantConst>(), message);
    sendMCPResponse(request, error);
}

void MCPServer::handlePostComplete(AsyncWebServerRequest* request) {
    BodyBuffer* body = static_cast<BodyBuffer*>(request->_tempObject);

    if (!validateOriginHeader(request)) {
        if (body) {
            free(body);
            request->_tempObject = nullptr;
        }
        sendJSONRPCError(request, 403, ErrorCode::INVALID_REQUEST, "Forbidden Origin");
        return;
    }

    if (!isJsonContentType(request->contentType())) {
        if (body) {
            free(body);
            request->_tempObject = nullptr;
        }
        sendJSONRPCError(request, 415, ErrorCode::INVALID_REQUEST, "Content-Type must be application/json");
        return;
    }

    if (!body) {
        if (request->contentLength() == 0) {
            // No body at all; let the parser produce the JSON-RPC parse error.
            handleJsonBody(request, "");
            return;
        }
        // The body callback could not allocate the accumulation buffer.
        sendJSONRPCError(request, 500, ErrorCode::INTERNAL_ERROR, "Out of memory");
        return;
    }

    const uint8_t status = body->status;
    const size_t received = body->received;
    if (status == BODY_OK && received == request->contentLength()) {
        body->data[received] = '\0';
        handleJsonBody(request, body->data);
    } else if (status == BODY_TOO_LARGE) {
        sendJSONRPCError(request, 413, ErrorCode::INVALID_REQUEST, "Request body too large");
    } else if (status == BODY_NO_LENGTH) {
        sendJSONRPCError(request, 411, ErrorCode::INVALID_REQUEST, "Content-Length required");
    } else {
        sendJSONRPCError(request, 400, ErrorCode::INVALID_REQUEST, "Incomplete request body");
    }

    free(body);
    request->_tempObject = nullptr;
}

void MCPServer::handleJsonBody(AsyncWebServerRequest* request, const char* body) {
    MCPRequest mcpReq = parseRequest(body);

    if (!validateProtocolVersionHeader(request)) {
        MCPResponse invalidVersion = createJSONRPCError(400, static_cast<int>(ErrorCode::INVALID_PARAMS),
                                                        mcpReq.id(), "Unsupported MCP-Protocol-Version");
        sendMCPResponse(request, invalidVersion);
        return;
    }

    /* tools/call runs user code of unknown duration, so it executes on the
     * worker task and is answered with a deferred chunked response. Everything
     * else — protocol methods, notifications, malformed requests — is pure
     * in-memory JSON work and stays inline. */
    if (worker_handle && mcpReq.method == "tools/call" && !mcpReq.isNotification()) {
        deferToolCall(request, std::move(mcpReq));
        return;
    }

    MCPResponse mcpRes = handle(mcpReq);
    sendMCPResponse(request, mcpRes);
}

void MCPServer::deferToolCall(AsyncWebServerRequest* request, MCPRequest&& mcpRequest) {
    /* Deferred responses rely on chunked framing, which needs HTTP/1.1.
     * ESPAsyncWebServer would cope with a version-0 request on its own —
     * beginChunkedResponse routes it to a non-chunked response whose body is
     * close-delimited — but rejecting explicitly is safer than trusting an
     * untested fallback. Inline methods (initialize, tools/list, ping) still
     * serve HTTP/1.0. */
    if (request->version() == 0) {
        MCPResponse unsupported =
            createJSONRPCError(505, static_cast<int>(ErrorCode::SERVER_ERROR), mcpRequest.id(),
                               "HTTP/1.1 required for deferred tool calls");
        sendMCPResponse(request, unsupported);
        return;
    }

    HttpToolJob* job = nullptr;
#ifdef MCP_HTTP_TEST_HOOKS
    if (s_fail_next_job_alloc > 0) {
        s_fail_next_job_alloc--;
    } else {
        job = new (std::nothrow) HttpToolJob();
    }
#else
    job = new (std::nothrow) HttpToolJob();
#endif
    if (!job) {
        /* Graceful in both exception modes: a client hammering a device that
         * is out of memory gets 500s, not reboots. The request is parsed by
         * now, so echo its id — sendJSONRPCError is for pre-parse rejections
         * and would answer id:null, which strict clients cannot correlate. */
        MCPResponse oom = createJSONRPCError(500, static_cast<int>(ErrorCode::INTERNAL_ERROR),
                                             mcpRequest.id(), "Out of memory");
        sendMCPResponse(request, oom);
        return;
    }
    job->request = std::move(mcpRequest);

    job->refs.fetch_add(1, std::memory_order_relaxed);  // the queue/worker reference
    if (xQueueSend(job_queue, &job, 0) != pdTRUE) {
        HttpToolJob::release(job);  // the hand-off that never happened
        /* Answer right away rather than queueing without bound. 200 + JSON-RPC
         * error keeps the failure parseable by MCP SDKs (a bare 503 would
         * surface as an opaque transport error). */
        MCPResponse busy = createJSONRPCError(200, static_cast<int>(ErrorCode::SERVER_ERROR),
                                              job->request.id(), "Server busy: tool call queue is full");
        sendMCPResponse(request, busy);
        HttpToolJob::release(job);  // the creator reference; frees the job
        return;
    }

    JobRef ref(job);  // adopts the creator reference

    /* Fast path. The deferred reply below can only be written when the filler
     * next runs, and that is driven by tcp_poll at one lwIP coarse tick —
     * ~500 ms. Most tools (read a pin, read a sensor) finish in single-digit
     * milliseconds, so without this a 2 ms tool still answered in half a
     * second. Giving the worker a brief bounded window and replying inline when
     * it lands removes that floor for the common case.
     *
     * This does block async_tcp, which is exactly what the worker exists to
     * avoid — but bounded by a handful of milliseconds, not by an arbitrary
     * handler. Set MCP_HTTP_FAST_PATH_WAIT_MS to 0 to opt out entirely and
     * always defer. */
#if MCP_HTTP_FAST_PATH_WAIT_MS > 0
    for (uint32_t waited = 0; waited < MCP_HTTP_FAST_PATH_WAIT_MS; waited++) {
        if (ref->done.load(std::memory_order_acquire)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (ref->done.load(std::memory_order_acquire)) {
        /* Plain, length-delimited response: no chunk framing, and one fewer
         * round of filler callbacks. */
        AsyncWebServerResponse* inlineResponse =
            request->beginResponse(200, "application/json", ref->response.c_str());
        inlineResponse->addHeader("MCP-Protocol-Version", PROTOCOL_VERSION);
        request->send(inlineResponse);
        return;
    }
#endif

    /* The filler runs on the async_tcp task whenever the TCP window opens or
     * the connection polls (~500 ms). It captures only the job — never `this` —
     * so a response outliving the server cannot touch freed state. Returning
     * RESPONSE_TRY_AGAIN until the worker finishes keeps the connection open
     * without blocking; ESPAsyncWebServer already disables the 3 s RX idle
     * timeout for the connection once send() is called. Returning 0 emits the
     * terminating chunk. */
    AsyncWebServerResponse* httpResponse = request->beginChunkedResponse(
        "application/json", [ref](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (!ref->done.load(std::memory_order_acquire)) {
                return RESPONSE_TRY_AGAIN;
            }
            const std::string& payload = ref->response;
            if (index >= payload.size()) {
                return 0;
            }
            size_t n = payload.size() - index;
            if (n > maxLen) {
                n = maxLen;
            }
            memcpy(buffer, payload.data() + index, n);
            return n;
        });
    httpResponse->addHeader("MCP-Protocol-Version", PROTOCOL_VERSION);
    request->send(httpResponse);
}

void MCPServer::sendMCPResponse(AsyncWebServerRequest* request, const MCPResponse& response) {
    std::string jsonResponse = serializeResponse(response);
    if (!response.hasBody() || jsonResponse.empty()) {
        request->send(response.code);
        return;
    }

    AsyncWebServerResponse* httpResponse =
        request->beginResponse(response.code, "application/json", jsonResponse.c_str());
    httpResponse->addHeader("MCP-Protocol-Version", PROTOCOL_VERSION);
    request->send(httpResponse);
}

bool MCPServer::validateProtocolVersionHeader(AsyncWebServerRequest* request) {
    if (!request->hasHeader("MCP-Protocol-Version")) {
        return true;
    }

    String value = request->getHeader("MCP-Protocol-Version")->value();
    return isSupportedProtocolVersion(value.c_str());
}

/* Origin and Host matching alone does not stop DNS rebinding: after a rebind,
 * both still contain the attacker's domain. Accept browser traffic only when
 * both authorities identify this device directly by its current IP address or
 * by the mDNS name we advertise. Native clients normally omit Origin. */
bool MCPServer::validateOriginHeader(AsyncWebServerRequest* request) {
    if (!request->hasHeader("Origin")) {
        return true;
    }
    if (!request->hasHeader("Host")) {
        return false;
    }

    std::string origin(request->getHeader("Origin")->value().c_str());
    std::string host(request->getHeader("Host")->value().c_str());
    const char httpPrefix[] = "http://";
    if (origin.compare(0, sizeof(httpPrefix) - 1, httpPrefix) != 0) {
        return false;  // this server does not provide TLS
    }
    std::string authority = origin.substr(sizeof(httpPrefix) - 1);
    if (authority.empty() || authority.find_first_of("/?#") != std::string::npos) {
        return false;
    }
    auto lower = [](std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    };
    authority = lower(std::move(authority));
    host = lower(std::move(host));
    if (authority != host) {
        return false;
    }

    const std::string localIp = WiFi.localIP().toString().c_str();
    const std::string mdns = mdnsHostname() + ".local";
    const std::string portSuffix = ":" + std::to_string(port);
    if (authority == localIp + portSuffix || authority == mdns + portSuffix) {
        return true;
    }
    return port == 80 && (authority == localIp || authority == mdns);
}

// ---------------------------------------------------------------------------
// Protocol layer
// ---------------------------------------------------------------------------

void MCPServer::RegisterTool(const Tool& tool) {
    std::lock_guard<std::mutex> lock(toolsMutex);
    tools[std::string(tool.name.c_str())] = tool;
    toolsListDirty = true;
}

void MCPServer::RegisterTool(Tool&& tool) {
    std::lock_guard<std::mutex> lock(toolsMutex);
    std::string key(tool.name.c_str());
    tools[key] = std::move(tool);
    toolsListDirty = true;
}

MCPRequest MCPServer::parseRequest(const std::string& json) {
    return parseRequest(json.c_str());
}

MCPRequest MCPServer::parseRequest(const char* json) {
    MCPRequest request;

    if (!json) {
        request.parseError = true;
        return request;
    }

    // Deserialize straight into the request's own document: the parse result is
    // what we keep, so there is nothing left to copy out of a scratch document.
    DeserializationError error = deserializeJson(request.doc, json);

    if (error) {
        request.doc.clear();  // a failed parse may leave a partial tree behind
        request.method = "";
        request.parseError = true;
        return request;
    }

    if (!request.doc.is<JsonObjectConst>()) {
        request.invalidRequest = true;
        return request;
    }

    JsonObjectConst root = request.doc.as<JsonObjectConst>();
    JsonVariantConst versionVar = root["jsonrpc"];
    JsonVariantConst methodVar = root["method"];
    JsonVariantConst idVar = root["id"];
    JsonVariantConst paramsVar = root["params"];

    request.hasIdField = !idVar.isUnbound();
    if (request.hasIdField &&
        !(idVar.isNull() || idVar.is<const char*>() || idVar.is<double>())) {
        request.invalidRequest = true;
        request.hasIdField = false;  // invalid ids must be answered as id:null
        return request;
    }
    request.idDoc.set(idVar);

    if (!versionVar.is<const char*>() || strcmp(versionVar.as<const char*>(), "2.0") != 0 ||
        !methodVar.is<const char*>()) {
        request.invalidRequest = true;
        return request;
    }

    if (!paramsVar.isUnbound() && !(paramsVar.is<JsonObjectConst>() || paramsVar.is<JsonArrayConst>())) {
        request.invalidRequest = true;
        return request;
    }

    request.method = methodVar.as<const char*>();
    request.paramsChecked = true;
    return request;
}

std::string MCPServer::serializeResponse(const MCPResponse& response) {
    if (!response.hasBody()) {
        return "";
    }

    /* Written out directly rather than assembled in a scratch JsonDocument:
     * copying result/error into a wrapper document deep-copied the entire
     * payload one more time, which on an 8 KiB tool result is the difference
     * between one and two full-size trees resident at once. */
    std::string out;
    StringAppender sink{&out};

    out += "{\"jsonrpc\":\"2.0\",\"id\":";
    serializeJson(response.idDoc, sink);  // a null document emits `null`, per spec

    if (!response.rawResult.empty()) {
        out += ",\"result\":";
        out += response.rawResult;
    } else if (!response.resultDoc.isNull()) {
        out += ",\"result\":";
        serializeJson(response.resultDoc, sink);
    }
    if (response.hasError()) {
        out += ",\"error\":";
        serializeJson(response.errorDoc, sink);
    }
    out += "}";

    return out;
}

MCPResponse MCPServer::handle(MCPRequest& request) {
    if (request.parseError) {
        return createJSONRPCError(400, static_cast<int>(ErrorCode::PARSE_ERROR), request.id(),
                                  "Parse error: Invalid JSON");
    }

    if (request.invalidRequest || request.method.empty()) {
        return createJSONRPCError(400, static_cast<int>(ErrorCode::INVALID_REQUEST), request.id(),
                                  "Invalid Request");
    }

    if (request.isNotification()) {
        // JSON-RPC 2.0: a notification never receives a response, whether the
        // method is known or not. notifications/initialized carries no state we
        // need to track, so acknowledging at the transport level is all that is
        // required — HTTP maps this to 202 Accepted with no body.
        return MCPResponse(202, false);
    }

    if (request.method == "initialize") {
        return handleInitialize(request);
    } else if (request.method == "ping") {
        return handlePing(request);
    } else if (request.method == "tools/list") {
        return handleToolsList(request);
    } else if (request.method == "tools/call") {
        return handleFunctionCalls(request);
    } else if (request.method == "notifications/initialized") {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_REQUEST), request.id(),
                                  "notifications/initialized must be sent as a notification");
    } else {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::METHOD_NOT_FOUND), request.id(),
                                  "Method not found: " + request.method);
    }
}

MCPResponse MCPServer::handleInitialize(MCPRequest& request) {
    JsonVariantConst params = request.params();
    if (!params.is<JsonObjectConst>() || !params["protocolVersion"].is<const char*>() ||
        !params["capabilities"].is<JsonObjectConst>() || !params["clientInfo"].is<JsonObjectConst>() ||
        !params["clientInfo"]["name"].is<const char*>() ||
        !params["clientInfo"]["version"].is<const char*>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "Invalid initialize parameters");
    }

    MCPResponse response(200, request.id());
    JsonObject result = response.resultDoc.to<JsonObject>();

    result["protocolVersion"] = negotiateProtocolVersion(params);

    JsonObject capabilities = result["capabilities"].to<JsonObject>();
    JsonObject toolsCap = capabilities["tools"].to<JsonObject>();
    toolsCap["listChanged"] = false;

    JsonObject serverInfo = result["serverInfo"].to<JsonObject>();
    serverInfo["name"] = serverName;
    serverInfo["version"] = serverVersion;

    if (serverInstructions.length() > 0) {
        result["instructions"] = serverInstructions;
    }
    return response;
}

MCPResponse MCPServer::handlePing(MCPRequest& request) {
    MCPResponse response(200, request.id());
    // The spec requires a prompt empty-result response; clients use ping to
    // probe liveness and may drop the connection on a -32601.
    response.resultDoc.to<JsonObject>();
    return response;
}

/* INVARIANT: callers must hold toolsMutex. The returned reference points into
 * the cache, which a concurrent RegisterTool would rewrite. */
const std::string& MCPServer::toolsListJson() {
    if (!toolsListDirty) {
        return toolsListCache;
    }

    JsonDocument doc;
    JsonObject result = doc.to<JsonObject>();
    JsonArray toolsArray = result["tools"].to<JsonArray>();
    for (const auto& [key, value] : tools) {
        JsonObject tool = toolsArray.add<JsonObject>();
        tool["name"] = key;
        tool["description"] = value.description;
        tool["inputSchema"].set(value.inputSchema);

        if (!value.outputSchema.isNull()) {
            tool["outputSchema"].set(value.outputSchema);
        }
    }

    serializeJson(doc, toolsListCache);  // the std::string writer clears for us
    toolsListDirty = false;
    return toolsListCache;
}

MCPResponse MCPServer::handleToolsList(MCPRequest& request) {
    if (request.hasParams() && !request.params().is<JsonObjectConst>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "tools/list params must be an object");
    }

    MCPResponse response(200, request.id());
    // Walking the schema tree once per registration rather than once per request.
    // The copy happens under the lock; the cache reference must not escape it.
    std::lock_guard<std::mutex> lock(toolsMutex);
    response.rawResult = toolsListJson();
    return response;
}

MCPResponse MCPServer::handleFunctionCalls(MCPRequest& request) {
    MCPResponse mcpResponse(200, request.id());
    JsonVariantConst params = request.params();

    if (!params.is<JsonObjectConst>() || !params["name"].is<const char*>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "Missing or invalid 'name' parameter");
    }

    const char* functionName = params["name"].as<const char*>();
    JsonVariantConst arguments = params["arguments"];
    if (!arguments.isUnbound() && !arguments.is<JsonObjectConst>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "'arguments' must be an object");
    }

    // Resolve the handler under the lock, then release it before running user
    // code of unknown duration.
    std::shared_ptr<ToolHandler> handler;
    {
        std::lock_guard<std::mutex> lock(toolsMutex);
        // Short-string optimization keeps this key off the heap for normal names.
        auto toolIt = tools.find(std::string(functionName));
        if (toolIt == tools.end()) {
            /* Per MCP, an unknown tool is a -32602 invalid-params protocol error
             * ("Unknown tool: ..."), not -32601 — the method (tools/call) exists. */
            return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                      std::string("Unknown tool: ") + functionName);
        }
        if (!toolIt->second.handler) {
            return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR), request.id(),
                                      std::string("Tool handler not initialized: ") + functionName);
        }
        handler = toolIt->second.handler;
    }

    JsonDocument argsDoc;
    argsDoc.set(arguments);

    bool toolError = false;
    JsonDocument resultDoc;
    try {
        resultDoc = handler->call(std::move(argsDoc), toolError);
    } catch (const std::exception& e) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR), request.id(),
                                  std::string("Tool handler exception: ") + e.what());
    } catch (...) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR), request.id(),
                                  "Tool handler threw unknown exception");
    }

    JsonObject result = mcpResponse.resultDoc.to<JsonObject>();
    JsonArray content = result["content"].to<JsonArray>();

    /* structuredContent is defined by MCP as a JSON object, and an error
     * payload would not conform to a declared outputSchema — attach it only for
     * successful object results. Non-object results still reach the client as
     * serialized text content. */
    const bool structured = !toolError && resultDoc.is<JsonObjectConst>();

#if defined(MCP_OMIT_TEXT_WHEN_STRUCTURED) && MCP_OMIT_TEXT_WHEN_STRUCTURED
    /* MCP only says a structured result SHOULD be mirrored as text, and
     * carrying both puts the payload on the wire (and in RAM) twice. Opt-in
     * because clients predating structuredContent read the text. */
    const bool emitText = !structured;
#else
    const bool emitText = true;
#endif
    if (emitText) {
        String resultText;
        serializeJson(resultDoc, resultText);

        JsonObject textContent = content.add<JsonObject>();
        textContent["type"] = "text";
        textContent["text"] = resultText;
    }
    if (structured) {
        result["structuredContent"].set(resultDoc.as<JsonVariantConst>());
    }
    result["isError"] = toolError;

    return mcpResponse;
}

bool MCPServer::isSupportedProtocolVersion(const char* version) const {
    if (!version) {
        return false;
    }
    return strcmp(version, PROTOCOL_VERSION) == 0 ||
           strcmp(version, PROTOCOL_VERSION_2025_06_18) == 0 ||
           strcmp(version, PROTOCOL_VERSION_2025_03_26) == 0;
}

const char* MCPServer::negotiateProtocolVersion(JsonVariantConst params) const {
    const char* requested = params["protocolVersion"].as<const char*>();
    if (isSupportedProtocolVersion(requested)) {
        return requested;
    }
    return PROTOCOL_VERSION;
}

MCPResponse MCPServer::createJSONRPCError(int httpCode, int rpcCode, const JsonVariantConst& id,
                                          const std::string& message) {
    MCPResponse response(httpCode, id);

    response.errorDoc["code"] = rpcCode;
    response.errorDoc["message"] = message;

    return response;
}
