#include "RemoteReaderController.h"
#ifdef PHASE2_REMOTE_DEBUG
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <cstring>

#include "EpubReaderActivity.h"
#include "Logging.h"
#include "WifiCredentialStore.h"

namespace {
constexpr uint16_t kRemotePort = 81;
constexpr uint32_t kConnectTimeoutMs = 15000;
}  // namespace

RemoteReaderController::RemoteReaderController(EpubReaderActivity& reader) : reader_(reader) {}
RemoteReaderController::~RemoteReaderController() { stop(); }

bool RemoteReaderController::begin() {
  if (active_) return true;

  // 1) Wi-Fi STA, connect to the last known network (saved on the SD card).
  // The credential store is NOT loaded at boot (only the Wi-Fi activity loads
  // it), so load it here before reading credentials.
  WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* cred = lastSsid.empty() ? nullptr : WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    // Fall back to the first saved network if no "last connected" is recorded.
    const auto& all = WIFI_STORE.getCredentials();
    if (!all.empty()) cred = &all.front();
  }
  if (!cred) {
    status_ = "No saved Wi-Fi network";
    LOG_ERR("REMOTE", "No saved Wi-Fi credentials; connect once via File Transfer first");
    return false;
  }

  WiFi.persistent(false);  // creds are managed by WifiCredentialStore
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  LOG_INF("REMOTE", "Connecting to '%s' ...", cred->ssid.c_str());

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < kConnectTimeoutMs) {
    delay(100);
    esp_task_wdt_reset();  // blocking connect — keep the task watchdog happy
  }
  if (WiFi.status() != WL_CONNECTED) {
    status_ = "Wi-Fi connect failed";
    LOG_ERR("REMOTE", "Wi-Fi connect timed out");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  ip_ = std::string(WiFi.localIP().toString().c_str());
  LOG_INF("REMOTE", "Wi-Fi up: %s", ip_.c_str());

  // 2) mDNS so the client can reach ws://crosspoint.local:81 without the IP.
  if (MDNS.begin("crosspoint")) {
    MDNS.addService("ws", "tcp", kRemotePort);
  }

  // 3) Minimal WebSocket server.
  ws_.reset(new WebSocketsServer(kRemotePort));
  ws_->onEvent([this](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
      case WStype_CONNECTED:
        LOG_INF("REMOTE", "client %u connected", num);
        ws_->sendTXT(num, "{\"evt\":\"ready\"}");
        break;
      case WStype_TEXT:
        handleText(num, payload, length);
        break;
      case WStype_DISCONNECTED:
        LOG_INF("REMOTE", "client %u disconnected", num);
        break;
      default:
        break;
    }
  });
  ws_->begin();

  active_ = true;
  status_ = "Remote session active";
  return true;
}

void RemoteReaderController::update() {
  if (active_ && ws_) ws_->loop();
}

void RemoteReaderController::stop() {
  if (!active_) return;
  if (ws_) {
    ws_->disconnect();
    ws_.reset();
  }
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  active_ = false;
  status_ = "Remote session ended";
  LOG_INF("REMOTE", "session stopped");
}

void RemoteReaderController::handleText(uint8_t num, const uint8_t* payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) {
    ws_->sendTXT(num, "{\"evt\":\"error\",\"msg\":\"bad json\"}");
    return;
  }
  const char* cmd = doc["cmd"] | "";
  LOG_INF("REMOTE", "cmd='%s'", cmd);

  if (strcmp(cmd, "ping") == 0) {
    ws_->sendTXT(num, "{\"evt\":\"pong\"}");
  } else if (strcmp(cmd, "count") == 0) {
    const int n = reader_.remoteSentenceCount();
    JsonDocument out;
    out["evt"] = "count";
    out["n"] = n;
    String s;
    serializeJson(out, s);
    ws_->sendTXT(num, s);
  } else if (strcmp(cmd, "diag") == 0) {
    const int para = doc["para"] | 1;
    const std::string info = reader_.remoteDiag(para);
    JsonDocument out;
    out["evt"] = "diag";
    out["info"] = info;
    String s;
    serializeJson(out, s);
    ws_->sendTXT(num, s);
  } else if (strcmp(cmd, "goto") == 0) {
    // Page-follow: turn to the page containing paragraph `para` (<p> ordinal) of
    // `spine` (-1 = current spine). The headline Phase 3 sync, no highlight.
    const int spine = doc["spine"] | -1;
    const int para = doc["para"] | 0;
    const bool ok = reader_.remoteGotoParagraph(spine, para);
    JsonDocument out;
    out["evt"] = "goto";
    out["spine"] = spine;
    out["para"] = para;
    out["ok"] = ok;
    String s;
    serializeJson(out, s);
    ws_->sendTXT(num, s);
  } else if (strcmp(cmd, "highlight") == 0) {
    if (!doc["para"].isNull()) {
      // Phase 3 precise highlight: (spine, para, sent) [+ optional text].
      const int spine = doc["spine"] | -1;
      const int para = doc["para"] | 1;
      const int sent = doc["sent"] | 0;
      const bool ok = reader_.remoteHighlightParaSentence(spine, para, sent);
      JsonDocument out;
      out["evt"] = "hl";
      out["spine"] = spine;
      out["para"] = para;
      out["sent"] = sent;
      out["ok"] = ok;
      String s;
      serializeJson(out, s);
      ws_->sendTXT(num, s);
    } else {
      // Test-only page-relative ordinal (kept for x4client.py).
      const int i = doc["i"] | 0;
      const bool ok = reader_.remoteHighlightSentence(i);
      JsonDocument out;
      out["evt"] = "hl";
      out["i"] = i;
      out["n"] = reader_.remoteSentenceCount();
      out["ok"] = ok;
      String s;
      serializeJson(out, s);
      ws_->sendTXT(num, s);
    }
  } else {
    String out = String("{\"evt\":\"error\",\"msg\":\"unknown cmd: ") + cmd + "\"}";
    ws_->sendTXT(num, out);
  }
}
#endif  // PHASE2_REMOTE_DEBUG
