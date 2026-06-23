#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include "WebUI.h"
#include "WireGuardManager.h"

// External variables and functions from main (.ino) – ESP32 RTSP Mic for BirdNET-Go
extern WiFiServer rtspServer;
extern WiFiClient rtspClient;
extern volatile bool isStreaming;
extern uint16_t rtpSequence;
extern uint32_t rtpTimestamp;
extern unsigned long lastStatsReset;
extern unsigned long lastRtspPlayMs;
extern uint32_t rtspPlayCount;
extern unsigned long lastRtspClientConnectMs;
extern unsigned long bootTime;
extern unsigned long lastRTSPActivity;
extern unsigned long lastWiFiCheck;
extern unsigned long lastTempCheck;
extern uint32_t minFreeHeap;
extern float maxTemperature;
extern bool rtspServerEnabled;
extern uint32_t audioPacketsSent;
extern uint32_t currentSampleRate;
extern float currentGainFactor;
extern uint16_t currentBufferSize;
extern uint8_t i2sShiftBits;
extern uint32_t minAcceptableRate;
extern uint32_t performanceCheckInterval;
extern bool autoRecoveryEnabled;
extern uint8_t cpuFrequencyMhz;
extern wifi_power_t currentWifiPowerLevel;
extern void resetToDefaultSettings();
extern bool autoThresholdEnabled;
extern uint32_t computeRecommendedMinRate();
extern bool scheduledResetEnabled;
extern uint32_t resetIntervalHours;
extern void scheduleReboot(bool factoryReset, uint32_t delayMs);
extern uint16_t lastPeakAbs16;
extern uint32_t audioClipCount;
extern bool audioClippedLastBlock;
extern uint16_t peakHoldAbs16;
extern bool overheatProtectionEnabled;
extern float overheatShutdownC;
extern bool overheatLockoutActive;
extern float overheatTripTemp;
extern unsigned long overheatTriggeredAt;
extern String overheatLastReason;
extern String overheatLastTimestamp;
extern bool overheatSensorFault;
extern float lastTemperatureC;
extern bool lastTemperatureValid;
extern bool overheatLatched;
extern bool agcEnabled;
extern volatile float agcMultiplier;
extern uint8_t ledMode;
extern String deviceHostname;
extern String normalizeHostname(const String& raw);
extern void scheduleReboot(bool factoryReset, uint32_t delayMs);

// Local helper: snap requested Wi‑Fi TX power (dBm) to nearest supported step
static float snapWifiTxDbm(float dbm) {
    static const float steps[] = {-1.0f, 2.0f, 5.0f, 7.0f, 8.5f, 11.0f, 13.0f, 15.0f, 17.0f, 18.5f, 19.0f, 19.5f};
    float best = steps[0];
    float bestd = fabsf(dbm - steps[0]);
    for (size_t i=1;i<sizeof(steps)/sizeof(steps[0]);++i){
        float d = fabsf(dbm - steps[i]);
        if (d < bestd){ bestd = d; best = steps[i]; }
    }
    return best;
}

static const uint32_t OH_MIN = 30;
static const uint32_t OH_MAX = 95;
static const uint32_t OH_STEP = 5;

// Async reboot/factory-reset task to avoid restarting from HTTP context
static void rebootTask(void* arg){
    bool doFactory = ((uintptr_t)arg) != 0;
    if (doFactory) {
        resetToDefaultSettings();
    }
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP.restart();
    vTaskDelete(NULL);
}

// Helper functions in main
extern float wifiPowerLevelToDbm(wifi_power_t lvl);
extern String formatUptime(unsigned long seconds);
extern String formatSince(unsigned long eventMs);
extern void restartI2S();
extern void saveAudioSettings();
extern void applyWifiTxPower(bool log);
extern const char* FW_VERSION_STR;

// Web server and in-memory log ring buffer
static WebServer web(80);
static const size_t LOG_CAP = 80;
static String logBuffer[LOG_CAP];
static size_t logHead = 0;
static size_t logCount = 0;

extern portMUX_TYPE logMux;

void webui_pushLog(const String &line) {
    portENTER_CRITICAL(&logMux);
    logBuffer[logHead] = line;
    logHead = (logHead + 1) % LOG_CAP;
    if (logCount < LOG_CAP) logCount++;
    portEXIT_CRITICAL(&logMux);
}

static String jsonEscape(const String &s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i]; if(c=='"'||c=='\\'){o+='\\';o+=c;} else if(c=='\n'){o+="\\n";} else {o+=c;}}
    return o;
}

static String profileName(uint16_t buf) {
    // Server-side fallback (English). UI localizes on client by buffer size.
    if (buf <= 256) return F("Ultra-Low Latency (Higher CPU, May have dropouts)");
    if (buf <= 512) return F("Balanced (Moderate CPU, Good stability)");
    if (buf <= 1024) return F("Stable Streaming (Lower CPU, Excellent stability)");
    return F("High Stability (Lowest CPU, Maximum stability)");
}

static void apiSendJSON(const String &json) {
    web.sendHeader("Cache-Control", "no-cache");
    web.send(200, "application/json", json);
}

// HTML UI
static String htmlIndex() {
    String ip = WiFi.localIP().toString();
    String h;
    h += F("<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>M5Stack Atom Echo - ");
    h += deviceHostname;
    h += F("</title>"

        "<style>:root{--bg:#0b1020;--fg:#e7ebf2;--muted:#9aa3b2;--card:#121a2e;--border:#1b2745;--acc:#4ea1f3;--acc2:#36d399;--warn:#f59e0b;--bad:#ef4444}"
"body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;background:linear-gradient(180deg,#0b1020 0%,#0f1530 100%);color:var(--fg)}"
"*,*::before,*::after{box-sizing:border-box}"
".page{max-width:1000px;margin:0 auto;padding:16px}"
        ".hero{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
        ".brand{display:flex;align-items:center;gap:10px;flex-wrap:wrap}"
        ".title{font-weight:700;font-size:18px;letter-spacing:.2px} .subtitle{color:var(--muted);font-size:13px}"
        ".badge{display:inline-block;border:1px solid var(--border);color:var(--muted);padding:2px 6px;border-radius:8px;font-size:12px;margin-left:8px}"
        ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:12px;margin-bottom:12px;box-shadow:0 1px 1px rgba(0,0,0,.2)}"
        ".actions button{box-shadow:inset 0 0 0 1px var(--border),0 1px 2px rgba(0,0,0,.18);font-weight:500;letter-spacing:.15px}"
        ".actions button.warning{border-color:var(--warn);color:var(--warn)}"
        ".row{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:12px} h1{font-size:20px;margin:0 0 4px} h2{font-size:15px;margin:4px 0 10px;color:var(--muted);font-weight:600;letter-spacing:.2px}"
        "table{width:100%;border-collapse:collapse} td{padding:8px 6px;border-bottom:1px solid var(--border)} td.k{color:var(--muted);width:44%} td.v{font-weight:600}"
        "button,select,input{font:inherit;padding:8px 10px;border-radius:10px;border:1px solid var(--border);background:#0d1427;color:var(--fg)}"
        "button{background:#0e152a} button:hover{border-color:var(--acc)} button.active{background:var(--acc);color:#061120;border-color:#2a7dd4}"
        ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px;margin-bottom:12px} .ok{color:var(--acc2)} .warn{color:var(--warn)} .bad{color:var(--bad)} .lang{float:right} .mono{font-family:ui-monospace,Consolas,Menlo,monospace}"
        "input[type=number]{width:130px} select{min-width:110px} .muted{color:var(--muted)}"
        ".field{display:flex;align-items:center;gap:8px} .unit{color:var(--muted);font-size:12px} .help{display:inline-flex;align-items:center;justify-content:center;width:16px;height:16px;border:1px solid var(--acc);border-radius:50%;font-size:12px;color:var(--fg);margin-left:6px;background:#0a1224;cursor:pointer} .help:hover{filter:brightness(1.1)} .hint{margin-top:6px;padding:8px;border:1px solid var(--border);border-radius:8px;background:#0d162c;color:var(--fg);font-size:12px;line-height:1.35}"
        ".dirty{border-color:var(--bad)!important; box-shadow:0 0 0 2px rgba(239,68,68,.25) inset; background:#1a0d12}"
        ".gh{margin-right:10px;color:var(--acc);text-decoration:none;border:1px solid var(--border);padding:4px 8px;border-radius:8px} .gh:hover{border-color:var(--acc)}"
        "pre{white-space:pre-wrap;word-break:break-word;background:#0c1325;border:1px solid var(--border);border-radius:10px;padding:10px;overflow:auto} pre#logs{height:45vh}"
".overlay{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.6);z-index:9999} .overlay .box{background:var(--card);border:1px solid var(--border);padding:16px 20px;border-radius:12px;color:var(--fg);text-align:center;min-width:260px}"
".subtitle a{color:var(--acc);text-decoration:underline;overflow-wrap:anywhere} .subtitle a:hover,.subtitle a:active{color:var(--fg)}"
".copy-btn{background:none;border:none;padding:2px;cursor:pointer;line-height:1;vertical-align:middle;color:var(--acc)} .copy-btn svg{display:block}"
"@media(max-width:600px){.hero{flex-direction:column;align-items:flex-start;gap:8px}.lang{float:none;margin-top:8px}.field{flex-wrap:nowrap;align-items:center}.field input,.field select{flex:1 1 0%;min-width:0;width:auto}.field button{flex:0 0 auto;width:auto;margin-top:0}.unit{flex:0 0 auto}input[type=number]{width:auto}select{min-width:0}.actions{flex-direction:column}.actions button{width:100%;min-height:44px}table{table-layout:fixed}td{overflow-wrap:anywhere}button,select,input{cursor:pointer}.page{padding:10px}.card{padding:10px}}"
        "</style></head><body>"
        "<div id='ovr' class='overlay'><div class='box' id='ovr_msg'>Restarting…</div></div>"
"<div id='confirm_overlay' class='overlay' style='display:none'><div class='box'><p id='confirm_msg' style='margin:0 0 12px'></p><div style='display:flex;gap:8px;justify-content:center'><button id='confirm_cancel'>Cancel</button><button id='confirm_ok' class='warning'>Confirm</button></div></div></div>"
        "<div class='page'>"
        "<div class='card'><div class='hero'><div><div class='brand'><div class='title' id='t_title'>M5Stack Atom Echo</div><span class='badge' id='fwv'></span></div><div class='subtitle'><span id='t_lan_url'>LAN URL</span>: <a id='rtsp' class='mono' href='rtsp://");
    h += ip;
    h += F(
        ":8554/' target='_blank'>rtsp://");
    h += ip;
    h += F(
        ":8554/</a><button onclick=\"copyUrl('rtsp','copy_btn_lan')\" id='copy_btn_lan' class='copy-btn' title='copy'><svg width='14' height='14' viewBox='0 0 16 16' fill='none' stroke='currentColor' stroke-width='1.5'><rect x='5.5' y='5.5' width='8' height='8' rx='1.5'/><path d='M10.5 5.5V3a1.5 1.5 0 00-1.5-1.5H3A1.5 1.5 0 001.5 3v6A1.5 1.5 0 003 10.5h2.5'/></svg></button><div id='wg_url_row' style='display:none'><span id='t_wg_url'>WireGuard URL</span>: <a id='rtsp_wg_url' class='mono' href='' target='_blank'></a><button onclick=\"copyUrl('rtsp_wg_url','copy_btn_wg')\" id='copy_btn_wg' class='copy-btn' title='copy'><svg width='14' height='14' viewBox='0 0 16 16' fill='none' stroke='currentColor' stroke-width='1.5'><rect x='5.5' y='5.5' width='8' height='8' rx='1.5'/><path d='M10.5 5.5V3a1.5 1.5 0 00-1.5-1.5H3A1.5 1.5 0 001.5 3v6A1.5 1.5 0 003 10.5h2.5'/></svg></button></div></div></div>"
        "<div class='lang'><a href='https://github.com/FirbyKirby/birdnetgo-m5stack-atom-echo-rtsp-mic' target='_blank' class='gh'>GitHub</a>Lang: <select id='langSel'><option value='en'>English</option><option value='cs'>Čeština</option></select></div></div></div>"
        "<div class='row'>"
        "<div class='card'><h2 id='t_status'>Status</h2><table>"
        "<tr><td class='k' id='t_ip'>IP Address</td><td class='v' id='ip'></td></tr>"
        "<tr><td class='k' id='t_wifi_rssi'>WiFi RSSI</td><td class='v' id='rssi'></td></tr>"
        "<tr><td class='k' id='t_wifi_tx'>WiFi TX Power</td><td class='v' id='wtx'></td></tr>"
        "<tr><td class='k' id='t_heap'>Free Heap (min)</td><td class='v' id='heap'></td></tr>"
        "<tr><td class='k' id='t_uptime'>Uptime</td><td class='v' id='uptime'></td></tr>"
        "<tr><td class='k' id='t_rtsp_server'>RTSP Server</td><td class='v' id='srv'></td></tr>"
        "<tr><td class='k' id='t_client'>Client</td><td class='v' id='client'></td></tr>"
        "<tr><td class='k' id='t_streaming'>Streaming</td><td class='v' id='stream'></td></tr>"
        "<tr><td class='k' id='t_pkt_rate'>Packet Rate</td><td class='v' id='rate'></td></tr>"
        "<tr><td class='k' id='t_last_connect'>Last RTSP Connect</td><td class='v' id='lcon'></td></tr>"
        "<tr><td class='k' id='t_last_play'>Last Stream Start</td><td class='v' id='lplay'></td></tr>"
        "</table><div class='actions'>"
        "<button onclick=\"toggleServer()\" id='b_srv_toggle' class='active'>Server: ON</button>"
        "<button onclick=\"act('reset_i2s')\" id='b_reset'>Reset I2S</button>"
        "<button onclick=\"rebootNow()\" id='b_reboot'>Reboot</button>"
        "<button onclick=\"defaultsNow()\" id='b_factory_reset' class='warning'>Factory Reset</button>"
        "<button onclick=\"shipReadyReset()\" id='b_ship_ready' class='warning'>Reset Wi-Fi</button>"
        "<div id='adv' class='footer muted'></div></div>"

        "<div class='card'><h2 id='t_device'>Device</h2><table>"
        "<tr><td class='k'><span id='t_hostname'>Hostname</span><span class='help' id='h_hostname'>?</span></td><td class='v'><div class='field'><input id='in_hostname' type='text' size='20' maxlength='63' placeholder='atomecho-XXXXXX'><button id='btn_hostname_set' onclick=\"setv('hostname',in_hostname.value)\">Set</button></div></td></tr>"
        "<tr id='row_hostname_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hostname_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hostname_preview'>Preview</span></td><td class='v'><code id='hostname_preview'>atomecho.local</code></td></tr>"
        "<tr><td colspan='2'><div class='muted' id='t_hostname_note'>Device will reboot to apply changes.</div></td></tr>"
        "</table></div>"

        "<div class='card'><h2 id='t_audio'>Audio</h2><table>"
        "<tr><td class='k'><span id='t_rate'>Sample Rate</span><span class='help' id='h_rate'>?</span><div class='hint' id='rate_hint' style='display:none'></div></td><td class='v'><div class='field'><input id='in_rate' type='number' step='0.1' min='8' max='96'><span class='unit'>kHz</span><button id='btn_rate_set' onclick=\"setv('rate',in_rate.value)\">Set</button></div></td></tr>"
        "<tr id='row_rate_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_rate_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_gain'>Gain</span><span class='help' id='h_gain'>?</span></td><td class='v'><div class='field'><input id='in_gain' type='number' step='0.1' min='0.1' max='100'><span class='unit'>×</span><button id='btn_gain_set' onclick=\"setv('gain',in_gain.value)\">Set</button></div></td></tr>"
        "<tr id='row_gain_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_gain_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hpf'>High-pass</span><span class='help' id='h_hpf'>?</span></td><td class='v'><div class='field'><select id='sel_hp'><option value='off'>OFF</option><option value='on'>ON</option></select><button onclick=\"setv('hp_enable',sel_hp.value)\">Set</button></div></td></tr>"
        "<tr id='row_hpf_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hpf_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hpf_cut'>HPF Cutoff</span><span class='help' id='h_hpf_cut'>?</span></td><td class='v'><div class='field'><input id='in_hp_cutoff' type='number' step='10' min='10' max='10000'><span class='unit'>Hz</span><button onclick=\"setv('hp_cutoff',in_hp_cutoff.value)\">Set</button></div></td></tr>"
        "<tr id='row_hpf_cut_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hpf_cut_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_agc'>AGC</span><span class='help' id='h_agc'>?</span></td><td class='v'><div class='field'><select id='sel_agc'><option value='off'>OFF</option><option value='on'>ON</option></select><button id='btn_agc_set' onclick=\"setv('agc_enable',sel_agc.value)\">Set</button><span class='unit' id='agc_info'></span></div></td></tr>"
        "<tr id='row_agc_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_agc_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_led'>LED Mode</span><span class='help' id='h_led'>?</span></td><td class='v'><div class='field'><select id='sel_led'><option value='0'>OFF</option><option value='1' selected>Static</option><option value='2'>Level</option></select><button id='btn_led_set' onclick=\"setv('led_mode',sel_led.value)\">Set</button></div></td></tr>"
        "<tr id='row_led_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_led_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_buf'>Buffer Size</span><span class='help' id='h_buf'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_buf'><option>256</option><option>512</option><option selected>1024</option><option>2048</option><option>4096</option><option>8192</option></select>"
        "<span class='unit'>samples</span><button id='btn_buf_set' onclick=\"setv('buffer',sel_buf.value)\">Set</button></div></td></tr>"
        "<tr id='row_buf_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_buf_hint'></div></td></tr>"
        "<tr><td class='k' id='t_latency'>Latency</td><td class='v' id='lat'></td></tr>"
        "<tr><td class='k'><span id='t_level'>Signal Level</span><span class='help' id='h_level'>?</span></td><td class='v' id='level'></td></tr>"
        "<tr id='row_level_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_level_hint'></div></td></tr>"
        "<tr><td class='k' id='t_profile'>Profile</td><td class='v' id='profile'></td></tr>"
        "</table></div>"

        "<div class='card'><h2 id='t_perf'>Reliability</h2><table>"
        "<tr><td class='k'><span id='t_auto'>Auto Recovery</span><span class='help' id='h_auto'>?</span></td><td class='v'><div class='field'><select id='in_auto'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_auto_set' onclick=\"setv('auto_recovery',in_auto.value)\">Set</button></div></td></tr>"
        "<tr id='row_auto_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_auto_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_thr_mode'>Threshold Mode</span><span class='help' id='h_thr_mode'>?</span></td><td class='v'><div class='field'><select id='in_thr_mode'><option value='auto'>Auto</option><option value='manual'>Manual</option></select><button id='btn_thrmode_set' onclick=\"setv('thr_mode',in_thr_mode.value)\">Set</button></div></td></tr>"
        "<tr id='row_thrmode_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_thr_mode_hint'></div></td></tr>"
        "<tr id='row_thr_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_thr_hint'></div></td></tr>"
        "<tr id='row_min_rate'><td class='k'><span id='t_thr'>Restart Threshold</span><span class='help' id='h_thr'>?</span></td><td class='v'><div class='field'><input id='in_thr' type='number' step='1' min='5' max='200'><span class='unit'>pkt/s</span><button id='btn_thr_set' onclick=\"setv('min_rate',in_thr.value)\">Set</button></div></td></tr>"
        "<tr><td class='k'><span id='t_sched'>Scheduled Reset</span><span class='help' id='h_sched'>?</span></td><td class='v'><div class='field'><select id='in_sched'><option value='on'>ON</option><option value='off' selected>OFF</option></select><button id='btn_sched_set' onclick=\"setv('sched_reset',in_sched.value)\">Set</button></div></td></tr>"
        "<tr id='row_sched_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_sched_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hours'>Reset After</span><span class='help' id='h_hours'>?</span></td><td class='v'><div class='field'><input id='in_hours' type='number' step='1' min='1' max='168'><span class='unit'>h</span><button id='btn_hours_set' onclick=\"setv('reset_hours',in_hours.value)\">Set</button></div></td></tr>"
        "<tr id='row_hours_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hours_hint'></div></td></tr>"
        "</table></div>"

        ""

        "<div class='card'><h2 id='t_thermal'>Thermal</h2><table>"
        "<tr><td class='k'><span id='t_therm_protect'>Overheat Protection</span><span class='help' id='h_therm_protect'>?</span></td><td class='v'><div class='field'><select id='sel_oh_enable'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_oh_enable' onclick=\"setv('oh_enable',sel_oh_enable.value)\">Set</button></div></td></tr>"
        "<tr id='row_therm_hint_protect' style='display:none'><td colspan='2'><div class='hint' id='txt_therm_hint_protect'></div></td></tr>"
        "<tr><td class='k'><span id='t_therm_limit'>Shutdown Limit</span><span class='help' id='h_therm_limit'>?</span></td><td class='v'><div class='field'><select id='sel_oh_limit'><option>30</option><option>35</option><option>40</option><option>45</option><option>50</option><option>55</option><option>60</option><option>65</option><option>70</option><option>75</option><option selected>80</option><option>85</option><option>90</option><option>95</option></select><span class='unit'>&deg;C</span><button id='btn_oh_limit' onclick=\"setv('oh_limit',sel_oh_limit.value)\">Set</button></div></td></tr>"
        "<tr id='row_therm_hint_limit' style='display:none'><td colspan='2'><div class='hint' id='txt_therm_hint_limit'></div></td></tr>"
        "<tr><td class='k' id='t_therm_status'>Status</td><td class='v' id='therm_status'></td></tr>"
        "<tr><td class='k' id='t_therm_now'>Current Temp</td><td class='v' id='therm_now'></td></tr>"
        "<tr><td class='k' id='t_therm_max'>Peak Temp</td><td class='v' id='therm_max'></td></tr>"
"<tr><td class='k' id='t_therm_cpu'>CPU Clock</td><td class='v' id='therm_cpu'></td></tr>"
"<tr><td class='k'><span id='t_therm_last'>Last Shutdown</span></td><td class='v'><div id='therm_last' class='hint'></div></td></tr>"
"<tr id='row_therm_latch' style='display:none'><td colspan='2'><div class='hint warn' id='txt_therm_latch'></div><div class='field' style='margin-top:8px'><button id='btn_therm_clear' class='danger' onclick=\"clearThermalLatch()\"></button></div></td></tr>"
"</table></div>"

        "<div id='advsec'>"
        "<div class='card'><h2 id='t_advanced_settings'>Advanced Settings</h2><table>"
        "<tr><td class='k'><span id='t_shift'>I2S Shift</span><span class='help' id='h_shift'>?</span></td><td class='v'><span id=.val_shift. class=.val.>0</span> bits <span style=.color:#888;font-size:0.9em.>(fixed for PDM)</span></td></tr>"
        "<tr id='row_shift_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_shift_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_chk'>Check Interval</span><span class='help' id='h_chk'>?</span></td><td class='v'><div class='field'><input id='in_chk' type='number' step='1' min='1' max='60'><span class='unit'>min</span><button id='btn_chk_set' onclick=\"setv('check_interval',in_chk.value)\">Set</button></div></td></tr>"
        "<tr id='row_chk_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_chk_hint'></div></td></tr>"
        "<tr id='row_tx_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_tx_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wifi_tx2'>TX Power</span><span class='help' id='h_tx'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_tx'><option>-1.0</option><option>2.0</option><option>5.0</option><option>7.0</option><option>8.5</option><option>11.0</option><option>13.0</option><option selected>15.0</option><option>17.0</option><option>18.5</option><option>19.0</option><option>19.5</option></select>"
        "<span class='unit'>dBm</span><button id='btn_tx_set' onclick=\"setv('wifi_tx',sel_tx.value)\">Set</button></div></td></tr>"
        "<tr><td class='k'><span id='t_cpu'>CPU Frequency</span><span class='help' id='h_cpu'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_cpu'><option>80</option><option>120</option><option>160</option><option>240</option></select><span class='unit'>MHz</span><button id='btn_cpu_set' onclick=\"setv('cpu_freq',sel_cpu.value)\">Set</button></div></td></tr>"
        "<tr id='row_cpu_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_cpu_hint'></div></td></tr>"
        "</table></div>"
        "</div>"

        "<div class='card'><h2 id='t_wg'>WireGuard</h2>"
        "<div class='field' style='margin-bottom:6px'><input id='in_wg_conf' type='file' accept='.conf,text/plain' onchange='wgImportPicker(event)' style='display:none'><button onclick=\"in_wg_conf.click()\" id='b_wg_import'>Import Config</button><span class='help' id='h_wg_import' style='margin-left:4px'>?</span></div>"
        "<div id='row_wg_import_hint' style='display:none'><div class='hint' id='txt_wg_import_hint'></div></div>"
        "<div id='wg_import_panel' class='hint' style='display:none;margin-bottom:6px'></div>"
        "<table>"
        "<tr><td class='k'><span id='t_wg_enable'>Enabled</span><span class='help' id='h_wg_enable'>?</span></td><td class='v'><div class='field'><select id='sel_wg_enable'><option value='on'>ON</option><option value='off'>OFF</option></select><button onclick=\"setv('wg_enable',sel_wg_enable.value)\">Set</button></div></td></tr>"
        "<tr id='row_wg_enable_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_wg_enable_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wg_priv'>Private Key</span><span class='help' id='h_wg_priv'>?</span></td><td class='v'><div class='field'><input id='in_wg_priv' type='password' size='44' maxlength='64'><button onclick=\"setv('wg_priv',in_wg_priv.value)\">Set</button></div></td></tr>"
        "<tr id='row_wg_priv_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_wg_priv_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wg_srvpub'>Server Public Key</span><span class='help' id='h_wg_srvpub'>?</span></td><td class='v'><div class='field'><input id='in_wg_srvpub' type='text' size='44' maxlength='64'><button onclick=\"setv('wg_srvpub',in_wg_srvpub.value)\">Set</button></div></td></tr>"
        "<tr id='row_wg_srvpub_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_wg_srvpub_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wg_endpoint'>Endpoint</span><span class='help' id='h_wg_endpoint'>?</span></td><td class='v'><div class='field'><input id='in_wg_endpoint' type='text' size='30' placeholder='host:port'><button onclick=\"setv('wg_endpoint',in_wg_endpoint.value)\">Set</button></div></td></tr>"
        "<tr id='row_wg_endpoint_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_wg_endpoint_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wg_tunaddr'>Tunnel IP</span><span class='help' id='h_wg_tunaddr'>?</span></td><td class='v'><div class='field'><input id='in_wg_tunaddr' type='text' size='20' placeholder='10.6.0.5/24'><button onclick=\"setv('wg_tunaddr',in_wg_tunaddr.value)\">Set</button></div></td></tr>"
        "<tr id='row_wg_tunaddr_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_wg_tunaddr_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wg_keepalive'>Keepalive</span><span class='help' id='h_wg_keepalive'>?</span></td><td class='v'><div class='field'><input id='in_wg_keepalive' type='number' step='1' min='0' max='65535'><span class='unit'>s</span><button onclick=\"setv('wg_keepalive',in_wg_keepalive.value)\">Set</button></div></td></tr>"
        "<tr id='row_wg_keepalive_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_wg_keepalive_hint'></div></td></tr>"
        "</table></div>"

        "<div class='card'><h2 id='t_wg_status'>WireGuard Status</h2><table>"
        "<tr><td class='k' id='t_wg_state'>Tunnel State</td><td class='v' id='wg_state'></td></tr>"
        "<tr><td class='k' id='t_wg_last_hs'>Last Handshake</td><td class='v' id='wg_last_hs'></td></tr>"
        "<tr><td class='k' id='t_wg_rx'>Bytes Received</td><td class='v' id='wg_rx'></td></tr>"
        "<tr><td class='k' id='t_wg_tx'>Bytes Transmitted</td><td class='v' id='wg_tx'></td></tr>"
        "</table>"
        "<div class='hint muted' style='margin-top:8px' id='t_wg_remote_note'>Admin can reach this UI at http://&lt;tunnel-ip&gt;/ from any WireGuard peer.</div>"
        "</div>"


        "<div class='card'><div style='display:flex;justify-content:space-between;align-items:center'><h2 id='t_logs'>Logs</h2><button id='btn_copy_logs' onclick='copyLogs()' title='Copy logs' style='background:none;border:1px solid var(--border);padding:4px 8px;cursor:pointer;border-radius:8px;line-height:1'><svg width='16' height='16' viewBox='0 0 16 16' fill='none' stroke='currentColor' stroke-width='1.5'><rect x='5.5' y='5.5' width='8' height='8' rx='1.5'/><path d='M10.5 5.5V3a1.5 1.5 0 00-1.5-1.5H3A1.5 1.5 0 001.5 3v6A1.5 1.5 0 003 10.5h2.5'/></svg></button></div><pre id='logs' class='mono'></pre></div>"

        "</div>"
        "</div>"
        "<script src='/gui.js'></script>"
        "</script></body></html>");
    return h;
}

// HTTP handlery
/*
 * httpIndex() serves a <3 KB HTML shell and delegates the ~45 KB JS payload
 * to /gui.js (see webui_begin endpoint registration). The shell fits in a single
 * lwIP TX window so the simple web.send() path is safe.
 *
 * The JS used to live inline in htmlIndex() (~65 KB total response), which
 * silently truncated on the ESP32 Arduino WebServer because WiFiClient::write()
 * silently drops data when the lwIP tcp_write() memory pool is exhausted —
 * even with small chunk sizes and retries (no error, no exception; Safari just
 * parses the partial body it received and stops). Moving the JS to SPIFFS and
 * serving it via web.streamFile() uses File::read() with proper lwIP
 * backpressure (blocks for ACK space instead of dropping data).
 *
 * If you see a blank page with just the hero card, the most likely cause is
 * SPIFFS not mounted: check serial output for "SPIFFS mounted" and ensure the
 * filesystem was uploaded with `pio run -t uploadfs` after `pio run -t upload`.
 */
static void httpIndex() {
    String h;
    h.reserve(4096);
    h = htmlIndex();
    web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, proxy-revalidate, max-age=0");
    web.sendHeader("Pragma", "no-cache");
    web.sendHeader("Expires", "0");
    web.send(200, "text/html; charset=utf-8", h);
}

/*
 * /gui.js — serves the UI script from SPIFFS.
 * web.streamFile() reads 1024 B per call and blocks for lwIP ACK space,
 * so the 45 KB JS is delivered reliably over multiple TCP windows.
 */
static void httpGuiJs() {
    if (!SPIFFS.exists("/gui.js")) {
        web.send(404, "text/plain", F("GUI asset not found. Run: pio run -t uploadfs"));
        return;
    }
    File f = SPIFFS.open("/gui.js", "r");
    if (!f) {
        web.send(500, "text/plain", F("SPIFFS read failed"));
        return;
    }
    web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, proxy-revalidate, max-age=0");
    web.sendHeader("Pragma", "no-cache");
    web.sendHeader("Expires", "0");
    web.streamFile(f, "application/javascript; charset=utf-8");
    f.close();
}

static void httpStatus() {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    String uptimeStr = formatUptime(uptimeSeconds);
    unsigned long runtime = millis() - lastStatsReset;
    uint32_t currentRate = (isStreaming && runtime > 1000) ? (audioPacketsSent * 1000) / runtime : 0;
    String json = "{";
    json += "\"fw_version\":\"" + String(FW_VERSION_STR) + "\",";
    json += "\"hostname\":\"" + deviceHostname + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel),1) + ",";
    json += "\"free_heap_kb\":" + String(ESP.getFreeHeap()/1024) + ",";
    json += "\"min_free_heap_kb\":" + String(minFreeHeap/1024) + ",";
    json += "\"uptime\":\"" + uptimeStr + "\",";
    json += "\"rtsp_server_enabled\":" + String(rtspServerEnabled?"true":"false") + ",";
    if (rtspClient && rtspClient.connected()) json += "\"client\":\"" + rtspClient.remoteIP().toString() + "\","; else json += "\"client\":\"\",";
    json += "\"streaming\":" + String(isStreaming?"true":"false") + ",";
    json += "\"current_rate_pkt_s\":" + String(currentRate) + ",";
    json += "\"last_rtsp_connect\":\"" + jsonEscape(formatSince(lastRtspClientConnectMs)) + "\",";
    json += "\"last_stream_start\":\"" + jsonEscape(formatSince(lastRtspPlayMs)) + "\"";
    json += "}";
    apiSendJSON(json);
}

static void httpAudioStatus() {
    float latency_ms = (float)currentBufferSize / currentSampleRate * 1000.0f;
    String json = "{";
    json += "\"sample_rate\":" + String(currentSampleRate) + ",";
    json += "\"gain\":" + String(currentGainFactor,2) + ",";
    json += "\"buffer_size\":" + String(currentBufferSize) + ",";
    json += "\"i2s_shift\":" + String(i2sShiftBits) + ",";
    json += "\"latency_ms\":" + String(latency_ms,1) + ",";
    extern bool highpassEnabled; extern uint16_t highpassCutoffHz;
    json += "\"profile\":\"" + jsonEscape(profileName(currentBufferSize)) + "\",";
    json += "\"hp_enable\":" + String(highpassEnabled?"true":"false") + ",";
    json += "\"hp_cutoff_hz\":" + String((uint32_t)highpassCutoffHz) + ",";
    json += "\"agc_enable\":" + String(agcEnabled?"true":"false") + ",";
    json += "\"agc_multiplier\":" + String(agcMultiplier, 2) + ",";
    float effectiveGain = agcEnabled ? (currentGainFactor * agcMultiplier) : currentGainFactor;
    json += "\"effective_gain\":" + String(effectiveGain, 2) + ",";
    // Metering/clipping
    uint16_t p = (peakHoldAbs16 > 0) ? peakHoldAbs16 : lastPeakAbs16;
    float peak_pct = (p <= 0) ? 0.0f : (100.0f * (float)p / 32767.0f);
    float peak_dbfs = (p <= 0) ? -90.0f : (20.0f * log10f((float)p / 32767.0f));
    json += "\"peak_pct\":" + String(peak_pct,1) + ",";
    json += "\"peak_dbfs\":" + String(peak_dbfs,1) + ",";
    json += "\"clip\":" + String(audioClippedLastBlock?"true":"false") + ",";
    json += "\"clip_count\":" + String(audioClipCount) + ",";
    json += "\"led_mode\":" + String(ledMode);
    json += "}";
    apiSendJSON(json);
}

static void httpPerfStatus() {
    String json = "{";
    json += "\"restart_threshold_pkt_s\":" + String(minAcceptableRate) + ",";
    json += "\"check_interval_min\":" + String(performanceCheckInterval) + ",";
    json += "\"auto_recovery\":" + String(autoRecoveryEnabled?"true":"false") + ",";
    json += "\"auto_threshold\":" + String(autoThresholdEnabled?"true":"false") + ",";
    json += "\"recommended_min_rate\":" + String(computeRecommendedMinRate()) + ",";
    json += "\"scheduled_reset\":" + String(scheduledResetEnabled?"true":"false") + ",";
    json += "\"reset_hours\":" + String(resetIntervalHours) + "}";
    apiSendJSON(json);
}

static void httpThermal() {
    String since = "";
    if (overheatTripTemp > 0.0f && overheatTriggeredAt != 0) {
        since = formatSince(overheatTriggeredAt);
    }
    bool manualRequired = overheatLatched || (!rtspServerEnabled && overheatProtectionEnabled && overheatTripTemp > 0.0f);
    String json = "{";
    if (lastTemperatureValid) {
        json += "\"current_c\":" + String(lastTemperatureC,1) + ",";
    } else {
        json += "\"current_c\":null,";
    }
    json += "\"current_valid\":" + String(lastTemperatureValid?"true":"false") + ",";
    json += "\"max_c\":" + String(maxTemperature,1) + ",";
    json += "\"cpu_mhz\":" + String(getCpuFrequencyMhz()) + ",";
    json += "\"protection_enabled\":" + String(overheatProtectionEnabled?"true":"false") + ",";
    json += "\"shutdown_c\":" + String(overheatShutdownC,0) + ",";
    json += "\"latched\":" + String(overheatLockoutActive?"true":"false") + ",";
    json += "\"latched_persist\":" + String(overheatLatched?"true":"false") + ",";
    json += "\"sensor_fault\":" + String(overheatSensorFault?"true":"false") + ",";
    json += "\"last_trip_c\":" + String(overheatTripTemp,1) + ",";
    json += "\"last_reason\":\"" + jsonEscape(overheatLastReason) + "\",";
    json += "\"last_trip_ts\":\"" + jsonEscape(overheatLastTimestamp) + "\",";
    json += "\"last_trip_since\":\"" + jsonEscape(since) + "\",";
    json += "\"manual_restart\":" + String(manualRequired?"true":"false");
    json += "}";
    apiSendJSON(json);
}

static void httpThermalClear() {
    if (overheatLatched) {
        overheatLatched = false;
        overheatLockoutActive = false;
        overheatTripTemp = 0.0f;
        overheatTriggeredAt = 0;
        overheatLastReason = String("Thermal latch cleared manually.");
        overheatLastTimestamp = String("");
        if (!rtspServerEnabled) {
            rtspServer.begin();
            rtspServer.setNoDelay(true);
            rtspServerEnabled = true;
        }
        saveAudioSettings();
        webui_pushLog(F("UI action: thermal_latch_clear"));
        apiSendJSON(F("{\"ok\":true}"));
    } else {
        apiSendJSON(F("{\"ok\":false}"));
    }
}

static void httpLogs() {
    String out;
    for (size_t i=0;i<logCount;i++){
        size_t idx = (logHead + LOG_CAP - logCount + i) % LOG_CAP;
        out += logBuffer[idx]; out += '\n';
    }
    web.send(200, "text/plain; charset=utf-8", out);
}

static void httpActionServerStart(){
    if (overheatLatched) {
        webui_pushLog(F("Server start blocked: thermal protection latched"));
        apiSendJSON(F("{\"ok\":false,\"error\":\"thermal_latched\"}"));
        return;
    }
    if (!rtspServerEnabled) {
        rtspServerEnabled=true; rtspServer.begin(); rtspServer.setNoDelay(true);
        overheatLockoutActive = false;
    }
    webui_pushLog(F("UI action: server_start"));
    apiSendJSON(F("{\"ok\":true}"));
}
extern WiFiClient* volatile streamClient;
extern bool requestStreamStop(const char* reason);
static void httpActionServerStop(){
    if (isStreaming) {
        requestStreamStop("server_stop");
    }
    rtspServerEnabled=false;
    rtspServer.stop();
    webui_pushLog(F("UI action: server_stop"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionResetI2S(){
    webui_pushLog(F("UI action: reset_i2s"));
    restartI2S(); apiSendJSON(F("{\"ok\":true}"));
}

static String fmtBytes(uint64_t b) {
    if (b < 1024) return String(b) + " B";
    if (b < 1048576) return String(b / 1024.0, 1) + " KB";
    if (b < 1073741824ULL) return String(b / 1048576.0, 1) + " MB";
    return String(b / 1073741824.0, 2) + " GB";
}

static void shipReadyResetTask(void*){
    vTaskDelay(pdMS_TO_TICKS(600));
    WiFiManager wm;
    wm.resetSettings();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP.restart();
}

static void httpWgStatus() {
    String json = "{";
    json += "\"enabled\":" + String(wg_isEnabled()?"true":"false") + ",";
    json += "\"state\":\"" + String(wg_stateStr()) + "\",";
    uint32_t lhMs = wg_lastHandshakeMs();
    if (lhMs > 0) {
        unsigned long seconds = (millis() - lhMs) / 1000;
        json += "\"last_handshake\":\"" + formatUptime(seconds) + " ago\",";
    } else {
        json += "\"last_handshake\":\"never\",";
    }
    json += "\"rx_bytes\":" + String(wg_rxBytes()) + ",";
    json += "\"tx_bytes\":" + String(wg_txBytes()) + ",";
    json += "\"rx_pretty\":\"" + fmtBytes(wg_rxBytes()) + "\",";
    json += "\"tx_pretty\":\"" + fmtBytes(wg_txBytes()) + "\",";
    json += "\"tunnel_ip\":\"" + wg_tunnelIp() + "\",";
    json += "\"tunnel_addr\":\"" + wg_tunnelAddress() + "\",";
    json += "\"endpoint\":\"" + jsonEscape(wg_endpointStr()) + "\",";
    json += "\"keepalive\":" + String(wg_keepalive()) + ",";
    json += "\"private_key\":\"" + String(wg_privateKey().length() > 0 ? "********" : "") + "\",";
    json += "\"server_public_key\":\"" + jsonEscape(wg_serverPublicKey()) + "\",";
    String lanUrl = "rtsp://" + WiFi.localIP().toString() + ":8554/";
    String wgUrl = "";
    String tip = wg_tunnelIp();
    if (tip.length() > 0 && wg_state() == WG_UP) {
        wgUrl = "rtsp://" + tip + ":8554/";
    }
    json += "\"lan_url\":\"" + lanUrl + "\",";
    json += "\"wg_url\":\"" + wgUrl + "\"";
    json += "}";
    apiSendJSON(json);
}

static inline bool argToFloat(const String &name, float &out) { if (!web.hasArg("value")) return false; out = web.arg("value").toFloat(); return true; }
static inline bool argToUInt(const String &name, uint32_t &out) { if (!web.hasArg("value")) return false; out = (uint32_t) web.arg("value").toInt(); return true; }
static inline bool argToUShort(const String &name, uint16_t &out) { if (!web.hasArg("value")) return false; out = (uint16_t) web.arg("value").toInt(); return true; }
static inline bool argToUChar(const String &name, uint8_t &out) { if (!web.hasArg("value")) return false; out = (uint8_t) web.arg("value").toInt(); return true; }

static void httpSet() {
    String key = web.arg("key");
    String val = web.hasArg("value") ? web.arg("value") : String("");
    if (val.length()) {
        String logVal = (key == "wg_priv") ? String("********") : val;
        webui_pushLog(String("UI set: ")+key+"="+logVal);
    }
    if (key == "gain") { float v; if (argToFloat("value", v) && v>=0.1f && v<=100.0f) { currentGainFactor=v; saveAudioSettings(); restartI2S(); } }
    else if (key == "rate") { uint32_t v; if (argToUInt("value", v) && v>=8000 && v<=96000) { currentSampleRate=v; if (autoThresholdEnabled) { minAcceptableRate = computeRecommendedMinRate(); } saveAudioSettings(); restartI2S(); } }
    else if (key == "buffer") { uint16_t v; if (argToUShort("value", v) && v>=256 && v<=8192) { currentBufferSize=v; if (autoThresholdEnabled) { minAcceptableRate = computeRecommendedMinRate(); } saveAudioSettings(); restartI2S(); } }
    // i2sShiftBits removed - fixed at 0 for PDM microphones
    else if (key == "wifi_tx") { float v; if (argToFloat("value", v) && v>=-1.0f && v<=19.5f) { extern float wifiTxPowerDbm; wifiTxPowerDbm = snapWifiTxDbm(v); applyWifiTxPower(true); saveAudioSettings(); } }
    else if (key == "auto_recovery") { String v=web.arg("value"); if (v=="on"||v=="off") { autoRecoveryEnabled=(v=="on"); saveAudioSettings(); } }
    else if (key == "thr_mode") { String v=web.arg("value"); if (v=="auto") { autoThresholdEnabled=true; minAcceptableRate = computeRecommendedMinRate(); saveAudioSettings(); } else if (v=="manual") { autoThresholdEnabled=false; saveAudioSettings(); } }
    else if (key == "min_rate") { uint32_t v; if (argToUInt("value", v) && v>=5 && v<=200) { minAcceptableRate=v; saveAudioSettings(); } }
    else if (key == "check_interval") { uint32_t v; if (argToUInt("value", v) && v>=1 && v<=60) { performanceCheckInterval=v; saveAudioSettings(); } }
    else if (key == "sched_reset") { String v=web.arg("value"); if (v=="on"||v=="off") { extern bool scheduledResetEnabled; scheduledResetEnabled=(v=="on"); saveAudioSettings(); } }
    else if (key == "reset_hours") { uint32_t v; if (argToUInt("value", v) && v>=1 && v<=168) { extern uint32_t resetIntervalHours; resetIntervalHours=v; saveAudioSettings(); } }
    else if (key == "cpu_freq") { uint32_t v; if (argToUInt("value", v) && v>=40 && v<=240) { cpuFrequencyMhz=(uint8_t)v; setCpuFrequencyMhz(cpuFrequencyMhz); saveAudioSettings(); } }
    else if (key == "hp_enable") { String v=web.arg("value"); if (v=="on"||v=="off") { extern bool highpassEnabled; highpassEnabled=(v=="on"); extern void updateHighpassCoeffs(); updateHighpassCoeffs(); saveAudioSettings(); } }
    else if (key == "hp_cutoff") { uint32_t v; if (argToUInt("value", v) && v>=10 && v<=10000) { extern uint16_t highpassCutoffHz; highpassCutoffHz=(uint16_t)v; extern void updateHighpassCoeffs(); updateHighpassCoeffs(); saveAudioSettings(); } }
    else if (key == "agc_enable") { String v=web.arg("value"); if (v=="on"||v=="off") { agcEnabled=(v=="on"); if (!agcEnabled) agcMultiplier=1.0f; saveAudioSettings(); } }
    else if (key == "led_mode") { uint32_t v; if (argToUInt("value", v) && v<=2) { ledMode=(uint8_t)v; saveAudioSettings(); } }
    else if (key == "oh_enable") { String v=web.arg("value"); if (v=="on"||v=="off") { overheatProtectionEnabled = (v=="on"); if (!overheatProtectionEnabled) { overheatLockoutActive = false; } saveAudioSettings(); } }
    else if (key == "oh_limit") { uint32_t v; if (argToUInt("value", v) && v>=OH_MIN && v<=OH_MAX) { uint32_t snapped = OH_MIN + ((v - OH_MIN)/OH_STEP)*OH_STEP; overheatShutdownC = (float)snapped; overheatLockoutActive = false; saveAudioSettings(); } }
    else if (key == "wg_enable") { String v=web.arg("value"); if (v=="on"||v=="off") { wg_setEnabled(v=="on"); } }
    else if (key == "wg_priv") { if (val.length()>0) { wg_setPrivateKey(val); } }
    else if (key == "wg_srvpub") { if (val.length()>0) { wg_setServerPublicKey(val); } }
    else if (key == "wg_endpoint") { if (val.length()>0) { int colon = val.lastIndexOf(':'); if (colon > 0) { String host = val.substring(0, colon); int portInt = val.substring(colon+1).toInt(); if (portInt > 0 && portInt < 65536) { wg_setEndpoint(host, (uint16_t)portInt); } else { webui_pushLog(F("UI set: wg_endpoint=invalid port")); } } } }
    else if (key == "wg_tunaddr") { if (val.length()>0) { wg_setTunnelAddress(val); } }
    else if (key == "wg_keepalive") { uint32_t v; if (argToUInt("value", v) && v<=65535) { wg_setKeepalive((uint16_t)v); } }
    else if (key == "hostname") {
        String normalized = normalizeHostname(val);
        if (normalized.length() > 0) {
            deviceHostname = normalized;
            saveAudioSettings();
            webui_pushLog(String("UI set: hostname=") + deviceHostname);
            apiSendJSON(F("{\"ok\":true,\"reboot\":true}"));
            scheduleReboot(false, 600);
            return;
        } else {
            apiSendJSON(F("{\"ok\":false,\"error\":\"invalid hostname\"}"));
            return;
        }
    }
    apiSendJSON(F("{\"ok\":true}"));
}

void webui_begin() {
    web.on("/", httpIndex);
    web.on("/gui.js", httpGuiJs);
    web.on("/api/status", httpStatus);
    web.on("/api/audio_status", httpAudioStatus);
    web.on("/api/perf_status", httpPerfStatus);
    web.on("/api/thermal", httpThermal);
    web.on("/api/thermal/clear", HTTP_POST, httpThermalClear);
    web.on("/api/logs", httpLogs);
    web.on("/api/action/server_start", httpActionServerStart);
    web.on("/api/action/server_stop", httpActionServerStop);
    web.on("/api/action/reset_i2s", httpActionResetI2S);
    web.on("/api/wg_status", httpWgStatus);
    web.on("/api/action/ship_ready_reset", [](){ webui_pushLog(F("UI action: ship_ready_reset")); apiSendJSON(F("{\"ok\":true}")); xTaskCreate(shipReadyResetTask, "ship_reset", 4096, NULL, 1, NULL); });
    web.on("/api/action/reboot", [](){ webui_pushLog(F("UI action: reboot")); apiSendJSON(F("{\"ok\":true}")); scheduleReboot(false, 600); });
    web.on("/api/action/factory_reset", [](){ webui_pushLog(F("UI action: factory_reset")); apiSendJSON(F("{\"ok\":true}")); scheduleReboot(true, 600); });
    web.on("/api/set", httpSet);
    web.begin();
}

void webui_handleClient() {
    web.handleClient();
}
