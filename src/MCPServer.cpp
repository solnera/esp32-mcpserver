#include "MCPServer.h"

#include <Arduino.h>
#include <ArduinoJson.h>

String Properties::toString() const {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    toJson(obj);
    String result;
    serializeJson(doc, result);
    return result;
}

void Properties::toJson(JsonObject& obj) const {
    obj["type"] = type;

    if (title.length() > 0) {
        obj["title"] = title;
    }

    if (description.length() > 0) {
        obj["description"] = description;
    }

    if (!properties.empty()) {
        JsonObject propertiesObj = obj["properties"].to<JsonObject>();
        for (const auto& kv : properties) {
            const String& key = kv.first;
            const Properties& value = kv.second;
            JsonObject propObj = propertiesObj[key].to<JsonObject>();
            value.toJson(propObj);
        }
    }

    if (!required.empty()) {
        JsonArray requiredArray = obj["required"].to<JsonArray>();
        for (const auto& req : required) {
            requiredArray.add(req);
        }
    }

    if (hasAdditionalProperties) {
        obj["additionalProperties"] = additionalProperties;
    }

    if (items) {
        JsonObject itemsObj = obj["items"].to<JsonObject>();
        items->toJson(itemsObj);
    }

    if (!enumValues.empty()) {
        JsonArray enumArray = obj["enum"].to<JsonArray>();
        for (const auto& value : enumValues) {
            enumArray.add(value);
        }
    }

    if (!oneOf.empty()) {
        JsonArray oneOfArray = obj["oneOf"].to<JsonArray>();
        for (const auto& schema : oneOf) {
            JsonObject schemaObj = oneOfArray.add<JsonObject>();
            schema.toJson(schemaObj);
        }
    }

    if (!anyOf.empty()) {
        JsonArray anyOfArray = obj["anyOf"].to<JsonArray>();
        for (const auto& schema : anyOf) {
            JsonObject schemaObj = anyOfArray.add<JsonObject>();
            schema.toJson(schemaObj);
        }
    }

    if (!allOf.empty()) {
        JsonArray allOfArray = obj["allOf"].to<JsonArray>();
        for (const auto& schema : allOf) {
            JsonObject schemaObj = allOfArray.add<JsonObject>();
            schema.toJson(schemaObj);
        }
    }

    if (format.length() > 0) {
        obj["format"] = format;
    }

    if (defaultValue.length() > 0) {
        obj["default"] = defaultValue;
    }
}

String Tool::toString() const {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();

    obj["name"] = name;
    obj["description"] = description;

    JsonObject inputSchemaObj = obj["inputSchema"].to<JsonObject>();
    inputSchema.toJson(inputSchemaObj);

    if (outputSchema.type.length() > 0) {
        JsonObject outputSchemaObj = obj["outputSchema"].to<JsonObject>();
        outputSchema.toJson(outputSchemaObj);
    }

    String result;
    serializeJson(doc, result);
    return result;
}

MCPServer::MCPServer(uint16_t port, const String& name, const String& version, const String& instructions)
    : serverName(name), serverVersion(version), serverInstructions(instructions) {
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
    server->on(
        "/mcp", HTTP_POST, [this](AsyncWebServerRequest* request) {}, NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            // Body data processing callback
            String body = "";
            for (size_t i = 0; i < len; i++) {
                body += (char)data[i];
            }

            // Print received MCP request content
            Serial.println("========================================");
            Serial.println("Received /mcp request:");
            Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());
            Serial.printf("Request length: %zu bytes\n", len);
            Serial.printf("Request content: %s\n", body.c_str());

            // Print request headers
            Serial.println("Request headers:");
            for (size_t i = 0; i < request->headers(); i++) {
                AsyncWebHeader* h = request->getHeader(i);
                Serial.printf("  %s: %s\n", h->name().c_str(), h->value().c_str());
            }
            Serial.println("========================================");

            MCPRequest mcpReq = parseRequest(body.c_str());
            MCPResponse mcpRes = handle(mcpReq);
            std::string jsonResponse = serializeResponse(mcpRes);

            // Handle session ID
            std::string sessionId;
            if (request->hasHeader("mcp-session-id")) {
                sessionId = request->getHeader("mcp-session-id")->value().c_str();
            } else {
                sessionId = generateSessionId();
            }
            // Send response
            AsyncWebServerResponse* response =
                request->beginResponse(mcpRes.code, "application/json", jsonResponse.c_str());
            response->addHeader("mcp-session-id", sessionId.c_str());
            request->send(response);
        });

    server->on("/mcp", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        // Print received MCP DELETE request
        Serial.println("========================================");
        Serial.println("Received /mcp DELETE request:");
        Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());

        // Print request headers
        Serial.println("Request headers:");
        for (size_t i = 0; i < request->headers(); i++) {
            AsyncWebHeader* h = request->getHeader(i);
            Serial.printf("  %s: %s\n", h->name().c_str(), h->value().c_str());
        }
        Serial.println("========================================");

        // DELETE request usually doesn't need request body, return success response directly
        MCPResponse res;  // Default constructor with code 200
        std::string jsonResponse = serializeResponse(res);

        // Handle session ID
        std::string sessionId;
        if (request->hasHeader("mcp-session-id")) {
            sessionId = request->getHeader("mcp-session-id")->value().c_str();
        } else {
            sessionId = generateSessionId();
        }

        // Send response
        AsyncWebServerResponse* response = request->beginResponse(res.code, "application/json", jsonResponse.c_str());
        response->addHeader("mcp-session-id", sessionId.c_str());
        request->send(response);
    });

    server->on("/mcp", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Serial.println("========================================");
        Serial.println("Received /mcp GET request:");
        Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());
        Serial.println("========================================");
        
        request->send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
    });

    server->onNotFound([this](AsyncWebServerRequest* request) {
        // Handle not found requests
        JsonDocument nullId;
        nullId.set(nullptr);
        MCPResponse res = createJSONRPCError(404, static_cast<int>(ErrorCode::INVALID_REQUEST), nullId.as<JsonVariantConst>(), "Path Not Found");
        std::string jsonResponse = serializeResponse(res);
        request->send(res.code, "application/json", jsonResponse.c_str());
    });
    server->begin();
}

MCPRequest MCPServer::parseRequest(const std::string& json) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);

    MCPRequest request;

    // Check if JSON parsing succeeded
    if (error) {
        // If parsing failed, return a default request object
        request.method = "";
        return request;
    }

    request.method = doc["method"].as<std::string>();
    request.idDoc.set(doc["id"]);  // Preserve original id type (string, number, or null)
    request.paramsDoc = doc["params"];
    return request;
}

std::string MCPServer::serializeResponse(const MCPResponse& response) {
    JsonDocument doc;
    doc["id"] = response.id();  // Use id() method to get JsonVariant
    doc["jsonrpc"] = "2.0";

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
    // Check if request method is empty (indicates JSON parsing failure)
    if (request.method.empty()) {
        return createJSONRPCError(400, static_cast<int>(ErrorCode::PARSE_ERROR), request.id(), "Parse error: Invalid JSON");
    }

    if (request.method == "initialize") {
        return handleInitialize(request);
    } else if (request.method == "tools/list") {
        return handleToolsList(request);
    } else if (request.method == "notifications/initialized") {
        // Handle initialization notification
        return handleInitialized(request);
    } else if (request.method == "tools/call") {
        return handleFunctionCalls(request);
    } else {
        // Unknown method
        return createJSONRPCError(200, static_cast<int>(ErrorCode::METHOD_NOT_FOUND), request.id(), "Method not found: " + request.method);
    }
}

MCPResponse MCPServer::handleInitialize(MCPRequest& request) {
    MCPResponse response(200, request.id());
    JsonObject result = response.resultDoc.to<JsonObject>();

    result["protocolVersion"] = PROTOCOL_VERSION;

    // Set capabilities
    JsonObject capabilities = result["capabilities"].to<JsonObject>();
    JsonObject experimental = capabilities["experimental"].to<JsonObject>();

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
    return MCPResponse(202, request.id());
}

MCPResponse MCPServer::handleToolsList(MCPRequest& request) {
    MCPResponse response(200, request.id());
    JsonDocument doc;
    JsonObject result = doc.to<JsonObject>();
    JsonArray toolsArray = result["tools"].to<JsonArray>();
    for (const auto& [key, value] : tools) {
        JsonObject tool = toolsArray.add<JsonObject>();
        tool["name"] = key;
        tool["description"] = value.description;

        JsonObject inputSchemaObj = tool["inputSchema"].to<JsonObject>();
        value.inputSchema.toJson(inputSchemaObj);

        if (value.outputSchema.type.length() > 0) {
            JsonObject outputSchemaObj = tool["outputSchema"].to<JsonObject>();
            value.outputSchema.toJson(outputSchemaObj);
        }
    }
    response.resultDoc = doc;
    return response;
}

MCPResponse MCPServer::handleFunctionCalls(MCPRequest& request) {
    MCPResponse mcpResponse(200, request.id());
    JsonVariantConst params = request.params();

    // Check required parameters
    if (!params["name"].is<std::string>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(), "Missing or invalid 'name' parameter");
    }

    std::string functionName = params["name"];
    JsonVariantConst arguments = params["arguments"];

    JsonObject result = mcpResponse.resultDoc.to<JsonObject>();
    JsonArray content = result["content"].to<JsonArray>();

    // Match corresponding functionName from tools
    // Convert std::string to Arduino String
    String functionNameStr = String(functionName.c_str());
    auto toolIt = tools.find(functionNameStr);
    if (toolIt != tools.end()) {
        // Check if handler exists
        if (toolIt->second.handler) {
            // Convert arguments to JsonDocument
            JsonDocument argsDoc;
            argsDoc.set(arguments);

            // Call handler
            JsonDocument resultDoc = toolIt->second.handler->call(argsDoc);

            // Serialize result to string
            String resultText;
            serializeJson(resultDoc, resultText);

            JsonObject textContent = content.add<JsonObject>();
            textContent["type"] = "text";
            textContent["text"] = resultText;
            return mcpResponse;
        } else {
            return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR), request.id(),
                                      std::string("Tool handler not initialized: ") + functionNameStr.c_str());
        }
    } else {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::METHOD_NOT_FOUND), request.id(),
                                  std::string("Method not supported: ") + functionNameStr.c_str());
    }
    return mcpResponse;
}

// JSON-RPC 2.0 success response
std::string MCPServer::createJSONRPCResponse(const JsonVariant& id, const JsonVariant& result) {
    JsonDocument doc;
    doc["jsonrpc"] = "2.0";
    doc["id"] = id;
    doc["result"] = result;

    std::string response;
    serializeJson(doc, response);
    return response;
}

// JSON-RPC 2.0 error response
MCPResponse MCPServer::createJSONRPCError(int httpCode, int code, const JsonVariantConst& id,
                                          const std::string& message) {
    MCPResponse response(httpCode, id);

    response.errorDoc["code"] = code;
    response.errorDoc["message"] = message;

    return response;
}

// Keep old method for compatibility
std::string MCPServer::createHTTPResponse(bool success, const std::string& message, const JsonVariant& data) {
    JsonDocument doc;
    doc["success"] = success;
    doc["message"] = message;

    if (!data.isNull()) {
        doc["data"] = data;
    }

    std::string response;
    serializeJson(doc, response);
    return response;
}

std::string MCPServer::generateSessionId() {
    // Generate a simple UUID format session ID
    std::string sessionId = "";
    const char* chars = "0123456789abcdef";

    // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    for (int i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            sessionId += "-";
        }
        sessionId += chars[random(16)];
    }

    return sessionId;
}

void MCPServer::RegisterTool(const Tool& tool) {
    tools[tool.name] = tool;
    Serial.printf("Tool registered: %s\n", tool.name.c_str());
}
