#pragma once
/**
 * @file NeopixelWSController.h
 * @brief NeopixelWSController: WebSocket-controlled NeoPixels with STA->AP fallback.
 *
 * Behavior:
 *  - Attempt to connect to configured Wi-Fi (station).
 *  - If station connection does not succeed within timeout, bring up a SoftAP
 *    using the same SSID/password (if password < 8 chars, AP is opened without password).
 *
 * Usage:
 *   NeopixelWSController lights("MySSID", "MyPass", 5, 16);
 *   lights.setConnectTimeout(15000); // optional, milliseconds
 *   lights.begin();
 *   // loop() called from main loop
 *
 * Author: (replace with your name)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>

class NeopixelWSController {
public:
  // Constructor: ssid, password, neopixel data pin, number of pixels
  NeopixelWSController(const char* ssid, const char* password, uint8_t pin, uint16_t numPixels)
    : _ssid(ssid), _password(password), _pin(pin), _numPixels(numPixels),
      _server(80), _ws("/ws"), _strip(numPixels, pin, NEO_GRB + NEO_KHZ800),
      _connectTimeoutMs(15000) {}

  // Set connection timeout for station mode (default 15000 ms).
  void setConnectTimeout(uint32_t ms) { _connectTimeoutMs = ms; }

  // Initialize Wi-Fi (STA with timeout -> AP fallback), WebSocket, and LEDs.
  void begin() {
    Serial.begin(115200);
    Serial.printf("NeopixelWSController starting. Trying STA connect to '%s'\n", _ssid);

    // Prefer STA mode first
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid, _password);

    uint32_t start = millis();
    bool staConnected = false;

    while (millis() - start < _connectTimeoutMs) {
      if (WiFi.status() == WL_CONNECTED) {
        staConnected = true;
        break;
      }
      delay(250);
      Serial.print(".");
    }

    if (staConnected) {
      Serial.printf("\nConnected as STA. IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      // STA failed -> start AP with the same credentials
      Serial.printf("\nSTA connect failed after %u ms. Starting SoftAP with SSID '%s'\n",
                    (unsigned)_connectTimeoutMs, _ssid);

      // Use WPA2 if password length >= 8; otherwise start open AP
      if (_password != nullptr && strlen(_password) >= 8) {
        WiFi.mode(WIFI_AP_STA); // allow both modes
        bool ok = WiFi.softAP(_ssid, _password);
        if (!ok) {
          Serial.println("softAP() returned false. Attempting open AP (no password).");
          WiFi.softAP(_ssid);
        }
      } else {
        Serial.println("Password too short for WPA2; starting open AP.");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(_ssid);
      }

      // Small delay to let AP initialize
      delay(500);
      Serial.printf("SoftAP active. AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    }

    // Initialize NeoPixel strip
    _strip.begin();
    _strip.show(); // clear

    // WebSocket event handler (captures this)
    _ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
      this->_onWsEvent(server, client, type, arg, data, len);
    });

    // Register the websocket endpoint and start server
    _server.addHandler(&_ws);
    _server.begin();

    // Print final instruction
    IPAddress ip = (WiFi.getMode() & WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP();
    Serial.printf("WebSocket endpoint: ws://%s/ws\n", ip.toString().c_str());
  }

  // Minimal loop: cleanup disconnected WS clients
  void loop() {
    _ws.cleanupClients();
  }

  // Manual API to set color from code
  void setColor(uint8_t r, uint8_t g, uint8_t b) {
    for (uint16_t i = 0; i < _numPixels; ++i)
      _strip.setPixelColor(i, _strip.Color(r, g, b));
    _strip.show();
  }

private:
  const char* _ssid;
  const char* _password;
  uint8_t _pin;
  uint16_t _numPixels;

  AsyncWebServer _server;
  AsyncWebSocket _ws;
  Adafruit_NeoPixel _strip;

  uint32_t _connectTimeoutMs;

  // Handle incoming websocket messages (simple JSON {r,g,b})
  inline void _onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (info->final && info->opcode == WS_TEXT) {
        // ensure NUL-termination (we assume buffer has an extra byte; 
        // this pattern matches earlier header-only version; be careful)
        data[len] = 0;
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, (char*)data) == DeserializationError::Ok) {
          int r = doc["r"] | 0;
          int g = doc["g"] | 0;
          int b = doc["b"] | 0;
          setColor((uint8_t)r, (uint8_t)g, (uint8_t)b);
          client->text("{\"status\":\"ok\"}");
        } else {
          client->text("{\"error\":\"bad_json\"}");
        }
      }
    }
  }
};
