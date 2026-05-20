#include <WiFi.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

DNSServer dnsServer;

Preferences prefs;

char mp8Log[512];

struct ShotData {
    uint32_t shotTime[5];
    uint8_t shotCount;
    bool miss;
};

ShotData mp8Data;

const int startButtonPin = 0;

const int batteryPin = 35; // ADC pin
bool lastStartButtonState = HIGH;
unsigned long lastStartDebounceTime = 0;


const int buttonPin = 19;


const byte DNS_PORT = 53;

#define FW_VERSION "TargetiX OS V2.0.3"
#define FW_BUILD   "Build 243"
#define EEPROM_WELCOME_ADDR 3510

const char* versionURL =
  "https://raw.githubusercontent.com/mykhailopardiniq-oss/TargetiX_OTA_DEV/main/version.txt";

const char* firmwareURL =
  "https://raw.githubusercontent.com/mykhailopardiniq-oss/TargetiX_OTA_DEV/main/firmware.bin";

  const char* changelogURL =
"https://raw.githubusercontent.com/mykhailopardiniq-oss/TargetiX_OTA_DEV/main/changelog.txt";

// ====== Wi-Fi AP ======
const char* ssid = "TargetiX_DEV";
const char* password = "sport_pistol";
WiFiServer server(80);

// ====== IO ======
const int redLedPin = 2;
const int greenLedPin = 4;

// ====== Runtime state ======
bool isProgramRunning = false;
bool stopRequested = false;
bool finishingRed = false;
unsigned long finishStartTime = 0;

unsigned long stageStartTime = 0;
int currentStage = 0;
int totalStages = 0;
unsigned long redDuration = 0;    // ms
unsigned long greenDuration = 0;  // ms
bool stageRed = true;
int activeProgramId = -1;  // ID активной программы
String lastOTAUpdate = "";  // время последнего OTA
String remoteVersion = "";
String changelogText = "";
bool showUpdateModal = false;
String lastSeenVersion = "";
bool updateAvailableFlag = false;
volatile int otaProgress = 0;

// ====== Language ======
enum Language {
  LANG_DE,
  LANG_UA,
  LANG_EN
};

Language currentLang = LANG_DE;
bool setupCompleted = false;
bool welcomeCompleted = false;
bool wifiSetupCompleted = false;
bool staEnabled = true;

String savedSSID = "";
String savedPASS = "";

#define EEPROM_LAST_VERSION_ADDR 3600

#define EEPROM_LANG_ADDR 3500

// ====== EEPROM: user programs ======
#define EEPROM_SIZE 2048
#define MAX_USER_PROGRAMS 12

#define EEPROM_SIZE 4096

#define MAX_CARDS 6
#define MAX_PROGRAMS_PER_CARD 8

struct UserProgram {
  char name[20];
  uint16_t redSec;
  uint16_t greenSec;
  uint16_t series;
};

struct ProgramCard {
  char title[20];
  uint8_t count;
  UserProgram programs[MAX_PROGRAMS_PER_CARD];
};

// EEPROM layout:
// [0] uint16_t signature = 0xBEEF
// [2] uint16_t count
// [4] array UserProgram[MAX_USER_PROGRAMS]
const uint16_t EEPROM_SIGNATURE = 0xBEEF;
UserProgram userPrograms[MAX_USER_PROGRAMS];
uint16_t userProgramCount = 0;
ProgramCard cards[MAX_CARDS];
uint8_t cardCount = 0;


// ====== Последняя программа для кнопки ======
uint16_t lastRed = 0, lastGreen = 0, lastSeries = 0;

// Адрес хранения последних значений в EEPROM
#define EEPROM_LAST_ADDR 2000
#define EEPROM_WIFI_FLAG 3000
#define EEPROM_WIFI_SSID 3010
#define EEPROM_WIFI_PASS 3050

bool loadChangelog() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url = String(changelogURL) + "?t=" + String(millis());

  https.setTimeout(5000);
  https.setReuse(false);

  if (!https.begin(client, url)) {
    Serial.println("❌ changelog begin failed");
    return false;
  }

  int code = https.GET();

  if (code != HTTP_CODE_OK) {
    Serial.println("❌ changelog HTTP error");
    https.end();
    return false;
  }

  changelogText = https.getString();

  https.end();

  Serial.println("📋 Changelog loaded");

  return true;
}

void saveSeenVersion(String ver) {

  EEPROM.begin(EEPROM_SIZE);

  char buf[64];

  memset(buf, 0, sizeof(buf));

  strncpy(buf, ver.c_str(), sizeof(buf) - 1);

  EEPROM.put(EEPROM_LAST_VERSION_ADDR, buf);

  EEPROM.commit();

  EEPROM.end();
}

void loadSeenVersion() {

  EEPROM.begin(EEPROM_SIZE);

  char buf[64];

  EEPROM.get(EEPROM_LAST_VERSION_ADDR, buf);

  EEPROM.end();

  lastSeenVersion = String(buf);
}

void saveLastProgram() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(EEPROM_LAST_ADDR, lastRed);
  EEPROM.put(EEPROM_LAST_ADDR + sizeof(uint16_t), lastGreen);
  EEPROM.put(EEPROM_LAST_ADDR + 2 * sizeof(uint16_t), lastSeries);
  EEPROM.commit();
  EEPROM.end();
}

void loadLastProgram() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_LAST_ADDR, lastRed);
  EEPROM.get(EEPROM_LAST_ADDR + sizeof(uint16_t), lastGreen);
  EEPROM.get(EEPROM_LAST_ADDR + 2 * sizeof(uint16_t), lastSeries);
  EEPROM.end();

  if (lastRed > 0 && lastGreen > 0 && lastSeries > 0) {
    Serial.printf("↩️ Восстановлена последняя программа: R=%u G=%u S=%u\n", lastRed, lastGreen, lastSeries);
  } else {
    lastRed = lastGreen = lastSeries = 0;
  }
}

void eepromLoad() {
  EEPROM.begin(EEPROM_SIZE);

  uint16_t sig;
  EEPROM.get(0, sig);

  if (sig != EEPROM_SIGNATURE) {
    cardCount = 0;
    EEPROM.put(0, EEPROM_SIGNATURE);
    EEPROM.put(2, cardCount);
    EEPROM.commit();
    EEPROM.end();
    return;
  }

  EEPROM.get(2, cardCount);
  if (cardCount > MAX_CARDS) cardCount = 0;

  EEPROM.get(3, cards);
  EEPROM.end();
}


void eepromSave() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, EEPROM_SIGNATURE);
  EEPROM.put(2, cardCount);
  EEPROM.put(3, cards);
  EEPROM.commit();
  EEPROM.end();
}

bool loadWiFiCredentials(String &ssid, String &pass) {

  EEPROM.begin(EEPROM_SIZE);

  uint8_t flag;

  EEPROM.get(EEPROM_WIFI_FLAG, flag);

  if (flag != 1) {
    EEPROM.end();
    return false;
  }

  char ssidBuf[32];
  char passBuf[64];

  EEPROM.get(EEPROM_WIFI_SSID, ssidBuf);
  EEPROM.get(EEPROM_WIFI_PASS, passBuf);

  EEPROM.end();

  ssid = String(ssidBuf);
  pass = String(passBuf);

  return ssid.length() > 0;
}

bool updateAvailable() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

WiFiClientSecure client;
client.setInsecure();

HTTPClient https;

String url = String(versionURL) + "?t=" + String(millis()); // анти-кеш

https.setTimeout(5000);
https.setReuse(false);

if (!https.begin(client, url)) {
  Serial.println("❌ HTTPS begin failed");
  return false;
}

  int httpCode = https.GET();

  if (httpCode != HTTP_CODE_OK) {
    https.end();
    return false;
  }

  String newVersion = https.getString();
  newVersion.trim();

  https.end();

  Serial.println("Текущая версия: " + String(FW_VERSION));
  Serial.println("GitHub версия: " + newVersion);

  return newVersion != String(FW_VERSION);
}

void performOTAUpdate() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPUpdate httpUpdater;
  httpUpdater.rebootOnUpdate(false);

  httpUpdater.onProgress([](int cur, int total) {
    otaProgress = (cur * 100) / total;
    Serial.printf("OTA Progress: %d%% (%d / %d)\n", otaProgress, cur, total);
  });

  Serial.println("Start OTA...");

  t_httpUpdate_return result = httpUpdater.update(client, firmwareURL);

  switch(result) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("Update failed. Error (%d): %s\n", httpUpdater.getLastError(), httpUpdater.getLastErrorString().c_str());
      otaProgress = 0;
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("No updates");
      otaProgress = 0;
      break;

    case HTTP_UPDATE_OK:
      otaProgress = 100;
      Serial.println("Update OK");
      delay(2000);
      ESP.restart();
      break;
  }
}

bool checkVersion() {

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url = String(versionURL) + "?t=" + String(millis());

  https.setTimeout(5000);
  https.setReuse(false);

  if (!https.begin(client, url)) {
    Serial.println("❌ begin failed");
    return false;
  }

  int code = https.GET();

  if (code != HTTP_CODE_OK) {
    Serial.println("❌ HTTP error: " + String(code));
    https.end();
    return false;
  }

  remoteVersion = https.getString();
  remoteVersion.trim();

  https.end();

  updateAvailableFlag = (remoteVersion != String(FW_VERSION));

  if (updateAvailableFlag) {
  loadChangelog();
} else {
  changelogText = "";
}

  Serial.println("FW: " + String(FW_VERSION));
  Serial.println("REMOTE: " + remoteVersion);

  return true;
}

void disableSTA() {

  WiFi.disconnect(true, true);

  WiFi.mode(WIFI_AP);

  staEnabled = false;

  Serial.println("📴 STA Wi-Fi отключен");
}

void enableSTA(String ssid, String pass) {

  WiFi.mode(WIFI_AP_STA);

  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 15000) {

    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {

    staEnabled = true;

    Serial.println("");
    Serial.println("✅ STA подключен");
    Serial.println(WiFi.localIP());

  } else {

    Serial.println("");
    Serial.println("❌ STA connection failed");
  }
}

void factoryReset() {
  EEPROM.begin(EEPROM_SIZE);

  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }

  EEPROM.commit();
  EEPROM.end();

  Serial.println("🧹 Factory reset выполнен. Перезагрузка...");

  delay(500);
  ESP.restart();
}

void saveWelcome() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(EEPROM_WELCOME_ADDR, welcomeCompleted);
  EEPROM.commit();
  EEPROM.end();
}

void loadWelcome() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_WELCOME_ADDR, welcomeCompleted);
  EEPROM.end();
}

String tr(const String& de, const String& ua, const String& en) {
  switch (currentLang) {
    case LANG_UA: return ua;
    case LANG_EN: return en;
    default: return de;
  }
}

String modeName(const String& key) {

  if (key == "MP5") {
    return tr(
      "Sportpistole",          // Deutsch
      "МП5",          // Українська
      "Sport Pistol"            // English
    );
  }

  if (key == "MP8") {
    return tr(
      "Schnellfeuer",   // Deutsch
      "МП8",      // Українська
      "Rapid Fire"      // English
    );
  }

  if (key == "MP10") {
    return tr(
      "Standardpistole",      // Deutsch
      "МП10",       // Українська
      "Standart Pistol"       // English
    );
  }

  return key;
}

void saveLanguage() {
  EEPROM.begin(EEPROM_SIZE);

  uint8_t lang = (uint8_t)currentLang;

  EEPROM.put(EEPROM_LANG_ADDR, setupCompleted);
  EEPROM.put(EEPROM_LANG_ADDR + 1, lang);

  EEPROM.commit();
  EEPROM.end();
}

void loadLanguage() {
  EEPROM.begin(EEPROM_SIZE);

  EEPROM.get(EEPROM_LANG_ADDR, setupCompleted);

  uint8_t lang;
  EEPROM.get(EEPROM_LANG_ADDR + 1, lang);

  EEPROM.end();

  if (lang <= LANG_EN) {
    currentLang = (Language)lang;
  } else {
    currentLang = LANG_DE;
  }
}

// ====== URL helpers ======
String urlDecode(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < s.length()) {
      char h1 = s[i + 1], h2 = s[i + 2];
      auto hexVal = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return 0;
      };
      char v = (hexVal(h1) << 4) | hexVal(h2);
      out += v;
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String getParam(const String& req, const String& key) {
  int qsStart = req.indexOf("GET ");
  if (qsStart == -1) return "";
  int pathStart = qsStart + 4;
  int spaceAfterPath = req.indexOf(' ', pathStart);
  if (spaceAfterPath == -1) return "";
  String path = req.substring(pathStart, spaceAfterPath);
  int q = path.indexOf('?');
  if (q == -1) return "";
  String query = path.substring(q + 1);
  int start = 0;
  while (start < query.length()) {
    int amp = query.indexOf('&', start);
    if (amp == -1) amp = query.length();
    int eq = query.indexOf('=', start);
    if (eq != -1 && eq < amp) {
      String k = query.substring(start, eq);
      String v = query.substring(eq + 1, amp);
      if (k == key) return urlDecode(v);
    }
    start = amp + 1;
  }
  return "";
}

// ====== Wi-Fi AP ======
void setupWiFi() {
  IPAddress local_IP(192, 168, 3, 1);
  IPAddress gateway(192, 168, 3, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ssid, password);
  server.begin();

dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  Serial.println("✅ WiFi AP запущен");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("PASS: ");
  Serial.println(password);
  Serial.println("IP: 192.168.3.1");
}

// ====== Program control ======
void startProgram(uint16_t redSec, uint16_t greenSec, uint16_t seriesCount) {
  if (redSec == 0 || greenSec == 0 || seriesCount == 0) return;
  redDuration = (unsigned long)redSec * 1000UL;
  greenDuration = (unsigned long)greenSec * 1000UL;
  totalStages = seriesCount;
  currentStage = 0;
  isProgramRunning = true;
  stopRequested = false;
  stageRed = true;
  stageStartTime = millis();
  finishingRed = false;

  // Сохраняем для кнопки + EEPROM
  lastRed = redSec;
  lastGreen = greenSec;
  lastSeries = seriesCount;
  saveLastProgram();

  Serial.printf("▶️ Старт: R=%us G=%us series=%u\n", redSec, greenSec, seriesCount);
}

void stopProgram() {
  stopRequested = true;
  activeProgramId = -1;
  digitalWrite(redLedPin, LOW);
  digitalWrite(greenLedPin, LOW);
  isProgramRunning = false;
  activeProgramId = -1;
  finishingRed = false;
  Serial.println("⏹ Остановлено");
}

void handleProgramLogic() {
  if (!isProgramRunning || stopRequested) return;

  unsigned long now = millis();

  if (currentStage >= totalStages) {
    if (!finishingRed) {
      digitalWrite(redLedPin, HIGH);
      digitalWrite(greenLedPin, LOW);
      finishStartTime = now;
      finishingRed = true;
      Serial.println("🟥 Завершение: красный 7 сек");
    } else if (now - finishStartTime >= 7000UL) {
      digitalWrite(redLedPin, LOW);
      isProgramRunning = false;
      stopRequested = false;
      finishingRed = false;
      Serial.println("✅ Завершено");
    }
    return;
  }

  if (stageRed) {
    digitalWrite(redLedPin, HIGH);
    digitalWrite(greenLedPin, LOW);
    if (now - stageStartTime >= redDuration) {
      stageRed = false;
      stageStartTime = now;
    }
  } else {
    digitalWrite(redLedPin, LOW);
    digitalWrite(greenLedPin, HIGH);
    if (now - stageStartTime >= greenDuration) {
      currentStage++;
      stageRed = true;
      stageStartTime = now;
    }
  }
}

// ====== Button ======
void handleButton() {
  static unsigned long lastChange = 0;
  static bool lastState = true;
  bool state = digitalRead(buttonPin);  // PULLUP: LOW=pressed
  unsigned long now = millis();

  if (state != lastState && (now - lastChange) > 40) {
    lastChange = now;
    lastState = state;
    if (state == LOW) {  // нажата
      if (isProgramRunning) {
        stopProgram();
      } else {
        if (lastRed > 0 && lastGreen > 0 && lastSeries > 0) {
          startProgram(lastRed, lastGreen, lastSeries);
        } else {
          Serial.println("⚠️ Нет сохранённой программы для запуска кнопкой");
        }
      }
    }
  }
}
// ====== UI HTML ======
String htmlHeader() {
  return "<!DOCTYPE html><html lang='ru'><head><meta charset='UTF-8'>"
         "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
         "<style>"
         /* ===== Стиль страницы ===== */
         "body{margin:0;font-family:'Segoe UI',sans-serif;background:#1c1c1c;color:#fff;display:flex;flex-direction:column;align-items:center;}"
         "header{background:#2d2d2d;width:100%;padding:20px;text-align:center;font-size:24px;font-weight:bold;color:#FFD700;}"
         ".grid{display:grid;grid-template-columns:1fr 1fr;gap:15px;padding:20px;width:100%;max-width:480px;}"
         ".btn{padding:16px;text-align:center;border-radius:12px;background:#0057B7;color:#fff;text-decoration:none;font-size:18px;}"
         ".btn:hover{background:#0043a0;} .yellow{background:#FFD700;color:#000;} .yellow:hover{background:#e6c200;}"
         ".footer{margin-top:auto;padding:20px;text-align:center;font-size:18px;color:#FFD700;}"
         "form{display:flex;flex-direction:column;gap:12px;padding:20px;width:100%;max-width:480px;}"
         "input{padding:12px;border-radius:10px;border:none;font-size:16px;} button{padding:14px;background:#FFD700;color:#000;border:none;border-radius:12px;font-size:18px;cursor:pointer;} button:hover{background:#e6c200;} .row{display:flex;gap:10px;} .tag{padding:6px 10px;border-radius:999px;background:#2d2d2d;color:#FFD700;font-size:14px;margin:10px 0;}"
         
         /* ===== Блок защиты от копирования ===== */
         "* {"
         "-webkit-user-select: none;"
         "-moz-user-select: none;"
         "-ms-user-select: none;"
         "user-select: none;"
         "-webkit-touch-callout: none;"
         "-webkit-tap-highlight-color: transparent;"
         "}"
         "input, textarea, select {"
         "user-select: text;"
         "-webkit-user-select: text;"
         "}"
         "</style>"
         
         "<script>"
         "// Запрещаем контекстное меню"
         "document.addEventListener('contextmenu', function(e){ e.preventDefault(); });"
         "// Запрещаем копирование и вырезание"
         "document.addEventListener('copy', function(e){ e.preventDefault(); });"
         "document.addEventListener('cut', function(e){ e.preventDefault(); });"
         "</script>"
         
         "</head><body>";
}

String welcomePage() {
  String html;
  int battery = getBatteryPercent();

  html += "<!DOCTYPE html><html lang='ru'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>TargetiX</title>";

  html += "<style>";

  html += "body{";
  html += "margin:0;";
  html += "height:100vh;";
  html += "display:flex;";
  html += "justify-content:center;";
  html += "align-items:center;";
  html += "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto;";
  html += "background:linear-gradient(180deg,#f7f8fc,#eef2f7);";
  html += "}";

  html += ".card{";
  html += "width:320px;";
  html += "padding:40px 30px;";
  html += "border-radius:28px;";
  html += "background:#ffffff;";
  html += "box-shadow:0 20px 60px rgba(0,0,0,0.12);";
  html += "text-align:center;";
  html += "}";

  html += ".logo{";
  html += "font-size:2.4rem;";
  html += "font-weight:800;";
  html += "color:#111827;";
  html += "margin-bottom:10px;";
  html += "}";

  html += ".sub{";
  html += "font-size:1rem;";
  html += "color:#6b7280;";
  html += "margin-bottom:30px;";
  html += "}";

  html += ".btn{";
  html += "padding:16px 40px;";
  html += "border-radius:16px;";
  html += "font-size:1.1rem;";
  html += "font-weight:700;";
  html += "color:white;";
  html += "background:linear-gradient(135deg,#3b82f6,#06b6d4);";
  html += "box-shadow:0 10px 25px rgba(59,130,246,0.25);";
  html += "cursor:pointer;";
  html += "transition:all .15s ease;";
  html += "display:inline-block;";
  html += "}";

  html += ".btn:active{";
  html += "transform:scale(0.96);";
  html += "box-shadow:0 5px 15px rgba(59,130,246,0.2);";
  html += "}";

  html += ".footer{";
  html += "position:absolute;";
  html += "bottom:14px;";
  html += "font-size:.8rem;";
  html += "color:#9ca3af;";
  html += "}";

  html += "*{-webkit-tap-highlight-color:transparent;user-select:none;}";

  html += ".fade{";
  html += "opacity:0;";
  html += "transform:scale(0.98);";
  html += "transition:all .25s ease;";
  html += "}";

  html += "</style>";

  html += "<script>";

  // запрет меню
  html += "document.addEventListener('contextmenu',e=>e.preventDefault());";

  // 🔊 звук (Web Audio API)
  html += "function clickSound(){";
  html += "const ctx=new (window.AudioContext||window.webkitAudioContext)();";
  html += "const o=ctx.createOscillator();";
  html += "const g=ctx.createGain();";
  html += "o.type='sine';";
  html += "o.frequency.value=520;";
  html += "g.gain.value=0.08;";
  html += "o.connect(g);";
  html += "g.connect(ctx.destination);";
  html += "o.start();";
  html += "setTimeout(()=>{o.stop();ctx.close();},120);";
  html += "}";

  html += "function goSetup(e){";
  html += "clickSound();";
  html += "e.currentTarget.style.transform='scale(0.96)';";
  html += "document.body.classList.add('fade');";
  html += "setTimeout(()=>{location.href='/STARTSETUP';},250);";
  html += "}";

  html += "</script>";

  html += "</head><body>";

  html += "<div class='card'>";

  html += "<div class='logo'>TargetiX</div>";
  html += "<div class='sub'>Professional simulator</div>";

  html += "<div class='btn' onclick='goSetup(event)'>Start</div>";

  html += "</div>";

  html += "<div class='footer'>TargetiX PRO</div>";

  html += "</body></html>";

  return html;
}

String setupPage() {
  String html;

  html += "<!DOCTYPE html><html lang='ru'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>TargetX Setup</title>";

  html += "<style>";

  html += "body{";
  html += "margin:0;";
  html += "height:100vh;";
  html += "display:flex;";
  html += "flex-direction:column;";
  html += "justify-content:center;";
  html += "align-items:center;";
  html += "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto;";
  html += "background:linear-gradient(180deg,#f7f8fc,#eef2f7);";
  html += "overflow:hidden;";
  html += "}";

  html += ".title{";
  html += "font-size:1.8rem;";
  html += "font-weight:800;";
  html += "color:#111827;";
  html += "margin-bottom:25px;";
  html += "}";

  html += ".card{";
  html += "width:85%;";
  html += "max-width:340px;";
  html += "background:#ffffff;";
  html += "padding:18px 20px;";
  html += "margin:10px;";
  html += "border-radius:18px;";
  html += "box-shadow:0 10px 30px rgba(0,0,0,0.08);";
  html += "cursor:pointer;";
  html += "transition:all .15s ease;";
  html += "display:flex;";
  html += "flex-direction:column;";
  html += "align-items:center;";
  html += "font-size:1.2rem;";
  html += "font-weight:700;";
  html += "color:#111827;";
  html += "}";

  html += ".card:hover{";
  html += "transform:translateY(-2px);";
  html += "}";

  html += ".card:active{";
  html += "transform:scale(0.96);";
  html += "}";

  html += ".hint{";
  html += "font-size:0.9rem;";
  html += "color:#10b981;";
  html += "margin-top:4px;";
  html += "}";

  html += "*{-webkit-tap-highlight-color:transparent;user-select:none;}";

  html += ".slide{";
  html += "opacity:0;";
  html += "transform:translateX(-30px);";
  html += "transition:all .25s ease;";
  html += "}";

  html += "body.fadeOut{";
  html += "opacity:0;";
  html += "transform:scale(0.98);";
  html += "transition:all .25s ease;";
  html += "}";

  html += "</style>";

  html += "<script>";

  html += "document.addEventListener('contextmenu',e=>e.preventDefault());";

  // 🔊 лёгкий звук выбора (опционально)
  html += "function clickSound(){";
  html += "const ctx=new (window.AudioContext||window.webkitAudioContext)();";
  html += "const o=ctx.createOscillator();";
  html += "const g=ctx.createGain();";
  html += "o.type='sine';";
  html += "o.frequency.value=600;";
  html += "g.gain.value=0.06;";
  html += "o.connect(g);";
  html += "g.connect(ctx.destination);";
  html += "o.start();";
  html += "setTimeout(()=>{o.stop();ctx.close();},100);";
  html += "}";

  html += "function goMain(event,url){";
  html += "clickSound();";
  html += "event.currentTarget.style.transform='scale(0.95)';";
  html += "document.body.classList.add('fadeOut');";
  html += "setTimeout(()=>{location.href=url;},250);";
  html += "}";

  html += "</script>";

  html += "</head><body>";

  html += "<div class='title'>🌍 Select Language</div>";

  html += "<div class='card' onclick=\"goMain(event,'/SETLANG?l=de')\">";
  html += "🇩🇪 Deutsch";
  html += "</div>";

  html += "<div class='card' onclick=\"goMain(event,'/SETLANG?l=ua')\">";
  html += "🇺🇦 Українська";
  html += "</div>";

  html += "<div class='card' onclick=\"goMain(event,'/SETLANG?l=en')\">";
  html += "🇬🇧 English";
  html += "<div class='hint'>Recommended</div>";
  html += "</div>";

  html += "</body></html>";

  return html;
}



String wifiSetupPage() {

  String html;

  html += "<!DOCTYPE html><html lang='ru'><head>";

  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";

  html += "<meta http-equiv='Cache-Control' content='no-cache, no-store, must-revalidate'>";
  html += "<meta http-equiv='Pragma' content='no-cache'>";
  html += "<meta http-equiv='Expires' content='0'>";

  html += "<title>Wi-Fi Setup</title>";

  html += "<style>";

  html += "body{";

  html += "margin:0;";
  html += "background:#f4f7f9;";
  html += "font-family:'Segoe UI';";
  html += "display:flex;";
  html += "flex-direction:column;";
  html += "align-items:center;";
  html += "min-height:100vh;";

  html += "}";

  html += "header{";

  html += "padding:20px;";
  html += "font-size:2rem;";
  html += "font-weight:700;";
  html += "color:#009688;";

  html += "}";

  html += ".card{";

  html += "width:85%;";
  html += "max-width:380px;";
  html += "background:white;";
  html += "padding:22px;";
  html += "margin:10px 0;";
  html += "border-radius:24px;";
  html += "box-shadow:0 4px 12px rgba(0,0,0,.15);";
  html += "text-align:center;";

  html += "}";

  html += "select,input{";

  html += "width:100%;";
  html += "padding:14px;";
  html += "margin-top:12px;";
  html += "border:none;";
  html += "border-radius:16px;";
  html += "font-size:1rem;";
  html += "background:#f1f1f1;";
  html += "box-sizing:border-box;";

  html += "}";

  /* КНОПКИ */

  html += ".btn{";

  html += "width:100%;";
  html += "padding:16px;";
  html += "margin-top:16px;";
  html += "border:none;";
  html += "border-radius:20px;";
  html += "background:#009688;";
  html += "color:white;";
  html += "font-size:1.2rem;";
  html += "font-weight:700;";
  html += "cursor:pointer;";

  html += "position:relative;";
  html += "overflow:hidden;";

  html += "transition:transform .12s ease, box-shadow .2s ease, opacity .2s ease;";

  html += "box-shadow:0 3px 8px rgba(0,0,0,.2);";

  html += "-webkit-tap-highlight-color:transparent;";
  html += "user-select:none;";

  html += "}";

  html += ".btn:hover{";

  html += "transform:translateY(-1px);";
  html += "box-shadow:0 6px 14px rgba(0,0,0,.25);";

  html += "}";

  html += ".btn:active{";

  html += "transform:scale(.96);";

  html += "}";

html += ".back{";

html += "background:#ccc;";
html += "color:#000;";

html += "padding:12px;";
html += "font-size:1rem;";

html += "max-width:220px;";
html += "margin:14px auto 0 auto;";

html += "}";

  /* RIPPLE EFFECT */

  html += ".ripple{";

  html += "position:absolute;";
  html += "border-radius:50%;";
  html += "transform:scale(0);";
  html += "animation:ripple .6s linear;";
  html += "background:rgba(255,255,255,.5);";
  html += "pointer-events:none;";

  html += "}";

  html += "@keyframes ripple{";

  html += "to{";
  html += "transform:scale(4);";
  html += "opacity:0;";
  html += "}";

  html += "}";

html += ".overlay{";

html += "position:fixed;";
html += "inset:0;";
html += "background:rgba(0,0,0,.6);";
html += "display:none;";
html += "align-items:center;";
html += "justify-content:center;";
html += "z-index:999;";

html += "}";

html += ".modal{";

html += "background:white;";
html += "border-radius:24px;";
html += "padding:24px;";
html += "width:85%;";
html += "max-width:360px;";
html += "text-align:center;";
html += "box-shadow:0 6px 20px rgba(0,0,0,.25);";

html += "}";

html += ".modalBtn{";

html += "margin-top:12px;";
html += "padding:15px;";
html += "border-radius:18px;";
html += "font-weight:700;";
html += "cursor:pointer;";
html += "position:relative;";
html += "overflow:hidden;";
html += "transition:transform .12s ease;";

html += "}";

html += ".modalBtn:active{";

html += "transform:scale(.96);";

html += "}";

html += ".danger{";

html += "background:#d32f2f;";
html += "color:white;";

html += "}";

html += ".cancel{";

html += "background:#ccc;";
html += "color:black;";

html += "}";

  html += "</style>";

  html += "<script>";

  html += "function onSelectChange(sel){";

  html += "let pass=document.getElementById('pass');";
  html += "let opt=sel.options[sel.selectedIndex];";

  html += "if(opt.dataset.open==='1'){";

  html += "pass.style.display='none';";
  html += "pass.value='';";

  html += "}else{";

  html += "pass.style.display='block';";

  html += "}";

  html += "}";

  html += "</script>";

  html += "</head><body>";

  html += "<header>📶 Wi-Fi</header>";

  // ===== ЕСЛИ ПОДКЛЮЧЕН =====

  if (WiFi.status() == WL_CONNECTED) {

    html += "<div class='card'>";

    html += "<div style='font-size:1.4rem;font-weight:700;color:#009688'>✅ Connected</div>";

    html += "<div style='margin-top:10px;font-size:1.2rem'>";
    html += WiFi.SSID();
    html += "</div>";

    html += "</div>";

    html += "<div class='card btn back' onclick='goSettings(event)'>";
    html += "⬅ Back";
    html += "</div>";

  }
  // ===== ЕСЛИ НЕ ПОДКЛЮЧЕН =====
  else {

    int n = WiFi.scanNetworks();

    html += "<div class='card'>";

    html += "<form action='/CONNECTWIFI' method='get'>";

    html += "<select name='ssid' onchange='onSelectChange(this)' required>";

    html += "<option value=''>Select Wi-Fi</option>";

    for (int i = 0; i < n; i++) {

      bool openNet = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

      html += "<option value='" + WiFi.SSID(i) + "'";

      html += String(openNet ? " data-open='1'" : " data-open='0'");

      html += ">";

      html += WiFi.SSID(i);

      html += " (" + String(WiFi.RSSI(i)) + " dBm)";

      if (openNet) html += " 🔓";

      html += "</option>";
    }

    html += "</select>";

    html += "<input id='pass' type='password' name='pass' placeholder='Wi-Fi Password'>";

    html += "<button class='btn' type='submit' onclick='createRipple(event)'>CONNECT</button>";

    html += "</form>";

    html += "</div>";

    html += "<div class='card btn back' onclick='goSettings(event)'>";
    html += "⬅ Back";
    html += "</div>";
  }

  html += R"rawliteral(

<script>

function createRipple(event) {

  const button = event.currentTarget;

  const circle = document.createElement("span");

  const diameter = Math.max(button.clientWidth, button.clientHeight);

  circle.style.width = circle.style.height = `${diameter}px`;

  circle.style.left =
    `${event.clientX - button.offsetLeft - diameter / 2}px`;

  circle.style.top =
    `${event.clientY - button.offsetTop - diameter / 2}px`;

  circle.classList.add("ripple");

  const ripple = button.getElementsByClassName("ripple")[0];

  if (ripple) {
    ripple.remove();
  }

  button.appendChild(circle);
}

function goSettings(event){

  createRipple(event);

  setTimeout(() => {

    location.href='/SETTINGS';

  }, 180);
}

</script>

)rawliteral";

  html += "</body></html>";

  return html;
}

String settingsPage() {

  String html;

  html += "<!DOCTYPE html><html lang='ru'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Settings</title>";

  html += "<style>";

  html += "body{";
  html += "margin:0;";
  html += "background:#f4f7f9;";
  html += "font-family:'Segoe UI';";
  html += "display:flex;";
  html += "flex-direction:column;";
  html += "align-items:center;";
  html += "padding-bottom:40px;";
  html += "}";

  html += "header{";
  html += "padding:22px;";
  html += "font-size:2rem;";
  html += "font-weight:700;";
  html += "color:#009688;";
  html += "}";

  html += ".card{";
  html += "width:85%;";
  html += "max-width:380px;";
  html += "background:white;";
  html += "padding:24px;";
  html += "margin:10px 0;";
  html += "border-radius:24px;";
  html += "box-shadow:0 4px 12px rgba(0,0,0,.15);";
  html += "text-align:center;";
  html += "cursor:pointer;";
  html += "transition:transform .1s;";
  html += "}";

  html += ".card:active{";
  html += "transform:scale(.95);";
  html += "}";


  html += ".title{";
  html += "font-size:1.5rem;";
  html += "font-weight:700;";
  html += "color:#009688;";
  html += "}";

  html += ".sub{";
  html += "margin-top:8px;";
  html += "color:#555;";
  html += "}";

  html += "*{-webkit-tap-highlight-color:transparent;user-select:none}";

html += ".overlay{position:fixed;inset:0;background:rgba(0,0,0,.6);display:none;align-items:center;justify-content:center;z-index:999}";
html += ".modal{background:white;border-radius:20px;padding:24px;width:85%;max-width:360px;text-align:center}";

html += ".btn{";
html += "width:80%;";
html += "margin:8px auto;";
html += "display:block;";

html += "padding:16px 12px;";   // ↑ больше высота (главное изменение)
html += "font-size:1rem;";      // чуть крупнее текст

html += "border-radius:16px;";
html += "font-weight:700;";
html += "cursor:pointer;";
html += "position:relative;";
html += "overflow:hidden;";
html += "transition:transform .12s ease, box-shadow .2s ease;";
html += "-webkit-tap-highlight-color:transparent;";
html += "user-select:none;";
html += "}";

html += ".btn:active{transform:scale(.96);}";

html += ".red{background:#d32f2f;color:white;}";
html += ".gray{background:#ccc;color:black;}";

  html += "</style>";

  html += "</head><body>";

  html += "<header>⚙️ Settings</header>";

  // ===== WIFI =====
html += "<div class='card' "
        "style='position:relative' "
        "onclick=\"location.href='/WIFISETUP'\">"

        "<div class='title'>📶 Wi-Fi</div>"
        "<div class='sub'>Network connection</div>"
        "</div>";

  // ===== OTA =====
  html += "<div class='card' onclick=\"location.href='/UPDATE'\">";
  html += "<div class='title'>⬆️ OTA Update</div>";
  html += "<div class='sub'>Firmware update</div>";
  html += "</div>";

html += "<div class='card' onclick=\"location.href='/ABOUT'\">";
html += "<div class='title'>ℹ️ About</div>";
html += "<div class='sub'>Firmware and version</div>";
html += "</div>";

html += "<div class='card' "
        "style='position:relative;color:#d32f2f' "
        "onclick=\"openResetModal(event)\">";

html += "<div class='title'>🗑️ Factory Reset</div>";

html += "<div class='sub'>Reset all data</div>";

html += "</div>";

html += R"rawliteral(

<div id="resetOverlay" class="overlay">

  <div class="modal">

    <h2 style="margin-top:0;color:#d32f2f">
      ⚠️ Factory Reset
    </h2>

    <p style="font-size:16px;color:#444;margin:14px 0">

      All data will be deleted:<br><br>

      • Wi-Fi settings<br>
      • Saved data<br>
      • Device settings<br><br>

      Continue?

    </p>

  <div class="btn red" onclick="confirmReset(event)">
  DELETE ALL
</div>

<div class="btn gray" onclick="closeResetModal(event)">
  CANCEL
</div>

  </div>

</div>

<script>

function createRipple(event) {

  const button = event.currentTarget;

  const circle = document.createElement("span");

  const diameter =
    Math.max(button.clientWidth, button.clientHeight);

  circle.style.width =
    circle.style.height = `${diameter}px`;

  circle.style.left =
    `${event.clientX - button.offsetLeft - diameter / 2}px`;

  circle.style.top =
    `${event.clientY - button.offsetTop - diameter / 2}px`;

  circle.classList.add("ripple");

  const ripple =
    button.getElementsByClassName("ripple")[0];

  if (ripple) {
    ripple.remove();
  }

  button.appendChild(circle);
}

function openResetModal(event){

  createRipple(event);

  setTimeout(() => {

    document.getElementById('resetOverlay')
      .style.display='flex';

  }, 120);
}

function closeResetModal(event){

  createRipple(event);

  setTimeout(() => {

    document.getElementById('resetOverlay')
      .style.display='none';

  }, 120);
}

function confirmReset(event){

  createRipple(event);

  setTimeout(() => {

    location.href='/RESET';

  }, 180);
}

</script>

)rawliteral";

  // ===== BACK =====
  html += "<div class='card' onclick=\"location.href='/'\">";
  html += "<div class='title'>⬅ Back</div>";
  html += "</div>";

  html += "</body></html>";

  return html;
}

int getBatteryPercent() {

  int raw = analogRead(batteryPin);

  float voltage = (raw / 4095.0) * 2.0 * 3.3 * 1.1;

  int percent = map(voltage * 100, 330, 420, 0, 100);

  percent = constrain(percent, 0, 100);

  return percent;
}



String mainPage() {
  String html;
  html.reserve(14000);

int battery = getBatteryPercent();

  html += "<!DOCTYPE html><html lang='ru'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>TargetiX</title>";

  /* ===== STYLE ===== */
  html += "<style>";

  html += "*{-webkit-tap-highlight-color:transparent;user-select:none}";
  html += "input,textarea{user-select:text}";

  html += "body{margin:0;background:#f4f7f9;font-family:'Segoe UI';padding-bottom:70px;overflow-x:hidden;max-width:100vw;box-sizing:border-box}";

  html += "header{padding:18px 18px 18px 22px;text-align:left;font-size:1.6rem;font-weight:600;color:#00695c}";

  html += ".grid{padding:15px;display:flex;flex-direction:column;gap:12px;width:100%;max-width:420px;margin:auto}";

  html += ".card{background:white;padding:26px;border-radius:22px;box-shadow:0 4px 12px rgba(0,0,0,.15);text-align:center;cursor:pointer;position:relative;transition:transform .12s;box-sizing:border-box;width:100%;overflow:hidden;max-width:100%}";

  html += ".card:active{transform:scale(.93)}";
  html += ".locked-card{opacity:.6;pointer-events:none}";

  html += ".program-btn.locked{opacity:.45;pointer-events:none}";

  html += ".program-btn.active{box-shadow:0 0 8px rgba(0,255,200,.9),0 0 20px rgba(0,255,200,.7);animation:pulse 1.3s infinite}";

  html += "@keyframes pulse{0%{box-shadow:0 0 8px rgba(0,255,200,.6)}50%{box-shadow:0 0 24px rgba(0,255,200,1)}100%{box-shadow:0 0 8px rgba(0,255,200,.6)}}";

  html += ".title{font-size:2rem;font-weight:700;color:#009688;word-break:break-word;max-width:100%}";

  html += ".sub{color:#555;margin-top:6px;font-size:1rem}";

  html += ".stop-btn{margin:20px;padding:26px;border-radius:30px;background:linear-gradient(45deg,#ff0000,#ff4d4d,#ff0000);color:white;font-size:2rem;font-weight:800;text-align:center;cursor:pointer;display:none;box-shadow:0 0 14px rgba(255,0,0,.6);transition:transform .08s ease, box-shadow .08s ease}";

  html += ".stop-btn:active{transform:scale(1.18);box-shadow:0 0 20px rgba(255,0,0,.9),0 0 40px rgba(255,0,0,.8),0 0 60px rgba(255,0,0,.6)}";

  html += ".sound-btn{position:fixed;top:14px;right:14px;width:52px;height:52px;border-radius:50%;background:white;display:flex;align-items:center;justify-content:center;font-size:26px;box-shadow:0 4px 10px rgba(0,0,0,.25);z-index:20;opacity:.6;pointer-events:none}";
  html += ".battery{position:fixed;top:14px;right:74px;background:white;padding:10px 14px;border-radius:18px;font-weight:700;font-size:18px;box-shadow:0 4px 10px rgba(0,0,0,.25);z-index:20}.wifi-btn{position:fixed;top:14px;left:14px;width:52px;height:52px;border-radius:50%;background:white;display:flex;align-items:center;justify-content:center;font-size:26px;box-shadow:0 4px 10px rgba(0,0,0,.25);z-index:20;opacity:.6;pointer-events:none}";

  html += ".overlay{position:fixed;inset:0;background:rgba(0,0,0,.6);display:none;align-items:center;justify-content:center;z-index:50}";

  html += ".modal{background:white;border-radius:26px;padding:26px;width:85%;max-width:360px;text-align:center}";

  html += ".btn{margin-top:14px;padding:16px;border-radius:18px;font-size:1.2rem;font-weight:600;cursor:pointer}";

  html += ".danger{background:#d32f2f;color:white}";
  html += ".cancel{background:#ccc}";

  /* FIX BOX */
  html += "*{box-sizing:border-box;max-width:100%}";

  /* ===== BADGES ===== */
  html += ".badge{position:absolute;top:12px;right:14px;padding:3px 8px;border-radius:12px;font-size:.65rem;font-weight:700;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:80px}";

  html += ".badge.full{background:#00c853;color:white;box-shadow:0 0 8px rgba(0,200,83,.6)}";

  html += ".badge.beta{background:#ff9800;color:white;box-shadow:0 0 8px rgba(255,152,0,.6)}";

  html += ".lock{";
html += "background:#9e9e9e;";
html += "color:white;";
html += "}";

html += ".profile-btn{";
html += "position:fixed;";
html += "bottom:14px;";
html += "left:14px;";
html += "width:52px;";
html += "height:52px;";
html += "border-radius:50%;";
html += "background:white;";
html += "display:flex;";
html += "align-items:center;";
html += "justify-content:center;";
html += "font-size:24px;";
html += "box-shadow:0 4px 10px rgba(0,0,0,.25);";
html += "z-index:20;";
html += "cursor:pointer;";
html += "}";

html += ".sheet-overlay{";
html += "position:fixed;";
html += "inset:0;";
html += "background:rgba(0,0,0,.45);";
html += "display:none;";
html += "z-index:100;";
html += "}";

html += ".sheet{";
html += "position:fixed;";
html += "left:0;";
html += "right:0;";
html += "bottom:0;";
html += "background:white;";
html += "border-radius:24px 24px 0 0;";
html += "padding:20px;";
html += "transform:translateY(100%);";
html += "transition:.25s ease;";
html += "max-width:420px;";
html += "margin:auto;";
html += "}";

html += ".sheet.open{transform:translateY(0);}";
html += ".sheet-overlay.show{display:block;}";
html += ".sheet-title{font-size:1.4rem;font-weight:700;margin-bottom:12px;}";
html += ".sheet-item{padding:14px;border-radius:14px;background:#f3f4f6;margin:8px 0;font-weight:600}";

  html += "</style>";

  /* ===== JS ===== */
  html += "<script>";

  html += "document.addEventListener('contextmenu',e=>e.preventDefault());";

  html += "if(localStorage.getItem('sound')===null){localStorage.setItem('sound','off');}";

  html += "let soundEnabled = localStorage.getItem('sound')==='on';";
  html += "let programRunning=false;";
  html += "let currentId=null;";
  html += "let pressTimer;";
  html += "let delCardId=null;";

  html += "function updSound(){document.getElementById('soundBtn').innerHTML=soundEnabled?'🔊':'🔇'}";

  html += "function toggleSound(){soundEnabled=!soundEnabled;localStorage.setItem('sound',soundEnabled?'on':'off');updSound()}";
  html += "let wifiEnabled=" + String(staEnabled ? "true" : "false") + ";";

html += "function updWifi(){";
html += "document.getElementById('wifiBtn').innerHTML=wifiEnabled?'📶':'📴';";
html += "}";

html += "async function toggleWifi(){";

html += "if(wifiEnabled){";
html += "await fetch('/WIFIOFF');";
html += "wifiEnabled=false;";
html += "}else{";
html += "await fetch('/WIFION');";
html += "wifiEnabled=true;";
html += "}";

html += "updWifi();";
html += "}";

  html += "function speak(t){if(!soundEnabled)return;let u=new SpeechSynthesisUtterance(t);u.lang='en-US';speechSynthesis.speak(u)}";

  html += "async function runProgram(url,id){if(programRunning)return;programRunning=true;currentId=id;lockButtons(true);if(soundEnabled){speak('Load');await new Promise(r=>setTimeout(r,3000));}fetch(url);if(soundEnabled)speak('Attention')}";

  html += "function stopOnly(){fetch('/STOP');programRunning=false;currentId=null;lockButtons(false);}";

  html += "function lockButtons(state){document.querySelectorAll('.program-btn').forEach(b=>b.classList.toggle('locked',state))}";

  html += "async function upd(){try{let r=await fetch('/STATUS');let t=(await r.text()).trim();document.querySelectorAll('.program-btn').forEach(b=>b.classList.remove('active'));if(t.startsWith('RUNNING')){programRunning=true;lockButtons(true);let id=t.split(':')[1];let btn=document.querySelector('.program-btn[data-id=\"'+id+'\"]');if(btn)btn.classList.add('active');document.getElementById('stopBtn').style.display='block';}else{programRunning=false;lockButtons(false);document.getElementById('stopBtn').style.display='none';}}catch(e){}}";

  html += "function lpStart(id){pressTimer=setTimeout(()=>{delCardId=id;document.getElementById('overlay').style.display='flex'},700)}";

  html += "function lpEnd(){clearTimeout(pressTimer)}";

  html += "function delCard(){fetch('/DELCARD?id='+delCardId).then(()=>location.reload())}";

  html += "setInterval(upd,500);";
html += "window.onload=function(){updSound();updWifi();};";

html += "function openProfile(){";
html += "document.getElementById('profileOverlay').classList.add('show');";
html += "setTimeout(()=>document.getElementById('sheet').classList.add('open'),10);";
html += "}";

html += "function closeProfile(){";
html += "document.getElementById('sheet').classList.remove('open');";
html += "setTimeout(()=>document.getElementById('profileOverlay').classList.remove('show'),200);";
html += "}";

  html += "</script>";

  html += "</head><body>";

  /* UI */
  html += "<div id='soundBtn' class='sound-btn' style='position:fixed'>"
        "🔇"
        "<div style='position:absolute;"
        "bottom:-2px;"
        "right:-2px;"
        "font-size:14px;"
        "background:#fff;"
        "border-radius:50%;"
        "padding:2px'>🔒</div>"
        "</div>";
  html += "<header>TargetiX</header>";
  html += "<div class='battery'>🔋 " + String(battery) + "%</div><div class='grid'>";

  /* SETTINGS */
  html += "<div class='card' onclick=\"location.href='/SETTINGS'\">";
  html += "<div class='title'>⚙️ Settings</div>";
  html += "</div>";

  /* MP5 */
  html += "<div class='card program-btn' style='position:relative' data-id='MP5' onclick=\"runProgram('/RUNPRESET?mode=MP5','MP5')\">";
  html += "<div class='badge full'>✔ FULL</div>";
  html += "<div class='title'>" + modeName("MP5") + "</div>";
  html += "</div>";

  /* MP8 */
  html += "<div class='card' style='position:relative' onclick=\"location.href='/MP8'\">";
  html += "<div class='badge full'>✔ FULL</div>";
  html += "<div class='title'>" + modeName("MP8") + "</div>";
  html += "</div>";

  /* MP10 */
  html += "<div class='card' style='position:relative' onclick=\"location.href='/MP10'\">";
  html += "<div class='badge full'>✔ FULL</div>";
  html += "<div class='title'>" + modeName("MP10") + "</div>";
  html += "</div>";

  /* USER CARDS */
  for(uint8_t i=0;i<cardCount;i++){
    html += "<div class='card' onmousedown='lpStart(" + String(i) + ")' onmouseup='lpEnd()' ontouchstart='lpStart(" + String(i) + ")' ontouchend='lpEnd()' onclick=\"location.href='/CARD?id=" + String(i) + "'\">";
    html += "<div class='title'>📁 " + String(cards[i].title) + "</div>";
    html += "<div class='sub'>Programs: " + String(cards[i].count) + "</div>";
    html += "</div>";
  }

/* ADD */
html += "<div class='card locked-card' style='position:relative'>";
html += "<div class='badge lock'>🔒 LOCK</div>";
html += "<div class='title'>➕</div>";
html += "<div class='sub'>" + tr("Hinzufügen", "Додати", "Add") + "</div>";
html += "</div>";

  /* OVERLAY */
  html += "<div id='overlay' class='overlay'><div class='modal'>";
  html += "<h2>Remove card?</h2>";
  html += "<div class='btn danger' onclick='delCard()'>Delete</div>";
  html += "<div class='btn cancel' onclick=\"document.getElementById('overlay').style.display='none'\">Cancel</div>";
  html += "</div></div>";

  html += "<div id='stopBtn' class='stop-btn' onclick='stopOnly()'>⛔ STOP</div>";

html += "<div id='overlay' class='sheet-overlay' onclick='closeProfile()'>";

html += "<div class='sheet' id='sheet' onclick='event.stopPropagation()'>";

html += "<div class='sheet-title'>👤 Profile</div>";

html += "<div class='sheet-item'>Nickname: User1234</div>";
html += "<div class='sheet-item'>ID: TX-48291</div>";
html += "<div class='sheet-item'>Language: EN</div>";

html += "<div class='sheet-item' onclick='alert(\"Edit profile\")'>✏️ Edit</div>";

html += "</div></div>";

html += "<div style='margin:30px auto 20px;max-width:420px;text-align:center;'>";
html += "<div style='background:white;margin:0 15px;padding:14px 18px;border-radius:16px;";
html += "box-shadow:0 2px 10px rgba(0,0,0,.08);font-size:.85rem;color:#666'>";

html += "📡 WiFi: " + String(staEnabled ? "ON" : "OFF") + " • ";
html += "⚙️ System: " + String(isProgramRunning ? "RUNNING" : "READY");

html += "</div></div>";

if (false) {

  String safeLog = changelogText;

  safeLog.replace("\n", "<br>");

  html += "<div id='updateModal' style='";
  html += "position:fixed;";
  html += "top:0;left:0;right:0;bottom:0;";
  html += "background:rgba(0,0,0,.75);";
  html += "display:flex;";
  html += "align-items:center;";
  html += "justify-content:center;";
  html += "z-index:9999;'>";

  html += "<div style='";
  html += "background:#2b2b2b;";
  html += "width:90%;";
  html += "max-width:520px;";
  html += "border-radius:24px;";
  html += "padding:24px;";
  html += "box-shadow:0 8px 30px rgba(0,0,0,.45);";
  html += "color:#fff;";
  html += "font-family:Segoe UI;'>";

  html += "<div style='font-size:28px;font-weight:700;margin-bottom:10px'>🚀 Firmware Updated</div>";

  html += "<div style='font-size:16px;color:#aaa;margin-bottom:20px'>";
  html += String(FW_VERSION);
  html += "</div>";

  html += "<div style='";
  html += "background:#1f1f1f;";
  html += "padding:18px;";
  html += "border-radius:18px;";
  html += "line-height:1.7;";
  html += "font-size:15px;";
  html += "max-height:320px;";
  html += "overflow-y:auto;'>";

  html += "<b style='font-size:18px'>📋 What's New</b><br><br>";

  html += safeLog;

  html += "</div>";

  html += "<div onclick=\"closeUpdateModal()\" style='";
  html += "margin-top:20px;";
  html += "background:#009688;";
  html += "padding:16px;";
  html += "border-radius:18px;";
  html += "text-align:center;";
  html += "font-size:18px;";
  html += "font-weight:600;";
  html += "cursor:pointer;'>";
  html += "Continue";
  html += "</div>";

  html += "</div></div>";

  html += R"rawliteral(

<script>

function closeUpdateModal(){

  document.getElementById('updateModal').style.display='none';
}

</script>

)rawliteral";
}

  html += "</body></html>";

  return html;
}

String mp8Page() {
  return R"=====(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>МП8</title>

<style>
body{margin:0;background:#f4f7f9;font-family:'Segoe UI'}
header{padding:18px;text-align:center;font-size:1.6rem;font-weight:600;color:#00695c}
.grid{padding:15px;display:grid;gap:15px}
.card{
  background:white;
  padding:26px;
  border-radius:22px;
  box-shadow:0 4px 12px rgba(0,0,0,.15);
  text-align:center;
  cursor:pointer;
  transition:transform .1s,opacity .2s
}
.card:active{transform:scale(.92)}
.card.locked{opacity:.45;pointer-events:none}
.title{font-size:2rem;font-weight:700;color:#009688}

.stop-btn{
  margin:20px;
  padding:22px;
  border-radius:26px;
  background:linear-gradient(45deg,#ff0000,#ff4d4d,#ff0000);
  color:white;
  font-size:1.8rem;
  font-weight:700;
  text-align:center;
  cursor:pointer;
  display:none;
  box-shadow:0 0 16px rgba(255,0,0,.8),
             0 0 32px rgba(255,0,0,.6),
             0 0 48px rgba(255,0,0,.4);
  animation:stopPulse 1.2s infinite alternate;
}
.stop-btn:active{transform:scale(1.1)}

@keyframes stopPulse{
  0%{transform:scale(1)}
  100%{transform:scale(1.05)}
}

/* защита */
*{-webkit-tap-highlight-color:transparent;user-select:none}
input,textarea,select{user-select:text}
</style>

<script>
/* ===== STATE ===== */
let programRunning = false;
let soundEnabled = localStorage.getItem('sound') !== 'off';

/* ===== VOICE ===== */
function speak(t){
  if(!soundEnabled) return;
  let u = new SpeechSynthesisUtterance(t);
  u.lang = 'en-US';
  speechSynthesis.speak(u);
}

/* ===== LOCK BUTTONS ===== */
function lockCards(state){
  document.querySelectorAll('.card').forEach(c=>{
    state ? c.classList.add('locked') : c.classList.remove('locked');
  });
}

/* ===== RUN MP8 (REAL START) ===== */
async function runMP8(url){
  if(programRunning) return;

  programRunning = true;
  lockCards(true);

  if(soundEnabled){
    speak('Load');
    await new Promise(r=>setTimeout(r,3000));
  }

  fetch(url);

  if(soundEnabled) speak('Attention');
}

/* ===== STOP ===== */
function stopOnly(){
  fetch('/STOP');
}

/* ===== STATUS ===== */
async function upd(){
  try{
    let r = await fetch('/STATUS');
    let t = (await r.text()).trim();

    if(t.startsWith('RUNNING')){
      programRunning = true;
      lockCards(true);
      document.getElementById('stopBtn').style.display = 'block';
    }else{
      programRunning = false;
      lockCards(false);
      document.getElementById('stopBtn').style.display = 'none';
    }
  }catch(e){}
}

setInterval(upd,500);
upd();

/* блок копирования */
document.addEventListener('contextmenu',e=>e.preventDefault());
document.addEventListener('copy',e=>e.preventDefault());
document.addEventListener('cut',e=>e.preventDefault());
</script>
</head>

<body>
<header>)=====" + modeName("MP8") + R"=====(</header>

<div class="grid">
  <div class="card" onclick="runMP8('/RUNPRESET?mode=MP8&g=8')">
    <div class="title">8 сек</div>
  </div>

  <div class="card" onclick="runMP8('/RUNPRESET?mode=MP8&g=6')">
    <div class="title">6 сек</div>
  </div>

  <div class="card" onclick="runMP8('/RUNPRESET?mode=MP8&g=4')">
    <div class="title">4 сек</div>
  </div>
</div>

<div id="stopBtn" class="stop-btn" onclick="stopOnly()">⛔ STOP</div>

</body>
</html>)=====";
}

String mp10Page() {
  return R"=====(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>МП10</title>

<style>
body{margin:0;background:#f4f7f9;font-family:'Segoe UI'}
header{padding:18px;text-align:center;font-size:1.6rem;font-weight:600;color:#00695c}
.grid{padding:15px;display:grid;gap:15px}

.card{
  background:white;
  padding:26px;
  border-radius:22px;
  box-shadow:0 4px 12px rgba(0,0,0,.15);
  text-align:center;
  cursor:pointer;
  transition:transform .1s,opacity .2s
}
.card:active{transform:scale(.92)}
.card.locked{opacity:.45;pointer-events:none}

.title{font-size:2rem;font-weight:700;color:#009688}

.stop-btn{
  margin:20px;
  padding:22px;
  border-radius:26px;
  background:linear-gradient(45deg,#ff0000,#ff4d4d,#ff0000);
  color:white;
  font-size:1.8rem;
  font-weight:700;
  text-align:center;
  cursor:pointer;
  display:none;
  box-shadow:0 0 16px rgba(255,0,0,.8),
             0 0 32px rgba(255,0,0,.6),
             0 0 48px rgba(255,0,0,.4);
  animation:stopPulse 1.2s infinite alternate;
}
.stop-btn:active{transform:scale(1.1)}

@keyframes stopPulse{
  0%{transform:scale(1)}
  100%{transform:scale(1.05)}
}

/* защита */
*{-webkit-tap-highlight-color:transparent;user-select:none}
input,textarea,select{user-select:text}
</style>

<script>
/* ===== STATE ===== */
let programRunning = false;
let soundEnabled = localStorage.getItem('sound') !== 'off';

/* ===== VOICE ===== */
function speak(text){
  if(!soundEnabled) return;
  let u = new SpeechSynthesisUtterance(text);
  u.lang = 'en-US';
  speechSynthesis.speak(u);
}

/* ===== LOCK CARDS ===== */
function lockCards(state){
  document.querySelectorAll('.card').forEach(c=>{
    state ? c.classList.add('locked') : c.classList.remove('locked');
  });
}

/* ===== RUN MP10 ===== */
async function runMP10(url){
  if(programRunning) return;

  programRunning = true;
  lockCards(true);

  if(soundEnabled){
    speak('Load');
    await new Promise(r=>setTimeout(r,3000));
  }

  fetch(url);

  if(soundEnabled) speak('Attention');
}

/* ===== STOP ===== */
function stopOnly(){
  fetch('/STOP');
}

/* ===== STATUS ===== */
async function upd(){
  try{
    let r = await fetch('/STATUS');
    let t = (await r.text()).trim();

    if(t.startsWith('RUNNING')){
      programRunning = true;
      lockCards(true);
      document.getElementById('stopBtn').style.display = 'block';
    }else{
      programRunning = false;
      lockCards(false);
      document.getElementById('stopBtn').style.display = 'none';
    }
  }catch(e){}
}

setInterval(upd,500);
upd();

/* блок копирования */
document.addEventListener('contextmenu',e=>e.preventDefault());
document.addEventListener('copy',e=>e.preventDefault());
document.addEventListener('cut',e=>e.preventDefault());
</script>
</head>

<body>
<header>)=====" + modeName("MP10") + R"=====(</header>

<div class="grid">
  <div class="card" onclick="runMP10('/RUNPRESET?mode=MP10&g=150')">
    <div class="title">150 сек</div>
  </div>

  <div class="card" onclick="runMP10('/RUNPRESET?mode=MP10&g=20')">
    <div class="title">20 сек</div>
  </div>

  <div class="card" onclick="runMP10('/RUNPRESET?mode=MP10&g=10')">
    <div class="title">10 сек</div>
  </div>
</div>

<div id="stopBtn" class="stop-btn" onclick="stopOnly()">⛔ STOP</div>

</body>
</html>)=====";
}

String addPage() {
  String html;
  html.reserve(3500);

  html += R"=====(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Новая программа</title>

<style>
body{margin:0;background:#f4f7f9;font-family:'Segoe UI'}
header{padding:18px;text-align:center;font-size:1.6rem;font-weight:600;color:#00695c}

form{
  padding:20px;
  display:flex;
  flex-direction:column;
  gap:14px
}

input,select{
  padding:14px;
  font-size:1.1rem;
  border-radius:16px;
  border:none;
  box-shadow:0 3px 8px rgba(0,0,0,.15)
}

button{
  padding:16px;
  font-size:1.3rem;
  border-radius:20px;
  border:none;
  background:#009688;
  color:white
}

.back{
  margin:20px;
  padding:14px;
  text-align:center;
  border-radius:18px;
  background:#ddd
}

/* ===== защита от копирования ===== */
*{
  -webkit-tap-highlight-color:transparent;
  user-select:none;
}
input, textarea, select{
  user-select:text;
  -webkit-user-select:text;
}
</style>

<script>
function onCardChange(sel){
  let newCard = document.querySelector("input[name='newcard']");
  if(sel.value === ""){
    newCard.style.display = "block";
  }else{
    newCard.style.display = "none";
    newCard.value = "";
  }
}

/* блокировка копирования и контекстного меню */
document.addEventListener('contextmenu', e=>e.preventDefault());
document.addEventListener('copy', e=>e.preventDefault());
document.addEventListener('cut', e=>e.preventDefault());
</script>

</head>
<body>

<header>➕ New program</header>

<form action="/SAVE" method="get">

<input name="progname" placeholder="Program name" maxlength="19" required>

<select name="card" onchange="onCardChange(this)">
  <option value="">➕ New card</option>
)=====";

  for (uint8_t i = 0; i < cardCount; i++) {
    html += "<option value='" + String(i) + "'>" + String(cards[i].title) + "</option>";
  }

  html += R"=====(</select>

<input type="text" name="newcard" placeholder="Name of the new card">

<input type="number" name="red" placeholder="Red (sec)" min="1" required>
<input type="number" name="green" placeholder="Green (sec)" min="1" required>
<input type="number" name="series" placeholder="Series" min="1" required>

<button type="submit">💾 Save</button>

</form>

<div class="back" onclick="history.back()">⬅ Back</div>

</body>
</html>)=====";

  return html;
}

String updatePage() {

  String status;

  if (remoteVersion == "") {
    status = "❌ Not verified";
  } else if (updateAvailableFlag) {
    status = "🟡 Update available";
  } else {
    status = "🟢 You have the latest version";
  }

  String html;

  html += "<!DOCTYPE html><html><head>";

  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";

  html += "<meta http-equiv='Cache-Control' content='no-cache, no-store, must-revalidate'>";
  html += "<meta http-equiv='Pragma' content='no-cache'>";
  html += "<meta http-equiv='Expires' content='0'>";

  html += "<style>";

  html += "body{";

  html += "font-family:Segoe UI;";
  html += "background:#f4f7f9;";
  html += "text-align:center;";
  html += "padding:30px;";

  html += "}";

  html += ".card{";

  html += "background:white;";
  html += "padding:25px;";
  html += "border-radius:20px;";
  html += "box-shadow:0 4px 12px rgba(0,0,0,.2);";
  html += "max-width:400px;";
  html += "margin:auto;";

  html += "}";

  html += ".btn{";

  html += "margin-top:15px;";
  html += "padding:16px;";
  html += "background:#009688;";
  html += "color:white;";
  html += "border-radius:18px;";
  html += "cursor:pointer;";

  html += "position:relative;";
  html += "overflow:hidden;";

  html += "transition:transform .12s ease, box-shadow .2s ease, opacity .2s ease;";

  html += "box-shadow:0 3px 8px rgba(0,0,0,.2);";

  html += "-webkit-tap-highlight-color:transparent;";
  html += "user-select:none;";

  html += "}";

  html += ".btn:hover{";

  html += "transform:translateY(-1px);";
  html += "box-shadow:0 6px 14px rgba(0,0,0,.25);";

  html += "}";

  html += ".btn:active{";

  html += "transform:scale(0.96);";

  html += "}";

  html += ".btn.red{";

  html += "background:#d32f2f;";

  html += "}";

  html += ".btn.gray{";

  html += "background:#555;";

  html += "}";

  html += ".btn.loading{";

  html += "opacity:.7;";
  html += "pointer-events:none;";

  html += "}";

  html += ".btn.loading::after{";

  html += "content:'';";
  html += "position:absolute;";
  html += "right:15px;";
  html += "top:50%;";
  html += "width:14px;";
  html += "height:14px;";
  html += "margin-top:-7px;";
  html += "border:2px solid #fff;";
  html += "border-top:2px solid transparent;";
  html += "border-radius:50%;";
  html += "animation:spin .8s linear infinite;";

  html += "}";

  html += "@keyframes spin{";

  html += "0%{transform:rotate(0deg)}";
  html += "100%{transform:rotate(360deg)}";

  html += "}";

  /* GOOGLE RIPPLE EFFECT */

  html += ".ripple{";

  html += "position:absolute;";
  html += "border-radius:50%;";
  html += "transform:scale(0);";
  html += "animation:ripple .6s linear;";
  html += "background:rgba(255,255,255,.5);";
  html += "pointer-events:none;";

  html += "}";

  html += "@keyframes ripple{";

  html += "to{";
  html += "transform:scale(4);";
  html += "opacity:0;";
  html += "}";

  html += "}";

  html += "</style></head><body>";

  html += "<div class='card'>";

  html += "<h2>OTA Update</h2>";

  html += "<p><b>Current version:</b> <span id='fw'>"
      + String(FW_VERSION)
      + " (" + String(FW_BUILD) + ")"
      + "</span></p>";

  html += "<p><b>Available:</b> <span id='remote'>" + remoteVersion + "</span></p>";

  html += "<h3 id='statusText'>" + status + "</h3>";

if (changelogText.length() > 0) {

  String safeLog = changelogText;

  safeLog.replace("\n", "<br>");

  html += "<div style='margin-top:18px;";
  html += "padding:16px;";
  html += "background:#f1f1f1;";
  html += "border-radius:14px;";
  html += "text-align:left;";
  html += "line-height:1.6;";
  html += "font-size:14px;";
  html += "max-height:240px;";
  html += "overflow-y:auto;";
  html += "color:#111;'>";   // ← ВАЖНО

  html += "<b style='color:#000'>📋 What's new:</b><br><br>";

  html += safeLog;

  html += "</div>";
}

  html += "<div class='btn' id='checkBtn' onclick='checkUpdate(event)'>Check update</div>";

  if (updateAvailableFlag) {

    html += "<div class='btn red' id='updateBtn' onclick='startUpdate(event)'>Update</div>";

  }

  html += "<div class='btn gray' onclick='refreshPage(event)'>Refresh page</div>";

  html += "<div class='btn gray' onclick='goBack(event)'>⬅ Back</div>";

  html += R"rawliteral(

<script>

function createRipple(event) {

  const button = event.currentTarget;

  const circle = document.createElement("span");

  const diameter = Math.max(button.clientWidth, button.clientHeight);

  circle.style.width = circle.style.height = `${diameter}px`;

  circle.style.left =
    `${event.clientX - button.offsetLeft - diameter / 2}px`;

  circle.style.top =
    `${event.clientY - button.offsetTop - diameter / 2}px`;

  circle.classList.add("ripple");

  const ripple = button.getElementsByClassName("ripple")[0];

  if (ripple) {
    ripple.remove();
  }

  button.appendChild(circle);
}

async function checkUpdate(event) {

  createRipple(event);

  const btn = document.getElementById('checkBtn');

  btn.classList.add('loading');

  try {

    await fetch('/CHECKUPDATE');

    const res = await fetch('/UPDATESTATUS');

    const data = await res.json();

    document.getElementById("remote").innerText =
      data.remoteVersion;

    let status = "";

    if (!data.remoteVersion || data.remoteVersion === "") {

      status = "❌ Not verified";

    } else if (data.updateAvailable) {

      status = "🟡 Update available";

    } else {

      status = "🟢 You have the latest version";

    }

    document.getElementById("statusText").innerText = status;

    let updateBtn = document.getElementById("updateBtn");

    if (data.updateAvailable && !updateBtn) {

      updateBtn = document.createElement("div");

      updateBtn.className = "btn red";

      updateBtn.id = "updateBtn";

      updateBtn.innerText = "Update";

      updateBtn.onclick = startUpdate;

      document.querySelector(".card").appendChild(updateBtn);
    }

  } catch(e) {

    console.log(e);

  }

  setTimeout(() => {

    btn.classList.remove('loading');

  }, 800);
}

async function startUpdate(event) {

  createRipple(event);

  const btn = document.getElementById('updateBtn');

  btn.classList.add('loading');

  alert('The update has started... the device will reboot automatically');

  fetch('/STARTUPDATE');
}

function refreshPage(event){

  createRipple(event);

  setTimeout(() => {

    location.reload();

  }, 180);
}

function goBack(event){

  createRipple(event);

  setTimeout(() => {

    history.back();

  }, 180);
}

</script>

)rawliteral";

  html += "</div></body></html>";

  return html;
}

String aboutPage() {

  String html;

  html += "<!DOCTYPE html><html><head>";

  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";

  html += "<meta http-equiv='Cache-Control' content='no-cache, no-store, must-revalidate'>";
  html += "<meta http-equiv='Pragma' content='no-cache'>";
  html += "<meta http-equiv='Expires' content='0'>";

  html += "<style>";

  html += "body{";

  html += "margin:0;";
  html += "font-family:Segoe UI;";
  html += "background:#f4f7f9;";
  html += "display:flex;";
  html += "align-items:center;";
  html += "justify-content:center;";
  html += "height:100vh;";

  html += "}";

  html += ".card{";

  html += "background:white;";
  html += "padding:18px;";
  html += "border-radius:18px;";
  html += "box-shadow:0 3px 8px rgba(0,0,0,.15);";
  html += "text-align:center;";
  html += "max-width:280px;";
  html += "width:78%;";

  html += "}";

  html += ".title{";

  html += "font-size:1.8rem;";
  html += "font-weight:700;";
  html += "color:#009688;";
  html += "margin-bottom:15px;";

  html += "}";

  html += ".ver{";

  html += "font-size:1.2rem;";
  html += "color:#333;";
  html += "margin:8px 0;";

  html += "}";

  html += ".btn{";

  html += "margin-top:18px;";
  html += "padding:14px;";
  html += "background:#009688;";
  html += "color:white;";
  html += "border-radius:18px;";
  html += "cursor:pointer;";
  html += "font-weight:700;";

  html += "position:relative;";
  html += "overflow:hidden;";

  html += "transition:transform .12s ease, box-shadow .2s ease, opacity .2s ease;";

  html += "box-shadow:0 3px 8px rgba(0,0,0,.2);";

  html += "-webkit-tap-highlight-color:transparent;";
  html += "user-select:none;";

  html += "}";

  html += ".btn:hover{";

  html += "transform:translateY(-1px);";
  html += "box-shadow:0 6px 14px rgba(0,0,0,.25);";

  html += "}";

  html += ".btn:active{";

  html += "transform:scale(.96);";

  html += "}";

  /* GOOGLE RIPPLE EFFECT */

  html += ".ripple{";

  html += "position:absolute;";
  html += "border-radius:50%;";
  html += "transform:scale(0);";
  html += "animation:ripple .6s linear;";
  html += "background:rgba(255,255,255,.5);";
  html += "pointer-events:none;";

  html += "}";

  html += "@keyframes ripple{";

  html += "to{";
  html += "transform:scale(4);";
  html += "opacity:0;";
  html += "}";

  html += "}";

  html += "</style>";

  html += "</head><body>";

  html += "<div class='card'>";

  html += "<div class='title'>ℹ️ About the firmware</div>";

  html += "<div class='ver'>Version: <b>" + String(FW_VERSION) + "</b></div>";

  html += "<div class='ver'>Assembly: <b>" + String(__DATE__) + "</b></div>";

  html += "<div class='ver'>Device: TargetiX</div>";

  html += "<div class='btn' onclick='goBack(event)'>⬅ Back</div>";

  html += "</div>";

  html += R"rawliteral(

<script>

function createRipple(event) {

  const button = event.currentTarget;

  const circle = document.createElement("span");

  const diameter = Math.max(button.clientWidth, button.clientHeight);

  circle.style.width = circle.style.height = `${diameter}px`;

  circle.style.left =
    `${event.clientX - button.offsetLeft - diameter / 2}px`;

  circle.style.top =
    `${event.clientY - button.offsetTop - diameter / 2}px`;

  circle.classList.add("ripple");

  const ripple = button.getElementsByClassName("ripple")[0];

  if (ripple) {
    ripple.remove();
  }

  button.appendChild(circle);
}

function goBack(event){

  createRipple(event);

  setTimeout(() => {

    history.back();

  }, 180);
}

</script>

)rawliteral";

  html += "</body></html>";

  return html;
}

String cardPage(uint8_t cardId) {
  if (cardId >= cardCount) return "Ошибка: нет такой карточки";

  ProgramCard &c = cards[cardId];
  String html;
  html.reserve(6000);

  html += "<!DOCTYPE html><html lang='ru'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>" + String(c.title) + "</title>";

  /* ===== STYLE ===== */
  html += "<style>";
html += "body{margin:0;background:#f4f7f9;font-family:'Segoe UI';padding-bottom:70px;overflow-x:hidden}";
  html += "input,textarea,select{user-select:text}";
  html += "header{padding:18px;text-align:center;font-size:1.6rem;font-weight:600;color:#00695c}";
  html += ".grid{padding:15px;display:flex;flex-direction:column;gap:12px}";
  html += ".card{background:white;padding:26px;border-radius:22px;box-shadow:0 4px 12px rgba(0,0,0,.15);text-align:center;cursor:pointer;transition:.15s}";
  html += ".card:active{transform:scale(.95)}";
  html += ".card.locked{opacity:.45;pointer-events:none}";
  html += ".title{font-size:2rem;font-weight:700;color:#009688}";
  html += ".sub{color:#555;margin-top:6px}";
  html += ".stop-btn{margin:20px;padding:22px;border-radius:26px;background:linear-gradient(45deg,#ff0000,#ff4d4d,#ff0000);color:white;font-size:1.8rem;font-weight:700;text-align:center;cursor:pointer;display:none;box-shadow:0 0 16px rgba(255,0,0,.8);animation:pulse 1.2s infinite alternate}";
  html += "@keyframes pulse{from{transform:scale(1)}to{transform:scale(1.05)}}";
  html += ".overlay{position:fixed;inset:0;background:rgba(0,0,0,.6);display:none;align-items:center;justify-content:center;z-index:10}";
  html += ".modal{background:white;border-radius:24px;padding:26px;width:85%;max-width:360px;text-align:center}";
  html += ".modal button{width:100%;margin-top:12px;padding:16px;font-size:1.2rem;border:none;border-radius:18px}";
  html += ".del{background:#d32f2f;color:white}";
  html += ".cancel{background:#ccc}";
  html += "</style>";

  /* ===== JS ===== */
  html += "<script>";
  html += "let programRunning=false;";
  html += "let soundEnabled=localStorage.getItem('sound')!=='off';";
  html += "let pressTimer=null;";
  html += "let delC=0, delP=0;";

  html += "function speak(t){if(!soundEnabled)return;let u=new SpeechSynthesisUtterance(t);u.lang='en-US';speechSynthesis.speak(u);}";

  html += "function lockCards(s){document.querySelectorAll('.card').forEach(c=>s?c.classList.add('locked'):c.classList.remove('locked'));}";

  /* ===== RUN ===== */
  html += "async function runCard(url){";
  html += "if(programRunning)return;";
  html += "programRunning=true;lockCards(true);";
  html += "if(soundEnabled){speak('Load');await new Promise(r=>setTimeout(r,3000));}";
  html += "fetch(url);";
  html += "if(soundEnabled)speak('Attention');";
  html += "}";

  /* ===== STOP ===== */
  html += "function stopOnly(){fetch('/STOP');}";

  /* ===== LONG PRESS DELETE ===== */
  html += "function lpStart(c,p){";
  html += "pressTimer=setTimeout(()=>{";
  html += "delC=c;delP=p;";
  html += "document.getElementById('overlay').style.display='flex';";
  html += "},700);}";
  html += "function lpEnd(){clearTimeout(pressTimer);}";

  html += "function confirmDel(){";
  html += "fetch('/DELPROG?c='+delC+'&p='+delP).then(()=>location.reload());}";

  /* ===== STATUS ===== */
  html += "async function upd(){";
  html += "try{let r=await fetch('/STATUS');let t=(await r.text()).trim();";
  html += "if(t.startsWith('RUNNING')){programRunning=true;lockCards(true);document.getElementById('stopBtn').style.display='block';}";
  html += "else{programRunning=false;lockCards(false);document.getElementById('stopBtn').style.display='none';}";
  html += "}catch(e){}}";
  html += "setInterval(upd,500);upd();";

  html += "document.addEventListener('contextmenu',e=>e.preventDefault());";
  html += "</script>";

  html += "</head><body>";
  html += "<header> " + String(c.title) + "</header>";
  html += "<div class='grid'>";

  /* ===== PROGRAMS ===== */
  for(uint8_t p=0;p<c.count;p++){
    UserProgram &pr=c.programs[p];
    html += "<div class='card' ";
    html += "onmousedown='lpStart(" + String(cardId) + "," + String(p) + ")' ";
    html += "onmouseup='lpEnd()' ";
    html += "ontouchstart='lpStart(" + String(cardId) + "," + String(p) + ")' ";
    html += "ontouchend='lpEnd()' ";
    html += "onclick=\"runCard('/RUN?c=" + String(cardId) + "&p=" + String(p) + "')\">";
    html += "<div class='title'>" + String(pr.name) + "</div>";
    html += "<div class='sub'>R " + String(pr.redSec) + " · G " + String(pr.greenSec) + " · ×" + String(pr.series) + "</div>";
    html += "</div>";
  }

  html += "</div>";

  /* ===== OVERLAY DELETE ===== */
  html += "<div id='overlay' class='overlay'>";
  html += "<div class='modal'>";
  html += "<h2>Remove a program?</h2>";
  html += "<button class='del' onclick='confirmDel()'>Delete</button>";
  html += "<button class='cancel' onclick=\"document.getElementById('overlay').style.display='none'\">Cancel</button>";
  html += "</div></div>";

  html += "<div id='stopBtn' class='stop-btn' onclick='stopOnly()'>⛔ STOP</div>";

  html += "</body></html>";
  return html;
}

void saveWiFiCredentials(String ssid, String pass) {

  EEPROM.begin(EEPROM_SIZE);

  uint8_t flag = 1;

  EEPROM.put(EEPROM_WIFI_FLAG, flag);

  char ssidBuf[32];
  char passBuf[64];

  memset(ssidBuf, 0, sizeof(ssidBuf));
  memset(passBuf, 0, sizeof(passBuf));

  strncpy(ssidBuf, ssid.c_str(), sizeof(ssidBuf) - 1);
  strncpy(passBuf, pass.c_str(), sizeof(passBuf) - 1);

  EEPROM.put(EEPROM_WIFI_SSID, ssidBuf);
  EEPROM.put(EEPROM_WIFI_PASS, passBuf);

  EEPROM.commit();
  EEPROM.end();

  Serial.println("💾 Wi-Fi сохранён");
}


void connectToWiFi(String ssid, String pass) {

  Serial.println("📶 Подключение к Wi‑Fi...");

enableSTA(ssid, pass);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi‑Fi подключен");
    Serial.println(WiFi.localIP());

    savedSSID = ssid;
    savedPASS = pass;

    wifiSetupCompleted = true;
    saveWiFiCredentials(ssid, pass);
  } else {
    Serial.println("\n❌ Ошибка подключения");
  }
}





void handleRequest(WiFiClient &client, String request) {

  /* ===== STATUS ===== */
  if (request.startsWith("GET /STATUS")) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    if (isProgramRunning)
      client.println("RUNNING:" + String(activeProgramId));
    else
      client.println("STOPPED");
    return;
  }

  /* ===== MP8 PAGE ===== */
  if (request.startsWith("GET /MP8")) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.print(mp8Page());
    return;
  }

  /* ===== MP10 PAGE ===== */
  if (request.startsWith("GET /MP10")) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.print(mp10Page());
    return;
  }
  /* ===== ADD PROGRAM PAGE ===== */
if (request.startsWith("GET /ADD")) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.print(addPage());
  return;
}


  /* ===== RUN USER PROGRAM ===== */
if (request.startsWith("GET /RUN?")) {
  int c = getParam(request, "c").toInt();
  int p = getParam(request, "p").toInt();

  if (c < cardCount && p < cards[c].count) {
    UserProgram &pr = cards[c].programs[p];
    startProgram(pr.redSec, pr.greenSec, pr.series);
  }

  client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
  return;
}


  /* ===== RUN PRESET ===== */
  if (request.startsWith("GET /RUNPRESET")) {
    String mode = getParam(request, "mode");
    int g = getParam(request, "g").toInt();

    if (mode == "MP5") {
      activeProgramId = -10;
      startProgram(7, 3, 5);
    } else if (mode == "MP8") {
      activeProgramId = -11;
      startProgram(7, g, 1);
    } else if (mode == "MP10") {
      activeProgramId = -12;
      startProgram(7, g, 1);
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Connection: close");
    client.println();
    return;
  }

  /* ===== STOP ===== */
  if (request.startsWith("GET /STOP")) {
    stopProgram();
    activeProgramId = -1;
    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /");
    client.println();
    return;
  }

  /* ===== CLEAR ALL ===== */
  if (request.startsWith("GET /CLEAR")) {
    userProgramCount = 0;
    eepromSave();
    activeProgramId = -1;
    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /");
    client.println();
    return;
  }
  /* ===== SAVE PROGRAM ===== */
if (request.startsWith("GET /SAVE")) {

  String progName = getParam(request, "progname");
  String cardStr = getParam(request, "card");
  String newCard = getParam(request, "newcard");

  uint16_t red = getParam(request, "red").toInt();
  uint16_t green = getParam(request, "green").toInt();
  uint16_t series = getParam(request, "series").toInt();

  if (progName == "" || red == 0 || green == 0 || series == 0) {
    client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
    return;
  }

  uint8_t cardId;

  // ➕ новая карточка
  if (cardStr == "") {
    if (cardCount >= MAX_CARDS || newCard == "") {
      client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
      return;
    }

    cardId = cardCount++;
    memset(&cards[cardId], 0, sizeof(ProgramCard));
    strncpy(cards[cardId].title, newCard.c_str(), 19);
  } else {
    cardId = cardStr.toInt();
    if (cardId >= cardCount) return;
  }

  ProgramCard &c = cards[cardId];
  if (c.count >= MAX_PROGRAMS_PER_CARD) return;

  UserProgram &p = c.programs[c.count++];
  memset(&p, 0, sizeof(UserProgram));
  strncpy(p.name, progName.c_str(), 19);
  p.redSec = red;
  p.greenSec = green;
  p.series = series;

  eepromSave();

  client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
  return;
}

// ===== CARD PAGES =====
if (request.startsWith("GET /CARD?")) {
  int c = getParam(request, "id").toInt();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.print(cardPage(c));
  return;
}

// ===== Удалить программу =====
if (request.startsWith("GET /DELPROG")) {
  int c = getParam(request, "c").toInt();
  int p = getParam(request, "p").toInt();
  if (c < cardCount && p < cards[c].count) {
    // Сдвигаем все программы после p на одну позицию влево
    for (int i = p; i < cards[c].count - 1; i++) {
      cards[c].programs[i] = cards[c].programs[i + 1];
    }
    cards[c].count--;
    eepromSave();
  }
  client.println("HTTP/1.1 303 See Other\r\nLocation: /CARD?id=" + String(c) + "\r\n");
  return;
}

// ===== Удалить карточку =====
if (request.startsWith("GET /DELCARD")) {
  int id = getParam(request, "id").toInt();
  if (id < cardCount) {
    // Сдвигаем все карточки после id на одну позицию влево
    for (int i = id; i < cardCount - 1; i++) {
      cards[i] = cards[i + 1];
    }
    cardCount--;
    eepromSave();
  }
  client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
  return;
}


/* ===== RUN LAST PROGRAM ===== */
if (request.startsWith("GET /RUNLAST")) {

  if (lastRed > 0 && lastGreen > 0 && lastSeries > 0) {
    activeProgramId = -99;   // ID для last program
    startProgram(lastRed, lastGreen, lastSeries);
    Serial.println("▶️ Запуск последней программы (RUNLAST)");
  } else {
    Serial.println("⚠️ Нет сохранённой последней программы");
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Connection: close");
  client.println();
  return;
}

// ===== TOGGLE START / STOP =====
if (request.startsWith("GET /TOGGLE")) {

  if (isProgramRunning) {
    stopProgram();
  } else {
    if (lastRed && lastGreen && lastSeries) {
      startProgram(lastRed, lastGreen, lastSeries);
    }
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Connection: close");
  client.println();
  return;
}


/* ===== SET LANGUAGE ===== */
if (request.startsWith("GET /SETLANG")) {

  String l = getParam(request, "l");

if (l == "de") currentLang = LANG_DE;
else if (l == "ua") currentLang = LANG_UA;
else if (l == "en") currentLang = LANG_EN;

  setupCompleted = true;
  saveLanguage();

  client.println("HTTP/1.1 303 See Other");
  wifiSetupCompleted = true;

client.println("Location: /");
  client.println();
  return;
}

if (request.startsWith("GET /STARTSETUP")) {

  welcomeCompleted = true;
  saveWelcome();

  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /");
  client.println();
  return;
}

/* ===== WIFI PAGE ===== */
if (request.startsWith("GET /WIFISETUP")) {

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  client.print(wifiSetupPage());
  return;
}

/* ===== CONNECT WIFI ===== */
if (request.startsWith("GET /CONNECTWIFI")) {

  String ssid = getParam(request, "ssid");
  String pass = getParam(request, "pass");

  connectToWiFi(ssid, pass);

  // ❗ НЕ редиректим в главное меню
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  client.print(wifiSetupPage());
  return;
}

if (request.startsWith("GET /STARTUPDATE")) {

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/plain");
  client.println("Connection: close");
  client.println();
  client.println("Updating...");

  performOTAUpdate();

  return;
}

if (request.startsWith("GET /UPDATE")) {

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  client.print(updatePage());
  return;
}

if (request.startsWith("GET /CHECKUPDATE")) {

  checkVersion();

  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /UPDATE");
  client.println();
  return;
}

/* ===== WIFI STA OFF ===== */
if (request.startsWith("GET /WIFIOFF")) {

  disableSTA();

  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /");
  client.println();
  return;
}

/* ===== WIFI STA ON ===== */
if (request.startsWith("GET /WIFION")) {

  if (savedSSID.length() > 0) {

    enableSTA(savedSSID, savedPASS);
  }

  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /");
  client.println();
  return;
}

/* ===== OTA PROGRESS ===== */
if (request.startsWith("GET /UPDATEPROGRESS")) {

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/event-stream");
  client.println("Cache-Control: no-cache");
  client.println("Connection: keep-alive");
  client.println();

  int last = -1;

  while (client.connected()) {

    if (otaProgress != last) {

      client.print("data: ");
      client.print(otaProgress);
      client.print("\n\n");

      last = otaProgress;
    }

    delay(200);
  }

  return;
}

/* ===== SETTINGS PAGE ===== */
if (request.startsWith("GET /SETTINGS")) {

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  client.print(settingsPage());

  return;
}

if (request.startsWith("GET /RESET")) {
  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /");
  client.println();

  factoryReset();
  return;
}

/* ===== ABOUT FIRMWARE ===== */
if (request.startsWith("GET /ABOUT")) {

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  client.print(aboutPage());
  return;
}

if (request.startsWith("GET /UPDATESTATUS")) {

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();

  client.print("{");
  client.print("\"remoteVersion\":\"" + remoteVersion + "\",");
  client.print("\"updateAvailable\":");
  client.print(updateAvailableFlag ? "true" : "false");
  client.print("}");

  return;
}
/* ===== UNKNOWN URL REDIRECT ===== */

if (
    request.indexOf("GET / ") == -1 &&
    request.indexOf("GET /STATUS") == -1 &&
    request.indexOf("GET /MP8") == -1 &&
    request.indexOf("GET /MP10") == -1
   ) {

  client.println("HTTP/1.1 302 Found");
  client.println("Location: http://192.168.3.1/");
  client.println("Connection: close");
  client.println();

  return;
}

/* ===== MAIN PAGE (ВСЕГДА В КОНЦЕ) ===== */

client.println("HTTP/1.1 200 OK");
client.println("Content-Type: text/html; charset=utf-8");
client.println("Connection: close");
client.println();

if (!welcomeCompleted) {

  client.print(welcomePage());

}
else if (!setupCompleted) {

  client.print(setupPage());

}
else {

  client.print(mainPage());
}

return;
}



// ====== Setup / Loop ======


void buildMP8Log(ShotData &data, uint32_t totalTime) {

    memset(mp8Log, 0, sizeof(mp8Log));

    if (data.miss) {

        snprintf(
            mp8Log,
            sizeof(mp8Log),
            "MP8 | MISS | Total: %lu ms",
            totalTime
        );

        return;
    }

    snprintf(
        mp8Log,
        sizeof(mp8Log),
        "MP8 | SUCCESS | Total: %lu ms\n"
    );

    char temp[64];

    for (int i = 0; i < data.shotCount; i++) {

        snprintf(
            temp,
            sizeof(temp),
            "Shot %d: %lu ms\n",
            i + 1,
            data.shotTime[i]
        );

        strncat(
            mp8Log,
            temp,
            sizeof(mp8Log) - strlen(mp8Log) - 1
        );
    }
}



void handleStartButton() {

    bool reading = digitalRead(startButtonPin);

    if (reading != lastStartButtonState) {
        lastStartDebounceTime = millis();
    }

    if ((millis() - lastStartDebounceTime) > 50) {

        if (reading == LOW) {

            Serial.println("START BUTTON");
        }
    }

    lastStartButtonState = reading;
}


void setup() {
  Serial.begin(115200);

    prefs.begin("targetix", false);

    pinMode(buttonPin, INPUT_PULLUP);
    pinMode(startButtonPin, INPUT_PULLUP);

    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 10000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };

    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL);

  pinMode(redLedPin, OUTPUT);
  pinMode(greenLedPin, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);
    pinMode(startButtonPin, INPUT_PULLUP);

  setupWiFi();
  loadWelcome();
  loadLanguage();
  eepromLoad();
  loadLastProgram();  // загружаем последнюю программу

  loadSeenVersion();

if (lastSeenVersion != String(FW_VERSION)) {

  showUpdateModal = true;

  saveSeenVersion(String(FW_VERSION));
}


String s, p;

if (loadWiFiCredentials(s, p)) {

savedSSID = s;
savedPASS = p;

  Serial.println("📶 Автоподключение...");

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(s.c_str(), p.c_str());

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 15000) {

    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {

    wifiSetupCompleted = true;

    Serial.println("");
    Serial.println("✅ Wi-Fi подключен");
    Serial.println(WiFi.localIP());
  }
}

  ArduinoOTA.setHostname("ESP32_Controller");
  ArduinoOTA.onStart([]() {
    Serial.println("🔄 OTA старт...");
  });
ArduinoOTA.onEnd([]() {
    Serial.println("✅ OTA ок");
    lastOTAUpdate = String(__DATE__) + " " + String(__TIME__);  // время последнего обновления
});

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("❌ OTA ошибка [%u]\n", error);
  });
  ArduinoOTA.begin();
}

void loop() {

    esp_task_wdt_reset();

    handleButton();
    handleStartButton();

  handleButton();
    handleStartButton();
  handleProgramLogic();
  ArduinoOTA.handle();

dnsServer.processNextRequest();

  WiFiClient client = server.available();
  if (!client) return;

  String request = client.readStringUntil('\r');
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }

  Serial.println("📨 " + request);
  handleRequest(client, request);
  client.stop();
}
