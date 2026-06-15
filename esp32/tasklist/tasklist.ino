/*
 * Notion Task List — XIAO ESP32C3 + Waveshare 2.13" e-ink
 *
 * Libraries required (install via Arduino Library Manager):
 *   - GxEPD2        by ZinggJM
 *   - ArduinoJson   by bblanchon
 *
 * Wiring (XIAO ESP32C3 → Waveshare 2.13"):
 *   3.3V → VCC     GND → GND
 *   D10  → DIN     D8  → CLK
 *   D1   → CS      D3  → DC
 *   D2   → RST     D4  → BUSY
 *
 * Display version:
 *   V2/V3/V4 → use GxEPD2_213_GDEM0213B74 (below)
 *   V1       → replace with GxEPD2_213_B72
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans7pt7b.h>
#include <time.h>

// ── Configuration ──────────────────────────────────────────────────────────
#define WIFI_SSID       "Vega Mini"
#define WIFI_PASSWORD   "doomslayer"
#define API_URL         "https://notion-tasklist.vercel.app/api/tasks"

// UTC offset for your timezone in seconds (e.g. UTC+8 = 8*3600 = 28800)
#define UTC_OFFSET_SEC  28800

// How often to refresh in minutes
#define SLEEP_MINUTES   30

// Max tasks the display can show (5 fits cleanly, +1 more indicator if extra)
#define MAX_VISIBLE     5
// ───────────────────────────────────────────────────────────────────────────

// Display pins
#define EPD_CS    D1
#define EPD_DC    D3
#define EPD_RST   D2
#define EPD_BUSY  D4

GxEPD2_BW<GxEPD2_213_GDEM0213B74, GxEPD2_213_GDEM0213B74::HEIGHT>
  display(GxEPD2_213_GDEM0213B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

struct Task {
  char title[48];
  bool done;
  char deadline[12]; // "YYYY-MM-DD" or ""
};

// ── Main ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  display.init(115200, true, 50, false);
  display.setRotation(1); // landscape

  if (!connectWiFi()) {
    showError("WiFi failed");
    goToSleep();
  }

  configTime(UTC_OFFSET_SEC, 0, "pool.ntp.org", "time.cloudflare.com");
  // Wait for time sync (up to 6s)
  struct tm timeinfo;
  bool timeSynced = getLocalTime(&timeinfo, 6000);

  Task tasks[10];
  int total = 0;
  int count = fetchTasks(tasks, 10, &total);

  if (count < 0) {
    showError("API error");
  } else {
    renderDisplay(tasks, count, total, timeSynced ? &timeinfo : nullptr);
  }

  display.hibernate();
  WiFi.disconnect(true);
  goToSleep();
}

void loop() {}

// ── WiFi ───────────────────────────────────────────────────────────────────

bool connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(500);
  }
  return false;
}

// ── API fetch ──────────────────────────────────────────────────────────────

// Returns number of tasks stored in buf (up to maxTasks).
// Sets *total to full count from API. Returns -1 on error.
int fetchTasks(Task* buf, int maxTasks, int* total) {
  WiFiClientSecure client;
  client.setInsecure(); // personal use — skip cert verification
  HTTPClient http;
  http.begin(client, API_URL);
  http.setTimeout(10000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP error: %d\n", code);
    http.end();
    return -1;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return -1;
  }

  JsonArray arr = doc.as<JsonArray>();
  *total = arr.size();
  int count = 0;

  for (JsonObject task : arr) {
    if (count >= maxTasks) break;
    strlcpy(buf[count].title, task["title"] | "Untitled", sizeof(buf[count].title));
    buf[count].done = task["done"] | false;
    const char* dl = task["deadline"] | "";
    strlcpy(buf[count].deadline, dl, sizeof(buf[count].deadline));
    count++;
  }
  return count;
}

// ── Rendering ──────────────────────────────────────────────────────────────

void renderDisplay(Task* tasks, int count, int total, struct tm* t) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader(t);
    drawTasks(tasks, count, total);
  } while (display.nextPage());
}

void drawHeader(struct tm* t) {
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeSans9pt7b);

  if (t) {
    char dateBuf[20];
    strftime(dateBuf, sizeof(dateBuf), "%a, %b %d", t);
    display.setCursor(4, 13);
    display.print(dateBuf);

    char timeBuf[8];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", t);
    int16_t tx, ty; uint16_t tw, th;
    display.getTextBounds(timeBuf, 0, 0, &tx, &ty, &tw, &th);
    display.setCursor(249 - tw - 4, 13);
    display.print(timeBuf);
  } else {
    display.setCursor(4, 13);
    display.print("Task List");
  }

  display.drawFastHLine(0, 17, 250, GxEPD_BLACK);
}

void drawTasks(Task* tasks, int count, int total) {
  const int startY    = 32;
  const int rowH      = 18;
  const int cbX       = 4;
  const int cbSize    = 11;
  const int titleX    = cbX + cbSize + 5;
  const int dlWidth   = 46; // pixels reserved for deadline on right
  const int titleMaxX = 250 - dlWidth - 4;
  int visible         = min(count, MAX_VISIBLE);

  for (int i = 0; i < visible; i++) {
    int baseY = startY + i * rowH;

    // Checkbox outline
    display.drawRect(cbX, baseY - cbSize + 2, cbSize, cbSize, GxEPD_BLACK);

    if (tasks[i].done) {
      // Checkmark inside box
      int bx = cbX + 2, by = baseY - cbSize + 4;
      display.drawLine(bx,     by + 3, bx + 2, by + 5, GxEPD_BLACK);
      display.drawLine(bx + 2, by + 5, bx + 7, by,     GxEPD_BLACK);
    }

    // Title — truncate to fit before deadline column
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);

    String title = tasks[i].title;
    // Trim until it fits, appending ellipsis
    while (title.length() > 0) {
      int16_t tx, ty; uint16_t tw, th;
      String candidate = (title.length() < (size_t)strlen(tasks[i].title))
                         ? title + "\xE2\x80\xA6" : title; // UTF-8 ellipsis
      display.getTextBounds(candidate.c_str(), titleX, baseY, &tx, &ty, &tw, &th);
      if (titleX + (int)tw <= titleMaxX) {
        display.setCursor(titleX, baseY);
        display.print(candidate);
        break;
      }
      title.remove(title.length() - 1);
    }

    // Strikethrough for completed tasks
    if (tasks[i].done) {
      display.drawFastHLine(titleX, baseY - 4, titleMaxX - titleX, GxEPD_BLACK);
    }

    // Deadline
    if (strlen(tasks[i].deadline) > 0) {
      display.setFont(&FreeSans7pt7b);
      char dlBuf[10];
      formatDeadline(tasks[i].deadline, dlBuf, sizeof(dlBuf));
      int16_t dx, dy; uint16_t dw, dh;
      display.getTextBounds(dlBuf, 0, 0, &dx, &dy, &dw, &dh);
      display.setCursor(249 - (int)dw - 2, baseY);
      display.print(dlBuf);
    }
  }

  // Overflow indicator
  if (total > MAX_VISIBLE) {
    display.setFont(&FreeSans7pt7b);
    display.setTextColor(GxEPD_BLACK);
    char moreBuf[12];
    snprintf(moreBuf, sizeof(moreBuf), "+%d more", total - MAX_VISIBLE);
    display.setCursor(4, 119);
    display.print(moreBuf);
  }

  // Empty state
  if (count == 0) {
    display.setFont(&FreeSans9pt7b);
    display.setCursor(4, 50);
    display.print("No tasks today!");
  }
}

// "2026-06-16" → "Jun 16"
void formatDeadline(const char* dateStr, char* out, size_t outSize) {
  static const char* months[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
  };
  int month = 0, day = 0;
  sscanf(dateStr, "%*d-%d-%d", &month, &day);
  if (month >= 1 && month <= 12) {
    snprintf(out, outSize, "%s %d", months[month - 1], day);
  } else {
    strlcpy(out, dateStr, outSize);
  }
}

// ── Error display ──────────────────────────────────────────────────────────

void showError(const char* msg) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, 30);
    display.print(msg);
    display.setCursor(4, 50);
    display.print("Retrying in");
    display.setCursor(4, 68);
    display.printf("%d min", SLEEP_MINUTES);
  } while (display.nextPage());
}

// ── Sleep ──────────────────────────────────────────────────────────────────

void goToSleep() {
  esp_deep_sleep((uint64_t)SLEEP_MINUTES * 60ULL * 1000000ULL);
}
