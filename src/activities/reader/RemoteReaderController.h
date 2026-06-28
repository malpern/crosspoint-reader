#pragma once
#ifdef PHASE2_REMOTE_DEBUG
#include <memory>
#include <string>

class WebSocketsServer;
class EpubReaderActivity;

// Phase 2 (debug): brings Wi-Fi up *inside the reader* and runs a minimal
// WebSocket command server on port 81, so an external client (the phone, or a
// laptop test client) can drive the reader. The stock reader has no networking,
// so this is the core new capability. Owned by EpubReaderActivity; created when
// a remote session starts, destroyed when it ends.
class RemoteReaderController {
 public:
  explicit RemoteReaderController(EpubReaderActivity& reader);
  ~RemoteReaderController();

  bool begin();                 // STA connect (last known network) + WS server on :81 + mDNS
  void update();                // pump the WS server; call each reader loop()
  void stop();                  // stop WS + disconnect Wi-Fi (no reboot)
  bool isActive() const { return active_; }
  const std::string& ip() const { return ip_; }
  const std::string& status() const { return status_; }

 private:
  void handleText(uint8_t num, const uint8_t* payload, size_t length);

  EpubReaderActivity& reader_;
  std::unique_ptr<WebSocketsServer> ws_;
  bool active_ = false;
  std::string ip_;
  std::string status_ = "idle";
};
#endif  // PHASE2_REMOTE_DEBUG
