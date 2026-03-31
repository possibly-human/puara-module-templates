/**
 * ossia_shim.h — small libossia-style send shim for ESP32 / Arduino + WiFiUDP
 *
 * This is not a full libossia port. It models only the send-side shape used in
 * this benchmark:
 *   - long-lived parameter objects identified by OSC path
 *   - values pushed through the parameter object
 *   - one OSC packet sent immediately per push
 *
 * What matches libossia :
 *   - parameters are created once and reused
 *   - push_value / push_value_list serialise and send directly from the
 *     parameter object
 *   - multi-address modes use one parameter per address
 *   - bundle sends go through one device-owned helper
 *
 * What this shim leaves out:
 *   - no full node tree, protocol layer, callbacks, or receive-side API
 *   - only int32 and int32-list payloads
 *   - list sends use arrays provided by the caller, so sending does not allocate
 *   - fixed stack buffers are sized for this benchmark's payload limits
 *   - parameter types are recorded for clarity, but the shim does not check them at runtime
 *
 * Interpret results as libossia-style direct parameter-push and bundle-send
 * overhead adapted to ESP32 WiFiUDP, not as a benchmark of the full libossia
 * graph, protocol stack, or value system.
 *
 * Value types:
 *   OSSIA_INT  — single int32, comparable to ossia::val_type::INT
 *   OSSIA_LIST — variable-length int32 array, comparable to a homogeneous
 *                integer ossia::value_list for this benchmark
 *
 * API:
 *   OssiaDevice   device(udp, ip, port)
 *   OssiaParam*   device.make_parameter("/path", OSSIA_INT | OSSIA_LIST)
 *   void          param.push_value(int32_t v)
 *   void          param.push_value_list(const int32_t* vals, int n)
 *   bool          device.push_bundle(params, vals, n_vals, n_msgs)
 */

#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>
#include <string>
#include <cstring>

// ── Value types (mirrors ossia::val_type) ─────────────────────────────────────
enum ossia_val_type : uint8_t {
  OSSIA_INT  = 0,
  OSSIA_LIST = 1,  // variable-length list — maps to ossia::value_list
};

// ── OSC wire helpers ──────────────────────────────────────────────────────────
static inline size_t ossia_pad4(size_t n) { return (n + 3) & ~3u; }

static inline size_t ossia_write_padded(uint8_t* dst, const char* s) {
  size_t len    = strlen(s) + 1;
  size_t padded = ossia_pad4(len);
  memcpy(dst, s, len);
  memset(dst + len, 0, padded - len);
  return padded;
}

static inline void ossia_write_int32(uint8_t* dst, int32_t v) {
  dst[0] = (uint8_t)(v >> 24);
  dst[1] = (uint8_t)(v >> 16);
  dst[2] = (uint8_t)(v >>  8);
  dst[3] = (uint8_t)(v      );
}

// ── OssiaParam ────────────────────────────────────────────────────────────────
// Represents the send-side part of a parameter node. 
class OssiaParam {
public:
  OssiaParam() = default;

  // Called by OssiaDevice::make_parameter.
  void _init(WiFiUDP& udp, const char* ip, int port, const char* path) {
    _udp  = &udp;
    _ip   = ip;   
    _port = port;
    _path = path;
    _path_padded_len = ossia_pad4(strlen(path) + 1);
  }

  /**
   * push_value — send a single int32.
   * Comparable to pushing an integer ossia::value through a parameter.
   */
  void push_value(int32_t v) {
    if (!_udp) return;
    // Fixed stack buffer: enough for the benchmark's short paths and single
    // int payload. This keeps the hot path allocation-free.
    // Wire: padded_path + ",i\0\0" (4B) + int32 (4B)
    const size_t total = _path_padded_len + 4 + 4;
    uint8_t buf[128];
    if (total > sizeof(buf)) return;
    uint8_t* p = buf;
    p += ossia_write_padded(p, _path.c_str());
    const char tag[] = {',','i',0,0};
    memcpy(p, tag, 4); p += 4;
    ossia_write_int32(p, v);
    _udp->beginPacket(_ip.c_str(), _port);
    _udp->write(buf, total);
    _udp->endPacket();
  }

  /**
   * push_value_list — send a variable-length list of int32s in one packet.
   *
   * Comparable to pushing an integer value_list through one
   * parameter. Real libossia uses a richer variant-based value container;
   * here we accept a plain int32 array to stay allocation-free on the hot path.
   *
   * The type tag is built dynamically on the stack: ",iii..." with n 'i's,
   * padded to a multiple of 4 bytes. Same one-pass serialisation as push_value.
   */
  void push_value_list(const int32_t* vals, int n) {
    // The benchmark never exceeds 32 int32 arguments in one OSSIA list push.
    // The bound is enforced here so the fixed local buffers stay safe.
    if (!_udp || n <= 0 || n > 32) return;

    // Build type tag: ',' + n×'i' + null + padding
    char tag_str[40];   // max: ',' + 32×'i' + '\0' = 34 chars, padded to 36
    tag_str[0] = ',';
    for (int i = 0; i < n; ++i) tag_str[1 + i] = 'i';
    tag_str[1 + n] = '\0';
    size_t tag_padded = ossia_pad4((size_t)(n + 2)); // comma + n chars + null

    const size_t total = _path_padded_len + tag_padded + (size_t)n * 4;
    uint8_t buf[256];
    if (total > sizeof(buf)) return;

    uint8_t* p = buf;
    p += ossia_write_padded(p, _path.c_str());
    // Write tag with padding
    size_t tag_raw = (size_t)(n + 2);
    memcpy(p, tag_str, tag_raw);
    memset(p + tag_raw, 0, tag_padded - tag_raw);
    p += tag_padded;
    // Write int32 args big-endian
    for (int i = 0; i < n; ++i) {
      ossia_write_int32(p, vals[i]);
      p += 4;
    }

    _udp->beginPacket(_ip.c_str(), _port);
    _udp->write(buf, total);
    _udp->endPacket();
  }

  const char* path_c_str() const { return _path.c_str(); }
  size_t path_padded_len() const { return _path_padded_len; }

private:
  WiFiUDP*     _udp  = nullptr;
  std::string  _ip;
  int          _port = 0;
  std::string  _path;
  size_t       _path_padded_len = 0;
};

// ── OssiaDevice ───────────────────────────────────────────────────────────────
// Lightweight send-side stand-in for ossia::net::generic_device. It owns the
// transport target and vends parameter objects, but it does not
// implement a full node graph, protocol stack, negotiation, or callbacks.

class OssiaDevice {
public:
  OssiaDevice(WiFiUDP& udp, const char* ip, int port)
    : _udp(udp), _ip(ip), _port(port) {}

  /**
   * make_parameter — mirrors find_or_create_node() + create_parameter().
   * Allocates a parameter on the heap.
   * Caller takes ownership via the returned pointer.
   */
  OssiaParam* make_parameter(const char* path, ossia_val_type type) {
    (void)type;  
    OssiaParam* param = new OssiaParam();
    param->_init(_udp, _ip.c_str(), _port, path);
    return param;
  }

  /**
   * push_bundle — send multiple parameter values as one OSC bundle packet.
   *
   * This models libossia's protocol-level batching more closely than repeated
   * direct parameter pushes. Each entry uses the target parameter's path and
   * a caller-supplied int32 list payload. 
   */
  bool push_bundle(OssiaParam* const* params, const int32_t* vals, int nVals, int nMsgs) {
    if (!params || !vals || nVals <= 0 || nVals > 32 || nMsgs <= 0 || nMsgs > 16) return false;

    const size_t tag_padded = ossia_pad4((size_t)(nVals + 2));  // comma + n chars + null
    size_t total = 16;  // "#bundle\0" + timetag
    for (int i = 0; i < nMsgs; ++i) {
      if (!params[i]) return false;
      const size_t msg_size = params[i]->path_padded_len() + tag_padded + (size_t)nVals * 4;
      total += 4 + msg_size;
    }

    uint8_t buf[1024];
    if (total > sizeof(buf)) return false;

    char tag_str[40];
    tag_str[0] = ',';
    for (int i = 0; i < nVals; ++i) tag_str[1 + i] = 'i';
    tag_str[1 + nVals] = '\0';
    const size_t tag_raw = (size_t)(nVals + 2);

    memcpy(buf, "#bundle\0", 8);
    memset(buf + 8, 0, 7);
    buf[15] = 1;  
    uint8_t* p = buf + 16;

    for (int i = 0; i < nMsgs; ++i) {
      const size_t msg_size = params[i]->path_padded_len() + tag_padded + (size_t)nVals * 4;
      p[0] = (uint8_t)(msg_size >> 24);
      p[1] = (uint8_t)(msg_size >> 16);
      p[2] = (uint8_t)(msg_size >> 8);
      p[3] = (uint8_t)(msg_size);
      p += 4;

      p += ossia_write_padded(p, params[i]->path_c_str());
      memcpy(p, tag_str, tag_raw);
      memset(p + tag_raw, 0, tag_padded - tag_raw);
      p += tag_padded;
      for (int j = 0; j < nVals; ++j) {
        ossia_write_int32(p, vals[j]);
        p += 4;
      }
    }

    _udp.beginPacket(_ip.c_str(), _port);
    _udp.write(buf, total);
    return _udp.endPacket() != 0;
  }

private:
  WiFiUDP&    _udp;
  std::string _ip;
  int         _port;
};
