#include <unity.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "MCPServer.h"

/* Drives the handlers MCPServer registers on the mock AsyncWebServer the
 * way the real library does: body callback per chunk, then the onRequest
 * callback once the request is complete. tools/call answers arrive through a
 * deferred chunked response; tests pull them with pumpChunked()/
 * pumpUntilComplete(), the stand-in for the real ack/poll cycle. */

namespace {

class EchoHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        JsonDocument result;
        result["echo"] = params["text"];
        return result;
    }
};

/* Blocks on the worker task until the test opens the gate; lets tests hold the
 * worker busy at a known point. */
std::atomic<bool> g_gate_open{false};
std::atomic<bool> g_gate_entered{false};

class GateHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        (void)params;
        g_gate_entered.store(true);
        while (!g_gate_open.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        JsonDocument result;
        result["gated"] = true;
        return result;
    }
};

struct TestServer {
    TestServer() : mcp(3000, "test-http", "1.0.0") {
        Tool tool;
        tool.name = "echo";
        tool.description = "Echo";
        tool.inputSchema = Schema::object().build();
        tool.handler = std::make_shared<EchoHandler>();
        mcp.RegisterTool(tool);

        Tool gate;
        gate.name = "gate";
        gate.description = "Blocks until the test opens the gate";
        gate.inputSchema = Schema::object().build();
        gate.handler = std::make_shared<GateHandler>();
        mcp.RegisterTool(gate);

        TEST_ASSERT_TRUE(mcp.begin());
        web = mock_async_web::lastServer();
    }

    const AsyncWebServer::Route* postRoute() const { return web->findRoute("/mcp", HTTP_POST); }

    MCPServer mcp;
    AsyncWebServer* web = nullptr;
};

/* Feeds `body` through the POST route in `chunkSize`-byte pieces and finishes
 * the request. chunkSize 0 = one chunk. The route is resolved from the request
 * itself, so header-dependent routing (an SSE-flavoured Accept) is exercised
 * rather than assumed away. */
void drivePost(TestServer& srv, AsyncWebServerRequest& req, const std::string& body, size_t chunkSize = 0) {
    const AsyncWebServer::Route* route = srv.web->findRoute(&req);
    TEST_ASSERT_NOT_NULL(route);

    req.setContentLength(body.size());
    if (!body.empty()) {
        const size_t step = chunkSize == 0 ? body.size() : chunkSize;
        for (size_t index = 0; index < body.size(); index += step) {
            size_t len = body.size() - index < step ? body.size() - index : step;
            route->onBody(&req, reinterpret_cast<uint8_t*>(const_cast<char*>(body.data() + index)), len, index,
                          body.size());
        }
    }
    route->onRequest(&req);
}

bool pumpUntilComplete(AsyncWebServerRequest& req, int timeoutMs = 2000) {
    for (int waited = 0; waited <= timeoutMs; waited += 5) {
        if (req.pumpChunked()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return req.pumpChunked();
}

bool waitForFlag(std::atomic<bool>& flag, int timeoutMs = 2000) {
    for (int waited = 0; waited <= timeoutMs; waited += 1) {
        if (flag.load()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}

const char* kGateCall = R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"gate","arguments":{}}})";

}  // namespace

void setUp(void) {
    g_gate_open.store(false);
    g_gate_entered.store(false);
}
void tearDown(void) {}

void test_tools_list_roundtrip(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"tools\""));
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"echo\""));
    TEST_ASSERT_EQUAL_STRING("2025-11-25", req.lastHeaders["MCP-Protocol-Version"].c_str());
    TEST_ASSERT_NULL(req._tempObject);
}

void test_fast_tool_call_answers_inline(void) {
    /* A handler that returns promptly is answered within the fast-path window,
     * as an ordinary length-delimited response — no chunk framing and, more to
     * the point, no waiting for the ~500 ms connection poll that drives the
     * deferred filler. */
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req,
              R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})",
              7);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_FALSE(req.hasPendingResponse());
    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"hi\""));
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"id\":2"));
    TEST_ASSERT_EQUAL_STRING("application/json", req.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("2025-11-25", req.lastHeaders["MCP-Protocol-Version"].c_str());
}

void test_slow_tool_call_falls_back_to_chunked_body(void) {
    /* Past the fast-path window the reply must still go out deferred: nothing
     * inline, body delivered by the chunked filler once the handler returns. */
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, kGateCall);
    TEST_ASSERT_TRUE(waitForFlag(g_gate_entered));

    TEST_ASSERT_EQUAL_INT(0, req.responseCount);
    TEST_ASSERT_TRUE(req.hasPendingResponse());

    g_gate_open.store(true);
    TEST_ASSERT_TRUE(pumpUntilComplete(req));
    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"gated\":true"));
    TEST_ASSERT_EQUAL_STRING("application/json", req.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("2025-11-25", req.lastHeaders["MCP-Protocol-Version"].c_str());
}

void test_tool_call_notification_stays_inline_202(void) {
    /* A tools/call without an id is a notification: no execution, no body,
     * answered 202 inline — it must not occupy the worker queue. */
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"echo","arguments":{}}})");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(202, req.lastCode);
    TEST_ASSERT_FALSE(req.hasPendingResponse());
    TEST_ASSERT_EQUAL_STRING("", req.lastBody.c_str());
}

void test_http_1_0_tool_call_is_rejected_without_chunk_framing(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setVersion(0);
    drivePost(srv, req,
              R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"echo","arguments":{}}})");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(505, req.lastCode);
    TEST_ASSERT_FALSE(req.hasPendingResponse());
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"id\":10"));
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "HTTP/1.1 required"));
}

void test_tool_call_job_alloc_failure_returns_500_not_abort(void) {
    /* OOM on the per-call job allocation must be a graceful inline 500 in
     * both exception modes — never an abort/reboot. */
    TestServer srv;
    AsyncWebServerRequest req;
    mcp_http_test_fail_next_job_alloc(1);
    drivePost(srv, req,
              R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"echo","arguments":{}}})");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(500, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "-32603"));
    /* The request was parsed before the allocation failed, so the error must
     * echo its id — not null — for clients to correlate it. */
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"id\":11"));
    TEST_ASSERT_FALSE(req.hasPendingResponse());

    /* The failure is not sticky: the next call goes through. */
    AsyncWebServerRequest req2;
    drivePost(srv, req2,
              R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"echo","arguments":{"text":"ok"}}})");
    TEST_ASSERT_TRUE(pumpUntilComplete(req2));
    TEST_ASSERT_EQUAL_INT(200, req2.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req2.lastBody.c_str(), "\"ok\""));
}

void test_slow_tool_does_not_block_other_requests(void) {
    /* The core property of the worker hand-off: while a handler blocks, the
     * request path (async_tcp in production) keeps answering. */
    TestServer srv;
    AsyncWebServerRequest slowReq;
    drivePost(srv, slowReq, kGateCall);
    TEST_ASSERT_TRUE(waitForFlag(g_gate_entered));

    AsyncWebServerRequest listReq;
    drivePost(srv, listReq, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    TEST_ASSERT_EQUAL_INT(1, listReq.responseCount);
    TEST_ASSERT_EQUAL_INT(200, listReq.lastCode);
    TEST_ASSERT_FALSE(slowReq.pumpChunked());  // still TRY_AGAIN while gated

    g_gate_open.store(true);
    TEST_ASSERT_TRUE(pumpUntilComplete(slowReq));
    TEST_ASSERT_NOT_NULL(strstr(slowReq.lastBody.c_str(), "\"gated\":true"));
    TEST_ASSERT_NOT_NULL(strstr(slowReq.lastBody.c_str(), "\"id\":5"));
}

void test_tool_call_queue_full_gets_busy_error(void) {
    TestServer srv;
    AsyncWebServerRequest gateReq;
    drivePost(srv, gateReq, kGateCall);
    /* Once the worker is inside the gated handler the queue is empty again;
     * exactly MCP_HTTP_JOB_QUEUE_DEPTH more calls fit, the next must bounce. */
    TEST_ASSERT_TRUE(waitForFlag(g_gate_entered));

    AsyncWebServerRequest queued[MCP_HTTP_JOB_QUEUE_DEPTH];
    for (auto& req : queued) {
        drivePost(srv, req, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"echo","arguments":{}}})");
        TEST_ASSERT_EQUAL_INT(0, req.responseCount);
        TEST_ASSERT_TRUE(req.hasPendingResponse());
    }

    AsyncWebServerRequest overflow;
    drivePost(srv, overflow,
              R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"echo","arguments":{}}})");
    TEST_ASSERT_EQUAL_INT(1, overflow.responseCount);
    TEST_ASSERT_EQUAL_INT(200, overflow.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(overflow.lastBody.c_str(), "-32000"));
    TEST_ASSERT_NOT_NULL(strstr(overflow.lastBody.c_str(), "\"id\":9"));

    g_gate_open.store(true);
    TEST_ASSERT_TRUE(pumpUntilComplete(gateReq));
    for (auto& req : queued) {
        TEST_ASSERT_TRUE(pumpUntilComplete(req));
        TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    }
}

void test_client_abort_discards_result_without_crash(void) {
    /* The client disconnects while its tool is still executing. Execution is
     * deliberately NOT cancelled (tools have side effects); the result is
     * discarded when the worker drops the last job reference. ASan verifies
     * the lifetime handling. */
    TestServer srv;
    {
        AsyncWebServerRequest req;
        drivePost(srv, req, kGateCall);
        TEST_ASSERT_TRUE(waitForFlag(g_gate_entered));
        TEST_ASSERT_TRUE(req.hasPendingResponse());
    }  // request dies mid-call, like a TCP disconnect
    g_gate_open.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST_ASSERT_TRUE(true);  // reaching here without ASan errors is the assertion
}

void test_server_teardown_with_inflight_job(void) {
    /* Destroying the server while one job executes and another is queued must
     * join the worker cleanly: the executing job finishes, the queued one is
     * discarded undelivered. */
    AsyncWebServerRequest gateReq;
    AsyncWebServerRequest queuedReq;
    std::thread releaser;
    {
        TestServer srv;
        drivePost(srv, gateReq, kGateCall);
        TEST_ASSERT_TRUE(waitForFlag(g_gate_entered));
        drivePost(srv, queuedReq,
                  R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"echo","arguments":{}}})");
        TEST_ASSERT_EQUAL_INT(0, queuedReq.responseCount);

        releaser = std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            g_gate_open.store(true);
        });
    }  // ~MCPServer blocks here until the worker has exited
    releaser.join();

    /* The gated job completed before the worker exited, so its response is
     * still deliverable — the filler owns the job, not the server. The queued
     * job was discarded: its response stays pending forever. */
    TEST_ASSERT_TRUE(pumpUntilComplete(gateReq));
    TEST_ASSERT_NOT_NULL(strstr(gateReq.lastBody.c_str(), "\"gated\":true"));
    TEST_ASSERT_FALSE(queuedReq.pumpChunked());
    TEST_ASSERT_TRUE(queuedReq.hasPendingResponse());
}

void test_empty_body_post_gets_parse_error_response(void) {
    /* Previously the empty lambda in the onRequest slot meant a body-less POST
     * never got any response and the connection hung until timeout. */
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, "");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(400, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "-32700"));
}

void test_oversized_body_is_rejected_with_413(void) {
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    const size_t total = MCP_HTTP_MAX_BODY_SIZE + 1;
    req.setContentLength(total);

    /* Only a fraction of the declared body ever arrives; the limit must apply
     * to the declared Content-Length, before buffering the payload. */
    std::string chunk(64, 'X');
    route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), 0, total);
    route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), chunk.size(), total);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(413, req.lastCode);
    TEST_ASSERT_NULL(req._tempObject);
}

void test_chunked_upload_without_length_gets_411(void) {
    /* NOTE: this drives the total==0 body-callback shape as defense-in-depth.
     * ESPAsyncWebServer 1.2.x does not actually support chunked request bodies,
     * so on that library this branch is a fallback, not an observed behavior. */
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    req.setContentLength(0);
    std::string chunk = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), 0, 0);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(411, req.lastCode);
}

void test_body_overshooting_content_length_is_clamped_and_answered(void) {
    /* A client that sends more bytes than its declared Content-Length must
     * still get a response for the declared prefix — not hang. */
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    std::string body = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    req.setContentLength(body.size());
    route->onBody(&req, reinterpret_cast<uint8_t*>(body.data()), body.size(), 0, body.size());
    std::string extra = "GARBAGE";
    route->onBody(&req, reinterpret_cast<uint8_t*>(extra.data()), extra.size(), body.size(), body.size());
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"tools\""));
}

void test_undersized_body_gets_400_incomplete(void) {
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    req.setContentLength(100);
    std::string chunk(40, 'X');
    route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), 0, 100);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(400, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "Incomplete"));
}

void test_non_json_content_type_gets_415(void) {
    /* ESPAsyncWebServer consumes urlencoded/multipart bodies itself, so our
     * body callback never runs; the server must not misreport that as OOM. */
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    req.setContentLength(24);
    req.setContentType("application/x-www-form-urlencoded");
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(415, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "application/json"));
}

void test_non_json_content_type_with_body_gets_415(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setContentType("text/plain");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(415, req.lastCode);
    TEST_ASSERT_NULL(req._tempObject);
}

void test_server_does_not_listen_until_begin(void) {
    MCPServer mcp(3001, "not-started", "1.0.0");
    AsyncWebServer* web = mock_async_web::lastServer();
    TEST_ASSERT_NOT_NULL(web);
    TEST_ASSERT_FALSE(web->started);
    TEST_ASSERT_NULL(web->findRoute("/mcp", HTTP_POST));

    Tool tool;
    tool.name = "early";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<EchoHandler>();
    mcp.RegisterTool(tool);

    TEST_ASSERT_TRUE(mcp.begin());
    TEST_ASSERT_TRUE(web->started);
    TEST_ASSERT_NOT_NULL(web->findRoute("/mcp", HTTP_POST));
}

void test_aborted_upload_is_reclaimed_by_free(void) {
    /* The request dies mid-upload: onRequest never runs and the destructor
     * releases _tempObject with free(), as the real library does. The buffer
     * must be a plain malloc block for that to be leak- and corruption-free
     * (a String stored there used to leak its heap buffer). */
    TestServer srv;
    {
        AsyncWebServerRequest req;
        const AsyncWebServer::Route* route = srv.postRoute();
        req.setContentLength(4096);
        std::string chunk(512, 'A');
        route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), 0, 4096);
        TEST_ASSERT_NOT_NULL(req._tempObject);
    }
    TEST_ASSERT_TRUE(true);  // reaching here without ASAN/heap errors is the assertion
}

void test_protocol_version_2025_06_18_header_is_accepted(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("MCP-Protocol-Version", "2025-06-18");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"tools\""));
}

void test_unsupported_protocol_version_header_is_rejected(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("MCP-Protocol-Version", "1999-01-01");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(400, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "-32602"));
}

void test_legacy_http_sse_protocol_version_header_is_rejected(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("MCP-Protocol-Version", "2024-11-05");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(400, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "-32602"));
}

void test_mismatched_origin_is_rejected(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("Origin", "http://evil.example");
    req.setHeader("Host", "192.168.4.1:3000");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(403, req.lastCode);
}

void test_rebound_origin_with_matching_attacker_host_is_rejected(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("Origin", "http://evil.example:3000");
    req.setHeader("Host", "evil.example:3000");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(403, req.lastCode);
}

void test_matching_origin_is_accepted(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("Origin", "http://192.168.4.1:3000");
    req.setHeader("Host", "192.168.4.1:3000");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
}

void test_advertised_mdns_origin_is_accepted_case_insensitively(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("Origin", "http://TEST-HTTP.local:3000");
    req.setHeader("Host", "test-http.LOCAL:3000");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
}

void test_notification_returns_202_without_body(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(202, req.lastCode);
    TEST_ASSERT_EQUAL_STRING("", req.lastBody.c_str());
}

void test_ping_over_http(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":7,"method":"ping"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"result\":{}"));
}

void test_streamable_http_accept_header_still_routes_to_endpoint(void) {
    /* MCP's Streamable HTTP transport requires this Accept on every client
     * POST. ESPAsyncWebServer reads the text/event-stream part as an SSE
     * subscription and refuses the request from any server->on() route, which
     * sent every conforming client to the catch-all 404 while a bare curl
     * worked. The endpoint therefore matches on path alone. */
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("Accept", "application/json, text/event-stream");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"ping"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"result\":{}"));
}

void test_streamable_http_accept_header_get_stream_probe_gets_405(void) {
    // The client's server-initiated-stream probe. This server offers no stream;
    // MCP prescribes 405 for that, and it must not fall through to the 404.
    TestServer srv;
    AsyncWebServerRequest req;
    req.setMethod(HTTP_GET);
    req.setHeader("Accept", "text/event-stream");

    const AsyncWebServer::Route* route = srv.web->findRoute(&req);
    TEST_ASSERT_NOT_NULL(route);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(405, req.lastCode);
    TEST_ASSERT_EQUAL_STRING("POST", req.lastHeaders["Allow"].c_str());
}

void test_get_mcp_returns_405(void) {
    TestServer srv;
    const AsyncWebServer::Route* route = srv.web->findRoute("/mcp", HTTP_GET);
    TEST_ASSERT_NOT_NULL(route);

    AsyncWebServerRequest req;
    req.setMethod(HTTP_GET);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(405, req.lastCode);
}

void test_get_mcp_rejects_invalid_origin_before_method_check(void) {
    TestServer srv;
    const AsyncWebServer::Route* route = srv.web->findRoute("/mcp", HTTP_GET);
    TEST_ASSERT_NOT_NULL(route);

    AsyncWebServerRequest req;
    req.setMethod(HTTP_GET);
    req.setHeader("Origin", "http://evil.example:3000");
    req.setHeader("Host", "evil.example:3000");
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(403, req.lastCode);
}

void test_delete_mcp_returns_405_with_allow(void) {
    /* Previously DELETE answered 200 with an empty JSON-RPC result, which told
     * a client the session had been torn down when nothing had happened. */
    TestServer srv;
    const AsyncWebServer::Route* route = srv.web->findRoute("/mcp", HTTP_DELETE);
    TEST_ASSERT_NOT_NULL(route);

    AsyncWebServerRequest req;
    req.setMethod(HTTP_DELETE);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(405, req.lastCode);
    TEST_ASSERT_EQUAL_STRING("POST", req.lastHeaders["Allow"].c_str());
}

void test_stateless_server_does_not_echo_client_session_id(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("mcp-session-id", "client-chosen-session");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_EQUAL_UINT(0, req.lastHeaders.count("mcp-session-id"));
}

void test_stateless_server_does_not_mint_session_id(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_UINT(0, req.lastHeaders.count("mcp-session-id"));
}

void test_deferred_tool_call_does_not_emit_session_header(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("mcp-session-id", "deferred-session");
    drivePost(srv, req, kGateCall);
    TEST_ASSERT_TRUE(waitForFlag(g_gate_entered));

    g_gate_open.store(true);
    TEST_ASSERT_TRUE(pumpUntilComplete(req));
    TEST_ASSERT_EQUAL_UINT(0, req.lastHeaders.count("mcp-session-id"));
}

void test_unknown_path_returns_404(void) {
    TestServer srv;
    TEST_ASSERT_TRUE(static_cast<bool>(srv.web->notFound));

    AsyncWebServerRequest req;
    srv.web->notFound(&req);

    TEST_ASSERT_EQUAL_INT(404, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "-32600"));
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"id\":null"));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_tools_list_roundtrip);
    RUN_TEST(test_fast_tool_call_answers_inline);
    RUN_TEST(test_slow_tool_call_falls_back_to_chunked_body);
    RUN_TEST(test_tool_call_notification_stays_inline_202);
    RUN_TEST(test_http_1_0_tool_call_is_rejected_without_chunk_framing);
    RUN_TEST(test_tool_call_job_alloc_failure_returns_500_not_abort);
    RUN_TEST(test_slow_tool_does_not_block_other_requests);
    RUN_TEST(test_tool_call_queue_full_gets_busy_error);
    RUN_TEST(test_client_abort_discards_result_without_crash);
    RUN_TEST(test_server_teardown_with_inflight_job);
    RUN_TEST(test_empty_body_post_gets_parse_error_response);
    RUN_TEST(test_oversized_body_is_rejected_with_413);
    RUN_TEST(test_chunked_upload_without_length_gets_411);
    RUN_TEST(test_body_overshooting_content_length_is_clamped_and_answered);
    RUN_TEST(test_undersized_body_gets_400_incomplete);
    RUN_TEST(test_non_json_content_type_gets_415);
    RUN_TEST(test_non_json_content_type_with_body_gets_415);
    RUN_TEST(test_server_does_not_listen_until_begin);
    RUN_TEST(test_aborted_upload_is_reclaimed_by_free);
    RUN_TEST(test_protocol_version_2025_06_18_header_is_accepted);
    RUN_TEST(test_unsupported_protocol_version_header_is_rejected);
    RUN_TEST(test_legacy_http_sse_protocol_version_header_is_rejected);
    RUN_TEST(test_mismatched_origin_is_rejected);
    RUN_TEST(test_rebound_origin_with_matching_attacker_host_is_rejected);
    RUN_TEST(test_matching_origin_is_accepted);
    RUN_TEST(test_advertised_mdns_origin_is_accepted_case_insensitively);
    RUN_TEST(test_notification_returns_202_without_body);
    RUN_TEST(test_ping_over_http);
    RUN_TEST(test_streamable_http_accept_header_still_routes_to_endpoint);
    RUN_TEST(test_streamable_http_accept_header_get_stream_probe_gets_405);
    RUN_TEST(test_get_mcp_returns_405);
    RUN_TEST(test_get_mcp_rejects_invalid_origin_before_method_check);
    RUN_TEST(test_delete_mcp_returns_405_with_allow);
    RUN_TEST(test_stateless_server_does_not_echo_client_session_id);
    RUN_TEST(test_stateless_server_does_not_mint_session_id);
    RUN_TEST(test_deferred_tool_call_does_not_emit_session_header);
    RUN_TEST(test_unknown_path_returns_404);
    return UNITY_END();
}
