/*
 * Notion Task List — XIAO ESP32C3 + Waveshare 2.13" Touch e-Ink (SKU 20716)
 *
 * Libraries required (Arduino Library Manager):
 *   - GxEPD2      by ZinggJM
 *   - ArduinoJson by bblanchon
 *
 * Wiring (XIAO ESP32C3 → Waveshare 2.13" Touch):
 *   3.3V → VCC      GND  → GND
 *   D10  → DIN      D8   → CLK      (display SPI)
 *   D1   → CS       D3   → DC
 *   D2   → RST      D4   → BUSY
 *   D5   → TP_SDA   D6   → TP_SCL   (touch I2C)
 *   D0   → TP_INT   D7   → TP_RST
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans7pt7b.h>
#include <Wire.h>
#include <time.h>

// ── Configuration ──────────────────────────────────────────────────────────
#define WIFI_SSID       "Vega Mini"
#define WIFI_PASSWORD   "doomslayer"
#define API_URL         "https://notion-tasklist.vercel.app/api/tasks"
#define UPDATE_URL      "https://notion-tasklist.vercel.app/api/update-task"
#define UTC_OFFSET_SEC  28800   // UTC+8 — adjust for your timezone
#define SLEEP_MINUTES   30
#define MAX_VISIBLE     5
// ───────────────────────────────────────────────────────────────────────────

// Display pins
#define EPD_CS    D1
#define EPD_DC    D3
#define EPD_RST   D2
#define EPD_BUSY  D4

// Touch pins (GT1151 capacitive controller)
#define TP_SDA    D5   // GPIO7
#define TP_SCL    D6   // GPIO21
#define TP_INT    D0   // GPIO2 — also used for deep sleep wakeup
#define TP_RST    D7   // GPIO20

// GT1151 registers
#define GT_ADDR     0x14
#define GT_REG_STA  0x814E
#define GT_REG_TP   0x8150

GxEPD2_BW<GxEPD2_213_GDEM0213B74, GxEPD2_213_GDEM0213B74::HEIGHT>
  display(GxEPD2_213_GDEM0213B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

struct Task {
  char id[37];       // Notion page UUID
  char title[48];
  int  status;       // 0 = To Do, 1 = Doing, 2 = Done
  char deadline[12]; // "YYYY-MM-DD" or ""
};

RTC_DATA_ATTR static Task rtcTasks[10];
RTC_DATA_ATTR static int  rtcCount = 0;

// ── Main ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool touchWake = (wake == ESP_SLEEP_WAKEUP_GPIO);

  // Skip hardware display reset on touch wake to enable faster partial refresh
  display.init(115200, !touchWake, 50, false);
  display.setRotation(1);

  if (touchWake) {
    handleTouch();
  } else {
    handleTimerWake();
  }

  display.hibernate();
  setWakeup();
  esp_deep_sleep_start();
}

void loop() {}

// ── Wake handlers ──────────────────────────────────────────────────────────

void handleTimerWake() {
  if (!connectWiFi()) {
    showError("WiFi failed");
    return;
  }
  configTime(UTC_OFFSET_SEC, 0, "pool.ntp.org", "time.cloudflare.com");
  struct tm timeinfo;
  bool synced = getLocalTime(&timeinfo, 6000);

  int total = 0;
  int count = fetchTasks(rtcTasks, 10, &total);
  if (count >= 0) {
    rtcCount = count;
    renderDisplay(rtcTasks, count, total, synced ? &timeinfo : nullptr, true);
  } else {
    showError("API error");
  }
  WiFi.disconnect(true);
}

void handleTouch() {
  if (!gt_init()) return;

  delay(50); // debounce
  uint16_t tx = 0, ty = 0;
  if (gt_read_touch(&tx, &ty) <= 0 || rtcCount == 0) return;

  delay(20); // wait for INT pin to clear

  int idx = mapTouchToTask(tx, ty);
  if (idx < 0 || idx >= rtcCount) return;

  // Cycle status: To Do → Doing → Done → To Do
  rtcTasks[idx].status = (rtcTasks[idx].status + 1) % 3;

  // Update display immediately for instant feedback
  renderDisplay(rtcTasks, rtcCount, rtcCount, nullptr, false);

  // Sync new status to Notion
  if (connectWiFi()) {
    updateTaskStatus(rtcTasks[idx].id, rtcTasks[idx].status);
    WiFi.disconnect(true);
  }
}

// Maps touch coordinates to a task row index.
// Returns -1 if the touch was outside the task area.
// If tap positions feel off, adjust startY to calibrate.
int mapTouchToTask(uint16_t tx, uint16_t ty) {
  const int startY = 19; // top of first task row in display pixels
  const int rowH   = 18;
  if (ty < startY) return -1;
  int row = (ty - startY) / rowH;
  if (row >= MAX_VISIBLE) return -1;
  return row;
}

// ── Sleep ──────────────────────────────────────────────────────────────────

void setWakeup() {
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60ULL * 1000000ULL);
  // Wake on touch: TP_INT (GPIO2 = D0) goes LOW when screen is touched
  pinMode(TP_INT, INPUT_PULLUP);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << 2, ESP_GPIO_WAKEUP_GPIO_LOW);
}

// ── WiFi ───────────────────────────────────────────────────────────────────

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(500);
  }
  return false;
}

// ── API ────────────────────────────────────────────────────────────────────

int fetchTasks(Task* buf, int maxTasks, int* total) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, API_URL);
  http.setTimeout(10000);

  int code = http.GET();
  if (code != 200) { http.end(); return -1; }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return -1;

  JsonArray arr = doc.as<JsonArray>();
  *total = arr.size();
  int count = 0;

  for (JsonObject task : arr) {
    if (count >= maxTasks) break;
    strlcpy(buf[count].id,       task["id"]       | "",         sizeof(buf[count].id));
    strlcpy(buf[count].title,    task["title"]    | "Untitled", sizeof(buf[count].title));
    buf[count].status = task["status"] | 0;
    strlcpy(buf[count].deadline, task["deadline"] | "",         sizeof(buf[count].deadline));
    count++;
  }
  return count;
}

void updateTaskStatus(const char* id, int status) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, UPDATE_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  String body = "{\"id\":\"" + String(id) + "\",\"status\":" + String(status) + "}";
  http.sendRequest("PATCH", body);
  http.end();
}

// ── GT1151 touch controller ────────────────────────────────────────────────

void gt_write_reg(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(GT_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(val);
  Wire.endTransmission();
}

void gt_read_reg(uint16_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(GT_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)GT_ADDR, len);
  for (int i = 0; i < len; i++)
    buf[i] = Wire.available() ? Wire.read() : 0;
}

bool gt_init() {
  // Reset sequence — holding INT low during reset sets I2C address to 0x14
  pinMode(TP_INT, OUTPUT); digitalWrite(TP_INT, LOW);
  pinMode(TP_RST, OUTPUT); digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(10);
  pinMode(TP_INT, INPUT);
  delay(50);

  Wire.begin(TP_SDA, TP_SCL);
  Wire.beginTransmission(GT_ADDR);
  return Wire.endTransmission() == 0;
}

// Returns number of touch points. Fills *x, *y with first point coordinates.
// Touch coordinates match display landscape orientation (250×122).
// If taps feel misaligned, swap x/y or adjust mapTouchToTask().
int gt_read_touch(uint16_t* x, uint16_t* y) {
  uint8_t status = 0;
  gt_read_reg(GT_REG_STA, &status, 1);
  if (!(status & 0x80)) return 0; // buffer not ready

  int count = status & 0x0F;
  if (count > 0) {
    uint8_t pt[8];
    gt_read_reg(GT_REG_TP, pt, 8);
    *x = pt[1] | ((uint16_t)pt[2] << 8);
    *y = pt[3] | ((uint16_t)pt[4] << 8);
  }
  gt_write_reg(GT_REG_STA, 0); // clear buffer, releases INT pin
  return count;
}

// ── Rendering ──────────────────────────────────────────────────────────────

void renderDisplay(Task* tasks, int count, int total, struct tm* t, bool full) {
  if (full) {
    display.setFullWindow();
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
  }
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader(t);
    drawTasks(tasks, count, total);
  } while (display.nextPage());
}

void drawHeader(struct tm* t) {
  display.setFont(&FreeSans9pt7b);
  display.setTextColor(GxEPD_BLACK);

  if (t) {
    char dateBuf[20];
    strftime(dateBuf, sizeof(dateBuf), "%a, %b %d", t);
    display.setCursor(4, 13);
    display.print(dateBuf);

    char timeBuf[8];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", t);
    int16_t tx, ty; uint16_t tw, th;
    display.getTextBounds(timeBuf, 0, 0, &tx, &ty, &tw, &th);
    display.setCursor(249 - (int)tw - 4, 13);
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
  const int dlWidth   = 46;
  const int titleMaxX = 250 - dlWidth - 4;
  int visible         = min(count, MAX_VISIBLE);

  for (int i = 0; i < visible; i++) {
    int baseY = startY + i * rowH;

    // Checkbox: [ ] To Do  [–] Doing  [✓] Done
    display.drawRect(cbX, baseY - cbSize + 2, cbSize, cbSize, GxEPD_BLACK);
    int bx = cbX + 2, by = baseY - cbSize + 4;
    if (tasks[i].status == 1) {
      display.drawFastHLine(bx, by + 3, 7, GxEPD_BLACK);
    } else if (tasks[i].status == 2) {
      display.drawLine(bx,     by + 3, bx + 2, by + 5, GxEPD_BLACK);
      display.drawLine(bx + 2, by + 5, bx + 7, by,     GxEPD_BLACK);
    }

    // Title — truncate with ellipsis to fit before deadline column
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    String title = tasks[i].title;
    while (title.length() > 0) {
      int16_t tx, ty; uint16_t tw, th;
      String candidate = (title.length() < strlen(tasks[i].title))
                         ? title + "\xE2\x80\xA6" : title;
      display.getTextBounds(candidate.c_str(), titleX, baseY, &tx, &ty, &tw, &th);
      if (titleX + (int)tw <= titleMaxX) {
        display.setCursor(titleX, baseY);
        display.print(candidate);
        break;
      }
      title.remove(title.length() - 1);
    }

    // Strikethrough for Done tasks
    if (tasks[i].status == 2)
      display.drawFastHLine(titleX, baseY - 4, titleMaxX - titleX, GxEPD_BLACK);

    // Deadline (right-aligned, small font)
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

  if (total > MAX_VISIBLE) {
    display.setFont(&FreeSans7pt7b);
    char moreBuf[12];
    snprintf(moreBuf, sizeof(moreBuf), "+%d more", total - MAX_VISIBLE);
    display.setCursor(4, 119);
    display.print(moreBuf);
  }

  if (count == 0) {
    display.setFont(&FreeSans9pt7b);
    display.setCursor(4, 50);
    display.print("No tasks today!");
  }
}

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
    display.printf("Retry in %d min", SLEEP_MINUTES);
  } while (display.nextPage());
}

void formatDeadline(const char* dateStr, char* out, size_t outSize) {
  static const char* months[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
  };
  int month = 0, day = 0;
  sscanf(dateStr, "%*d-%d-%d", &month, &day);
  if (month >= 1 && month <= 12)
    snprintf(out, outSize, "%s %d", months[month - 1], day);
  else
    strlcpy(out, dateStr, outSize);
}
