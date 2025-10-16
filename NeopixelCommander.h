#pragma once

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>

class NeopixelCommander
{
public:
  // Constructor: ssid, password, neopixel data pin, number of pixels
  NeopixelCommander(const char *ssid, const char *password, uint8_t pin, uint16_t numPixels)
      : _ssid(ssid), _password(password), _pin(pin), _numPixels(numPixels),
        _server(80), _ws("/ws"), _strip(numPixels, pin, NEO_GRB + NEO_KHZ800),
        _connectTimeoutMs(15000) {}

  // Set connection timeout for station mode (default 15000 ms).
  void setConnectTimeout(uint32_t ms) { _connectTimeoutMs = ms; }

  // Initialize Wi-Fi (STA with timeout -> AP fallback), WebSocket, and LEDs.
  void begin()
  {
    Serial.begin(115200);
    Serial.printf("NeopixelWSController starting. Trying STA connect to '%s'\n", _ssid);

    // Prefer STA mode first
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid, _password);

    uint32_t start = millis();
    bool staConnected = false;

    while (millis() - start < _connectTimeoutMs)
    {
      if (WiFi.status() == WL_CONNECTED)
      {
        staConnected = true;
        break;
      }
      delay(250);
      Serial.print(".");
    }

    if (staConnected)
    {
      Serial.printf("\nConnected as STA. IP: %s\n", WiFi.localIP().toString().c_str());
    }
    else
    {
      // STA failed -> start AP with the same credentials
      Serial.printf("\nSTA connect failed after %u ms. Starting SoftAP with SSID '%s'\n",
                    (unsigned)_connectTimeoutMs, _ssid);

      // Use WPA2 if password length >= 8; otherwise start open AP
      if (_password != nullptr && strlen(_password) >= 8)
      {
        WiFi.mode(WIFI_AP_STA); // allow both modes
        bool ok = WiFi.softAP(_ssid, _password);
        if (!ok)
        {
          Serial.println("softAP() returned false. Attempting open AP (no password).");
          WiFi.softAP(_ssid);
        }
      }
      else
      {
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
                       AwsEventType type, void *arg, uint8_t *data, size_t len)
                { this->_onWsEvent(server, client, type, arg, data, len); });

    // Register the websocket endpoint and start server
    _server.addHandler(&_ws);

    _server.on("/api/setColor", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
    if (request->hasParam("r", true) && request->hasParam("g", true) && request->hasParam("b", true)) {
        int r = request->getParam("r", true)->value().toInt();
        int g = request->getParam("g", true)->value().toInt();
        int b = request->getParam("b", true)->value().toInt();
        this->setColor(r,g,b);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
        request->send(400, "application/json", "{\"error\":\"missing_params\"}");
    } });

    _server.on("/api/clear", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
    _strip.clear();
    _strip.show();
    request->send(200, "application/json", "{\"status\":\"ok\"}"); });

    _server.on("/api/setBrightness", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
    if (request->hasParam("brightness", true)) {
        int b = request->getParam("brightness", true)->value().toInt();
        _strip.setBrightness(b);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
        request->send(400, "application/json", "{\"error\":\"missing_param\"}");
    } });

    _server.on("/api/show", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
    this->show();
    request->send(200, "application/json", "{\"status\":\"ok\"}"); });

    _server.begin();

    // Print final instruction
    IPAddress ip = (WiFi.getMode() & WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP();
    Serial.printf("WebSocket endpoint: ws://%s/ws\n", ip.toString().c_str());
  }

  // Minimal loop: cleanup disconnected WS clients
  void loop()
  {
    _ws.cleanupClients();
  }

  // Manual API to set color from code
  void setColor(uint8_t r, uint8_t g, uint8_t b)
  {
    for (uint16_t i = 0; i < _numPixels; ++i)
      _strip.setPixelColor(i, _strip.Color(r, g, b));
  }
  void setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b)
  {
    if (n < _numPixels)
    {
      _strip.setPixelColor(n, _strip.Color(r, g, b));
    }
  }

  void clear()
  {
    _strip.clear();
  }
  void show()
  {
    _strip.show();
  }
  void setBrightness(uint8_t brightness)
  {
    _strip.setBrightness(brightness);
  }

private:
  const char *_ssid;
  const char *_password;
  uint8_t _pin;
  uint16_t _numPixels;

  AsyncWebServer _server;
  AsyncWebSocket _ws;
  Adafruit_NeoPixel _strip;

  uint32_t _connectTimeoutMs;

  // Handle incoming websocket messages (simple JSON {r,g,b})
  inline void _onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data, size_t len)
  {
    if (type == WS_EVT_DATA)
    {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->final && info->opcode == WS_TEXT)
      {
        // ensure NUL-termination (we assume buffer has an extra byte;
        // this pattern matches earlier header-only version; be careful)
        data[len] = 0;
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, (char *)data) == DeserializationError::Ok)
        {
          String cmd = doc["cmd"] | "";
          if (cmd == "setColor")
          {
            uint8_t r = doc["r"] | 0;
            uint8_t g = doc["g"] | 0;
            uint8_t b = doc["b"] | 0;
            setColor(r, g, b);
          }
          else if (cmd == "clear")
          {
            clear();
          }
          else if (cmd == "setPixelColor")
          {
            uint16_t idx = doc["index"] | 0;
            uint8_t r = doc["r"] | 0;
            uint8_t g = doc["g"] | 0;
            uint8_t b = doc["b"] | 0;
            setPixelColor(idx, r, g, b);
          }
          else if (cmd == "show")
          {
            show();
          }
          else if (cmd == "setBrightness")
          {
            uint8_t b = doc["brightness"] | 255;
            setBrightness(b);
          }
          else
          {
            client->text("{\"error\":\"unknown_cmd\"}");
            return;
          }
          client->text("{\"status\":\"ok\"}");
        }
        else
        {
          client->text("{\"error\":\"bad_json\"}");
        }
      }
    }
  }
};
