/*
 * Ночник «Заснул или нет» — мини-проект модуля 1, ВРИИ
 *
 * Sense : радар HLK-LD2410C (присутствие + энергия движения), фоторезистор (темно / светло)
 * Think : скользящее среднее энергии движения + пороги с гистерезисом,
 *         конечный автомат из 4 состояний: ПУСТО, НЕ_СПИТ, ЗАСЫПАЕТ, СПИТ
 * Act   : светодиодная лента WS2812, тёплый свет с плавным затуханием
 *
 * Плата: ESP32 DevKit. Библиотеки: "ld2410" (ncmreynolds), "Adafruit NeoPixel".
 */

#include <ld2410.h>
#include <Adafruit_NeoPixel.h>

// ---------- Пины ----------
const int PIN_RADAR_RX = 16;   // ESP32 RX2  <- TX радара
const int PIN_RADAR_TX = 17;   // ESP32 TX2  -> RX радара
const int PIN_LED      = 5;    // DIN ленты WS2812
const int PIN_LIGHT    = 34;   // фоторезистор (делитель с 10 кОм), только вход АЦП
const int NUM_LEDS     = 15;

// ---------- Режим для видео ----------
// true  — короткие таймеры, чтобы всё показать за 1 минуту видео
// false — реальные таймеры для ночи
const bool DEMO_MODE = true;

const unsigned long T_STILL  = DEMO_MODE ? 15000UL : 10UL * 60000UL; // неподвижен -> ЗАСЫПАЕТ
const unsigned long T_FADE   = DEMO_MODE ? 10000UL :  5UL * 60000UL; // время затухания
const unsigned long T_EMPTY  = DEMO_MODE ?  5000UL : 30000UL;        // нет человека -> ПУСТО

// ---------- Обработка ----------
const int   WIN = 10;           // окно скользящего среднего (10 отсчётов * 200 мс = 2 с)
const int   MOVE_ON  = 35;      // выше -> «двигается»      (гистерезис:
const int   MOVE_OFF = 15;      // ниже -> «не двигается»    между 15 и 35 ничего не меняем)
const int   DARK_ON  = 1200;    // АЦП ниже -> «темно»  (подобрать замером!)
const int   DARK_OFF = 1800;    // АЦП выше -> «светло»
const unsigned long SAMPLE_MS = 200;

const uint8_t BRIGHT_AWAKE = 80;  // яркость в состоянии НЕ_СПИТ (из 255)

ld2410 radar;
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);

enum State { EMPTY, AWAKE, FALLING_ASLEEP, ASLEEP };
const char* stateName[] = {"ПУСТО", "НЕ_СПИТ", "ЗАСЫПАЕТ", "СПИТ"};
State state = EMPTY;

int  buf[WIN];
int  bufIdx = 0;
long bufSum = 0;

bool moving = false;            // результат гистерезиса по движению
bool dark   = false;            // результат гистерезиса по свету

unsigned long stateSince = 0;   // когда вошли в текущее состояние
unsigned long stillSince = 0;   // с какого момента человек неподвижен
unsigned long lastSeen   = 0;   // когда радар последний раз видел человека
unsigned long lastSample = 0;

// --- скользящее среднее ---
int movingAverage(int x) {
  bufSum -= buf[bufIdx];
  buf[bufIdx] = x;
  bufSum += x;
  bufIdx = (bufIdx + 1) % WIN;
  return bufSum / WIN;
}

void setLight(uint8_t brightness) {
  // тёплый цвет; если светло — ночник не нужен
  uint8_t b = dark ? brightness : 0;
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(b, b * 45 / 100, b / 10));
  strip.show();
}

void go(State s, unsigned long now) {
  Serial.printf("[%lu] %s -> %s\n", now / 1000, stateName[state], stateName[s]);
  state = s;
  stateSince = now;
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(256000, SERIAL_8N1, PIN_RADAR_RX, PIN_RADAR_TX);
  radar.begin(Serial2);
  strip.begin();
  strip.show();
  for (int i = 0; i < WIN; i++) buf[i] = 0;
  Serial.println("Ночник запущен");
}

void loop() {
  radar.read();                       // радар шлёт кадры постоянно, читаем как можно чаще
  unsigned long now = millis();
  if (now - lastSample < SAMPLE_MS) return;
  lastSample = now;

  // ===== SENSE =====
  bool present = radar.presenceDetected();
  int  rawMove = radar.movingTargetDetected() ? radar.movingTargetEnergy() : 0;
  int  light   = analogRead(PIN_LIGHT);

  // ===== THINK: обработка =====
  int avgMove = movingAverage(rawMove);

  if (!moving && avgMove > MOVE_ON)  moving = true;     // гистерезис по движению
  if ( moving && avgMove < MOVE_OFF) moving = false;
  if (!dark && light < DARK_ON)  dark = true;            // гистерезис по свету
  if ( dark && light > DARK_OFF) dark = false;

  if (present) lastSeen = now;
  if (moving)  stillSince = now;                         // таймер неподвижности сбрасывается
  bool gone = (now - lastSeen > T_EMPTY);
  unsigned long stillFor = now - stillSince;

  // ===== THINK: конечный автомат =====
  switch (state) {
    case EMPTY:
      if (present) { stillSince = now; go(AWAKE, now); }
      break;
    case AWAKE:
      if (gone)                    go(EMPTY, now);
      else if (stillFor > T_STILL) go(FALLING_ASLEEP, now);
      break;
    case FALLING_ASLEEP:
      if (gone)                              go(EMPTY, now);
      else if (moving)                       go(AWAKE, now);
      else if (now - stateSince > T_FADE)    go(ASLEEP, now);
      break;
    case ASLEEP:
      if (gone)        go(EMPTY, now);
      else if (moving) go(AWAKE, now);   // проснулся / встал ночью
      break;
  }

  // ===== ACT =====
  switch (state) {
    case EMPTY:  setLight(0); break;
    case AWAKE:  setLight(BRIGHT_AWAKE); break;
    case FALLING_ASLEEP: {
      unsigned long t = now - stateSince;
      long b = BRIGHT_AWAKE - (long)BRIGHT_AWAKE * t / T_FADE;   // линейное затухание
      setLight(b < 0 ? 0 : b);
      break;
    }
    case ASLEEP: setLight(0); break;
  }

  // строка для Serial Plotter: удобно снять график для README
  Serial.printf("raw:%d avg:%d moving:%d light:%d state:%d\n",
                rawMove, avgMove, moving, light, state);
}
