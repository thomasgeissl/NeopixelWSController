#pragma once

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>

#define COMMANDS_PER_LOOP 10
#define DEBUG_LOGGING true

enum CommandType
{
  SET_PIXEL_COLOR,
  SET_COLOR,
  CLEAR,
  SHOW,
  SET_BRIGHTNESS
};

struct Command
{
  CommandType type;
  uint16_t index;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t brightness;
  uint32_t clientId;
  uint32_t commandId; // unique ID for this command
};

class NeopixelCommander
{
public:
  NeopixelCommander(const char *ssid, const char *password, uint8_t pin, uint16_t numPixels)
      : _ssid(ssid), _password(password), _pin(pin), _numPixels(numPixels),
        _server(80), _ws("/ws"), _strip(numPixels, pin, NEO_GRB + NEO_KHZ800),
        _connectTimeoutMs(15000) {}

  void setConnectTimeout(uint32_t ms) { _connectTimeoutMs = ms; }

  void begin()
  {
    Serial.begin(115200);
    if (DEBUG_LOGGING) Serial.printf("NeopixelWSController starting. Trying STA connect to '%s'\n", _ssid);

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
      if (DEBUG_LOGGING) Serial.print(".");
    }

    if (staConnected)
    {
      if (DEBUG_LOGGING) Serial.printf("\nConnected as STA. IP: %s\n", WiFi.localIP().toString().c_str());
    }
    else
    {
      if (DEBUG_LOGGING) Serial.printf("\nSTA connect failed after %u ms. Starting SoftAP with SSID '%s'\n",
                    (unsigned)_connectTimeoutMs, _ssid);

      if (_password != nullptr && strlen(_password) >= 8)
      {
        WiFi.mode(WIFI_AP_STA);
        bool ok = WiFi.softAP(_ssid, _password);
        if (!ok)
        {
          if (DEBUG_LOGGING) Serial.println("softAP() returned false. Attempting open AP (no password).");
          WiFi.softAP(_ssid);
        }
      }
      else
      {
        if (DEBUG_LOGGING) Serial.println("Password too short for WPA2; starting open AP.");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(_ssid);
      }

      delay(500);
      if (DEBUG_LOGGING) Serial.printf("SoftAP active. AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    }

    _strip.begin();
    _strip.show();

    _ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len)
                { this->_onWsEvent(server, client, type, arg, data, len); });

    _server.addHandler(&_ws);

    // HTTP Pixel count endpoint
    _server.on("/api/pixelCount", HTTP_GET, [this](AsyncWebServerRequest *request)
               {
      char response[64];
      snprintf(response, sizeof(response), "{\"status\":\"ok\",\"pixelCount\":%u}", _numPixels);
      request->send(200, "application/json", response); });

    // HTTP Ping endpoint
    _server.on("/ping", HTTP_GET, [this](AsyncWebServerRequest *request)
               {
      if (DEBUG_LOGGING) Serial.println("Received HTTP ping (GET)");
      request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"pong\"}"); });

    _server.on("/ping", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
      if (DEBUG_LOGGING) Serial.println("Received HTTP ping (POST)");
      request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"pong\"}"); });

    _server.on("/api/setColor", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
      if (request->hasParam("r", true) && request->hasParam("g", true) && request->hasParam("b", true)) {
          int r = request->getParam("r", true)->value().toInt();
          int g = request->getParam("g", true)->value().toInt();
          int b = request->getParam("b", true)->value().toInt();
          this->setColor(r,g,b);
          request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
          request->send(400, "application/json", "{\"status\":\"error\",\"error\":\"missing_params\"}");
      } });

    _server.on("/api/clear", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
      _strip.clear();
      request->send(200, "application/json", "{\"status\":\"ok\"}"); });
    
    _server.on("/api/clear", HTTP_GET, [this](AsyncWebServerRequest *request)
               {
      _strip.clear();
      request->send(200, "application/json", "{\"status\":\"ok\"}"); });

    _server.on("/api/setBrightness", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
      if (request->hasParam("brightness", true)) {
          int b = request->getParam("brightness", true)->value().toInt();
          _strip.setBrightness(b);
          request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
          request->send(400, "application/json", "{\"status\":\"error\",\"error\":\"missing_param\"}");
      } });

    _server.on("/api/show", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
                 this->show();
                 request->send(200, "application/json", "{\"status\":\"ok\"}");
               });

    _server.begin();

    IPAddress ip = (WiFi.getMode() & WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP();
    if (DEBUG_LOGGING) Serial.printf("WebSocket endpoint: ws://%s/ws\n", ip.toString().c_str());
    if (DEBUG_LOGGING) Serial.printf("HTTP ping endpoint: http://%s/ping\n", ip.toString().c_str());
  }

  void loop()
  {
    _ws.cleanupClients();
    
    // Process multiple commands per loop to keep up with incoming rate
    int processed = 0;
    while (queueStart != queueEnd && processed < COMMANDS_PER_LOOP)
    {
      Command cmd = commandQueue[queueStart];
      queueStart = (queueStart + 1) % QUEUE_SIZE;

      // Execute the command
      switch (cmd.type)
      {
      case SET_PIXEL_COLOR:
        _strip.setPixelColor(cmd.index, cmd.r, cmd.g, cmd.b);
        break;
      case SET_COLOR:
        for (uint16_t i = 0; i < _numPixels; ++i)
          _strip.setPixelColor(i, _strip.Color(cmd.r, cmd.g, cmd.b));
        break;
      case CLEAR:
        _strip.clear();
        break;
      case SHOW:
        _strip.show();
        break;
      case SET_BRIGHTNESS:
        _strip.setBrightness(cmd.brightness);
        break;
      }

      // Send acknowledgment AFTER the command is executed
      AsyncWebSocketClient *client = _ws.client(cmd.clientId);
      if (client && client->status() == WS_CONNECTED)
      {
        char ackMsg[64];
        snprintf(ackMsg, sizeof(ackMsg), "{\"status\":\"ok\",\"ack\":%u}", cmd.commandId);
        client->text(ackMsg);
      }
      
      processed++;
    }
  }

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
  static const uint16_t QUEUE_SIZE = 512;
  
  const char *_ssid;
  const char *_password;
  uint8_t _pin;
  uint16_t _numPixels;

  AsyncWebServer _server;
  AsyncWebSocket _ws;
  Adafruit_NeoPixel _strip;

  uint32_t _connectTimeoutMs;

  Command commandQueue[QUEUE_SIZE];
  uint16_t queueStart = 0;
  uint16_t queueEnd = 0;

  bool enqueueCommand(const Command &cmd)
  {
    uint16_t nextEnd = (queueEnd + 1) % QUEUE_SIZE;
    if (nextEnd == queueStart)
    {
      if (DEBUG_LOGGING) Serial.printf("WARNING: Command queue full! Dropping command ID %u\n", cmd.commandId);
      return false;
    }
    commandQueue[queueEnd] = cmd;
    queueEnd = nextEnd;
    return true;
  }

  inline void _onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data, size_t len)
  {
    if (type == WS_EVT_CONNECT)
    {
      if (DEBUG_LOGGING) Serial.printf("WebSocket client #%u connected\n", client->id());
    }
    else if (type == WS_EVT_DISCONNECT)
    {
      if (DEBUG_LOGGING) Serial.printf("WebSocket client #%u disconnected\n", client->id());
    }
    else if (type == WS_EVT_DATA)
    {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->final && info->opcode == WS_TEXT)
      {
        data[len] = 0;
        
        // Check for simple "ping" message (not JSON)
        if (strcmp((char *)data, "ping") == 0)
        {
          if (DEBUG_LOGGING) Serial.printf("Received WebSocket ping from client #%u\n", client->id());
          client->text("{\"status\":\"ok\",\"message\":\"pong\"}");
          return;
        }
        
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, (char *)data) == DeserializationError::Ok)
        {
          const char* cmd = doc["cmd"] | "";
          
          // Handle JSON ping command
          if (strcmp(cmd, "ping") == 0)
          {
            if (DEBUG_LOGGING) Serial.printf("Received WebSocket ping (JSON) from client #%u\n", client->id());
            client->text("{\"status\":\"ok\",\"message\":\"pong\"}");
            return;
          }
          
          // Handle getPixelCount command
          if (strcmp(cmd, "getPixelCount") == 0)
          {
            if (DEBUG_LOGGING) Serial.printf("Received getPixelCount from client #%u\n", client->id());
            char response[64];
            snprintf(response, sizeof(response), "{\"status\":\"ok\",\"pixelCount\":%u}", _numPixels);
            client->text(response);
            return;
          }
          
          uint32_t id = doc["id"] | 0; // Get command ID from client
          
          if (id == 0)
          {
            // Command ID is required for non-ping commands
            client->text("{\"status\":\"error\",\"error\":\"missing_id\"}");
            return;
          }
          
          Command command;
          command.clientId = client->id();
          command.commandId = id;
          bool validCmd = true;

          if (strcmp(cmd, "setColor") == 0)
          {
            command.type = SET_COLOR;
            command.r = doc["r"] | 0;
            command.g = doc["g"] | 0;
            command.b = doc["b"] | 0;
          }
          else if (strcmp(cmd, "clear") == 0)
          {
            command.type = CLEAR;
          }
          else if (strcmp(cmd, "setPixelColor") == 0)
          {
            command.type = SET_PIXEL_COLOR;
            command.index = doc["index"] | 0;
            command.r = doc["r"] | 0;
            command.g = doc["g"] | 0;
            command.b = doc["b"] | 0;
            
            // Validate pixel index bounds
            if (command.index >= _numPixels)
            {
              char errMsg[96];
              snprintf(errMsg, sizeof(errMsg), "{\"status\":\"error\",\"error\":\"index_out_of_bounds\",\"id\":%u,\"max\":%u}", id, _numPixels - 1);
              client->text(errMsg);
              validCmd = false;
            }
          }
          else if (strcmp(cmd, "show") == 0)
          {
            command.type = SHOW;
          }
          else if (strcmp(cmd, "setBrightness") == 0)
          {
            command.type = SET_BRIGHTNESS;
            command.brightness = doc["brightness"] | 255;
          }
          else
          {
            validCmd = false;
            char errMsg[80];
            snprintf(errMsg, sizeof(errMsg), "{\"status\":\"error\",\"error\":\"unknown_cmd\",\"id\":%u}", id);
            client->text(errMsg);
          }

          if (validCmd)
          {
            if (!enqueueCommand(command))
            {
              char errMsg[80];
              snprintf(errMsg, sizeof(errMsg), "{\"status\":\"error\",\"error\":\"queue_full\",\"id\":%u}", id);
              client->text(errMsg);
            }
            // ACK with ID will be sent after processing in loop()
          }
        }
        else
        {
          client->text("{\"status\":\"error\",\"error\":\"bad_json\"}");
        }
      }
    }
  }
};