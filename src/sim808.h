#ifndef SIM808_H
#define SIM808_H

#include <Arduino.h>
#include <utility>

#define CHEIE_ADAFRUIT "aio_KSXE208u6TMca4Ua16a849sKSZNp"
#define USER_ADAFRUIT  "AndreiP25"

#define SIM_RX D4
#define SIM_TX D3
#define SIM_PWR D2

// ─── Numar telefon pentru alerte SMS ─────────────────────────
// Formatul international, ex: "+40712345678"
#define PHONE_ALERT "+40752660673"

// ─── Structuri de date ────────────────────────────────────────

enum ActivityState {
    ACT_RESTING,   // < 0.5 km/h  – stă
    ACT_WALKING,   // 0.5 – 5     – mersul normal
    ACT_RUNNING,   // 5   – 15    – aleargă
    ACT_VEHICLE    // > 15        – în mașină / furat
};
struct GPSLocation {
    bool  fix;
    float latitude;
    float longitude;
    float speed;
};

struct SystemState {
    bool  manualAlertActive;
    float geofenceCenterLat;
    float geofenceCenterLng;
    float geofenceMaxDistance;
    bool  forceBuzzer;
    bool  alert;
};

// ─── Nivel 1 – Primitiva AT ───────────────────────────────────
bool sendSIM808Command(String cmd, unsigned long timeout = 30000);

// ─── Nivel 2 – HTTP generic ───────────────────────────────────
bool   httpPost(const String& url, const String& jsonPayload);
String httpGet(const String& url);

// ─── Nivel 3 – API Adafruit ───────────────────────────────────
bool   sendHTTPData(const String& feedName, const String& value);
String getHTTPData(const String& feedName);
bool   sendHTTPLocation(float lat, float lon, float ele);

// ─── Nivel 4 – GPS & Baterie ──────────────────────────────────
GPSLocation getLocation();
int         getBatteryPercent();   // AT+CBC → 0-100 sau -1 la eroare

// ─── Nivel 5 – High-level ────────────────────────────────────
void   updateSIM(SystemState& state);
void   sendBark(int intensity);
void   sendGPS(GPSLocation loc);
bool   sendSMS(const String& number, const String& message);
bool   setupSIM();   // returneaza true daca init complet (GPRS activ)

// Apelat din sim808.cpp in toate yield()-urile pentru a servi HTTP
// in timpul operatiunilor blocante
extern void webTick();

// Log care apare atat in Serial cat si in web log (192.168.4.1)
extern void rlog(const String& msg);

#endif