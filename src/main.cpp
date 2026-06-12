#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"

struct SensorDef {
    uint8_t     pin;
    const char* label;
    const char* uniqueId;
    const char* discoveryTopic;
    const char* stateTopic;
};

static const SensorDef SENSORS[] = {
    { PIN_RHS_TOP,
      "RHS Tank Top",    "hw_rhs_top",
      "homeassistant/sensor/hw_rhs_top/temperature/config",
      "homeassistant/sensor/hw_rhs_top/temperature/state" },
    { PIN_RHS_BOTTOM,
      "RHS Tank Bottom", "hw_rhs_bottom",
      "homeassistant/sensor/hw_rhs_bottom/temperature/config",
      "homeassistant/sensor/hw_rhs_bottom/temperature/state" },
    { PIN_LHS_TOP,
      "LHS Tank Top",    "hw_lhs_top",
      "homeassistant/sensor/hw_lhs_top/temperature/config",
      "homeassistant/sensor/hw_lhs_top/temperature/state" },
    { PIN_LHS_BOTTOM,
      "LHS Tank Bottom", "hw_lhs_bottom",
      "homeassistant/sensor/hw_lhs_bottom/temperature/config",
      "homeassistant/sensor/hw_lhs_bottom/temperature/state" },
};
static const int NUM_SENSORS = 4;

OneWire ow0(PIN_RHS_TOP), ow1(PIN_RHS_BOTTOM), ow2(PIN_LHS_TOP), ow3(PIN_LHS_BOTTOM);
DallasTemperature ds[NUM_SENSORS];

float temps[NUM_SENSORS];

// ── Display ───────────────────────────────────────────────────────────────────

LiquidCrystal_I2C lcd(0x27, 16, 2);
bool lcdPresent = false;

static bool lcdProbe() {
    Wire.begin();
    Wire.beginTransmission(0x27);
    return Wire.endTransmission() == 0;
}

static void formatTempInt(char* buf, float t) {
    if (t == DEVICE_DISCONNECTED_C) {
        strcpy(buf, "---  ");
    } else {
        int deg = (int)roundf(t);
        if (deg <   0) deg = 0;
        if (deg > 999) deg = 999;
        snprintf(buf, 6, "%03dC ", deg);
    }
}

void updateDisplay() {
    if (!lcdPresent) return;
    char lt[6], rt[6], lb[6], rb[6];
    formatTempInt(lt, temps[2]);
    formatTempInt(rt, temps[0]);
    formatTempInt(lb, temps[3]);
    formatTempInt(rb, temps[1]);
    char line1[17], line2[17];
    snprintf(line1, sizeof(line1), "LT:%sRT:%s", lt, rt);
    snprintf(line2, sizeof(line2), "LB:%sRB:%s", lb, rb);
    lcd.setCursor(0, 0); lcd.print(line1);
    lcd.setCursor(0, 1); lcd.print(line2);
}

// ── WiFi / MQTT / OTA / Web ───────────────────────────────────────────────────

#if ENABLE_WIFI
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Preferences.h>

#define AVAIL_TOPIC          "homeassistant/sensor/tank_monitor/availability"
#define LASTSEEN_DISC_TOPIC  "homeassistant/sensor/hw_last_seen/config"
#define LASTSEEN_STATE_TOPIC "homeassistant/sensor/hw_last_seen/state"

// Runtime MQTT settings — loaded from NVS on boot, fallback to config.h defaults
static char g_mqttHost[64];
static int  g_mqttPort;
static char g_mqttUser[32];
static char g_mqttPass[64];

static Preferences   prefs;
static WebServer     webServer(80);
static unsigned long lastMqttRetry    = 0;
static bool          otaReady         = false;
static unsigned long mqttPublishCount = 0;
static char          mqttConnectedTime[20] = "--";
static char          mqttLastPubTime[20]   = "--";

// LCD display rotation: alternates between temps and IP every DISPLAY_TOGGLE_MS
static unsigned long lastDisplayToggle = 0;
static bool          showingIp         = false;
static const unsigned long DISPLAY_TOGGLE_MS = 5000UL;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

static void setupOTA();
static void setupWebServer();

static void nowToStr(char* buf, size_t len) {
    struct tm t;
    if (getLocalTime(&t, 0)) strftime(buf, len, "%d %b %H:%M:%S", &t);
    else                     strncpy(buf, "--", len);
}

static bool mqttPublish(const char* topic, const char* payload, bool retain = false) {
    bool ok = mqtt.publish(topic, payload, retain);
    if (ok) { mqttPublishCount++; nowToStr(mqttLastPubTime, sizeof(mqttLastPubTime)); }
    return ok;
}

static void loadSettings() {
    prefs.begin("tank", true);
    String h = prefs.getString("mqtt_host", MQTT_HOST);
    h.toCharArray(g_mqttHost, sizeof(g_mqttHost));
    g_mqttPort = prefs.getInt("mqtt_port", MQTT_PORT);
    String u = prefs.getString("mqtt_user", MQTT_USER);
    u.toCharArray(g_mqttUser, sizeof(g_mqttUser));
    String p = prefs.getString("mqtt_pass", MQTT_PASS);
    p.toCharArray(g_mqttPass, sizeof(g_mqttPass));
    prefs.end();
}

static void persistSettings(const String& host, int port, const String& user, const String& pass) {
    prefs.begin("tank", false);
    prefs.putString("mqtt_host", host);
    prefs.putInt("mqtt_port", port);
    prefs.putString("mqtt_user", user);
    prefs.putString("mqtt_pass", pass);
    prefs.end();
}

static bool publishDiscovery(const SensorDef& s) {
    JsonDocument doc;
    doc["name"]                = s.label;
    doc["unique_id"]           = s.uniqueId;
    doc["state_topic"]         = s.stateTopic;
    doc["availability_topic"]  = AVAIL_TOPIC;
    doc["unit_of_measurement"] = "°C";
    doc["device_class"]        = "temperature";
    doc["state_class"]         = "measurement";
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = "hot_water_tanks";
    device["name"]           = "Hot Water Tanks";
    device["model"]          = "ESP32 + DS18B20";
    char payload[512];
    serializeJson(doc, payload);
    return mqtt.publish(s.discoveryTopic, payload, /*retain=*/true);
}

static bool publishLastSeenDiscovery() {
    JsonDocument doc;
    doc["name"]               = "Last Seen";
    doc["unique_id"]          = "hw_last_seen";
    doc["state_topic"]        = LASTSEEN_STATE_TOPIC;
    doc["availability_topic"] = AVAIL_TOPIC;
    doc["device_class"]       = "timestamp";
    doc["entity_category"]    = "diagnostic";
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = "hot_water_tanks";
    device["name"]           = "Hot Water Tanks";
    device["model"]          = "ESP32 + DS18B20";
    char payload[512];
    serializeJson(doc, payload);
    return mqtt.publish(LASTSEEN_DISC_TOPIC, payload, /*retain=*/true);
}

static void publishLastSeen() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return;  // NTP not yet synced
    char ts[26];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &t);
    mqttPublish(LASTSEEN_STATE_TOPIC, ts);
}

// Non-blocking MQTT connect — tries at most once every 5 s so the web server stays responsive
static bool tryConnectMqtt() {
    if (mqtt.connected()) return true;
    if (WiFi.status() != WL_CONNECTED) return false;
    unsigned long now = millis();
    if (lastMqttRetry > 0 && now - lastMqttRetry < 5000UL) return false;
    lastMqttRetry = now;
    Serial.print("Connecting to MQTT broker...");
    // LWT: broker publishes "offline" (retained) if we disconnect unexpectedly
    if (mqtt.connect("tank-monitor", g_mqttUser, g_mqttPass,
                     AVAIL_TOPIC, /*qos*/1, /*retain*/true, "offline")) {
        Serial.println("connected");
        mqtt.publish(AVAIL_TOPIC, "online", /*retain=*/true);
        nowToStr(mqttConnectedTime, sizeof(mqttConnectedTime));
        mqtt.publish("homeassistant/sensor/tank_monitor/temperature/config", "", true);
        for (int i = 0; i < NUM_SENSORS; i++) {
            bool ok = publishDiscovery(SENSORS[i]);
            Serial.printf("Discovery %s: %s\n", SENSORS[i].label, ok ? "published" : "FAILED");
        }
        publishLastSeenDiscovery();
        return true;
    }
    Serial.printf("failed (rc=%d), retry in 5s\n", mqtt.state());
    return false;
}

// Non-blocking WiFi maintenance — called every loop(). On first successful connect (and on
// reconnect after drop) lazily initialises OTA, web server, and NTP. Retries every 30 s.
static void maintainWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!otaReady) {
            otaReady = true;
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            setupOTA();
            setupWebServer();
            String ip = WiFi.localIP().toString();
            Serial.printf("WiFi connected. IP: %s\n", ip.c_str());
            Serial.printf("Web: http://%s/  OTA: pio run -t upload --upload-port=%s\n",
                          ip.c_str(), ip.c_str());
            if (lcdPresent) {
                lcd.clear();
                lcd.setCursor(0, 0); lcd.print("WiFi connected");
                lcd.setCursor(0, 1); lcd.print(ip.substring(0, 16));
            }
            lastDisplayToggle = millis();
        }
        return;
    }

    if (otaReady) {
        Serial.println("WiFi lost — will retry every 30 s");
        otaReady = false;
    }

    static unsigned long lastAttempt = 0;
    unsigned long now = millis();
    if (lastAttempt > 0 && now - lastAttempt < 30000UL) return;
    lastAttempt = now;

    Serial.printf("WiFi connecting to %s...\n", WIFI_SSID);
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void setupOTA() {
    ArduinoOTA.setHostname("tank-monitor");
    ArduinoOTA.onStart([]() {
        Serial.println("OTA update starting...");
        if (lcdPresent) {
            lcd.clear();
            lcd.print("OTA Updating...");
        }
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("OTA update complete");
        if (lcdPresent) {
            lcd.clear();
            lcd.print("OTA Done!");
        }
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA progress: %u%%\n", progress * 100 / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA error [%u]\n", error);
    });
    ArduinoOTA.begin();
    Serial.println("OTA ready (hostname: tank-monitor)");
}

// ── LCD rotation ──────────────────────────────────────────────────────────────

static void showIpScreen() {
    if (!lcdPresent || WiFi.status() != WL_CONNECTED) return;
    String ip = WiFi.localIP().toString();
    char line2[17];
    snprintf(line2, sizeof(line2), "%-16s", ip.c_str());
    lcd.setCursor(0, 0); lcd.print("  IP Address:   ");
    lcd.setCursor(0, 1); lcd.print(line2);
}

// Call every loop(); toggles between temp and IP screen every DISPLAY_TOGGLE_MS
static void tickDisplay() {
    if (!lcdPresent) return;
    unsigned long now = millis();
    if (now - lastDisplayToggle < DISPLAY_TOGGLE_MS) return;
    lastDisplayToggle = now;
    showingIp = !showingIp;
    if (showingIp) showIpScreen();
    else           updateDisplay();
}

// ── Web UI ────────────────────────────────────────────────────────────────────

static const char PAGE_CSS[] =
    "*{box-sizing:border-box}"
    "body{margin:0;padding:0;background:#f0f0f0;font-family:sans-serif;font-size:14px;color:#222}"
    ".wrap{max-width:920px;margin:0 auto;padding:20px 24px}"
    "h1{margin:0 0 12px;font-size:1.8em;font-weight:700}"
    "h2{margin:20px 0 8px;font-size:1.1em;font-weight:700}"
    ".topbar{display:flex;align-items:center;gap:20px;border-bottom:1px solid #ccc;"
             "padding-bottom:10px;margin-bottom:20px}"
    ".topbar a{text-decoration:none;color:#0066cc;font-size:.95em}"
    ".topbar a.on{font-weight:700}"
    ".topbar form{margin:0 0 0 auto}"
    ".btn-reboot{background:#cc0000;color:#fff;border:none;border-radius:4px;"
                "padding:5px 14px;cursor:pointer;font-size:.9em}"
    ".clock{position:fixed;top:16px;right:20px;background:#fff;border:1px solid #ccc;"
            "border-radius:4px;padding:6px 14px;text-align:right;font-size:.8em;"
            "line-height:1.7;color:#444}"
    ".section{border:1px solid #ccc;border-radius:4px;margin-bottom:16px;"
              "overflow:hidden;background:#fff}"
    ".shdr{background:#333;color:#fff;font-weight:700;padding:8px 14px;font-size:.9em}"
    ".section table{width:100%;border-collapse:collapse}"
    ".section td{padding:9px 14px;border-bottom:1px solid #ebebeb;vertical-align:middle}"
    ".section tr:last-child td{border-bottom:none}"
    ".section td:first-child{width:38%;color:#555}"
    ".ok{color:#2a9d2a}.na{color:#aaa}.err{color:#cc0000}"
    ".connected{color:#2a9d2a;font-weight:600}"
    ".disconnected{color:#cc0000;font-weight:600}"
    "input[type=text],input[type=number],input[type=password]{"
        "width:100%;padding:6px 8px;border:1px solid #ccc;border-radius:3px;font-size:1em}"
    ".hint{font-size:.8em;color:#aaa;margin-top:3px}"
    ".actions{display:flex;gap:10px;margin:10px 0 4px}"
    ".btn{background:#333;color:#fff;border:none;border-radius:4px;"
         "padding:8px 16px;cursor:pointer;font-size:.9em}"
    ".btn:hover{background:#555}"
    ".foot{font-size:.8em;color:#aaa;margin-top:6px}";

static String pageHead(bool autoRefresh = false) {
    String h = "<!DOCTYPE html><html><head>"
               "<meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Tank Monitor</title>";
    if (autoRefresh) h += "<meta http-equiv='refresh' content='30'>";
    h += "<style>"; h += PAGE_CSS; h += "</style></head><body><div class='wrap'>";
    // Fixed clock widget
    h += "<div class='clock'>Local time: ";
    struct tm t;
    if (getLocalTime(&t, 0)) {
        char buf[20]; strftime(buf, sizeof(buf), "%H:%M:%S %d-%m-%y", &t); h += buf;
    } else { h += "--"; }
    h += "<br>Up: ";
    unsigned long s = millis() / 1000;
    int dd = s / 86400; s %= 86400;
    int hh = s / 3600;  s %= 3600;
    int mm = s / 60;    s %= 60;
    char up[20]; snprintf(up, sizeof(up), "%dd %02d:%02d:%02d", dd, hh, (int)mm, (int)s);
    h += up; h += "</div>";
    return h;
}

static String navBar(const char* active) {
    String n = "<h1>Tank Monitor (ESP32)</h1><div class='topbar'>";
    n += strcmp(active, "dash") == 0
         ? "<a href='/' class='on'>Dashboard</a>" : "<a href='/'>Dashboard</a>";
    n += strcmp(active, "mqtt") == 0
         ? "<a href='/mqtt' class='on'>MQTT</a>" : "<a href='/mqtt'>MQTT</a>";
    n += "<form method='POST' action='/reboot'>"
         "<button class='btn-reboot' type='submit'>Reboot</button></form>"
         "</div>";
    return n;
}

// ── Dashboard ─────────────────────────────────────────────────────────────────

static void handleDashboard() {
    String html = pageHead(/*autoRefresh=*/true);
    html += navBar("dash");
    html += "<div class='section'><div class='shdr'>Sensors</div><table>";
    for (int i = 0; i < NUM_SENSORS; i++) {
        html += "<tr><td>"; html += SENSORS[i].label; html += "</td>";
        if (temps[i] == DEVICE_DISCONNECTED_C) {
            html += "<td class='na'>--</td><td class='na'>--</td>";
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "<td class='ok'>%.1f&deg;C</td><td>%.1f&deg;F</td>",
                     temps[i], temps[i] * 9.0f / 5.0f + 32.0f);
            html += buf;
        }
        html += "</tr>";
    }
    html += "</table></div>";
    html += "<p class='foot'>";
    if (mqtt.connected()) html += "<span class='ok'>&#x25cf;&nbsp;MQTT connected</span>";
    else                  html += "<span class='err'>&#x25cf;&nbsp;MQTT disconnected &mdash; "
                                  "<a href='/mqtt'>check settings</a></span>";
    html += " &middot; auto-refreshes every 30&nbsp;s</p>";
    html += "</div></body></html>";
    webServer.send(200, "text/html", html);
}

// ── MQTT status + settings ────────────────────────────────────────────────────

static void handleMqttPage() {
    String html = pageHead();
    html += navBar("mqtt");

    // Status card
    html += "<h2>MQTT</h2>"
            "<div class='section'><div class='shdr'>Status</div><table>";
    html += "<tr><td>Broker</td><td>"; html += g_mqttHost; html += ":";
            html += g_mqttPort; html += "</td></tr>";
    html += "<tr><td>Connection</td><td>";
    if (mqtt.connected()) html += "<span class='connected'>&#x25cf;&nbsp;Connected</span>";
    else                  html += "<span class='disconnected'>&#x25cf;&nbsp;Disconnected</span>";
    html += "</td></tr>";
    html += "<tr><td>Last connected</td><td>"; html += mqttConnectedTime; html += "</td></tr>";
    html += "<tr><td>Messages published</td><td>"; html += String(mqttPublishCount); html += "</td></tr>";
    html += "<tr><td>Last published</td><td>"; html += mqttLastPubTime; html += "</td></tr>";
    html += "</table></div>";

    // Action buttons
    html += "<div class='actions'>"
            "<form method='POST' action='/mqtt/reconnect' style='margin:0'>"
            "<button class='btn' type='submit'>Force reconnect</button></form>"
            "<form method='POST' action='/mqtt/discover' style='margin:0'>"
            "<button class='btn' type='submit'>Republish discovery</button></form>"
            "<form method='POST' action='/mqtt/publish' style='margin:0'>"
            "<button class='btn' type='submit'>Send last reading</button></form>"
            "</div>"
            "<p class='foot'>Republish discovery re-sends the HA auto-discovery config messages. "
            "Send last reading republishes the most recent sensor values.</p>";

    // Settings card
    html += "<h2>MQTT Settings</h2>"
            "<form method='POST' action='/save'>"
            "<div class='section'>"
            "<div class='shdr'>Broker</div><table>"
            "<tr><td>Host / IP</td><td>"
            "<input type='text' name='host' value='"; html += g_mqttHost;
    html += "' required></td></tr>"
            "<tr><td>Port</td><td>"
            "<input type='number' name='port' value='"; html += g_mqttPort;
    html += "' min='1' max='65535' required></td></tr>"
            "</table>"
            "<div class='shdr'>Credentials</div><table>"
            "<tr><td>Username</td><td>"
            "<input type='text' name='user' autocomplete='off' value='"; html += g_mqttUser;
    html += "'></td></tr>"
            "<tr><td>Password</td><td>"
            "<input type='password' name='pass' autocomplete='new-password' placeholder='(unchanged)'>"
            "<div class='hint'>Leave blank to keep current password.</div>"
            "</td></tr>"
            "</table></div>"
            "<div class='actions'>"
            "<button class='btn' type='submit'>Save &amp; reconnect</button></div>"
            "<p class='foot'>Firmware build: " __DATE__ " " __TIME__ "</p>"
            "</form>";

    html += "</div></body></html>";
    webServer.send(200, "text/html", html);
}

static void handleMqttReconnect() {
    mqtt.disconnect();
    lastMqttRetry = 0;
    webServer.sendHeader("Location", "/mqtt");
    webServer.send(303, "text/plain", "");
}

static void handleMqttDiscover() {
    if (mqtt.connected()) {
        for (int i = 0; i < NUM_SENSORS; i++) publishDiscovery(SENSORS[i]);
        publishLastSeenDiscovery();
    }
    webServer.sendHeader("Location", "/mqtt");
    webServer.send(303, "text/plain", "");
}

static void handleMqttPublish() {
    if (mqtt.connected()) {
        for (int i = 0; i < NUM_SENSORS; i++) {
            if (temps[i] != DEVICE_DISCONNECTED_C) {
                char payload[16];
                snprintf(payload, sizeof(payload), "%.1f", temps[i]);
                mqttPublish(SENSORS[i].stateTopic, payload);
            } else {
                mqttPublish(SENSORS[i].stateTopic, "unavailable");
            }
        }
        publishLastSeen();
    }
    webServer.sendHeader("Location", "/mqtt");
    webServer.send(303, "text/plain", "");
}

static void handleSave() {
    String host = webServer.arg("host");
    int    port = webServer.arg("port").toInt();
    String user = webServer.arg("user");
    String pass = webServer.arg("pass");

    if (host.isEmpty() || port < 1 || port > 65535) {
        webServer.send(400, "text/plain", "Bad request: host and valid port are required");
        return;
    }
    if (pass.isEmpty()) pass = String(g_mqttPass);  // keep existing password if blank

    persistSettings(host, port, user, pass);

    String html = pageHead() + navBar("mqtt");
    html += "<p>Settings saved. Rebooting&hellip;</p>"
            "<p class='foot'>Reconnect in a few seconds, then <a href='/mqtt'>reload</a>.</p>"
            "</div></body></html>";
    webServer.send(200, "text/html", html);
    delay(1000);
    ESP.restart();
}

static void handleReboot() {
    String html = pageHead() + navBar("dash");
    html += "<p>Rebooting&hellip;</p>"
            "<p class='foot'>Reconnect in a few seconds, then <a href='/'>reload</a>.</p>"
            "</div></body></html>";
    webServer.send(200, "text/html", html);
    delay(500);
    ESP.restart();
}

static void setupWebServer() {
    webServer.on("/",               HTTP_GET,  handleDashboard);
    webServer.on("/mqtt",           HTTP_GET,  handleMqttPage);
    webServer.on("/mqtt/reconnect", HTTP_POST, handleMqttReconnect);
    webServer.on("/mqtt/discover",  HTTP_POST, handleMqttDiscover);
    webServer.on("/mqtt/publish",   HTTP_POST, handleMqttPublish);
    webServer.on("/save",           HTTP_POST, handleSave);
    webServer.on("/reboot",         HTTP_POST, handleReboot);
    webServer.on("/settings",       HTTP_GET,  []() {   // backward-compat redirect
        webServer.sendHeader("Location", "/mqtt");
        webServer.send(301, "text/plain", "");
    });
    webServer.begin();
}

#endif // ENABLE_WIFI

// ── Setup / Loop ──────────────────────────────────────────────────────────────

unsigned long lastReadAt = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    lcdPresent = lcdProbe();
    if (lcdPresent) {
        lcd.init();
        lcd.backlight();
        lcd.print("Tank Monitor");
        lcd.setCursor(0, 1);
        lcd.print("Starting up...");
        Serial.println("LCD display: found");
    } else {
        Serial.println("LCD display: not found, running without display");
    }

    OneWire* buses[] = { &ow0, &ow1, &ow2, &ow3 };
    for (int i = 0; i < NUM_SENSORS; i++) {
        temps[i] = DEVICE_DISCONNECTED_C;
        ds[i].setOneWire(buses[i]);
        ds[i].begin();
        Serial.printf("GPIO %2d (%s): %d sensor(s)\n",
                      SENSORS[i].pin, SENSORS[i].label, ds[i].getDeviceCount());
    }
    // Discard the DS18B20 power-on reset value (always 85°C) before the main loop reads
    for (int i = 0; i < NUM_SENSORS; i++) ds[i].requestTemperatures();
    delay(1000);

#if ENABLE_WIFI
    loadSettings();
    mqtt.setServer(g_mqttHost, g_mqttPort);
    mqtt.setBufferSize(1024);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // first attempt; maintainWifi() retries in loop()
#endif

    updateDisplay();
}

void loop() {
#if ENABLE_WIFI
    maintainWifi();
    if (WiFi.status() == WL_CONNECTED) {
        tryConnectMqtt();
        mqtt.loop();
        if (otaReady) ArduinoOTA.handle();
        webServer.handleClient();
    }
    tickDisplay();
#endif

    unsigned long now = millis();
    if (lastReadAt == 0 || now - lastReadAt >= READ_INTERVAL_MS) {
        for (int i = 0; i < NUM_SENSORS; i++) {
            ds[i].requestTemperatures();
            temps[i] = ds[i].getTempCByIndex(0);

            if (temps[i] == DEVICE_DISCONNECTED_C) {
                Serial.printf("%-16s: not connected\n", SENSORS[i].label);
#if ENABLE_WIFI
                if (mqtt.connected()) mqttPublish(SENSORS[i].stateTopic, "unavailable");
#endif
            } else {
                Serial.printf("%-16s: %.1f°C  (%.1f°F)\n",
                              SENSORS[i].label, temps[i], temps[i] * 9.0f / 5.0f + 32.0f);
#if ENABLE_WIFI
                if (mqtt.connected()) {
                    char payload[16];
                    snprintf(payload, sizeof(payload), "%.1f", temps[i]);
                    mqttPublish(SENSORS[i].stateTopic, payload);
                }
#endif
            }
        }
#if ENABLE_WIFI
        if (mqtt.connected()) publishLastSeen();
#endif
        lastReadAt = millis();
        // Only redraw temps if the LCD is currently showing the temp screen
#if ENABLE_WIFI
        if (!showingIp) updateDisplay();
#else
        updateDisplay();
#endif
    }
}
