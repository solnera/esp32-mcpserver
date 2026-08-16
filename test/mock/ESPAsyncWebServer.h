#pragma once

/* Minimal ESPAsyncWebServer mock for native tests.
 *
 * Faithful where it matters to HttpMCPServer:
 *  - _tempObject is released with free() in the request destructor, exactly
 *    like the real library — so an aborted upload exercises the same teardown
 *    path (a C++ object stored there would leak / corrupt the heap).
 *  - Header lookup is case-insensitive.
 *  - The MCP endpoint is registered as an AsyncWebHandler that matches on path
 *    alone (the real library would refuse a server->on() route for any request
 *    whose Accept carries text/event-stream). findRoute() probes the registered
 *    handlers with a synthetic request the same way AsyncWebServer::_attachHandler
 *    does, so tests still drive body/request callbacks per (uri, method) via
 *    mock_async_web::lastServer().
 *  - Chunked/deferred responses mirror AsyncChunkedResponse: send() keeps the
 *    response object alive and the filler is pulled via pumpChunked() (the
 *    test-side stand-in for the ack/poll cycle) until it returns 0; a filler
 *    returning RESPONSE_TRY_AGAIN sends nothing that round. The response
 *    object — and the filler lambda with its captures — is destroyed on
 *    completion (like _onAck) or with the request (like a disconnect).
 */

#include "WString.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

enum WebRequestMethod : int {
    HTTP_GET = 0x01,
    HTTP_POST = 0x02,
    HTTP_DELETE = 0x04,
    HTTP_PUT = 0x08,
    HTTP_PATCH = 0x10,
    HTTP_HEAD = 0x20,
    HTTP_OPTIONS = 0x40,
    HTTP_ANY = 0x7F,
};

// Same value as the real library; a filler returns it to mean "no data yet,
// ask again on the next ack/poll cycle".
#define RESPONSE_TRY_AGAIN 0xFFFFFFFF

using AwsResponseFiller = std::function<size_t(uint8_t*, size_t, size_t)>;

class AsyncWebServerRequest;

class AsyncWebHeader {
public:
    explicit AsyncWebHeader(const char* value) : value_(value) {}
    const String& value() const { return value_; }

private:
    String value_;
};

class AsyncWebServerResponse {
public:
    AsyncWebServerResponse(int code, const String& contentType, const String& body)
        : code(code), contentType(contentType.c_str()), body(body.c_str()) {}

    // Chunked variant; the real AsyncChunkedResponse hard-codes code 200.
    AsyncWebServerResponse(const String& contentType, AwsResponseFiller filler)
        : code(200), contentType(contentType.c_str()), filler(std::move(filler)) {}

    void addHeader(const String& name, const String& value) {
        headers[name.c_str()] = value.c_str();
    }

    int code;
    std::string contentType;
    std::string body;
    std::map<std::string, std::string> headers;
    AwsResponseFiller filler;  // non-empty => chunked/deferred
};

class AsyncWebServerRequest {
public:
    AsyncWebServerRequest() = default;
    AsyncWebServerRequest(const AsyncWebServerRequest&) = delete;
    AsyncWebServerRequest& operator=(const AsyncWebServerRequest&) = delete;

    ~AsyncWebServerRequest() {
        /* Mirrors ~AsyncWebServerRequest in the real library: _tempObject is
         * released with free(), destructors are NOT run, and a still-pending
         * response is deleted with the request (the disconnect path). */
        if (_tempObject) {
            free(_tempObject);
            _tempObject = nullptr;
        }
        delete _pendingResponse;
        _pendingResponse = nullptr;
    }

    /* ---- API consumed by HttpMCPServer ---- */

    bool hasHeader(const String& name) const {
        return headers_.count(lower(name.c_str())) > 0;
    }

    AsyncWebHeader* getHeader(const String& name) {
        auto it = headers_.find(lower(name.c_str()));
        return it == headers_.end() ? nullptr : &it->second;
    }

    size_t contentLength() const { return contentLength_; }

    const String& contentType() const { return contentType_; }

    uint8_t version() const { return version_; }

    const String& url() const { return url_; }

    int method() const { return method_; }

    /* The real library downgrades any request whose Accept carries
     * text/event-stream to RCT_EVENT, and AsyncCallbackWebHandler::canHandle()
     * then declines it. MCP clients send exactly that header on every POST, so
     * routing has to be exercised with it modelled. */
    bool isHTTP() const { return !eventStreamAccept_; }

    AsyncWebServerResponse* beginResponse(int code, const String& contentType, const String& body) {
        return new AsyncWebServerResponse(code, contentType, body);
    }

    AsyncWebServerResponse* beginChunkedResponse(const String& contentType, AwsResponseFiller callback) {
        return new AsyncWebServerResponse(contentType, std::move(callback));
    }

    void send(int code) { record(code, "", "", {}); }

    void send(int code, const String& contentType, const String& body) {
        record(code, contentType.c_str(), body.c_str(), {});
    }

    void send(AsyncWebServerResponse* response) {
        if (response->filler) {
            /* Deferred: content arrives through the filler; tests pull it with
             * pumpChunked(). Nothing is recorded until the terminating chunk. */
            delete _pendingResponse;
            _pendingResponse = response;
            return;
        }
        record(response->code, response->contentType, response->body, response->headers);
        delete response;
    }

    /* One ack/poll cycle of AsyncChunkedResponse::_fillBuffer: invoke the
     * filler once; TRY_AGAIN sends nothing, 0 is the terminating chunk (the
     * response completes, is recorded, and is destroyed like in _onAck).
     * Returns true once the response has completed. */
    bool pumpChunked(size_t maxLen = 512) {
        if (!_pendingResponse) {
            return responseCount > 0;
        }
        uint8_t buffer[512];
        if (maxLen > sizeof(buffer)) {
            maxLen = sizeof(buffer);
        }
        size_t got = _pendingResponse->filler(buffer, maxLen, _chunkIndex);
        if (got == RESPONSE_TRY_AGAIN) {
            return false;
        }
        if (got == 0) {
            record(_pendingResponse->code, _pendingResponse->contentType, _chunkBody, _pendingResponse->headers);
            delete _pendingResponse;
            _pendingResponse = nullptr;
            return true;
        }
        _chunkBody.append(reinterpret_cast<char*>(buffer), got);
        _chunkIndex += got;
        return false;
    }

    bool hasPendingResponse() const { return _pendingResponse != nullptr; }

    void* _tempObject = nullptr;

    /* ---- test-side setup & inspection ---- */

    void setHeader(const char* name, const char* value) {
        headers_.emplace(lower(name), AsyncWebHeader(value));
        if (lower(name) == "accept" && lower(value).find("text/event-stream") != std::string::npos) {
            eventStreamAccept_ = true;
        }
    }

    void setContentLength(size_t length) { contentLength_ = length; }

    void setContentType(const char* type) { contentType_ = type; }

    void setVersion(uint8_t version) { version_ = version; }

    void setUrl(const char* url) { url_ = url; }

    void setMethod(int method) { method_ = method; }

    int responseCount = 0;
    int lastCode = -1;
    std::string lastContentType;
    std::string lastBody;
    std::map<std::string, std::string> lastHeaders;

private:
    static std::string lower(const char* s) {
        std::string out(s ? s : "");
        for (char& c : out) {
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    void record(int code, std::string contentType, std::string body, std::map<std::string, std::string> headers) {
        responseCount++;
        lastCode = code;
        lastContentType = std::move(contentType);
        lastBody = std::move(body);
        lastHeaders = std::move(headers);
    }

    std::map<std::string, AsyncWebHeader> headers_;
    size_t contentLength_ = 0;
    uint8_t version_ = 1;
    String url_ = String("/mcp");
    int method_ = HTTP_POST;
    bool eventStreamAccept_ = false;
    String contentType_ = String("application/json");
    AsyncWebServerResponse* _pendingResponse = nullptr;
    std::string _chunkBody;
    size_t _chunkIndex = 0;
};

/* Mirrors the subset of the real base class that MCPServer overrides. The
 * default canHandle() declines, matching the library. */
class AsyncWebHandler {
public:
    virtual ~AsyncWebHandler() = default;

    virtual bool canHandle(AsyncWebServerRequest* request) const {
        (void)request;
        return false;
    }
    virtual void handleRequest(AsyncWebServerRequest* request) { (void)request; }
    virtual void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index,
                            size_t total) {
        (void)request;
        (void)data;
        (void)len;
        (void)index;
        (void)total;
    }
    // The real server skips body delivery entirely for a trivial handler.
    virtual bool isRequestHandlerTrivial() const { return true; }
};

using ArRequestHandlerFunction = std::function<void(AsyncWebServerRequest*)>;
using ArUploadHandlerFunction =
    std::function<void(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool)>;
using ArBodyHandlerFunction = std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)>;

class AsyncWebServer;

namespace mock_async_web {
/* The AsyncWebServer most recently constructed — HttpMCPServer allocates its
 * server internally, so tests reach it through this hook. */
inline AsyncWebServer*& lastServer() {
    static AsyncWebServer* server = nullptr;
    return server;
}
}  // namespace mock_async_web

class AsyncWebServer {
public:
    struct Route {
        std::string uri;
        int method;
        ArRequestHandlerFunction onRequest;
        ArBodyHandlerFunction onBody;
    };

    explicit AsyncWebServer(uint16_t port) : port(port) {
        mock_async_web::lastServer() = this;
    }

    ~AsyncWebServer() {
        if (mock_async_web::lastServer() == this) {
            mock_async_web::lastServer() = nullptr;
        }
    }

    void on(const char* uri, int method, ArRequestHandlerFunction onRequest) {
        routes.push_back({uri, method, std::move(onRequest), nullptr});
    }

    void on(const char* uri, int method, ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload,
            ArBodyHandlerFunction onBody) {
        (void)onUpload;
        routes.push_back({uri, method, std::move(onRequest), std::move(onBody)});
    }

    // Takes ownership, like the real AsyncWebServer (a unique_ptr list).
    AsyncWebHandler& addHandler(AsyncWebHandler* handler) {
        handlers.emplace_back(handler);
        return *handlers.back();
    }

    void onNotFound(ArRequestHandlerFunction handler) { notFound = std::move(handler); }

    void begin() { started = true; }

    /* Resolves a request the way AsyncWebServer::_attachHandler does: explicit
     * on() routes first — which, like AsyncCallbackWebHandler::canHandle(),
     * decline anything that is not plain HTTP — then the registered handlers,
     * which decide for themselves. A matched handler is surfaced as a
     * forwarding Route so tests see one shape either way; a nullptr return is
     * the catch-all (onNotFound) taking over. */
    const Route* findRoute(AsyncWebServerRequest* request) const {
        for (const auto& route : routes) {
            if (request->isHTTP() && route.uri == request->url().c_str() &&
                (route.method & request->method()) != 0) {
                return &route;
            }
        }

        for (const auto& handler : handlers) {
            if (!handler->canHandle(request)) {
                continue;
            }
            AsyncWebHandler* target = handler.get();
            // std::list: the returned pointer must survive later lookups.
            handlerRoutes_.push_back(
                Route{request->url().c_str(), request->method(),
                      [target](AsyncWebServerRequest* r) { target->handleRequest(r); },
                      target->isRequestHandlerTrivial()
                          ? ArBodyHandlerFunction()
                          : [target](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index,
                                     size_t total) { target->handleBody(r, data, len, index, total); }});
            return &handlerRoutes_.back();
        }
        return nullptr;
    }

    // Convenience for tests that only care about (uri, method) routing.
    const Route* findRoute(const char* uri, int method) const {
        AsyncWebServerRequest probe;
        probe.setUrl(uri);
        probe.setMethod(method);
        return findRoute(&probe);
    }

    uint16_t port;
    bool started = false;
    std::vector<Route> routes;
    std::vector<std::unique_ptr<AsyncWebHandler>> handlers;
    ArRequestHandlerFunction notFound;

private:
    mutable std::list<Route> handlerRoutes_;
};
