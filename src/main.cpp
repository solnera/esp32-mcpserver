#include <Arduino.h>
#include <WiFi.h>
#include "MCPServer.h"

// WiFi configuration
const char* ssid = "";
const char* password = "";

// MCP Server port
const uint16_t MCP_PORT = 3000;

// Create MCP Server instance
MCPServer* mcpServer = nullptr;

// Example tool handler: Echo text
class EchoHandler : public ToolHandler {
public:
    JsonDocument call(JsonDocument params) override {
        JsonDocument result;
        
        // Get text parameter from input
        JsonVariantConst paramsVariant = params.as<JsonVariantConst>();
        String text = paramsVariant["text"].as<String>();
        // Log the received text via Serial
        Serial.printf("[Echo Tool] Received text: %s\n", text.c_str());
        // Echo the text back
        result["echo"] = text;
        result["length"] = text.length();
        result["timestamp"] = millis();
        
        return result;
    }
};

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n========================================");
    Serial.println("ESP32 MCP Server Example");
    Serial.println("========================================");
      
    // Connect to WiFi
    Serial.printf("Connecting to WiFi: %s\n", ssid);
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\nWiFi connected!");
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
    
    // Create MCP Server instance with custom name and version
    Serial.printf("\nStarting MCP Server (port: %d)...\n", MCP_PORT);
    mcpServer = new MCPServer(MCP_PORT, "echo service", "1.0.0");
    
    // Register tool: Echo
    Tool echoTool;
    echoTool.name = "echo";
    echoTool.description = "Echo back the input text";
    
    // Configure input parameter schema
    echoTool.inputSchema.type = "object";
    echoTool.inputSchema.description = "Echo tool parameters";
    
    // Define text parameter
    Properties textProperty;
    textProperty.type = "string";
    textProperty.description = "Text to echo back";
    echoTool.inputSchema.properties["text"] = textProperty;
    
    // Set required parameters
    echoTool.inputSchema.required.push_back("text");
    
    // Set handler
    echoTool.handler = std::make_shared<EchoHandler>();
    
    mcpServer->RegisterTool(echoTool);
    Serial.println("✓ Tool registered: echo");
    
    Serial.println("\n========================================");
    Serial.println("MCP Server started successfully!");
    Serial.printf("Access URL: http://%s:%d/mcp\n", WiFi.localIP().toString().c_str(), MCP_PORT);
    Serial.println("========================================\n");
}

void loop() {
    // Main loop stays idle
    // MCP Server handles requests through async web server
    delay(1000);
    
    // Optional: Print status information periodically
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 30000) {  // Print every 30 seconds
        lastPrint = millis();
        Serial.printf("[Status] Uptime: %lu seconds, WiFi status: %s\n", 
                     millis() / 1000, 
                     WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    }
}
