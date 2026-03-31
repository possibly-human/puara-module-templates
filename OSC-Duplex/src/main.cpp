/**
 * main.cpp — ESP32 OSC send benchmark
 * ===================================
 *
 * This file benchmarks OSC send performance of various libraries on ESP32
 * under real WiFi conditions.
 *
 * It compares each transport in terms of:
 *   1. send overhead relative to raw UDP
 *   2. send cost as rate increases
 *   3. send cost as payload size increases
 *   4. behaviour for multi-address sends
 *   5. behaviour for bundle sends
 *   6. behaviour near practical limits under stress
 *   7. timing stability and jitter over WiFi
 *
 * All transports use the same WiFi socket, destination, timing rules, warmup,
 * and measurement windows. Each benchmark payload includes a local timestamp so
 * the receiver can reconstruct timing without a shared clock.
 *
 * LIBRARIES/TRANSPORTS
 *   RAW_UDP      — plain UDP baseline, no OSC library
 *   CNMAT        — https://github.com/CNMAT/OSC
 *   MICROOSC     — https://github.com/thomasfredericks/MicroOsc
 *   LO           — benchmark shim based on liblo-style message/bundle usage
 *   OSSIA        — benchmark shim based on libossia-style parameter pushes
 *   TINYOSC      — https://github.com/mhroth/tinyosc
 *   OSCPKT       — https://github.com/eddietree/oscpkt
 *
 * ADAPTATION NOTE
 *   CNMAT, MicroOsc, TinyOSC, and OSCPKT use their native send APIs.
 *   LO and OSSIA use benchmark-focused shims that preserve the send-side usage
 *   patterns relevant to this comparison. (TinyOSC also required a small local
 *   patch in .pio/libdeps to disable its desktop demo main.c and add an
 *   htonll/ntohll fallback for this ESP32 build.)
 *
 * MODES
 *   Fixed:
 *     Rate      — 10 Hz, 100 Hz, 500 Hz, 1000 Hz, FAST
 *     Payload   — PAY8, PAY16
 *     Multi     — MULTI4, MULTI8
 *     Bundle    — BUNDLE2, BUNDLE4
 *   Ramp-to-break:
 *     RATE      — 500, 750, 1000, 1500, 2000, 3000, 5000 Hz
 *     PAYLOAD   — 8, 16, 32 args
 *     MULTI     — 4, 8, 16, 32 addresses
 *     BUNDLE    — 4, 8, 16, 32 messages
 *
 * PACKET PAYLOAD (4 × int32 in every packet)
 *   [0] run_id            — unique id for the current benchmark run
 *   [1] seq               — send sequence number
 *   [2] send_start_us     — micros() at the start of the current send
 *   [3] prev_send_cost_us — duration of the previous send, reported one packet late
 *                           (elapsed time from the start of the previous send
 *                           call to the point where that packet was handed off)
 *
 * BUNDLE POLICY
 *   CNMAT, LO, TINYOSC, and OSCPKT use native bundle APIs.
 *   RAW_UDP and MICROOSC use a shared manual bundle encoder so they can still
 *   be tested on the same one-packet bundle workload, even though that is not
 *   a native library bundle API comparison for those transports.
 *   OSSIA bundle modes use the shim's protocol-level batch sender.
 *
 * NETWORK CONTEXT
 *   The benchmark keeps real WiFi in the send path, so radio and receiver behaviour
 *   are part of the result. WiFi snapshots and major WiFi events are logged so
 *   transport cost changes can be distinguished from network changes.
 *
 * SERIAL OUTPUT
 *   Serial is used for metadata and diagnostic rows:
 *     E — send / build failures
 *     S — per-run summary counters
 *     T — measurement duration
 *     N — WiFi snapshots at run start/end
 *     W — WiFi event log
 *     "BENCHMARK COMPLETE" — end of benchmark
 */

#include "Arduino.h"
#include "puara.h"
#include <OSCBundle.h>
#include <OSCMessage.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <MicroOscUdp.h>
#include <cstring>
#include "esp_wifi.h"
#include "lo_shim.h"
#include "ossia_shim.h"
#include <tinyosc.h>
#include <oscpkt.hh>

static constexpr int MAX_MULTI_ADDRS = 32;
static constexpr int MAX_BUNDLE_MSGS = 32;
static constexpr int MANUAL_BUNDLE_CAP = 2048;

enum TransportId : int32_t
{
  TRANSPORT_RAW_UDP = 0,
  TRANSPORT_CNMAT = 1,
  TRANSPORT_MICROOSC = 2,
  TRANSPORT_LO = 3,
  TRANSPORT_OSSIA = 4,
  TRANSPORT_TINYOSC = 5,
  TRANSPORT_OSCPKT = 6
};

enum ModeId : int32_t
{
  MODE_10HZ = 0,
  MODE_100HZ = 1,
  MODE_500HZ = 2,
  MODE_1000HZ = 3,
  MODE_FAST = 4,
  MODE_PAY8 = 5,
  MODE_PAY16 = 6,
  MODE_MULTI4 = 7,
  MODE_MULTI8 = 8,
  MODE_BUNDLE2 = 9,
  MODE_BUNDLE4 = 10,
  MODE_RAMP_RATE = 11,
  MODE_RAMP_PAYLOAD = 12,
  MODE_RAMP_MULTI = 13,
  MODE_RAMP_BUNDLE = 14,
};

static constexpr uint32_t TEST_DURATION_MS = 10000;
static constexpr uint32_t GAP_MS = 2000;
static constexpr uint32_t TRANSPORT_GAP_MS = 3000;
static constexpr uint32_t WARMUP_MS = 5000;
static constexpr uint32_t RAMP_WARMUP_MS = 2000;
static constexpr uint32_t RAMP_STEP_MS = 5000;
static constexpr int N_PASSES = 3;
static constexpr uint32_t BUSYWAIT_THRESHOLD_US = 1500;
static constexpr uint32_t SEND_SPIKE_THRESHOLD_US = 1000;

static constexpr float RAMP_COST_RATIO_LIMIT = 0.80f;
static constexpr float RAMP_ERROR_RATE_LIMIT = 0.05f;
static constexpr uint32_t RAMP_ABS_COST_LIMIT_US = 5000;

static constexpr uint32_t INTERVAL_US_10HZ = 100000;
static constexpr uint32_t INTERVAL_US_100HZ = 10000;
static constexpr uint32_t INTERVAL_US_500HZ = 2000;
static constexpr uint32_t INTERVAL_US_1000HZ = 1000;

static constexpr size_t MAX_ERROR_RECORDS = 64;
static constexpr const char *BENCH_ADDR = "/bench";

enum ErrorCode : uint8_t
{
  ERR_SEND_FAIL = 1,
  ERR_BUILD_FAIL = 2
};

struct ErrorRecord
{
  uint32_t time_us;
  uint32_t seq;
  ErrorCode code;
};

enum BenchPhase : uint8_t
{
  PHASE_FIXED,
  PHASE_RAMP_RATE,
  PHASE_RAMP_PAYLOAD,
  PHASE_RAMP_MULTI,
  PHASE_RAMP_BUNDLE,
  PHASE_DONE
};

Puara puara;
WiFiUDP Udp;

// MicroOscUdp needs a fixed internal buffer size.
// 256 B is enough for the largest payload mode used here (PAY32).
MicroOscUdp<256> *microOscUdp = nullptr;
// Note : all globals prefixed g_

// OSSIA benchmark state.
// ossiaParam is the shared /bench parameter used by single-message modes.
// ossiaMultiParams holds one parameter per /bench/N address for MULTI modes.
// The adaptation details are documented in ossia_shim.h.
OssiaDevice *ossiaDevice = nullptr;
OssiaParam *ossiaParam = nullptr;
OssiaParam *ossiaMultiParams[MAX_MULTI_ADDRS] = {};

// OSC settings
std::string oscIP{};
int oscPort = 0;
int localPort = 0;

// Error tracking
ErrorRecord g_errors[MAX_ERROR_RECORDS];
size_t g_errorCount = 0;

void addError(uint32_t seq, ErrorCode code)
{
  // Keep only the first MAX_ERROR_RECORDS detailed error rows.
  // Total failures are still reflected in the summary counters.
  if (g_errorCount < MAX_ERROR_RECORDS)
    g_errors[g_errorCount++] = {micros(), seq, code};
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark state
// ─────────────────────────────────────────────────────────────────────────────

// ── Sequencer state ────────────────────────────────────────────────────────────
// PHASE_FIXED repeats the fixed comparison modes for N_PASSES.
// PHASE_RAMP_* ramps one category per transport to find practical limits.

BenchPhase g_phase = PHASE_FIXED;
int g_pass = 0;
int g_modeIdx = 0;
int g_transIdx = 0;
int g_rampStep = 0;
int g_rampRateIdx = 0;
uint32_t g_rampIntervalUs = 0;
int g_rampExtraArgs = 0;
int g_rampNAddrs = 0;
int g_rampNMsgs = 0;

// ── Per-window state ────────────────────────────────────────────────────────────

TransportId g_transport = TRANSPORT_RAW_UDP;
ModeId g_mode = MODE_10HZ;
bool g_inWarmup = true;
uint32_t g_modeStartMs = 0;
uint32_t g_nextSchedUs = 0;
uint32_t g_seq = 0;
uint32_t g_runId = 0;
uint32_t g_prevSendCostUs = 0;
uint32_t g_attempted = 0;
uint32_t g_sendOk = 0;
uint64_t g_sendCostSum = 0;
uint32_t g_sendCostSamples = 0;
uint32_t g_wifiDisconnects = 0;
uint32_t g_wifiGotIp = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Utility helpers
// ─────────────────────────────────────────────────────────────────────────────

uint32_t modeIntervalUs(ModeId m)
{
  switch (m)
  {
  case MODE_10HZ:
    return INTERVAL_US_10HZ;
  case MODE_100HZ:
    return INTERVAL_US_100HZ;
  case MODE_500HZ:
    return INTERVAL_US_500HZ;
  case MODE_1000HZ:
    return INTERVAL_US_1000HZ;
  case MODE_PAY8:
    return INTERVAL_US_100HZ;
  case MODE_PAY16:
    return INTERVAL_US_100HZ;
  case MODE_MULTI4:
    return INTERVAL_US_100HZ;
  case MODE_MULTI8:
    return INTERVAL_US_100HZ;
  case MODE_BUNDLE2:
    return INTERVAL_US_100HZ;
  case MODE_BUNDLE4:
    return INTERVAL_US_100HZ;
  case MODE_RAMP_RATE:
    return g_rampIntervalUs;
  case MODE_RAMP_PAYLOAD:
    return g_rampIntervalUs;
  case MODE_RAMP_MULTI:
    return g_rampIntervalUs;
  case MODE_RAMP_BUNDLE:
    return g_rampIntervalUs;
  default:
    return 0;
  }
}

const char *modeName(ModeId m)
{
  switch (m)
  {
  case MODE_10HZ:
    return "10Hz";
  case MODE_100HZ:
    return "100Hz";
  case MODE_500HZ:
    return "500Hz";
  case MODE_1000HZ:
    return "1000Hz";
  case MODE_FAST:
    return "FAST";
  case MODE_PAY8:
    return "PAY8";
  case MODE_PAY16:
    return "PAY16";
  case MODE_MULTI4:
    return "MULTI4";
  case MODE_MULTI8:
    return "MULTI8";
  case MODE_BUNDLE2:
    return "BUNDLE2";
  case MODE_BUNDLE4:
    return "BUNDLE4";
  case MODE_RAMP_RATE:
    return "RAMP_RATE";
  case MODE_RAMP_PAYLOAD:
    return "RAMP_PAY";
  case MODE_RAMP_MULTI:
    return "RAMP_MULTI";
  case MODE_RAMP_BUNDLE:
    return "RAMP_BUNDLE";
  default:
    return "DONE";
  }
}

const char *transportName(TransportId t)
{
  switch (t)
  {
  case TRANSPORT_RAW_UDP:
    return "RAW_UDP";
  case TRANSPORT_CNMAT:
    return "CNMAT";
  case TRANSPORT_MICROOSC:
    return "MICROOSC";
  case TRANSPORT_LO:
    return "LO";
  case TRANSPORT_OSSIA:
    return "OSSIA";
  case TRANSPORT_TINYOSC:
    return "TINYOSC";
  case TRANSPORT_OSCPKT:
    return "OSCPKT";
  default:
    return "UNKNOWN";
  }
}

bool isRampPhase(BenchPhase phase)
{
  return phase == PHASE_RAMP_RATE || phase == PHASE_RAMP_PAYLOAD || phase == PHASE_RAMP_MULTI || phase == PHASE_RAMP_BUNDLE;
}

uint32_t currentWarmupMs()
{
  return isRampPhase(g_phase) ? RAMP_WARMUP_MS : WARMUP_MS;
}

uint32_t currentMeasureDurationMs()
{
  return isRampPhase(g_phase) ? RAMP_STEP_MS : TEST_DURATION_MS;
}

const char *wifiStatusName(wl_status_t status)
{
  switch (status)
  {
  case WL_CONNECTED:
    return "CONNECTED";
  case WL_NO_SHIELD:
    return "NO_SHIELD";
  case WL_IDLE_STATUS:
    return "IDLE";
  case WL_NO_SSID_AVAIL:
    return "NO_SSID";
  case WL_SCAN_COMPLETED:
    return "SCAN_DONE";
  case WL_CONNECT_FAILED:
    return "CONNECT_FAIL";
  case WL_CONNECTION_LOST:
    return "CONN_LOST";
  case WL_DISCONNECTED:
    return "DISCONNECTED";
  default:
    return "UNKNOWN";
  }
}

struct WiFiSnapshot
{
  const char *statusName;
  int rssi;
  int channel;
};

WiFiSnapshot currentWiFiSnapshot()
{
  WiFiSnapshot snap{
      wifiStatusName(WiFi.status()),
      WiFi.RSSI(),
      (int)WiFi.channel()};

  wifi_ap_record_t apInfo{};
  if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK)
  { // Prefer ESP-IDF WiFi state when available
    snap.statusName = "CONNECTED";
    snap.rssi = (int)apInfo.rssi;

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary > 0)
    {
      snap.channel = (int)primary;
    }
    else if (apInfo.primary > 0)
    {
      snap.channel = (int)apInfo.primary;
    }
  }
  else
  {
    if (WiFi.isConnected())
      snap.statusName = "CONNECTED";
  }

  return snap;
}

// Emit a WiFi state row at measurement start/end
void logWiFiSnapshot(const char *phase)
{
  WiFiSnapshot snap = currentWiFiSnapshot();
  Serial.printf("N,%u,%d,%d,%s,%lu,%s,%d,%d,%lu,%lu\n",
                (unsigned)g_runId,
                (int)g_transport,
                (int)g_mode,
                phase,
                (unsigned long)millis(),
                snap.statusName,
                snap.rssi,
                snap.channel,
                (unsigned long)g_wifiDisconnects,
                (unsigned long)g_wifiGotIp);
}

// Record WiFi events
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t)
{
  if (!g_inWarmup && g_phase != PHASE_DONE)
  {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      g_wifiDisconnects++;
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      g_wifiGotIp++;
  }

  WiFiSnapshot snap = currentWiFiSnapshot();
  Serial.printf("W,%u,%lu,%d,%s,%d,%d\n",
                (unsigned)g_runId,
                (unsigned long)millis(),
                (int)event,
                snap.statusName,
                snap.rssi,
                snap.channel);
}

// Reset all per-mode counters. Called at warmup-end.
void resetModeState()
{
  g_seq = 0;
  g_errorCount = 0;
  g_prevSendCostUs = 0;
  g_attempted = 0;
  g_sendOk = 0;
  g_sendCostSum = 0;
  g_sendCostSamples = 0;
  g_wifiDisconnects = 0;
  g_wifiGotIp = 0;
}

// Start a new mode: enter warmup, bump run_id and reset scheduler timestamps.
void beginMode(ModeId mode)
{
  g_mode = mode;
  g_inWarmup = true;
  g_modeStartMs = millis();
  g_nextSchedUs = micros();
  g_runId++;
  resetModeState();

  Serial.printf(
      "\n=== MODE START [run=%u] [%s] %s  warmup=%lums ===\n",
      (unsigned)g_runId, transportName(g_transport),
      modeName(g_mode), (unsigned long)currentWarmupMs());
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-address strings used by MULTI and BUNDLE tests.
// They are defined once here so every transport uses the same address set.
// ─────────────────────────────────────────────────────────────────────────────

const char *MULTI_ADDRS[MAX_MULTI_ADDRS] = {
    "/bench/0", "/bench/1", "/bench/2", "/bench/3",
    "/bench/4", "/bench/5", "/bench/6", "/bench/7",
    "/bench/8", "/bench/9", "/bench/10", "/bench/11",
    "/bench/12", "/bench/13", "/bench/14", "/bench/15",
    "/bench/16", "/bench/17", "/bench/18", "/bench/19",
    "/bench/20", "/bench/21", "/bench/22", "/bench/23",
    "/bench/24", "/bench/25", "/bench/26", "/bench/27",
    "/bench/28", "/bench/29", "/bench/30", "/bench/31"};

// ─────────────────────────────────────────────────────────────────────────────
// Sequencer
//
// Fixed phase:
//   For each pass, each fixed mode is run across all transports.
//
// Ramp phase:
//   RATE is ramped by frequency.
//   PAYLOAD, MULTI, and BUNDLE ramp their own counters at 100 Hz,
//   then repeat at a higher stress rate if they still do not break.
//   Each ramp stops at the first failing step or at the last configured step.
// ─────────────────────────────────────────────────────────────────────────────

// ── Fixed-mode and transport tables ────────────────────────────────────────────
static constexpr int N_ALL_TRANSPORTS = 7;
static constexpr int N_FIXED_MODES = 11;

static const TransportId ALL_TRANSPORTS[N_ALL_TRANSPORTS] = {
    TRANSPORT_RAW_UDP, TRANSPORT_CNMAT, TRANSPORT_MICROOSC, TRANSPORT_LO,
    TRANSPORT_OSSIA, TRANSPORT_TINYOSC, TRANSPORT_OSCPKT};
static const ModeId FIXED_MODES[N_FIXED_MODES] = {
    MODE_10HZ, MODE_100HZ, MODE_500HZ, MODE_1000HZ, MODE_FAST,
    MODE_PAY8, MODE_PAY16, MODE_MULTI4, MODE_MULTI8, MODE_BUNDLE2, MODE_BUNDLE4};

// ── Ramp step tables ────────────────────────────────────────────────────────────
static constexpr int N_RATE_RAMP = 7;
static constexpr int N_PAYLOAD_RAMP = 3;
static constexpr int N_MULTI_RAMP = 4;
static constexpr int N_BUNDLE_RAMP = 4;
static constexpr int N_PAYLOAD_RAMP_RATES = 2;
static constexpr int N_MULTI_RAMP_RATES = 2;
static constexpr int N_BUNDLE_RAMP_RATES = 2;
static constexpr uint32_t RATE_RAMP_HZ[N_RATE_RAMP] = {500, 750, 1000, 1500, 2000, 3000, 5000};
// Payload ramp: extra args appended after the 4 packet header ints.
static constexpr int PAYLOAD_RAMP_EXTRA[N_PAYLOAD_RAMP] = {4, 12, 28}; // → PAY8, PAY16, PAY32
static constexpr uint32_t PAYLOAD_RAMP_HZ[N_PAYLOAD_RAMP_RATES] = {100, 1000};
// Multi ramp: one packet per address each tick, up to the largest address table.
static constexpr int MULTI_RAMP_ADDRS[N_MULTI_RAMP] = {4, 8, 16, 32};
static constexpr uint32_t MULTI_RAMP_HZ[N_MULTI_RAMP_RATES] = {100, 500};
// Bundle ramp matches MULTI message counts, but packs them into one packet.
static constexpr int BUNDLE_RAMP_MSGS[N_BUNDLE_RAMP] = {4, 8, 16, 32};
static constexpr uint32_t BUNDLE_RAMP_HZ[N_BUNDLE_RAMP_RATES] = {100, 500};

int rampPhaseStepCount(BenchPhase phase)
{
  switch (phase)
  {
  case PHASE_RAMP_RATE:
    return N_RATE_RAMP;
  case PHASE_RAMP_PAYLOAD:
    return N_PAYLOAD_RAMP;
  case PHASE_RAMP_MULTI:
    return N_MULTI_RAMP;
  case PHASE_RAMP_BUNDLE:
    return N_BUNDLE_RAMP;
  default:
    return 0;
  }
}

int rampPhaseRateStageCount(BenchPhase phase)
{
  switch (phase)
  {
  case PHASE_RAMP_PAYLOAD:
    return N_PAYLOAD_RAMP_RATES;
  case PHASE_RAMP_MULTI:
    return N_MULTI_RAMP_RATES;
  case PHASE_RAMP_BUNDLE:
    return N_BUNDLE_RAMP_RATES;
  default:
    return 1;
  }
}

uint32_t rampPhaseRateHz(BenchPhase phase, int rateIdx)
{
  switch (phase)
  {
  case PHASE_RAMP_RATE:
    return RATE_RAMP_HZ[g_rampStep];
  case PHASE_RAMP_PAYLOAD:
    return PAYLOAD_RAMP_HZ[rateIdx];
  case PHASE_RAMP_MULTI:
    return MULTI_RAMP_HZ[rateIdx];
  case PHASE_RAMP_BUNDLE:
    return BUNDLE_RAMP_HZ[rateIdx];
  default:
    return 0;
  }
}

// ── Ramp step setup ─────────────────────────────────────────────────────────────
void beginRampStep()
{
  switch (g_phase)
  {
  case PHASE_RAMP_RATE:
    g_rampIntervalUs = 1000000u / RATE_RAMP_HZ[g_rampStep];
    beginMode(MODE_RAMP_RATE);
    Serial.printf("  [RAMP_RATE  step %d] %luHz  interval=%luus\n",
                  g_rampStep, (unsigned long)RATE_RAMP_HZ[g_rampStep],
                  (unsigned long)g_rampIntervalUs);
    break;
  case PHASE_RAMP_PAYLOAD:
    g_rampExtraArgs = PAYLOAD_RAMP_EXTRA[g_rampStep];
    g_rampIntervalUs = 1000000u / rampPhaseRateHz(g_phase, g_rampRateIdx);
    beginMode(MODE_RAMP_PAYLOAD);
    Serial.printf("  [RAMP_PAY   step %d] %d args total @%luHz interval=%luus\n",
                  g_rampStep, 4 + g_rampExtraArgs,
                  (unsigned long)rampPhaseRateHz(g_phase, g_rampRateIdx),
                  (unsigned long)g_rampIntervalUs);
    break;
  case PHASE_RAMP_MULTI:
    g_rampNAddrs = MULTI_RAMP_ADDRS[g_rampStep];
    g_rampIntervalUs = 1000000u / rampPhaseRateHz(g_phase, g_rampRateIdx);
    beginMode(MODE_RAMP_MULTI);
    Serial.printf("  [RAMP_MULTI step %d] %d addrs/tick @%luHz interval=%luus\n",
                  g_rampStep, g_rampNAddrs,
                  (unsigned long)rampPhaseRateHz(g_phase, g_rampRateIdx),
                  (unsigned long)g_rampIntervalUs);
    break;
  case PHASE_RAMP_BUNDLE:
    g_rampNMsgs = BUNDLE_RAMP_MSGS[g_rampStep];
    g_rampIntervalUs = 1000000u / rampPhaseRateHz(g_phase, g_rampRateIdx);
    beginMode(MODE_RAMP_BUNDLE);
    Serial.printf("  [RAMP_BDL   step %d] %d msgs/bundle @%luHz interval=%luus\n",
                  g_rampStep, g_rampNMsgs,
                  (unsigned long)rampPhaseRateHz(g_phase, g_rampRateIdx),
                  (unsigned long)g_rampIntervalUs);
    break;
  default:
    break;
  }
}

// ── Breaking criterion ──────────────────────────────────────────────────────────
// RATE ramps break when send cost consumes too much of the target interval.
// The other ramps use a fixed absolute send-cost limit instead.
bool isRampBreaking()
{
  if (g_attempted == 0 || g_sendCostSamples == 0)
    return false;
  float avgCost = (float)g_sendCostSum / (float)g_sendCostSamples;
  float errorRate = (float)(g_attempted - g_sendOk) / (float)g_attempted;
  if (g_phase == PHASE_RAMP_RATE)
    return (avgCost > (float)g_rampIntervalUs * RAMP_COST_RATIO_LIMIT) || (errorRate > RAMP_ERROR_RATE_LIMIT);
  return (avgCost > (float)RAMP_ABS_COST_LIMIT_US) || (errorRate > RAMP_ERROR_RATE_LIMIT);
}

// ── Main sequencer ──────────────────────────────────────────────────────────────
void advanceToNext()
{

  // ── Fixed phase ────────────────────────────────────────────────────────────
  if (g_phase == PHASE_FIXED)
  {
    // Try next transport for the same mode
    g_transIdx++;
    if (g_transIdx < N_ALL_TRANSPORTS)
    {
      delay(TRANSPORT_GAP_MS);
      g_transport = ALL_TRANSPORTS[g_transIdx];
      beginMode(FIXED_MODES[g_modeIdx]);
      return;
    }
    // All transports done for this mode : next mode
    g_transIdx = 0;
    g_modeIdx++;
    if (g_modeIdx < N_FIXED_MODES)
    {
      delay(GAP_MS);
      g_transport = ALL_TRANSPORTS[0];
      beginMode(FIXED_MODES[g_modeIdx]);
      return;
    }
    // All modes done : next pass
    g_modeIdx = 0;
    g_pass++;
    if (g_pass < N_PASSES)
    {
      delay(GAP_MS);
      g_transport = ALL_TRANSPORTS[0];
      beginMode(FIXED_MODES[g_modeIdx]);
      return;
    }
    // All passes done : start ramp phase
    Serial.println("\n=== FIXED PHASE COMPLETE — starting ramp-to-failure ===");
    g_phase = PHASE_RAMP_RATE;
    g_transIdx = 0;
    g_rampStep = 0;
    g_rampRateIdx = 0;
    delay(TRANSPORT_GAP_MS);
    g_transport = ALL_TRANSPORTS[0];
    beginRampStep();
    return;
  }

  // ── Ramp phases ────────────────────────────────────────────────────────────
  int maxSteps = rampPhaseStepCount(g_phase);

  bool breaking = isRampBreaking();
  if (breaking)
  {
    float avgCost = g_sendCostSamples ? (float)g_sendCostSum / g_sendCostSamples : 0;
    float errorRate = g_attempted ? (float)(g_attempted - g_sendOk) / g_attempted : 0;
    Serial.printf("  [BREAK] [%s] step %d: avg_cost=%.0fus  err=%.1f%%\n",
                  transportName(g_transport), g_rampStep, avgCost, errorRate * 100.0f);
  }

  g_rampStep++;
  bool exhausted = (g_rampStep >= maxSteps) || breaking;

  if (!exhausted)
  {
    delay(GAP_MS);
    beginRampStep();
    return;
  }

  if (!breaking && g_rampRateIdx + 1 < rampPhaseRateStageCount(g_phase))
  {
    g_rampStep = 0;
    g_rampRateIdx++;
    delay(GAP_MS);
    beginRampStep();
    return;
  }

  // This transport is done : move to next
  g_rampStep = 0;
  g_rampRateIdx = 0;
  g_transIdx++;
  if (g_transIdx < N_ALL_TRANSPORTS)
  {
    delay(TRANSPORT_GAP_MS);
    g_transport = ALL_TRANSPORTS[g_transIdx];
    beginRampStep();
    return;
  }

  // All transports done for this ramp category : advance to next category
  g_transIdx = 0;
  g_rampStep = 0;
  g_rampRateIdx = 0;
  BenchPhase next = PHASE_DONE;
  if (g_phase == PHASE_RAMP_RATE)
    next = PHASE_RAMP_PAYLOAD;
  else if (g_phase == PHASE_RAMP_PAYLOAD)
    next = PHASE_RAMP_MULTI;
  else if (g_phase == PHASE_RAMP_MULTI)
    next = PHASE_RAMP_BUNDLE;
  if (next == PHASE_DONE)
  {
    Serial.println("\n=== BENCHMARK COMPLETE ===");
    g_phase = PHASE_DONE;
    return;
  }
  Serial.printf("\n=== RAMP CATEGORY COMPLETE — starting %s ===\n",
                next == PHASE_RAMP_PAYLOAD ? "RAMP_PAYLOAD" : next == PHASE_RAMP_MULTI ? "RAMP_MULTI"
                                                                                       : "RAMP_BUNDLE");
  g_phase = next;
  delay(TRANSPORT_GAP_MS);
  g_transport = ALL_TRANSPORTS[0];
  beginRampStep();
}

// ─────────────────────────────────────────────────────────────────────────────
// Puara settings
// oscIP / oscPort / localPort are read once from the Puara config store at setup
// ─────────────────────────────────────────────────────────────────────────────

void loadSettings()
{
  localPort = puara.getVarNumber("localPORT");
  oscIP = puara.getVarText("oscIP");
  oscPort = puara.getVarNumber("oscPORT");
}

// ─────────────────────────────────────────────────────────────────────────────
// Serial reporting
//
// Timing data travels inside UDP packets. Serial is reserved for:
//   E rows:     send-side failures
//   N rows:     WiFi snapshots at measurement start/end
//   W rows:     asynchronous WiFi events such as disconnect / got-IP
// These rows give context for "real UDP send" results without mixing WiFi state
// into the timed packet path itself.
// ─────────────────────────────────────────────────────────────────────────────

void printSerialHeader()
{
  Serial.println("# Serial rows:");
  Serial.println("# E run_id,transport,mode,seq,time_us,error_code  (1=SEND_FAIL 2=BUILD_FAIL)");
  Serial.println("# S run_id,transport,mode,elapsed_ms,attempted,ok,error_count  (per-run summary)");
  Serial.println("# N run_id,transport,mode,phase,time_ms,wifi_status,rssi,channel,disconnects,got_ip");
  Serial.println("# W run_id,time_ms,event_id,wifi_status,rssi,channel  (WiFi event log)");
  Serial.println("# Timing data (send_start_us, prev_send_cost_us) is embedded in every UDP packet.");
}

// Flush all recorded errors to serial at mode end, then print a
// brief summary plus WiFi snapshot.
void dumpModeReport()
{
  int tid = (int)g_transport;
  int mid = (int)g_mode;
  uint32_t rid = g_runId;
  uint32_t elapsedMs = millis() - g_modeStartMs;

  logWiFiSnapshot("end");

  for (size_t i = 0; i < g_errorCount; ++i)
  {
    const auto &e = g_errors[i];
    Serial.printf("E,%u,%d,%d,%lu,%lu,%u\n",
                  (unsigned)rid, tid, mid,
                  (unsigned long)e.seq,
                  (unsigned long)e.time_us,
                  (unsigned)e.code);
  }

  Serial.printf("S,%u,%d,%d,%lu,%lu,%lu,%zu\n",
                (unsigned)rid, tid, mid,
                (unsigned long)elapsedMs,
                (unsigned long)g_attempted,
                (unsigned long)g_sendOk,
                g_errorCount);

  Serial.printf("\n=== [%s] %s SUMMARY (run %u) ===\n",
                transportName(g_transport), modeName(g_mode), (unsigned)g_runId);
  Serial.printf("  elapsed_ms: %lu  attempted: %lu  ok: %lu  fail: %lu  errors: %zu\n",
                (unsigned long)elapsedMs,
                (unsigned long)g_attempted, (unsigned long)g_sendOk,
                (unsigned long)(g_attempted - g_sendOk),
                g_errorCount);
  WiFiSnapshot snap = currentWiFiSnapshot();
  Serial.printf("  wifi: status=%s  rssi=%d  channel=%d  disconnects=%lu  got_ip=%lu\n",
                snap.statusName, snap.rssi, snap.channel,
                (unsigned long)g_wifiDisconnects, (unsigned long)g_wifiGotIp);
  Serial.println("  (timing in UDP packets — run analyze.py for full stats)");
}

// ─────────────────────────────────────────────────────────────────────────────
// Send-path implementation
// ─────────────────────────────────────────────────────────────────────────────

void recordSend(uint32_t seq, uint32_t costUs, bool ok,
                bool suppressSendFailLog = false)
{
  g_attempted++;
  if (ok)
  {
    g_sendOk++;
  }
  else if (!suppressSendFailLog)
  {
    addError(seq, ERR_SEND_FAIL);
  }
  g_prevSendCostUs = costUs;
  g_sendCostSum += costUs;
  g_sendCostSamples++;
}

void recordBatchSend(uint32_t seq, uint32_t costUs,
                     uint32_t attempted, uint32_t ok,
                     bool suppressSendFailLog = false)
{
  // MULTI sends several separate packets in one benchmark tick, so the run
  // summary counts successes per packet while still storing one send-cost sample
  // for the whole tick.
  g_attempted += attempted;
  g_sendOk += ok;
  if (!suppressSendFailLog && ok < attempted)
  {
    for (uint32_t i = ok; i < attempted; ++i)
      addError(seq, ERR_SEND_FAIL);
  }
  g_prevSendCostUs = costUs;
  g_sendCostSum += costUs;
  g_sendCostSamples++;
}

template <typename Writer>
bool sendUdpPacket(Writer &&writer)
{
  // Shared UDP path
  Udp.beginPacket(oscIP.c_str(), oscPort);
  writer();
  return Udp.endPacket() != 0;
}

static int buildOscBundle(uint8_t *buf, int cap, uint32_t seq,
                          uint32_t tickT0, int nMsgs)
{
  // Shared bundle encoder for transports without a native bundle API.
  if (cap < 16)
    return -1;

  memcpy(buf, "#bundle\0", 8);
  memset(buf + 8, 0, 7);
  buf[15] = 1;
  int pos = 16;

  for (int i = 0; i < nMsgs; i++)
  {
    const char *addr = MULTI_ADDRS[i];
    int32_t args[4] = {
        (int32_t)g_runId, (int32_t)seq,
        (int32_t)tickT0, (int32_t)g_prevSendCostUs};

    uint8_t tmp[80];
    int mpos = 0;

    int alen = strlen(addr) + 1;
    int apad = (alen + 3) & ~3;
    memcpy(tmp, addr, alen);
    memset(tmp + alen, 0, apad - alen);
    mpos += apad;

    const char tag[8] = {',', 'i', 'i', 'i', 'i', '\0', '\0', '\0'};
    memcpy(tmp + mpos, tag, 8);
    mpos += 8;

    for (int j = 0; j < 4; j++)
    {
      tmp[mpos] = (uint8_t)(args[j] >> 24);
      tmp[mpos + 1] = (uint8_t)(args[j] >> 16);
      tmp[mpos + 2] = (uint8_t)(args[j] >> 8);
      tmp[mpos + 3] = (uint8_t)(args[j]);
      mpos += 4;
    }

    if (pos + 4 + mpos > cap)
      return -1;

    buf[pos] = (uint8_t)(mpos >> 24);
    buf[pos + 1] = (uint8_t)(mpos >> 16);
    buf[pos + 2] = (uint8_t)(mpos >> 8);
    buf[pos + 3] = (uint8_t)(mpos);
    memcpy(buf + pos + 4, tmp, mpos);
    pos += 4 + mpos;
  }

  return pos;
}

static oscpkt::Message g_oscpktMsg;
static oscpkt::Message g_oscpktMsgs[MAX_BUNDLE_MSGS];
static bool sendOscpktPacket(oscpkt::PacketWriter &pw)
{
  // oscpkt separates message construction from packet serialization, so the
  // send helper only writes the finished packet bytes if serialization succeeded.
  if (!pw.isOk())
    return false;
  char *data = pw.packetData();
  uint32_t size = pw.packetSize();
  if (!data || size == 0)
    return false;
  return sendUdpPacket([&]()
                       { Udp.write((const uint8_t *)data, size); });
}

bool sendSingle(uint32_t seq)
{
  // SINGLE sends one benchmark message to BENCH_ADDR
  uint32_t t0 = micros();
  bool ok = false;

  switch (g_transport)
  {
  case TRANSPORT_RAW_UDP:
  {
    uint32_t buf[4] = {(uint32_t)g_runId, seq, t0, g_prevSendCostUs};
    ok = sendUdpPacket([&]()
                       { Udp.write((const uint8_t *)buf, sizeof(buf)); });
    break;
  }
  case TRANSPORT_CNMAT:
  {
    OSCMessage msg(BENCH_ADDR);
    msg.add((int32_t)g_runId);
    msg.add((int32_t)seq);
    msg.add((int32_t)t0);
    msg.add((int32_t)g_prevSendCostUs);
    if (msg.hasError())
    {
      uint32_t cost = micros() - t0;
      msg.empty();
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, cost, false, true);
      return false;
    }
    ok = sendUdpPacket([&]()
                       { msg.send(Udp); });
    msg.empty();
    break;
  }
  case TRANSPORT_MICROOSC:
    if (!microOscUdp)
    {
      recordSend(seq, 0, false);
      return false;
    }
    microOscUdp->sendMessage(BENCH_ADDR, "iiii",
                             (int32_t)g_runId, (int32_t)seq,
                             (int32_t)t0, (int32_t)g_prevSendCostUs);
    ok = (Udp.getWriteError() == 0);
    Udp.clearWriteError();
    break;
  case TRANSPORT_LO:
  {
    lo_message msg = lo_message_new();
    if (!msg)
    {
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, 0, false, true);
      return false;
    }
    if (lo_message_add_int32(msg, (int32_t)g_runId) < 0 ||
        lo_message_add_int32(msg, (int32_t)seq) < 0 ||
        lo_message_add_int32(msg, (int32_t)t0) < 0 ||
        lo_message_add_int32(msg, (int32_t)g_prevSendCostUs) < 0)
    {
      lo_message_free(msg);
      addError(seq, ERR_BUILD_FAIL);
      uint32_t cost = micros() - t0;
      recordSend(seq, cost, false, true);
      return false;
    }
    ok = lo_send(Udp, oscIP.c_str(), oscPort, BENCH_ADDR, msg) != 0;
    lo_message_free(msg);
    break;
  }
  case TRANSPORT_OSSIA:
  {
    if (!ossiaParam)
    {
      recordSend(seq, 0, false);
      return false;
    }
    const int32_t vals[4] = {
        (int32_t)g_runId, (int32_t)seq,
        (int32_t)t0, (int32_t)g_prevSendCostUs};
    ossiaParam->push_value_list(vals, 4);
    ok = (Udp.getWriteError() == 0);
    Udp.clearWriteError();
    break;
  }
  case TRANSPORT_TINYOSC:
  {
    char buf[64];
    int32_t n = tosc_writeMessage(buf, sizeof(buf), BENCH_ADDR, "iiii",
                                  (int32_t)g_runId, (int32_t)seq,
                                  (int32_t)t0, (int32_t)g_prevSendCostUs);
    if (n <= 0)
    {
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, 0, false, true);
      return false;
    }
    ok = sendUdpPacket([&]()
                       { Udp.write((const uint8_t *)buf, (size_t)n); });
    break;
  }
  case TRANSPORT_OSCPKT:
  {
    g_oscpktMsg.init(BENCH_ADDR)
        .pushInt32((int32_t)g_runId)
        .pushInt32((int32_t)seq)
        .pushInt32((int32_t)t0)
        .pushInt32((int32_t)g_prevSendCostUs);
    if (!g_oscpktMsg.isOk())
    {
      uint32_t cost = micros() - t0;
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, cost, false, true);
      return false;
    }
    oscpkt::PacketWriter pw;
    pw.addMessage(g_oscpktMsg);
    ok = sendOscpktPacket(pw);
    break;
  }
  default:
    break;
  }

  uint32_t cost = micros() - t0;
  taskYIELD();
  recordSend(seq, cost, ok);
  return ok;
}

bool sendPayload(uint32_t seq, int extraArgs)
{
  // PAYLOAD modes keep one OSC address but vary the number of int32 arguments.
  // The first 4 ints are always the benchmark header; extra args are filler.
  uint32_t t0 = micros();
  int32_t argBuf[32];
  argBuf[0] = (int32_t)g_runId;
  argBuf[1] = (int32_t)seq;
  argBuf[2] = (int32_t)t0;
  argBuf[3] = (int32_t)g_prevSendCostUs;
  for (int i = 4; i < 4 + extraArgs; ++i)
    argBuf[i] = (int32_t)0x55555555;
  int nArgs = 4 + extraArgs;
  bool ok = false;

  switch (g_transport)
  {
  case TRANSPORT_RAW_UDP:
    ok = sendUdpPacket([&]()
                       { Udp.write((const uint8_t *)argBuf, nArgs * 4); });
    break;
  case TRANSPORT_CNMAT:
  {
    OSCMessage msg(BENCH_ADDR);
    for (int i = 0; i < nArgs; ++i)
      msg.add(argBuf[i]);
    if (msg.hasError())
    {
      msg.empty();
      uint32_t cost = micros() - t0;
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, cost, false, true);
      return false;
    }
    ok = sendUdpPacket([&]()
                       { msg.send(Udp); });
    msg.empty();
    break;
  }
  case TRANSPORT_MICROOSC:
    if (!microOscUdp)
      break;
    if (nArgs == 8)
      microOscUdp->sendMessage(BENCH_ADDR, "iiiiiiii",
                               argBuf[0], argBuf[1], argBuf[2], argBuf[3],
                               argBuf[4], argBuf[5], argBuf[6], argBuf[7]);
    else if (nArgs == 16)
      microOscUdp->sendMessage(BENCH_ADDR, "iiiiiiiiiiiiiiii",
                               argBuf[0], argBuf[1], argBuf[2], argBuf[3],
                               argBuf[4], argBuf[5], argBuf[6], argBuf[7],
                               argBuf[8], argBuf[9], argBuf[10], argBuf[11],
                               argBuf[12], argBuf[13], argBuf[14], argBuf[15]);
    else
      microOscUdp->sendMessage(BENCH_ADDR, "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii",
                               argBuf[0], argBuf[1], argBuf[2], argBuf[3],
                               argBuf[4], argBuf[5], argBuf[6], argBuf[7],
                               argBuf[8], argBuf[9], argBuf[10], argBuf[11],
                               argBuf[12], argBuf[13], argBuf[14], argBuf[15],
                               argBuf[16], argBuf[17], argBuf[18], argBuf[19],
                               argBuf[20], argBuf[21], argBuf[22], argBuf[23],
                               argBuf[24], argBuf[25], argBuf[26], argBuf[27],
                               argBuf[28], argBuf[29], argBuf[30], argBuf[31]);
    ok = (Udp.getWriteError() == 0);
    Udp.clearWriteError();
    break;
  case TRANSPORT_LO:
  {
    lo_message msg = lo_message_new();
    if (!msg)
    {
      uint32_t cost = micros() - t0;
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, cost, false, true);
      return false;
    }
    for (int i = 0; i < nArgs; ++i)
    {
      if (lo_message_add_int32(msg, argBuf[i]) < 0)
      {
        lo_message_free(msg);
        uint32_t cost = micros() - t0;
        addError(seq, ERR_BUILD_FAIL);
        recordSend(seq, cost, false, true);
        return false;
      }
    }
    ok = lo_send(Udp, oscIP.c_str(), oscPort, BENCH_ADDR, msg) != 0;
    lo_message_free(msg);
    break;
  }
  case TRANSPORT_OSSIA:
    if (!ossiaParam)
      break;
    ossiaParam->push_value_list(argBuf, nArgs);
    ok = (Udp.getWriteError() == 0);
    Udp.clearWriteError();
    break;
  case TRANSPORT_TINYOSC:
  {
    char buf[256];
    int32_t n = (nArgs == 8)
                    ? tosc_writeMessage(buf, sizeof(buf), BENCH_ADDR, "iiiiiiii",
                                        argBuf[0], argBuf[1], argBuf[2], argBuf[3],
                                        argBuf[4], argBuf[5], argBuf[6], argBuf[7])
                : (nArgs == 16)
                    ? tosc_writeMessage(buf, sizeof(buf), BENCH_ADDR, "iiiiiiiiiiiiiiii",
                                        argBuf[0], argBuf[1], argBuf[2], argBuf[3],
                                        argBuf[4], argBuf[5], argBuf[6], argBuf[7],
                                        argBuf[8], argBuf[9], argBuf[10], argBuf[11],
                                        argBuf[12], argBuf[13], argBuf[14], argBuf[15])
                    : tosc_writeMessage(buf, sizeof(buf), BENCH_ADDR, "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii",
                                        argBuf[0], argBuf[1], argBuf[2], argBuf[3],
                                        argBuf[4], argBuf[5], argBuf[6], argBuf[7],
                                        argBuf[8], argBuf[9], argBuf[10], argBuf[11],
                                        argBuf[12], argBuf[13], argBuf[14], argBuf[15],
                                        argBuf[16], argBuf[17], argBuf[18], argBuf[19],
                                        argBuf[20], argBuf[21], argBuf[22], argBuf[23],
                                        argBuf[24], argBuf[25], argBuf[26], argBuf[27],
                                        argBuf[28], argBuf[29], argBuf[30], argBuf[31]);
    if (n <= 0)
    {
      uint32_t cost = micros() - t0;
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, cost, false, true);
      return false;
    }
    ok = sendUdpPacket([&]()
                       { Udp.write((const uint8_t *)buf, n); });
    break;
  }
  case TRANSPORT_OSCPKT:
  {
    g_oscpktMsg.init(BENCH_ADDR);
    for (int i = 0; i < nArgs; ++i)
      g_oscpktMsg.pushInt32(argBuf[i]);
    if (!g_oscpktMsg.isOk())
    {
      uint32_t cost = micros() - t0;
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, cost, false, true);
      return false;
    }
    oscpkt::PacketWriter pw;
    pw.addMessage(g_oscpktMsg);
    ok = sendOscpktPacket(pw);
    break;
  }
  default:
    break;
  }

  uint32_t cost = micros() - t0;
  taskYIELD();
  recordSend(seq, cost, ok);
  return ok;
}

bool sendMultiOne(const char *addr, uint32_t seq, uint32_t tickT0)
{
  // Shared helper for MULTI: sends one message to one address
  switch (g_transport)
  {
  case TRANSPORT_RAW_UDP:
  {
    const char *slash = strrchr(addr, '/');
    int addrIdx = slash ? atoi(slash + 1) : 0;
    uint32_t wireSeq = ((uint32_t)addrIdx << 16) | (seq & 0xFFFF);
    uint32_t buf[4] = {(uint32_t)g_runId, wireSeq, tickT0, g_prevSendCostUs};
    return sendUdpPacket([&]()
                         { Udp.write((const uint8_t *)buf, sizeof(buf)); });
  }
  case TRANSPORT_CNMAT:
  {
    OSCMessage msg(addr);
    msg.add((int32_t)g_runId);
    msg.add((int32_t)seq);
    msg.add((int32_t)tickT0);
    msg.add((int32_t)g_prevSendCostUs);
    bool ok = sendUdpPacket([&]()
                            { msg.send(Udp); });
    msg.empty();
    return ok;
  }
  case TRANSPORT_MICROOSC:
    if (!microOscUdp)
      return false;
    microOscUdp->sendMessage(addr, "iiii",
                             (int32_t)g_runId, (int32_t)seq,
                             (int32_t)tickT0, (int32_t)g_prevSendCostUs);
    {
      bool ok = (Udp.getWriteError() == 0);
      Udp.clearWriteError();
      return ok;
    }
  case TRANSPORT_LO:
  {
    lo_message msg = lo_message_new();
    if (!msg)
      return false;
    if (lo_message_add_int32(msg, (int32_t)g_runId) < 0 ||
        lo_message_add_int32(msg, (int32_t)seq) < 0 ||
        lo_message_add_int32(msg, (int32_t)tickT0) < 0 ||
        lo_message_add_int32(msg, (int32_t)g_prevSendCostUs) < 0)
    {
      lo_message_free(msg);
      return false;
    }
    int ok = lo_send(Udp, oscIP.c_str(), oscPort, addr, msg);
    lo_message_free(msg);
    return ok != 0;
  }
  case TRANSPORT_OSSIA:
  {
    const char *sl = strrchr(addr, '/');
    int idx = sl ? atoi(sl + 1) : -1;
    if (idx < 0 || idx >= MAX_MULTI_ADDRS || !ossiaMultiParams[idx])
      return false;
    const int32_t vals[4] = {(int32_t)g_runId, (int32_t)seq,
                             (int32_t)tickT0, (int32_t)g_prevSendCostUs};
    ossiaMultiParams[idx]->push_value_list(vals, 4);
    bool ok = (Udp.getWriteError() == 0);
    Udp.clearWriteError();
    return ok;
  }
  case TRANSPORT_TINYOSC:
  {
    char buf[64];
    int32_t n = tosc_writeMessage(buf, sizeof(buf), addr, "iiii",
                                  (int32_t)g_runId, (int32_t)seq,
                                  (int32_t)tickT0, (int32_t)g_prevSendCostUs);
    if (n <= 0)
      return false;
    return sendUdpPacket([&]()
                         { Udp.write((const uint8_t *)buf, n); });
  }
  case TRANSPORT_OSCPKT:
  {
    g_oscpktMsg.init(addr)
        .pushInt32((int32_t)g_runId)
        .pushInt32((int32_t)seq)
        .pushInt32((int32_t)tickT0)
        .pushInt32((int32_t)g_prevSendCostUs);
    if (!g_oscpktMsg.isOk())
      return false;
    oscpkt::PacketWriter pw;
    pw.addMessage(g_oscpktMsg);
    return sendOscpktPacket(pw);
  }
  default:
    return false;
  }
}

bool sendMulti(uint32_t seq, int nAddrs)
{
  // MULTI sends one separate message per address within the same benchmark tick.
  uint32_t t0 = micros();
  int ok = 0;
  for (int i = 0; i < nAddrs; ++i)
  {
    if (sendMultiOne(MULTI_ADDRS[i], seq, t0))
      ++ok;
    taskYIELD();
  }
  uint32_t cost = micros() - t0;
  recordBatchSend(seq, cost, (uint32_t)nAddrs, (uint32_t)ok);
  return ok == nAddrs;
}

bool sendBundle(uint32_t seq, int nMsgs)
{
  // BUNDLE sends several messages in one packet. Some transports do this with
  // their native bundle API, while others use the shared manual encoder
  uint32_t t0 = micros();
  bool ok = false;

  switch (g_transport)
  {
  case TRANSPORT_RAW_UDP:
  case TRANSPORT_MICROOSC:
  {
    uint8_t buf[MANUAL_BUNDLE_CAP];
    int n = buildOscBundle(buf, sizeof(buf), seq, t0, nMsgs);
    if (n > 0)
    {
      if (g_transport == TRANSPORT_RAW_UDP)
      {
        ok = sendUdpPacket([&]()
                           { Udp.write(buf, n); });
      }
      else
      {
        Udp.beginPacket(oscIP.c_str(), oscPort);
        Udp.write(buf, n);
        ok = Udp.endPacket() != 0;
      }
    }
    else
    {
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, (uint32_t)(micros() - t0), false, true);
      return false;
    }
    break;
  }
  case TRANSPORT_TINYOSC:
  {
    char buf[MANUAL_BUNDLE_CAP];
    tosc_bundle bundle;
    tosc_writeBundle(&bundle, 1, buf, sizeof(buf));
    bool build_ok = true;
    for (int i = 0; i < nMsgs; ++i)
    {
      if (tosc_writeNextMessage(&bundle, MULTI_ADDRS[i], "iiii",
                                (int32_t)g_runId, (int32_t)seq,
                                (int32_t)t0, (int32_t)g_prevSendCostUs) == 0)
      {
        build_ok = false;
        break;
      }
    }
    if (!build_ok)
    {
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, (uint32_t)(micros() - t0), false, true);
      return false;
    }
    ok = sendUdpPacket([&]()
                       { Udp.write((const uint8_t *)buf, (size_t)bundle.bundleLen); });
    break;
  }
  case TRANSPORT_OSSIA:
  {
    if (!ossiaDevice || nMsgs > MAX_BUNDLE_MSGS)
    {
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, (uint32_t)(micros() - t0), false, true);
      return false;
    }
    const int32_t vals[4] = {
        (int32_t)g_runId, (int32_t)seq,
        (int32_t)t0, (int32_t)g_prevSendCostUs};
    ok = ossiaDevice->push_bundle(ossiaMultiParams, vals, 4, nMsgs);
    break;
  }
  case TRANSPORT_CNMAT:
  {
    if (nMsgs > MAX_BUNDLE_MSGS)
    {
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, (uint32_t)(micros() - t0), false, true);
      return false;
    }
    OSCBundle bndl;
    for (int i = 0; i < nMsgs; i++)
    {
      OSCMessage &msg = bndl.add(MULTI_ADDRS[i]);
      msg.add((int32_t)g_runId);
      msg.add((int32_t)seq);
      msg.add((int32_t)t0);
      msg.add((int32_t)g_prevSendCostUs);
    }
    if (bndl.hasError())
    {
      uint32_t cost = micros() - t0;
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, cost, false, true);
      return false;
    }
    ok = sendUdpPacket([&]()
                       { bndl.send(Udp); });
    bndl.empty();
    break;
  }
  case TRANSPORT_LO:
  {
    lo_bundle b = lo_bundle_new();
    if (!b)
    {
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, 0, false, true);
      return false;
    }
    bool build_ok = true;
    for (int i = 0; i < nMsgs && build_ok; i++)
    {
      lo_message msg = lo_message_new();
      if (!msg)
      {
        build_ok = false;
        break;
      }
      if (lo_message_add_int32(msg, (int32_t)g_runId) < 0 ||
          lo_message_add_int32(msg, (int32_t)seq) < 0 ||
          lo_message_add_int32(msg, (int32_t)t0) < 0 ||
          lo_message_add_int32(msg, (int32_t)g_prevSendCostUs) < 0)
      {
        lo_message_free(msg);
        build_ok = false;
        break;
      }
      if (lo_bundle_add_message(b, MULTI_ADDRS[i], msg) < 0)
      {
        lo_message_free(msg);
        build_ok = false;
        break;
      }
    }
    if (!build_ok)
    {
      lo_bundle_free_recursive(b);
      addError(seq, ERR_BUILD_FAIL);
      uint32_t cost = micros() - t0;
      recordSend(seq, cost, false, true);
      return false;
    }
    ok = lo_send_bundle(Udp, oscIP.c_str(), oscPort, b) != 0;
    lo_bundle_free_recursive(b);
    break;
  }
  case TRANSPORT_OSCPKT:
  {
    if (nMsgs > MAX_BUNDLE_MSGS)
    {
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, (uint32_t)(micros() - t0), false, true);
      return false;
    }
    oscpkt::PacketWriter pw;
    pw.startBundle();
    for (int i = 0; i < nMsgs; i++)
    {
      g_oscpktMsgs[i].init(MULTI_ADDRS[i]).pushInt32((int32_t)g_runId).pushInt32((int32_t)seq).pushInt32((int32_t)t0).pushInt32((int32_t)g_prevSendCostUs);
      if (!g_oscpktMsgs[i].isOk())
      {
        uint32_t cost = micros() - t0;
        addError(seq, ERR_BUILD_FAIL);
        recordSend(seq, cost, false, true);
        return false;
      }
      pw.addMessage(g_oscpktMsgs[i]);
    }
    pw.endBundle();
    if (!pw.isOk())
    {
      uint32_t cost = micros() - t0;
      addError(seq, ERR_BUILD_FAIL);
      recordSend(seq, cost, false, true);
      return false;
    }
    ok = sendOscpktPacket(pw);
    break;
  }
  default:
    break;
  }

  uint32_t cost = micros() - t0;
  taskYIELD();
  recordSend(seq, cost, ok);
  return ok;
}

bool sendBench(uint32_t seq)
{
  // Dispatch the current benchmark mode to the matching send pattern.
  switch (g_mode)
  {
  case MODE_10HZ:
  case MODE_100HZ:
  case MODE_500HZ:
  case MODE_1000HZ:
  case MODE_FAST:
  case MODE_RAMP_RATE:
    return sendSingle(seq);
  case MODE_PAY8:
    return sendPayload(seq, 4);
  case MODE_PAY16:
    return sendPayload(seq, 12);
  case MODE_RAMP_PAYLOAD:
    return sendPayload(seq, g_rampExtraArgs);
  case MODE_MULTI4:
    return sendMulti(seq, 4);
  case MODE_MULTI8:
    return sendMulti(seq, 8);
  case MODE_RAMP_MULTI:
    return sendMulti(seq, g_rampNAddrs);
  case MODE_BUNDLE2:
    return sendBundle(seq, 2);
  case MODE_BUNDLE4:
    return sendBundle(seq, 4);
  case MODE_RAMP_BUNDLE:
    return sendBundle(seq, g_rampNMsgs);
  default:
    return false;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Warmup keepalive
//
// During warmup, benchmark packets are not sent because early sends are often
// distorted by early sends are often distorted by WiFi wake-up and network priming.
// Instead, a 1-byte ping is sent every 100 ms to keep the path active.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t WARMUP_PING_MS = 100;
static uint32_t g_lastPingMs = 0;
void warmupKeepalive()
{
  if (!oscIP.empty() && oscIP != "0.0.0.0" && oscPort > 0)
  {
    uint32_t now = millis();
    if ((uint32_t)(now - g_lastPingMs) >= WARMUP_PING_MS)
    {
      g_lastPingMs = now;
      static const uint8_t ping = 0xFF;
      Udp.beginPacket(oscIP.c_str(), oscPort);
      Udp.write(&ping, 1);
      Udp.endPacket();
    }
  }
  vTaskDelay(1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Measurement scheduler
//
// FAST sends back-to-back with no target interval.
// All other modes fire against g_nextSchedUs, an absolute schedule target.
//   - If the next send is still >1.5 ms away, sleep so WiFi/lwIP work can run.
//   - If it is close, busy-wait for tighter timing.
// ─────────────────────────────────────────────────────────────────────────────

void measureSend()
{
  if (oscIP.empty() || oscIP == "0.0.0.0" || oscPort <= 0)
  {
    vTaskDelay(1);
    return;
  }

  if (g_mode == MODE_FAST)
  {
    sendBench(g_seq++);
  }
  else
  {
    uint32_t nowUs = micros();
    int32_t remaining = (int32_t)(g_nextSchedUs - nowUs);
    if (remaining > (int32_t)BUSYWAIT_THRESHOLD_US)
    {
      vTaskDelay(1);
      return;
    }
    while ((int32_t)(micros() - g_nextSchedUs) < 0)
    { /* busy-wait */
    }
    g_nextSchedUs += modeIntervalUs(g_mode);
    sendBench(g_seq++);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup / loop
//
// setup() configures the shared network environment once.
// loop() runs a simple state machine: warmup -> measure -> report -> advance.
// ─────────────────────────────────────────────────────────────────────────────

void setup()
{
  Serial.begin(115200);
  delay(1000);

  puara.start();
  loadSettings();
  if (localPort > 0)
    Udp.begin(localPort);
  if (!oscIP.empty() && oscIP != "0.0.0.0" && oscPort > 0)
  {
    IPAddress destIP;
    destIP.fromString(oscIP.c_str());
    // Create objects for MicroOscUdp and OssiaDevice.
    microOscUdp = new MicroOscUdp<256>(Udp, destIP, (unsigned int)oscPort);
    ossiaDevice = new OssiaDevice(Udp, oscIP.c_str(), oscPort);
    ossiaParam = ossiaDevice->make_parameter(BENCH_ADDR, OSSIA_LIST);
    for (int i = 0; i < MAX_MULTI_ADDRS; ++i)
      ossiaMultiParams[i] = ossiaDevice->make_parameter(MULTI_ADDRS[i], OSSIA_LIST);
  }
  WiFi.onEvent(onWiFiEvent);

  // Disable WiFi power saving
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Silence NetworkUdp pbuf spam.
  esp_log_level_set("NetworkUdp", ESP_LOG_NONE);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);

  Serial.println();
  Serial.println("=== OSC benchmark: RAW_UDP|CNMAT|MICROOSC|LO|OSSIA|TINYOSC|OSCPKT ===");
  Serial.printf("=== Fixed phase: %d passes × %d modes × %d transports ===\n", N_PASSES, N_FIXED_MODES, N_ALL_TRANSPORTS);
  Serial.println("=== Ramp phase: RATE→PAYLOAD→MULTI→BUNDLE, per transport, ramp to failure ===");
  Serial.printf("oscIP=%s  oscPort=%d  localPort=%d\n",
                oscIP.c_str(), oscPort, localPort);
  printSerialHeader();

  g_transport = ALL_TRANSPORTS[0];
  beginMode(FIXED_MODES[0]);
}

void loop()
{
  if (g_phase == PHASE_DONE)
  {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    return;
  }

  uint32_t nowMs = millis();

  // ── Warmup ─────────────────────────────────────────────────────────────────
  if (g_inWarmup)
  {
    uint32_t warmupMs = currentWarmupMs();
    if ((uint32_t)(nowMs - g_modeStartMs) >= warmupMs)
    {
      g_inWarmup = false;
      g_modeStartMs = millis();
      g_nextSchedUs = micros();
      resetModeState();
      Serial.printf("=== MEASURE START [run=%u] [%s] %s ===\n",
                    (unsigned)g_runId, transportName(g_transport), modeName(g_mode));
      logWiFiSnapshot("start");
      return;
    }
    warmupKeepalive();
    return;
  }

  // ── Measurement window ─────────────────────────────────────────────────────
  uint32_t durationMs = currentMeasureDurationMs();
  bool done = ((uint32_t)(nowMs - g_modeStartMs) >= durationMs);

  if (done)
  {
    dumpModeReport();
    advanceToNext();
    return;
  }

  measureSend();
}

#ifndef Arduino_h
extern "C"
{
  void app_main(void);
}
void app_main()
{
  setup();
  while (1)
  {
    loop();
  }
}
#endif
