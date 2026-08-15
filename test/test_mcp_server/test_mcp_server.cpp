#include <unity.h>
#include <ArduinoJson.h>
#include <cstring>
#include "MCPServer.h"

/* ======== Test Helpers ======== */

class TestMCPServer : public MCPServer {
public:
    /* The transport needs a port; the protocol tests never call begin(), so
     * nothing is ever bound and 0 is only a placeholder for the mock server. */
    TestMCPServer(const String& name = DEFAULT_SERVER_NAME,
                  const String& version = DEFAULT_SERVER_VERSION,
                  const String& instructions = "")
        : MCPServer(0, name, version, instructions) {}

    using MCPServer::parseRequest;
    using MCPServer::serializeResponse;
    using MCPServer::handle;
    using MCPServer::createJSONRPCError;
    using MCPServer::tools;
    using MCPServer::toolsListJson;
};

/* tools/list answers from a pre-serialized cache rather than a JsonDocument, so
 * its body has to be read back off the wire. Asserting on the serialized reply
 * is what the client actually sees anyway. */
static void parseResponseBody(TestMCPServer* server, const MCPResponse& res, JsonDocument& out) {
    std::string json = server->serializeResponse(res);
    DeserializationError err = deserializeJson(out, json);
    TEST_ASSERT_FALSE(err);
}

/* Reads a tool's payload out of a result whichever way it was carried, so tests
 * about the payload itself stay valid under MCP_OMIT_TEXT_WHEN_STRUCTURED.
 * Tests specifically about the text/structured split assert on the raw result
 * instead — see test_structured_result_text_mirroring_follows_build_flag. */
static void readToolPayload(JsonVariantConst result, JsonDocument& out) {
    JsonVariantConst structured = result["structuredContent"];
    if (!structured.isNull()) {
        out.set(structured);
        return;
    }
    const char* text = result["content"][0]["text"].as<const char*>();
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_FALSE(deserializeJson(out, text));
}

class EchoHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        JsonDocument result;
        result["echo"] = params["message"];
        return result;
    }
};

class ComplexHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        JsonDocument result;
        result["status"] = "ok";
        JsonArray items = result["items"].to<JsonArray>();
        items.add(1);
        items.add(2);
        items.add(3);
        JsonObject nested = result["nested"].to<JsonObject>();
        nested["key"] = "value";
        return result;
    }
};

class EmptyHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        (void)params;
        JsonDocument result;
        return result;
    }
};

class FailingHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        bool ignored;
        return call(std::move(params), ignored);
    }
    JsonDocument call(JsonDocument params, bool& isError) override {
        (void)params;
        isError = true;
        JsonDocument result;
        result["error"] = "sensor offline";
        return result;
    }
};

class ScalarHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        (void)params;
        JsonDocument result;
        result.set(42);
        return result;
    }
};

class ParamInspectHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        JsonDocument result;
        /* Echo back the number of top-level keys */
        int count = 0;
        for (JsonPairConst kv : params.as<JsonObjectConst>()) {
            (void)kv;
            count++;
        }
        result["param_count"] = count;
        return result;
    }
};

static TestMCPServer* server;

void setUp(void) {
    server = new TestMCPServer("TestServer", "1.0.0", "Test instructions");
}

void tearDown(void) {
    delete server;
    server = nullptr;
}

/* ======== parseRequest ======== */

void test_parse_valid_request(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0.0"}}})");

    TEST_ASSERT_EQUAL_STRING("initialize", req.method.c_str());
    TEST_ASSERT_EQUAL(1, req.id().as<int>());
    TEST_ASSERT_TRUE(req.hasParams());
}

void test_parse_string_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":"abc-123","method":"tools/list"})");

    TEST_ASSERT_EQUAL_STRING("tools/list", req.method.c_str());
    TEST_ASSERT_EQUAL_STRING("abc-123", req.id().as<const char*>());
}

void test_parse_fractional_numeric_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1.5,"method":"tools/list"})");

    TEST_ASSERT_FALSE(req.invalidRequest);
    TEST_ASSERT_EQUAL_INT(15, static_cast<int>(req.id().as<double>() * 10));
}

void test_parse_invalid_json(void) {
    MCPRequest req = server->parseRequest("{invalid}");
    TEST_ASSERT_EQUAL_STRING("", req.method.c_str());
}

void test_parse_empty_string(void) {
    MCPRequest req = server->parseRequest("");
    TEST_ASSERT_EQUAL_STRING("", req.method.c_str());
}

void test_parse_no_params(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_STRING("tools/list", req.method.c_str());
    TEST_ASSERT_FALSE(req.hasParams());
}

void test_parse_with_arguments(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hello"}}})");

    TEST_ASSERT_EQUAL_STRING("tools/call", req.method.c_str());
    TEST_ASSERT_TRUE(req.hasParams());
    TEST_ASSERT_EQUAL_STRING("echo", req.params()["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("hello", req.params()["arguments"]["message"].as<const char*>());
}

void test_invalid_jsonrpc_envelope_is_rejected(void) {
    const char* invalidRequests[] = {
        R"({"id":1,"method":"ping"})",
        R"({"jsonrpc":"1.0","id":1,"method":"ping"})",
        R"({"jsonrpc":"2.0","id":{},"method":"ping"})",
        R"({"jsonrpc":"2.0","id":1,"method":"ping","params":"bad"})",
        R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])",
    };

    for (const char* json : invalidRequests) {
        MCPRequest req = server->parseRequest(json);
        MCPResponse res = server->handle(req);
        TEST_ASSERT_TRUE(req.invalidRequest);
        TEST_ASSERT_TRUE(res.hasError());
        TEST_ASSERT_EQUAL(-32600, res.error()["code"].as<int>());
    }
}

/* ======== handle: initialize ======== */

void test_handle_initialize(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0.0"}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());

    JsonVariantConst r = res.result();
    TEST_ASSERT_EQUAL_STRING("2025-11-25", r["protocolVersion"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("TestServer", r["serverInfo"]["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("1.0.0", r["serverInfo"]["version"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Test instructions", r["instructions"].as<const char*>());
    TEST_ASSERT_FALSE(r["capabilities"]["tools"]["listChanged"].as<bool>());
}

void test_handle_initialize_does_not_negotiate_legacy_http_sse_version(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0.0"}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_EQUAL_STRING("2025-11-25", res.result()["protocolVersion"].as<const char*>());
}

void test_handle_initialize_falls_back_to_latest_for_unknown_version(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2099-01-01","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0.0"}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_EQUAL_STRING("2025-11-25", res.result()["protocolVersion"].as<const char*>());
}

void test_handle_initialize_no_instructions(void) {
    TestMCPServer srv("Srv", "2.0.0", "");
    MCPRequest req = srv.parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0.0"}}})");
    MCPResponse res = srv.handle(req);

    TEST_ASSERT_TRUE(res.result()["instructions"].isNull());
}

void test_handle_initialize_requires_mandatory_params(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32602, res.error()["code"].as<int>());
    TEST_ASSERT_EQUAL(1, res.id().as<int>());
}

/* ======== handle: notifications/initialized ======== */

void test_handle_notifications_initialized(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(202, res.code);
    TEST_ASSERT_FALSE(res.hasBody());
    std::string json = server->serializeResponse(res);
    TEST_ASSERT_EQUAL_STRING("", json.c_str());
}

/* ======== handle: tools/list ======== */

void test_handle_tools_list_empty(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());

    JsonDocument body;
    parseResponseBody(server, res, body);
    TEST_ASSERT_TRUE(body["result"]["tools"].is<JsonArrayConst>());
    TEST_ASSERT_EQUAL(0, body["result"]["tools"].as<JsonArrayConst>().size());
}

void test_handle_tools_list_with_tool(void) {
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo tool";
    tool.inputSchema = Schema::object()
        .property("message", Schema::string().description("Message to echo"))
        .required({"message"})
        .build();
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    JsonDocument body;
    parseResponseBody(server, res, body);
    JsonArrayConst tools = body["result"]["tools"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(1, tools.size());
    TEST_ASSERT_EQUAL_STRING("echo", tools[0]["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Echo tool", tools[0]["description"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("object", tools[0]["inputSchema"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string",
        tools[0]["inputSchema"]["properties"]["message"]["type"].as<const char*>());
}

void test_handle_tools_list_multiple_tools(void) {
    Tool t1;
    t1.name = "tool_a";
    t1.description = "A";
    t1.inputSchema = Schema::object().build();
    t1.handler = std::make_shared<EchoHandler>();

    Tool t2;
    t2.name = "tool_b";
    t2.description = "B";
    t2.inputSchema = Schema::object().build();
    t2.handler = std::make_shared<EchoHandler>();

    server->RegisterTool(t1);
    server->RegisterTool(t2);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    JsonDocument body;
    parseResponseBody(server, res, body);
    TEST_ASSERT_EQUAL(2, body["result"]["tools"].as<JsonArrayConst>().size());
}

void test_handle_tools_list_with_output_schema(void) {
    Tool tool;
    tool.name = "typed";
    tool.description = "Typed tool";
    tool.inputSchema = Schema::object().build();
    tool.outputSchema = Schema::object()
        .property("result", Schema::string())
        .build();
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    JsonDocument body;
    parseResponseBody(server, res, body);
    JsonVariantConst t = body["result"]["tools"][0];
    TEST_ASSERT_EQUAL_STRING("object", t["outputSchema"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string",
        t["outputSchema"]["properties"]["result"]["type"].as<const char*>());
}

/* ======== tools/list caching ======== */

void test_tools_list_body_is_cached_between_calls(void) {
    Tool tool;
    tool.name = "cached";
    tool.description = "original";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    TEST_ASSERT_NOT_NULL(strstr(server->toolsListJson().c_str(), "original"));

    /* Mutate the stored tool behind RegisterTool's back, so nothing marks the
     * cache dirty. A rebuild-per-request implementation would pick the new
     * description up; the cache must not. */
    server->tools.find("cached")->second.description = "mutated";
    TEST_ASSERT_NOT_NULL(strstr(server->toolsListJson().c_str(), "original"));
    TEST_ASSERT_NULL(strstr(server->toolsListJson().c_str(), "mutated"));
}

void test_registering_a_tool_invalidates_the_tools_list_cache(void) {
    MCPRequest first = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse firstRes = server->handle(first);
    JsonDocument firstBody;
    parseResponseBody(server, firstRes, firstBody);
    TEST_ASSERT_EQUAL(0, firstBody["result"]["tools"].as<JsonArrayConst>().size());

    Tool tool;
    tool.name = "late";
    tool.description = "registered after the first list";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest second = server->parseRequest(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    MCPResponse secondRes = server->handle(second);
    JsonDocument secondBody;
    parseResponseBody(server, secondRes, secondBody);
    TEST_ASSERT_EQUAL(1, secondBody["result"]["tools"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("late", secondBody["result"]["tools"][0]["name"].as<const char*>());
}

void test_register_tool_move_overload_registers_and_invalidates(void) {
    Tool tool;
    tool.name = "moved";
    tool.description = "Moved in";
    tool.inputSchema = Schema::object()
        .property("field", Schema::string())
        .build();
    tool.handler = std::make_shared<EchoHandler>();

    server->RegisterTool(std::move(tool));

    TEST_ASSERT_EQUAL(1, server->tools.size());

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);
    JsonDocument body;
    parseResponseBody(server, res, body);

    JsonVariantConst listed = body["result"]["tools"][0];
    TEST_ASSERT_EQUAL_STRING("moved", listed["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Moved in", listed["description"].as<const char*>());
    /* The schema tree has to survive the move, not just the scalar fields. */
    TEST_ASSERT_EQUAL_STRING("string",
        listed["inputSchema"]["properties"]["field"]["type"].as<const char*>());
}

/* ======== handle: tools/call ======== */

void test_handle_tool_call_success(void) {
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hello"}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());

    JsonDocument payload;
    readToolPayload(res.result(), payload);
    TEST_ASSERT_EQUAL_STRING("hello", payload["echo"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("hello", res.result()["structuredContent"]["echo"].as<const char*>());
    TEST_ASSERT_FALSE(res.result()["isError"].as<bool>());
}

void test_structured_result_text_mirroring_follows_build_flag(void) {
    /* structuredContent is always attached for a successful object result.
     * Whether it is ALSO mirrored as serialized text depends on
     * MCP_OMIT_TEXT_WHEN_STRUCTURED; env:native and env:native-omit-text
     * explicitly build the numeric 0 and 1 sides. */
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hello"}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL_STRING("hello", res.result()["structuredContent"]["echo"].as<const char*>());

    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
#if defined(MCP_OMIT_TEXT_WHEN_STRUCTURED) && MCP_OMIT_TEXT_WHEN_STRUCTURED
    TEST_ASSERT_EQUAL(0, content.size());
#else
    TEST_ASSERT_EQUAL(1, content.size());
    JsonDocument textDoc;
    deserializeJson(textDoc, content[0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("hello", textDoc["echo"].as<const char*>());
#endif
}

void test_handle_tool_call_handler_reports_execution_error(void) {
    Tool tool;
    tool.name = "flaky";
    tool.description = "Fails at runtime";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<FailingHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"flaky","arguments":{}}})");
    MCPResponse res = server->handle(req);

    /* Per MCP, an execution failure is a RESULT with isError=true so the LLM
     * can see it — not a JSON-RPC protocol error. */
    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());
    TEST_ASSERT_TRUE(res.result()["isError"].as<bool>());

    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(1, content.size());
    TEST_ASSERT_EQUAL_STRING("text", content[0]["type"].as<const char*>());
    JsonDocument textDoc;
    deserializeJson(textDoc, content[0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("sensor offline", textDoc["error"].as<const char*>());

    /* Error payloads would not conform to a declared outputSchema. */
    TEST_ASSERT_TRUE(res.result()["structuredContent"].isNull());
}

void test_handle_tool_call_scalar_result_has_no_structured_content(void) {
    Tool tool;
    tool.name = "scalar";
    tool.description = "Returns a bare number";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<ScalarHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"scalar","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.result()["isError"].as<bool>());
    /* The value still reaches the client as serialized text content... */
    TEST_ASSERT_EQUAL_STRING("42", res.result()["content"][0]["text"].as<const char*>());
    /* ...but structuredContent is defined as a JSON object by MCP, so a
     * non-object result must not be attached. */
    TEST_ASSERT_TRUE(res.result()["structuredContent"].isNull());
}

void test_handle_tool_call_missing_name(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32602, res.error()["code"].as<int>());
}

void test_handle_tool_call_unknown_tool(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"nonexistent","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    /* Per MCP, an unknown tool is -32602 invalid params, not -32601: the
     * method (tools/call) itself exists. */
    TEST_ASSERT_EQUAL(-32602, res.error()["code"].as<int>());
    TEST_ASSERT_NOT_NULL(strstr(res.error()["message"].as<const char*>(), "Unknown tool: nonexistent"));
}

void test_handle_tool_call_null_handler(void) {
    Tool tool;
    tool.name = "broken";
    tool.description = "Broken";
    tool.inputSchema = Schema::object().build();
    tool.handler = nullptr;
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"broken","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32603, res.error()["code"].as<int>());
}

/* ======== handle: unknown method & parse error ======== */

void test_handle_unknown_method(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"unknown/method"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32601, res.error()["code"].as<int>());
}

void test_handle_parse_error(void) {
    MCPRequest req = server->parseRequest("{bad json}");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32700, res.error()["code"].as<int>());
}

/* ======== serializeResponse ======== */

void test_serialize_response_with_result(void) {
    MCPResponse res;
    res.idDoc.set(1);
    res.resultDoc["value"] = "test";

    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
    TEST_ASSERT_EQUAL(1, doc["id"].as<int>());
    TEST_ASSERT_EQUAL_STRING("test", doc["result"]["value"].as<const char*>());
    TEST_ASSERT_TRUE(doc["error"].isNull());
}

void test_serialize_response_with_error(void) {
    JsonDocument idDoc;
    idDoc.set(42);
    MCPResponse res = server->createJSONRPCError(
        400, -32700, idDoc.as<JsonVariantConst>(), "Parse error");

    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL(42, doc["id"].as<int>());
    TEST_ASSERT_EQUAL(-32700, doc["error"]["code"].as<int>());
    TEST_ASSERT_EQUAL_STRING("Parse error", doc["error"]["message"].as<const char*>());
}

void test_serialize_includes_jsonrpc_version(void) {
    MCPResponse res;
    res.idDoc.set(99);

    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
}

/* ======== RegisterTool ======== */

void test_register_tool(void) {
    Tool tool;
    tool.name = "test_tool";
    tool.description = "A test tool";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<EchoHandler>();

    server->RegisterTool(tool);

    TEST_ASSERT_EQUAL(1, server->tools.size());
    TEST_ASSERT_TRUE(server->tools.find("test_tool") != server->tools.end());
}

void test_register_overwrites_same_name(void) {
    Tool t1;
    t1.name = "tool";
    t1.description = "first";
    t1.inputSchema = Schema::object().build();
    t1.handler = std::make_shared<EchoHandler>();

    Tool t2;
    t2.name = "tool";
    t2.description = "second";
    t2.inputSchema = Schema::object().build();
    t2.handler = std::make_shared<EchoHandler>();

    server->RegisterTool(t1);
    server->RegisterTool(t2);

    TEST_ASSERT_EQUAL(1, server->tools.size());

    /* Verify it's the second registration */
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    JsonDocument body;
    parseResponseBody(server, res, body);
    TEST_ASSERT_EQUAL_STRING("second",
        body["result"]["tools"][0]["description"].as<const char*>());
}

/* ======== MCPResponse struct ======== */

void test_response_default_constructor(void) {
    MCPResponse res;
    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_FALSE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());
}

void test_response_id_constructor(void) {
    JsonDocument id;
    id.set(42);
    MCPResponse res(id.as<JsonVariantConst>());

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_EQUAL(42, res.id().as<int>());
}

void test_response_code_id_constructor(void) {
    JsonDocument id;
    id.set("req-1");
    MCPResponse res(404, id.as<JsonVariantConst>());

    TEST_ASSERT_EQUAL(404, res.code);
    TEST_ASSERT_EQUAL_STRING("req-1", res.id().as<const char*>());
}

/* ======== MCPRequest struct ======== */

void test_request_default_state(void) {
    MCPRequest req;
    TEST_ASSERT_EQUAL_STRING("", req.method.c_str());
    TEST_ASSERT_FALSE(req.hasParams());
    TEST_ASSERT_TRUE(req.id().isNull());
}

/* ======== createJSONRPCError ======== */

void test_create_error_all_codes(void) {
    JsonDocument id;
    id.set(1);

    struct { int code; const char* name; } codes[] = {
        {-32700, "PARSE_ERROR"},
        {-32600, "INVALID_REQUEST"},
        {-32601, "METHOD_NOT_FOUND"},
        {-32602, "INVALID_PARAMS"},
        {-32603, "INTERNAL_ERROR"},
        {-32000, "SERVER_ERROR"},
    };

    for (auto& c : codes) {
        MCPResponse res = server->createJSONRPCError(400, c.code, id.as<JsonVariantConst>(), c.name);
        TEST_ASSERT_TRUE(res.hasError());
        TEST_ASSERT_EQUAL(c.code, res.error()["code"].as<int>());
        TEST_ASSERT_EQUAL_STRING(c.name, res.error()["message"].as<const char*>());
        TEST_ASSERT_EQUAL(400, res.code);
    }
}

void test_create_error_preserves_string_id(void) {
    JsonDocument id;
    id.set("request-abc");

    MCPResponse res = server->createJSONRPCError(500, -32000, id.as<JsonVariantConst>(), "Server error");
    TEST_ASSERT_EQUAL_STRING("request-abc", res.id().as<const char*>());
}

/* ======== tools/call edge cases ======== */

void test_handle_tool_call_empty_arguments(void) {
    Tool tool;
    tool.name = "inspect";
    tool.description = "Inspect params";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<ParamInspectHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"inspect","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());

    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
    JsonDocument textDoc;
    deserializeJson(textDoc, content[0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL(0, textDoc["param_count"].as<int>());
}

void test_handle_tool_call_complex_result(void) {
    Tool tool;
    tool.name = "complex";
    tool.description = "Returns complex data";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<ComplexHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"complex","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);

    JsonDocument payload;
    readToolPayload(res.result(), payload);
    TEST_ASSERT_EQUAL_STRING("ok", payload["status"].as<const char*>());
    TEST_ASSERT_EQUAL(3, payload["items"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("value", payload["nested"]["key"].as<const char*>());
}

void test_handle_tool_call_empty_result(void) {
    Tool tool;
    tool.name = "empty";
    tool.description = "Returns nothing";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<EmptyHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"empty","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    /* Content should still have one text entry */
    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(1, content.size());
}

void test_handle_tool_call_multiple_different_tools(void) {
    Tool echo;
    echo.name = "echo";
    echo.description = "Echo";
    echo.inputSchema = Schema::object().build();
    echo.handler = std::make_shared<EchoHandler>();

    Tool complex;
    complex.name = "complex";
    complex.description = "Complex";
    complex.inputSchema = Schema::object().build();
    complex.handler = std::make_shared<ComplexHandler>();

    server->RegisterTool(echo);
    server->RegisterTool(complex);

    /* Call echo */
    MCPRequest req1 = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hi"}}})");
    MCPResponse res1 = server->handle(req1);
    TEST_ASSERT_TRUE(res1.hasResult());
    JsonDocument doc1;
    readToolPayload(res1.result(), doc1);
    TEST_ASSERT_EQUAL_STRING("hi", doc1["echo"].as<const char*>());

    /* Call complex */
    MCPRequest req2 = server->parseRequest(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"complex","arguments":{}}})");
    MCPResponse res2 = server->handle(req2);
    TEST_ASSERT_TRUE(res2.hasResult());
    JsonDocument doc2;
    readToolPayload(res2.result(), doc2);
    TEST_ASSERT_EQUAL_STRING("ok", doc2["status"].as<const char*>());
}

/* ======== Full roundtrip: parse → handle → serialize ======== */

void test_roundtrip_initialize(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0.0"}}})");
    MCPResponse res = server->handle(req);
    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
    TEST_ASSERT_EQUAL(1, doc["id"].as<int>());
    TEST_ASSERT_EQUAL_STRING("2025-11-25", doc["result"]["protocolVersion"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("TestServer", doc["result"]["serverInfo"]["name"].as<const char*>());
    TEST_ASSERT_TRUE(doc["error"].isNull());
}

void test_roundtrip_tool_call(void) {
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo";
    tool.inputSchema = Schema::object().build();
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":"abc","method":"tools/call","params":{"name":"echo","arguments":{"message":"world"}}})");
    MCPResponse res = server->handle(req);
    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("abc", doc["id"].as<const char*>());
    TEST_ASSERT_TRUE(doc["error"].isNull());

    /* Verify the payload survived the full parse → handle → serialize trip */
    JsonDocument payload;
    readToolPayload(doc["result"], payload);
    TEST_ASSERT_EQUAL_STRING("world", payload["echo"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("world", doc["result"]["structuredContent"]["echo"].as<const char*>());
}

void test_roundtrip_error(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":99,"method":"nonexistent"})");
    MCPResponse res = server->handle(req);
    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
    TEST_ASSERT_EQUAL(99, doc["id"].as<int>());
    TEST_ASSERT_EQUAL(-32601, doc["error"]["code"].as<int>());
    TEST_ASSERT_TRUE(doc["result"].isNull());
}

/* ======== Default server name/version ======== */

void test_server_default_name_version(void) {
    TestMCPServer defaultServer;
    MCPRequest req = defaultServer.parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0.0"}}})");
    MCPResponse res = defaultServer.handle(req);

    TEST_ASSERT_EQUAL_STRING("ESP32-MCP-Server", res.result()["serverInfo"]["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("1.0.0", res.result()["serverInfo"]["version"].as<const char*>());
    TEST_ASSERT_TRUE(res.result()["instructions"].isNull());
}

/* ======== tools/list: complex schemas ======== */

void test_handle_tools_list_complex_schemas(void) {
    Tool tool;
    tool.name = "advanced";
    tool.description = "Advanced tool";
    tool.inputSchema = Schema::object()
        .additionalProperties(false)
        .property("name", Schema::string().description("User name").format("email"))
        .property("tags", Schema::array().items(Schema::string()))
        .property("color", Schema::string().enumValues({"red", "blue"}).defaultValue("red"))
        .required({"name"})
        .build();
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    JsonDocument body;
    parseResponseBody(server, res, body);
    JsonVariantConst t = body["result"]["tools"][0];
    JsonVariantConst schema = t["inputSchema"];
    TEST_ASSERT_FALSE(schema["additionalProperties"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("email", schema["properties"]["name"]["format"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string", schema["properties"]["tags"]["items"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL(2, schema["properties"]["color"]["enum"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("red", schema["properties"]["color"]["default"].as<const char*>());
    TEST_ASSERT_EQUAL(1, schema["required"].as<JsonArrayConst>().size());
}

/* ======== parseRequest: null id ======== */

void test_parse_null_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":null,"method":"initialize","params":{}})");
    TEST_ASSERT_EQUAL_STRING("initialize", req.method.c_str());
    TEST_ASSERT_TRUE(req.id().isNull());
}

void test_parse_zero_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":0,"method":"tools/list"})");
    TEST_ASSERT_EQUAL_STRING("tools/list", req.method.c_str());
    TEST_ASSERT_EQUAL(0, req.id().as<int>());
}

void test_parse_negative_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":-1,"method":"tools/list"})");
    TEST_ASSERT_EQUAL_STRING("tools/list", req.method.c_str());
    TEST_ASSERT_EQUAL(-1, req.id().as<int>());
}

/* ======== handle: ping ======== */

void test_handle_ping_returns_empty_result(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());
    TEST_ASSERT_EQUAL(7, res.id().as<int>());

    /* The result must be an empty object, serialized as "result":{} */
    std::string json = server->serializeResponse(res);
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"result\":{}"));
}

void test_handle_ping_notification_gets_no_response(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","method":"ping"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_FALSE(res.hasBody());
    TEST_ASSERT_EQUAL_STRING("", server->serializeResponse(res).c_str());
}

/* ======== handle: protocol version 2025-06-18 ======== */

void test_handle_initialize_negotiates_2025_06_18(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0.0"}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_EQUAL_STRING("2025-06-18", res.result()["protocolVersion"].as<const char*>());
}

/* ======== handle: invalid request vs parse error ======== */

void test_valid_json_without_method_is_invalid_request(void) {
    /* Structurally valid JSON that is not a JSON-RPC request must yield
     * -32600 Invalid Request (with the id echoed), not -32700 Parse error. */
    MCPRequest req = server->parseRequest(R"({"jsonrpc":"2.0","id":3})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32600, res.error()["code"].as<int>());
    TEST_ASSERT_EQUAL(3, res.id().as<int>());
}

void test_empty_object_is_invalid_request_with_null_id(void) {
    MCPRequest req = server->parseRequest("{}");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32600, res.error()["code"].as<int>());
    TEST_ASSERT_TRUE(res.id().isNull());
}

/* ======== handle: notification semantics ======== */

class CountingHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        (void)params;
        calls++;
        JsonDocument result;
        result["ok"] = true;
        return result;
    }
    int calls = 0;
};

void test_notification_tools_call_gets_no_response_and_is_not_executed(void) {
    Tool tool;
    tool.name = "counter";
    tool.description = "Counts invocations";
    tool.inputSchema = Schema::object().build();
    auto handler = std::make_shared<CountingHandler>();
    tool.handler = handler;
    server->RegisterTool(tool);

    /* No id → notification. JSON-RPC 2.0 forbids any response; MCP clients
     * never send tools/call as a notification, so it must not execute either. */
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"counter","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_FALSE(res.hasBody());
    TEST_ASSERT_EQUAL_STRING("", server->serializeResponse(res).c_str());
    TEST_ASSERT_EQUAL(0, handler->calls);
}

void test_notification_tools_list_gets_no_response(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","method":"tools/list"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_FALSE(res.hasBody());
}

void test_request_to_initialized_method_is_invalid_request(void) {
    /* notifications/initialized carrying an id is malformed — it must yield a
     * proper error object, not a body-less 202 or an empty response. */
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":9,"method":"notifications/initialized"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasBody());
    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32600, res.error()["code"].as<int>());
    TEST_ASSERT_EQUAL(9, res.id().as<int>());
}

/* ======== Main ======== */

int main(int argc, char** argv) {
    UNITY_BEGIN();

    /* parseRequest */
    RUN_TEST(test_parse_valid_request);
    RUN_TEST(test_parse_string_id);
    RUN_TEST(test_parse_fractional_numeric_id);
    RUN_TEST(test_parse_invalid_json);
    RUN_TEST(test_parse_empty_string);
    RUN_TEST(test_parse_no_params);
    RUN_TEST(test_parse_with_arguments);
    RUN_TEST(test_invalid_jsonrpc_envelope_is_rejected);

    /* handle: initialize */
    RUN_TEST(test_handle_initialize);
    RUN_TEST(test_handle_initialize_does_not_negotiate_legacy_http_sse_version);
    RUN_TEST(test_handle_initialize_falls_back_to_latest_for_unknown_version);
    RUN_TEST(test_handle_initialize_no_instructions);
    RUN_TEST(test_handle_initialize_requires_mandatory_params);

    /* handle: notifications */
    RUN_TEST(test_handle_notifications_initialized);
    RUN_TEST(test_notification_tools_call_gets_no_response_and_is_not_executed);
    RUN_TEST(test_notification_tools_list_gets_no_response);
    RUN_TEST(test_request_to_initialized_method_is_invalid_request);

    /* handle: ping */
    RUN_TEST(test_handle_ping_returns_empty_result);
    RUN_TEST(test_handle_ping_notification_gets_no_response);

    /* handle: protocol versions */
    RUN_TEST(test_handle_initialize_negotiates_2025_06_18);

    /* handle: tools/list */
    RUN_TEST(test_handle_tools_list_empty);
    RUN_TEST(test_handle_tools_list_with_tool);
    RUN_TEST(test_handle_tools_list_multiple_tools);
    RUN_TEST(test_handle_tools_list_with_output_schema);

    /* handle: tools/call */
    RUN_TEST(test_handle_tool_call_success);
    RUN_TEST(test_handle_tool_call_handler_reports_execution_error);
    RUN_TEST(test_handle_tool_call_scalar_result_has_no_structured_content);
    RUN_TEST(test_structured_result_text_mirroring_follows_build_flag);
    RUN_TEST(test_tools_list_body_is_cached_between_calls);
    RUN_TEST(test_registering_a_tool_invalidates_the_tools_list_cache);
    RUN_TEST(test_register_tool_move_overload_registers_and_invalidates);
    RUN_TEST(test_handle_tool_call_missing_name);
    RUN_TEST(test_handle_tool_call_unknown_tool);
    RUN_TEST(test_handle_tool_call_null_handler);

    /* handle: errors */
    RUN_TEST(test_handle_unknown_method);
    RUN_TEST(test_handle_parse_error);
    RUN_TEST(test_valid_json_without_method_is_invalid_request);
    RUN_TEST(test_empty_object_is_invalid_request_with_null_id);

    /* serializeResponse */
    RUN_TEST(test_serialize_response_with_result);
    RUN_TEST(test_serialize_response_with_error);
    RUN_TEST(test_serialize_includes_jsonrpc_version);

    /* RegisterTool */
    RUN_TEST(test_register_tool);
    RUN_TEST(test_register_overwrites_same_name);

    /* MCPResponse struct */
    RUN_TEST(test_response_default_constructor);
    RUN_TEST(test_response_id_constructor);
    RUN_TEST(test_response_code_id_constructor);

    /* MCPRequest struct */
    RUN_TEST(test_request_default_state);

    /* createJSONRPCError */
    RUN_TEST(test_create_error_all_codes);
    RUN_TEST(test_create_error_preserves_string_id);

    /* tools/call edge cases */
    RUN_TEST(test_handle_tool_call_empty_arguments);
    RUN_TEST(test_handle_tool_call_complex_result);
    RUN_TEST(test_handle_tool_call_empty_result);
    RUN_TEST(test_handle_tool_call_multiple_different_tools);

    /* Full roundtrip */
    RUN_TEST(test_roundtrip_initialize);
    RUN_TEST(test_roundtrip_tool_call);
    RUN_TEST(test_roundtrip_error);

    /* Default server */
    RUN_TEST(test_server_default_name_version);

    /* tools/list: complex schemas */
    RUN_TEST(test_handle_tools_list_complex_schemas);

    /* parseRequest edge cases */
    RUN_TEST(test_parse_null_id);
    RUN_TEST(test_parse_zero_id);
    RUN_TEST(test_parse_negative_id);

    return UNITY_END();
}
