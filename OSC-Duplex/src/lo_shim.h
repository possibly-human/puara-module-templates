/**
 * lo_shim.h — small liblo-style send shim for ESP32 / Arduino + WiFiUDP
 *
 * This is not a full liblo port. It models the send-side pattern that matters
 * for this benchmark:
 *   - message objects keep separate heap-backed type-tag and argument buffers
 *   - int32 arguments are appended one at a time through liblo-style calls
 *   - send calls serialise to a temporary OSC wire buffer, then send by UDP
 *   - bundle calls collect messages and serialise one OSC bundle packet
 *
 * Why this shim exists:
 *   - desktop liblo expects a more POSIX-like runtime than ESP32 Arduino
 *   - the benchmark still wants to preserve the costs of liblo-style message
 *     building: heap allocation, realloc growth, serialisation, and one final
 *     UDP send per message or bundle
 *
 * What matches liblo :
 *   - separate heap buffers for type tags and argument data
 *   - append-style message construction with realloc growth
 *   - temporary serialisation buffer allocated at send time
 *   - one OSC bundle packet for bundle sends
 *
 * What this shim leaves out :
 *   - only the int32 send-side subset used by this benchmark
 *   - no liblo address/server objects, sockets, or receive-side API
 *   - no attempt to reproduce every internal liblo structure byte-for-byte
 *   - bundles only contain messages, not other bundles
 *   - bundle storage and message lifetime handling are kept simple for this benchmark
 *
 * Interpret results as liblo-style message/bundle send overhead adapted to
 * ESP32 WiFiUDP, not as a benchmark of the full liblo library.
 *
 * API subset implemented here:
 *   lo_message  lo_message_new()
 *   int         lo_message_add_int32(lo_message m, int32_t val)
 *   int         lo_send(WiFiUDP& udp, ip, port, path, lo_message)
 *   void        lo_message_free(lo_message m)
 *   lo_bundle   lo_bundle_new()
 *   int         lo_bundle_add_message(lo_bundle b, path, lo_message m)
 *   int         lo_send_bundle(WiFiUDP& udp, ip, port, lo_bundle b)
 *   void        lo_bundle_free_recursive(lo_bundle b)
 */

#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>
#include <cstdlib>
#include <cstring>

// ── OSC wire helpers ──────────────────────────────────────────────────────────

static inline size_t lo_pad4(size_t n) { return (n + 3) & ~3u; }

static inline size_t lo_write_padded(uint8_t* dst, const char* s) {
  size_t len    = strlen(s) + 1;
  size_t padded = lo_pad4(len);
  memcpy(dst, s, len);
  memset(dst + len, 0, padded - len);
  return padded;
}

static inline void lo_write_int32(uint8_t* dst, int32_t v) {
  dst[0] = (uint8_t)(v >> 24);
  dst[1] = (uint8_t)(v >> 16);
  dst[2] = (uint8_t)(v >>  8);
  dst[3] = (uint8_t)(v      );
}

// ── lo_message ────────────────────────────────────────────────────────────────
// Keep the liblo-style message shape that matters here:
// type tags and argument bytes live in separate growable buffers.

struct lo_message_t {
  char*    types;        // type tag string, starts with a comma, e.g. ",iiii\0"
  size_t   types_len;    // length including leading comma, excluding null
  size_t   types_alloc;

  uint8_t* data;
  size_t   data_len;
  size_t   data_alloc;   // 0 until first add
};

using lo_message = lo_message_t*;

static inline lo_message lo_message_new() {
  lo_message m = (lo_message)malloc(sizeof(lo_message_t));
  if (!m) return nullptr;

  // Match liblo's small initial type buffer and lazy data allocation.
  m->types_alloc = 8;
  m->types       = (char*)malloc(m->types_alloc);
  if (!m->types) { free(m); return nullptr; }
  m->types[0]    = ',';
  m->types[1]    = '\0';
  m->types_len   = 1;

  m->data_alloc  = 0;
  m->data        = nullptr;
  m->data_len    = 0;

  return m;
}

static inline void lo_message_free(lo_message m) {
  if (!m) return;
  free(m->types);
  free(m->data);
  free(m);
}

static inline int lo_message_add_int32(lo_message m, int32_t val) {
  // Grow data first, then append the type char.
  if (m->data_len + 4 > m->data_alloc) {
    size_t new_alloc = (m->data_alloc == 0) ? 8 : m->data_alloc * 2;
    uint8_t* d = (uint8_t*)realloc(m->data, new_alloc);
    if (!d) return -1;
    m->data       = d;
    m->data_alloc = new_alloc;
  }

  // Grow the type tag one character at a time.
  if (m->types_len + 2 > m->types_alloc) {
    size_t new_alloc = m->types_alloc * 2;
    char* t = (char*)realloc(m->types, new_alloc);
    if (!t) return -1;
    m->types       = t;
    m->types_alloc = new_alloc;
  }
  m->types[m->types_len++] = 'i';
  m->types[m->types_len]   = '\0';

  lo_write_int32(m->data + m->data_len, val);
  m->data_len += 4;

  return 0;
}

// Serialise one OSC message into a caller-provided buffer.
static inline size_t lo_message_serialise(uint8_t* p, const char* path, lo_message m) {
  size_t n = 0;
  n += lo_write_padded(p + n, path);
  n += lo_write_padded(p + n, m->types);
  memcpy(p + n, m->data, m->data_len);
  n += m->data_len;
  return n;
}

static inline int lo_send(WiFiUDP& udp, const char* ip, int port,
                           const char* path, lo_message m) {
  size_t path_bytes  = lo_pad4(strlen(path) + 1);
  size_t types_bytes = lo_pad4(m->types_len + 1);
  size_t total       = path_bytes + types_bytes + m->data_len;

  // Allocate one temporary wire buffer per send.
  uint8_t* buf = (uint8_t*)malloc(total);
  if (!buf) return 0;

  lo_message_serialise(buf, path, m);

  udp.beginPacket(ip, port);
  udp.write(buf, total);
  int ok = udp.endPacket();

  free(buf);
  return ok;
}

// ── lo_bundle ─────────────────────────────────────────────────────────────────
// Keep the liblo-style bundle shape that matters here:
// a growable entry list, duplicated paths, and one final bundle serialisation.
//
// Wire format: "#bundle\0" (8B) + timetag (8B) + N × [int32 size | message]

// Each bundle entry is heap-backed so add-message still does allocation work.
struct lo_bundle_entry_t {
  char*      path;  // duplicated per entry
  lo_message msg;
};

struct lo_bundle_t {
  lo_bundle_entry_t** entries;     // growable pointer array
  int                 count;
  int                 alloc;
  uint64_t            timetag;
};

using lo_bundle = lo_bundle_t*;

static constexpr uint64_t LO_TT_IMMEDIATE = 1;

static inline lo_bundle lo_bundle_new(uint64_t timetag = LO_TT_IMMEDIATE) {
  lo_bundle b = (lo_bundle)malloc(sizeof(lo_bundle_t));
  if (b) {
    b->count   = 0;
    b->alloc   = 4;
    b->timetag = timetag;
    b->entries = (lo_bundle_entry_t**)calloc((size_t)b->alloc, sizeof(lo_bundle_entry_t*));
    if (!b->entries) {
      free(b);
      return nullptr;
    }
  }
  return b;
}

static inline int lo_bundle_add_message(lo_bundle b, const char* path, lo_message m) {
  if (!b || !m) return -1;
  if (b->count >= b->alloc) {
    int new_alloc = b->alloc * 2;
    lo_bundle_entry_t** new_entries =
      (lo_bundle_entry_t**)realloc(b->entries, (size_t)new_alloc * sizeof(lo_bundle_entry_t*));
    if (!new_entries) return -1;
    memset(new_entries + b->alloc, 0, (size_t)(new_alloc - b->alloc) * sizeof(lo_bundle_entry_t*));
    b->entries = new_entries;
    b->alloc   = new_alloc;
  }

  // Duplicate the path when a message is added.
  lo_bundle_entry_t* entry = (lo_bundle_entry_t*)malloc(sizeof(lo_bundle_entry_t));
  if (!entry) return -1;
  entry->path = (char*)malloc(strlen(path) + 1);
  if (!entry->path) { free(entry); return -1; }
  strcpy(entry->path, path);
  entry->msg = m;
  b->entries[b->count++] = entry;
  return 0;
}

static inline void lo_bundle_free_recursive(lo_bundle b) {
  if (!b) return;
  for (int i = 0; i < b->count; i++) {
    if (b->entries[i]) {
      lo_message_free(b->entries[i]->msg);
      free(b->entries[i]->path);
      free(b->entries[i]);
    }
  }
  free(b->entries);
  free(b);
}

static inline int lo_send_bundle(WiFiUDP& udp, const char* ip, int port, lo_bundle b) {
  if (!b || b->count == 0) return 0;

  // Bundle sends also use one temporary wire buffer.
  size_t total = 16; // "#bundle\0" (8) + timetag (8)
  for (int i = 0; i < b->count; i++) {
    lo_message m = b->entries[i]->msg;
    size_t msg_size = lo_pad4(strlen(b->entries[i]->path) + 1)
                    + lo_pad4(m->types_len + 1)
                    + m->data_len;
    total += 4 + msg_size;
  }

  uint8_t* buf = (uint8_t*)malloc(total);
  if (!buf) return 0;

  uint8_t* p = buf;

  // Bundle header
  memcpy(p, "#bundle\0", 8); p += 8;
  // Write timetag big-endian.
  uint64_t tt = b->timetag;
  for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)(tt & 0xFF); tt >>= 8; }
  p += 8;

  // Messages
  for (int i = 0; i < b->count; i++) {
    lo_message m = b->entries[i]->msg;
    size_t msg_size = lo_pad4(strlen(b->entries[i]->path) + 1)
                    + lo_pad4(m->types_len + 1)
                    + m->data_len;
    p[0] = (uint8_t)(msg_size >> 24);
    p[1] = (uint8_t)(msg_size >> 16);
    p[2] = (uint8_t)(msg_size >>  8);
    p[3] = (uint8_t)(msg_size);
    p += 4;
    p += lo_message_serialise(p, b->entries[i]->path, m);
  }

  udp.beginPacket(ip, port);
  udp.write(buf, total);
  int ok = udp.endPacket();

  free(buf);
  return ok;
}
