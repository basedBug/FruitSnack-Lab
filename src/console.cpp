// FruitSnack — command console implementation.
#include "console.h"

#include <Arduino.h>
#include <ctype.h>
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

/// The UART/USB console.  The web server has its own instance.
Console gConsole;

namespace {

constexpr size_t kMaxTokens = 10;

Console::Result failResult(const String &text) {
  Console::Result result;
  result.ok = false;
  result.text = text;
  return result;
}

Console::Result okResult(const String &text) {
  Console::Result result;
  result.ok = true;
  result.text = text;
  return result;
}

size_t splitTokens(const String &line, String *tokens, size_t max_tokens) {
  size_t count = 0;
  int index = 0;
  const int length = static_cast<int>(line.length());
  while (index < length && count < max_tokens) {
    while (index < length && isspace(static_cast<unsigned char>(line.charAt(index)))) {
      ++index;
    }
    if (index >= length) {
      break;
    }
    const int start = index;
    while (index < length && !isspace(static_cast<unsigned char>(line.charAt(index)))) {
      ++index;
    }
    tokens[count++] = line.substring(start, index);
  }
  return count;
}

String joinTokens(const String *tokens, size_t from, size_t count) {
  String out;
  for (size_t i = from; i < count; ++i) {
    if (i > from) {
      out += ' ';
    }
    out += tokens[i];
  }
  return out;
}

bool parseFloat(const String &text, float &out) {
  if (text.length() == 0 || text.length() > 31) {
    return false;
  }
  char buffer[32];
  text.toCharArray(buffer, sizeof(buffer));
  char *end = nullptr;
  const float value = strtof(buffer, &end);
  if (end == buffer || end == nullptr || *end != '\0') {
    return false;
  }
  if (isnan(value) || isinf(value)) {
    return false;
  }
  out = value;
  return true;
}

float clamp(float value, float low, float high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

const char *onOff(bool value) {
  return value ? "on" : "off";
}

/// Format an ohms value for the `sensor` command.
void fmtOhms(char *out, size_t size, float ohms) {
  if (!isfinite(ohms) || ohms <= 0.0f) {
    snprintf(out, size, "n/a");
    return;
  }
  const float kohms = ohms / 1000.0f;
  if (kohms >= 10.0f) {
    snprintf(out, size, "%.1f kOhm", kohms);
  } else {
    snprintf(out, size, "%.2f kOhm", kohms);
  }
}

}  // namespace

Console::Console() {
  graph_buf_ = new (std::nothrow) TelemetrySample[kGraphCapacity];
}

Console::~Console() {
  delete[] graph_buf_;
  graph_buf_ = nullptr;
}

bool Console::scheduleRestart(uint32_t delay_ms) {
  // Deferred restart: the command handler returns its reply first, then a
  // detached task waits `delay_ms` and reboots.  This keeps the reply from
  // being cut off by an inline ESP.restart().
  const BaseType_t created =
      xTaskCreate(
          [](void *arg) {
            const uint32_t delay = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
            vTaskDelay(pdMS_TO_TICKS(delay));
            ESP.restart();
            vTaskDelete(nullptr);
          },
          "reboot", 2048, reinterpret_cast<void *>(static_cast<uintptr_t>(delay_ms)), 1, nullptr);
  if (created != pdPASS) {
    LOG_ERROR("reboot failed: could not create the restart task");
    return false;
  }
  return true;
}

/// Load the stored configuration for a read-modify-write cycle.  A missing file
/// is fine (the defaults in `out` are what a fresh device should use), but a
/// file that is present and unreadable must not be replaced by defaults: that
/// would silently destroy the user's WiFi credentials and hostname.
bool Console::loadForWrite(AppSettings &out) const {
  if (gSettings.load(out)) {
    return true;
  }
  return !gSettings.exists();
}

bool Console::persistControlSettings(String *error) {
  AppSettings stored;
  if (!loadForWrite(stored)) {
    if (error != nullptr) *error = "config unreadable";
    return false;
  }
  const AppSettings applied = gController.getSettings();

  stored.gains = applied.gains;
  stored.setpointC = applied.setpointC;
  stored.maxTemperatureC = applied.maxTemperatureC;
  stored.outputLimitPct = applied.outputLimitPct;
  stored.sensorOffsetC = applied.sensorOffsetC;
  stored.ntcPullupOhms = applied.ntcPullupOhms;
  stored.controlPeriodMs = applied.controlPeriodMs;
  stored.pidEnabled = applied.pidEnabled;

  if (!gSettings.save(stored)) {
    if (error != nullptr) *error = "failed to save the configuration";
    return false;
  }
  return true;
}

Console::Result Console::setModeChecked(ControlMode mode, const String &ok_text) {
  // setMode() refuses a switch while a sensor fault or the overtemp latch is
  // active, so the result has to be read back instead of assumed.
  gController.setMode(mode);
  if (gController.getMode() != mode) {
    String text = "mode refused: sensor fault or overtemp latch";
    text += " (still ";
    text += controlModeName(gController.getMode());
    text += ")";
    return failResult(text);
  }
  return okResult(ok_text);
}

String Console::helpText() const {
  String out;
  out += "FruitSnack console commands:\n";
  out += "  help                       show this list\n";
  out += "  status                     show the system state\n";
  out += "  set <celsius>              set the temperature setpoint\n";
  out += "  sp                         show the setpoint\n";
  out += "  on | off                   enable / disable PID control\n";
  out += "  mode <off|pid|manual>      set the control mode\n";
  out += "  manual <0-100>             set the manual duty percent\n";
  out += "  duty                       show the current duty\n";
  out += "  pid                        show the PID gains and live terms\n";
  out += "  pid set <kp> <ki> <kd>     set the PID gains\n";
  out += "  pid save                   persist the gains\n";
  out += "  config                     show the persisted configuration\n";
  out += "  config set <key> <value>   change a config value\n";
  out += "  sensor                     show the thermistor electrical diagnostics\n";
  out += "  save                       persist the current settings\n";
  out += "  clear                      clear the graph history\n";
  out += "  graph [n]                  print the n most recent samples as CSV\n";
  out += "  info                       firmware / chip / memory information\n";
  out += "  wifi                       wifi status\n";
  out += "  wifi scan                  scan for networks\n";
  out += "  wifi reconnect             reconnect now\n";
  out += "  mdns                       show the hostname and URL\n";
  out += "  reboot                     restart the device\n";
  return out;
}

Console::Result Console::execute(const String &line) {
  String tokens[kMaxTokens];
  const size_t count = splitTokens(line, tokens, kMaxTokens);
  if (count == 0) {
    return okResult(String());
  }

  String command = tokens[0];
  command.toLowerCase();

  if (command == "help") {
    return okResult(helpText());
  }

  if (command == "status") {
    const SystemState state = gController.snapshot();
    String out;
    out += "mode: ";
    out += controlModeName(state.mode);
    out += "\nstate: ";
    out += heaterStateName(state.state);
    out += "\ntemperature: ";
    out += String(state.temperatureC, 2);
    out += " C (raw ";
    out += String(state.temperatureRawC, 2);
    out += " C)\nsetpoint: ";
    out += String(state.setpointC, 2);
    out += " C\noutput: ";
    out += String(state.outputPct, 1);
    out += " %\nduty: ";
    out += String(static_cast<long>(lroundf(state.duty)));
    out += "\npid: kp=";
    out += String(state.gains.kp, 3);
    out += " ki=";
    out += String(state.gains.ki, 3);
    out += " kd=";
    out += String(state.gains.kd, 3);
    out += "\npid terms: p=";
    out += String(state.pTerm, 2);
    out += " i=";
    out += String(state.iTerm, 2);
    out += " d=";
    out += String(state.dTerm, 2);
    out += " saturated=";
    out += state.pidSaturated ? "yes" : "no";
    out += "\nuptime: ";
    out += String(state.uptimeMs / 1000.0f, 1);
    out += " s\nsensor warning: ";
    out += state.ntcWarn[0] != '\0' ? state.ntcWarn : "none";
    out += "\nadc: ";
    out += String(static_cast<long>(lroundf(state.adcMillivolts)));
    out += " mV (raw ";
    out += String(static_cast<long>(lroundf(state.adcRaw)));
    out += ")\n";
    if (state.overtemp) {
      out += "WARNING: over temperature\n";
    }
    if (state.sensorFault) {
      out += "WARNING: sensor fault\n";
    }
    return okResult(out);
  }

  if (command == "set") {
    if (count < 2) {
      return failResult("usage: set <celsius>");
    }
    float value = 0.0f;
    if (!parseFloat(tokens[1], value)) {
      return failResult("invalid temperature: " + tokens[1]);
    }
    value = clamp(value, 0.0f, TEMPERATURE_CEILING_C);
    gController.setSetpoint(value);
    return okResult("setpoint = " + String(value, 2) + " C");
  }

  if (command == "sp") {
    return okResult("setpoint = " + String(gController.getSetpoint(), 2) + " C");
  }

  if (command == "on") {
    return setModeChecked(ControlMode::Pid, "mode = pid");
  }

  if (command == "off") {
    return setModeChecked(ControlMode::Off, "mode = off");
  }

  if (command == "mode") {
    if (count < 2) {
      return failResult("usage: mode <off|pid|manual>");
    }
    String mode = tokens[1];
    mode.toLowerCase();
    ControlMode target = ControlMode::Off;
    if (mode == "off") {
      target = ControlMode::Off;
    } else if (mode == "pid") {
      target = ControlMode::Pid;
    } else if (mode == "manual") {
      target = ControlMode::Manual;
    } else {
      return failResult("unknown mode '" + tokens[1] + "' (use off, pid or manual)");
    }
    return setModeChecked(target, "mode = " + mode);
  }

  if (command == "manual") {
    if (count < 2) {
      return failResult("usage: manual <0-100>");
    }
    float duty = 0.0f;
    if (!parseFloat(tokens[1], duty)) {
      return failResult("invalid duty: " + tokens[1]);
    }
    duty = clamp(duty, 0.0f, 100.0f);
    const Result mode_result = setModeChecked(ControlMode::Manual, String());
    if (!mode_result.ok) {
      return mode_result;
    }
    gController.setManualDuty(duty);
    return okResult("manual duty = " + String(duty, 1) + " %");
  }

  if (command == "duty") {
    const SystemState state = gController.snapshot();
    return okResult("duty = " + String(static_cast<long>(lroundf(state.duty))) + " (" +
                    String(state.outputPct, 1) + " %)");
  }

  if (command == "pid") {
    if (count == 1) {
      const SystemState state = gController.snapshot();
      String out;
      out += "gains: kp=";
      out += String(state.gains.kp, 4);
      out += " ki=";
      out += String(state.gains.ki, 4);
      out += " kd=";
      out += String(state.gains.kd, 4);
      out += "\nterms: p=";
      out += String(state.pTerm, 3);
      out += " i=";
      out += String(state.iTerm, 3);
      out += " d=";
      out += String(state.dTerm, 3);
      out += " saturated=";
      out += state.pidSaturated ? "yes" : "no";
      out += "\n";
      return okResult(out);
    }

    String sub = tokens[1];
    sub.toLowerCase();
    if (sub == "save") {
      String error;
      if (!persistControlSettings(&error)) {
        return failResult(error);
      }
      return okResult("PID gains saved");
    }
    if (sub == "set") {
      if (count < 5) {
        return failResult("usage: pid set <kp> <ki> <kd>");
      }
      float kp = 0.0f;
      float ki = 0.0f;
      float kd = 0.0f;
      if (!parseFloat(tokens[2], kp) || !parseFloat(tokens[3], ki) || !parseFloat(tokens[4], kd)) {
        return failResult("invalid gain value");
      }
      if (kp < 0.0f || ki < 0.0f || kd < 0.0f) {
        return failResult("gains must be >= 0");
      }
      PidGains gains;
      gains.kp = kp;
      gains.ki = ki;
      gains.kd = kd;
      gController.setGains(gains);
      return okResult("gains: kp=" + String(kp, 4) + " ki=" + String(ki, 4) + " kd=" + String(kd, 4));
    }
    return failResult("usage: pid | pid set <kp> <ki> <kd> | pid save");
  }

  if (command == "config") {
    if (count == 1) {
      AppSettings stored;
      gSettings.load(stored);
      String out;
      out += "ssid: ";
      out += stored.wifiSsid;
      out += "\npassword: ";
      out += (stored.wifiPassword[0] != '\0' ? "********" : "(none)");
      out += "\nhostname: ";
      out += stored.hostname;
      out += "\nkp: ";
      out += String(stored.gains.kp, 4);
      out += "\nki: ";
      out += String(stored.gains.ki, 4);
      out += "\nkd: ";
      out += String(stored.gains.kd, 4);
      out += "\nsetpoint: ";
      out += String(stored.setpointC, 2);
      out += "\nmaxtemp: ";
      out += String(stored.maxTemperatureC, 2);
      out += "\noutputlimit: ";
      out += String(stored.outputLimitPct, 1);
      out += "\nsensoroffset: ";
      out += String(stored.sensorOffsetC, 2);
      out += "\nntcpullup: ";
      out += String(stored.ntcPullupOhms, 1);
      out += "\ncontrolperiodms: ";
      out += String(stored.controlPeriodMs);
      out += "\npidenabled: ";
      out += onOff(stored.pidEnabled);
      out += "\n";
      return okResult(out);
    }

    String sub = tokens[1];
    sub.toLowerCase();
    if (sub != "set") {
      return failResult("usage: config | config set <key> <value>");
    }
    if (count < 4) {
      return failResult("usage: config set <key> <value>");
    }

    String key = tokens[2];
    key.toLowerCase();
    const String value = joinTokens(tokens, 3, count);

    AppSettings stored;
    if (!loadForWrite(stored)) {
      return failResult("config unreadable");
    }
    bool network_key = false;
    bool sensor_key = false;
    String applied_text;

    if (key == "ssid") {
      strlcpy(stored.wifiSsid, value.c_str(), sizeof(stored.wifiSsid));
      network_key = true;
      applied_text = "ssid = " + value;
    } else if (key == "password") {
      strlcpy(stored.wifiPassword, value.c_str(), sizeof(stored.wifiPassword));
      network_key = true;
      applied_text = "password = ********";
    } else if (key == "hostname") {
      strlcpy(stored.hostname, value.c_str(), sizeof(stored.hostname));
      network_key = true;
      applied_text = "hostname = " + value;
    } else {
      float number = 0.0f;
      if (!parseFloat(value, number)) {
        return failResult("invalid value for " + key + ": " + value);
      }
      if (key == "kp") {
        stored.gains.kp = number;
      } else if (key == "ki") {
        stored.gains.ki = number;
      } else if (key == "kd") {
        stored.gains.kd = number;
      } else if (key == "setpoint") {
        stored.setpointC = clamp(number, 0.0f, TEMPERATURE_CEILING_C);
      } else if (key == "maxtemp") {
        stored.maxTemperatureC = clamp(number, 0.0f, TEMPERATURE_CEILING_C);
      } else if (key == "outputlimit") {
        stored.outputLimitPct = clamp(number, 0.0f, 100.0f);
      } else if (key == "sensoroffset") {
        stored.sensorOffsetC = clamp(number, -50.0f, 50.0f);
      } else if (key == "ntcpullup" || key == "ntcpullupohms") {
        if (number <= 0.0f) return failResult("ntcpullup must be > 0");
        stored.ntcPullupOhms = number;
        sensor_key = true;
      } else {
        return failResult("unknown config key '" + key + "'");
      }
      applied_text = key + " = " + value;
    }

    if (!gSettings.save(stored)) {
      return failResult("failed to save the configuration");
    }

    if (network_key) {
      gNetwork.applySettings(stored);
    } else if (sensor_key) {
      gController.applySettings(stored);
    } else if (key == "kp" || key == "ki" || key == "kd") {
      gController.setGains(stored.gains);
    } else if (key == "setpoint") {
      gController.setSetpoint(stored.setpointC);
    } else if (key == "maxtemp") {
      gController.setMaxTemperature(stored.maxTemperatureC);
    } else if (key == "outputlimit") {
      gController.setOutputLimit(stored.outputLimitPct);
    } else if (key == "sensoroffset") {
      gController.setSensorOffset(stored.sensorOffsetC);
    }

    return okResult(applied_text + " (saved)");
  }

  if (command == "sensor") {
    if (count >= 2) {
      return failResult("usage: sensor");
    }

    const SystemState state = gController.snapshot();
    const long raw = lroundf(state.adcRaw);

    char ohms[32];
    fmtOhms(ohms, sizeof(ohms), state.ntcResistance);

    char line[160];
    String out;
    snprintf(line, sizeof(line), "node: %ld mV (raw %ld)\n",
             lroundf(state.adcMillivolts), raw);
    out += line;
    out += "thermistor: ";
    out += ohms;
    snprintf(line, sizeof(line), "\ntemperature: %.1f C\n", state.temperatureC);
    out += line;
    out += "warning: ";
    out += state.ntcWarn[0] != '\0' ? state.ntcWarn : "none";
    out += "\n";
    return okResult(out);
  }

  if (command == "save") {
    String error;
    if (!persistControlSettings(&error)) {
      return failResult(error);
    }
    return okResult("settings saved");
  }

  if (command == "clear") {
    gController.clearTelemetry();
    return okResult("graph history cleared");
  }

  if (command == "graph") {
    size_t wanted = 20;
    if (count >= 2) {
      float number = 0.0f;
      if (!parseFloat(tokens[1], number) || number < 0.0f) {
        return failResult("invalid sample count: " + tokens[1]);
      }
      wanted = static_cast<size_t>(number);
      if (wanted == 0) {
        wanted = 1;
      }
      if (wanted > kGraphCapacity) {
        wanted = kGraphCapacity;
      }
    }
    if (graph_buf_ == nullptr) {
      return failResult("graph buffer unavailable");
    }

    const size_t total = gController.copyTelemetry(graph_buf_, kGraphCapacity);
    if (total == 0) {
      return okResult("no samples");
    }
    const size_t first = (total > wanted) ? total - wanted : 0;

    String out;
    out += "t_ms,temp,sp,out\n";
    char row[96];
    for (size_t i = first; i < total; ++i) {
      snprintf(row, sizeof(row), "%lu,%.2f,%.2f,%.1f\n",
               static_cast<unsigned long>(graph_buf_[i].tMs), graph_buf_[i].temperatureC,
               graph_buf_[i].setpointC, graph_buf_[i].outputPct);
      out += row;
    }
    return okResult(out);
  }

  if (command == "info") {
    String out;
    out += "firmware: ";
    out += FW_NAME;
    out += " ";
    out += FW_VERSION;
    out += "\nbuilt: ";
    out += FW_COMPILE_DATE;
    out += "\nchip: ";
    out += ESP.getChipModel();
    out += " rev ";
    out += String(ESP.getChipRevision());
    out += "\nsdk: ";
    out += ESP.getSdkVersion();
    out += "\ncore: ";
    out += ESP.getCoreVersion();
    out += "\ncpu: ";
    out += String(ESP.getCpuFreqMHz());
    out += " MHz\nheap: free=";
    out += String(ESP.getFreeHeap());
    out += " min=";
    out += String(ESP.getMinFreeHeap());
    out += " maxalloc=";
    out += String(ESP.getMaxAllocHeap());
    out += "\nflash: ";
    out += String(ESP.getFlashChipSize());
    out += " bytes\npsram: ";
    out += String(ESP.getPsramSize());
    out += " bytes\nlittlefs: ";
    out += String(static_cast<unsigned>(gSettings.usedBytes()));
    out += " / ";
    out += String(static_cast<unsigned>(gSettings.totalBytes()));
    out += " bytes\nuptime: ";
    out += String(millis() / 1000.0f, 1);
    out += " s\n";
    return okResult(out);
  }

  if (command == "wifi") {
    if (count >= 2) {
      String sub = tokens[1];
      sub.toLowerCase();
      if (sub == "scan") {
        gNetwork.startScan([](const std::vector<WifiScanEntry> &networks) {
          if (networks.empty()) {
            gLogger.info("wifi scan: no networks found");
            return;
          }
          char line[128];
          snprintf(line, sizeof(line), "wifi scan: %u networks", static_cast<unsigned>(networks.size()));
          gLogger.info("%s", line);
          for (const WifiScanEntry &entry : networks) {
            snprintf(line, sizeof(line), "  %-32s %4d dBm  ch%u  %s", entry.ssid.c_str(),
                     static_cast<int>(entry.rssi), static_cast<unsigned>(entry.channel),
                     entry.secure ? "secured" : "open");
            gLogger.info("%s", line);
          }
        });
        return okResult("scanning for wifi networks...");
      }
      if (sub == "reconnect") {
        gNetwork.reconnect();
        return okResult("wifi reconnect requested");
      }
      return failResult("usage: wifi | wifi scan | wifi reconnect");
    }

    String out;
    out += "ssid: ";
    out += gNetwork.ssid();
    out += "\nip: ";
    out += gNetwork.ipAddress();
    out += "\nrssi: ";
    out += String(gNetwork.rssi());
    out += " dBm\nmac: ";
    out += gNetwork.macAddress();
    out += "\nstate: ";
    out += gNetwork.isConnected() ? "connected" : "disconnected";
    out += "\n";
    return okResult(out);
  }

  if (command == "mdns") {
    String out;
    out += "hostname: ";
    out += gNetwork.hostname();
    out += "\nurl: http://";
    out += gNetwork.hostname();
    out += ".local";
    if (HTTP_PORT != 80) {
      out += ":";
      out += String(HTTP_PORT);
    }
    out += "/\n";
    return okResult(out);
  }

  if (command == "reboot") {
    if (reboot_pending_) {
      return okResult("rebooting...");
    }
    if (!scheduleRestart(500)) {
      return failResult("reboot failed: could not create the restart task");
    }
    reboot_pending_ = true;
    return okResult("rebooting...");
  }

  return failResult("unknown command '" + tokens[0] + "' - type 'help' for the command list");
}
