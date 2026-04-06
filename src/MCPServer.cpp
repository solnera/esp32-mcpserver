#include "MCPServer.h"

#include <Arduino.h>
#include <ArduinoJson.h>

MCPServer::MCPServer(uint16_t port, const String& name, const String& version, const String& instructions)
    : serverName(name), serverVersion(version), serverInstructions(instructions) {
    // Seed random for session ID generation
    randomSeed(esp_random());
    server = new AsyncWebServer(port);
    setupWebServer();
}

MCPServer::~MCPServer() {
    if (server) {
        delete server;
        server = nullptr;
    }
}

void MCPServer::setupWebServer() {
    // Body callback accumulates chunks; request handler processes after all data arrives.
    // Using malloc for _tempObject since ESPAsyncWebServer calls free() on it in its destructor,
    // ensuring safe cleanup even if the client disconnects mid-transfer.
    server->on(
        "/mcp", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            // Request handler: called after all body chunks are received
            auto* body = static_cast<BodyBuffer*>(request->_tempObject);
            if (!body || body->length == 0) {
                request->_tempObject = nullptr;
                request->send(400, "application/json",
                              "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Empty request body\"}}");
                return;
            }

            // Null-terminate for safe parsing
            body->data[body->length] = '\0';

            Serial.println("========================================");
            Serial.println("Received /mcp request:");
            Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());
            Serial.printf("Request length: %zu bytes\n", body->length);
            Serial.printf("Request content: %s\n", body->data);

            Serial.println("Request headers:");
            for (size_t i = 0; i < request->headers(); i++) {
                const AsyncWebHeader* h = request->getHeader(i);
                Serial.printf("  %s: %s\n", h->name().c_str(), h->value().c_str());
            }
            Serial.println("========================================");

            MCPRequest mcpReq = parseRequest(std::string(body->data, body->length));

            // Clean up body buffer before processing (free memory early)
            free(body);
            request->_tempObject = nullptr;

            MCPResponse mcpRes = handle(mcpReq);

            // Handle notifications: no response body per JSON-RPC 2.0
            if (mcpRes.code == 204) {
                request->send(204);
                return;
            }

            std::string jsonResponse = serializeResponse(mcpRes);

            // Handle session ID
            std::string sessionId;
            if (request->hasHeader("mcp-session-id")) {
                sessionId = request->getHeader("mcp-session-id")->value().c_str();
            } else {
                sessionId = generateSessionId();
            }

            AsyncWebServerResponse* response =
                request->beginResponse(mcpRes.code, "application/json", jsonResponse.c_str());
            response->addHeader("mcp-session-id", sessionId.c_str());
            request->send(response);
        },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            // Body callback: just accumulate chunks
            if (index == 0) {
                request->_tempObject = BodyBuffer::create(total);
                if (!request->_tempObject) return;
            }

            auto* body = static_cast<BodyBuffer*>(request->_tempObject);
            if (!body) return;

            size_t copyLen = len;
            if (body->length + copyLen > body->capacity) {
                copyLen = body->capacity - body->length;
            }
            memcpy(body->data + body->length, data, copyLen);
            body->length += copyLen;
        });

    server->on("/mcp", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        Serial.println("========================================");
        Serial.println("Received /mcp DELETE request:");
        Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());

        Serial.println("Request headers:");
        for (size_t i = 0; i < request->headers(); i++) {
            const AsyncWebHeader* h = request->getHeader(i);
            Serial.printf("  %s: %s\n", h->name().c_str(), h->value().c_str());
        }
        Serial.println("========================================");

        request->send(200, "application/json", "{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":{}}");
    });

    server->on("/mcp", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Serial.println("========================================");
        Serial.println("Received /mcp GET request:");
        Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());
        Serial.println("========================================");

        request->send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
    });

    server->onNotFound([this](AsyncWebServerRequest* request) {
        JsonDocument nullId;
        nullId.set(nullptr);
        MCPResponse res = createJSONRPCError(
            404, static_cast<int>(ErrorCode::INVALID_REQUEST),
            nullId.as<JsonVariantConst>(), "Path Not Found");
        std::string jsonResponse = serializeResponse(res);
        request->send(res.code, "application/json", jsonResponse.c_str());
    });

    server->begin();
}

MCPRequest MCPServer::parseRequest(const std::string& json) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);

    MCPRequest request;

    if (error) {
        request.method = "";
        return request;
    }

    request.method = doc["method"].as<std::string>();
    request.idDoc.set(doc["id"]);
    request.paramsDoc = doc["params"];
    return request;
}

std::string MCPServer::serializeResponse(const MCPResponse& response) {
    JsonDocument doc;
    doc["jsonrpc"] = "2.0";
    doc["id"] = response.id();

    if (response.hasResult()) {
        doc["result"] = response.result();
    }
    if (response.hasError()) {
        doc["error"] = response.error();
    }

    std::string jsonResponse;
    serializeJson(doc, jsonResponse);
    return jsonResponse;
}

MCPResponse MCPServer::handle(MCPRequest& request) {
    if (request.method.empty()) {
        return createJSONRPCError(400, static_cast<int>(ErrorCode::PARSE_ERROR),
                                  request.id(), "Parse error: Invalid JSON");
    }

    if (request.method == "initialize") {
        return handleInitialize(request);
    } else if (request.method == "tools/list") {
        return handleToolsList(request);
    } else if (request.method == "notifications/initialized") {
        return handleInitialized(request);
    } else if (request.method == "tools/call") {
        return handleFunctionCalls(request);
    } else {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::METHOD_NOT_FOUND),
                                  request.id(), "Method not found: " + request.method);
    }
}

MCPResponse MCPServer::handleInitialize(MCPRequest& request) {
    MCPResponse response(200, request.id());
    JsonObject result = response.resultDoc.to<JsonObject>();

    result["protocolVersion"] = PROTOCOL_VERSION;

    JsonObject capabilities = result["capabilities"].to<JsonObject>();
    capabilities["experimental"].to<JsonObject>();

    JsonObject tools = capabilities["tools"].to<JsonObject>();
    tools["listChanged"] = false;

    JsonObject serverInfo = result["serverInfo"].to<JsonObject>();
    serverInfo["name"] = serverName;
    serverInfo["version"] = serverVersion;

    if (serverInstructions.length() > 0) {
        result["instructions"] = serverInstructions;
    }
    return response;
}

MCPResponse MCPServer::handleInitialized(MCPRequest& request) {
    // Notifications get no response body per JSON-RPC 2.0
    MCPResponse response;
    response.code = 204;
    return response;
}

MCPResponse MCPServer::handleToolsList(MCPRequest& request) {
    MCPResponse response(200, request.id());
    JsonObject result = response.resultDoc.to<JsonObject>();
    JsonArray toolsArray = result["tools"].to<JsonArray>();

    std::lock_guard<std::mutex> lock(toolsMutex);
    for (const auto& [key, value] : tools) {
        JsonObject tool = toolsArray.add<JsonObject>();
        tool["name"] = key;
        tool["description"] = value.description;
        tool["inputSchema"].set(value.inputSchema);

        if (!value.outputSchema.isNull()) {
            tool["outputSchema"].set(value.outputSchema);
        }
    }
    return response;
}

MCPResponse MCPServer::handleFunctionCalls(MCPRequest& request) {
    MCPResponse mcpResponse(200, request.id());
    JsonVariantConst params = request.params();

    if (!params["name"].is<std::string>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS),
                                  request.id(), "Missing or invalid 'name' parameter");
    }

    String functionName = params["name"].as<const char*>();
    JsonVariantConst arguments = params["arguments"];

    std::shared_ptr<ToolHandler> handler;
    {
        std::lock_guard<std::mutex> lock(toolsMutex);
        auto toolIt = tools.find(functionName);
        if (toolIt == tools.end()) {
            return createJSONRPCError(200, static_cast<int>(ErrorCode::METHOD_NOT_FOUND),
                                      request.id(),
                                      std::string("Method not supported: ") + functionName.c_str());
        }
        if (!toolIt->second.handler) {
            return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR),
                                      request.id(),
                                      std::string("Tool handler not initialized: ") + functionName.c_str());
        }
        handler = toolIt->second.handler;
    }

    // Call handler outside of lock, with exception protection
    JsonDocument argsDoc;
    argsDoc.set(arguments);

    JsonDocument resultDoc;
    try {
        resultDoc = handler->call(std::move(argsDoc));
    } catch (const std::exception& e) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR),
                                  request.id(),
                                  std::string("Tool handler exception: ") + e.what());
    } catch (...) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR),
                                  request.id(), "Tool handler threw unknown exception");
    }

    JsonObject result = mcpResponse.resultDoc.to<JsonObject>();
    JsonArray content = result["content"].to<JsonArray>();
    JsonObject textContent = content.add<JsonObject>();
    textContent["type"] = "text";

    String resultText;
    serializeJson(resultDoc, resultText);
    textContent["text"] = resultText;

    return mcpResponse;
}

MCPResponse MCPServer::createJSONRPCError(int httpCode, int code, const JsonVariantConst& id,
                                          const std::string& message) {
    MCPResponse response(httpCode, id);
    response.errorDoc["code"] = code;
    response.errorDoc["message"] = message;
    return response;
}

std::string MCPServer::generateSessionId() {
    std::string sessionId;
    sessionId.reserve(36);
    const char* chars = "0123456789abcdef";

    for (int i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            sessionId += '-';
        }
        sessionId += chars[random(16)];
    }

    return sessionId;
}

void MCPServer::RegisterTool(const Tool& tool) {
    std::lock_guard<std::mutex> lock(toolsMutex);
    tools[tool.name] = tool;
    Serial.printf("Tool registered: %s\n", tool.name.c_str());
}
