/*
  Crypto Ticker — ESP8266 + LCD1602/2004 I2C
  Shows a Binance spot price (USDT pair) with 24h change on a character LCD.
  WiFi and coin are configured from a built-in web page.

  https://github.com/ArtemDNK/Crypto_ticker_esp8266
*/

// ArduinoJson must come before ESPAsyncWebServer (it checks __has_include)
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecure.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>
#include "web_page.h"
#include "lcd_compat.h"

// ====================== Configuration ======================
#define FW_VERSION "4.0"

static const char AP_SSID[]   = "CryptoTicker";
static const char AP_PASS[]   = "12345678";      // >= 8 chars, change it!
static const char HOSTNAME[]  = "cryptoticker";  // http://cryptoticker.local

// Market-data-only mirror first (lighter, not geo-blocked as often), then main API.
static const char* const API_HOSTS[] = { "data-api.binance.vision", "api.binance.com" };
constexpr uint8_t  API_HOST_COUNT        = sizeof(API_HOSTS) / sizeof(API_HOSTS[0]);

constexpr uint32_t PRICE_INTERVAL_MS     = 5000;    // between price requests
constexpr uint32_t STA_CONNECT_TIMEOUT   = 15000;   // initial connect attempt
constexpr uint32_t STA_LOST_TO_PORTAL_MS = 60000;   // WiFi lost this long -> open setup AP
constexpr uint32_t PORTAL_RETRY_MS       = 300000;  // in setup AP: retry saved WiFi every 5 min
constexpr uint8_t  FAILS_BEFORE_ALERT    = 3;       // failed requests before "!" icon
constexpr uint8_t  LCD_COLS = 16;
constexpr uint8_t  LCD_ROWS = 2;

// ====================== Settings (EEPROM) ======================
// Layout is identical to v3.x, so saved WiFi/coin survive the update.
struct Settings {
  char ssid[33];
  char password[65];
  char symbol[20];
  char displayName[10];
  char marker;
};
constexpr char SETTINGS_MARKER = 'V';
Settings settings;

// ====================== Globals ======================
LiquidCrystal_I2C* lcd = nullptr;
AsyncWebServer server(80);
BearSSL::WiFiClientSecure tls;

enum class NetMode : uint8_t { STA, PORTAL };
NetMode  netMode       = NetMode::STA;
bool     staConnected  = false;
uint32_t staLostAt     = 0;
bool     portalTrying  = false;
uint32_t portalTryAt   = 0;
uint32_t portalRetryAt = 0;

// Price state
double   lastPrice  = NAN;
double   openPrice  = NAN;
int8_t   trend      = 0;       // -1 down, 0 same, 1 up
uint8_t  failCount  = 0;
char     lastError[LCD_COLS + 1] = "";
uint8_t  apiHost    = 0;
int8_t   mfln       = -1;      // -1 unknown, 0 no, 1 server supports 1 KB TLS fragments
uint32_t nextFetchAt = 0;

// Requests from web handlers. Handlers run in the network (SYS) context,
// so they only leave a note here; the actual work (I2C, flash) happens in loop().
volatile bool reqSymbol = false;
volatile bool reqWifi   = false;
char pendingSymbol[sizeof(Settings::symbol)];
char pendingSsid[sizeof(Settings::ssid)];
char pendingPass[sizeof(Settings::password)];
uint32_t restartAt = 0;

// LCD glyphs
const uint8_t GLYPH_UP   = 0, GLYPH_DOWN = 1, GLYPH_WIFI = 2;
uint8_t arrowUp[8]   = {0b00100,0b01110,0b10101,0b00100,0b00100,0b00100,0b00100,0b00000};
uint8_t arrowDown[8] = {0b00100,0b00100,0b00100,0b00100,0b10101,0b01110,0b00100,0b00000};
uint8_t wifiIcon[8]  = {0b00000,0b01110,0b10001,0b00100,0b01010,0b00000,0b00100,0b00000};

// ====================== Helpers ======================
static inline bool timeReached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

uint8_t detectLcdAddress() {
  static const uint8_t candidates[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                                       0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};
  Wire.begin();  // SDA = GPIO4 (D2), SCL = GPIO5 (D1)
  for (uint8_t a : candidates) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0x27;
}

void lcdRow(uint8_t row, const char* text) {
  char buf[LCD_COLS + 1];
  snprintf(buf, sizeof(buf), "%-16.16s", text);
  lcd->setCursor(0, row);
  lcd->print(buf);
}

void lcdShow(const char* l1, const char* l2) {
  lcdRow(0, l1);
  lcdRow(1, l2);
}

// "BTCUSDT" -> "BTC" (max 9 chars, so "XXXXXXXXX/USDT" + arrow + icon fit 16 cols)
void deriveDisplayName() {
  size_t len = strlen(settings.symbol);
  if (len > 4 && strcmp(settings.symbol + len - 4, "USDT") == 0) len -= 4;
  if (len > sizeof(settings.displayName) - 1) len = sizeof(settings.displayName) - 1;
  memcpy(settings.displayName, settings.symbol, len);
  settings.displayName[len] = '\0';
}

bool validSymbol(const char* s) {
  size_t len = strlen(s);
  if (len < 5 || len >= sizeof(settings.symbol)) return false;
  for (size_t i = 0; i < len; i++)
    if (!isupper((unsigned char)s[i]) && !isdigit((unsigned char)s[i])) return false;
  return true;
}

void formatPrice(double p, char* out, size_t n) {
  int dec = p >= 10000 ? 0 : p >= 1000 ? 1 : p >= 1 ? 2 : p >= 0.01 ? 4 : p >= 0.0001 ? 6 : 8;
  snprintf(out, n, "%.*f", dec, p);
}

// Returns false if there is no open price to compare with.
bool formatChange(char* out, size_t n) {
  if (!(openPrice > 0) || isnan(lastPrice)) { out[0] = '\0'; return false; }
  double ch = (lastPrice - openPrice) / openPrice * 100.0;
  double a = fabs(ch);
  int dec = a < 10 ? 2 : a < 100 ? 1 : 0;
  snprintf(out, n, "%+.*f%%", dec, ch);
  return true;
}

// ====================== Settings ======================
void loadSettings() {
  EEPROM.get(0, settings);
  // Force termination in case of garbage
  settings.ssid[sizeof(settings.ssid) - 1] = '\0';
  settings.password[sizeof(settings.password) - 1] = '\0';
  settings.symbol[sizeof(settings.symbol) - 1] = '\0';

  if (settings.marker != SETTINGS_MARKER) {
    memset(&settings, 0, sizeof(settings));
    settings.marker = SETTINGS_MARKER;
  }
  if (!validSymbol(settings.symbol)) strlcpy(settings.symbol, "BTCUSDT", sizeof(settings.symbol));
  deriveDisplayName();
  Serial.printf_P(PSTR("Settings: ssid='%s' symbol=%s\n"), settings.ssid, settings.symbol);
}

void saveSettings() {
  settings.marker = SETTINGS_MARKER;
  deriveDisplayName();
  EEPROM.put(0, settings);
  EEPROM.commit();
  Serial.println(F("Settings saved"));
}

// ====================== Display ======================
void drawTicker() {
  char buf[LCD_COLS + 1];

  // Row 0: "BTC/USDT      ↑W"
  snprintf(buf, sizeof(buf), "%s/USDT", settings.displayName);
  lcdRow(0, buf);
  lcd->setCursor(LCD_COLS - 2, 0);
  if (trend > 0)      lcd->write(GLYPH_UP);
  else if (trend < 0) lcd->write(GLYPH_DOWN);
  else                lcd->print('-');
  lcd->setCursor(LCD_COLS - 1, 0);
  if (failCount >= FAILS_BEFORE_ALERT) lcd->print('!');
  else                                 lcd->write(GLYPH_WIFI);

  // Row 1: "$67123     +1.23%"
  if (isnan(lastPrice)) {
    lcdRow(1, lastError[0] ? lastError : "Loading...");
    return;
  }
  char price[LCD_COLS + 1], change[10];
  price[0] = '$';
  formatPrice(lastPrice, price + 1, sizeof(price) - 1);
  size_t pl = strlen(price);
  if (formatChange(change, sizeof(change)) && pl + 1 + strlen(change) <= LCD_COLS) {
    snprintf(buf, sizeof(buf), "%s%*s", price, (int)(LCD_COLS - pl), change);
  } else {
    strlcpy(buf, price, sizeof(buf));
  }
  lcdRow(1, buf);
}

void drawPortal() {
  lcdShow("Setup WiFi:", "192.168.4.1");
}

// ====================== WiFi ======================
bool connectStaBlocking() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(settings.ssid, settings.password);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_CONNECT_TIMEOUT) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

void startPortal() {
  WiFi.mode(WIFI_AP_STA);  // STA part is needed for network scanning
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.disconnect();       // stop STA retries: they hop channels and break the AP
  netMode       = NetMode::PORTAL;
  staConnected  = false;
  portalTrying  = false;
  portalRetryAt = millis() + PORTAL_RETRY_MS;
  drawPortal();
  Serial.printf_P(PSTR("Setup AP '%s' at %s\n"), AP_SSID, WiFi.softAPIP().toString().c_str());
}

void stopPortal() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  netMode      = NetMode::STA;
  staConnected = true;
  nextFetchAt  = millis();
  Serial.printf_P(PSTR("WiFi back: %s\n"), WiFi.localIP().toString().c_str());
  drawTicker();
}

void handleWifi() {
  bool up = WiFi.status() == WL_CONNECTED;
  uint32_t now = millis();

  if (netMode == NetMode::STA) {
    if (up && !staConnected) {
      staConnected = true;
      nextFetchAt  = now;
      Serial.println(F("WiFi reconnected"));
    } else if (!up && staConnected) {
      staConnected = false;
      staLostAt    = now;
      lcdRow(1, "WiFi lost...");
      Serial.println(F("WiFi lost"));
    } else if (!up && now - staLostAt >= STA_LOST_TO_PORTAL_MS) {
      startPortal();
    }
    return;
  }

  // Setup portal: occasionally retry saved network, but only when nobody is
  // connected to the AP (the STA scan would drop their connection).
  if (portalTrying) {
    if (up) {
      stopPortal();
    } else if (now - portalTryAt >= STA_CONNECT_TIMEOUT) {
      WiFi.disconnect();
      portalTrying  = false;
      portalRetryAt = now + PORTAL_RETRY_MS;
      drawPortal();
    }
  } else if (settings.ssid[0] && timeReached(portalRetryAt)) {
    if (WiFi.softAPgetStationNum() == 0) {
      Serial.println(F("Retrying saved WiFi"));
      WiFi.begin(settings.ssid, settings.password);
      portalTrying = true;
      portalTryAt  = now;
      lcdRow(1, "Retry WiFi...");
    } else {
      portalRetryAt = now + 30000;
    }
  }
}

// ====================== Binance ======================
// Synchronous request with short timeouts; the async web server keeps working
// meanwhile because WiFiClient waits with yield().
// httpCode: 0 = connection failed, -1 = bad response, otherwise HTTP status.
bool fetchTicker(const char* host, double& last, double& open, int& httpCode) {
  httpCode = 0;

  // A 1 KB receive buffer instead of the default 16 KB saves ~15 KB of heap,
  // but only if the server supports TLS Max Fragment Length. Probe once per host.
  if (mfln < 0) {
    mfln = BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 1024) ? 1 : 0;
    Serial.printf_P(PSTR("%s MFLN: %s\n"), host, mfln ? "yes" : "no");
  }
  tls.setInsecure();  // public market data, no secrets sent
  tls.setTimeout(5000);
  if (mfln) tls.setBufferSizes(1024, 512);
  else      tls.setBufferSizes(16384, 512);

  if (!tls.connect(host, 443)) return false;

  // HTTP/1.0 => no chunked encoding, plain body we can stream into the parser.
  // type=MINI returns a small object with lastPrice and openPrice (24h ago).
  char req[200];
  int len = snprintf_P(req, sizeof(req),
      PSTR("GET /api/v3/ticker/24hr?type=MINI&symbol=%s HTTP/1.0\r\n"
           "Host: %s\r\nUser-Agent: CryptoTicker/" FW_VERSION "\r\nConnection: close\r\n\r\n"),
      settings.symbol, host);
  tls.write((const uint8_t*)req, len);

  char line[40];
  size_t n = tls.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n] = '\0';
  const char* sp = strchr(line, ' ');
  httpCode = sp ? atoi(sp + 1) : -1;
  if (httpCode <= 0) httpCode = -1;

  if (httpCode != 200 || !tls.find("\r\n\r\n")) {
    tls.stop();
    return false;
  }

  JsonDocument filter;
  filter["lastPrice"] = true;
  filter["openPrice"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, tls, DeserializationOption::Filter(filter));
  tls.stop();
  if (err) { httpCode = -1; return false; }

  const char* lp = doc["lastPrice"];
  const char* op = doc["openPrice"];
  if (!lp) { httpCode = -1; return false; }
  last = strtod(lp, nullptr);
  open = op ? strtod(op, nullptr) : NAN;
  return last > 0;
}

void updatePrice() {
  double last = NAN, open = NAN;
  int code;
  bool ok = fetchTicker(API_HOSTS[apiHost], last, open, code);

  if (ok) {
    trend     = isnan(lastPrice) ? 0 : (last > lastPrice ? 1 : (last < lastPrice ? -1 : 0));
    lastPrice = last;
    openPrice = open;
    failCount = 0;
    lastError[0] = '\0';
    char p[20];
    formatPrice(last, p, sizeof(p));
    Serial.printf_P(PSTR("%s: $%s (heap %u)\n"), settings.symbol, p, ESP.getFreeHeap());
  } else {
    if (failCount < 255) failCount++;
    if (code == 400)      strlcpy(lastError, "Unknown symbol", sizeof(lastError));
    else if (code == 451 || code == 403)
                          snprintf(lastError, sizeof(lastError), "Blocked: %d", code);
    else if (code > 0)    snprintf(lastError, sizeof(lastError), "HTTP error %d", code);
    else if (code == 0)   strlcpy(lastError, "No connection", sizeof(lastError));
    else                  strlcpy(lastError, "Bad response", sizeof(lastError));
    Serial.printf_P(PSTR("Fetch failed (%s): %s\n"), API_HOSTS[apiHost], lastError);

    // Host unreachable or geo-blocked twice in a row -> try the other host.
    if ((code == 0 || code == 403 || code == 451) && failCount % 2 == 0) {
      apiHost = (apiHost + 1) % API_HOST_COUNT;
      mfln = -1;
      Serial.printf_P(PSTR("Switching to %s\n"), API_HOSTS[apiHost]);
    }
  }
  drawTicker();
}

// ====================== Web server ======================
void sendJson(AsyncWebServerRequest* request, JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  request->send(code, "application/json", out);
}

void sendOk(AsyncWebServerRequest* request, bool ok) {
  request->send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send_P(200, "text/html", HTML_PAGE);
  });

  // GET /scan?start=1 starts a new async scan; GET /scan polls the result.
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    int n = WiFi.scanComplete();
    if (request->hasParam("start") || n == WIFI_SCAN_FAILED) {
      if (n != WIFI_SCAN_RUNNING) {
        WiFi.scanDelete();
        WiFi.scanNetworks(true, false);
      }
      request->send(200, "application/json", "{\"st\":\"scan\"}");
      return;
    }
    if (n == WIFI_SCAN_RUNNING) {
      request->send(200, "application/json", "{\"st\":\"scan\"}");
      return;
    }

    JsonDocument doc;
    doc["st"] = "done";
    JsonArray arr = doc["n"].to<JsonArray>();
    // Sort indices by RSSI, drop hidden networks and duplicates (mesh / repeaters).
    int idx[48];
    int cnt = 0;
    for (int i = 0; i < n && cnt < 48; i++) {
      if (WiFi.SSID(i).length() == 0) continue;
      bool dup = false;
      for (int j = 0; j < cnt && !dup; j++) {
        if (WiFi.SSID(idx[j]) == WiFi.SSID(i)) {
          dup = true;
          if (WiFi.RSSI(i) > WiFi.RSSI(idx[j])) idx[j] = i;
        }
      }
      if (!dup) idx[cnt++] = i;
    }
    for (int a = 1; a < cnt; a++)
      for (int b = a; b > 0 && WiFi.RSSI(idx[b]) > WiFi.RSSI(idx[b - 1]); b--) {
        int t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t;
      }
    for (int k = 0; k < cnt && k < 20; k++) {
      JsonObject o = arr.add<JsonObject>();
      o["s"] = WiFi.SSID(idx[k]);
      o["r"] = WiFi.RSSI(idx[k]);
      o["e"] = WiFi.encryptionType(idx[k]) != ENC_TYPE_NONE;
    }
    WiFi.scanDelete();
    sendJson(request, doc);
  });

  // Change coin (no restart)
  server.on("/sc", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("symbol", true)) return sendOk(request, false);
    String sym = request->getParam("symbol", true)->value();
    sym.trim();
    sym.toUpperCase();
    if (!validSymbol(sym.c_str())) return sendOk(request, false);
    strlcpy(pendingSymbol, sym.c_str(), sizeof(pendingSymbol));
    reqSymbol = true;
    sendOk(request, true);
  });

  // Save WiFi credentials (restart)
  server.on("/sw", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true)) return sendOk(request, false);
    String s = request->getParam("ssid", true)->value();
    String p = request->hasParam("password", true) ? request->getParam("password", true)->value() : String();
    if (s.length() == 0 || s.length() > 32 || p.length() > 64 || (p.length() > 0 && p.length() < 8))
      return sendOk(request, false);
    strlcpy(pendingSsid, s.c_str(), sizeof(pendingSsid));
    strlcpy(pendingPass, p.c_str(), sizeof(pendingPass));
    reqWifi = true;
    sendOk(request, true);
  });

  // Status for the web page
  server.on("/st", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    bool c = netMode == NetMode::STA && staConnected;
    doc["c"]  = c;
    doc["s"]  = c ? WiFi.SSID() : String();
    doc["i"]  = c ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["y"]  = settings.symbol;
    doc["n"]  = settings.displayName;
    doc["ss"] = settings.ssid;
    if (!isnan(lastPrice)) {
      char p[20], ch[10];
      formatPrice(lastPrice, p, sizeof(p));
      doc["p"] = p;
      if (formatChange(ch, sizeof(ch))) doc["ch"] = ch;
    }
    if (lastError[0]) doc["err"] = lastError;
    doc["v"] = FW_VERSION;
    doc["h"] = ESP.getFreeHeap();
    sendJson(request, doc);
  });

  server.onNotFound([](AsyncWebServerRequest* request) {
    // Captive-portal-ish: anything unknown goes to the main page
    request->redirect("/");
  });
}

void handleWebRequests() {
  if (reqSymbol) {
    reqSymbol = false;
    strlcpy(settings.symbol, pendingSymbol, sizeof(settings.symbol));
    saveSettings();
    lastPrice = openPrice = NAN;
    trend = 0;
    failCount = 0;
    lastError[0] = '\0';
    nextFetchAt = millis();
    if (netMode == NetMode::STA) drawTicker();
    Serial.printf_P(PSTR("Symbol -> %s\n"), settings.symbol);
  }
  if (reqWifi) {
    reqWifi = false;
    strlcpy(settings.ssid, pendingSsid, sizeof(settings.ssid));
    strlcpy(settings.password, pendingPass, sizeof(settings.password));
    saveSettings();
    lcdShow("WiFi saved", "Restarting...");
    restartAt = millis() + 1000;  // let the HTTP response go out first
  }
}

// ====================== Arduino ======================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== Crypto Ticker v" FW_VERSION " ==="));

  EEPROM.begin(sizeof(Settings));
  loadSettings();

  uint8_t addr = detectLcdAddress();
  lcd = new LiquidCrystal_I2C(addr, LCD_COLS, LCD_ROWS);
  lcdStart(*lcd, 0);
  lcd->backlight();
  lcd->createChar(GLYPH_UP, arrowUp);
  lcd->createChar(GLYPH_DOWN, arrowDown);
  lcd->createChar(GLYPH_WIFI, wifiIcon);
  Serial.printf_P(PSTR("LCD at 0x%02X\n"), addr);
  lcdShow("Crypto Ticker", "v" FW_VERSION);

  WiFi.persistent(false);       // credentials live in our EEPROM, don't wear SDK flash
  WiFi.setAutoReconnect(true);
  WiFi.hostname(HOSTNAME);

  if (settings.ssid[0]) {
    lcdShow("Connecting...", settings.ssid);
    if (connectStaBlocking()) {
      staConnected = true;
      Serial.printf_P(PSTR("Connected: %s\n"), WiFi.localIP().toString().c_str());
      lcdShow("WiFi OK", WiFi.localIP().toString().c_str());
      delay(1500);
      drawTicker();
    } else {
      startPortal();
    }
  } else {
    startPortal();
  }

  setupWebServer();
  server.begin();
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  Serial.println(F("Web server started"));
}

void loop() {
  handleWebRequests();
  if (restartAt && timeReached(restartAt)) ESP.restart();

  MDNS.update();
  handleWifi();

  if (netMode == NetMode::STA && staConnected && timeReached(nextFetchAt)) {
    updatePrice();
    nextFetchAt = millis() + PRICE_INTERVAL_MS;
  }
}
