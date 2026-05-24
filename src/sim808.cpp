#include "sim808.h"
#include <SoftwareSerial.h>

SoftwareSerial sim808(SIM_RX, SIM_TX);

#define USER_ADAFRUIT  "AndreiP25"
#define CHEIE_ADAFRUIT "aio_KSXE208u6TMca4Ua16a849sKSZNp"

// ============================================================
// NIVEL 1 - Primitiva AT
// ============================================================

bool sendSIM808Command(String cmd, unsigned long timeout) {
    sim808.println(cmd);
    unsigned long start = millis();
    String raspuns = "";

    while (millis() - start < timeout) {
        if (sim808.available()) {
            char c = sim808.read();
            raspuns += c;
            if (raspuns.indexOf("OK")    != -1) return true;
            if (raspuns.indexOf("ERROR") != -1) return false;
        }
        yield();
    }
    Serial.println("[TIMEOUT] Comanda: " + cmd);
    return false;
}

// ============================================================
// NIVEL 2 - Nucleu HTTP (un POST și un GET generic)
// ============================================================

static bool httpInit() {
    if (!sendSIM808Command("AT+HTTPINIT")) {
        sendSIM808Command("AT+HTTPTERM");
        return sendSIM808Command("AT+HTTPINIT");
    }
    return true;
}

static void httpSetCommonParams(const String& url) {
    sendSIM808Command("AT+HTTPPARA=\"CID\",1");
    sendSIM808Command("AT+HTTPPARA=\"URL\",\"" + url + "\"");
    sendSIM808Command("AT+HTTPPARA=\"USERDATA\",\"X-AIO-Key: " + String(CHEIE_ADAFRUIT) + "\"");
}

static bool httpWaitForAction(int actionType) {
    unsigned long start = millis();
    String response = "";

    while (millis() - start < 8000) {
        if (sim808.available()) {
            char c = sim808.read();
            response += c;
            Serial.write(c);
            if (response.indexOf(",200,") != -1 || response.indexOf(",201,") != -1)
                return true;
            if (response.indexOf(",4") != -1)
                return false;
        }
        yield();
    }
    return false;
}

bool httpPost(const String& url, const String& jsonPayload) {
    if (!httpInit()) return false;

    httpSetCommonParams(url);
    sendSIM808Command("AT+HTTPPARA=\"CONTENT\",\"application/json\"");

    sim808.println("AT+HTTPDATA=" + String(jsonPayload.length()) + ",10000");
    delay(2000);
    sim808.println(jsonPayload);
    delay(1000);

    sim808.println("AT+HTTPACTION=1");
    bool ok = httpWaitForAction(1);

    sendSIM808Command("AT+HTTPTERM");
    return ok;
}

String httpGet(const String& url) {
    if (!httpInit()) return "";

    httpSetCommonParams(url);
    sim808.println("AT+HTTPACTION=0");

    // Asteapta confirmarea descarcarii
    unsigned long start = millis();
    String response = "";
    int dataLength = 0;

    while (millis() - start < 8000) {
        if (sim808.available()) {
            char c = sim808.read();
            response += c;

            int idx = response.indexOf("+HTTPACTION: 0,200,");
            if (idx != -1 && response.endsWith("\n")) {
                String lenStr = response.substring(idx + 19);
                lenStr.trim();
                dataLength = lenStr.toInt();
                break;
            }
            if (response.indexOf("+HTTPACTION: 0,4") != -1) break;
        }
        yield();
    }

    String valoare = "";
    if (dataLength > 0) {
        sim808.println("AT+HTTPREAD");
        unsigned long readStart = millis();
        String jsonRaw = "";

        while (millis() - readStart < 4000) {
            if (sim808.available()) jsonRaw += (char)sim808.read();
            yield();
        }

        // Parsare simpla fara librarie JSON
        int idx = jsonRaw.indexOf("\"value\":\"");
        if (idx != -1) {
            int start = idx + 9;
            int end   = jsonRaw.indexOf("\"", start);
            if (end != -1) valoare = jsonRaw.substring(start, end);
        }
    }

    sendSIM808Command("AT+HTTPTERM");
    Serial.println("[HTTP GET] Valoare: " + valoare);
    return valoare;
}

// ============================================================
// NIVEL 3 - API Adafruit (wrappers de business)
// ============================================================

static String adafruitUrl(const String& feedName, const String& suffix = "/data") {
    return "http://io.adafruit.com/api/v2/" + String(USER_ADAFRUIT) + "/feeds/" + feedName + suffix;
}

bool sendHTTPData(const String& feedName, const String& value) {
    Serial.println("\n--- [HTTP] Trimitere pe feed: " + feedName + " ---");
    String payload = "{\"value\":\"" + value + "\"}";
    return httpPost(adafruitUrl(feedName), payload);
}

String getHTTPData(const String& feedName) {
    Serial.println("\n--- [HTTP] Citire de pe feed: " + feedName + " ---");
    return httpGet(adafruitUrl(feedName, "/data/last"));
}

bool sendHTTPLocation(float lat, float lon, float ele) {
    Serial.println("\n--- [HTTP] Trimitere locatie GPS ---");
    String payload = "{\"value\":\"0\","
                     "\"lat\":"  + String(lat, 6) + ","
                     "\"lon\":"  + String(lon, 6) + ","
                     "\"ele\":"  + String(ele, 1) + "}";
    return httpPost(adafruitUrl("location"), payload);
}

// ============================================================
// NIVEL 4 - GPS
// ============================================================

static float convertNMEAToDecimal(float raw) {
    int degrees  = (int)(raw / 100);
    float minutes = raw - (degrees * 100.0);
    return degrees + (minutes / 60.0);
}

GPSLocation getLocation() {
    GPSLocation loc = {false, 0.0, 0.0, 0.0};

    sim808.println("AT+CGPSINF=32");
    unsigned long start = millis();
    String raspuns = "";

    while (millis() - start < 3000) {
        if (sim808.available()) {
            char c = sim808.read();
            raspuns += c;
            if (raspuns.indexOf("OK") != -1) break;
        }
        yield();
    }

    int idxInf = raspuns.indexOf("+CGPSINF: 32,");
    if (idxInf == -1) { Serial.println("[GPS] Format invalid."); return loc; }

    String date = raspuns.substring(idxInf + 13);
    date.trim();

    int virgule[15];
    int count = 0;
    for (int i = 0; i < (int)date.length() && count < 15; i++)
        if (date[i] == ',') virgule[count++] = i;

    if (count < 6) { Serial.println("[GPS] Date incomplete."); return loc; }

    if (date.substring(virgule[0] + 1, virgule[1]) != "A") {
        Serial.println("[GPS] No Fix.");
        return loc;
    }

    loc.fix = true;
    loc.latitude  = convertNMEAToDecimal(date.substring(virgule[1] + 1, virgule[2]).toFloat());
    if (date.substring(virgule[2] + 1, virgule[3]) == "S") loc.latitude  = -loc.latitude;

    loc.longitude = convertNMEAToDecimal(date.substring(virgule[3] + 1, virgule[4]).toFloat());
    if (date.substring(virgule[4] + 1, virgule[5]) == "W") loc.longitude = -loc.longitude;

    loc.speed = date.substring(virgule[5] + 1, virgule[6]).toFloat() * 1.852;
    return loc;
}

 
// ─── Baterie via AT+CBC ───────────────────────────────────────
// Raspuns: +CBC: <bcs>,<bcl>,<voltage>
//   bcs  : 0=nepornit, 1=incarcare, 2=plin
//   bcl  : 0-100 (procente)
//   voltage: mV
int getBatteryPercent() {
    sim808.println("AT+CBC");
    unsigned long start = millis();
    String raspuns = "";
 
    while (millis() - start < 3000) {
        if (sim808.available()) {
            char c = sim808.read();
            raspuns += c;
            if (raspuns.indexOf("OK") != -1) break;
        }
        yield();
    }
 
    // Cauta "+CBC: "
    int idx = raspuns.indexOf("+CBC:");
    if (idx == -1) {
        Serial.println("[BAT] Raspuns invalid AT+CBC");
        return -1;
    }
 
    String data = raspuns.substring(idx + 5);
    data.trim();
 
    // Format: "0,85,4100"
    int comma1 = data.indexOf(',');
    int comma2 = data.indexOf(',', comma1 + 1);
    if (comma1 == -1 || comma2 == -1) return -1;
 
    // int bcs     = data.substring(0, comma1).toInt();  // status incarcare
    int bcl        = data.substring(comma1 + 1, comma2).toInt(); // procente
    int voltage_mv = data.substring(comma2 + 1).toInt();
 
    Serial.println("[BAT] " + String(bcl) + "% (" + String(voltage_mv) + " mV)");
    return bcl;
}


// ============================================================
// NIVEL 5 - Comenzi de nivel inalt
// ============================================================

void setupSIM() {
    pinMode(SIM_PWR, OUTPUT);
    digitalWrite(SIM_PWR, HIGH); delay(1500);
    digitalWrite(SIM_PWR, LOW);

    sim808.begin(9600);
    delay(3000);

    Serial.println("\n--- Sincronizare Autobauding ---");
    while (!sendSIM808Command("AT", 2000)) delay(500);

    Serial.println("\n--- Pornire GPS ---");
    sendSIM808Command("AT+CGPSPWR=1");

    Serial.println("\n--- Configurare GPRS ---");
    sendSIM808Command("AT+SAPBR=0,1", 2000); delay(500);
    sendSIM808Command("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
    sendSIM808Command("AT+SAPBR=3,1,\"APN\",\"net\"");
    delay(500);

    if (sendSIM808Command("AT+SAPBR=1,1", 10000)) {
        Serial.println("\n[SUCCESS] GPRS activ!");
        sendSIM808Command("AT+SAPBR=2,1", 2000);
    } else {
        Serial.println("\n[FAIL] Nu s-a putut activa GPRS.");
    }
}

void updateSIM(SystemState& state) {
    while (sim808.available()) Serial.write(sim808.read());
}

void sendBark(int intensity) { 
    sendHTTPData("barking", String(intensity)); 
}

void sendGPS(GPSLocation loc) {
    if (!loc.fix) {
        Serial.println("[GPS] No Fix - nu trimitem.");
        return;
    }

    if(sendHTTPLocation(loc.latitude, loc.longitude, 0.0)) {
        Serial.println("[GPS] Pozitie trimisa!");
    } else {
        Serial.println("[GPS] Eroare trimitere.");
    }
}

// ─── SMS via GSM ─────────────────────────────────────────────
// Functioneaza chiar daca GPRS / Adafruit e down
// SIM808 trebuie sa aiba semnal GSM (nu neaparat date)
bool sendSMS(const String& number, const String& message) {
    Serial.println("\n--- [SMS] Trimitere catre " + number + " ---");
    Serial.println("[SMS] Mesaj: " + message);
 
    // Mod text (nu PDU)
    if (!sendSIM808Command("AT+CMGF=1")) {
        Serial.println("[SMS] Eroare CMGF");
        return false;
    }
 
    // Destinatar
    sim808.println("AT+CMGS=\"" + number + "\"");
    delay(1000);
 
    // Asteaptam promptul '>'
    unsigned long start = millis();
    String prompt = "";
    while (millis() - start < 3000) {
        if (sim808.available()) {
            char c = sim808.read();
            prompt += c;
            if (prompt.indexOf('>') != -1) break;
        }
        yield();
    }
 
    if (prompt.indexOf('>') == -1) {
        Serial.println("[SMS] Prompt '>' negasit");
        return false;
    }
 
    // Trimitem mesajul + Ctrl+Z (char 26) pentru a confirma
    sim808.print(message);
    delay(500);
    sim808.write(26);   // Ctrl+Z = send
 
    // Asteptam confirmare +CMGS sau ERROR
    start = millis();
    String raspuns = "";
    while (millis() - start < 10000) {
        if (sim808.available()) {
            char c = sim808.read();
            raspuns += c;
            Serial.write(c);
            if (raspuns.indexOf("+CMGS:") != -1) {
                Serial.println("\n[SMS] Trimis cu succes!");
                return true;
            }
            if (raspuns.indexOf("ERROR") != -1) {
                Serial.println("\n[SMS] Eroare la trimitere");
                return false;
            }
        }
        yield();
    }
 
    Serial.println("[SMS] Timeout");
    return false;
}