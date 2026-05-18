#ifndef SIM808_H
#define SIM808_H

#include <Arduino.h>
#include <utility>

#define CHEIE_ADAFRUIT "aio_KSXE208u6TMca4Ua16a849sKSZNp"
#define USER_ADAFRUIT "AndreiP25"


#define SIM_RX D4
#define SIM_TX D3
#define SIM_PWR D2


struct GPSLocation {
    bool fix;
    float latitude;
    float longitude;
    float speed;
};

struct SystemState {
    bool manualAlertActive;
    float geofenceCenterLat;
    float geofenceCenterLng;
    float geofenceMaxDistance;
    bool forceBuzzer;
};

bool sendSIM808Command(String cmd, unsigned long timeout = 3000);

bool httpPost(const String& url, const String& jsonPayload);
String httpGet(const String& url);

bool sendHTTPData(const String& feedName, const String& value);
String getHTTPData(const String& feedName);
bool sendHTTPLocation(float lat, float lon, float ele);

GPSLocation getLocation();
void sendBark(int intensity);
void sendGPS(GPSLocation loc);
void setupSIM();
void updateSIM(SystemState& state);

#endif