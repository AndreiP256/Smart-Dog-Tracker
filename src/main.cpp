#include <Arduino.h>
#include <SoftwareSerial.h>
#include <sim808.h>

#define BUZZER D1
#define LED    D0
#define MIC    A0

const unsigned long INTERVAL_CITIRE = 3000;

bool gsmReady = false;
unsigned long ultimulTimpCitire = 0;
SystemState state = {false, 44.4355, 26.0461, 100.0, false};

// ---------------------------------------------------------------------------

void pollCommands() {
    String comandaBuzzer = getHTTPData("buzzer");

    if (comandaBuzzer == "ON") {
        Serial.println("[ACTION] Pornesc buzzerul de la distanță!");
        state.forceBuzzer = true;
    } else if (comandaBuzzer == "OFF") {
        Serial.println("[ACTION] Opresc buzzerul.");
        state.forceBuzzer = false;
    }
}

void updateBuzzer() {
    if (state.forceBuzzer) {
        tone(BUZZER, 1500);
    } else {
        noTone(BUZZER);
    }
}

void updateGPS() {
    GPSLocation loc = getLocation();
    Serial.print("Latitudine: ");  Serial.println(loc.latitude, 6);
    Serial.print("Longitudine: "); Serial.println(loc.longitude, 6);
    sendGPS(loc);
}

// ---------------------------------------------------------------------------

void setup() {
    pinMode(LED,    OUTPUT);
    pinMode(MIC,    INPUT);
    pinMode(BUZZER, OUTPUT);

    Serial.begin(115200);
    delay(2000);

    setupSIM();
    gsmReady = true;
    tone(BUZZER, 2000, 200);
}

void loop() {
    updateSIM(state);

    if (!gsmReady) return;

    unsigned long timpCurent = millis();
    if (timpCurent - ultimulTimpCitire >= INTERVAL_CITIRE) {
        ultimulTimpCitire = timpCurent;
        pollCommands();
        updateGPS();
    }

    updateBuzzer();
}