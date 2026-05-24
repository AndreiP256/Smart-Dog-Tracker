#include <Arduino.h>
#include <SoftwareSerial.h>
#include <math.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include "sim808.h"

// ─── Pini ─────────────────────────────────────────────────────
#define BUZZER D0
#define LED    D1
#define MIC    A0

// ─── Intervale (ms) ───────────────────────────────────────────
#define INTERVAL_DEFAULT      15000UL
#define INTERVAL_ALERT         2000UL
#define INTERVAL_BATTERY      60000UL
#define INTERVAL_BARK_SAMPLE    100UL

// ─── Barking ──────────────────────────────────────────────────
#define BARK_THRESHOLD   350
#define BARK_CONFIRM_MS  400

// ─── LED blink speeds ─────────────────────────────────────────
#define BLINK_SLOW  1000UL
#define BLINK_FAST   200UL

// ─── WiFi AP pentru debug wireless ───────────────────────────
// Conecteaza-te la reteaua "JackTrack-Debug" si deschide 192.168.4.1
#define WIFI_AP_SSID "JackTrack-Debug"
#define WIFI_AP_PASS "jacktrack"
ESP8266WebServer webServer(80);

// Forward declarations
String activityName(ActivityState a);

// Apelat din sim808.cpp in yield() ca sa nu blocheze HTTP server-ul
void webTick() {
    webServer.handleClient();
}

// ─── Remote log ───────────────────────────────────────────────
// Stocam ultimele N linii de log in RAM, vizibile din browser
#define LOG_LINES 60
String logBuffer[LOG_LINES];
int    logHead = 0;
int    logCount = 0;

void rlog(const String& msg) {
    Serial.println(msg);
    logBuffer[logHead] = msg;
    logHead  = (logHead + 1) % LOG_LINES;
    if (logCount < LOG_LINES) logCount++;
}

// ─── LED blink codes ─────────────────────────────────────────
// Numara blink-urile ca sa diagnostichezi fara serial:
//  1 blink  = setup pornit
//  2 blink  = SIM808 gasit si gata
//  3 blink  = GPRS conectat
//  4 blink  = OVER-VOLTAGE detectat (problema hardware!)
//  5 blink  = SIM808 nu raspunde deloc
void blinkCode(int n) {
    noTone(BUZZER);
    for (int i = 0; i < n; i++) {
        digitalWrite(LED, HIGH); delay(200);
        digitalWrite(LED, LOW);  delay(200);
    }
    delay(600);
}

// ─── State global ─────────────────────────────────────────────
bool          gsmReady       = false;
unsigned long tCitire        = 0;
unsigned long tBaterie       = 0;
unsigned long tBarkSample    = 0;
unsigned long tLedToggle     = 0;
bool          ledState       = false;
unsigned long INTERVAL_CITIRE = INTERVAL_DEFAULT;

SystemState state = {
    false,
    44.4355f, 26.0461f,
    100.0f,
    false, false
};

GPSLocation lastKnownLoc = {false, 0.0f, 0.0f, 0.0f};

ActivityState currentActivity  = ACT_RESTING;
ActivityState lastSentActivity = ACT_RESTING;

// ─── LED modes ────────────────────────────────────────────────
enum LedMode { LED_OFF, LED_SOLID, LED_BLINK_SLOW, LED_BLINK_FAST };
LedMode ledMode = LED_OFF;

void setLedMode(LedMode mode) {
    ledMode = mode;
    if (mode == LED_OFF)   digitalWrite(LED, LOW);
    if (mode == LED_SOLID) digitalWrite(LED, HIGH);
}

void updateLED() {
    if (ledMode != LED_BLINK_SLOW && ledMode != LED_BLINK_FAST) return;
    unsigned long interval = (ledMode == LED_BLINK_FAST) ? BLINK_FAST : BLINK_SLOW;
    unsigned long now = millis();
    if (now - tLedToggle >= interval) {
        tLedToggle = now;
        ledState = !ledState;
        digitalWrite(LED, ledState);
    }
}

// ─── WiFi AP + Web log server ─────────────────────────────────
void setupWiFiAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    rlog("[WiFi] AP pornit: " + String(WIFI_AP_SSID)
       + " | IP: " + WiFi.softAPIP().toString());

    // GET /       → pagina HTML cu auto-refresh
    webServer.on("/", []() {
        String html =
            "<!DOCTYPE html><html><head>"
            "<meta charset='utf-8'>"
            "<meta http-equiv='refresh' content='3'>"
            "<title>JackTrack Log</title>"
            "<style>"
            "body{background:#0c0e0d;color:#dedad2;font:13px 'Courier New',monospace;"
            "padding:16px;margin:0}"
            "h2{color:#d99a2e;letter-spacing:4px;margin-bottom:12px}"
            ".line{border-bottom:1px solid #1c1e1b;padding:3px 0;white-space:pre-wrap}"
            ".err{color:#c94040} .ok{color:#3fa876} .warn{color:#d99a2e}"
            ".ts{color:#555;margin-right:8px}"
            "a{color:#d99a2e}"
            "</style></head><body>"
            "<h2>🐾 JACKTRACK — LIVE LOG</h2>"
            "<p style='color:#555;font-size:11px'>Auto-refresh 3s &nbsp;|&nbsp; "
            "<a href='/log.txt'>Download .txt</a> &nbsp;|&nbsp; "
            "<a href='/reset' onclick=\"return confirm('Reset?')\">↺ Restart ESP</a></p>"
            "<div id='log'>";

        // Afisam in ordine cronologica
        int start = (logCount < LOG_LINES) ? 0 : logHead;
        for (int i = 0; i < logCount; i++) {
            String line = logBuffer[(start + i) % LOG_LINES];
            String cls = "line";
            if (line.indexOf("EROARE") != -1 || line.indexOf("FAIL") != -1
             || line.indexOf("TIMEOUT") != -1 || line.indexOf("OVER-VOLTAGE") != -1)
                cls += " err";
            else if (line.indexOf("SUCCESS") != -1 || line.indexOf("Trimis") != -1
                  || line.indexOf("activ") != -1)
                cls += " ok";
            else if (line.indexOf("ALERT") != -1 || line.indexOf("WARN") != -1
                  || line.indexOf("GEO") != -1)
                cls += " warn";
            html += "<div class='" + cls + "'>" + line + "</div>";
        }
        html += "</div></body></html>";
        webServer.send(200, "text/html", html);
    });

    // GET /log.txt → log brut descarcabil
    webServer.on("/log.txt", []() {
        String txt = "";
        int start = (logCount < LOG_LINES) ? 0 : logHead;
        for (int i = 0; i < logCount; i++)
            txt += logBuffer[(start + i) % LOG_LINES] + "\n";
        webServer.send(200, "text/plain", txt);
    });

    // GET /state → starea curenta ca JSON (util pentru debug rapid)
    webServer.on("/state", []() {
        String json = "{";
        json += "\"gsmReady\":"    + String(gsmReady ? "true" : "false") + ",";
        json += "\"alert\":"       + String(state.alert ? "true" : "false") + ",";
        json += "\"forceBuzzer\":" + String(state.forceBuzzer ? "true" : "false") + ",";
        json += "\"geoLat\":"      + String(state.geofenceCenterLat, 6) + ",";
        json += "\"geoLon\":"      + String(state.geofenceCenterLng, 6) + ",";
        json += "\"geoRadius\":"   + String(state.geofenceMaxDistance, 1) + ",";
        json += "\"lastLat\":"     + String(lastKnownLoc.latitude, 6) + ",";
        json += "\"lastLon\":"     + String(lastKnownLoc.longitude, 6) + ",";
        json += "\"lastFix\":"     + String(lastKnownLoc.fix ? "true" : "false") + ",";
        json += "\"activity\":\""  + activityName(currentActivity) + "\",";
        json += "\"interval\":"    + String(INTERVAL_CITIRE);
        json += "}";
        webServer.send(200, "application/json", json);
    });

    // GET /reset → restart ESP
    webServer.on("/reset", []() {
        webServer.send(200, "text/html",
            "<meta http-equiv='refresh' content='5;url=/'>"
            "<p style='color:#d99a2e;font-family:monospace'>Repornire...</p>");
        delay(500);
        ESP.restart();
    });

    webServer.begin();
    rlog("[WiFi] Web log la http://192.168.4.1");
}

// ─── Activity ─────────────────────────────────────────────────
ActivityState classifyActivity(float speedKmh) {
    if (speedKmh < 0.5f)  return ACT_RESTING;
    if (speedKmh < 5.0f)  return ACT_WALKING;
    if (speedKmh < 15.0f) return ACT_RUNNING;
    return ACT_VEHICLE;
}

String activityName(ActivityState a) {
    switch (a) {
        case ACT_RESTING: return "RESTING";
        case ACT_WALKING: return "WALKING";
        case ACT_RUNNING: return "RUNNING";
        case ACT_VEHICLE: return "VEHICLE";
        default:          return "UNKNOWN";
    }
}

void updateActivityState(const GPSLocation& loc) {
    if (!loc.fix) return;
    ActivityState detected = classifyActivity(loc.speed);
    if (detected != lastSentActivity) {
        rlog("[ACT] " + activityName(lastSentActivity) + " → " + activityName(detected));
        sendHTTPData("activity", activityName(detected));
        lastSentActivity = detected;
        if (detected == ACT_VEHICLE) {
            String msg = "JACK ALERT: Viteza >15 km/h! Posibil in vehicul. "
                         "https://maps.google.com/?q="
                         + String(loc.latitude, 5) + "," + String(loc.longitude, 5);
            sendSMS(PHONE_ALERT, msg);
        }
    }
    currentActivity = detected;
}

// ─── Haversine ────────────────────────────────────────────────
float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371000.0f;
    float dLat = radians(lat2 - lat1);
    float dLon = radians(lon2 - lon1);
    float a = sin(dLat/2)*sin(dLat/2)
            + cos(radians(lat1))*cos(radians(lat2))
            * sin(dLon/2)*sin(dLon/2);
    return R * 2.0f * atan2(sqrt(a), sqrt(1.0f - a));
}

bool isInsideGeofence(const GPSLocation& loc) {
    if (!loc.fix) return true;
    float dist = haversineDistance(
        loc.latitude, loc.longitude,
        state.geofenceCenterLat, state.geofenceCenterLng);
    rlog("[GEO] Distanta: " + String(dist, 1) + " m (max: "
       + String(state.geofenceMaxDistance, 1) + " m)");
    return dist <= state.geofenceMaxDistance;
}

// ─── Barking ──────────────────────────────────────────────────
int  barkPeak   = 0;
bool barkActive = false;
unsigned long barkStart = 0;

void updateBarking() {
    unsigned long now = millis();
    if (now - tBarkSample < INTERVAL_BARK_SAMPLE) return;
    tBarkSample = now;
    int amplitude = abs(analogRead(MIC) - 512);
    if (amplitude > BARK_THRESHOLD) {
        if (!barkActive) { barkActive = true; barkStart = now; barkPeak = amplitude; }
        else {
            if (amplitude > barkPeak) barkPeak = amplitude;
            if (now - barkStart >= BARK_CONFIRM_MS) {
                rlog("[BARK] Detectat! Intensitate: " + String(barkPeak));
                sendBark(barkPeak);
                barkActive = false; barkPeak = 0;
            }
        }
    } else { barkActive = false; }
}

// ─── Poll comenzi Adafruit ────────────────────────────────────
void pollCommands() {
    String cmdBuzzer = getHTTPData("buzzer");
    if      (cmdBuzzer == "ON")  { rlog("[CMD] Buzzer ON");  state.forceBuzzer = true;  }
    else if (cmdBuzzer == "OFF") { rlog("[CMD] Buzzer OFF"); state.forceBuzzer = false; }

    String cmdAlert = getHTTPData("alert");
    if      (cmdAlert == "ON")  { rlog("[CMD] Alerta manuala ON");  state.manualAlertActive = true;  }
    else if (cmdAlert == "OFF") { rlog("[CMD] Alerta manuala OFF"); state.manualAlertActive = false; }

    String cmdSetGeo = getHTTPData("setGeofence");
    if (cmdSetGeo == "1") {
        GPSLocation loc = getLocation();
        if (loc.fix) {
            state.geofenceCenterLat = loc.latitude;
            state.geofenceCenterLng = loc.longitude;
            rlog("[GEO] Centru setat: " + String(loc.latitude, 6)
               + ", " + String(loc.longitude, 6));
            sendHTTPData("geofence-lat", String(state.geofenceCenterLat, 6));
            sendHTTPData("geofence-lon", String(state.geofenceCenterLng, 6));
            sendHTTPData("setGeofence", "0");
        } else { rlog("[GEO] Nu pot seta centrul – no GPS fix"); }
    }

    String latitude  = getHTTPData("geofence-lat");
    String longitude = getHTTPData("geofence-lon");
    float lat_f = latitude.toFloat(), lon_f = longitude.toFloat();
    if (lat_f != 0.0f && lon_f != 0.0f) {
        state.geofenceCenterLat = lat_f;
        state.geofenceCenterLng = lon_f;
    }

    String cmdGeoDist = getHTTPData("geofencing");
    if (cmdGeoDist.length() > 0) {
        float newDist = cmdGeoDist.toFloat();
        if (newDist > 0.0f) {
            state.geofenceMaxDistance = newDist;
            rlog("[GEO] Raza actualizata: " + String(newDist, 1) + " m");
        }
    }
}

// ─── Buzzer ───────────────────────────────────────────────────
void updateBuzzer() {
    if (state.forceBuzzer) tone(BUZZER, 1500);
    else                   noTone(BUZZER);
}

// ─── Alert ────────────────────────────────────────────────────
void updateAlert(const GPSLocation& loc) {
    if (state.manualAlertActive) {
        if (!state.alert) { state.alert = true; sendHTTPData("alert", "1"); }
        INTERVAL_CITIRE = INTERVAL_ALERT;
        setLedMode(LED_BLINK_FAST);
        return;
    }
    bool inside = isInsideGeofence(loc);
    if (inside) {
        if (state.alert) {
            rlog("[GEO] Cainele a revenit in zona!");
            state.alert = false; state.forceBuzzer = false;
            sendHTTPData("alert", "0"); sendHTTPData("buzzer", "OFF");
            sendSMS(PHONE_ALERT, "JACK OK: A revenit in zona sigura.");
        }
        INTERVAL_CITIRE = INTERVAL_DEFAULT;
        setLedMode(LED_SOLID);
    } else {
        if (!state.alert) {
            rlog("[GEO] !!! IESIT DIN GEOFENCE !!!");
            state.alert = true; state.forceBuzzer = true;
            sendHTTPData("alert", "1"); sendHTTPData("buzzer", "ON");
            String msg = "JACK ALERT: A iesit din zona sigura! "
                         "https://maps.google.com/?q="
                         + String(loc.latitude, 5) + "," + String(loc.longitude, 5);
            sendSMS(PHONE_ALERT, msg);
        }
        INTERVAL_CITIRE = INTERVAL_ALERT;
        setLedMode(LED_BLINK_FAST);
    }
}

// ─── GPS update ───────────────────────────────────────────────
void updateGPS() {
    GPSLocation loc = getLocation();
    if (loc.fix) {
        lastKnownLoc = loc;
        rlog("[GPS] Fix! Lat=" + String(loc.latitude, 6)
           + " Lon=" + String(loc.longitude, 6)
           + " Speed=" + String(loc.speed, 1) + " km/h");
        sendGPS(loc);
        updateActivityState(loc);
        updateAlert(loc);
    } else {
        rlog("[GPS] No fix – trimit ultima locatie cunoscuta");
        setLedMode(LED_BLINK_SLOW);
        if (lastKnownLoc.fix) sendGPS(lastKnownLoc);
        updateAlert(loc);
    }
}

// ─── Battery ──────────────────────────────────────────────────
void updateBattery() {
    int pct = getBatteryPercent();
    if (pct < 0) { rlog("[BAT] Nu am putut citi bateria"); return; }
    sendHTTPData("battery", String(pct));
    rlog("[BAT] " + String(pct) + "%");
    if (pct < 15) {
        rlog("[BAT] !!! BATERIE CRITICA !!!");
        sendSMS(PHONE_ALERT, "JACK ALERT: Baterie critica " + String(pct) + "%!");
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED, HIGH); delay(100);
            digitalWrite(LED, LOW);  delay(100);
        }
    }
}

// ─── Setup ────────────────────────────────────────────────────
void setup() {
    pinMode(LED,    OUTPUT);
    pinMode(MIC,    INPUT);
    pinMode(BUZZER, OUTPUT);
    Serial.begin(115200);
    delay(1000);

    rlog("\n=============================");
    rlog("  Smart Dog Tracker – Boot");
    rlog("=============================");

    // 1 blink = setup pornit
    blinkCode(1);

    // WiFi AP primul – asa poti vedea ce se intampla imediat
    setupWiFiAP();

    // LED blink rapid pe parcursul init-ului SIM
    setLedMode(LED_BLINK_FAST);
    rlog("[SETUP] Init SIM808...");

    gsmReady = setupSIM();

    if (gsmReady) {
        blinkCode(2);   // 2 blink = SIM + GPRS gata
        tone(BUZZER, 2000, 150); delay(250);
        tone(BUZZER, 2500, 150); delay(250);
        noTone(BUZZER);
        setLedMode(LED_SOLID);
        rlog("[SETUP] SIM gata!");
    } else {
        blinkCode(5);   // 5 blink = init esuat
        rlog("[SETUP] SIM init esuat – ruleaza fara GSM.");
        setLedMode(LED_BLINK_SLOW);
    }
    rlog("[SETUP] Conecteaza-te la WiFi '" + String(WIFI_AP_SSID)
       + "' si deschide http://192.168.4.1");
}

// ─── Loop ─────────────────────────────────────────────────────
void loop() {
    webServer.handleClient();   // serveaza cererile web
    updateSIM(state);
    updateLED();
    updateBuzzer();
    updateBarking();

    if (!gsmReady) return;

    unsigned long now = millis();
    if (now - tCitire >= INTERVAL_CITIRE) {
        tCitire = now;
        pollCommands();
        updateGPS();
    }
    if (now - tBaterie >= INTERVAL_BATTERY) {
        tBaterie = now;
        updateBattery();
    }
}