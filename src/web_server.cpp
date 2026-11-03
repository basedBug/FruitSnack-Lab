// FruitSnack — HTTP + websocket server implementation.
#include "web_server.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "heater_controller.h"
#include "logger.h"
#include "network_manager.h"
#include "settings.h"
#include "state_json.h"

WebServer gWeb;

namespace {

constexpr size_t kMaxRequestBody = 4096;
constexpr size_t kLogFloodLimit = 20;  // records per second before throttling
constexpr uint32_t kRebootDelayMs = 400;
constexpr uint32_t kCleanupPeriodMs = 1000;
// The upstream server has a known issue with very large single-buffer
// responses under memory pressure, so keep our JSON bodies small.
constexpr size_t kMaxGraphResponseSamples = 200;
constexpr size_t kMaxLogResponseLines = 60;
constexpr size_t kMaxWsReplyChars = 4000;

/// Reported when setMode() refuses a switch because a sensor fault or the
/// overtemp latch is active.
constexpr const char *kModeRefused = "mode refused: sensor fault or overtemp latch";

/// The dashboard shell inside the LittleFS partition.
constexpr const char *kDashboardIndex = "/index.html";

/// True when the dashboard image has been flashed to the filesystem.
bool dashboard_present() { return LittleFS.exists(kDashboardIndex); }

bool ws_client_ready(AsyncWebSocketClient *client) {
  return client != nullptr && client->status() == WS_CONNECTED && client->canSend();
}

struct BodyBuf {
  size_t len;
  char data[kMaxRequestBody];
};

void appendBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  (void)index;
  (void)total;
  if (request->_tempObject == nullptr) {
    BodyBuf *buffer = static_cast<BodyBuf *>(malloc(sizeof(BodyBuf)));
    if (buffer == nullptr) {
      return;
    }
    buffer->len = 0;
    request->_tempObject = buffer;
  }
  BodyBuf *buffer = static_cast<BodyBuf *>(request->_tempObject);
  if (buffer == nullptr || len == 0) {
    return;
  }
  if (buffer->len + len >= kMaxRequestBody) {
    return;  // truncate over-long bodies instead of overflowing
  }
  memcpy(buffer->data + buffer->len, data, len);
  buffer->len += len;
}

bool parseBody(AsyncWebServerRequest *request, JsonDocument &doc) {
  BodyBuf *buffer = static_cast<BodyBuf *>(request->_tempObject);
  if (buffer == nullptr || buffer->len == 0) {
    return false;
  }
  const DeserializationError error = deserializeJson(doc, buffer->data, buffer->len);
  return !error;
}

void releaseBody(AsyncWebServerRequest *request) {
  BodyBuf *buffer = static_cast<BodyBuf *>(request->_tempObject);
  if (buffer != nullptr) {
    free(buffer);
    request->_tempObject = nullptr;
  }
}

/// Parse the request body and release it.  On a malformed body the 400 reply is
/// sent here, so every POST handler just does `if (!readJsonBody(...)) return;`.
bool readJsonBody(AsyncWebServerRequest *request, JsonDocument &doc) {
  const bool parsed = parseBody(request, doc);
  releaseBody(request);
  if (parsed) {
    return true;
  }
  JsonDocument error;
  error["ok"] = false;
  error["error"] = "invalid json";
  String out;
  serializeJson(error, out);
  request->send(400, "application/json", out.c_str());
  return false;
}

float clampValue(float value, float low, float high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

/// Map a mode name (already lower case) onto a ControlMode.
bool parseModeName(const String &text, ControlMode &out) {
  if (text == "off") {
    out = ControlMode::Off;
    return true;
  }
  if (text == "pid") {
    out = ControlMode::Pid;
    return true;
  }
  if (text == "manual") {
    out = ControlMode::Manual;
    return true;
  }
  return false;
}

/// Apply a mode and read it back. setMode() silently refuses a switch while a
/// sensor fault or the overtemp latch is active, so the read-back is what
/// decides success.
bool applyMode(ControlMode mode) {
  gController.setMode(mode);
  return gController.getMode() == mode;
}

/// Persist new PID gains: returns nullptr when they were stored, otherwise a
/// short reason for the reply. A missing file is fine (defaults plus the new
/// gains), an unreadable one is not: saving over it would destroy the stored
/// configuration.
const char *persistGains(const PidGains &gains) {
  AppSettings settings;
  if (gSettings.load(settings) || !gSettings.exists()) {
    settings.gains = gains;
    if (gSettings.save(settings)) {
      return nullptr;
    }
    LOG_WARN("failed to persist PID gains");
    return "save failed: the gains were applied but not saved";
  }
  LOG_WARN("PID gains not persisted: config.json is unreadable");
  return "config unreadable: the gains were applied but not saved";
}

void escapeJson(const char *src, char *dst, size_t capacity) {
  size_t out = 0;
  for (const char *p = src; p != nullptr && *p != '\0' && out + 7 < capacity; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"':
        dst[out++] = '\\';
        dst[out++] = '"';
        break;
      case '\\':
        dst[out++] = '\\';
        dst[out++] = '\\';
        break;
      case '\n':
        dst[out++] = '\\';
        dst[out++] = 'n';
        break;
      case '\r':
        dst[out++] = '\\';
        dst[out++] = 'r';
        break;
      case '\t':
        dst[out++] = '\\';
        dst[out++] = 't';
        break;
      default:
        if (c < 0x20) {
          const int written = snprintf(dst + out, capacity - out, "\\u%04x", static_cast<unsigned>(c));
          if (written > 0) {
            out += static_cast<size_t>(written);
          }
        } else {
          dst[out++] = static_cast<char>(c);
        }
        break;
    }
  }
  dst[out] = '\0';
}

bool scheduleRestart(uint32_t delay_ms) {
  const BaseType_t created =
      xTaskCreate(
          [](void *arg) {
            const uint32_t delay = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
            vTaskDelay(pdMS_TO_TICKS(delay));
            ESP.restart();
            vTaskDelete(nullptr);
          },
          "web-reboot", 2048, reinterpret_cast<void *>(static_cast<uintptr_t>(delay_ms)), 1, nullptr);
  if (created != pdPASS) {
    LOG_ERROR("reboot failed: could not create the web restart task");
    return false;
  }
  return true;
}

}  // namespace

/// Shown when the dashboard image is missing from the LittleFS partition.
void WebServer::sendDashboardMissing(AsyncWebServerRequest *request) {
  const bool mounted = gSettings.mounted();
  const char *title = mounted ? "Dashboard not installed"
                               : "Filesystem not mounted";
  const char *detail =
      mounted ? "The firmware is running and the API is up, but the web dashboard was "
                "not found in the device's LittleFS partition."
              : "The firmware is running, but the LittleFS partition could not be "
                "mounted. The partition table is probably not the one this firmware "
                "expects.";
  const char *fix =
      mounted ? "pio run -t uploadfs"
              : "pio run -t upload && pio run -t uploadfs";

  String page;
  page.reserve(1400);
  page += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>FruitSnack - ";
  page += title;
  page += "</title><style>"
          "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;"
          "background:#12141a;color:#e6e8ee;margin:0;min-height:100vh;display:flex;"
          "align-items:center;justify-content:center;padding:1.5rem}"
          "div{max-width:36rem;background:#1a1d26;border:1px solid #2a2e3a;"
          "border-radius:12px;padding:2rem}"
          "h1{font-size:1.25rem;margin:0 0 .75rem}"
          "p{line-height:1.65;color:#aeb4c4}"
          "code{background:#12141a;border:1px solid #2a2e3a;padding:.15rem .45rem;"
          "border-radius:4px;color:#7aa2f7}"
          "a{color:#7aa2f7}</style></head><body><div><h1>";
  page += title;
  page += "</h1><p>";
  page += detail;
  page += "</p><p>Fix it with:<br><code>";
  page += fix;
  page += "</code></p><p>The API works meanwhile: "
          "<a href=\"/api/info\">/api/info</a></p></div></body></html>";

  request->send(503, "text/html", page.c_str());
}

WebServer::WebServer() : server_(HTTP_PORT), ws_(WS_PATH) {
  log_mutex_ = xSemaphoreCreateMutex();
}

WebServer::~WebServer() {
  delete[] graph_buf_;
  graph_buf_ = nullptr;
}

void WebServer::begin() {
  if (graph_buf_ == nullptr) {
    graph_buf_ = new (std::nothrow) TelemetrySample[kGraphSamples];
  }

  ws_.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                     uint8_t *data, size_t len) { onWsEvent(server, client, type, arg, data, len); });
  server_.addHandler(&ws_);

  registerRoutes();

  server_.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setTryGzipFirst(false)  // assets are shipped uncompressed
      .setCacheControl("no-cache");

  server_.onNotFound([this](AsyncWebServerRequest *request) {
    const bool is_get = request->method() == HTTP_GET;
    if (is_get && request->url().startsWith("/api/")) {
      JsonDocument doc;
      doc["ok"] = false;
      doc["error"] = "not found";
      String out;
      serializeJson(doc, out);
      request->send(404, "application/json", out.c_str());
      return;
    }
    // Client-side routes (e.g. /settings) have no file of their own; send the
    // dashboard shell so the SPA can take over.
    if (is_get) {
      if (dashboard_present()) {
        request->send(LittleFS, kDashboardIndex, "text/html");
      } else {
        // The dashboard image is not in the filesystem partition. Sending a
        // missing file through send(FS, ...) would leave the request
        // unanswered and the library would reply 501 "Handler did not handle
        // the request", so explain the actual problem instead.
        sendDashboardMissing(request);
      }
      return;
    }
    request->send(404, "text/plain", "not found");
  });

  server_.begin();

  gLogger.addSink([this](LogLevel level, const char *text) { broadcastLog(level, text); });

  if (!gSettings.mounted()) {
    gLogger.warn(
        "LittleFS could not be mounted - the partition table is probably wrong; "
        "run: pio run -t upload");
  } else if (!dashboard_present()) {
    gLogger.warn(
        "dashboard missing from LittleFS - flash it with: pio run -t uploadfs");
  }

  gLogger.info("web server listening");
}

void WebServer::loop() {
  const uint32_t now = millis();

  // Reap timed-out/overflowing websocket clients even when nobody is actively
  // streaming (the library requires this to be called periodically).
  if (static_cast<uint32_t>(now - last_cleanup_ms_) >= kCleanupPeriodMs) {
    last_cleanup_ms_ = now;
    ws_.cleanupClients(WS_MAX_CLIENTS);
  }

  if (ws_.count() == 0) {
    return;
  }

  if (static_cast<uint32_t>(now - last_state_ms_) >= WS_BROADCAST_PERIOD_MS) {
    last_state_ms_ = now;
    broadcastState();
  }
  if (static_cast<uint32_t>(now - last_graph_ms_) >= 1000U) {
    last_graph_ms_ = now;
    pushGraph();
  }
}

size_t WebServer::clientCount() const {
  return ws_.count();
}

void WebServer::fillWifiDiag(JsonDocument &doc) {
  JsonObject diag = doc["diag"].as<JsonObject>();
  if (diag.isNull()) {
    return;
  }
  diag["rssi"] = gNetwork.rssi();
  const String ip = gNetwork.ipAddress();
  const String mac = gNetwork.macAddress();
  const String host = gNetwork.hostname();
  diag["ip"] = ip.c_str();
  diag["mac"] = mac.c_str();
  diag["hostname"] = host.c_str();
}

void WebServer::broadcastState() {
  JsonDocument doc;
  stateToJson(doc, gController.snapshot());
  fillWifiDiag(doc);

  String inner;
  inner.reserve(640);
  serializeJson(doc, inner);

  String out;
  out.reserve(inner.length() + 32U);
  out = "{\"type\":\"state\",\"data\":";
  out += inner;
  out += '}';
  if (ws_.textAll(out.c_str(), out.length()) == AsyncWebSocket::DISCARDED) {
    return;  // client queues are full; the next 200 ms tick sends fresh state
  }
}

void WebServer::broadcastLog(LogLevel level, const char *text) {
  if (text == nullptr) {
    return;
  }
  const uint32_t now = millis();

  bool notice = false;
  uint32_t suppressed = 0;
  bool drop = false;

  if (log_mutex_ != nullptr) {
    xSemaphoreTake(log_mutex_, portMAX_DELAY);
  }
  if (log_window_ms_ == 0 || static_cast<int32_t>(now - log_window_ms_) >= 1000) {
    log_window_ms_ = now;
    log_count_ = 0;
    if (log_suppressed_ > 0) {
      suppressed = log_suppressed_;
      log_suppressed_ = 0;
      notice = true;
    }
  }
  if (log_count_ >= kLogFloodLimit) {
    ++log_suppressed_;
    drop = true;
  } else {
    ++log_count_;
  }
  if (log_mutex_ != nullptr) {
    xSemaphoreGive(log_mutex_);
  }

  if (ws_.count() == 0) {
    return;
  }

  auto emit = [&](LogLevel line_level, const char *line_text) {
    char escaped[384];
    escapeJson(line_text, escaped, sizeof(escaped));
    char payload[512];
    const int written = snprintf(payload, sizeof(payload),
                                 "{\"type\":\"log\",\"data\":{\"level\":\"%s\",\"ts\":%lu,\"text\":\"%s\"}}",
                                 logLevelName(line_level), static_cast<unsigned long>(now), escaped);
    if (written > 0 && static_cast<size_t>(written) < sizeof(payload)) {
      const AsyncWebSocket::SendStatus status = ws_.textAll(payload, static_cast<size_t>(written));
      if (status == AsyncWebSocket::DISCARDED) {
        return;  // client queues are full; drop rather than stalling the logger
      }
    }
  };

  if (notice) {
    char message[128];
    snprintf(message, sizeof(message), "log flooding suppressed: %lu lines dropped",
             static_cast<unsigned long>(suppressed));
    emit(LogLevel::Warn, message);
  }
  if (!drop) {
    emit(level, text);
  }
}

void WebServer::pushGraph() {
  if (graph_buf_ == nullptr || ws_.count() == 0) {
    return;
  }

  const size_t count = gController.copyTelemetry(graph_buf_, kGraphSamples);
  if (count == 0) {
    return;
  }

  JsonDocument doc;
  doc.to<JsonArray>();
  size_t sent = 0;
  uint32_t newest = last_graph_ts_;
  for (size_t i = 0; i < count && sent < kGraphPushMax; ++i) {
    if (static_cast<int32_t>(graph_buf_[i].tMs - last_graph_ts_) > 0) {
      sampleToJson(doc, graph_buf_[i]);
      newest = graph_buf_[i].tMs;
      ++sent;
    }
  }
  if (sent == 0) {
    return;
  }

  String samples;
  samples.reserve(64U * sent);
  serializeJson(doc, samples);

  String out;
  out.reserve(samples.length() + 48U);
  out = "{\"type\":\"graph\",\"data\":{\"samples\":";
  out += samples;
  out += "}}";
  // Only mark these samples as delivered if at least one client accepted the
  // frame; otherwise the next tick retries them.
  if (ws_.textAll(out.c_str(), out.length()) != AsyncWebSocket::DISCARDED) {
    last_graph_ts_ = newest;
  }
}

void WebServer::sendWsReply(AsyncWebSocketClient *client, uint32_t id, bool ok, const String &text) {
  if (client == nullptr) {
    return;
  }
  JsonDocument doc;
  doc["type"] = "reply";
  JsonObject data = doc["data"].to<JsonObject>();
  data["id"] = id;
  data["ok"] = ok;
  data["text"] = text.c_str();

  String out;
  out.reserve(text.length() + 64U);
  serializeJson(doc, out);
  if (ws_client_ready(client)) {
    client->text(out);
  }
}

void WebServer::onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                          uint8_t *data, size_t len) {
  (void)server;
  switch (type) {
    case WS_EVT_CONNECT: {
      // `arg` points at the HTTP request, which is released immediately after
      // this callback; deliberately not stored.
      if (client == nullptr) {
        return;
      }
      JsonDocument doc;
      doc["type"] = "hello";
      JsonObject payload = doc["data"].to<JsonObject>();
      payload["fw"] = FW_NAME;
      payload["version"] = FW_VERSION;
      payload["protocol"] = 1;
      payload["clients"] = ws_.count();
      String out;
      serializeJson(doc, out);
      client->text(out);
      // Start the graph stream at "now" so a fresh client does not receive the
      // whole buffered history in one frame.
      last_graph_ts_ = millis();
      break;
    }
    case WS_EVT_DATA: {
      AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
      if (client == nullptr || info == nullptr) {
        return;
      }
      // Only handle a complete, unfragmented text message.
      if (!(info->final && info->index == 0 && info->len == len)) {
        return;
      }
      if (info->message_opcode != WS_TEXT) {
        return;
      }
      const String message(reinterpret_cast<const char *>(data), static_cast<unsigned int>(len));
      handleWsMessage(client, message);
      break;
    }
    case WS_EVT_DISCONNECT:
      // `arg` is NULL for disconnect events; no close code is provided.
      break;
    case WS_EVT_PING:
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
    default:
      break;
  }
}

void WebServer::handleWsMessage(AsyncWebSocketClient *client, const String &message) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, message.c_str(), message.length());
  if (error) {
    sendWsReply(client, 0, false, String("invalid json: ") + error.c_str());
    return;
  }

  String type = doc["type"] | "";
  type.toLowerCase();
  const uint32_t id = doc["data"]["id"] | 0U;

  if (type == "ping") {
    JsonDocument pong;
    pong["type"] = "pong";
    JsonObject payload = pong["data"].to<JsonObject>();
    payload["id"] = id;
    String out;
    serializeJson(pong, out);
    if (ws_client_ready(client)) {
      client->text(out);
    }
    return;
  }

  if (type == "cmd") {
    const String command = doc["data"]["command"] | "";
    const Console::Result result = console_.execute(command);
    String reply_text = result.text;
    if (reply_text.length() > kMaxWsReplyChars) {
      reply_text = reply_text.substring(0, kMaxWsReplyChars);
      reply_text += "\n[truncated]";
    }
    sendWsReply(client, id, result.ok, reply_text);
    return;
  }

  if (type == "setpoint") {
    if (!doc["data"]["value"].is<float>()) {
      sendWsReply(client, id, false, "missing value");
      return;
    }
    const float value = clampValue(doc["data"]["value"].as<float>(), 0.0f, TEMPERATURE_CEILING_C);
    gController.setSetpoint(value);
    sendWsReply(client, id, true, "setpoint = " + String(value, 2) + " C");
    return;
  }

  if (type == "mode") {
    if (!doc["data"]["mode"].is<const char *>()) {
      sendWsReply(client, id, false, "missing mode");
      return;
    }
    String mode = doc["data"]["mode"].as<const char *>();
    mode.toLowerCase();
    ControlMode target = ControlMode::Off;
    if (!parseModeName(mode, target)) {
      sendWsReply(client, id, false, "unknown mode '" + mode + "'");
      return;
    }
    if (!applyMode(target)) {
      sendWsReply(client, id, false, kModeRefused);
      return;
    }
    sendWsReply(client, id, true, "mode = " + mode);
    return;
  }

  if (type == "manual") {
    if (!doc["data"]["duty"].is<float>()) {
      sendWsReply(client, id, false, "missing duty");
      return;
    }
    const float duty = clampValue(doc["data"]["duty"].as<float>(), 0.0f, 100.0f);
    gController.setManualDuty(duty);
    sendWsReply(client, id, true, "manual duty = " + String(duty, 1) + " %");
    return;
  }

  if (type == "pid") {
    PidGains gains = gController.getGains();
    bool any = false;
    if (doc["data"]["kp"].is<float>()) {
      gains.kp = doc["data"]["kp"].as<float>();
      any = true;
    }
    if (doc["data"]["ki"].is<float>()) {
      gains.ki = doc["data"]["ki"].as<float>();
      any = true;
    }
    if (doc["data"]["kd"].is<float>()) {
      gains.kd = doc["data"]["kd"].as<float>();
      any = true;
    }
    if (!any) {
      sendWsReply(client, id, false, "missing gains");
      return;
    }
    if (gains.kp < 0.0f || gains.ki < 0.0f || gains.kd < 0.0f) {
      sendWsReply(client, id, false, "gains must be >= 0");
      return;
    }
    gController.setGains(gains);
    const char *persist_error = persistGains(gains);
    if (persist_error != nullptr) {
      sendWsReply(client, id, true, persist_error);
      return;
    }
    sendWsReply(client, id, true, "gains updated");
    return;
  }

  sendWsReply(client, id, false, "unknown message type");
}

void WebServer::registerRoutes() {
  // ------------------------------------------------------------- GET state
  server_.on("/api/state", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    stateToJson(doc, gController.snapshot());
    fillWifiDiag(doc);
    String out;
    out.reserve(640);
    serializeJson(doc, out);
    request->send(200, "application/json", out.c_str());
  });

  // ------------------------------------------------------------- GET graph
  server_.on("/api/graph", HTTP_GET, [this](AsyncWebServerRequest *request) {
    size_t limit = kMaxGraphResponseSamples;
    if (request->hasParam("limit")) {
      const AsyncWebParameter *param = request->getParam("limit");
      if (param != nullptr) {
        const long requested = param->value().toInt();
        if (requested > 0) {
          limit = static_cast<size_t>(requested < static_cast<long>(kMaxGraphResponseSamples)
                                          ? requested
                                          : kMaxGraphResponseSamples);
        }
      }
    }

    JsonDocument doc;
    doc.to<JsonArray>();
    if (graph_buf_ != nullptr) {
      const size_t count = gController.copyTelemetry(graph_buf_, limit);
      for (size_t i = 0; i < count; ++i) {
        sampleToJson(doc, graph_buf_[i]);
      }
    }

    String samples;
    serializeJson(doc, samples);
    String body;
    body.reserve(samples.length() + 16U);
    body = "{\"samples\":";
    body += samples;
    body += '}';
    request->send(200, "application/json", body.c_str());
  });

  // ------------------------------------------------------------- GET config
  server_.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
    AppSettings settings;
    gSettings.load(settings);  // defaults when no file exists
    JsonDocument doc;
    settingsToJson(doc, settings);
    doc["wifiPassword"] = "********";  // never leak the stored secret
    String out;
    out.reserve(640);
    serializeJson(doc, out);
    request->send(200, "application/json", out.c_str());
  });

  // ------------------------------------------------------------ POST config
  server_.on(
      "/api/config", HTTP_POST,
      [this](AsyncWebServerRequest *request) {
        JsonDocument doc;
        if (!readJsonBody(request, doc)) {
          return;
        }

        AppSettings settings;
        // A present but unreadable file must not be replaced by defaults: that
        // would destroy the stored WiFi credentials and hostname.
        if (!gSettings.load(settings) && gSettings.exists()) {
          JsonDocument error;
          error["ok"] = false;
          error["error"] = "config unreadable";
          String out;
          serializeJson(error, out);
          request->send(500, "application/json", out.c_str());
          return;
        }
        applyJsonToSettings(doc, settings);
        const bool saved = gSettings.save(settings);
        // Only touch the live state once the configuration is on disk: a failed
        // save must not leave the running system on settings that will vanish
        // at the next reboot.
        if (saved) {
          gController.applySettings(settings);
          gNetwork.applySettings(settings);
        }

        JsonDocument reply;
        reply["ok"] = saved;
        if (!saved) {
          reply["error"] = "failed to save configuration";
        }
        String out;
        serializeJson(reply, out);
        request->send(saved ? 200 : 500, "application/json", out.c_str());
      },
      nullptr, appendBody);

  // ---------------------------------------------------------- POST setpoint
  server_.on(
      "/api/setpoint", HTTP_POST,
      [this](AsyncWebServerRequest *request) {
        JsonDocument doc;
        if (!readJsonBody(request, doc)) {
          return;
        }

        if (!doc["value"].is<float>()) {
          JsonDocument error;
          error["ok"] = false;
          error["error"] = "missing value";
          String out;
          serializeJson(error, out);
          request->send(400, "application/json", out.c_str());
          return;
        }
        const float value = clampValue(doc["value"].as<float>(), 0.0f, TEMPERATURE_CEILING_C);
        gController.setSetpoint(value);

        JsonDocument reply;
        reply["ok"] = true;
        reply["value"] = value;
        String out;
        serializeJson(reply, out);
        request->send(200, "application/json", out.c_str());
      },
      nullptr, appendBody);

  // -------------------------------------------------------------- POST mode
  server_.on(
      "/api/mode", HTTP_POST,
      [this](AsyncWebServerRequest *request) {
        JsonDocument doc;
        if (!readJsonBody(request, doc)) {
          return;
        }

        if (!doc["mode"].is<const char *>()) {
          JsonDocument error;
          error["ok"] = false;
          error["error"] = "missing mode";
          String out;
          serializeJson(error, out);
          request->send(400, "application/json", out.c_str());
          return;
        }
        String mode = doc["mode"].as<const char *>();
        mode.toLowerCase();
        ControlMode target = ControlMode::Off;
        const bool known = parseModeName(mode, target);
        // A refused mode change (sensor fault / overtemp latch) is a failure,
        // not a success: read the mode back and report honestly.
        const bool ok = known && applyMode(target);

        JsonDocument reply;
        reply["ok"] = ok;
        if (!known) {
          reply["error"] = "unknown mode";
        } else if (!ok) {
          reply["error"] = kModeRefused;
        }
        String out;
        serializeJson(reply, out);
        request->send(ok ? 200 : 400, "application/json", out.c_str());
      },
      nullptr, appendBody);

  // ------------------------------------------------------------ POST manual
  server_.on(
      "/api/manual", HTTP_POST,
      [this](AsyncWebServerRequest *request) {
        JsonDocument doc;
        if (!readJsonBody(request, doc)) {
          return;
        }

        if (!doc["duty"].is<float>()) {
          JsonDocument error;
          error["ok"] = false;
          error["error"] = "missing duty";
          String out;
          serializeJson(error, out);
          request->send(400, "application/json", out.c_str());
          return;
        }
        gController.setManualDuty(clampValue(doc["duty"].as<float>(), 0.0f, 100.0f));

        JsonDocument reply;
        reply["ok"] = true;
        String out;
        serializeJson(reply, out);
        request->send(200, "application/json", out.c_str());
      },
      nullptr, appendBody);

  // --------------------------------------------------------------- POST pid
  server_.on(
      "/api/pid", HTTP_POST,
      [this](AsyncWebServerRequest *request) {
        JsonDocument doc;
        if (!readJsonBody(request, doc)) {
          return;
        }

        PidGains gains = gController.getGains();
        bool any = false;
        if (doc["kp"].is<float>()) {
          gains.kp = doc["kp"].as<float>();
          any = true;
        }
        if (doc["ki"].is<float>()) {
          gains.ki = doc["ki"].as<float>();
          any = true;
        }
        if (doc["kd"].is<float>()) {
          gains.kd = doc["kd"].as<float>();
          any = true;
        }
        if (!any || gains.kp < 0.0f || gains.ki < 0.0f || gains.kd < 0.0f) {
          JsonDocument error;
          error["ok"] = false;
          error["error"] = "invalid gains";
          String out;
          serializeJson(error, out);
          request->send(400, "application/json", out.c_str());
          return;
        }

        gController.setGains(gains);
        const char *persist_error = persistGains(gains);

        JsonDocument reply;
        reply["ok"] = true;
        if (persist_error != nullptr) {
          reply["persisted"] = false;
          reply["error"] = persist_error;
        }
        String out;
        serializeJson(reply, out);
        request->send(200, "application/json", out.c_str());
      },
      nullptr, appendBody);

  // -------------------------------------------------------------- GET info
  server_.on("/api/info", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["firmware"] = FW_NAME;
    doc["version"] = FW_VERSION;
    doc["chip"] = ESP.getChipModel();
    doc["sdk"] = ESP.getSdkVersion();
    doc["core"] = ESP.getCoreVersion();
    doc["heapSize"] = ESP.getHeapSize();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["flashSize"] = ESP.getFlashChipSize();
    doc["psramSize"] = ESP.getPsramSize();
    doc["littlefsTotal"] = static_cast<uint32_t>(gSettings.totalBytes());
    doc["littlefsUsed"] = static_cast<uint32_t>(gSettings.usedBytes());
    doc["uptime"] = millis();
    const String ip = gNetwork.ipAddress();
    const String hostname = gNetwork.hostname();
    doc["ip"] = ip.c_str();
    doc["rssi"] = gNetwork.rssi();
    doc["hostname"] = hostname.c_str();
    // Lets a remote check tell "server broken" from "dashboard not flashed".
    doc["dashboard"] = dashboard_present();
    String out;
    out.reserve(640);
    serializeJson(doc, out);
    request->send(200, "application/json", out.c_str());
  });

  // -------------------------------------------------------------- GET logs
  server_.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest *request) {
    size_t limit = kMaxLogResponseLines;
    if (request->hasParam("limit")) {
      const AsyncWebParameter *param = request->getParam("limit");
      if (param != nullptr) {
        const long requested = param->value().toInt();
        if (requested > 0) {
          limit = static_cast<size_t>(requested < static_cast<long>(kMaxLogResponseLines)
                                          ? requested
                                          : kMaxLogResponseLines);
        }
      }
    }
    if (limit == 0) {
      limit = 1;
    }

    LogLine *lines = new (std::nothrow) LogLine[limit];
    if (lines == nullptr) {
      request->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
      return;
    }
    const size_t count = gLogger.recent(limit, lines);

    JsonDocument doc;
    JsonArray array = doc["lines"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
      JsonObject entry = array.add<JsonObject>();
      entry["level"] = logLevelName(lines[i].level);
      entry["ts"] = lines[i].tsMs;
      entry["text"] = lines[i].text;
    }
    delete[] lines;
    String out;
    out.reserve(count * 96U + 32U);
    serializeJson(doc, out);
    request->send(200, "application/json", out.c_str());
  });

  // ------------------------------------------------------------ POST reboot
  server_.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest *request) {
    // Schedule first so a task-creation failure can be reported honestly
    // instead of answering ok:true for a reboot that will never happen.
    if (!reboot_pending_) {
      reboot_pending_ = scheduleRestart(kRebootDelayMs);
    }
    if (!reboot_pending_) {
      request->send(500, "application/json",
                    "{\"ok\":false,\"error\":\"reboot task could not be created\"}");
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });
}
