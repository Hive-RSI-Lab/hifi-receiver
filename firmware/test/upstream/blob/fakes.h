// Host stand-ins for what upstream's components/spotify/Shim.cpp and
// cspot's LoginBlob.cpp use from ESP-IDF, bell and the rest of cspot, so
// test_up_blob.py can build both, as patched, under ThreadSanitizer.
// Every header Shim.cpp and LoginBlob.cpp include (but LoginBlob.h,
// ConstantParameters.h and cJSON.h, which are upstream's own) is a one-line
// file that includes this one.
//
// The Crypto here is not cryptography: it is the identity, made so that a
// login blob the harness writes in the clear decodes back to what it wrote
// (see aesECBdecrypt). What is under test is who reads and writes the
// LoginBlob, from which thread.
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "cJSON.h"

// ---- logging ---------------------------------------------------------------
#define CSPOT_LOG(level, ...) ((void)0)
#define BELL_LOG(level, ...) ((void)0)
#define BELL_SLEEP_MS(ms) usleep((ms) * 1000)
namespace bell {
inline void setDefaultLogger() {}
inline void enableTimestampLogging(bool) {}
}  // namespace bell

// ---- ESP-IDF: errors, timers, NVS, config -------------------------------------
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTPD_INVALID_REQ 0xb007
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
typedef struct fake_timer* TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
struct fake_timer {
  void* id;
  TimerCallbackFunction_t cb;
};
inline TimerHandle_t xTimerCreate(const char*, unsigned, int, void* id, TimerCallbackFunction_t cb) {
  return new fake_timer{id, cb};
}
inline void* pvTimerGetTimerID(TimerHandle_t t) { return t->id; }
inline int xTimerStart(TimerHandle_t t, unsigned) { t->cb(t); return 1; }
inline int xTimerDelete(TimerHandle_t t, unsigned) { delete t; return 1; }

#define NVS_DEFAULT_PART_NAME "nvs"
typedef enum { NVS_TYPE_STR = 0x21 } nvs_type_t;
inline void* get_nvs_value_alloc_for_partition(const char*, const char*, nvs_type_t, const char*, size_t*) {
  return NULL;
}
inline esp_err_t store_nvs_value_len_for_partition(const char*, const char*, nvs_type_t, const char*, const void*,
                                                   size_t) {
  return ESP_OK;
}
// the speaker's settings: zeroconf on, as on the board out of the box
inline cJSON* config_alloc_get_cjson(const char*) {
  cJSON* c = cJSON_CreateObject();
  cJSON_AddNumberToObject(c, "zeroConf", 1);
  return c;
}
inline void config_set_cjson_str_and_free(const char*, cJSON* c) { cJSON_Delete(c); }

// ---- ESP-IDF's HTTP server: the harness plays it ------------------------------
typedef void* httpd_handle_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;
typedef struct httpd_req {
  size_t content_len;
  std::string body;  // what httpd_req_recv hands out
  size_t sent;       // how much of it is gone
  std::string reply;
} httpd_req_t;
typedef struct httpd_uri {
  const char* uri;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t* r);
  void* user_ctx;
} httpd_uri_t;
#define HTTPD_SOCK_ERR_TIMEOUT -3
inline int httpd_req_recv(httpd_req_t* r, char* buf, size_t len) {
  size_t n = std::min(len, r->body.size() - r->sent);
  memcpy(buf, r->body.data() + r->sent, n);
  r->sent += n;
  return (int)n;
}
inline esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
inline esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len) {
  r->reply.assign(buf, len);
  return ESP_OK;
}
// the zeroconf handlers, once the cspot task has registered them
namespace fake {
inline std::mutex mu;
inline std::condition_variable cv;
inline esp_err_t (*get)(httpd_req_t*) = nullptr;
inline esp_err_t (*post)(httpd_req_t*) = nullptr;
}  // namespace fake
inline esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* u) {
  std::lock_guard l(fake::mu);
  (u->method == HTTP_GET ? fake::get : fake::post) = u->handler;
  fake::cv.notify_all();
  return ESP_OK;
}

// ---- bell --------------------------------------------------------------------
namespace bell {
class Task {
 public:
  Task(std::string, int, int, int) {}
  virtual ~Task() = default;
  bool startTask() {
    std::thread([this] { runTask(); }).detach();
    return true;
  }
  virtual void runTask() = 0;
};
class WrappedSemaphore {
  std::mutex m;
  std::condition_variable c;
  int count = 0;

 public:
  WrappedSemaphore(int = 1) {}
  int wait() {
    std::unique_lock l(m);
    c.wait(l, [this] { return count > 0; });
    count--;
    return 0;
  }
  void give() {
    std::lock_guard l(m);
    count++;
    c.notify_one();
  }
};
class MDNSService {
 public:
  static std::unique_ptr<MDNSService> registerService(const std::string&, const std::string&, const std::string&,
                                                      const std::string&, int,
                                                      const std::map<std::string, std::string>&) {
    return nullptr;
  }
};
}  // namespace bell

#include "fake_crypto.h"

// ---- nanopb's AuthenticationType (LoginBlob::loadUserPass) --------------------
typedef enum { AuthenticationType_AUTHENTICATION_USER_PASS = 0 } AuthenticationType;

// ---- the rest of cspot: a session that logs in with what it was given ---------
#include "LoginBlob.h"

typedef enum { AudioFormat_OGG_VORBIS_96, AudioFormat_OGG_VORBIS_160, AudioFormat_OGG_VORBIS_320 } AudioFormat;

namespace fake {
// what the harness learns from the cspot task (all under mu)
inline int connects = 0;       // calls to connectWithRandomAp
inline bool logged_in = false; // authenticate has read its login
inline std::string ctx_user;   // the account the session was made for
inline std::string auth_user;  // the account authenticate read
inline std::vector<uint8_t> auth_data;
}  // namespace fake

namespace cspot {
struct TrackInfo {
  std::string name, album, artist, imageUrl;
  int duration = 0;
};
class MercurySession {
 public:
  std::string ctxUser;
  // the board's resolves an access point and opens a TLS connection: long
  // enough for a zeroconf POST to land. Here: tell the harness it may send
  // one now, and take a while. A second session never gets this far.
  void connectWithRandomAp() {
    std::unique_lock l(fake::mu);
    if (++fake::connects > 1) {
      l.unlock();
      for (;;) pause();
    }
    fake::ctx_user = ctxUser;
    fake::cv.notify_all();
    l.unlock();
    usleep(50 * 1000);
  }
  // as cspot's Session::authenticate: read the login (and keep it, to log in
  // again after a drop) while the access point answers; with no account
  // server here the login is refused (empty authData)
  std::vector<uint8_t> authenticate(std::shared_ptr<LoginBlob> blob) {
    authBlob = blob;
    usleep(150 * 1000);  // the POST lands about now
    std::string user = blob->username;
    std::vector<uint8_t> data = blob->authData;
    int type = blob->authType;
    (void)type;
    std::lock_guard l(fake::mu);
    fake::auth_user = user;
    fake::auth_data = data;
    fake::logged_in = true;
    fake::cv.notify_all();
    return {};
  }
  void startTask() {}
  void handlePacket() {}
  std::shared_ptr<LoginBlob> authBlob;
};
struct Config {
  std::string deviceId, deviceName, username, clientId, clientSecret;
  std::vector<uint8_t> authData;
  int volume = 0;
  AudioFormat audioFormat = AudioFormat_OGG_VORBIS_160;
};
class Context {
 public:
  Config config;
  std::shared_ptr<MercurySession> session;
  // as cspot's: read the login into the session's config
  static std::shared_ptr<Context> createFromBlob(std::shared_ptr<LoginBlob> blob) {
    auto ctx = std::make_shared<Context>();
    ctx->session = std::make_shared<MercurySession>();
    ctx->config.deviceId = blob->getDeviceId();
    ctx->config.deviceName = blob->getDeviceName();
    ctx->config.authData = blob->authData;
    ctx->config.username = blob->getUserName();
    ctx->session->ctxUser = ctx->config.username;
    return ctx;
  }
  std::string getCredentialsJson() { return "{}"; }
};
class TrackPlayer {
 public:
  void setDataCallback(std::function<size_t(uint8_t*, size_t, std::string_view)>) {}
};
class SpircHandler {
 public:
  enum class EventType { PLAY_PAUSE, VOLUME, TRACK_INFO, DISC, NEXT, PREV, SEEK, DEPLETED, FLUSH, PLAYBACK_START };
  struct Event {
    EventType eventType;
    std::variant<TrackInfo, int, bool> data;
  };
  SpircHandler(std::shared_ptr<Context>) {}
  TrackPlayer* getTrackPlayer() { return &player; }
  void setEventHandler(std::function<void(std::unique_ptr<Event>)>) {}
  void setRemoteVolume(int) {}
  void nextSong() {}
  void previousSong() {}
  void setPause(bool) {}
  void disconnect() {}
  void notifyAudioReachedPlayback(const std::string&) {}
  void notifyAudioEnded() {}
  void updatePositionMs(uint32_t) {}

 private:
  TrackPlayer player;
};
}  // namespace cspot
