#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
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
    h += F(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>M5Stack Atom Echo - RTSP Microphone</title>"
        "<style>:root{--bg:#0b1020;--fg:#e7ebf2;--muted:#9aa3b2;--card:#121a2e;--border:#1b2745;--acc:#4ea1f3;--acc2:#36d399;--warn:#f59e0b;--bad:#ef4444}"
        "body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;background:linear-gradient(180deg,#0b1020 0%,#0f1530 100%);color:var(--fg)}"
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
        "</style></head><body>"
        "<div id='ovr' class='overlay'><div class='box' id='ovr_msg'>Restarting…</div></div>"
        "<div class='page'>"
        "<div class='card'><div class='hero'><div><div class='brand'><div class='title' id='t_title'>M5Stack Atom Echo</div><span class='badge' id='fwv'></span></div><div class='subtitle'><span id='t_lan_url'>LAN URL</span>: <a id='rtsp' class='mono' href='rtsp://");
    h += ip;
    h += F(
        ":8554/audio' target='_blank'>rtsp://");
    h += ip;
    h += F(
        ":8554/audio</a><button onclick=\"copyUrl('rtsp','copy_btn_lan')\" id='copy_btn_lan' style='background:none;border:1px solid var(--border);padding:1px 6px;cursor:pointer;border-radius:6px;line-height:1;font-size:11px;margin-left:4px'>copy</button><div id='wg_url_row' style='display:none'><span id='t_wg_url'>WireGuard URL</span>: <span id='rtsp_wg_url' class='mono'></span><button onclick=\"copyUrl('rtsp_wg_url','copy_btn_wg')\" id='copy_btn_wg' style='background:none;border:1px solid var(--border);padding:1px 6px;cursor:pointer;border-radius:6px;line-height:1;font-size:11px;margin-left:4px'>copy</button></div></div></div>"
        "<div class='lang'><a href='https://github.com/stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic' target='_blank' class='gh'>GitHub</a>Lang: <select id='langSel'><option value='en'>English</option><option value='cs'>Čeština</option></select></div></div></div>"
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

        "<div class='card'><h2 id='t_audio'>Audio</h2><table>"
        "<tr><td class='k'><span id='t_rate'>Sample Rate</span><span class='help' id='h_rate'>?</span><div class='hint' id='rate_hint' style='display:none'></div></td><td class='v'><div class='field'><input id='in_rate' type='number' step='1000' min='8000' max='96000'><span class='unit'>Hz</span><button id='btn_rate_set' onclick=\"setv('rate',in_rate.value)\">Set</button></div></td></tr>"
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
        "<script>"
"const T={en:{title:'ESP32 RTSP Mic for BirdNET-Go',status:'Status',ip:'IP Address',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi TX Power',heap:'Free Heap (min)',uptime:'Uptime',rtsp_server:'RTSP Server',client:'Client',streaming:'Streaming',pkt_rate:'Packet Rate',last_connect:'Last RTSP Connect',last_play:'Last Stream Start',audio:'Audio',rate:'Sample Rate',gain:'Gain',buf:'Buffer Size',latency:'Latency',profile:'Profile',perf:'Reliability',auto:'Auto Recovery',wifi:'WiFi',wifi_tx2:'TX Power (dBm)',thermal:'Thermal',logs:'Logs',bsrvon:'Server ON',bsrvoff:'Server OFF',breset:'Reset I2S',breboot:'Reboot',bdefaults:'Defaults',confirm_reboot:'Restart device now?',confirm_reset:'Reset to defaults and reboot?',restarting:'Restarting device…',resetting:'Restoring defaults and rebooting…',advanced_settings:'Advanced Settings',shift:'I2S Shift',thr:'Restart Threshold',chk:'Check Interval',thr_mode:'Threshold Mode',auto_m:'Auto',manual_m:'Manual',sched:'Scheduled Reset',hours:'Reset After',cpu:'CPU Frequency',set:'Set',profile_ultra:'Ultra-Low Latency (Higher CPU, May have dropouts)',profile_balanced:'Balanced (Moderate CPU, Good stability)',profile_stable:'Stable Streaming (Lower CPU, Excellent stability)',profile_high:'High Stability (Lowest CPU, Maximum stability)',help_rate:'Higher sample-rate = more detail, more bandwidth.',help_gain:'Amplifies audio after I²S shift; too high clips.',help_buf:'More samples per packet = higher latency, more stability.',help_auto:'Auto-restarts the pipeline when packet-rate collapses.',help_tx:'Wi‑Fi TX power; lowering can reduce RF noise.',help_shift:'Digital right shift applied before scaling.',help_thr:'Minimum packet-rate before auto-recovery triggers.',help_chk:'How often performance is checked.',help_sched:'Periodic device restart for stability.',help_hours:'Interval between scheduled restarts.',help_cpu:'Lower MHz = cooler, higher latency possible.',therm_protect:'Overheat Protection',therm_limit:'Shutdown Limit',therm_status:'Status',therm_now:'Current Temp',therm_max:'Peak Temp',therm_cpu:'CPU Clock',therm_last:'Last Shutdown',therm_status_ready:'Protection ready',therm_status_disabled:'Protection disabled',therm_status_latched:'Cooling required – restart manually',therm_status_sensor_fault:'Sensor unavailable – protection paused',therm_status_latched_persist:'Protection latched — acknowledge to re-enable',therm_hint:'80 °C suits most ESP32 boards; drop to 70–75 °C for sealed enclosures.',therm_last_none:'No shutdown recorded yet.',therm_last_fmt:'Stopped at %TEMP% °C (limit %LIMIT% °C) after %TIME% uptime (%AGO%).',therm_last_sensor_fault:'Thermal protection disabled: temperature sensor unavailable.',therm_latch_notice:'Thermal shutdown latched the RTSP server. Confirm only after hardware cools down.',therm_clear_btn:'Acknowledge & re-enable RTSP',therm_time_unknown:'unknown time',therm_time_ago_unknown:'just now',help_therm_protect:'Automatically stops streaming when the ESP32 exceeds the limit to protect the board and microphone preamp.',help_therm_limit:'Temperature threshold for thermal shutdown. 80 °C is a safe default; use 70–75 °C if airflow is poor.'},cs:{title:'ESP32 RTSP Mic pro BirdNET-Go',status:'Stav',ip:'IP adresa',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi výkon',heap:'Volná RAM (min)',uptime:'Doba běhu',rtsp_server:'RTSP server',client:'Klient',streaming:'Streamování',pkt_rate:'Rychlost paketů',last_connect:'Poslední RTSP připojení',last_play:'Poslední start streamu',audio:'Audio',rate:'Vzorkovací frekvence',gain:'Zisk',buf:'Velikost bufferu',latency:'Latence',profile:'Profil',perf:'Spolehlivost',auto:'Automatická obnova',wifi:'WiFi',wifi_tx2:'TX výkon (dBm)',thermal:'Teplota',logs:'Logy',bsrvon:'Server ZAP',bsrvoff:'Server VYP',breset:'Reset I2S',breboot:'Restart',bdefaults:'Výchozí',confirm_reboot:'Restartovat zařízení nyní?',confirm_reset:'Obnovit výchozí nastavení a restartovat?',restarting:'Zařízení se restartuje…',resetting:'Obnovuji výchozí nastavení a restartuji…',advanced_settings:'Pokročilá nastavení',shift:'I2S posun',thr:'Prahová hodnota restartu',chk:'Interval kontroly',thr_mode:'Režim prahu',auto_m:'Automaticky',manual_m:'Manuálně',sched:'Plánovaný restart',hours:'Po kolika hodinách',cpu:'Frekvence CPU',set:'Nastavit',profile_ultra:'Ultra nízká latence (vyšší zátěž CPU, možné výpadky)',profile_balanced:'Vyvážené (střední zátěž CPU, dobrá stabilita)',profile_stable:'Stabilní stream (nižší zátěž CPU, výborná stabilita)',profile_high:'Vysoká stabilita (nejnižší zátěž CPU, max. stabilita)',help_rate:'Vyšší frekvence = více detailů, větší datový tok.',help_gain:'Zesílení po I²S posunu; příliš vysoké klipuje.',help_buf:'Více vzorků v paketu = vyšší latence, větší stabilita.',help_auto:'Při poklesu rychlosti paketů dojde k obnově.',help_tx:'Výkon vysílače Wi‑Fi; snížení může zlepšit šum.',help_shift:'Digitální bitový posun před škálováním.',help_thr:'Minimální rychlost paketů pro spuštění obnovy.',help_chk:'Jak často se provádí kontrola výkonu.',help_sched:'Pravidelný restart zařízení kvůli stabilitě.',help_hours:'Interval mezi plánovanými restarty.',help_cpu:'Nižší MHz = chladnější, může přidat latenci.',therm_protect:'Ochrana proti přehřátí',therm_limit:'Vypínací teplota',therm_status:'Stav',therm_now:'Aktuální teplota',therm_max:'Maximální teplota',therm_cpu:'Takt CPU',therm_last:'Poslední zásah',therm_status_ready:'Ochrana připravena',therm_status_disabled:'Ochrana vypnuta',therm_status_latched:'Přehřátí – nejprve vychlaďte a spusťte ručně',therm_status_sensor_fault:'Senzor teploty nedostupný – ochrana pozastavena',therm_status_latched_persist:'Ochrana zůstává blokovaná – potvrďte znovuspuštění',therm_hint:'80 °C je bezpečné pro většinu ESP32; v uzavřených krabičkách volte 70–75 °C.',therm_last_none:'Zatím žádné přehřátí.',therm_last_fmt:'Stream vypnut při %TEMP% °C (limit %LIMIT% °C) po %TIME% běhu (%AGO%).',therm_last_sensor_fault:'Tepelná ochrana vypnuta: teplota není k dispozici.',therm_latch_notice:'Tepelná ochrana odstavila RTSP server. Zapínejte až po vychladnutí.',therm_clear_btn:'Potvrdit a znovu povolit RTSP',therm_time_unknown:'neznámý čas',therm_time_ago_unknown:'právě teď',help_therm_protect:'Při překročení limitu zastaví stream, aby chránila desku a předzesilovač.',help_therm_limit:'Teplota, při které se stream vypne. 80 °C vyhoví odkrytým deskám; v teplém prostředí nastavte 70–75 °C.'}};"
        "const HELP_EXT_EN={led:'LED Mode', help_led:'Off: LED stays dark during streaming. Static: Solid color (blue=ready, green=streaming). Level: Color changes with audio level — green=good, orange=hot, red=clipping, dim purple=quiet.',agc:'AGC (Auto Gain)', help_agc:'Automatic Gain Control adjusts volume automatically. Fast attack prevents clipping on loud sounds; slow release gradually boosts quiet periods. Great for outdoor bird recording where distance varies. Base Gain still applies — AGC adjusts on top of it.', hpf:'High-pass', hpf_cut:'HPF Cutoff', help_hpf:'High-pass filter (2nd-order, ~12 dB/oct) removes low-frequency rumble such as distant traffic, wind or handling noise. Turn ON to attenuate frequencies below the cutoff while keeping most bird vocalizations intact.', help_hpf_cut:'Cutoff frequency for the high-pass filter. Typical: 300–800 Hz. Lower values (300–400 Hz) keep more ambience and low calls; higher values (600–800 Hz) strongly reduce road noise. Very high settings may suppress low-pitched species.', help_rate:'How many audio samples per second are captured. Higher rates increase detail and bandwidth and CPU usage. 48 kHz is a safe default; 44.1 kHz is also fine. Very high rates may stress Wi‑Fi and processing.',help_gain:'Software amplification after the I2S shift. Use to boost loudness. Too high causes clipping (distortion). With default shift, 1.0× is neutral. Adjust while watching the stream.',help_buf:'Samples per network packet. Bigger buffer increases latency but improves stability on weak Wi‑Fi; smaller buffer lowers latency but may drop packets. 1024 is a good balance.',help_auto:'When enabled, the device restarts the audio pipeline if packet rate drops below the threshold. Helps recover from glitches without manual intervention.',help_tx:'Wi‑Fi transmit power in dBm. Lower values can reduce RF self-noise near the microphone and power draw, but reduce range. Only specific steps are supported by the radio. Change carefully if your signal is weak.',help_shift:'Right bit-shift applied to 32‑bit I2S samples before converting to 16‑bit. Higher shift lowers volume and avoids clipping; lower shift raises volume but may clip.',help_thr:'Minimum packet rate (packets per second) considered healthy while streaming. If measured rate stays below this at a check, auto recovery restarts I2S. In Auto mode this comes from sample rate and buffer size (about 70% of expected).',help_chk:'How often performance is checked (minutes). Shorter intervals react faster with small CPU cost; longer intervals reduce checks.',help_sched:'Optional periodic device reboot for long-term stability on problematic networks. Leave OFF unless you need it.',help_hours:'Number of hours between scheduled reboots. Applies only when Scheduled Reset is ON.',help_cpu:'Processor clock. Lower MHz reduces heat and power; higher MHz can help under heavy load. 120 MHz is a balanced default.',help_thr_mode:'Auto: Threshold is computed from Sample Rate and Buffer; recommended for most users. Manual: You set the exact minimum packet rate; use if you know your network and latency constraints.', level:'Signal Level', help_level:'Shows the highest peak since last update. Aim for 60–80% (about −4 to −2 dBFS). If it says CLIPPING, increase I2S Shift or reduce Gain. Turning ON the High‑pass (500–600 Hz) often helps.', clip_ok:'OK', clip_warn:'High level — close to clipping (reduce Gain or increase I2S Shift).', clip_bad:'CLIPPING! Increase I2S Shift or reduce Gain; try High‑pass 500–600 Hz.'};"
        "const HELP_EXT_CS={led:'Režim LED', help_led:'Vyp: LED je zhasnutá. Statická: Pevná barva (modrá=připraveno, zelená=streamuje). Úroveň: Barva se mění podle hlasitosti — zelená=ok, oranžová=vysoko, červená=přebuzení, tmavě fialová=ticho.',agc:'AGC (Auto zisk)', help_agc:'Automatické řízení zisku přizpůsobuje hlasitost. Rychlý útlum zabrání přebuzení u hlasitých zvuků; pomalé uvolnění postupně zesiluje tiché úseky. Ideální pro venkovní nahrávání ptáků, kde se vzdálenost mění. Základní zisk se stále uplatňuje — AGC upravuje nad ním.', hpf:'Vysokopropustný filtr', hpf_cut:'Mezní frekvence HPF', help_hpf:'Vysokopropustný filtr (2. řád, ~12 dB/okt.) potlačí nízké frekvence jako vzdálená silnice, vítr nebo manipulační hluk. Zapněte pro zeslabení pásem pod mezní frekvencí a zachování většiny ptačích hlasů.', help_hpf_cut:'Mezní frekvence vysokopropustného filtru. Typicky 300–800 Hz. Nižší hodnoty (300–400 Hz) ponechají více atmosféry a nízkých zvuků; vyšší (600–800 Hz) silněji potlačí silniční hluk. Příliš vysoké nastavení může omezit nízko posazené druhy.', help_rate:'Kolik vzorků za sekundu se pořizuje. Vyšší frekvence zvyšuje detail i nároky na šířku pásma a CPU. 48 kHz je bezpečné výchozí nastavení; 44,1 kHz je také v pořádku. Velmi vysoké frekvence mohou zatěžovat Wi‑Fi a zpracování.',help_gain:'Softwarové zesílení po I2S posunu. 1,0× je neutrální s výchozím posunem. Příliš vysoká hodnota způsobí ořez (zkreslení). Upravujte podle poslechu a spektra.',help_buf:'Počet vzorků v jednom síťovém paketu. Větší buffer zvyšuje latenci a zlepšuje stabilitu na slabším Wi‑Fi; menší buffer snižuje latenci, ale může zvyšovat ztráty paketů. 1024 je dobrý kompromis.',help_auto:'Při poklesu rychlosti odchozích paketů pod práh zařízení automaticky restartuje audio pipeline. Pomáhá zotavit se z výpadků bez zásahu.',help_tx:'Vysílací výkon Wi‑Fi v dBm. Snížení může omezit vlastní RF šum u mikrofonu a spotřebu, ale zmenší dosah. Čip podporuje jen určité kroky. Pokud máte slabý signál, měňte opatrně.',help_shift:'Pravý bitový posun na 32bitových I2S vzorcích před převodem na 16bit audio. Vyšší posun snižuje hlasitost a brání klipování; nižší posun zvyšuje hlasitost, ale může klipovat.',help_thr:'Minimální rychlost paketů (paketů za sekundu), považovaná při streamování za zdravou. Pokud při kontrole klesne pod tuto hodnotu, automatická obnova restartuje I2S. V režimu Auto se práh odvozuje z frekvence a bufferu (asi 70 % očekávané hodnoty).',help_chk:'Jak často se kontroluje výkon (minuty). Kratší interval reaguje rychleji s malou zátěží CPU; delší interval snižuje počet kontrol.',help_sched:'Volitelný pravidelný restart zařízení pro dlouhodobou stabilitu na problematických sítích. Nechte VYP, pokud není nutné.',help_hours:'Počet hodin mezi plánovanými restarty. Platí pouze pokud je Plánovaný restart ZAP.',help_cpu:'Frekvence procesoru. Nižší MHz snižuje zahřívání a spotřebu; vyšší MHz pomůže při zátěži. 120 MHz je vyvážené výchozí nastavení.',help_thr_mode:'Auto: Práh restartu se počítá z Vzorkovací frekvence a Bufferu; doporučeno pro většinu uživatelů. Manuálně: Nastavíte přesný minimální počet paketů za sekundu; použijte, pokud znáte svou síť a požadavky na latenci.', level:'Úroveň signálu', help_level:'Zobrazuje nejvyšší špičku od poslední obnovy. Cíl je 60–80 % (asi −4 až −2 dBFS). Při CLIPPING zvyšte I2S posun nebo snižte Gain. Často pomůže zapnout High‑pass (500–600 Hz).', clip_ok:'OK', clip_warn:'Vysoká úroveň — blízko klipu (snižte Gain nebo zvyšte I2S posun).', clip_bad:'CLIPPING! Zvyšte I2S posun nebo snižte Gain; zkuste High‑pass 500–600 Hz.'};"
        "Object.assign(T.en, HELP_EXT_EN); Object.assign(T.cs, HELP_EXT_CS);"
        "Object.assign(T.en,{wireguard:'WireGuard',wg:'WireGuard',wg_enable:'Enabled',wg_priv:'Private Key',wg_srvpub:'Server Public Key',wg_endpoint:'Endpoint',wg_tunaddr:'Tunnel IP',wg_keepalive:'Keepalive',wg_status:'WireGuard Status',wg_state:'Tunnel State',wg_last_hs:'Last Handshake',wg_rx:'Bytes Received',wg_tx:'Bytes Transmitted',lan_url:'LAN URL',wg_url:'WireGuard URL',wg_remote_note:'Admin can reach this UI at http://<tunnel-ip>/ from any WireGuard peer.',b_ship_ready:'Reset Wi-Fi',b_factory_reset:'Factory Reset',confirm_ship_ready:'Reset Wi-Fi settings and reboot? WireGuard and audio configuration will be preserved. The device will return to the captive portal for Wi-Fi setup.',confirm_defaults:'Reset all settings to factory defaults? Wi‑Fi credentials will not be affected.',help_wg_enable:'Enable or disable the WireGuard tunnel. The tunnel connects automatically when enabled.',help_wg_priv:'Your WireGuard private key (base64). Generated externally with wg tools.',help_wg_srvpub:'The public key of the WireGuard server (base64).',help_wg_endpoint:'WireGuard server address in host:port format (e.g. myserver.ddns.net:51820).',help_wg_tunaddr:'Device tunnel IP in CIDR notation (e.g. 10.6.0.5/24). Must match the server peer config.',help_wg_keepalive:'Persistent keepalive interval in seconds (default 25). Keeps NAT mappings open for inbound connections.',wg_import:'Import Config',wg_import_preview:'Import preview',wg_import_apply:'Apply',wg_import_cancel:'Cancel',wg_import_psk_warn:'PresharedKey found — not supported, will be ignored',wg_import_failed:'Import failed:',wg_import_err_no_peer:'No [Peer] section found',wg_import_err_multi_peer:'Multiple [Peer] sections — single-peer configs only',wg_import_err_missing:'Required field missing',wg_import_err_ipv6:'Tunnel address is not IPv4 (only IPv4 is supported)',wg_import_err_bad_key:'Key is not a 44-char base64 string',wg_import_err_bad_endpoint:'Endpoint must be host:port',help_wg_import:'Import a standard WireGuard .conf file to fill all fields automatically.'});"
        "Object.assign(T.cs,{wireguard:'WireGuard',wg:'WireGuard',wg_enable:'Povoleno',wg_priv:'Privátní klíč',wg_srvpub:'Veřejný klíč serveru',wg_endpoint:'Endpoint',wg_tunaddr:'IP tunelu',wg_keepalive:'Keepalive',wg_status:'Stav WireGuard',wg_state:'Stav tunelu',wg_last_hs:'Poslední handshake',wg_rx:'Přijaté bajty',wg_tx:'Odeslané bajty',lan_url:'LAN URL',wg_url:'WireGuard URL',wg_remote_note:'Administrátor dosáhne na UI na http://<tunnel-ip>/ z libovolného WireGuard peera.',b_ship_ready:'Resetovat Wi-Fi',b_factory_reset:'Obnovení továrního nastavení',confirm_ship_ready:'Resetovat nastavení Wi-Fi a restartovat? Konfigurace WireGuard a zvuku bude zachována. Zařízení se vrátí na captive portal pro nastavení Wi-Fi.',confirm_defaults:'Resetovat všechna nastavení na výchozí? Přihlašovací údaje Wi‑Fi nebudou ovlivněny.',help_wg_enable:'Povolit nebo zakázat WireGuard tunel.',help_wg_priv:'Privátní WireGuard klíč (base64).',help_wg_srvpub:'Veřejný klíč WireGuard serveru (base64).',help_wg_endpoint:'Adresa WireGuard serveru ve formátu host:port.',help_wg_tunaddr:'IP adresa tunelu v CIDR notaci (např. 10.6.0.5/24).',help_wg_keepalive:'Interval persistent keepalive v sekundách (výchozí 25).',wg_import:'Importovat Config',wg_import_preview:'Náhled importu',wg_import_apply:'Použít',wg_import_cancel:'Zrušit',wg_import_psk_warn:'Nalezen PresharedKey — není podporován, bude ignorován',wg_import_failed:'Import selhal:',wg_import_err_no_peer:'Nenalezena sekce [Peer]',wg_import_err_multi_peer:'Více sekcí [Peer] — podporovány jsou pouze konfigurace s jedním peerem',wg_import_err_missing:'Chybí povinné pole',wg_import_err_ipv6:'Adresa tunelu není IPv4 (podporováno pouze IPv4)',wg_import_err_bad_key:'Klíč není 44znakový base64 řetězec',wg_import_err_bad_endpoint:'Endpoint musí být ve formátu host:port',help_wg_import:'Importujte standardní WireGuard .conf pro automatické vyplnění všech polí.'});"
        "let lang=localStorage.getItem('lang')||'en'; const $=id=>document.getElementById(id);"
"function applyLang(){const L=T[lang]; const st=(id,t)=>{const e=$(id); if(e) e.textContent=t}; const help=(k)=>{const b=L[k]||''; return b}; st('t_title',L.title); st('t_status',L.status); st('t_ip',L.ip); st('t_wifi_rssi',L.wifi_rssi); st('t_wifi_tx',L.wifi_tx); st('t_heap',L.heap); st('t_uptime',L.uptime); st('t_rtsp_server',L.rtsp_server); st('t_client',L.client); st('t_streaming',L.streaming); st('t_pkt_rate',L.pkt_rate); st('t_last_connect',L.last_connect); st('t_last_play',L.last_play); st('t_audio',L.audio); st('t_rate',L.rate); st('t_gain',L.gain); st('t_buf',L.buf); st('t_latency',L.latency); st('t_level',L.level); st('t_profile',L.profile); st('t_perf',L.perf); st('t_auto',L.auto); st('t_wifi',L.wifi); st('t_wifi_tx2',L.wifi_tx2); st('t_thermal',L.thermal); st('t_therm_protect',L.therm_protect); st('t_therm_limit',L.therm_limit); st('t_therm_status',L.therm_status); st('t_therm_now',L.therm_now); st('t_therm_max',L.therm_max); st('t_therm_cpu',L.therm_cpu); st('t_therm_last',L.therm_last); st('t_logs',L.logs); st('b_reset',L.breset); st('b_reboot',L.breboot); st('b_factory_reset',L.b_factory_reset); st('t_advanced_settings',L.advanced_settings); st('t_shift',L.shift); st('t_thr',L.thr); st('t_chk',L.chk); st('t_thr_mode',L.thr_mode); st('t_sched',L.sched); st('t_hours',L.hours); st('t_cpu',L.cpu); const hm=(id,k)=>{const e=$(id); if(e) e.setAttribute('title',help(k))}; hm('h_rate','help_rate'); hm('h_gain','help_gain'); hm('h_hpf','help_hpf'); hm('h_hpf_cut','help_hpf_cut'); hm('h_buf','help_buf'); hm('h_auto','help_auto'); hm('h_tx','help_tx'); hm('h_thr','help_thr'); hm('h_chk','help_chk'); hm('h_shift','help_shift'); hm('h_sched','help_sched'); hm('h_hours','help_hours'); hm('h_cpu','help_cpu'); hm('h_thr_mode','help_thr_mode'); hm('h_level','help_level'); hm('h_therm_protect','help_therm_protect'); hm('h_therm_limit','help_therm_limit'); st('t_wg',L.wg); st('t_wg_enable',L.wg_enable); st('t_wg_priv',L.wg_priv); st('t_wg_srvpub',L.wg_srvpub); st('t_wg_endpoint',L.wg_endpoint); st('t_wg_tunaddr',L.wg_tunaddr); st('t_wg_keepalive',L.wg_keepalive); st('t_wg_status',L.wg_status); st('t_wg_state',L.wg_state); st('t_wg_last_hs',L.wg_last_hs); st('t_wg_rx',L.wg_rx); st('t_wg_tx',L.wg_tx); st('t_lan_url',L.lan_url); st('t_wg_url',L.wg_url); st('b_ship_ready',L.b_ship_ready); st('b_wg_import',L.wg_import); hm('h_wg_enable','help_wg_enable'); hm('h_wg_priv','help_wg_priv'); hm('h_wg_srvpub','help_wg_srvpub'); hm('h_wg_endpoint','help_wg_endpoint'); hm('h_wg_tunaddr','help_wg_tunaddr'); hm('h_wg_keepalive','help_wg_keepalive'); hm('h_wg_import','help_wg_import'); st('btn_rate_set',L.set); st('btn_gain_set',L.set); st('btn_buf_set',L.set); st('btn_auto_set',L.set); st('btn_thrmode_set',L.set); st('btn_thr_set',L.set); st('btn_sched_set',L.set); st('btn_hours_set',L.set); st('btn_shift_set',L.set); st('btn_chk_set',L.set); st('btn_tx_set',L.set); st('btn_cpu_set',L.set); st('btn_oh_enable',L.set); st('btn_oh_limit',L.set); const sht=(id,k)=>{const e=$(id); if(e) e.textContent=help(k)}; sht('txt_rate_hint','help_rate'); sht('txt_gain_hint','help_gain'); sht('txt_hpf_hint','help_hpf'); sht('txt_hpf_cut_hint','help_hpf_cut'); sht('txt_buf_hint','help_buf'); sht('txt_auto_hint','help_auto'); sht('txt_thr_hint','help_thr'); sht('txt_thr_mode_hint','help_thr_mode'); sht('txt_sched_hint','help_sched'); sht('txt_hours_hint','help_hours'); sht('txt_shift_hint','help_shift'); sht('txt_chk_hint','help_chk'); sht('txt_tx_hint','help_tx'); sht('txt_cpu_hint','help_cpu'); sht('txt_level_hint','help_level'); sht('txt_therm_hint_protect','help_therm_protect'); sht('txt_therm_hint_limit','help_therm_limit'); st('t_hpf',L.hpf); st('t_hpf_cut',L.hpf_cut); st('t_agc',L.agc); hm('h_agc','help_agc'); st('btn_agc_set',L.set); sht('txt_agc_hint','help_agc'); st('t_led',L.led); hm('h_led','help_led'); st('btn_led_set',L.set); sht('txt_led_hint','help_led'); sht('txt_wg_enable_hint','help_wg_enable'); sht('txt_wg_priv_hint','help_wg_priv'); sht('txt_wg_srvpub_hint','help_wg_srvpub'); sht('txt_wg_endpoint_hint','help_wg_endpoint'); sht('txt_wg_tunaddr_hint','help_wg_tunaddr'); sht('txt_wg_keepalive_hint','help_wg_keepalive'); sht('txt_wg_import_hint','help_wg_import'); st('t_wg_remote_note',L.wg_remote_note); document.title=L.title;}"
        "function profileText(buf){const L=T[lang]; buf=parseInt(buf,10)||0; if(buf<=256) return L.profile_ultra; if(buf<=512) return L.profile_balanced; if(buf<=1024) return L.profile_stable; return L.profile_high;}"
        "function fmtBool(b){return b?'<span class=ok>YES</span>':'<span class=bad>NO</span>'}"
        "function fmtSrv(b){return b?'<span class=ok>ENABLED</span>':'<span class=bad>DISABLED</span>'}"
        "function showOverlay(msg){ $('ovr_msg').textContent=msg; $('ovr').style.display='flex'; }"
        "function copyLogs(){const t=$('logs').textContent;const b=$('btn_copy_logs');const orig=b.innerHTML;const ta=document.createElement('textarea');ta.value=t;ta.style.position='fixed';ta.style.opacity='0';document.body.appendChild(ta);ta.select();try{document.execCommand('copy');b.innerHTML='<svg width=16 height=16 viewBox=\"0 0 16 16\" fill=\"none\" stroke=\"var(--acc2)\" stroke-width=\"1.5\"><path d=\"M3 9l3 3 7-7\"/></svg>';setTimeout(()=>{b.innerHTML=orig},1500)}catch(e){}document.body.removeChild(ta)}"
"function copyUrl(srcId,btnId){const el=$(srcId);if(!el)return;const t=el.textContent;const b=$(btnId);if(!b)return;const orig=b.textContent;const ta=document.createElement('textarea');ta.value=t;ta.style.position='fixed';ta.style.opacity='0';document.body.appendChild(ta);ta.select();try{document.execCommand('copy');b.textContent='Copied';setTimeout(()=>{b.textContent=orig},1500)}catch(e){}document.body.removeChild(ta)}"
        "function rebootSequence(kind){ const L=T[lang]; const msg=(kind==='factory_reset')?L.resetting:L.restarting; showOverlay(msg); function tick(){ fetch('/api/status',{cache:'no-store'}).then(r=>{ if(r.ok){ location.reload(); } else { setTimeout(tick,2000); } }).catch(()=>setTimeout(tick,2000)); } setTimeout(tick,4000); }"
        "function act(a){fetch('/api/action/'+a,{cache:'no-store'}).then(r=>r.json()).then(loadStatus)}"
        "function rebootNow(){ rebootSequence('reboot'); act('reboot'); }"
        "function defaultsNow(){const L=T[lang]; if(confirm(L.confirm_defaults||'Reset all settings to factory defaults?')){rebootSequence('factory_reset'); act('factory_reset'); }}"
        "function toggleServer(){const b=$('b_srv_toggle'); const on=b.classList.contains('active'); b.classList.toggle('active',!on); b.textContent='Server: '+(!on?'ON':'OFF'); act(on?'server_stop':'server_start');}"
"function shipReadyReset(){const L=T[lang]; if(confirm(L.confirm_ship_ready||'Reset Wi-Fi settings and reboot? Audio and WireGuard configuration will be preserved. The device will return to the captive portal.')){rebootSequence('reboot'); act('ship_ready_reset');}}"
        "function parseWgConf(text){const out={iface:{},peers:[]};let cur=null;for(const raw of text.split(String.fromCharCode(10))){const line=raw.trim();if(!line||line.startsWith('#'))continue;if(line.startsWith('[')&&line.endsWith(']')){const name=line.slice(1,-1).trim().toLowerCase();if(name==='interface'){cur=out.iface;}else if(name==='peer'){cur={};out.peers.push(cur);}else{cur=null;}continue;}const eq=line.indexOf('=');if(eq<0||!cur)continue;const k=line.slice(0,eq).trim().toLowerCase();const v=line.slice(eq+1).trim();cur[k]=v;}return out;}"
        "function isBase64Key(s){return /^[A-Za-z0-9+/]{43}=$/.test(s);}"
        "function firstIpv4Cidr(s){if(!s)return '';for(const tok of s.split(',')){const t=tok.trim();if(/^\\d{1,3}(\\.\\d{1,3}){3}\\/\\d{1,2}$/.test(t))return t;}return '';}"
        "function wgImportParse(text){const r=parseWgConf(text);const e=[];if(r.peers.length===0)e.push('no_peer');if(r.peers.length>1)e.push('multi_peer');const priv=r.iface['privatekey']||'';const addr=firstIpv4Cidr(r.iface['address']||'');const p=r.peers[0]||{};const pub=p['publickey']||'';const endp=p['endpoint']||'';const kaRaw=p['persistentkeepalive'];const ka=kaRaw?parseInt(kaRaw,10):NaN;const psk=!!p['presharedkey'];if(!priv)e.push('missing:PrivateKey');else if(!isBase64Key(priv))e.push('bad_key:PrivateKey');if(!addr)e.push('missing_or_v6:Address');if(!pub)e.push('missing:PublicKey');else if(!isBase64Key(pub))e.push('bad_key:PublicKey');if(!endp)e.push('missing:Endpoint');else if(!/^.+:\\d{1,5}$/.test(endp))e.push('bad_endpoint');return{errors:e,values:{priv:priv,addr:addr,pub:pub,endp:endp,ka:ka,psk:psk}};}"
        "function wgImportPreview(parsed){const panel=$('wg_import_panel');const L=T[lang];if(!panel)return;if(parsed.errors.length){const keyFor=function(e){if(e.indexOf('missing:')===0)return'wg_import_err_missing';if(e.indexOf('bad_key:')===0)return'wg_import_err_bad_key';if(e==='missing_or_v6:Address')return'wg_import_err_ipv6';if(e==='bad_endpoint')return'wg_import_err_bad_endpoint';if(e==='no_peer')return'wg_import_err_no_peer';if(e==='multi_peer')return'wg_import_err_multi_peer';return'wg_import_err_missing';};let html='<div class=bad>'+L.wg_import_failed+'</div><ul style=margin:6px 0 0 18px>';for(let i=0;i<parsed.errors.length;i++){const e=parsed.errors[i];const k=keyFor(e);const field=(e.indexOf(':')>=0)?e.split(':')[1]:'';const msg=L[k]||e;html+='<li>'+msg+(field?' ('+field+')':'')+'</li>';}html+='</ul>';panel.innerHTML=html;panel.style.display='';return;}const v=parsed.values;const mask=function(s){return s.length<=8?s:(s.slice(0,4)+String.fromCharCode(8230)+s.slice(-4));};let html='<div>'+L.wg_import_preview+'</div><div class=mono style=margin-top:6px>';html+='<div>Private Key:&nbsp;&nbsp;<b>'+mask(v.priv)+'</b></div>';html+='<div>Tunnel IP:&nbsp;&nbsp;&nbsp;<b>'+v.addr+'</b></div>';html+='<div>Server Key:&nbsp;&nbsp;<b>'+mask(v.pub)+'</b></div>';html+='<div>Endpoint:&nbsp;&nbsp;&nbsp;&nbsp;<b>'+v.endp+'</b></div>';if(!isNaN(v.ka))html+='<div>Keepalive:&nbsp;&nbsp;<b>'+v.ka+' s</b></div>';if(v.psk)html+='<div class=warn style=margin-top:6px>'+L.wg_import_psk_warn+'</div>';html+='</div><div style=margin-top:8px;display:flex;gap:8px><button id=b_wg_apply>'+L.wg_import_apply+'</button><button id=b_wg_cancel onclick=\"wgImportCancel()\">'+L.wg_import_cancel+'</button></div>';panel.innerHTML=html;panel.style.display='';const apply=$('b_wg_apply');if(apply)apply.onclick=function(){wgImportApply(parsed.values);};}"
        "function wgImportCancel(){const panel=$('wg_import_panel');if(panel){panel.style.display='none';panel.innerHTML='';}}"
        "function wgImportApply(v){setv('wg_priv',v.priv);setv('wg_tunaddr',v.addr);setv('wg_srvpub',v.pub);setv('wg_endpoint',v.endp);if(!isNaN(v.ka)&&v.ka>=0&&v.ka<=65535)setv('wg_keepalive',v.ka);wgImportCancel();}"
        "function wgImportPicker(evt){const f=evt.target.files&&evt.target.files[0];if(!f)return;const r=new FileReader();r.onload=function(){const text=String(r.result||'');const parsed=wgImportParse(text);wgImportPreview(parsed);evt.target.value='';};r.readAsText(f);}"
        "const locks={}; const edits={};"
        "function setv(k,v){v=String(v?\?'').trim().replace(',', '.'); if(v==='')return; locks[k]=Date.now()+5000; delete edits[k]; fetch('/api/set?key='+encodeURIComponent(k)+'&value='+encodeURIComponent(v),{cache:'no-store'}).then(r=>r.json()).then(loadAll)}"
        "function bindSaver(el,key){if(!el)return; el.addEventListener('keydown',e=>{if(e.key==='Enter'){setv(key,el.value)}})}"
        "function trackEdit(el,key){if(!el)return; const bump=()=>{edits[key]=Date.now()+10000; toggleDirty(el,key)}; el.addEventListener('input',bump); el.addEventListener('change',bump)}"
        "function toggleDirty(el,key){ if(!el)return; const now=Date.now(); const d=(edits[key]&&now<edits[key]); el.classList.toggle('dirty', !!d); if(!d){ delete edits[key]; } }"
        "function loadStatus(){fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ $('ip').textContent=j.ip; $('rssi').textContent=j.wifi_rssi+' dBm'; $('wtx').textContent=j.wifi_tx_dbm.toFixed(1)+' dBm'; $('heap').textContent=j.free_heap_kb+' KB ('+j.min_free_heap_kb+' KB)'; $('uptime').textContent=j.uptime; $('srv').innerHTML=fmtSrv(j.rtsp_server_enabled); const bto=$('b_srv_toggle');if(bto){if(j.rtsp_server_enabled){bto.textContent='Server: ON';bto.classList.add('active');}else{bto.textContent='Server: OFF';bto.classList.remove('active');}} $('client').textContent=j.client || 'Waiting...'; $('stream').innerHTML=fmtBool(j.streaming); $('rate').textContent=j.current_rate_pkt_s+' pkt/s'; $('lcon').textContent=j.last_rtsp_connect; $('lplay').textContent=j.last_stream_start; const stx=$('sel_tx'); const now=Date.now(); if(stx){ const editing=(edits['wifi_tx']&&now<edits['wifi_tx']); if(!(locks['wifi_tx']&&now<locks['wifi_tx']) && !editing) stx.value=j.wifi_tx_dbm.toFixed(1); toggleDirty(stx,'wifi_tx'); } const fv=$('fwv'); if(fv && j.fw_version){ fv.textContent='v'+j.fw_version; } })}"
        "function loadAudio(){fetch('/api/audio_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const r=$('in_rate'); const g=$('in_gain'); const sb=$('sel_buf'); const s=$('in_shift'); const hp=$('sel_hp'); const hpc=$('in_hp_cutoff'); const now=Date.now(); if(r){ const editing=(edits['rate']&&now<edits['rate']); if(!(locks['rate']&&now<locks['rate']) && !editing) r.value=j.sample_rate; toggleDirty(r,'rate'); } if(g){ const editing=(edits['gain']&&now<edits['gain']); if(!(locks['gain']&&now<locks['gain']) && !editing) g.value=j.gain.toFixed(2); toggleDirty(g,'gain'); } if(sb){ const editing=(edits['buffer']&&now<edits['buffer']); if(!(locks['buffer']&&now<locks['buffer']) && !editing) sb.value=j.buffer_size; toggleDirty(sb,'buffer'); } if(s){ const editing=(edits['shift']&&now<edits['shift']); if(!(locks['shift']&&now<locks['shift']) && !editing) s.value=j.i2s_shift; toggleDirty(s,'shift'); } if(hp){ const editing=(edits['hp_enable']&&now<edits['hp_enable']); if(!(locks['hp_enable']&&now<locks['hp_enable']) && !editing) hp.value=j.hp_enable?'on':'off'; toggleDirty(hp,'hp_enable'); } if(hpc){ const editing=(edits['hp_cutoff']&&now<edits['hp_cutoff']); if(!(locks['hp_cutoff']&&now<locks['hp_cutoff']) && !editing) hpc.value=j.hp_cutoff_hz; toggleDirty(hpc,'hp_cutoff'); } const agc=$('sel_agc'); if(agc){ const editing=(edits['agc_enable']&&now<edits['agc_enable']); if(!(locks['agc_enable']&&now<locks['agc_enable']) && !editing) agc.value=j.agc_enable?'on':'off'; toggleDirty(agc,'agc_enable'); } const agi=$('agc_info'); if(agi){ if(j.agc_enable) agi.textContent='x'+j.agc_multiplier.toFixed(1)+' (eff: '+j.effective_gain.toFixed(1)+'x)'; else agi.textContent=''; } const led=$('sel_led'); if(led){ const editing=(edits['led_mode']&&now<edits['led_mode']); if(!(locks['led_mode']&&now<locks['led_mode']) && !editing) led.value=String(j.led_mode||0); toggleDirty(led,'led_mode'); } $('lat').textContent=j.latency_ms.toFixed(1)+' ms'; $('profile').textContent=profileText(j.buffer_size); const L=T[lang]; const lvl=$('level'); if(lvl){ const pct=j.peak_pct||0, db=j.peak_dbfs||-90, clip=j.clip, cc=j.clip_count||0; if(clip){ lvl.innerHTML = `<span class='bad'>${L.clip_bad}</span> Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS), clips: ${cc}`; } else if(pct>=90){ lvl.innerHTML = `<span class='warn'>${L.clip_warn}</span> Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS)`; } else { lvl.textContent = `Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS) — ${L.clip_ok}`; } } updateAdvice(j); })}"
        "function updateAdvice(a){const L=T[lang]; let tips=[]; if(a.buffer_size<512) tips.push(L.adv_buf512); if(a.buffer_size<1024) tips.push(L.adv_buf1024); if(a.gain>20) tips.push(L.adv_gain); $('adv').textContent=tips.join(' ');}"
        "function loadPerf(){fetch('/api/perf_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const el=$('in_auto'); if(el) el.value=j.auto_recovery?'on':'off'; const thr=$('in_thr'); const chk=$('in_chk'); const mode=$('in_thr_mode'); const sch=$('in_sched'); const hrs=$('in_hours'); const now=Date.now(); if(mode){ const editing=(edits['thr_mode']&&now<edits['thr_mode']); if(!(locks['thr_mode']&&now<locks['thr_mode']) && !editing) mode.value=j.auto_threshold?'auto':'manual'; toggleDirty(mode,'thr_mode'); } if(thr){ const editing=(edits['min_rate']&&now<edits['min_rate']); if(!(locks['min_rate']&&now<locks['min_rate']) && !editing) thr.value=j.restart_threshold_pkt_s; toggleDirty(thr,'min_rate'); } if(chk){ const editing=(edits['check_interval']&&now<edits['check_interval']); if(!(locks['check_interval']&&now<locks['check_interval']) && !editing) chk.value=j.check_interval_min; toggleDirty(chk,'check_interval'); } if(sch){ const editing=(edits['sched_reset']&&now<edits['sched_reset']); if(!(locks['sched_reset']&&now<locks['sched_reset']) && !editing) sch.value=j.scheduled_reset?'on':'off'; toggleDirty(sch,'sched_reset'); } if(hrs){ const editing=(edits['reset_hours']&&now<edits['reset_hours']); if(!(locks['reset_hours']&&now<locks['reset_hours']) && !editing) hrs.value=j.reset_hours; toggleDirty(hrs,'reset_hours'); } $('row_min_rate').style.display=j.auto_threshold?'none':''; })}"
"function loadTherm(){fetch('/api/thermal',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const now=Date.now(); const L=T[lang]; const en=$('sel_oh_enable'); if(en){ const editing=(edits['oh_enable']&&now<edits['oh_enable']); if(!(locks['oh_enable']&&now<locks['oh_enable']) && !editing) en.value=j.protection_enabled?'on':'off'; toggleDirty(en,'oh_enable'); } const lim=$('sel_oh_limit'); if(lim){ const editing=(edits['oh_limit']&&now<edits['oh_limit']); if(!(locks['oh_limit']&&now<locks['oh_limit']) && !editing) lim.value=(Number(j.shutdown_c)||80).toFixed(0); toggleDirty(lim,'oh_limit'); } const sc=$('sel_cpu'); if(sc && !(locks['cpu_freq']&&now<locks['cpu_freq'])){ sc.value=j.cpu_mhz; } const currentValid=(j.current_valid&&typeof j.current_c==='number'&&isFinite(j.current_c)); const cur=$('therm_now'); if(cur) cur.textContent=currentValid?j.current_c.toFixed(1)+' °C':'N/A'; const max=$('therm_max'); if(max){ const maxValid=(typeof j.max_c==='number'&&isFinite(j.max_c)); max.textContent=maxValid?j.max_c.toFixed(1)+' °C':'N/A'; } const cpu=$('therm_cpu'); if(cpu) cpu.textContent=j.cpu_mhz+' MHz'; const status=$('therm_status'); if(status){ if(j.sensor_fault){ status.innerHTML='<span class=warn>'+L.therm_status_sensor_fault+'</span>'; } else if(j.latched_persist){ status.innerHTML='<span class=warn>'+L.therm_status_latched_persist+'</span>'; } else if(!j.protection_enabled){ status.innerHTML='<span class=bad>'+L.therm_status_disabled+'</span>'; } else if(j.manual_restart || j.latched){ status.innerHTML='<span class=warn>'+L.therm_status_latched+'</span>'; } else { status.innerHTML='<span class=ok>'+L.therm_status_ready+'</span>'; } } const latchRow=$('row_therm_latch'); const latchMsg=$('txt_therm_latch'); const latchBtn=$('btn_therm_clear'); if(latchRow){ if(j.latched_persist){ latchRow.style.display=''; if(latchMsg) latchMsg.textContent=L.therm_latch_notice; if(latchBtn){ latchBtn.textContent=L.therm_clear_btn; latchBtn.disabled=false; } } else { latchRow.style.display='none'; if(latchBtn){ latchBtn.disabled=true; } } } const last=$('therm_last'); if(last){ if(j.sensor_fault){ last.textContent=L.therm_last_sensor_fault; } else if(j.last_trip_ts && j.last_trip_ts.length){ let msg=L.therm_last_fmt; const temp=(typeof j.last_trip_c==='number'&&isFinite(j.last_trip_c)&&j.last_trip_c>0)?j.last_trip_c.toFixed(1):'0'; const limit=(Number(j.shutdown_c)||0).toFixed(0); const ts=j.last_trip_ts||L.therm_time_unknown; const ago=j.last_trip_since||L.therm_time_ago_unknown; msg=msg.replace('%TEMP%',temp).replace('%LIMIT%',limit).replace('%TIME%',ts).replace('%AGO%',ago); last.textContent=msg; if(j.latched_persist){ last.textContent+=' — '+L.therm_status_latched_persist; } else if(j.manual_restart){ last.textContent+=' — '+L.therm_status_latched; } } else if(j.last_reason && j.last_reason.length){ last.textContent=j.last_reason; } else { last.textContent=L.therm_last_none; } } })}"
"function loadLogs(){fetch('/api/logs',{cache:'no-store'}).then(r=>r.text()).then(t=>{ const lg=$('logs'); lg.textContent=t; lg.scrollTop=lg.scrollHeight; })}"
"function loadWg(){fetch('/api/wg_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{$('wg_state').innerHTML=(j.state==='up')?'<span class=ok>UP</span>':'<span class=bad>'+(j.state==='connecting'?'CONNECTING':j.state.toUpperCase())+'</span>';$('wg_last_hs').textContent=j.last_handshake;$('wg_rx').textContent=j.rx_pretty+' ('+j.rx_bytes+' bytes)';$('wg_tx').textContent=j.tx_pretty+' ('+j.tx_bytes+' bytes)';const wgRow=$('wg_url_row');if(wgRow){if(j.wg_url){$('rtsp_wg_url').textContent=j.wg_url;wgRow.style.display='';}else{$('rtsp_wg_url').textContent='';wgRow.style.display='none';}}const now=Date.now();const we=$('sel_wg_enable');if(we){const editing=(edits['wg_enable']&&now<edits['wg_enable']);if(!(locks['wg_enable']&&now<locks['wg_enable'])&&!editing)we.value=j.enabled?'on':'off';toggleDirty(we,'wg_enable');}const ka=$('in_wg_keepalive');if(ka){const editing=(edits['wg_keepalive']&&now<edits['wg_keepalive']);if(!(locks['wg_keepalive']&&now<locks['wg_keepalive'])&&!editing)ka.value=j.keepalive;toggleDirty(ka,'wg_keepalive');}const ep=$('in_wg_endpoint');if(ep){const editing=(edits['wg_endpoint']&&now<edits['wg_endpoint']);if(!(locks['wg_endpoint']&&now<locks['wg_endpoint'])&&!editing)ep.value=j.endpoint;toggleDirty(ep,'wg_endpoint');}const ta=$('in_wg_tunaddr');if(ta){const editing=(edits['wg_tunaddr']&&now<edits['wg_tunaddr']);if(!(locks['wg_tunaddr']&&now<locks['wg_tunaddr'])&&!editing)ta.value=j.tunnel_addr||'';toggleDirty(ta,'wg_tunaddr');}const sp=$('in_wg_srvpub');if(sp){const editing=(edits['wg_srvpub']&&now<edits['wg_srvpub']);if(!(locks['wg_srvpub']&&now<locks['wg_srvpub'])&&!editing)sp.value=j.server_public_key;toggleDirty(sp,'wg_srvpub');}}).catch(()=>{})}"
"function loadAll(){loadStatus();loadAudio();loadPerf();loadTherm();loadWg();loadLogs()}"
"function clearThermalLatch(){ const btn=$('btn_therm_clear'); if(btn) btn.disabled=true; fetch('/api/thermal/clear',{method:'POST',cache:'no-store'}).then(r=>r.json()).then(j=>{ if(!j.ok){ console.warn('Thermal latch clear rejected'); } loadAll(); }).catch(()=>loadAll());}"
        "setInterval(loadAll,3000); setInterval(loadLogs,9000);"
        "const sel=document.getElementById('langSel'); sel.value=lang; sel.onchange=()=>{lang=sel.value;localStorage.setItem('lang',lang);applyLang()}; applyLang();"
        "bindSaver($('in_rate'),'rate'); bindSaver($('in_gain'),'gain'); bindSaver($('in_shift'),'shift'); bindSaver($('in_thr'),'min_rate'); bindSaver($('in_chk'),'check_interval'); bindSaver($('in_hours'),'reset_hours'); bindSaver($('in_hp_cutoff'),'hp_cutoff'); bindSaver($('in_wg_endpoint'),'wg_endpoint'); bindSaver($('in_wg_tunaddr'),'wg_tunaddr'); bindSaver($('in_wg_srvpub'),'wg_srvpub'); bindSaver($('in_wg_keepalive'),'wg_keepalive'); bindSaver($('in_wg_priv'),'wg_priv');"
        "trackEdit($('in_rate'),'rate'); trackEdit($('in_gain'),'gain'); trackEdit($('in_shift'),'shift'); trackEdit($('in_thr'),'min_rate'); trackEdit($('in_chk'),'check_interval'); trackEdit($('in_hours'),'reset_hours'); trackEdit($('in_hp_cutoff'),'hp_cutoff');"
"trackEdit($('sel_led'),'led_mode'); trackEdit($('in_auto'),'auto_recovery'); trackEdit($('in_thr_mode'),'thr_mode'); trackEdit($('in_sched'),'sched_reset'); trackEdit($('sel_buf'),'buffer'); trackEdit($('sel_tx'),'wifi_tx'); trackEdit($('sel_hp'),'hp_enable'); trackEdit($('sel_agc'),'agc_enable'); trackEdit($('sel_cpu'),'cpu_freq'); trackEdit($('sel_oh_enable'),'oh_enable'); trackEdit($('sel_oh_limit'),'oh_limit'); trackEdit($('sel_wg_enable'),'wg_enable'); trackEdit($('in_wg_keepalive'),'wg_keepalive'); trackEdit($('in_wg_srvpub'),'wg_srvpub'); trackEdit($('in_wg_tunaddr'),'wg_tunaddr'); trackEdit($('in_wg_endpoint'),'wg_endpoint'); trackEdit($('in_wg_priv'),'wg_priv');"
        "const H=(hid,rid)=>{const h=$(hid), r=$(rid); if(h&&r){ h.onclick=()=>{ r.style.display = (r.style.display==='none'||!r.style.display)?'block':'none'; }; }};"
"H('h_led','row_led_hint'); H('h_rate','row_rate_hint'); H('h_gain','row_gain_hint'); H('h_hpf','row_hpf_hint'); H('h_hpf_cut','row_hpf_cut_hint'); H('h_agc','row_agc_hint'); H('h_buf','row_buf_hint'); H('h_auto','row_auto_hint'); H('h_thr','row_thr_hint'); H('h_thr_mode','row_thrmode_hint'); H('h_chk','row_chk_hint'); H('h_sched','row_sched_hint'); H('h_hours','row_hours_hint'); H('h_tx','row_tx_hint'); H('h_shift','row_shift_hint'); H('h_cpu','row_cpu_hint'); H('h_level','row_level_hint'); H('h_therm_protect','row_therm_hint_protect'); H('h_therm_limit','row_therm_hint_limit'); H('h_wg_enable','row_wg_enable_hint'); H('h_wg_priv','row_wg_priv_hint'); H('h_wg_srvpub','row_wg_srvpub_hint'); H('h_wg_endpoint','row_wg_endpoint_hint'); H('h_wg_tunaddr','row_wg_tunaddr_hint'); H('h_wg_keepalive','row_wg_keepalive_hint'); H('h_wg_import','row_wg_import_hint');"
        "loadAll();"
        "</script></body></html>");
    return h;
}

// HTTP handlery
static void httpIndex() { web.send(200, "text/html; charset=utf-8", htmlIndex()); }

static void httpStatus() {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    String uptimeStr = formatUptime(uptimeSeconds);
    unsigned long runtime = millis() - lastStatsReset;
    uint32_t currentRate = (isStreaming && runtime > 1000) ? (audioPacketsSent * 1000) / runtime : 0;
    String json = "{";
    json += "\"fw_version\":\"" + String(FW_VERSION_STR) + "\",";
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
    apiSendJSON(F("{\"ok\":true}"));
}

void webui_begin() {
    web.on("/", httpIndex);
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
