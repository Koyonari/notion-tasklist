/*
 * WiFi + API test sketch — no display needed
 * Open Serial Monitor at 115200 baud to see output
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define WIFI_SSID      "Vega Mini"
#define WIFI_PASSWORD  "doomslayer"
#define API_URL        "https://notion-tasklist.vercel.app/api/tasks"

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- WiFi + API Test ---");

  // Scan for networks first
  WiFi.mode(WIFI_STA);
  Serial.println("Scanning networks...");
  int found = WiFi.scanNetworks();
  if (found == 0) {
    Serial.println("No networks found at all — antenna issue?");
  } else {
    for (int i = 0; i < found; i++) {
      Serial.printf("  %d: %s (%d dBm) %s\n", i+1,
        WiFi.SSID(i).c_str(), WiFi.RSSI(i),
        WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
    }
  }
  WiFi.scanDelete();

  // Connect WiFi
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("\nWiFi FAILED — status code: %d\n", WiFi.status());
    Serial.println("Codes: 1=SSID not found, 4=wrong password, 6=timed out");
    return;
  }
  Serial.println("\nWiFi OK — IP: " + WiFi.localIP().toString());

  // Fetch tasks
  Serial.println("Fetching tasks from API...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, API_URL);
  http.setTimeout(10000);

  int code = http.GET();
  Serial.printf("HTTP status: %d\n", code);

  if (code == 200) {
    String body = http.getString();
    Serial.println("Response: " + body);

    JsonDocument doc;
    deserializeJson(doc, body);
    int count = doc.as<JsonArray>().size();
    Serial.printf("Parsed %d task(s):\n", count);

    for (JsonObject task : doc.as<JsonArray>()) {
      const char* title    = task["title"] | "Untitled";
      bool done            = task["done"] | false;
      const char* deadline = task["deadline"] | "no deadline";
      Serial.printf("  [%s] %s — %s\n", done ? "x" : " ", title, deadline);
    }
  } else {
    Serial.println("API error — check your API_URL and that Vercel is deployed");
  }

  http.end();
  Serial.println("--- Done ---");
}

void loop() {}
