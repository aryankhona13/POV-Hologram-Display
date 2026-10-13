// =====================================================
//  POV HOLOGRAM  -  ESP8266  (version 2.2)
//
//  What is new in version 2.2:
//   * FIXED scrolling text exit ghosting: LEDs completely
//     blank when the text exits, eliminating leftover dots,
//     circles, or ghost pixels.
//   * Dynamic text length: maximum text length is computed
//     dynamically based on available display width.
//   * NEW Sentence Scrolling mode: allows a full sentence
//     (up to 100 chars) to scroll continuously across the display.
//   * Limited angle range for Sentence Scrolling: avoids full
//     360-degree rotation distortion and prevents circular patterns.
//   * Calibrated battery monitoring for ESP8266 + 1:1 divider
//     (4.2V=100%, 3.6V=0%) with burst median filter + EMA smoothing.
//   * OTA firmware upload supported and fully compatible.
//   * All settings saved in EEPROM (EE_MAGIC migration safe).
//
//  Arduino IDE settings:
//    Board         : NodeMCU 1.0 (ESP-12E Module)  or  LOLIN(WEMOS) D1 mini
//    CPU Frequency : 160 MHz            <-- important
//    Flash Size    : 4MB (FS:none OTA:~1019KB)  (any 4MB "OTA" option works)
//    Libraries     : FastLED (only used for colour helpers)
//    ESP8266 core  : 3.1.x
//
//  Wiring:
//    LED strip 1 data  -> D1 (GPIO5)
//    LED strip 2 data  -> D2 (GPIO4)
//    Hall sensor       -> D7 (GPIO13)
//    Battery divider   -> A0
//
//  Best RPM (value shown on dashboard): about 900 to 1300
// =====================================================
#define FASTLED_ESP8266_RAW_PIN_ORDER   // pin numbers below are GPIO numbers
#define FASTLED_ALLOW_INTERRUPTS 0      // clean LED data (no random flashes while WiFi is busy)
#define USE_FAST_DRIVER 1               // 1 = own fast dual-strip driver (recommended), 0 = FastLED driver

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <FastLED.h>
#include <Updater.h>
#include <EEPROM.h>
#include <time.h>
#include <sys/time.h>

#if F_CPU < 160000000L
#warning "Set Tools > CPU Frequency to 160 MHz for stable LED timing"
#endif

#ifndef IRAM_ATTR
#define IRAM_ATTR ICACHE_RAM_ATTR
#endif

void forceRedraw();
void resetAnimation();
void blankDisplay();

// =====================================================
// PIN & HARDWARE CONFIGURATION
// =====================================================
#define STRIP1_PIN   5      // D1
#define STRIP2_PIN   4      // D2
#define HALL_PIN     13     // D7
#define BATTERY_PIN  A0

#define NUM_LEDS    8

// true = LED number 0 of this strip is the one closest to the hub
#define STRIP1_LED0_AT_CENTER true
#define STRIP2_LED0_AT_CENTER true

// =====================================================
// BATTERY CALIBRATION FOR ESP8266
// Hardware: Battery -> 10k -> ADC (A0) -> 10k -> GND (1:1 divider)
// Measured test values:
//   Battery 4.2V -> ADC divider point ~2.05V -> ~655 raw ADC
//   Battery 3.6V -> ADC divider point ~1.78V -> ~569 raw ADC
// =====================================================
#define BATT_RAW_4V2  655.0f   // 100% battery
#define BATT_RAW_3V6  569.0f   // 0% battery

// =====================================================
// LIMITS & SIZES
// =====================================================
#define MAX_TEXT_LEN_STATIC  32     // max buffer size for normal text
#define MAX_SENTENCE_LEN     100    // max characters in sentence mode
#define CHAR_COLS            5
#define MIN_GAP              1
#define MAX_GAP              4
#define MAX_INNER_OFF        3
#define TEXT_COL_DEG         3.6f
#define TEXT_BUF_COLS        160
#define SENT_BUF_COLS        650    // buffer for full sentence columns
#define CV_COLS              100
#define CV_DEG               (360.0f / CV_COLS)

// Sentence viewing aperture: limited angular arc to prevent 360-deg circular distortion
#define SENT_APERTURE_DEG    150.0f

// =====================================================
// TIMING SETTINGS
// =====================================================
#define MIN_RPM            300
#define MIN_PULSE_GAP_US   2000UL
#define STOP_TIMEOUT_US    1000000UL
#define LOOP_OVERHEAD_US   80        // time the loop needs around one LED update
#define SAFE_MARGIN        1.3f      // safety margin used for quality check
#define GOOD_RPM_MIN       900       // below this picture can flicker
#define LED_LEAD           1.0f
#define WEB_FREE_US        6000.0f
#define LED_LATCH_US       60        // LED reset time between two transfers

// global text width (percent of default column width)
#define WIDTH_MIN_PCT      40
#define WIDTH_MAX_PCT      200

// =====================================================
// WIFI & MEMORY
// =====================================================
const char* ssid       = "POV_Hologram_STA_MODE";
const char* password   = "013171317";
const char* apName     = "POV_Hologram_AP_MODE";
const char* apPassword = "013171317";

ESP8266WebServer server(80);

// =====================================================
// MODES & SETTINGS
// =====================================================
#define NUM_MODES 8
enum {
  MODE_TEXT     = 0,
  MODE_CANVAS   = 1,
  MODE_DIGITAL  = 2,
  MODE_ANALOG   = 3,
  MODE_DATE     = 4,
  MODE_SHAPES   = 5,
  MODE_AUTO     = 6,
  MODE_SENTENCE = 7
};

struct ModeCfg {
  int  off;
  int  angle;
  bool hflip;
  bool vflip;
};
ModeCfg cfg[NUM_MODES] = {
  {3, 90, false, false},    // text
  {3, 90, false, false},    // canvas
  {3, 90, false, false},    // digital clock
  {3, 90, false, false},    // analog clock
  {3, 90, false, false},    // date & day
  {0, 90, false, false},    // shapes
  {3, 90, false, false},    // auto fallback
  {3, 90, false, false}     // sentence scrolling (angle limited to 0-180)
};

// AUTO CYCLE EXCLUSION: indices 0..5 and 7 can be included in auto
bool autoEn[NUM_MODES] = {true, true, true, true, true, true, false, false};

int      mode        = MODE_TEXT;
uint8_t  brightness  = 150;

// text mode
String   displayText   = "HELLO";
int      letterGap     = 2;
bool     scrollEnabled = false;
int      scrollSpeed   = 5;
int      holdTimeSec   = 3;

// sentence scrolling mode
String   sentenceText    = "Welcome to Government Engineering College Rajkot";
int      sentScrollSpeed = 5;
CRGB     sentColor       = CRGB(0, 255, 204);

// auto mode variables
int      autoIntervalSec  = 5;
int      currentAutoMode  = MODE_TEXT;
uint32_t lastAutoSwitchMs = 0;
bool     inTransition     = false;
uint32_t transitionStart  = 0;
int      nextAutoMode     = MODE_TEXT;

// colours
CRGB textColor = CRGB(0, 255, 204);

// Digital clock colors
CRGB digHourColor  = CRGB(0, 229, 255);
CRGB digMinColor   = CRGB(0, 229, 255);
CRGB digSecColor   = CRGB(0, 229, 255);
CRGB digColonColor = CRGB(255, 255, 255);

// Analog clock colors
CRGB hourColor = CRGB(255, 80, 80);
CRGB minColor  = CRGB(80, 170, 255);
CRGB secColor  = CRGB(0, 255, 120);
CRGB tickColor = CRGB(140, 140, 140);

// Date & Day settings and colors
bool dateAuto      = true;
int  currentShowDt = 0; // 0 = Day, 1 = Date
int  dtWordGap     = 4;
int  dateGap       = 2;
int  dayGap        = 2;

CRGB dayColor      = CRGB(255, 150, 0);
CRGB dateNumColor  = CRGB(0, 255, 150);
CRGB dateMonColor  = CRGB(0, 200, 255);

// Shapes colors & settings
int  currentShape = 0; // 0=Heart, 1=Smiley
bool shapeAuto    = false;
CRGB heartColor   = CRGB(255, 0, 100);
CRGB smileyColor  = CRGB(255, 200, 0);
int  heartWidth   = 10;
int  heartHeight  = 10;

// Sub-transitions for Day/Date & Shapes
bool     subTransition   = false;
uint32_t subTransStart   = 0;
int      subTransType    = 0; // 1 = Shapes, 2 = Date&Day
int      nextSubVal      = 0;

// clock settings
bool use24h      = true;
bool showSeconds = false;
int  digGap      = 1;
bool showSecHand = true;
bool showTicks   = true;
int  tzOffsetMin = 330;

// global text width (percent of default column width)
int  widthPct = 100;

// after a manual choice (shape / day-date) auto switching waits 10 seconds
bool     subHoldOn       = false;
uint32_t subHoldStart    = 0;
uint32_t subLastSwitchMs = 0;

// speed quality (updated once per revolution)
int   quality   = 0;       // 0 stopped, 1 too slow, 2 too fast, 3 good
float maxRpmNow = 0;

// saving
bool     settingsDirty = false;
bool     cvDirty       = false;
bool     cvSaved       = false;
uint32_t lastChangeMs  = 0;

// =====================================================
// RUNTIME VARIABLES
// =====================================================
CRGB leds1[NUM_LEDS];
CRGB leds2[NUM_LEDS];
CRGB cv[CV_COLS][NUM_LEDS];
CRGB ax[CV_COLS][NUM_LEDS];
CRGB sh[CV_COLS][NUM_LEDS];

volatile uint32_t lastHallTime   = 0;
volatile uint32_t revolutionTime = 0;

// speed filter
#define NUM_PRED 4
const float PRED_BETA[NUM_PRED] = {1.0f, 0.5f, 0.25f, 0.12f};
float predT[NUM_PRED];
float predErr[NUM_PRED];
int   bestPred    = 1;
float devVar      = 0;
bool  filterValid = false;
float estPeriod   = 0;
uint32_t prevHallSeen = 0;
int   liveRPM     = 0;
float jitterPct   = 0;

// text layout (used by text mode, digital clock, date&day)
uint8_t textBuf[TEXT_BUF_COLS];
CRGB    colorBuf[TEXT_BUF_COLS];
int     textColumns = 0;
bool    needLayout  = true;
float   colDegInUse = TEXT_COL_DEG;
int     gapInUse    = 2;
String  clockStr    = "--:--";
String  dateStr     = "";

// sentence layout
uint8_t sentBuf[SENT_BUF_COLS];
int     sentColumns      = 0;
bool    sentNeedLayout   = true;
float   sentColDeg       = TEXT_COL_DEG;
float   sentScrollOffset = 0.0f;
uint32_t sentLastUpdateUs = 0;

// scroll animation (normal text)
int      animState = 0; // 0=enter, 1=hold, 2=exit, 3=pause outside
uint32_t animStart = 0;

// bookkeeping
bool axDirty = true;
int  axHourCol = -2, axMinCol = -2, axSecCol = -2;
bool shDirty = true;

// battery filtering & stability
int      batteryPercent   = 100;
float    batteryFiltered  = -1.0f;
uint32_t lastBatteryCheck = 0;

float showTimeUs = 400.0f;
int   lastCol1 = -2;
int   lastCol2 = -2;
uint32_t lastServeMs = 0;

int getActiveMode() { return (mode == MODE_AUTO) ? currentAutoMode : mode; }

// Dynamic max characters calculation based on actual rendered width
int getDynamicMaxTextLen() {
  float deg = TEXT_COL_DEG * (float)widthPct / 100.0f;
  if (deg < 0.1f) deg = 0.1f;
  int maxCols = (int)(355.0f / deg);
  if (maxCols > TEXT_BUF_COLS) maxCols = TEXT_BUF_COLS;
  int colPerChar = CHAR_COLS + letterGap;
  if (colPerChar < 1) colPerChar = 1;
  int maxChars = (maxCols + letterGap) / colPerChar;
  if (maxChars < 3) maxChars = 3;
  if (maxChars > 30) maxChars = 30;
  return maxChars;
}

// =====================================================
// FONT: 5 columns x 5 rows, bit 0 = top row
// =====================================================
const uint8_t FONT[65][5] = {
  {0x00, 0x00, 0x00, 0x00, 0x00}, // space

  {0x1E, 0x05, 0x05, 0x05, 0x1E}, // A
  {0x1F, 0x15, 0x15, 0x15, 0x0A}, // B
  {0x0E, 0x11, 0x11, 0x11, 0x11}, // C
  {0x1F, 0x11, 0x11, 0x11, 0x0E}, // D
  {0x1F, 0x15, 0x15, 0x15, 0x11}, // E
  {0x1F, 0x05, 0x05, 0x05, 0x01}, // F
  {0x0E, 0x11, 0x15, 0x15, 0x1D}, // G
  {0x1F, 0x04, 0x04, 0x04, 0x1F}, // H
  {0x11, 0x11, 0x1F, 0x11, 0x11}, // I
  {0x08, 0x10, 0x11, 0x0F, 0x01}, // J
  {0x1F, 0x04, 0x04, 0x0A, 0x11}, // K
  {0x1F, 0x10, 0x10, 0x10, 0x10}, // L
  {0x1F, 0x02, 0x04, 0x02, 0x1F}, // M
  {0x1F, 0x02, 0x04, 0x08, 0x1F}, // N
  {0x0E, 0x11, 0x11, 0x11, 0x0E}, // O
  {0x1F, 0x05, 0x05, 0x05, 0x02}, // P
  {0x0E, 0x11, 0x11, 0x09, 0x16}, // Q
  {0x1F, 0x05, 0x05, 0x0D, 0x12}, // R
  {0x12, 0x15, 0x15, 0x15, 0x09}, // S
  {0x01, 0x01, 0x1F, 0x01, 0x01}, // T
  {0x0F, 0x10, 0x10, 0x10, 0x0F}, // U
  {0x07, 0x08, 0x10, 0x08, 0x07}, // V
  {0x1F, 0x08, 0x04, 0x08, 0x1F}, // W
  {0x11, 0x0A, 0x04, 0x0A, 0x11}, // X
  {0x01, 0x02, 0x1C, 0x02, 0x01}, // Y
  {0x11, 0x19, 0x15, 0x13, 0x11}, // Z

  {0x08, 0x12, 0x12, 0x12, 0x1C}, // a
  {0x1F, 0x12, 0x12, 0x12, 0x0C}, // b
  {0x0C, 0x12, 0x12, 0x12, 0x12}, // c
  {0x0C, 0x12, 0x12, 0x12, 0x1F}, // d
  {0x0C, 0x1A, 0x1A, 0x1A, 0x1C}, // e
  {0x04, 0x1E, 0x05, 0x05, 0x00}, // f
  {0x14, 0x1A, 0x1A, 0x1A, 0x0E}, // g
  {0x1F, 0x02, 0x02, 0x02, 0x1C}, // h
  {0x00, 0x14, 0x1D, 0x10, 0x00}, // i
  {0x00, 0x10, 0x14, 0x0D, 0x00}, // j
  {0x1F, 0x04, 0x04, 0x0A, 0x10}, // k
  {0x00, 0x00, 0x1F, 0x00, 0x00}, // l
  {0x1E, 0x02, 0x1E, 0x02, 0x1C}, // m
  {0x1E, 0x02, 0x02, 0x02, 0x1C}, // n
  {0x0C, 0x12, 0x12, 0x12, 0x0C}, // o
  {0x1E, 0x0A, 0x0A, 0x0A, 0x04}, // p
  {0x04, 0x0A, 0x0A, 0x0A, 0x1E}, // q
  {0x1E, 0x04, 0x02, 0x02, 0x04}, // r
  {0x04, 0x12, 0x12, 0x12, 0x08}, // s
  {0x02, 0x1F, 0x12, 0x00, 0x00}, // t
  {0x0E, 0x10, 0x10, 0x10, 0x1E}, // u
  {0x06, 0x08, 0x10, 0x08, 0x06}, // v
  {0x0E, 0x10, 0x0C, 0x10, 0x0E}, // w
  {0x12, 0x0C, 0x00, 0x0C, 0x12}, // x
  {0x16, 0x18, 0x18, 0x18, 0x0E}, // y
  {0x12, 0x1A, 0x12, 0x16, 0x12}, // z

  {0x0E, 0x11, 0x15, 0x11, 0x0E}, // 0
  {0x00, 0x12, 0x1F, 0x10, 0x00}, // 1
  {0x12, 0x19, 0x15, 0x15, 0x12}, // 2
  {0x11, 0x15, 0x15, 0x15, 0x0A}, // 3
  {0x08, 0x0C, 0x0A, 0x1F, 0x08}, // 4
  {0x17, 0x15, 0x15, 0x15, 0x09}, // 5
  {0x0E, 0x15, 0x15, 0x15, 0x08}, // 6
  {0x01, 0x01, 0x19, 0x05, 0x03}, // 7
  {0x0A, 0x15, 0x15, 0x15, 0x0A}, // 8
  {0x02, 0x15, 0x15, 0x15, 0x0E}, // 9

  {0x0A, 0x00, 0x00, 0x00, 0x00}, // :  (1 column wide)
  {0x04, 0x04, 0x04, 0x00, 0x00}  // -  (3 columns wide)
};

const uint8_t ROW_MAP[4][8] = {
  {0, 1, 2, 3, 4, 0, 0, 0},
  {0, 1, 1, 2, 3, 4, 0, 0},
  {0, 1, 1, 2, 3, 3, 4, 0},
  {0, 1, 1, 2, 2, 3, 3, 4}
};

int glyphIndex(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
  if (c >= 'a' && c <= 'z') return c - 'a' + 27;
  if (c >= '0' && c <= '9') return c - '0' + 53;
  if (c == ':') return 63;
  if (c == '-') return 64;
  return 0;
}

int glyphWidth(char c) {
  if (c == ':') return 1;
  if (c == '-') return 3;
  if (c == ' ') return (getActiveMode() == MODE_DATE) ? dtWordGap : CHAR_COLS;
  return CHAR_COLS;
}

uint8_t fitToRows(uint8_t col, int rows) {
  const uint8_t* m = ROW_MAP[rows - 5];
  uint8_t out = 0;
  for (int r = 0; r < rows; r++) {
    if (col & (1 << m[r])) out |= (1 << r);
  }
  return out;
}

int measureColumns(const String& s, int gap) {
  int n = s.length();
  if (n == 0) return 0;
  int total = (n - 1) * gap;
  for (int k = 0; k < n; k++) total += glyphWidth(s[k]);
  return total;
}

void buildColumns(const String& s, int gap) {
  memset(textBuf, 0, sizeof(textBuf));
  fill_solid(colorBuf, TEXT_BUF_COLS, CRGB::Black);

  int n = s.length();
  int pos = 0;
  int colonCount = 0;

  for (int k = 0; k < n; k++) {
    char c = s[k];
    CRGB charColor = textColor;
    int am = getActiveMode();

    if (am == MODE_DIGITAL) {
      if (c == ':') {
        charColor = digColonColor;
        colonCount++;
      } else {
        if (colonCount == 0) charColor = digHourColor;
        else if (colonCount == 1) charColor = digMinColor;
        else charColor = digSecColor;
      }
    } else if (am == MODE_DATE) {
      if (currentShowDt == 0) {
        charColor = dayColor;
      } else {
        if (c >= '0' && c <= '9') charColor = dateNumColor;
        else if (c != ' ') charColor = dateMonColor;
      }
    }

    int w  = glyphWidth(c);
    int gi = glyphIndex(c);
    for (int j = 0; j < w && pos + j < TEXT_BUF_COLS; j++) {
      if (c == ' ') {
        textBuf[pos + j] = 0;
      } else {
        textBuf[pos + j] = FONT[gi][j];
      }
      colorBuf[pos + j] = charColor;
    }
    pos += w;
    if (k < n - 1) pos += gap;
  }
  textColumns = (pos < TEXT_BUF_COLS) ? pos : TEXT_BUF_COLS;
}

void buildSentenceColumns() {
  memset(sentBuf, 0, sizeof(sentBuf));
  int n = sentenceText.length();
  int pos = 0;
  int gap = letterGap;

  for (int k = 0; k < n; k++) {
    char c = sentenceText[k];
    int w  = (c == ':') ? 1 : ((c == '-') ? 3 : CHAR_COLS);
    int gi = glyphIndex(c);

    for (int j = 0; j < w && pos + j < SENT_BUF_COLS; j++) {
      if (c == ' ') sentBuf[pos + j] = 0;
      else          sentBuf[pos + j] = FONT[gi][j];
    }
    pos += w;
    if (k < n - 1) pos += gap;
  }
  sentColumns = (pos < SENT_BUF_COLS) ? pos : SENT_BUF_COLS;
}

const String& activeString() {
  int am = getActiveMode();
  if (am == MODE_DIGITAL) return clockStr;
  if (am == MODE_DATE)    return dateStr;
  return displayText;
}

int activeGap() {
  int am = getActiveMode();
  if (am == MODE_DIGITAL) return digGap;
  if (am == MODE_DATE) return (currentShowDt == 0) ? dayGap : dateGap;
  return letterGap;
}

// =====================================================
// SAVE & LOAD  (EEPROM emulation)
// =====================================================
void markChanged()       { settingsDirty = true; lastChangeMs = millis(); }
void markCanvasChanged() { cvDirty = true;       lastChangeMs = millis(); }

uint32_t rgbOf(CRGB c) { return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b; }
CRGB crgbOf(uint32_t v) { return CRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

#define EEPROM_SIZE    4096
#define EE_CV_OFFSET   768
#define EE_MAGIC       0x504F5635UL      // "POV5"
#define EE_OLD_MAGIC   0x504F5633UL      // "POV3"

struct SaveBlob {
  uint32_t magic;
  char     text[32];
  int      brightness, scrollSpeed, holdTimeSec, letterGap, digGap, dateGap, dayGap, dtWordGap;
  int      tzOffsetMin, mode, autoIntervalSec, currentShape, currentShowDt, heartWidth, heartHeight, widthPct;
  bool     scrollEnabled, use24h, showSeconds, showSecHand, showTicks, shapeAuto, dateAuto, cvSaved;
  bool     autoEn[NUM_MODES];
  ModeCfg  cfg[NUM_MODES];
  uint32_t colors[15];
  char     sentence[104];
  int      sentScrollSpeed;
  bool     sentReserved;
};

static_assert(sizeof(SaveBlob) <= EE_CV_OFFSET, "SaveBlob too big");
static_assert(EE_CV_OFFSET + sizeof(cv) <= EEPROM_SIZE, "EEPROM too small");

CRGB* const colorSlot[15] = {
  &textColor, &digHourColor, &digMinColor, &digSecColor, &digColonColor,
  &hourColor, &minColor, &secColor, &tickColor,
  &dayColor, &dateNumColor, &dateMonColor, &heartColor, &smileyColor,
  &sentColor
};

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);

  SaveBlob b;
  EEPROM.get(0, b);
  if (b.magic != EE_MAGIC && b.magic != EE_OLD_MAGIC) return; // fresh start with defaults

  if (b.magic == EE_MAGIC) {
    b.text[31] = 0;
    displayText = String(b.text);
    if (displayText.length() > 30) displayText = displayText.substring(0, 30);

    b.sentence[103] = 0;
    sentenceText = String(b.sentence);
    if (sentenceText.length() > MAX_SENTENCE_LEN) sentenceText = sentenceText.substring(0, MAX_SENTENCE_LEN);
    if (sentenceText.length() == 0) sentenceText = "Welcome to Government Engineering College Rajkot";

    sentScrollSpeed = constrain(b.sentScrollSpeed, 1, 10);

    for (int m = 0; m < NUM_MODES; m++) autoEn[m] = b.autoEn[m];
    for (int m = 0; m < NUM_MODES; m++) {
      cfg[m].off   = constrain(b.cfg[m].off, 0, MAX_INNER_OFF);
      cfg[m].angle = constrain(b.cfg[m].angle, 0, (m == MODE_SENTENCE ? 180 : 360));
      cfg[m].hflip = b.cfg[m].hflip;
      cfg[m].vflip = b.cfg[m].vflip;
    }
    for (int i = 0; i < 15; i++) *colorSlot[i] = crgbOf(b.colors[i]);
  } else {
    // Migrated from POV3: safely read basic fields without reading past old struct
    b.text[7] = 0;
    displayText = String(b.text);
    for (int m = 0; m < 6; m++) autoEn[m] = b.autoEn[m];
    autoEn[6] = false; autoEn[7] = false;
    for (int m = 0; m < 7; m++) {
      cfg[m].off   = constrain(b.cfg[m].off, 0, MAX_INNER_OFF);
      cfg[m].angle = constrain(b.cfg[m].angle, 0, 360);
      cfg[m].hflip = b.cfg[m].hflip;
      cfg[m].vflip = b.cfg[m].vflip;
    }
    cfg[MODE_SENTENCE] = {3, 90, false, false};
    for (int i = 0; i < 14; i++) *colorSlot[i] = crgbOf(b.colors[i]);
    sentColor = textColor;
  }

  brightness      = constrain(b.brightness, 10, 255);
  scrollSpeed     = constrain(b.scrollSpeed, 1, 10);
  holdTimeSec     = constrain(b.holdTimeSec, 1, 6);
  letterGap       = constrain(b.letterGap, MIN_GAP, MAX_GAP);
  digGap          = constrain(b.digGap, 1, 3);
  dateGap         = constrain(b.dateGap, 1, 4);
  dayGap          = constrain(b.dayGap, 1, 4);
  dtWordGap       = constrain(b.dtWordGap, 1, 10);
  tzOffsetMin     = constrain(b.tzOffsetMin, -720, 840);
  mode            = constrain(b.mode, 0, NUM_MODES - 1);
  autoIntervalSec = constrain(b.autoIntervalSec, 2, 60);
  currentShape    = constrain(b.currentShape, 0, 1);
  currentShowDt   = constrain(b.currentShowDt, 0, 1);
  heartWidth      = constrain(b.heartWidth, 5, 20);
  heartHeight     = constrain(b.heartHeight, 5, 20);
  widthPct        = constrain(b.widthPct, WIDTH_MIN_PCT, WIDTH_MAX_PCT);

  scrollEnabled = b.scrollEnabled;
  use24h        = b.use24h;
  showSeconds   = b.showSeconds;
  showSecHand   = b.showSecHand;
  showTicks     = b.showTicks;
  shapeAuto     = b.shapeAuto;
  dateAuto      = b.dateAuto;

  if (b.cvSaved) {
    cvSaved = true;
    uint8_t* p = (uint8_t*)cv;
    for (size_t i = 0; i < sizeof(cv); i++) p[i] = EEPROM.read(EE_CV_OFFSET + i);
  }
}

void saveAll() {
  SaveBlob b;
  memset(&b, 0, sizeof(b));

  b.magic = EE_MAGIC;
  strncpy(b.text, displayText.c_str(), 31);
  b.text[31] = 0;

  strncpy(b.sentence, sentenceText.c_str(), 103);
  b.sentence[103] = 0;
  b.sentScrollSpeed = sentScrollSpeed;

  b.brightness      = brightness;
  b.scrollSpeed     = scrollSpeed;
  b.holdTimeSec     = holdTimeSec;
  b.letterGap       = letterGap;
  b.digGap          = digGap;
  b.dateGap         = dateGap;
  b.dayGap          = dayGap;
  b.dtWordGap       = dtWordGap;
  b.tzOffsetMin     = tzOffsetMin;
  b.mode            = mode;
  b.autoIntervalSec = autoIntervalSec;
  b.currentShape    = currentShape;
  b.currentShowDt   = currentShowDt;
  b.heartWidth      = heartWidth;
  b.heartHeight     = heartHeight;
  b.widthPct        = widthPct;

  b.scrollEnabled = scrollEnabled;
  use24h          = use24h;
  b.use24h        = use24h;
  b.showSeconds   = showSeconds;
  b.showSecHand   = showSecHand;
  b.showTicks     = showTicks;
  b.shapeAuto     = shapeAuto;
  b.dateAuto      = dateAuto;

  for (int m = 0; m < NUM_MODES; m++) b.autoEn[m] = autoEn[m];
  for (int m = 0; m < NUM_MODES; m++) b.cfg[m] = cfg[m];
  for (int i = 0; i < 15; i++) b.colors[i] = rgbOf(*colorSlot[i]);

  if (cvDirty) cvSaved = true;
  b.cvSaved = cvSaved;

  EEPROM.put(0, b);

  if (cvDirty) {
    const uint8_t* p = (const uint8_t*)cv;
    for (size_t i = 0; i < sizeof(cv); i++) EEPROM.write(EE_CV_OFFSET + i, p[i]);
  }

  EEPROM.commit();
  settingsDirty = false;
  cvDirty = false;
}

void maybeSave() {
  if ((settingsDirty || cvDirty) && millis() - lastChangeMs > 5000) saveAll();
}

// =====================================================
// TIME
// =====================================================
bool getLocalTm(struct tm& t, float& frac) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  bool valid = (tv.tv_sec >= 1700000000L);

  time_t local = tv.tv_sec + (time_t)tzOffsetMin * 60;
  gmtime_r(&local, &t);
  frac = tv.tv_usec / 1000000.0f;
  return valid;
}

// =====================================================
// PRE-BUILT SHAPES
// =====================================================
void buildShapes() {
  shDirty = false;
  fill_solid(&sh[0][0], CV_COLS * NUM_LEDS, CRGB::Black);

  int R = NUM_LEDS - cfg[MODE_SHAPES].off;
  if (R <= 0) return;

  CRGB colorToUse = (currentShape == 0) ? heartColor : smileyColor;

  for (int c = 0; c < CV_COLS; c++) {
    float theta = c * 2.0f * 3.14159265f / CV_COLS;
    float sinT = sinf(theta);
    float cosT = cosf(theta);
    for (int r = 0; r < R; r++) {
      float rad = (float)(R - r) / (float)R;
      float x = rad * sinT;
      float y = rad * cosT;
      bool hit = false;

      if (currentShape == 0) {
        float scaleX = (float)heartWidth / 10.0f;
        float scaleY = (float)heartHeight / 10.0f;
        float sx = x * 1.15f / scaleX;
        float sy = (y + 0.1f) * 1.15f / scaleY;

        float dd = sy - cbrtf(sx * sx);
        float val = sx * sx + dd * dd;
        if (val <= 1.0f) hit = true;
      }
      else if (currentShape == 1) {
        if (rad > 0.85f) hit = true;
        float ex = fabsf(x) - 0.35f;
        float ey = y - 0.35f;
        if (ex * ex + ey * ey < 0.04f) hit = true;
        if (y < -0.1f && rad > 0.5f && rad < 0.75f) hit = true;
      }

      if (hit) sh[c][r] = colorToUse;
    }
  }
}

// =====================================================
// FAST DUAL-STRIP LED DRIVER
// Both strips are sent simultaneously. Takes ~240 us for 8 LEDs.
// =====================================================
uint8_t  wsBuf1[NUM_LEDS * 3];
uint8_t  wsBuf2[NUM_LEDS * 3];
uint32_t lastShowEndUs = 0;

#if USE_FAST_DRIVER
static inline __attribute__((always_inline)) uint32_t ccount() {
  uint32_t c;
  __asm__ __volatile__("rsr %0,ccount" : "=a"(c));
  return c;
}

#define CPU_MHZ        (F_CPU / 1000000UL)
#define NS2CYC(ns)     (((uint32_t)(ns) * CPU_MHZ) / 1000UL)
#define WS_T0H_CYC     NS2CYC(420)     // high time of a "0" bit (~400 ns)
#define WS_T1H_CYC     NS2CYC(820)     // high time of a "1" bit (~800 ns)
#define WS_BIT_CYC     NS2CYC(1250)    // one full bit (1.25 us)

void IRAM_ATTR wsSendDual(const uint8_t* a, const uint8_t* b, int nBytes) {
  const uint32_t m1 = 1UL << STRIP1_PIN;
  const uint32_t m2 = 1UL << STRIP2_PIN;
  const uint32_t both = m1 | m2;

  noInterrupts();
  GPOC = both;
  uint32_t slot = ccount() + 32;
  for (int i = 0; i < nBytes; i++) {
    const uint32_t x = a[i];
    const uint32_t y = b[i];
    for (int bit = 7; bit >= 0; bit--) {
      uint32_t lowAt = (((x >> bit) & 1) ? 0 : m1) | (((y >> bit) & 1) ? 0 : m2);
      __asm__ __volatile__("" : "+r"(lowAt));
      uint32_t t;
      do { t = ccount(); } while ((int32_t)(t - slot) < 0);
      GPOS = both;
      while ((ccount() - t) < WS_T0H_CYC) {}
      GPOC = lowAt;
      while ((ccount() - t) < WS_T1H_CYC) {}
      GPOC = both;
      slot = t + WS_BIT_CYC;
    }
  }
  interrupts();
}
#endif

void ledShow() {
#if USE_FAST_DRIVER
  for (int i = 0; i < NUM_LEDS; i++) {
    wsBuf1[i * 3 + 0] = scale8(leds1[i].g, brightness);
    wsBuf1[i * 3 + 1] = scale8(leds1[i].r, brightness);
    wsBuf1[i * 3 + 2] = scale8(leds1[i].b, brightness);
    wsBuf2[i * 3 + 0] = scale8(leds2[i].g, brightness);
    wsBuf2[i * 3 + 1] = scale8(leds2[i].r, brightness);
    wsBuf2[i * 3 + 2] = scale8(leds2[i].b, brightness);
  }
  while ((uint32_t)(micros() - lastShowEndUs) < LED_LATCH_US) {}
  wsSendDual(wsBuf1, wsBuf2, NUM_LEDS * 3);
  lastShowEndUs = micros();
#else
  FastLED.show();
#endif
}

// =====================================================
// LED HELPERS & TRANSITION
// =====================================================
void showLEDs() {
  if (inTransition || subTransition) {
    uint32_t t = inTransition ? (millis() - transitionStart) : (millis() - subTransStart);
    uint8_t fade = 0;

    if (t < 500) {
      fade = (t * 255) / 500;
    } else if (t < 1000) {
      if (inTransition && currentAutoMode != nextAutoMode) {
        currentAutoMode = nextAutoMode;
        forceRedraw(); needLayout = true; sentNeedLayout = true; axDirty = true; shDirty = true; resetAnimation();
      }
      if (subTransition) {
        if (subTransType == 1 && currentShape != nextSubVal) {
          currentShape = nextSubVal;
          shDirty = true; forceRedraw();
        }
        if (subTransType == 2 && currentShowDt != nextSubVal) {
          currentShowDt = nextSubVal;
          needLayout = true; forceRedraw();
        }
      }
      fade = ((1000 - t) * 255) / 500;
    } else {
      if (inTransition) { inTransition = false; lastAutoSwitchMs = millis(); }
      if (subTransition) subTransition = false;
      fade = 0;
    }

    if (fade > 0) {
      fadeLightBy(leds1, NUM_LEDS, fade);
      fadeLightBy(leds2, NUM_LEDS, fade);
    }
  }

  static CRGB sent1[NUM_LEDS], sent2[NUM_LEDS];
  static uint32_t lastSentMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastSentMs < 40 &&
      memcmp((const void*)leds1, (const void*)sent1, sizeof(sent1)) == 0 &&
      memcmp((const void*)leds2, (const void*)sent2, sizeof(sent2)) == 0) return;
  memcpy((void*)sent1, (const void*)leds1, sizeof(sent1));
  memcpy((void*)sent2, (const void*)leds2, sizeof(sent2));
  lastSentMs = nowMs;

  uint32_t t0 = micros();
  ledShow();
  uint32_t dt = micros() - t0;
  showTimeUs = showTimeUs * 0.875f + (float)dt * 0.125f;
  showTimeUs = constrain(showTimeUs, 150.0f, 3000.0f);
}

void forceRedraw() { lastCol1 = -2; lastCol2 = -2; }

// Complete blank: forces WS2812 hardware to send all black
void blankDisplay() {
  fill_solid(leds1, NUM_LEDS, CRGB::Black);
  fill_solid(leds2, NUM_LEDS, CRGB::Black);
  ledShow();
  lastCol1 = -1;
  lastCol2 = -1;
}

float wrap360(float a) {
  while (a < 0.0f)    a += 360.0f;
  while (a >= 360.0f) a -= 360.0f;
  return a;
}

void renderTextStrip(CRGB* strip, int column, bool led0AtCenter) {
  fill_solid(strip, NUM_LEDS, CRGB::Black);
  if (column < 0 || column >= textColumns) return;

  const ModeCfg& C = cfg[getActiveMode()];
  int rows = NUM_LEDS - C.off;
  uint8_t data = fitToRows(textBuf[column], rows);
  CRGB color = colorBuf[column];

  for (int r = 0; r < rows; r++) {
    int src = C.vflip ? (rows - 1 - r) : r;
    if (data & (1 << src)) {
      int led = led0AtCenter ? (C.off + r) : (NUM_LEDS - 1 - C.off - r);
      strip[led] = color;
    }
  }
}

void renderSentenceStrip(CRGB* strip, int column, bool led0AtCenter) {
  fill_solid(strip, NUM_LEDS, CRGB::Black);
  if (column < 0 || column >= sentColumns) return;

  const ModeCfg& C = cfg[MODE_SENTENCE];
  int rows = NUM_LEDS - C.off;
  uint8_t data = fitToRows(sentBuf[column], rows);

  for (int r = 0; r < rows; r++) {
    int src = C.vflip ? (rows - 1 - r) : r;
    if (data & (1 << src)) {
      int led = led0AtCenter ? (C.off + r) : (NUM_LEDS - 1 - C.off - r);
      strip[led] = sentColor;
    }
  }
}

void renderPictureStrip(CRGB* strip, int col, CRGB* buf, bool led0AtCenter) {
  fill_solid(strip, NUM_LEDS, CRGB::Black);

  const ModeCfg& C = cfg[getActiveMode()];
  int rows = NUM_LEDS - C.off;

  for (int i = 0; i < rows; i++) {
    int ii = C.vflip ? (rows - 1 - i) : i;
    CRGB c = buf[col * NUM_LEDS + ii];
    if (c.r || c.g || c.b) {
      int k   = NUM_LEDS - 1 - i;
      int led = led0AtCenter ? k : (NUM_LEDS - 1 - k);
      strip[led] = c;
    }
  }
}

// =====================================================
// SPEED FILTER
// =====================================================
void updateFilter(uint32_t rawPeriod) {
  if (rawPeriod == 0) { filterValid = false; liveRPM = 0; return; }

  float T = (float)rawPeriod;

  if (!filterValid || fabsf(T - predT[bestPred]) > 0.10f * predT[bestPred]) {
    for (int k = 0; k < NUM_PRED; k++) { predT[k] = T; predErr[k] = 0.0f; }
    devVar = 0.0f;
    bestPred = 1;
    filterValid = true;
  } else {
    float e3 = T - predT[NUM_PRED - 1];
    devVar = 0.85f * devVar + 0.15f * e3 * e3;

    for (int k = 0; k < NUM_PRED; k++) {
      float e = T - predT[k];
      predErr[k] = 0.8f * predErr[k] + 0.2f * e * e;
      predT[k]  += PRED_BETA[k] * e;
    }

    int best = bestPred;
    for (int k = 0; k < NUM_PRED; k++) {
      if (predErr[k] < 0.8f * predErr[best]) best = k;
    }
    bestPred = best;
  }

  estPeriod = predT[bestPred];
  liveRPM   = (int)(60000000.0f / predT[NUM_PRED - 1] + 0.5f);
  jitterPct = sqrtf(devVar) / predT[NUM_PRED - 1] * 100.0f;
}

// =====================================================
// TEXT LAYOUT
// =====================================================
void computeLayout() {
  needLayout = false;

  const String& s = activeString();
  int n = s.length();
  if (n == 0) { textColumns = 0; return; }

  float deg = TEXT_COL_DEG * (float)widthPct / 100.0f;
  int maxCols = (int)(355.0f / deg);
  if (maxCols > TEXT_BUF_COLS) maxCols = TEXT_BUF_COLS;

  int gap = activeGap();
  while (gap > 0 && measureColumns(s, gap) > maxCols) gap--;
  int cols = measureColumns(s, gap);

  if (cols > maxCols) deg = 355.0f / cols;

  int oldCols = textColumns;
  buildColumns(s, gap);

  bool changed = (textColumns != oldCols) || (gap != gapInUse) || fabsf(deg - colDegInUse) > 0.001f;

  gapInUse    = gap;
  colDegInUse = deg;

  if (changed) forceRedraw();
}

void computeSentenceLayout() {
  sentNeedLayout = false;
  float deg = TEXT_COL_DEG * (float)widthPct / 100.0f;
  if (deg < 0.1f) deg = 0.1f;
  sentColDeg = deg;
  buildSentenceColumns();
  forceRedraw();
}

// =====================================================
// TIME STRINGS
// =====================================================
void updateClockString() {
  static uint32_t lastChk = 0;
  uint32_t ms = millis();
  if (ms - lastChk < 100) return;
  lastChk = ms;

  struct tm t;
  float frac;
  char b[16];

  getLocalTm(t, frac);
  int h = t.tm_hour;
  if (!use24h) { h = h % 12; if (h == 0) h = 12; }

  if (use24h) {
    if (showSeconds) snprintf(b, sizeof(b), "%02d:%02d:%02d", h, t.tm_min, t.tm_sec);
    else             snprintf(b, sizeof(b), "%02d:%02d", h, t.tm_min);
  } else {
    if (showSeconds) snprintf(b, sizeof(b), "%d:%02d:%02d", h, t.tm_min, t.tm_sec);
    else             snprintf(b, sizeof(b), "%d:%02d", h, t.tm_min);
  }

  if (clockStr != b) {
    clockStr = b;
    needLayout = true;
    forceRedraw();
  }
}

void updateDateString() {
  static uint32_t lastChk = 0;
  uint32_t ms = millis();
  if (ms - lastChk < 100) return;
  lastChk = ms;

  struct tm t;
  float frac;
  char b[16] = "NO TIME";

  if (getLocalTm(t, frac)) {
    if (currentShowDt == 0) {
      const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
      snprintf(b, sizeof(b), "%s", days[t.tm_wday]);
    } else {
      const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
      snprintf(b, sizeof(b), "%02d %s", t.tm_mday, months[t.tm_mon]);
    }
  }

  if (dateStr != b) {
    dateStr = b;
    needLayout = true;
    forceRedraw();
  }
}

// =====================================================
// ANALOG CLOCK PICTURE
// =====================================================
void putHand(float deg, int len, int widthCols, CRGB color, int R) {
  int ci = (int)floorf(deg / CV_DEG + CV_COLS / 2.0f);
  for (int k = 0; k < len; k++) {
    int ring = R - 1 - k;
    if (ring < 0) break;
    for (int w = 0; w < widthCols; w++) {
      int c = ((ci + w - widthCols / 2) % CV_COLS + CV_COLS) % CV_COLS;
      ax[c][ring] = color;
    }
  }
}

void buildAnalog() {
  axDirty = false;
  fill_solid(&ax[0][0], CV_COLS * NUM_LEDS, CRGB::Black);

  int R = NUM_LEDS - cfg[MODE_ANALOG].off;

  if (showTicks) {
    for (int h = 0; h < 12; h++) {
      int c = ((int)floorf(h * 30.0f / CV_DEG + CV_COLS / 2.0f)) % CV_COLS;
      ax[c][0] = tickColor;
      if (h % 3 == 0 && R >= 3) ax[c][1] = tickColor;
    }
  }

  struct tm t;
  float frac;
  getLocalTm(t, frac);

  float sec = t.tm_sec + frac;
  float mn  = t.tm_min + sec / 60.0f;
  float hr  = (t.tm_hour % 12) + mn / 60.0f;

  putHand(hr * 30.0f, max(2, R / 2), 3, hourColor, R);
  putHand(mn * 6.0f,  max(3, R - 1), 2, minColor, R);
  if (showSecHand) putHand(sec * 6.0f, R, 1, secColor, R);
}

void updateAnalog() {
  static uint32_t lastChk = 0;
  uint32_t ms = millis();
  if (!axDirty && ms - lastChk < 20) return;
  lastChk = ms;

  struct tm t;
  float frac;
  int hc = -2, mc = -2, sc = -2;

  getLocalTm(t, frac);
  float sec = t.tm_sec + frac;
  float mn  = t.tm_min + sec / 60.0f;
  float hr  = (t.tm_hour % 12) + mn / 60.0f;
  hc = (int)floorf(hr * 30.0f / CV_DEG + CV_COLS / 2.0f);
  mc = (int)floorf(mn * 6.0f  / CV_DEG + CV_COLS / 2.0f);
  sc = showSecHand ? (int)floorf(sec * 6.0f / CV_DEG + CV_COLS / 2.0f) : -1;

  if (axDirty || hc != axHourCol || mc != axMinCol || sc != axSecCol) {
    axHourCol = hc;
    axMinCol  = mc;
    axSecCol  = sc;
    buildAnalog();
  }
}

// =====================================================
// SMOOTH SCROLL ANIMATION (FIXED EXIT GHOSTING BUG)
// =====================================================
float smooth01(float p) {
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  return p * p * (3.0f - 2.0f * p);
}

void resetAnimation() {
  animState = 0;
  animStart = millis();
}

float animShift(float width) {
  float dur = (float)(3200 - (scrollSpeed - 1) * 300);
  float t   = (float)(millis() - animStart);
  // Full clearance distance: scroll past the window edge so every column exits completely
  float exitDist = width + colDegInUse * 4.0f;

  switch (animState) {
    case 0: // Entering
      if (t >= dur) { animState = 1; animStart = millis(); return 0.0f; }
      return -width * (1.0f - smooth01(t / dur));
    case 1: // Holding in center
      if (t >= holdTimeSec * 1000.0f) { animState = 2; animStart = millis(); }
      return 0.0f;
    case 2: // Exiting smoothly
      if (t >= dur) {
        animState = 3;
        animStart = millis();
        blankDisplay(); // Guaranteed clean off immediately when exit completes
        return exitDist;
      }
      return exitDist * smooth01(t / dur);
    default: // State 3: Pause outside (display is completely blank)
      if (t >= 600.0f) {
        animState = 0;
        animStart = millis();
        return -width;
      }
      return exitDist;
  }
}

// =====================================================
// HALL SENSOR & BATTERY MONITORING
// =====================================================
void IRAM_ATTR hallDetected() {
  uint32_t now   = micros();
  uint32_t delta = now - lastHallTime;

  uint32_t guard = revolutionTime / 4;
  if (guard < MIN_PULSE_GAP_US) guard = MIN_PULSE_GAP_US;

  if (delta < guard) return;

  if (lastHallTime == 0 || delta > STOP_TIMEOUT_US) revolutionTime = 0;
  else                                              revolutionTime = delta;
  lastHallTime = now;
}

// Burst-median filter: samples 16 readings, discards outliers, averages middle 8
float readBatteryRawSmoothed() {
  const int N = 16;
  int samples[N];
  for (int i = 0; i < N; i++) {
    samples[i] = analogRead(BATTERY_PIN);
    delayMicroseconds(60);
  }
  // Insertion sort
  for (int i = 1; i < N; i++) {
    int key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }
  // Discard 4 lowest (current draw sag) and 4 highest (spikes), average middle 8
  float sum = 0;
  for (int i = 4; i < 12; i++) {
    sum += samples[i];
  }
  return sum / 8.0f;
}

// Battery percentage calculation with multi-stage filter and hysteresis
void updateBattery() {
  uint32_t now = millis();
  if (now - lastBatteryCheck < 1000) return; // 1 second update interval
  lastBatteryCheck = now;

  float raw = readBatteryRawSmoothed();

  // Exponential Moving Average (EMA) to prevent jumps from LED current drops
  if (batteryFiltered < 0.0f) {
    batteryFiltered = raw;
  } else {
    batteryFiltered = batteryFiltered * 0.95f + raw * 0.05f;
  }

  // Calculate percentage based on user measured calibration points:
  // 4.2V -> 2.05V at A0 (~655 raw ADC) = 100%
  // 3.6V -> 1.78V at A0 (~569 raw ADC) = 0%
  float pct = (batteryFiltered - BATT_RAW_3V6) / (BATT_RAW_4V2 - BATT_RAW_3V6) * 100.0f;
  pct = constrain(pct, 0.0f, 100.0f);
  int targetInt = (int)(pct + 0.5f);

  // Rate-limiting / hysteresis: smoothly update by at most 1% per second
  if (abs(targetInt - batteryPercent) >= 1) {
    if (targetInt > batteryPercent) batteryPercent++;
    else if (targetInt < batteryPercent) batteryPercent--;
  }
}

void serviceBackground() {
  uint32_t ms = millis();
  if (ms - lastServeMs < 5) return;
  lastServeMs = ms;
  server.handleClient();
  updateBattery();
  maybeSave();
}

// =====================================================
// WEB DASHBOARD
// =====================================================
const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>POV Hologram</title>
<style>
:root{--bg:#0b0e14;--panel:#141824;--panel2:#1a2030;--line:#252c3f;--accent:#22d3ee;--accent2:#818cf8;--text:#e6eaf2;--dim:#8a94a8}
*{box-sizing:border-box}
body{margin:0;background:radial-gradient(1200px 600px at 10% -10%,#17213a 0%,#0b0e14 60%) fixed;color:var(--text);font-family:Inter,'Segoe UI',system-ui,sans-serif;min-height:100vh}
.top{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:12px;padding:14px 20px;border-bottom:1px solid var(--line);background:rgba(11,14,20,.85);position:sticky;top:0;z-index:5}
.brand{font-weight:700;letter-spacing:3px;font-size:16px}
.brand small{display:block;font-weight:400;letter-spacing:2px;color:var(--dim);font-size:10px;margin-top:3px}
.pills{display:flex;gap:8px;flex-wrap:wrap}
.pill{background:var(--panel);border:1px solid var(--line);border-radius:999px;padding:6px 12px;font-size:11px;letter-spacing:1px;color:var(--dim)}
.pill b{color:var(--text);font-weight:600;margin-left:6px;font-size:13px;letter-spacing:0}
.wrap{max-width:1100px;margin:0 auto;padding:18px}
.seg{display:flex;flex-wrap:wrap;gap:4px;background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:6px;margin-bottom:18px}
.seg div{flex:1 1 auto;text-align:center;padding:12px 2px;border-radius:10px;font-size:11px;font-weight:700;letter-spacing:1px;color:var(--dim);cursor:pointer;user-select:none;min-width:70px}
.seg div.on{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#06101a}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(310px,1fr));gap:16px}
.card{background:linear-gradient(180deg,var(--panel),var(--panel2));border:1px solid var(--line);border-radius:16px;padding:18px;box-shadow:0 8px 24px rgba(0,0,0,.25)}
.wide{grid-column:1/-1}
.ct{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}
.ct h3{margin:0;font-size:12px;letter-spacing:2px;text-transform:uppercase;color:var(--dim);font-weight:700}
.tag{font-size:11px;color:var(--accent);border:1px solid var(--line);border-radius:999px;padding:3px 10px;letter-spacing:1px}
.row{display:flex;justify-content:space-between;align-items:center;font-size:14px;margin:12px 0 2px}
.row .v{color:var(--accent);font-weight:600}
.note{color:var(--dim);font-size:12px;margin:8px 0 0;line-height:1.5}
input[type=range]{width:100%;accent-color:var(--accent);margin:6px 0 8px}
input[type=text],input[type=datetime-local],select,textarea{width:100%;background:#0d111b;border:1px solid var(--line);color:var(--text);border-radius:10px;padding:11px 12px;font-size:15px}
input[type=text]{font-family:ui-monospace,monospace;letter-spacing:2px;text-align:center;font-size:18px}
textarea{font-family:ui-monospace,monospace;font-size:14px;resize:vertical;min-height:55px}
.tg{display:flex;justify-content:space-between;align-items:center;padding:11px 0;border-top:1px solid var(--line);font-size:14px}
.tg.first{border-top:none}
.sl{position:relative;width:46px;height:26px;flex:none;display:inline-block}
.sl input{opacity:0;width:0;height:0}
.sl i{position:absolute;top:0;left:0;right:0;bottom:0;background:#2a3246;border-radius:26px;cursor:pointer}
.sl i:before{content:"";position:absolute;width:20px;height:20px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
.sl input:checked+i{background:var(--accent)}
.sl input:checked+i:before{transform:translateX(20px)}
.btn{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#06101a;border:none;border-radius:10px;padding:11px 16px;font-weight:700;font-size:13px;cursor:pointer;margin:4px 4px 4px 0}
.btn.ghost{background:transparent;color:var(--accent);border:1px solid var(--accent)}
.mini{background:transparent;color:var(--accent);border:1px solid var(--accent);padding:8px 14px;border-radius:20px;font-weight:700;font-size:12px;cursor:pointer;margin:3px}
.mini.on{background:var(--accent);color:#06101a}
.chips{display:flex;flex-wrap:wrap;gap:6px;justify-content:center;margin-bottom:10px}
canvas.wheel{display:block;margin:0 auto;border-radius:50%}
.swatch{height:26px;border-radius:8px;margin-top:12px;border:1px solid var(--line);background:#f00}
.pal{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin:10px 0}
.dot{width:32px;height:32px;border-radius:50%;border:2px solid #334155;cursor:pointer}
#cv{display:block;margin:10px auto;touch-action:none;border-radius:8px;border:1px solid var(--line)}
.big{font-size:22px;font-weight:700;letter-spacing:1px;text-align:center;margin:6px 0 12px}
.foot{text-align:center;margin:22px 0 10px;font-size:12px;color:var(--dim)}
.foot a{color:var(--accent);text-decoration:none;border:1px solid var(--accent);border-radius:20px;padding:8px 18px;margin-left:8px}
#toast{position:fixed;bottom:22px;left:50%;transform:translateX(-50%);background:#0d111b;border:1px solid var(--accent);padding:10px 18px;border-radius:12px;font-size:13px;opacity:0;transition:.3s;pointer-events:none;z-index:9}
#toast.show{opacity:1}
</style>
</head>
<body>

<div class="top">
  <div class="brand">POV HOLOGRAM<small>CONTROL CENTER</small></div>
  <div class="pills">
    <div class="pill">RPM<b id="rpm">0</b></div>
    <div class="pill">BATTERY<b id="battery">--</b></div>
    <div class="pill">JITTER<b id="jit">--</b></div>
    <div class="pill">QUALITY<b id="qual">--</b></div>
    <div class="pill">TIME<b id="clk">--</b></div>
  </div>
</div>

<div class="wrap">

  <div class="seg">
    <div id="m0" onclick="setMode(0,true)">TEXT</div>
    <div id="m1" onclick="setMode(1,true)">CANVAS</div>
    <div id="m2" onclick="setMode(2,true)">DIGITAL WATCH</div>
    <div id="m3" onclick="setMode(3,true)">ANALOG WATCH</div>
    <div id="m4" onclick="setMode(4,true)">DATE & DAY</div>
    <div id="m5" onclick="setMode(5,true)">SHAPES</div>
    <div id="m6" onclick="setMode(6,true)">AUTO</div>
    <div id="m7" onclick="setMode(7,true)">SENTENCE</div>
  </div>

  <div class="grid">

    <!-- DISPLAY SETTINGS -->
    <div class="card" data-m="0123457">
      <div class="ct"><h3>Display</h3><span class="tag" id="modeTag">TEXT</span></div>
      <div class="row"><span>Inner LEDs off</span><span class="v" id="offV">3 - uses 5 LEDs</span></div>
      <input type="range" id="offSlider" min="0" max="3" value="3" oninput="updateOff(this.value,false)" onchange="updateOff(this.value,true)">
      <div class="row"><span>Position angle</span><span class="v" id="angleV">90&deg;</span></div>
      <input type="range" id="angleSlider" min="0" max="360" value="90" oninput="updateAngle(this.value,false)" onchange="updateAngle(this.value,true)">
      <div class="tg first"><span>Horizontal flip</span><label class="sl"><input type="checkbox" id="hflip" onchange="updateFlip('h',this.checked)"><i></i></label></div>
      <div class="tg"><span>Vertical flip</span><label class="sl"><input type="checkbox" id="vflip" onchange="updateFlip('v',this.checked)"><i></i></label></div>
      <div class="tg" style="margin-top:12px; border-top:1px dashed var(--accent)">
         <span style="color:var(--text);font-weight:bold;">Include in AUTO cycle</span>
         <label class="sl"><input type="checkbox" id="autoEnCb" onchange="updateAutoEn(this.checked)"><i></i></label>
      </div>
      <p class="note" id="modeNote"></p>
    </div>

    <!-- COLOUR -->
    <div class="card" data-m="0123457">
      <div class="ct"><h3 id="colorTitle">Text color</h3></div>
      
      <div class="chips" data-m="2" style="display:none">
        <button class="mini on" id="cdHour" onclick="setATarget('dHour')">Hour</button>
        <button class="mini" id="cdMin" onclick="setATarget('dMin')">Minute</button>
        <button class="mini" id="cdSec" onclick="setATarget('dSec')">Second</button>
        <button class="mini" id="cdDot" onclick="setATarget('dDot')">Dot</button>
      </div>
      
      <div class="chips" data-m="3" style="display:none">
        <button class="mini on" id="cHour" onclick="setATarget('hour')">Hour</button>
        <button class="mini" id="cMin" onclick="setATarget('min')">Minute</button>
        <button class="mini" id="cSec" onclick="setATarget('sec')">Second</button>
        <button class="mini" id="cTick" onclick="setATarget('tick')">Marks</button>
      </div>

      <div class="chips" data-m="4" style="display:none">
        <button class="mini on" id="cyDay" onclick="setATarget('day')">Day Text (SAT)</button>
        <button class="mini" id="cyDtNum" onclick="setATarget('dtNum')">Date (03)</button>
        <button class="mini" id="cyDtMon" onclick="setATarget('dtMon')">Month (OCT)</button>
      </div>

      <div class="chips" data-m="5" style="display:none">
        <button class="mini on" id="csHeart" onclick="setATarget('heart')">Heart Color</button>
        <button class="mini" id="csSmiley" onclick="setATarget('smiley')">Smiley Color</button>
      </div>

      <canvas id="wheel" class="wheel" width="180" height="180"></canvas>
      <div class="swatch" id="swatch"></div>
    </div>

    <!-- AUTO MODE SETTINGS -->
    <div class="card" data-m="6">
      <div class="ct"><h3>Automatic Mode</h3></div>
      <div class="row"><span>Switch every</span><span class="v" id="aintervV">5 sec</span></div>
      <input type="range" id="aintervSlider" min="2" max="60" value="5" oninput="updateAutoInt(this.value,false)" onchange="updateAutoInt(this.value,true)">
      <p class="note">Automatically cycles smoothly through your selected modes. Use the checkbox in each mode's display settings to include or exclude it from this auto loop.</p>
    </div>

    <!-- SHAPES MODE SETTINGS -->
    <div class="card" data-m="5">
      <div class="ct"><h3>Select Shape</h3></div>
      <div class="tg first"><span>Auto-cycle shapes</span><label class="sl"><input type="checkbox" id="shAuto" onchange="updateToggle('/set_shAuto',this.checked)"><i></i></label></div>
      <div class="chips">
        <button class="mini on" id="shp0" onclick="setShape(0)">💖 Heart</button>
        <button class="mini" id="shp1" onclick="setShape(1)">😊 Smiley</button>
      </div>
      <div id="heartControls" style="display:none; margin-top:10px;">
          <div class="row"><span>Heart Width</span><span class="v" id="hwV">1.0x</span></div>
          <input type="range" id="hwSlider" min="5" max="20" value="10" oninput="updateHScale('w',this.value,false)" onchange="updateHScale('w',this.value,true)">
          <div class="row"><span>Heart Height</span><span class="v" id="hhV">1.0x</span></div>
          <input type="range" id="hhSlider" min="5" max="20" value="10" oninput="updateHScale('h',this.value,false)" onchange="updateHScale('h',this.value,true)">
      </div>
      <p class="note">Giant rotating polar emojis. Enable Auto-cycle to switch shapes automatically with a smooth fade.</p>
    </div>

    <!-- DATE & DAY -->
    <div class="card" data-m="4">
      <div class="ct"><h3>Date & Day</h3></div>
      <div class="tg first"><span>Auto-cycle Date & Day</span><label class="sl"><input type="checkbox" id="dtAuto" onchange="updateToggle('/set_dtAuto',this.checked)"><i></i></label></div>
      <div class="chips">
        <button class="mini on" id="dt0" onclick="setDt(0)">Day (SAT)</button>
        <button class="mini" id="dt1" onclick="setDt(1)">Date (03 OCT)</button>
      </div>
      <div class="row"><span id="dayGapLbl">Day letter gap</span><span class="v" id="dayGapV">2 columns</span></div>
      <input type="range" id="dayGapSlider" min="1" max="4" value="2" oninput="updateGapVal('daygap',this.value,false)" onchange="updateGapVal('daygap',this.value,true)">
      <div class="row"><span id="dateGapLbl">Date letter gap</span><span class="v" id="dateGapV">2 columns</span></div>
      <input type="range" id="dateGapSlider" min="1" max="4" value="2" oninput="updateGapVal('dategap',this.value,false)" onchange="updateGapVal('dategap',this.value,true)">
      <div class="row"><span>Word gap (Date space)</span><span class="v" id="dtWordGapV">4 columns</span></div>
      <input type="range" id="dtWordGapSlider" min="1" max="10" value="4" oninput="updateWordGap(this.value,false)" onchange="updateWordGap(this.value,true)">
      <p class="note">Displays Day in short format and Date in two digit format.</p>
    </div>

    <!-- TEXT -->
    <div class="card" data-m="0">
      <div class="ct"><h3>Text</h3><span class="tag">Maximum allowed: <span id="maxLenLbl">7</span> characters based on display width</span></div>
      <input type="text" id="textInput" maxlength="7" value="HELLO" oninput="sendText()">
      <div class="row"><span>Letter gap</span><span class="v" id="gapV">2 columns</span></div>
      <input type="range" id="gapSlider" min="1" max="4" value="2" oninput="updateGap(this.value,false)" onchange="updateGap(this.value,true)">
      <p class="note">Capital and small letters, numbers, space, colon and minus work.</p>
    </div>

    <div class="card" data-m="0">
      <div class="ct"><h3>Scroll animation</h3></div>
      <div class="tg first"><span>Auto-scroll (enter / hold / exit)</span><label class="sl"><input type="checkbox" id="scrollTgl" onchange="updateScroll(this.checked)"><i></i></label></div>
      <div class="row"><span>Speed</span><span class="v" id="speedV">5</span></div>
      <input type="range" id="speedSlider" min="1" max="10" value="5" oninput="updateSpeed(this.value,false)" onchange="updateSpeed(this.value,true)">
      <div class="row"><span>Hold time</span><span class="v" id="holdV">3 sec</span></div>
      <input type="range" id="holdSlider" min="1" max="6" value="3" oninput="updateHold(this.value,false)" onchange="updateHold(this.value,true)">
    </div>

    <!-- SENTENCE SCROLLING -->
    <div class="card" data-m="7">
      <div class="ct"><h3>Sentence Scrolling</h3><span class="tag">max 100 characters</span></div>
      <textarea id="sentInput" maxlength="100" rows="2" oninput="sendSentence()">Welcome to Government Engineering College Rajkot</textarea>
      <div class="row"><span>Scroll speed</span><span class="v" id="sentSpeedV">5</span></div>
      <input type="range" id="sentSpeedSlider" min="1" max="10" value="5" oninput="updateSentSpeed(this.value,false)" onchange="updateSentSpeed(this.value,true)">
      <p class="note">Enter a complete sentence to scroll across the front viewing aperture. Uses limited angle range so no 360-degree circular distortion occurs. Position angle, direction/flip, inner LEDs off, and color can be adjusted.</p>
    </div>

    <!-- DIGITAL CLOCK -->
    <div class="card" data-m="2">
      <div class="ct"><h3>Digital watch</h3></div>
      <div class="tg first"><span>24 hour format</span><label class="sl"><input type="checkbox" id="t24" onchange="updateToggle('/set_24h',this.checked)"><i></i></label></div>
      <div class="tg"><span>Show seconds</span><label class="sl"><input type="checkbox" id="dsec" onchange="updateToggle('/set_dsec',this.checked)"><i></i></label></div>
      <div class="row"><span>Digit gap</span><span class="v" id="dgapV">1 column</span></div>
      <input type="range" id="dgapSlider" min="1" max="3" value="1" oninput="updateDGap(this.value,false)" onchange="updateDGap(this.value,true)">
    </div>

    <!-- ANALOG CLOCK -->
    <div class="card" data-m="3">
      <div class="ct"><h3>Analog watch</h3></div>
      <div class="tg first"><span>Second hand</span><label class="sl"><input type="checkbox" id="asec" onchange="updateToggle('/set_asec',this.checked)"><i></i></label></div>
      <div class="tg"><span>Hour marks</span><label class="sl"><input type="checkbox" id="aticks" onchange="updateToggle('/set_aticks',this.checked)"><i></i></label></div>
      <p class="note">12 o'clock faces Position angle. If clock runs wrong way, use Horizontal flip.</p>
    </div>

    <!-- TIME -->
    <div class="card" data-m="2346">
      <div class="ct"><h3>Time</h3><span class="tag" id="netInfo">--</span></div>
      <div class="big" id="clkBig">Time not set</div>
      <button class="btn" onclick="syncPhone()">Sync with phone</button>
      <div class="row"><span>Set manually</span></div>
      <input type="datetime-local" id="dt">
      <button class="btn ghost" onclick="setManual()">Set time</button>
      <div class="row"><span>Time zone</span></div>
      <select id="tz" onchange="setTz(this.value)">
        <option value="-480">UTC-8</option><option value="-300">UTC-5</option><option value="0">UTC</option>
        <option value="60">UTC+1</option><option value="120">UTC+2</option><option value="180">UTC+3</option>
        <option value="240">UTC+4</option><option value="330">UTC+5:30 (India)</option><option value="360">UTC+6</option>
        <option value="420">UTC+7</option><option value="480">UTC+8</option><option value="540">UTC+9</option>
        <option value="600">UTC+10</option>
      </select>
      <p class="note">If WiFi has internet, time is set automatically via NTP. Without internet, press "Sync with phone" after power-on.</p>
    </div>

    <!-- CANVAS -->
    <div class="card wide" data-m="1" id="cvCard">
      <div class="ct"><h3>Canvas editor</h3><span class="tag">live</span></div>
      <div style="text-align:center">
        <button class="mini on" id="tPen" onclick="setTool('pen')">Pen</button>
        <button class="mini" id="tErase" onclick="setTool('erase')">Eraser</button>
        <button class="mini" id="tFill" onclick="setTool('fill')">Bucket</button>
        <button class="mini on" id="vFlat" onclick="setView('flat')">Flat</button>
        <button class="mini" id="vPolar" onclick="setView('polar')">Circle</button>
      </div>
      <div class="row"><span>Brush size</span><span class="v" id="brushVal">1</span></div>
      <input type="range" id="brush" min="1" max="3" value="1" oninput="setBrush(this.value)">
      <div class="pal" id="pal"></div>
      <div id="cvbox"><canvas id="cv"></canvas></div>
      <div id="panRow">
        <div class="row"><span>Columns</span><span class="v" id="panLbl">0-24</span></div>
        <input type="range" id="pan" min="0" max="75" value="0" oninput="setPan(this.value)">
      </div>
      <div style="text-align:center">
        <button class="btn ghost" onclick="fillAll()">Fill all</button>
        <button class="btn ghost" onclick="clearAll()">Clear all</button>
      </div>
      <p class="note">Everything you draw goes live to display. Pick pen colour on colour wheel or circles; old pixels keep their colour. Column 50 faces front.</p>
      <p class="note" id="cvInfo"></p>
    </div>

    <!-- SYSTEM -->
    <div class="card">
      <div class="ct"><h3>System</h3></div>
      <div class="row"><span>Brightness</span><span class="v" id="brightV">150</span></div>
      <input type="range" id="brightSlider" min="10" max="255" value="150" oninput="updateBright(this.value,false)" onchange="updateBright(this.value,true)">
      <p class="note">Brightness is shared by all modes. All other display settings are saved separately for every mode.</p>
      <div class="row"><span>Text width (all modes)</span><span class="v" id="widthV">100%</span></div>
      <input type="range" id="widthSlider" min="40" max="200" step="5" value="100" oninput="updateWidth(this.value,false)" onchange="updateWidth(this.value,true)">
      <p class="note">Fixed width: it does not change with RPM. Used by Text, Sentence, Digital watch and Date &amp; Day.</p>
      <p class="note" id="widthNote"></p>
      <p class="note" id="rpmNote"></p>
    </div>

  </div>

  <div class="foot">Firmware updates <a href="/update">OTA Update</a></div>
</div>

<div id="toast"></div>

<script>
const $=id=>document.getElementById(id);
const MODES=["TEXT","CANVAS","DIGITAL WATCH","ANALOG WATCH","DATE & DAY","SHAPES","AUTOMATIC","SENTENCE"];
const QN=["STOPPED","TOO SLOW","TOO FAST","GOOD"];
const COLS=100,ROWS=8,VIS=25,HALF=50;
let mode=0,S=null,activeRows=5,penColor=0xff0000,tool="pen",brush=1,view="flat",aTarget="hour",viewStart=0;
let grid=new Array(COLS*ROWS).fill(0),pend={},sending=false,drawing=false,last=null,raf=0;
let busyUntil=0,lastSent=0,dragging=false,pick={x:170,y:90},wheelImg=null,dtInit=false;
const wheel=$("wheel"),wctx=wheel.getContext("2d");
const cv=$("cv"),cx=cv.getContext("2d");

function busy(){busyUntil=Date.now()+2000;}
function sendLive(u){const t=Date.now();if(t-lastSent>150){lastSent=t;fetch(u);}}
function setVal(u,v,done){busy();const f=u+v;if(done)fetch(f);else sendLive(f);}
function hx(n,l){return n.toString(16).padStart(l,"0");}
function css(v){return "#"+hx(v,6);}
function toast(t){const e=$("toast");e.innerText=t;e.classList.add("show");setTimeout(()=>e.classList.remove("show"),1800);}
function localISO(d){const p=n=>String(n).padStart(2,"0");return d.getFullYear()+"-"+p(d.getMonth()+1)+"-"+p(d.getDate())+"T"+p(d.getHours())+":"+p(d.getMinutes());}

function updateDynamicLabels() {
    const dObj = new Date();
    const dNum = String(dObj.getDate()).padStart(2, '0');
    const months = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"];
    const dMon = months[dObj.getMonth()];
    const days = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"];
    const dDay = days[dObj.getDay()];

    if($("dt0")) $("dt0").innerText = "Day (" + dDay + ")";
    if($("dt1")) $("dt1").innerText = "Date (" + dNum + " " + dMon + ")";
    if($("cyDay")) $("cyDay").innerText = "Day Text (" + dDay + ")";
    if($("cyDtNum")) $("cyDtNum").innerText = "Date (" + dNum + ")";
    if($("cyDtMon")) $("cyDtMon").innerText = "Month (" + dMon + ")";
    
    if($("dayGapLbl")) $("dayGapLbl").innerText = "Day letter gap (" + dDay + ")";
    if($("dateGapLbl")) $("dateGapLbl").innerText = "Date letter gap (" + dNum + ")";
}
setInterval(updateDynamicLabels, 60000);

/* ---------- colour wheel ---------- */
function hsv2rgb(h,s,v){
  const c=v*s,x=c*(1-Math.abs((h/60)%2-1)),m=v-c;
  let r=0,g=0,b=0;
  if(h<60){r=c;g=x;}else if(h<120){r=x;g=c;}else if(h<180){g=c;b=x;}
  else if(h<240){g=x;b=c;}else if(h<300){r=x;b=c;}else{r=c;b=x;}
  return [Math.round((r+m)*255),Math.round((g+m)*255),Math.round((b+m)*255)];
}
function toHex(c){return c.map(v=>hx(v,2)).join("");}
function posFromHex(hex){
  const r=parseInt(hex.substr(0,2),16)/255,g=parseInt(hex.substr(2,2),16)/255,b=parseInt(hex.substr(4,2),16)/255;
  const mx=Math.max(r,g,b),mn=Math.min(r,g,b),d=mx-mn;
  let h=0;
  if(d>0){if(mx===r)h=60*(((g-b)/d+6)%6);else if(mx===g)h=60*((b-r)/d+2);else h=60*((r-g)/d+4);}
  const s=(mx===0)?0:d/mx,a=h*Math.PI/180;
  return {x:90+Math.cos(a)*s*80,y:90+Math.sin(a)*s*80};
}
function drawWheel(){
  const img=wctx.createImageData(180,180);
  for(let y=0;y<180;y++){
    for(let x=0;x<180;x++){
      const dx=x-90,dy=y-90,d=Math.sqrt(dx*dx+dy*dy);
      if(d<=85){
        let h=Math.atan2(dy,dx)*180/Math.PI;if(h<0)h+=360;
        const c=hsv2rgb(h,d/85,1),i=(y*180+x)*4;
        img.data[i]=c[0];img.data[i+1]=c[1];img.data[i+2]=c[2];img.data[i+3]=255;
      }
    }
  }
  wheelImg=img;paintWheel();
}
function paintWheel(){
  wctx.clearRect(0,0,180,180);wctx.putImageData(wheelImg,0,0);
  wctx.beginPath();wctx.arc(pick.x,pick.y,6,0,6.2832);wctx.lineWidth=3;wctx.strokeStyle="#141824";wctx.stroke();
  wctx.beginPath();wctx.arc(pick.x,pick.y,6,0,6.2832);wctx.lineWidth=1.5;wctx.strokeStyle="#fff";wctx.stroke();
}
function colorTarget(){
  if(mode===0) return "text";
  if(mode===1) return "pen";
  if(mode===7) return "sent";
  return aTarget; 
}
function colorHex(t){
  if(t==="pen")return hx(penColor,6);
  if(!S)return "ffffff";
  const m={
    text:S.ct, sent:S.cST, dHour:S.dh, dMin:S.dm, dSec:S.ds, dDot:S.dd,
    hour:S.ch, min:S.cm, sec:S.cs, tick:S.ck,
    day:S.cDY, dtNum:S.cDN, dtMon:S.cDM, heart:S.cHT, smiley:S.cSM
  };
  return m[t]||"ffffff";
}
function syncWheel(){
  const h=colorHex(colorTarget());
  pick=posFromHex(h);
  $("swatch").style.background="#"+h;
  if(wheelImg)paintWheel();
}
function pickAt(e,done){
  const rect=wheel.getBoundingClientRect(),k=180/rect.width;
  let x=(e.clientX-rect.left)*k-90,y=(e.clientY-rect.top)*k-90;
  let d=Math.sqrt(x*x+y*y);
  if(d>80){x*=80/d;y*=80/d;d=80;}
  pick={x:90+x,y:90+y};
  let h=Math.atan2(y,x)*180/Math.PI;if(h<0)h+=360;
  const hex=toHex(hsv2rgb(h,d/80,1));
  $("swatch").style.background="#"+hex;
  paintWheel();
  const t=colorTarget();
  if(t==="pen"){penColor=parseInt(hex,16);}
  else{setVal("/set_color?t="+t+"&val=",hex,done);}
}
wheel.addEventListener("pointerdown",e=>{dragging=true;wheel.setPointerCapture(e.pointerId);pickAt(e,false);});
wheel.addEventListener("pointermove",e=>{if(dragging)pickAt(e,false);});
wheel.addEventListener("pointerup",e=>{if(dragging){pickAt(e,true);dragging=false;}});
wheel.addEventListener("pointercancel",()=>{dragging=false;});

function setATarget(t){
  aTarget=t;
  if($("cHour")) $("cHour").className="mini"+(t==="hour"?" on":"");
  if($("cMin")) $("cMin").className="mini"+(t==="min"?" on":"");
  if($("cSec")) $("cSec").className="mini"+(t==="sec"?" on":"");
  if($("cTick")) $("cTick").className="mini"+(t==="tick"?" on":"");
  
  if($("cdHour")) $("cdHour").className="mini"+(t==="dHour"?" on":"");
  if($("cdMin")) $("cdMin").className="mini"+(t==="dMin"?" on":"");
  if($("cdSec")) $("cdSec").className="mini"+(t==="dSec"?" on":"");
  if($("cdDot")) $("cdDot").className="mini"+(t==="dDot"?" on":"");

  if($("cyDay")) $("cyDay").className="mini"+(t==="day"?" on":"");
  if($("cyDtNum")) $("cyDtNum").className="mini"+(t==="dtNum"?" on":"");
  if($("cyDtMon")) $("cyDtMon").className="mini"+(t==="dtMon"?" on":"");
  
  if($("csHeart")) $("csHeart").className="mini"+(t==="heart"?" on":"");
  if($("csSmiley")) $("csSmiley").className="mini"+(t==="smiley"?" on":"");

  syncWheel();
}

function setShape(s){
  for(let i=0;i<2;i++){ if($("shp"+i)) $("shp"+i).className="mini"+(i===s?" on":""); }
  if($("heartControls")) $("heartControls").style.display = (s===0) ? "block" : "none";
  busy();fetch("/set_shape?val="+s).then(updateStatus);
}
function setDt(s){
  for(let i=0;i<2;i++){ if($("dt"+i)) $("dt"+i).className="mini"+(i===s?" on":""); }
  busy();fetch("/set_showDt?val="+s).then(updateStatus);
}

function updateHScale(axis, v, d) {
    $(axis === 'w' ? 'hwV' : 'hhV').innerText = (v/10).toFixed(1) + "x";
    setVal("/set_hscale?" + axis + "=", v, d);
}

function setPen(h){
  penColor=parseInt(h,16);
  pick=posFromHex(h);
  $("swatch").style.background="#"+h;
  paintWheel();
}
function buildPalette(){
  ["ff0000","ff7f00","ffff00","00ff00","00ffff","0000ff","8000ff","ff00ff","ffffff"].forEach(h=>{
    const d=document.createElement("div");
    d.className="dot";d.style.background="#"+h;d.onclick=()=>setPen(h);
    $("pal").appendChild(d);
  });
}

/* ---------- controls ---------- */
function sendText(){busy();fetch("/set_text?val="+encodeURIComponent($("textInput").value));}
function sendSentence(){busy();fetch("/set_sentence?val="+encodeURIComponent($("sentInput").value));}
function updateBright(v,d){$("brightV").innerText=v;setVal("/set_brightness?val=",v,d);}
function updateWidth(v,d){$("widthV").innerText=v+"%";setVal("/set_width?val=",v,d);}
function updateAngle(v,d){
  $("angleV").innerHTML=v+"&deg;";
  if(mode===7 && parseInt(v)>180){ v="180"; $("angleSlider").value=180; $("angleV").innerHTML="180&deg;"; }
  setVal("/set_angle?val=",v,d);
}
function updateGap(v,d){$("gapV").innerText=v+(v==1?" column":" columns");setVal("/set_gap?val=",v,d);}
function updateDGap(v,d){$("dgapV").innerText=v+(v==1?" column":" columns");setVal("/set_dgap?val=",v,d);}
function updateGapVal(n,v,d){
  $(n+"V").innerText=v+(v==1?" column":" columns");
  setVal("/set_"+n+"?val=",v,d);
  if(n === 'daygap') setDt(0);
  if(n === 'dategap') setDt(1);
}
function updateWordGap(v,d){
  $("dtWordGapV").innerText=v+(v==1?" column":" columns");
  setVal("/set_dtWordGap?val=",v,d);
  setDt(1);
}
function updateSpeed(v,d){$("speedV").innerText=v;setVal("/set_speed?val=",v,d);}
function updateSentSpeed(v,d){$("sentSpeedV").innerText=v;setVal("/set_sentSpeed?val=",v,d);}
function updateHold(v,d){$("holdV").innerText=v+" sec";setVal("/set_hold?val=",v,d);}
function updateAutoInt(v,d){$("aintervV").innerText=v+" sec";setVal("/set_auto_sec?val=",v,d);}
function updateAutoEn(v){busy();fetch("/set_autoEn?m="+mode+"&val="+v);}
function offLabel(v){return v+" - uses "+(8-v)+" LEDs";}
function updateOff(v,d){
  $("offV").innerText=offLabel(v);
  activeRows=8-v;if(mode===1)redraw();
  setVal("/set_off?val=",v,d);
}
function updateFlip(w,v){busy();fetch("/set_"+w+"flip?val="+v);}
function updateScroll(v){busy();fetch("/set_scroll?val="+v);}
function updateToggle(u,v){busy();fetch(u+"?val="+v);}

/* ---------- time ---------- */
function syncPhone(){
  const n=new Date();
  fetch("/set_time?epoch="+Math.floor(n.getTime()/1000)+"&tz="+(-n.getTimezoneOffset()))
    .then(()=>{toast("Time synced with phone");setTimeout(updateStatus,400);});
}
function setManual(){
  const v=$("dt").value;
  if(!v){toast("Pick date and time first");return;}
  const d=new Date(v);
  fetch("/set_time?epoch="+Math.floor(d.getTime()/1000)+"&tz="+(-d.getTimezoneOffset()))
    .then(()=>{toast("Time set");setTimeout(updateStatus,400);});
}
function setTz(v){busy();fetch("/set_time?tz="+v).then(()=>{toast("Time zone saved");setTimeout(updateStatus,400);});}

/* ---------- mode switch ---------- */
function setMode(m,send){
  mode=m;
  for(let i=0;i<8;i++){ const el=$("m"+i); if(el) el.className=(i===m)?"on":""; }
  document.querySelectorAll("[data-m]").forEach(e=>{e.style.display=(e.dataset.m.indexOf(String(m))>=0)?"":"none";});
  if($("modeTag")) $("modeTag").innerText=(m<8)?MODES[m]:"";
  if($("modeNote")) $("modeNote").innerText="Display settings are saved for "+(m<8?MODES[m]:"")+" mode.";
  
  if(S && m<8) $("autoEnCb").checked = S["ae"+m];

  const titles = ["Text color","Pen color","Digital watch colors","Analog watch colors","Date & Day colors", "Shape colors","", "Sentence color"];
  if($("colorTitle")) $("colorTitle").innerText=titles[m];
  
  if(m===2) setATarget("dHour");
  if(m===3) setATarget("hour");
  if(m===4) setATarget("day");
  if(m===5) setATarget("heart");

  // Clamped angle slider for sentence mode
  if(m===7) {
    $("angleSlider").max=180;
    if(parseInt($("angleSlider").value)>180) $("angleSlider").value=180;
  } else {
    $("angleSlider").max=360;
  }

  if(send){busy();fetch("/set_mode?val="+m).then(()=>{busyUntil=0;updateStatus();});}
  if(m===1){sizeCanvas();loadGrid();}
  syncWheel();
}

/* ---------- canvas editor ---------- */
function setTool(t){
  tool=t;
  $("tPen").className="mini"+(t==="pen"?" on":"");
  $("tErase").className="mini"+(t==="erase"?" on":"");
  $("tFill").className="mini"+(t==="fill"?" on":"");
}
function setBrush(v){brush=parseInt(v);$("brushVal").innerText=v;}
function setView(v){
  view=v;
  $("vFlat").className="mini"+(v==="flat"?" on":"");
  $("vPolar").className="mini"+(v==="polar"?" on":"");
  $("panRow").style.display=(v==="flat")?"block":"none";
  sizeCanvas();
}
function setPan(v){
  viewStart=parseInt(v);
  $("panLbl").innerText=viewStart+"-"+(viewStart+VIS-1);
  redraw();
}
function sizeCanvas(){
  const w=Math.max(220,Math.min($("cvbox").clientWidth,720));
  cv.width=w;
  cv.height=(view==="polar")?w:Math.round(w/VIS*1.5*ROWS);
  cv.style.width=cv.width+"px";cv.style.height=cv.height+"px";
  drawEditor();
}
function drawEditor(){
  const W=cv.width,H=cv.height;
  cx.fillStyle="#0d111b";cx.fillRect(0,0,W,H);
  if(view==="flat"){
    const cw=W/VIS,ch=H/ROWS;
    cx.font=Math.max(9,Math.round(cw*0.45))+"px sans-serif";
    for(let k=0;k<VIS;k++){
      const c=viewStart+k;
      for(let i=0;i<ROWS;i++){
        const v=grid[c*ROWS+i];
        cx.fillStyle=(i>=activeRows)?"#11141c":(v?css(v):(((c+i)%2)?"#1b1f2c":"#202538"));
        cx.fillRect(k*cw+1,i*ch+1,cw-2,ch-2);
      }
      if(c%5===0){cx.fillStyle="#94a3b8";cx.fillText(c,k*cw+2,ch*ROWS-3);}
    }
  }else{
    const cxm=W/2,cym=H/2,Ro=W/2-2,Ri=Ro*0.28,th=(Ro-Ri)/ROWS,da=2*Math.PI/COLS;
    for(let c=0;c<COLS;c++){
      const s=(c+HALF)%COLS,a1=s*da-Math.PI/2,a2=(s+1)*da-Math.PI/2;
      for(let i=0;i<ROWS;i++){
        const v=grid[c*ROWS+i],r2=Ro-i*th,r1=r2-th;
        cx.beginPath();cx.arc(cxm,cym,r2,a1,a2);cx.arc(cxm,cym,r1,a2,a1,true);cx.closePath();
        cx.fillStyle=(i>=activeRows)?"#11141c":(v?css(v):(((c+i)%2)?"#1b1f2c":"#202538"));
        cx.fill();
      }
    }
  }
}
function redraw(){
  if(!raf){raf=requestAnimationFrame(()=>{raf=0;drawEditor();});}
}
function cellAt(e){
  const r=cv.getBoundingClientRect();
  const x=(e.clientX-r.left)*(cv.width/r.width),y=(e.clientY-r.top)*(cv.height/r.height);
  if(view==="flat"){
    const cw=cv.width/VIS,ch=cv.height/ROWS;
    if(x<0||x>=cv.width||y<0||y>=cv.height)return null;
    return {c:viewStart+Math.floor(x/cw),i:Math.floor(y/ch)};
  }
  const W=cv.width,dx=x-W/2,dy=y-W/2,d=Math.sqrt(dx*dx+dy*dy);
  const Ro=W/2-2,Ri=Ro*0.28,th=(Ro-Ri)/ROWS;
  if(d<Ri||d>Ro)return null;
  let deg=Math.atan2(dy,dx)*180/Math.PI+90;
  if(deg<0)deg+=360;if(deg>=360)deg-=360;
  const s=Math.floor(deg/(360/COLS))%COLS;
  return {c:(s-HALF+COLS)%COLS,i:Math.min(Math.floor((Ro-d)/th),ROWS-1)};
}
function setPx(c,i,v){
  if(i<0||i>=activeRows)return;
  c=(c+COLS)%COLS;
  const k=c*ROWS+i;
  if(grid[k]!==v){grid[k]=v;pend[k]=v;}
}
function applyBrush(c,i){
  const v=(tool==="erase")?0:penColor,b=brush-1;
  for(let dc=-b;dc<=b;dc++)for(let di=-b;di<=b;di++)setPx(c+dc,i+di,v);
}
function stroke(cell){
  if(last===null){applyBrush(cell.c,cell.i);}
  else{
    let dc=cell.c-last.c;
    if(view==="polar"){if(dc>COLS/2)dc-=COLS;if(dc<-COLS/2)dc+=COLS;}
    const di=cell.i-last.i,n=Math.max(Math.abs(dc),Math.abs(di),1);
    for(let s=1;s<=n;s++){
      applyBrush((last.c+Math.round(dc*s/n)+COLS)%COLS,last.i+Math.round(di*s/n));
    }
  }
  last=cell;
}
function floodFill(cell){
  if(cell.i<0||cell.i>=activeRows)return;
  const target=grid[((cell.c+COLS)%COLS)*ROWS+cell.i],repl=penColor;
  if(target===repl)return;
  const stack=[[cell.c,cell.i]];
  while(stack.length){
    const p=stack.pop(),i=p[1],c=(p[0]+COLS)%COLS;
    if(i<0||i>=activeRows)continue;
    const k=c*ROWS+i;
    if(grid[k]!==target)continue;
    grid[k]=repl;pend[k]=repl;
    stack.push([c+1,i],[c-1,i],[c,i+1],[c,i-1]);
  }
  redraw();
}
function fillAll(){
  for(let c=0;c<COLS;c++)for(let i=0;i<activeRows;i++)setPx(c,i,penColor);
  redraw();
}
function clearAll(){
  grid.fill(0);pend={};redraw();
  fetch("/cv_clear");
}
cv.addEventListener("pointerdown",e=>{
  const cell=cellAt(e);if(!cell)return;
  cv.setPointerCapture(e.pointerId);
  if(tool==="fill"){floodFill(cell);return;}
  drawing=true;last=null;stroke(cell);redraw();
});
cv.addEventListener("pointermove",e=>{
  if(!drawing)return;
  const cell=cellAt(e);
  if(!cell){last=null;return;}
  stroke(cell);redraw();
});
const endDraw=()=>{drawing=false;last=null;};
cv.addEventListener("pointerup",endDraw);
cv.addEventListener("pointercancel",endDraw);

function flush(){
  if(sending)return;
  const keys=Object.keys(pend);
  if(!keys.length)return;
  const part=keys.slice(0,150),sent=[];
  let body="";
  for(const ks of part){
    const k=parseInt(ks),v=pend[ks];
    body+=hx(Math.floor(k/ROWS),2)+(k%ROWS).toString(16)+hx(v,6);
    sent.push([k,v]);
    delete pend[ks];
  }
  sending=true;
  fetch("/cv_px",{method:"POST",body:body})
    .catch(()=>{for(const s of sent){if(pend[s[0]]===undefined)pend[s[0]]=s[1];}})
    .finally(()=>{sending=false;});
}
function loadGrid(){
  fetch("/cv_get").then(r=>r.text()).then(t=>{
    if(t.length<COLS*ROWS*6)return;
    for(let k=0;k<COLS*ROWS;k++)grid[k]=parseInt(t.substr(k*6,6),16);
    drawEditor();
  }).catch(()=>{});
}

/* ---------- live status ---------- */
function updateStatus(){
  fetch("/status").then(r=>r.json()).then(d=>{
    S=d;
    $("rpm").innerText=d.rpm;
    $("battery").innerText=d.battery+"%";
    $("jit").innerText=Number(d.jitter).toFixed(1)+"%";
    $("clk").innerText=d.timeok?d.clk:"not set";
    $("clkBig").innerText=d.timeok?(d.date+"   "+d.clk):"Time not set";
    $("netInfo").innerText=(d.ap?"Hotspot ":"WiFi ")+d.ip;
    $("cvInfo").innerText=(d.q===2)?"RPM is too high - some columns are skipped. Lower RPM.":"";
    $("qual").innerText=QN[d.q]||"--";
    let rn="";
    if(d.q===0) rn="Rotor stopped or below 300 RPM.";
    else if(d.maxrpm>d.goodrpm) rn="Best RPM at this width: "+d.goodrpm+" to "+d.maxrpm+" (now "+d.rpm+").";
    else rn="At this width max RPM is only "+d.maxrpm+". Make text wider or lower RPM.";
    $("rpmNote").innerText=rn;
    $("widthNote").innerText="One column = "+(d.cdeg/10).toFixed(1)+" deg. Current text spans "+d.span+" deg.";
    if(!dtInit){$("dt").value=localISO(new Date());dtInit=true;}
    if(Date.now()<busyUntil)return;
    if(d.mode!==mode)setMode(d.mode,false);

    $("textInput").maxLength=d.maxlen;
    $("maxLenLbl").innerText=d.maxlen;
    if(document.activeElement!==$("textInput"))$("textInput").value=d.text;
    if(document.activeElement!==$("sentInput") && d.sent!==undefined)$("sentInput").value=d.sent;

    $("brightSlider").value=d.bright;$("brightV").innerText=d.bright;
    $("widthSlider").value=d.w;$("widthV").innerText=d.w+"%";
    $("offSlider").value=d.off;$("offV").innerText=offLabel(d.off);
    activeRows=8-d.off;
    $("angleSlider").value=d.angle;$("angleV").innerHTML=d.angle+"&deg;";
    $("hflip").checked=d.hflip;$("vflip").checked=d.vflip;

    if(mode<8) $("autoEnCb").checked = d["ae"+mode];

    $("gapSlider").value=d.gap;$("gapV").innerText=d.gap+(d.gap==1?" column":" columns");
    $("speedSlider").value=d.speed;$("speedV").innerText=d.speed;
    $("holdSlider").value=d.hold;$("holdV").innerText=d.hold+" sec";
    $("scrollTgl").checked=d.scroll;

    if(d.sentSpeed!==undefined){ $("sentSpeedSlider").value=d.sentSpeed; $("sentSpeedV").innerText=d.sentSpeed; }
    
    $("aintervSlider").value=d.ainterv;
    $("aintervV").innerText=d.ainterv+" sec";
    
    for(let i=0;i<2;i++){ if($("shp"+i)) $("shp"+i).className="mini"+(i===d.shape?" on":""); }
    if($("heartControls")) $("heartControls").style.display = (d.shape===0) ? "block" : "none";
    if($("hwSlider")) { $("hwSlider").value = d.hW; $("hwV").innerText = (d.hW/10).toFixed(1) + "x"; }
    if($("hhSlider")) { $("hhSlider").value = d.hH; $("hhV").innerText = (d.hH/10).toFixed(1) + "x"; }
    
    for(let i=0;i<2;i++){ if($("dt"+i)) $("dt"+i).className="mini"+(i===d.dtS?" on":""); }
    $("dtWordGapSlider").value=d.dtWGap;$("dtWordGapV").innerText=d.dtWGap+(d.dtWGap==1?" column":" columns");
    $("dayGapSlider").value=d.daygap;$("dayGapV").innerText=d.daygap+(d.daygap==1?" column":" columns");
    $("dateGapSlider").value=d.dategap;$("dateGapV").innerText=d.dategap+(d.dategap==1?" column":" columns");

    $("t24").checked=d.use24;$("dsec").checked=d.dsec;
    $("dgapSlider").value=d.dgap;$("dgapV").innerText=d.dgap+(d.dgap==1?" column":" columns");
    $("asec").checked=d.asec;$("aticks").checked=d.aticks;
    $("shAuto").checked=d.shAuto;
    $("dtAuto").checked=d.dtAuto;
    $("tz").value=d.tz;

    if(mode===7) $("angleSlider").max=180;
    else $("angleSlider").max=360;

    if(!dragging)syncWheel();
    if(mode===1&&!drawing)redraw();
  }).catch(()=>{});
}

updateDynamicLabels();
drawWheel();buildPalette();setView("flat");setMode(0,false);
window.addEventListener("resize",()=>{if(mode===1)sizeCanvas();});
setInterval(updateStatus,2500);
setInterval(flush,100);
updateStatus();
</script>
</body>
</html>
)rawliteral";

// =====================================================
// OTA PAGE
// =====================================================
const char ota_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OTA Update</title>
<style>
  body { background: #0b0e14; color: #e6eaf2; font-family: 'Segoe UI', sans-serif; text-align: center; padding-top: 50px;}
  .box { background: #141824; border: 1px solid #252c3f; padding: 40px; border-radius: 16px; display: inline-block;}
  input[type="file"] { margin-bottom: 20px; color: #22d3ee; }
  input[type="submit"] { background: #22d3ee; color: #06101a; font-weight: bold; border: none; padding: 12px 25px; border-radius: 10px; cursor: pointer; font-size: 16px;}
</style>
</head>
<body>
  <div class="box">
    <h2>Firmware OTA Update</h2>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="update" accept=".bin" required><br>
      <input type="submit" value="Flash to ESP8266">
    </form>
  </div>
</body></html>
)rawliteral";

// =====================================================
// WEB HANDLERS
// =====================================================
String hex2(uint8_t v) {
  const char* h = "0123456789abcdef";
  String s = "";
  s += h[v >> 4];
  s += h[v & 15];
  return s;
}

String hex6(CRGB c) { return hex2(c.r) + hex2(c.g) + hex2(c.b); }

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int firstAutoMode() {
  for (int i = 0; i < NUM_MODES; i++) {
    if (i != MODE_AUTO && autoEn[i]) return i;
  }
  return MODE_TEXT;
}

#define TF(b) ((b) ? "true" : "false")

void handleStatus() {
  const ModeCfg& C = cfg[mode];

  struct tm t;
  float frac;
  bool timeok = getLocalTm(t, frac);
  char clk[16] = "--:--:--";
  char dat[20] = "";
  if (timeok) {
    strftime(clk, sizeof(clk), "%H:%M:%S", &t);
    strftime(dat, sizeof(dat), "%d %b %Y", &t);
  }

  bool apMode = (WiFi.getMode() == WIFI_AP);
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  static char j[2048];
  int n = 0;
#define ADD(...) do { if (n < (int)sizeof(j) - 1) n += snprintf(j + n, sizeof(j) - n, __VA_ARGS__); } while (0)

  int jt = (int)(jitterPct * 10.0f + 0.5f);
  int maxLen = getDynamicMaxTextLen();

  ADD("{\"rpm\":%d,\"battery\":%d,\"jitter\":%d.%d,\"mode\":%d,\"maxlen\":%d,\"bright\":%d,\"off\":%d,\"angle\":%d,\"hflip\":%s,\"vflip\":%s",
      liveRPM, batteryPercent, jt / 10, jt % 10, mode, maxLen, (int)brightness, C.off, C.angle, TF(C.hflip), TF(C.vflip));
  ADD(",\"gap\":%d,\"scroll\":%s,\"speed\":%d,\"hold\":%d,\"sentSpeed\":%d,\"ainterv\":%d,\"shape\":%d,\"hW\":%d,\"hH\":%d,\"dtS\":%d",
      letterGap, TF(scrollEnabled), scrollSpeed, holdTimeSec, sentScrollSpeed, autoIntervalSec, currentShape, heartWidth, heartHeight, currentShowDt);
  ADD(",\"use24\":%s,\"dsec\":%s,\"dgap\":%d,\"dategap\":%d,\"daygap\":%d,\"dtWGap\":%d,\"asec\":%s,\"aticks\":%s,\"shAuto\":%s,\"dtAuto\":%s",
      TF(use24h), TF(showSeconds), digGap, dateGap, dayGap, dtWordGap, TF(showSecHand), TF(showTicks), TF(shapeAuto), TF(dateAuto));
  for (int i = 0; i < NUM_MODES; i++) ADD(",\"ae%d\":%s", i, TF(autoEn[i]));
  ADD(",\"tz\":%d,\"timeok\":%s,\"clk\":\"%s\",\"date\":\"%s\",\"ip\":\"%s\",\"ap\":%s",
      tzOffsetMin, TF(timeok), clk, dat, ip.c_str(), TF(apMode));
  ADD(",\"w\":%d,\"cdeg\":%d,\"span\":%d,\"q\":%d,\"maxrpm\":%d,\"goodrpm\":%d,\"fast\":%s",
      widthPct, (int)(colDegInUse * 10.0f + 0.5f), (int)(colDegInUse * textColumns + 0.5f), quality, (int)maxRpmNow, GOOD_RPM_MIN, TF(quality == 2));
  ADD(",\"ct\":\"%06x\",\"dh\":\"%06x\",\"dm\":\"%06x\",\"ds\":\"%06x\",\"dd\":\"%06x\"",
      (unsigned)rgbOf(textColor), (unsigned)rgbOf(digHourColor), (unsigned)rgbOf(digMinColor), (unsigned)rgbOf(digSecColor), (unsigned)rgbOf(digColonColor));
  ADD(",\"ch\":\"%06x\",\"cm\":\"%06x\",\"cs\":\"%06x\",\"ck\":\"%06x\"",
      (unsigned)rgbOf(hourColor), (unsigned)rgbOf(minColor), (unsigned)rgbOf(secColor), (unsigned)rgbOf(tickColor));
  ADD(",\"cDY\":\"%06x\",\"cDN\":\"%06x\",\"cDM\":\"%06x\",\"cHT\":\"%06x\",\"cSM\":\"%06x\",\"cST\":\"%06x\"",
      (unsigned)rgbOf(dayColor), (unsigned)rgbOf(dateNumColor), (unsigned)rgbOf(dateMonColor), (unsigned)rgbOf(heartColor), (unsigned)rgbOf(smileyColor), (unsigned)rgbOf(sentColor));
  ADD(",\"text\":\"%s\",\"sent\":\"%s\"}", displayText.c_str(), sentenceText.c_str());
#undef ADD

  server.send(200, "application/json", j);
}

void handleSetMode() {
  if (server.hasArg("val")) {
    mode = constrain(server.arg("val").toInt(), 0, NUM_MODES - 1);
    currentAutoMode = firstAutoMode();
    lastAutoSwitchMs = millis();
    inTransition = false;
    subTransition = false;
    resetAnimation();
    needLayout = true;
    sentNeedLayout = true;
    axDirty = true;
    shDirty = true;
    blankDisplay();
    forceRedraw();
    markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetText() {
  if (server.hasArg("val")) {
    String raw = server.arg("val");
    String clean = "";
    int maxAllowed = getDynamicMaxTextLen();
    for (unsigned int i = 0; i < raw.length() && (int)clean.length() < maxAllowed; i++) {
      char c = raw[i];
      bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '-' || c == ' ';
      clean += ok ? c : ' ';
    }
    clean.trim();
    displayText = clean;
    needLayout = true;
    resetAnimation();
    blankDisplay();
    forceRedraw();
    markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetSentence() {
  if (server.hasArg("val")) {
    String raw = server.arg("val");
    String clean = "";
    for (unsigned int i = 0; i < raw.length() && (int)clean.length() < MAX_SENTENCE_LEN; i++) {
      char c = raw[i];
      bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '-' || c == ' ';
      clean += ok ? c : ' ';
    }
    clean.trim();
    sentenceText = clean;
    sentNeedLayout = true;
    sentScrollOffset = 0.0f;
    blankDisplay();
    forceRedraw();
    markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetSentSpeed() {
  if (server.hasArg("val")) {
    sentScrollSpeed = constrain(server.arg("val").toInt(), 1, 10);
    markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetAngle() {
  if (server.hasArg("val")) {
    int maxAngle = (mode == MODE_SENTENCE) ? 180 : 360;
    cfg[mode].angle = constrain(server.arg("val").toInt(), 0, maxAngle);
    markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetOff() {
  if (server.hasArg("val")) {
    cfg[mode].off = constrain(server.arg("val").toInt(), 0, MAX_INNER_OFF);
    axDirty = true; shDirty = true; forceRedraw(); markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleHorizontalFlip() {
  if (server.hasArg("val")) { cfg[mode].hflip = (server.arg("val") == "true"); forceRedraw(); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleVerticalFlip() {
  if (server.hasArg("val")) { cfg[mode].vflip = (server.arg("val") == "true"); forceRedraw(); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetBrightness() {
  if (server.hasArg("val")) {
    brightness = constrain(server.arg("val").toInt(), 10, 255);
    FastLED.setBrightness(brightness);
    forceRedraw(); markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetColor() {
  if (server.hasArg("val")) {
    String v = server.arg("val");
    if (v.length() == 6) {
      char* endp;
      long rgb = strtol(v.c_str(), &endp, 16);
      if (*endp == '\0') {
        CRGB c((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        String t = server.hasArg("t") ? server.arg("t") : "text";
        if      (t == "dHour")  digHourColor  = c;
        else if (t == "dMin")   digMinColor   = c;
        else if (t == "dSec")   digSecColor   = c;
        else if (t == "dDot")   digColonColor = c;
        else if (t == "hour")   hourColor     = c;
        else if (t == "min")    minColor      = c;
        else if (t == "sec")    secColor      = c;
        else if (t == "tick")   tickColor     = c;
        else if (t == "day")    dayColor      = c;
        else if (t == "dtNum")  dateNumColor  = c;
        else if (t == "dtMon")  dateMonColor  = c;
        else if (t == "heart")  { heartColor = c; shDirty = true; forceRedraw(); }
        else if (t == "smiley") { smileyColor = c; shDirty = true; forceRedraw(); }
        else if (t == "sent")   { sentColor = c; forceRedraw(); }
        else                    textColor = c;
        axDirty = true; needLayout = true; sentNeedLayout = true; forceRedraw(); markChanged();
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSetGap() {
  if (server.hasArg("val")) {
    letterGap = constrain(server.arg("val").toInt(), MIN_GAP, MAX_GAP);
    needLayout = true; sentNeedLayout = true; forceRedraw(); markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetScroll() {
  if (server.hasArg("val")) {
    scrollEnabled = (server.arg("val") == "true");
    resetAnimation();
    blankDisplay();
    forceRedraw();
    markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetSpeed() {
  if (server.hasArg("val")) { scrollSpeed = constrain(server.arg("val").toInt(), 1, 10); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetHold() {
  if (server.hasArg("val")) { holdTimeSec = constrain(server.arg("val").toInt(), 1, 6); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetAutoSec() {
  if (server.hasArg("val")) { autoIntervalSec = constrain(server.arg("val").toInt(), 2, 60); lastAutoSwitchMs = millis(); inTransition = false; markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetAutoEn() {
  if (server.hasArg("m") && server.hasArg("val")) {
    int m = server.arg("m").toInt();
    if (m >= 0 && m < NUM_MODES) {
      autoEn[m] = (server.arg("val") == "true");
      markChanged();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSetShape() {
  if (server.hasArg("val")) {
    currentShape = constrain(server.arg("val").toInt(), 0, 1);
    shDirty = true; forceRedraw(); markChanged(); subTransition = false;
    subHoldOn = true; subHoldStart = millis();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetHScale() {
  if (server.hasArg("w")) heartWidth = constrain(server.arg("w").toInt(), 5, 20);
  if (server.hasArg("h")) heartHeight = constrain(server.arg("h").toInt(), 5, 20);
  shDirty = true; forceRedraw(); markChanged();
  server.send(200, "text/plain", "OK");
}

void handleSetShAuto() {
  if (server.hasArg("val")) { shapeAuto = (server.arg("val") == "true"); subHoldOn = false; subLastSwitchMs = millis(); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetShowDt() {
  if (server.hasArg("val")) {
    currentShowDt = constrain(server.arg("val").toInt(), 0, 1);
    needLayout = true; forceRedraw(); markChanged(); subTransition = false;
    subHoldOn = true; subHoldStart = millis();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetDtAuto() {
  if (server.hasArg("val")) { dateAuto = (server.arg("val") == "true"); subHoldOn = false; subLastSwitchMs = millis(); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetDateGap() {
  if (server.hasArg("val")) { dateGap = constrain(server.arg("val").toInt(), 1, 4); needLayout = true; forceRedraw(); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetDayGap() {
  if (server.hasArg("val")) { dayGap = constrain(server.arg("val").toInt(), 1, 4); needLayout = true; forceRedraw(); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetDtWordGap() {
  if (server.hasArg("val")) { dtWordGap = constrain(server.arg("val").toInt(), 1, 10); needLayout = true; forceRedraw(); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetWidth() {
  if (server.hasArg("val")) {
    widthPct = constrain(server.arg("val").toInt(), WIDTH_MIN_PCT, WIDTH_MAX_PCT);
    needLayout = true; sentNeedLayout = true; forceRedraw(); markChanged();
  }
  server.send(200, "text/plain", "OK");
}

void handleSet24h() {
  if (server.hasArg("val")) { use24h = (server.arg("val") == "true"); clockStr = ""; markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetDSec() {
  if (server.hasArg("val")) { showSeconds = (server.arg("val") == "true"); clockStr = ""; markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetDGap() {
  if (server.hasArg("val")) { digGap = constrain(server.arg("val").toInt(), 1, 3); needLayout = true; forceRedraw(); markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetASec() {
  if (server.hasArg("val")) { showSecHand = (server.arg("val") == "true"); axDirty = true; markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetATicks() {
  if (server.hasArg("val")) { showTicks = (server.arg("val") == "true"); axDirty = true; markChanged(); }
  server.send(200, "text/plain", "OK");
}

void handleSetTime() {
  if (server.hasArg("tz")) { tzOffsetMin = constrain(server.arg("tz").toInt(), -720, 840); markChanged(); }
  if (server.hasArg("epoch")) {
    unsigned long e = strtoul(server.arg("epoch").c_str(), NULL, 10);
    if (e > 1700000000UL) {
      struct timeval tv;
      tv.tv_sec  = (time_t)e;
      tv.tv_usec = 0;
      settimeofday(&tv, NULL);
    }
  }
  clockStr = ""; dateStr = ""; axDirty = true; forceRedraw();
  server.send(200, "text/plain", "OK");
}

void handleCvPx() {
  String body = server.arg("plain");
  int n = body.length() / 9;

  for (int t = 0; t < n; t++) {
    const char* p = body.c_str() + t * 9;
    int c1 = hexval(p[0]), c2 = hexval(p[1]), ring = hexval(p[2]);
    int r1 = hexval(p[3]), r2 = hexval(p[4]);
    int g1 = hexval(p[5]), g2 = hexval(p[6]);
    int b1 = hexval(p[7]), b2 = hexval(p[8]);

    if (c1 < 0 || c2 < 0 || ring < 0 || r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) continue;

    int col = c1 * 16 + c2;
    if (col >= CV_COLS || ring >= NUM_LEDS) continue;

    cv[col][ring] = CRGB(r1 * 16 + r2, g1 * 16 + g2, b1 * 16 + b2);
  }

  markCanvasChanged();
  server.send(200, "text/plain", "OK");
}

void handleCvGet() {
  String s;
  s.reserve(CV_COLS * NUM_LEDS * 6 + 4);
  char b[8];
  for (int c = 0; c < CV_COLS; c++) {
    for (int i = 0; i < NUM_LEDS; i++) {
      snprintf(b, sizeof(b), "%02x%02x%02x", (unsigned)cv[c][i].r, (unsigned)cv[c][i].g, (unsigned)cv[c][i].b);
      s += b;
    }
  }
  server.send(200, "text/plain", s);
}

void handleCvClear() {
  fill_solid(&cv[0][0], CV_COLS * NUM_LEDS, CRGB::Black);
  markCanvasChanged();
  forceRedraw();
  server.send(200, "text/plain", "OK");
}

// =====================================================
// DRAWING (one frame)
// =====================================================
int textColFromRel(float rel, float span, float shift, bool hflip) {
  if (rel >= span) return -1;
  int col = (int)floorf((rel - shift) / colDegInUse);
  if (col < 0 || col >= textColumns) return -1;
  if (hflip) col = textColumns - 1 - col;
  return col;
}

void drawTextFrame(float angle1, float angle2) {
  if (activeString().length() == 0) { blankDisplay(); serviceBackground(); return; }

  if (needLayout) computeLayout();
  if (textColumns <= 0) { blankDisplay(); serviceBackground(); return; }

  // When scrolling in pause state 3 (outside), force display blank
  if (getActiveMode() == MODE_TEXT && scrollEnabled && animState == 3) {
    blankDisplay();
    serviceBackground();
    return;
  }

  const ModeCfg& C = cfg[getActiveMode()];

  float span       = colDegInUse * textColumns;
  float shift      = (getActiveMode() == MODE_TEXT && scrollEnabled) ? animShift(span) : 0.0f;
  float startAngle = (float)C.angle - span / 2.0f;

  float rel1 = wrap360(angle1 - startAngle);
  float rel2 = wrap360(angle2 - startAngle);

  int col1 = textColFromRel(rel1, span, shift, C.hflip);
  int col2 = textColFromRel(rel2, span, shift, C.hflip);

  if (col1 != lastCol1 || col2 != lastCol2) {
    renderTextStrip(leds1, col1, STRIP1_LED0_AT_CENTER);
    renderTextStrip(leds2, col2, STRIP2_LED0_AT_CENTER);
    showLEDs();
    lastCol1 = col1;
    lastCol2 = col2;
    return;
  }

  bool freeNow = false;
  if (rel1 >= span && rel2 >= span) {
    float toStart = fminf(360.0f - rel1, 360.0f - rel2);
    freeNow = (toStart / 360.0f * estPeriod > WEB_FREE_US);
  }

  if (freeNow || millis() - lastServeMs > 150) serviceBackground();
}

// Sentence scrolling: confined to limited aperture arc to prevent 360 distortion
void drawSentenceFrame(float angle1, float angle2) {
  if (sentenceText.length() == 0) { blankDisplay(); serviceBackground(); return; }

  if (sentNeedLayout) computeSentenceLayout();
  if (sentColumns <= 0) { blankDisplay(); serviceBackground(); return; }

  const ModeCfg& C = cfg[MODE_SENTENCE];

  // Advance scroll offset continuously
  uint32_t nowUs = micros();
  if (sentLastUpdateUs == 0) sentLastUpdateUs = nowUs;
  uint32_t dtUs = nowUs - sentLastUpdateUs;
  sentLastUpdateUs = nowUs;

  float speedDegPerSec = 15.0f + (float)(sentScrollSpeed - 1) * 8.0f;
  float sentSpanDeg    = (float)sentColumns * sentColDeg;
  float totalTravel    = SENT_APERTURE_DEG + sentSpanDeg;

  sentScrollOffset += speedDegPerSec * ((float)dtUs / 1000000.0f);
  if (sentScrollOffset >= totalTravel) sentScrollOffset -= totalTravel;

  float startAngle = (float)C.angle - SENT_APERTURE_DEG / 2.0f;

  float rel1 = wrap360(angle1 - startAngle);
  float rel2 = wrap360(angle2 - startAngle);

  int col1 = -1;
  int col2 = -1;

  if (rel1 < SENT_APERTURE_DEG) {
    float posDeg = sentScrollOffset - rel1;
    int c = (int)floorf(posDeg / sentColDeg);
    if (c >= 0 && c < sentColumns) {
      col1 = C.hflip ? (sentColumns - 1 - c) : c;
    }
  }

  if (rel2 < SENT_APERTURE_DEG) {
    float posDeg = sentScrollOffset - rel2;
    int c = (int)floorf(posDeg / sentColDeg);
    if (c >= 0 && c < sentColumns) {
      col2 = C.hflip ? (sentColumns - 1 - c) : c;
    }
  }

  if (col1 != lastCol1 || col2 != lastCol2) {
    renderSentenceStrip(leds1, col1, STRIP1_LED0_AT_CENTER);
    renderSentenceStrip(leds2, col2, STRIP2_LED0_AT_CENTER);
    showLEDs();
    lastCol1 = col1;
    lastCol2 = col2;
    return;
  }

  bool freeNow = (rel1 >= SENT_APERTURE_DEG && rel2 >= SENT_APERTURE_DEG);
  if (freeNow || millis() - lastServeMs > 150) serviceBackground();
}

int pictureColAt(float angle, float start, bool hflip) {
  float rel = wrap360(angle - start);
  int col = (int)(rel / CV_DEG);
  if (col >= CV_COLS) col = CV_COLS - 1;
  if (col < 0) col = 0;
  if (hflip) col = CV_COLS - 1 - col;
  return col;
}

void drawPictureFrame(float angle1, float angle2, CRGB* buf) {
  const ModeCfg& C = cfg[getActiveMode()];
  float start = (float)C.angle - 180.0f;

  int col1 = pictureColAt(angle1, start, C.hflip);
  int col2 = pictureColAt(angle2, start, C.hflip);

  if (col1 != lastCol1 || col2 != lastCol2) {
    renderPictureStrip(leds1, col1, buf, STRIP1_LED0_AT_CENTER);
    renderPictureStrip(leds2, col2, buf, STRIP2_LED0_AT_CENTER);
    showLEDs();
    lastCol1 = col1;
    lastCol2 = col2;
  }

  serviceBackground();
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  Serial.println();

  loadSettings();
  currentAutoMode = firstAutoMode();
  resetAnimation();

#if USE_FAST_DRIVER
  pinMode(STRIP1_PIN, OUTPUT);
  pinMode(STRIP2_PIN, OUTPUT);
  digitalWrite(STRIP1_PIN, LOW);
  digitalWrite(STRIP2_PIN, LOW);
#else
  FastLED.addLeds<WS2812, STRIP1_PIN, GRB>(leds1, NUM_LEDS);
  FastLED.addLeds<WS2812, STRIP2_PIN, GRB>(leds2, NUM_LEDS);
  FastLED.setBrightness(brightness);
  FastLED.setDither(0);
#endif
  blankDisplay();

  pinMode(HALL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_PIN), hallDetected, FALLING);

  // Initialize battery measurement with clean burst average before LEDs start
  for (int i = 0; i < 20; i++) {
    float r = readBatteryRawSmoothed();
    if (batteryFiltered < 0.0f) batteryFiltered = r;
    else batteryFiltered = batteryFiltered * 0.8f + r * 0.2f;
    delay(2);
  }
  updateBattery();

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(ssid, password);
  uint32_t startMs = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.softAP(apName, apPassword);
    Serial.print("Hotspot IP: ");
    Serial.println(WiFi.softAPIP());
  }

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", webpage); });
  server.on("/status",         HTTP_GET,  handleStatus);
  server.on("/set_mode",       HTTP_GET,  handleSetMode);
  server.on("/set_text",       HTTP_GET,  handleSetText);
  server.on("/set_sentence",   HTTP_GET,  handleSetSentence);
  server.on("/set_sentSpeed",  HTTP_GET,  handleSetSentSpeed);
  server.on("/set_angle",      HTTP_GET,  handleSetAngle);
  server.on("/set_off",        HTTP_GET,  handleSetOff);
  server.on("/set_hflip",      HTTP_GET,  handleHorizontalFlip);
  server.on("/set_vflip",      HTTP_GET,  handleVerticalFlip);
  server.on("/set_brightness", HTTP_GET,  handleSetBrightness);
  server.on("/set_color",      HTTP_GET,  handleSetColor);
  server.on("/set_gap",        HTTP_GET,  handleSetGap);
  server.on("/set_scroll",     HTTP_GET,  handleSetScroll);
  server.on("/set_speed",      HTTP_GET,  handleSetSpeed);
  server.on("/set_hold",       HTTP_GET,  handleSetHold);
  server.on("/set_auto_sec",   HTTP_GET,  handleSetAutoSec);
  server.on("/set_autoEn",     HTTP_GET,  handleSetAutoEn);
  server.on("/set_shape",      HTTP_GET,  handleSetShape);
  server.on("/set_hscale",     HTTP_GET,  handleSetHScale);
  server.on("/set_shAuto",     HTTP_GET,  handleSetShAuto);
  server.on("/set_showDt",     HTTP_GET,  handleSetShowDt);
  server.on("/set_dtAuto",     HTTP_GET,  handleSetDtAuto);
  server.on("/set_dategap",    HTTP_GET,  handleSetDateGap);
  server.on("/set_daygap",     HTTP_GET,  handleSetDayGap);
  server.on("/set_dtWordGap",  HTTP_GET,  handleSetDtWordGap);
  server.on("/set_width",      HTTP_GET,  handleSetWidth);
  server.on("/set_24h",        HTTP_GET,  handleSet24h);
  server.on("/set_dsec",       HTTP_GET,  handleSetDSec);
  server.on("/set_dgap",       HTTP_GET,  handleSetDGap);
  server.on("/set_asec",       HTTP_GET,  handleSetASec);
  server.on("/set_aticks",     HTTP_GET,  handleSetATicks);
  server.on("/set_time",       HTTP_GET,  handleSetTime);
  server.on("/cv_px",          HTTP_POST, handleCvPx);
  server.on("/cv_get",         HTTP_GET,  handleCvGet);
  server.on("/cv_clear",       HTTP_GET,  handleCvClear);

  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send_P(200, "text/html", ota_page);
  });
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "SUCCESS");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      WiFiUDP::stopAll();
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      Update.begin(maxSketchSpace);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      Update.end(true);
    }
  });

  server.begin();
}

// Quality evaluation called once per revolution
void updateQuality() {
  int am = getActiveMode();
  bool textLike = (am == MODE_TEXT || am == MODE_DIGITAL || am == MODE_DATE || am == MODE_SENTENCE);
  float colDeg = textLike ? colDegInUse : CV_DEG;
  float needUs = (showTimeUs + LOOP_OVERHEAD_US) * SAFE_MARGIN;
  maxRpmNow = 60000000.0f * (colDeg / 360.0f) / needUs;
  if (liveRPM < MIN_RPM)           quality = 0;
  else if (liveRPM > maxRpmNow)    quality = 2;
  else if (liveRPM < GOOD_RPM_MIN) quality = 1;
  else                             quality = 3;
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {
  uint32_t now, lastHall, period;

  noInterrupts();
  lastHall = lastHallTime;
  period   = revolutionTime;
  interrupts();
  now = micros();

  if (lastHall != prevHallSeen) {
    prevHallSeen = lastHall;
    updateFilter(period);
    if (filterValid) updateQuality();
  }

  uint32_t elapsed = now - lastHall;
  if (filterValid && (float)elapsed > estPeriod * 1.5f) filterValid = false;

  if (!filterValid || liveRPM < MIN_RPM) {
    if (!filterValid) liveRPM = 0;
    quality = 0;
    blankDisplay();
    serviceBackground();
    return;
  }

  float phase = (float)elapsed / estPeriod;
  if (phase > 0.999f) phase = 0.999f;

  float lead   = 360.0f * (showTimeUs * LED_LEAD) / estPeriod;
  float angle1 = wrap360(phase * 360.0f + lead);
  float angle2 = wrap360(angle1 + 180.0f);

  // Main Auto Mode cycling
  if (mode == MODE_AUTO) {
    if (!inTransition && millis() - lastAutoSwitchMs > (uint32_t)autoIntervalSec * 1000UL) {
      int next = (currentAutoMode + 1) % NUM_MODES;
      int tries = 0;
      while ((next == MODE_AUTO || !autoEn[next]) && tries < NUM_MODES) {
        next = (next + 1) % NUM_MODES;
        tries++;
      }
      if (tries < NUM_MODES && next != currentAutoMode) {
        inTransition = true;
        transitionStart = millis();
        nextAutoMode = next;
      } else {
        lastAutoSwitchMs = millis();
      }
    }
  } else {
    currentAutoMode = mode;
    lastAutoSwitchMs = millis();
  }

  int am = getActiveMode();

  // Sub-Mode Transitions (Shapes & Date Auto-Cycle Fade)
  if (subHoldOn && millis() - subHoldStart >= 10000UL) subHoldOn = false;
  if (!inTransition && !subTransition && !subHoldOn) {
    bool wantShape = (am == MODE_SHAPES && shapeAuto);
    bool wantDate  = (am == MODE_DATE && dateAuto);
    if (!wantShape && !wantDate) {
      subLastSwitchMs = millis();
    } else if (millis() - subLastSwitchMs >= 4000UL) {
      subLastSwitchMs = millis();
      subTransition = true; subTransStart = millis();
      if (wantShape) { subTransType = 1; nextSubVal = 1 - currentShape; }
      else           { subTransType = 2; nextSubVal = 1 - currentShowDt; }
    }
  }

  if (am == MODE_CANVAS) {
    drawPictureFrame(angle1, angle2, &cv[0][0]);
  } else if (am == MODE_ANALOG) {
    updateAnalog();
    drawPictureFrame(angle1, angle2, &ax[0][0]);
  } else if (am == MODE_SHAPES) {
    if (shDirty) buildShapes();
    drawPictureFrame(angle1, angle2, &sh[0][0]);
  } else if (am == MODE_SENTENCE) {
    drawSentenceFrame(angle1, angle2);
  } else {
    if (am == MODE_DIGITAL) updateClockString();
    if (am == MODE_DATE)    updateDateString();
    drawTextFrame(angle1, angle2);
  }
}
