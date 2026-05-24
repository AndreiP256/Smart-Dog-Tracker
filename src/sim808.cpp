#include "sim808.h"
#include <SoftwareSerial.h>

SoftwareSerial sim808(SIM_RX, SIM_TX);

// ============================================================
// NIVEL 1 – Primitiva AT
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
        yield(); webTick();
    }
    rlog("[TIMEOUT] Comanda: " + cmd);
    return false;
}

// ============================================================
// NIVEL 2 – Nucleu HTTP
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
        yield(); webTick();
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
        yield(); webTick();
    }

    String valoare = "";
    if (dataLength > 0) {
        sim808.println("AT+HTTPREAD");
        unsigned long readStart = millis();
        String jsonRaw = "";

        while (millis() - readStart < 4000) {
            if (sim808.available()) jsonRaw += (char)sim808.read();
            yield(); webTick();
        }

        // Parsare simpla fara librarie JSON
        // Adafruit poate returna "value":"x" sau "value": "x" (cu spatiu)
        int idx = jsonRaw.indexOf("\"value\":");
        if (idx != -1) {
            int cursor = idx + 8;
            while (cursor < (int)jsonRaw.length() && (jsonRaw[cursor] == ' ' || jsonRaw[cursor] == '"'))
                cursor++;
            int end = jsonRaw.indexOf("\"", cursor);
            if (end != -1) valoare = jsonRaw.substring(cursor, end);
            valoare.trim();
        }
        rlog("[HTTP RAW] " + jsonRaw.substring(0, min((int)jsonRaw.length(), 120)));
    }

    sendSIM808Command("AT+HTTPTERM");
    rlog("[HTTP GET] " + url + " → " + valoare);
    return valoare;
}

// ============================================================
// NIVEL 3 – API Adafruit
// ============================================================

static String adafruitUrl(const String& feedName, const String& suffix = "/data") {
    return "http://io.adafruit.com/api/v2/" + String(USER_ADAFRUIT) + "/feeds/" + feedName + suffix;
}

bool sendHTTPData(const String& feedName, const String& value) {
    rlog("\n--- [HTTP POST] feed: " + feedName + " = " + value + " ---");
    String payload = "{\"value\":\"" + value + "\"}";
    return httpPost(adafruitUrl(feedName), payload);
}

String getHTTPData(const String& feedName) {
    rlog("\n--- [HTTP GET] feed: " + feedName + " ---");
    return httpGet(adafruitUrl(feedName, "/data/last"));
}

bool sendHTTPLocation(float lat, float lon, float ele) {
    rlog("\n--- [HTTP] Trimitere locatie GPS ---");
    String payload = "{\"value\":\"0\","
                     "\"lat\":"  + String(lat, 6) + ","
                     "\"lon\":"  + String(lon, 6) + ","
                     "\"ele\":"  + String(ele, 1) + "}";
    return httpPost(adafruitUrl("location"), payload);
}

// ============================================================
// NIVEL 4 – GPS
// ============================================================

static float convertNMEAToDecimal(float raw) {
    int   degrees = (int)(raw / 100);
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
        yield(); webTick();
    }

    int idxInf = raspuns.indexOf("+CGPSINF: 32,");
    if (idxInf == -1) { rlog("[GPS] Format invalid."); return loc; }

    String date = raspuns.substring(idxInf + 13);
    date.trim();

    int virgule[15];
    int count = 0;
    for (int i = 0; i < (int)date.length() && count < 15; i++)
        if (date[i] == ',') virgule[count++] = i;

    if (count < 6) { rlog("[GPS] Date incomplete."); return loc; }

    if (date.substring(virgule[0] + 1, virgule[1]) != "A") {
        rlog("[GPS] No Fix.");
        return loc;
    }

    loc.fix = true;
    loc.latitude  = convertNMEAToDecimal(date.substring(virgule[1] + 1, virgule[2]).toFloat());
    if (date.substring(virgule[2] + 1, virgule[3]) == "S") loc.latitude = -loc.latitude;

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
        yield(); webTick();
    }

    // Cauta "+CBC: "
    int idx = raspuns.indexOf("+CBC:");
    if (idx == -1) {
        rlog("[BAT] Raspuns invalid AT+CBC");
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

    rlog("[BAT] " + String(bcl) + "% (" + String(voltage_mv) + " mV)");
    return bcl;
}

// ============================================================
// NIVEL 5 – Comenzi de nivel inalt
// ============================================================

// Asteapta un mesaj specific de la SIM808, logeaza TOT ce vine
// ca sa putem vedea exact ce trimite modulul
static bool waitForUnsolicited(const String& token, unsigned long timeout = 20000) {
    unsigned long start = millis();
    String buf = "";
    String lineBuf = "";

    rlog("[SIM] Astept '" + token + "' (max " + String(timeout/1000) + "s)...");

    while (millis() - start < timeout) {
        if (sim808.available()) {
            char c = sim808.read();
            buf     += c;
            lineBuf += c;

            // Logeaza linie cu linie in web log
            if (c == '\n') {
                String trimmed = lineBuf;
                trimmed.trim();
                if (trimmed.length() > 0) rlog("[SIM<<] " + trimmed);
                lineBuf = "";
            }

            if (buf.indexOf("OVER-VOLTAGE POWER DOWN") != -1) {
                rlog("[EROARE CRITICA] OVER-VOLTAGE POWER DOWN!");
                rlog("[HINT] BAT+ e conectat la 5V? Trebuie OUT+ TP4056 (3.7-4.2V)!");
                return false;
            }
            if (buf.indexOf("UNDER-VOLTAGE POWER DOWN") != -1) {
                rlog("[EROARE] UNDER-VOLTAGE POWER DOWN!");
                rlog("[HINT] Bateria e prea descarcata sau nu da curent suficient (SIM808 cere ~2A).");
                return false;
            }
            if (buf.indexOf("UNDER-VOLTAGE WARNING") != -1) {
                rlog("[WARN] Under-voltage warning – tensiune scazuta, posibil instabil.");
            }
            if (buf.indexOf(token) != -1) {
                rlog("[SIM] Gasit '" + token + "' dupa " + String(millis()-start) + " ms");
                return true;
            }
            if (buf.length() > 256) buf = buf.substring(buf.length() - 256);
        }
        yield(); webTick();
    }

    // Timeout – logam ce am primit ca sa stim ce a trimis modulul
    String last = buf; last.trim();
    rlog("[SIM] Timeout " + String(timeout/1000) + "s – '" + token + "' nu a venit.");
    if (last.length() > 0)
        rlog("[SIM] Ultimele date: '" + last + "'");
    else
        rlog("[SIM] Nimic primit de la modul – verifica conexiunea RX/TX si alimentarea.");
    return false;
}

// Returneaza true daca init-ul a reusit complet
bool setupSIM() {
    // ── Exact pattern-ul original care functiona ──────────────
    // Ordinea conteaza: puls INAINTE de sim808.begin()
    pinMode(SIM_PWR, OUTPUT);
    digitalWrite(SIM_PWR, HIGH); delay(1500);
    digitalWrite(SIM_PWR, LOW);

    sim808.begin(9600);
    delay(3000);   // da timp modulului sa booteze si sa trimita RDY

    // Citim tot ce a venit in timpul delay-ului (RDY, +CFUN, +CPIN)
    // si logam ca sa vedem starea modulului
    rlog("\n--- Sincronizare AT ---");
    String bootMsg = "";
    unsigned long drainStart = millis();
    while (millis() - drainStart < 500) {
        if (sim808.available()) {
            char c = sim808.read();
            bootMsg += c;
        }
        yield(); webTick();
    }
    if (bootMsg.length() > 0) {
        bootMsg.trim();
        rlog("[SIM<<] " + bootMsg);
        if (bootMsg.indexOf("OVER-VOLTAGE") != -1) {
            rlog("[EROARE] OVER-VOLTAGE! BAT+ conectat la 5V? Trebuie OUT+ TP4056.");
            return false;
        }
        if (bootMsg.indexOf("UNDER-VOLTAGE") != -1) {
            rlog("[EROARE] UNDER-VOLTAGE! Bateria prea slaba sau curent insuficient.");
            return false;
        }
    }

    // Poll AT pana raspunde – exact ca in original
    int retries = 0;
    while (!sendSIM808Command("AT", 2000)) {
        if (++retries > 20) {
            rlog("[SIM] Nu raspunde la AT dupa 20 incercari. Abort.");
            return false;
        }
        rlog("[SIM] Retry AT " + String(retries) + "/20...");
        delay(500); webTick();
    }
    rlog("[SIM] AT OK!");
    sendSIM808Command("ATE0");   // echo off

    // GPS
    rlog("\n--- Pornire GPS ---");
    if (sendSIM808Command("AT+CGPSPWR=1"))
        rlog("[GPS] Pornit.");
    else
        rlog("[GPS] Eroare pornire – continuam oricum.");

    // ── Asteapta inregistrare GSM (max 30s) ──────────────────
    rlog("\n--- Verificare semnal GSM ---");
    bool registered = false;
    for (int i = 0; i < 15; i++) {
        sim808.println("AT+CREG?");
        unsigned long t = millis();
        String r = "";
        while (millis() - t < 2000) {
            if (sim808.available()) r += (char)sim808.read();
            yield(); webTick();
        }
        r.trim();
        rlog("[GSM] CREG: " + r);
        // 0,1 = inregistrat local  |  0,5 = roaming
        if (r.indexOf(",1") != -1 || r.indexOf(",5") != -1) {
            registered = true;
            rlog("[GSM] Inregistrat in retea dupa " + String(i * 2) + "s.");
            break;
        }
        delay(500); webTick();
    }
    if (!registered) {
        rlog("[FAIL] Nu e inregistrat in retea GSM.");
        rlog("[HINT] SIM valid? Are semnal? PIN dezactivat?");
        return false;
    }

    // ── Configurare GPRS ─────────────────────────────────────
    rlog("\n--- Configurare GPRS ---");

    // Inchide bearer-ul daca era deschis, ignora eroarea
    sendSIM808Command("AT+SAPBR=0,1", 5000);
    delay(1000);

    bool gprsOk = false;
    for (int attempt = 1; attempt <= 3 && !gprsOk; attempt++) {
        rlog("[GPRS] Tentativa " + String(attempt) + "/3...");

        if (!sendSIM808Command("AT+SAPBR=3,1,\"Contype\",\"GPRS\""))
            rlog("[GPRS] WARN: Contype failed");
        if (!sendSIM808Command("AT+SAPBR=3,1,\"APN\",\"net\""))
            rlog("[GPRS] WARN: APN failed");

        delay(500);

        rlog("[GPRS] Deschid bearer (AT+SAPBR=1,1, max 30s)...");
        if (sendSIM808Command("AT+SAPBR=1,1", 30000)) {
            gprsOk = true;
        } else {
            rlog("[GPRS] Tentativa " + String(attempt) + " esuata, astept 3s...");
            sendSIM808Command("AT+SAPBR=0,1", 5000);
            delay(3000);
        }
    }

    if (!gprsOk) {
        rlog("[FAIL] GPRS esuat dupa 3 tentative. Verifica SIM si APN.");
        return false;
    }

    rlog("[SUCCESS] GPRS activ!");
    sendSIM808Command("AT+SAPBR=2,1", 2000);   // log IP alocat
    rlog("[GPRS] Stabilizare 2s...");
    delay(2000);
    return true;
}

void updateSIM(SystemState& state) {
    while (sim808.available()) Serial.write(sim808.read());
}

void sendBark(int intensity) {
    sendHTTPData("barking", String(intensity));
}

void sendGPS(GPSLocation loc) {
    if (!loc.fix) {
        rlog("[GPS] No Fix - nu trimitem.");
        return;
    }
    if (sendHTTPLocation(loc.latitude, loc.longitude, 0.0)) {
        rlog("[GPS] Pozitie trimisa!");
    } else {
        rlog("[GPS] Eroare trimitere.");
    }
}

// ─── SMS via GSM ─────────────────────────────────────────────
// Functioneaza chiar daca GPRS / Adafruit e down
// SIM808 trebuie sa aiba semnal GSM (nu neaparat date)
bool sendSMS(const String& number, const String& message) {
    rlog("\n--- [SMS] Trimitere catre " + number + " ---");
    rlog("[SMS] Mesaj: " + message);

    // Mod text (nu PDU)
    if (!sendSIM808Command("AT+CMGF=1")) {
        rlog("[SMS] Eroare CMGF");
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
        yield(); webTick();
    }

    if (prompt.indexOf('>') == -1) {
        rlog("[SMS] Prompt '>' negasit");
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
                rlog("\n[SMS] Trimis cu succes!");
                return true;
            }
            if (raspuns.indexOf("ERROR") != -1) {
                rlog("\n[SMS] Eroare la trimitere");
                return false;
            }
        }
        yield(); webTick();
    }

    rlog("[SMS] Timeout");
    return false;
}