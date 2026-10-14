/*
  Sterownik magazynu RCE - ESP32 (wersja 1: tylko podgląd w monitorze portu szeregowego)

  Co robi:
   - pobiera ceny RCE (PSE, API v2) na dziś i na jutro (15-min -> średnie godzinowe),
   - wyznacza optymalny plan doby dla magazynu o pojemności capH godzin pełnej mocy
     (np. 4 h ładowania = pełny, 4 h rozładowania = pusty): każda godzina to ładowanie,
     rozładowanie albo przerwa; zysk rozładowania = cena * sprawność - próg, koszt ładowania = cena;
     limit godzin ładowania na dobę (domyślnie 8 = dwa pełne cykle),
   - co godzinę wypisuje w Serial: cena -> ŁADUJEMY / ROZŁADOWUJEMY / -,
   - parametry (sprawność, próg, liczba godzin) ustawiane w portalu AP (WiFiManager),
     portal otwierany przyciskiem BOOT (przytrzymaj ~1,5 s) także w trakcie pracy.

  Biblioteki (Menedżer bibliotek):
   - WiFiManager (tzapu)  ~2.0.17
   - ArduinoJson (Benoit Blanchon)  7.x

  Uwaga: przytrzymanie BOOT podczas resetu wprowadza ESP32 w tryb wgrywania.
  Przycisk naciskaj PO starcie (w ciągu 4 s od uruchomienia albo w dowolnym momencie pracy).
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

// ---------- USTAWIENIA STAŁE ----------
#if CONFIG_IDF_TARGET_ESP32C3
  #define BOOT_PIN 9   // ESP32-C3 (np. Super Mini): przycisk BOOT jest na GPIO9
#else
  #define BOOT_PIN 0   // klasyczny ESP32: przycisk BOOT na GPIO0
#endif
static const char*    AP_NAME                  = "RCE-Magazyn";
static const char*    TZ_INFO                  = "CET-1CEST,M3.5.0,M10.5.0/3";  // Polska, auto zmiana czasu
static const int      FETCH_TOMORROW_FROM_HOUR = 13;                            // od tej godziny szukamy cen na jutro
static const uint32_t RETRY_MS                 = 10UL * 60UL * 1000UL;          // ponawianie nieudanego pobrania
static const uint32_t PORTAL_TIMEOUT_S         = 180;
static const char*    PSE_URL_BASE             = "https://api.raporty.pse.pl/api/rce-pln";

// ---------- ALGORYTM (bez zależności od Arduino) ----------
// BEGIN_ALGO
#include <stdint.h>

enum Status : uint8_t { ST_IDLE, ST_CHARGE, ST_DISCHARGE };

struct Config {
  float eff;        // sprawność obiegu [%]
  float thr;        // próg opłacalności [zł/MWh] na każdą rozładowaną godzinę
  int   capH;       // pojemność magazynu w godzinach pełnej mocy (4 = 4 h ładowania / 4 h rozładowania)
  int   maxCharge;  // maks. liczba godzin ładowania na dobę (8 = dwa pełne cykle)
};

struct Plan {
  bool   valid;
  char   date[11];     // YYYY-MM-DD
  int    n;            // liczba godzin w dobie (23, 24 lub 25)
  int    label[26];    // godzina zegarowa
  float  price[26];    // średnia cena godzinowa [zł/MWh]
  Status st[26];
};

static Config cfg = {88.0f, 100.0f, 4, 8};

#define MAXH   26
#define MAXCAP 12
#define MAXCH  25
static float   dpv[MAXH][MAXCAP + 1][MAXCH];
static uint8_t dpa[MAXH][MAXCAP + 1][MAXCH];

// Optymalny plan doby (programowanie dynamiczne).
// Każda godzina: bezczynność / ładowanie pełną mocą / rozładowanie pełną mocą.
// Stan: ile godzin energii jest w magazynie (0..capH) i ile godzin już naładowano.
// Zysk: rozładowanie = cena * sprawność - próg, ładowanie = - cena.
// Magazyn na początku doby traktujemy jako pusty, energia pozostawiona na koniec jest bez wartości.
static void computePlan(Plan &p) {
  const int n = p.n;
  for (int i = 0; i < n; i++) p.st[i] = ST_IDLE;
  if (n < 2) return;

  int cap = cfg.capH;      if (cap < 1) cap = 1;      if (cap > MAXCAP) cap = MAXCAP;
  int mc  = cfg.maxCharge; if (mc < 1) mc = 1;        if (mc > MAXCH - 1) mc = MAXCH - 1;
  const float NEG = -1e30f;
  const float eff = cfg.eff / 100.0f;

  for (int s = 0; s <= cap; s++)
    for (int c = 0; c <= mc; c++) dpv[0][s][c] = NEG;
  dpv[0][0][0] = 0.0f;

  for (int h = 0; h < n; h++) {
    for (int s = 0; s <= cap; s++)
      for (int c = 0; c <= mc; c++) { dpv[h + 1][s][c] = NEG; dpa[h + 1][s][c] = ST_IDLE; }

    for (int s = 0; s <= cap; s++) {
      for (int c = 0; c <= mc; c++) {
        float v = dpv[h][s][c];
        if (v <= NEG / 2) continue;

        // bezczynność
        if (v > dpv[h + 1][s][c]) { dpv[h + 1][s][c] = v; dpa[h + 1][s][c] = ST_IDLE; }

        // ładowanie
        if (s < cap && c < mc) {
          float nv = v - p.price[h];
          if (nv > dpv[h + 1][s + 1][c + 1]) { dpv[h + 1][s + 1][c + 1] = nv; dpa[h + 1][s + 1][c + 1] = ST_CHARGE; }
        }

        // rozładowanie
        if (s > 0) {
          float nv = v + p.price[h] * eff - cfg.thr;
          if (nv > dpv[h + 1][s - 1][c]) { dpv[h + 1][s - 1][c] = nv; dpa[h + 1][s - 1][c] = ST_DISCHARGE; }
        }
      }
    }
  }

  int bs = 0, bc = 0;
  float best = NEG;
  for (int s = 0; s <= cap; s++)
    for (int c = 0; c <= mc; c++)
      if (dpv[n][s][c] > best) { best = dpv[n][s][c]; bs = s; bc = c; }

  int s = bs, c = bc;
  for (int h = n; h >= 1; h--) {
    uint8_t a = dpa[h][s][c];
    p.st[h - 1] = (Status)a;
    if (a == ST_CHARGE) { s--; c--; }
    else if (a == ST_DISCHARGE) { s++; }
  }
}
// END_ALGO

// ---------- WYGLĄD PORTALU AP (microbms) ----------
// Ciemny motyw + gradient z logo: różowy #C80356 -> fiolet #771E79.
// Logo (PNG 200x114, ~4 KB) jest wkompilowane jako base64, bo w trybie AP nie ma internetu.
static const char PORTAL_HEAD[] PROGMEM =
  "<style>"
  "body{background:#14101c!important;color:#ece6f2!important;}"
  ".wrap{padding:0 12px;}"
  ".wrap:before{content:'';display:block;width:200px;height:114px;margin:16px auto 6px;background:url(data:image/png;base64,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) center/contain no-repeat;}"
  "h1,h2,h3{color:#ece6f2!important;text-align:center;}"
  "h1{letter-spacing:.06em;}"
  "div,label,td,span,dt,dd{color:#ece6f2;}"
  "a{color:#e0559a!important;}"
  "button,input[type=submit]{background:linear-gradient(90deg,#c80356,#771e79)!important;color:#fff!important;border:0!important;border-radius:.5rem!important;}"
  "button.D{background:#3a2447!important;}"
  "input,select{background:#1e1829!important;color:#ece6f2!important;border:1px solid #5a2a6e!important;border-radius:.4rem!important;}"
  "input:focus{outline:2px solid #c80356;}"
  ".msg{background:#1e1829!important;border-color:#c80356!important;color:#ece6f2!important;}"
  ".q{color:#ece6f2;}"
  "</style>";

// ---------- STAN ----------
static Plan planToday    = {};
static Plan planTomorrow = {};
static Preferences prefs;
static WiFiManager wm;
static bool cfgChanged = false;
static int  lastShownHour = -1;
static uint32_t lastTryToday = 0, lastTryTomorrow = 0;

// parametry portalu (bufory muszą żyć cały czas)
static WiFiManagerParameter pEff("eff", "Sprawność obiegu [%]", "88", 8, "type='number' step='any'");
static WiFiManagerParameter pThr("thr", "Próg opłacalności [zł/MWh]", "100", 8, "type='number' step='any'");
static WiFiManagerParameter pCap("cap", "Pojemność magazynu [h pełnej mocy]", "4", 4, "type='number' min='1' max='12'");
static WiFiManagerParameter pMc ("mc",  "Maks. godzin ładowania na dobę", "8", 4, "type='number' min='1' max='24'");

// ---------- KONFIGURACJA (NVS) ----------
static void loadConfig() {
  prefs.begin("rce", true);
  cfg.eff        = prefs.getFloat("eff", 88.0f);
  cfg.thr        = prefs.getFloat("thr", 100.0f);
  cfg.capH       = prefs.getInt("cap", 4);
  cfg.maxCharge  = prefs.getInt("mc", 8);
  prefs.end();
}

static void saveConfig() {
  prefs.begin("rce", false);
  prefs.putFloat("eff", cfg.eff);
  prefs.putFloat("thr", cfg.thr);
  prefs.putInt("cap", cfg.capH);
  prefs.putInt("mc", cfg.maxCharge);
  prefs.end();
}

static void fillPortalDefaults() {
  char b[16];
  snprintf(b, sizeof b, "%.1f", cfg.eff);       pEff.setValue(b, 8);
  snprintf(b, sizeof b, "%.1f", cfg.thr);       pThr.setValue(b, 8);
  snprintf(b, sizeof b, "%d", cfg.capH);        pCap.setValue(b, 4);
  snprintf(b, sizeof b, "%d", cfg.maxCharge);   pMc.setValue(b, 4);
}

static void saveParamsCb() {
  float eff = atof(pEff.getValue());
  float thr = atof(pThr.getValue());
  int   cap = atoi(pCap.getValue());
  int   mc  = atoi(pMc.getValue());
  if (eff < 1.0f) eff = 1.0f;
  if (eff > 100.0f) eff = 100.0f;
  if (cap < 1) cap = 1;
  if (cap > 12) cap = 12;
  if (mc < 1) mc = 1;
  if (mc > 24) mc = 24;
  cfg.eff = eff; cfg.thr = thr; cfg.capH = cap; cfg.maxCharge = mc;
  saveConfig();
  cfgChanged = true;
  Serial.println("Zapisano nowe parametry.");
}

// ---------- WYŚWIETLANIE ----------
static const char* statusText(Status s) {
  switch (s) {
    case ST_CHARGE:    return "ŁADUJEMY";
    case ST_DISCHARGE: return "ROZŁADOWUJEMY";
    default:           return "-";
  }
}

static void printPlan(const Plan &p, const char *title) {
  Serial.printf("\n=== %s %s ===\n", title, p.date);
  Serial.println("godz   cena [zł/MWh]   akcja");
  int nc = 0, nd = 0;
  for (int h = 0; h < p.n; h++) {
    Serial.printf(" %02d    %10.2f      %s\n", p.label[h], p.price[h], statusText(p.st[h]));
    if (p.st[h] == ST_CHARGE) nc++;
    if (p.st[h] == ST_DISCHARGE) nd++;
  }
  Serial.printf("Ładowanie: %d h, rozładowanie: %d h | sprawność %.1f%%, próg %.1f zł/MWh, pojemność %d h, maks. ładowanie %d h\n",
                nc, nd, cfg.eff, cfg.thr, cfg.capH, cfg.maxCharge);
}

static void showNow(const struct tm &t) {
  if (!planToday.valid) return;
  for (int h = 0; h < planToday.n; h++) {
    if (planToday.label[h] == t.tm_hour) {
      Serial.printf("[%02d:%02d] cena %.2f zł/MWh -> %s\n",
                    t.tm_hour, t.tm_min, planToday.price[h], statusText(planToday.st[h]));
      return;
    }
  }
}

// ---------- POBIERANIE CEN ----------
static bool fetchDay(const char *date, Plan &p) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(PSE_URL_BASE) + "?$filter=business_date%20eq%20%27" + date + "%27&$select=period,rce_pln";
  WiFiClientSecure client;
  client.setInsecure();   // dane publiczne, tylko do odczytu; bez przypinania certyfikatu
  HTTPClient http;
  http.useHTTP10(true);   // bez chunked, żeby strumień dało się parsować wprost
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  Serial.printf("PSE %s -> HTTP %d\n", date, code);
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  filter["value"][0]["period"]  = true;
  filter["value"][0]["rce_pln"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("Błąd JSON: %s\n", err.c_str()); return false; }

  JsonArray arr = doc["value"].as<JsonArray>();
  int cnt = arr.size();
  // doba = 92 / 96 / 100 kwadransów (zmiana czasu); rekordy przychodzą rosnąco w czasie
  if (cnt < 92 || cnt > 100 || (cnt % 4) != 0) {
    Serial.printf("Dane niekompletne lub jeszcze nieopublikowane (%d rekordów)\n", cnt);
    return false;
  }

  Plan np = {};
  np.n = cnt / 4;
  for (int h = 0; h < np.n; h++) {
    float sum = 0;
    for (int q = 0; q < 4; q++) sum += arr[h * 4 + q]["rce_pln"].as<float>();
    np.price[h] = sum / 4.0f;
    np.label[h] = atoi(arr[h * 4]["period"].as<const char *>());   // "HH:MM - HH:MM" -> HH
  }
  strlcpy(np.date, date, sizeof np.date);
  np.valid = true;
  computePlan(np);
  p = np;
  return true;
}

// ---------- CZAS ----------
static void syncTime() {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  Serial.print("Synchronizacja czasu");
  uint32_t s = millis();
  while (time(nullptr) < 1700000000 && millis() - s < 20000) { delay(250); Serial.print('.'); }
  Serial.println();
}

static void tomorrowStr(struct tm t, char *out) {
  t.tm_mday += 1;
  t.tm_isdst = -1;
  mktime(&t);
  strftime(out, 11, "%Y-%m-%d", &t);
}

static bool due(uint32_t &last, uint32_t interval) {
  if (last == 0 || millis() - last >= interval) { last = millis() | 1; return true; }
  return false;
}

// ---------- PORTAL AP ----------
static void openPortal() {
  Serial.printf("\nPortal konfiguracyjny: sieć Wi-Fi \"%s\", adres 192.168.4.1 (%u s)\n", AP_NAME, (unsigned)PORTAL_TIMEOUT_S);
  fillPortalDefaults();
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.startConfigPortal(AP_NAME);   // blokuje do zapisu/timeoutu

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.begin();   // zapisane dane Wi-Fi
    uint32_t s = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - s < 15000) delay(250);
  }
  if (cfgChanged) {
    cfgChanged = false;
    if (planToday.valid)    { computePlan(planToday);    printPlan(planToday, "Plan na dzis (nowe parametry)"); }
    if (planTomorrow.valid) { computePlan(planTomorrow); printPlan(planTomorrow, "Plan na jutro (nowe parametry)"); }
  }
  lastShownHour = -1;
}

static void handleBootButton() {
  static uint32_t since = 0;
  if (digitalRead(BOOT_PIN) == LOW) {
    if (!since) since = millis() | 1;
    else if (millis() - since > 1500) { since = 0; openPortal(); }
  } else {
    since = 0;
  }
}

static bool bootPressedAtStart(uint32_t windowMs) {
  uint32_t t0 = millis(), low = 0;
  while (millis() - t0 < windowMs) {
    if (digitalRead(BOOT_PIN) == LOW) {
      if (!low) low = millis() | 1;
      if (millis() - low > 1500) return true;
    } else {
      low = 0;
    }
    delay(20);
  }
  return false;
}

// ---------- SETUP / LOOP ----------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // natywne USB (C3): czekamy na monitor
  delay(500);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  loadConfig();

  wm.addParameter(&pEff);
  wm.addParameter(&pThr);
  wm.addParameter(&pCap);
  wm.addParameter(&pMc);
  wm.setSaveParamsCallback(saveParamsCb);
  wm.setTitle("microbms");
  wm.setCustomHeadElement(PORTAL_HEAD);
  fillPortalDefaults();

  WiFi.mode(WIFI_STA);
  Serial.println("\nSterownik magazynu RCE");
  Serial.println("Przytrzymaj BOOT ok. 2 s w ciągu 4 s, aby otworzyć konfigurację...");

  if (bootPressedAtStart(4000)) {
    openPortal();
  } else {
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
    if (!wm.autoConnect(AP_NAME)) {   // bez zapisanej sieci otwiera portal
      Serial.println("Brak połączenia z Wi-Fi - restart");
      delay(1000);
      ESP.restart();
    }
  }
  if (WiFi.status() != WL_CONNECTED) { delay(1000); ESP.restart(); }

  Serial.printf("Wi-Fi OK, IP: %s\n", WiFi.localIP().toString().c_str());
  syncTime();
}

void loop() {
  handleBootButton();

  time_t now = time(nullptr);
  if (now < 1700000000) { delay(500); return; }   // czas jeszcze niezsynchronizowany
  struct tm t;
  localtime_r(&now, &t);
  char today[11], tmr[11];
  strftime(today, sizeof today, "%Y-%m-%d", &t);
  tomorrowStr(t, tmr);

  // zmiana doby: jutrzejszy plan staje się dzisiejszym
  if (planToday.valid && strcmp(planToday.date, today) != 0) {
    if (planTomorrow.valid && strcmp(planTomorrow.date, today) == 0) {
      planToday = planTomorrow;
      planTomorrow.valid = false;
      printPlan(planToday, "Plan na dzis");
    } else {
      planToday.valid = false;
    }
    lastShownHour = -1;
  }

  // plan na dziś (przy starcie lub po awarii)
  if (!planToday.valid && due(lastTryToday, RETRY_MS / 10)) {   // co ok. minutę
    if (fetchDay(today, planToday)) {
      printPlan(planToday, "Plan na dzis");
      lastShownHour = -1;
    }
  }

  // plan na jutro: od 13:00, ponawiamy co 10 min aż się uda
  if (!planTomorrow.valid && t.tm_hour >= FETCH_TOMORROW_FROM_HOUR && due(lastTryTomorrow, RETRY_MS)) {
    if (fetchDay(tmr, planTomorrow)) printPlan(planTomorrow, "Plan na jutro");
  }

  // status co godzinę
  if (planToday.valid && t.tm_hour != lastShownHour) {
    lastShownHour = t.tm_hour;
    showNow(t);
  }

  delay(200);
}
