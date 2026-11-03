// FruitSnack — asynchronous HTTP + websocket server.
//
// Serves the static dashboard from LittleFS, exposes the REST API and pushes
// state / graph / log events over the websocket at /ws.  Every log record is
// mirrored to connected browsers through a Logger sink.
//
// The module owns its singleton (gWeb) and reaches the control core, the
// settings store and the network manager through their singletons.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "app_state.h"
#include "console.h"
#include "logger.h"

class WebServer {
 public:
  WebServer();
  ~WebServer();

  WebServer(const WebServer &) = delete;
  WebServer &operator=(const WebServer &) = delete;

  void begin();
  void loop();

  /// Force a state push now.
  void broadcastState();

  /// Push one log record to every websocket client (throttled).
  void broadcastLog(LogLevel level, const char *text);

  size_t clientCount() const;

 private:
  static constexpr size_t kGraphSamples = 600;
  static constexpr size_t kGraphPushMax = 60;

  void registerRoutes();
  void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                 uint8_t *data, size_t len);
  void handleWsMessage(AsyncWebSocketClient *client, const String &message);
  void sendWsReply(AsyncWebSocketClient *client, uint32_t id, bool ok, const String &text);
  void sendDashboardMissing(AsyncWebServerRequest *request);
  void pushGraph();
  void fillWifiDiag(JsonDocument &doc);

  AsyncWebServer server_;
  AsyncWebSocket ws_;
  /// Own console instance: the websocket `cmd` path must not share the UART
  /// console's graph buffer, or two concurrent `graph` dumps would interleave.
  Console console_;

  TelemetrySample *graph_buf_ = nullptr;
  uint32_t last_state_ms_ = 0;
  uint32_t last_graph_ms_ = 0;
  uint32_t last_cleanup_ms_ = 0;
  volatile uint32_t last_graph_ts_ = 0;

  SemaphoreHandle_t log_mutex_ = nullptr;
  uint32_t log_window_ms_ = 0;
  uint32_t log_count_ = 0;
  uint32_t log_suppressed_ = 0;
  bool reboot_pending_ = false;
};

/// The one web server.
extern WebServer gWeb;
