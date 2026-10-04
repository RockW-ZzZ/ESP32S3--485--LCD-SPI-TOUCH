/* =====================================================================
 *  充电器上位机 v3.1 (修正版)  ESP32-S3 N16R8
 *  ST7789 320x240 + EC11 + 外键 + MAX485 + 蜂鸣器(GPIO8)
 *  功能: 485 Modbus 轮询 / RUN状态机+心跳开机 / CC·CP / 急停 / 电量统计
 *        NVS持久化 / AP+STA / Web镜像UI+二次确认 / 配色 / 波形 / OTA / 看门狗
 * ===================================================================== */
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

// ================= 引脚 =================
#define PIN_LCD_SCLK 12
#define PIN_LCD_MOSI 11
#define PIN_LCD_DC 4
#define PIN_LCD_CS 10
#define PIN_LCD_RST 5
#define PIN_LCD_BL 6
#define PIN_EC11_A 1
#define PIN_EC11_B 2
#define PIN_EC11_SW 3
#define PIN_BTN 7
#define PIN_RS485_TX 17
#define PIN_RS485_RX 18
#define PIN_RS485_DE -1
#define PIN_BUZZER 8
// 蜂鸣器模块触发极性: 三脚模块(信号经三极管驱动无源蜂鸣器)多为"低电平触发",
// 即信号为低时三极管导通。此时代码"空闲"应输出高电平, 播放时输出PWM方波。
// 若你的模块是高电平触发或直接用无源蜂鸣器接GPIO8, 把这里改成 0。
#define BUZZ_ACTIVE_LOW 1
#if BUZZ_ACTIVE_LOW
#define BUZZ_IDLE_DUTY 255u   // 空闲输出持续高电平(三极管截止)
#else
#define BUZZ_IDLE_DUTY 0u
#endif

#define RS485_BAUD 9600
#define LONG_PRESS_MS 300
#define AP_SSID "ChargerCtrl"
#define AP_PASS "12345678"
#define ADMIN_PIN "admin8888"
#define DEF_WEB_PORT 8899
#define DEF_PIN "1234"

// ================= 充电器 Modbus 配置(依据换电柜充电器协议V1.2) =================
#define CHG_ADDR 0x00         // 从机地址(单机用00;多机用查询到的地址)
#define FC_WRITE_MULTI 0x0F   // 写电压电流功能码(无字节数域)
#define REG_READ 0x0001       // 读: 电压/电流/状态 共3个
#define REG_POWER 0x0000      // 开关机寄存器
#define POWER_ON_VAL 0x00FF   // 开机(厂家抓包/手册示例值: 00 06 00 00 00 FF)
#define POWER_OFF_VAL 0x0000  // 关机(手册示例值: 00 06 00 00 00 00)
#define REG_SETV 0x0001       // 写多: 起始=电压, 数量=2
#define V_SCALE 10.0f         // 电压放大10倍
#define A_SCALE 100.0f        // 电流放大100倍(小端)
#define PWR_PROT_RATIO 1.10f  // 功率保护倍数(110%)
#define TRICKLE_GUARD_MS 10000UL

// ============ 设定电流爬坡(防充电器会话复位) ============
// 正常充电时电流设定只增不减: RUN后从 RAMP_START_A 起, 每 rampMs 加 rampStepA,
// 到达目标(UI设定)后恒定保持; 0x0F 每 rampMs 发一次, 永不下调。
// rampMs / rampStepA 可在网页"设置"里改(默认 1000ms / 0.05A)。
#define RAMP_START_A 1.0f      // 开机电流设定起始值(A)
#define RAMP_MS_DEF 1000       // 爬坡/0x0F重发节拍默认(ms)
#define RAMP_STEP_A_DEF 0.05f  // 每节拍电流爬升步进默认(A)

#define HB_MIN 1
#define HB_MAX 600

// ================= LCD =================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
public:
  LGFX(void) {
    auto bc = _bus_instance.config();
    bc.spi_host = SPI2_HOST;
    bc.freq_write = 80000000;   // 27MHz: 换屏/干扰环境下更稳(原80→40→27); 整帧约45ms
    bc.freq_read = 16000000;
    bc.pin_sclk = PIN_LCD_SCLK;
    bc.pin_mosi = PIN_LCD_MOSI;
    bc.pin_dc = PIN_LCD_DC;
    bc.dma_channel = SPI_DMA_CH_AUTO;
    _bus_instance.config(bc);
    _panel_instance.setBus(&_bus_instance);
    auto pc = _panel_instance.config();
    pc.pin_cs = PIN_LCD_CS;
    pc.pin_rst = PIN_LCD_RST;
    pc.memory_width = 240;
    pc.memory_height = 320;
    pc.panel_width = 240;
    pc.panel_height = 320;
    _panel_instance.config(pc);
    auto lc = _light_instance.config();
    lc.pin_bl = PIN_LCD_BL;
    lc.pwm_channel = 0;
    _light_instance.config(lc);
    _panel_instance.setLight(&_light_instance);
    setPanel(&_panel_instance);
  }
};
LGFX tft;
LGFX_Sprite sp(&tft);

// ---- LCD 初始化 / 受继电器干扰后重新初始化 ----
volatile bool lcdReinitReq = false;
uint32_t lcdReinitAt = 0;
void lcdInit() {
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.setBrightness(255);
}
void lcdReinit() {
  pinMode(PIN_LCD_RST, OUTPUT);
  digitalWrite(PIN_LCD_RST, LOW);
  delay(20);
  digitalWrite(PIN_LCD_RST, HIGH);
  delay(120);
  lcdInit();
  sp.fillSprite(TFT_BLACK);
  sp.pushSprite(0, 0);
}

// ================= 编码器 =================
volatile int8_t encStepBuf = 0;
volatile uint8_t encState = 0, encSeqCnt = 0;
volatile int8_t encSeqDir = 0;
static const int8_t ENC_TABLE[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
void IRAM_ATTR encoderISR() {
  uint8_t a = (GPIO.in >> PIN_EC11_A) & 1, b = (GPIO.in >> PIN_EC11_B) & 1;
  uint8_t ns = (a << 1) | b;
  int8_t d = ENC_TABLE[(encState << 2) | ns];
  encState = ns;
  if (d == 0) return;
  if (d == encSeqDir) {
    if (++encSeqCnt >= 4) {
      encStepBuf += encSeqDir;
      encSeqCnt = 0;
    }
  } else {
    encSeqDir = d;
    encSeqCnt = 1;
  }
}
int8_t getEncoderStep() {
  noInterrupts();
  int8_t s = encStepBuf;
  encStepBuf = 0;
  interrupts();
  return s;
}

// ================= 按键 =================
#define BTN_EC11_SW 0
#define BTN_EXT 1
const uint8_t btnPins[2] = { PIN_EC11_SW, PIN_BTN };
bool btnRaw[2] = { 0 }, btnStable[2] = { 0 }, btnPrevStable[2] = { 0 }, btnLongFired[2] = { 0 };
uint32_t btnPressTime[2] = { 0 };
bool pageTransitionLock = false;
void readButtons() {
  for (int i = 0; i < 2; i++) {
    bool raw = (digitalRead(btnPins[i]) == LOW);
    if (raw && !btnStable[i]) {
      if (!btnRaw[i]) {
        btnPressTime[i] = millis();
        btnRaw[i] = true;
      } else if (millis() - btnPressTime[i] > 30) btnStable[i] = true;
    } else if (!raw) {
      btnRaw[i] = false;
      btnStable[i] = false;
      btnLongFired[i] = false;
    }
  }
}
bool getShortPress(uint8_t i) {
  bool was = btnPrevStable[i];
  btnPrevStable[i] = btnStable[i];
  if (pageTransitionLock) return false;
  return (was && !btnStable[i] && !btnLongFired[i]);
}
bool getLongPress(uint8_t i) {
  if (btnStable[i] && !btnLongFired[i] && millis() - btnPressTime[i] > LONG_PRESS_MS) {
    btnLongFired[i] = true;
    return true;
  }
  return false;
}
bool isButtonFullyReleased(uint8_t i) {
  return !btnRaw[i] && !btnStable[i] && !btnPrevStable[i];
}
void clearButtonAndLock() {
  for (int i = 0; i < 2; i++) btnRaw[i] = btnStable[i] = btnPrevStable[i] = btnLongFired[i] = false;
  noInterrupts();
  encStepBuf = 0;
  encSeqCnt = 0;
  interrupts();
  pageTransitionLock = true;
}

// ================= 蜂鸣器(音量=占空比) =================
volatile bool bzReqFocus = false, bzReq485 = false, bzReqWifi = false, bzReqRun = false, bzReqAlarm = false;
bool bzMute = false;
uint8_t bzVol = 60;
struct BzPat {
  uint16_t onMs, offMs;
  uint8_t count;
};
BzPat bzCur = { 0, 0, 0 };
int bzRemain = 0;
bool bzOn = false;
uint32_t bzT0 = 0;
bool bzBusy = false;
void buzzRaw(bool on);
void buzzInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2700, 8);
#else
  ledcSetup(4, 2700, 8);
  ledcAttachPin(PIN_BUZZER, 4);
#endif
  buzzRaw(false);   // 上电即置为空闲电平(低触发模块=高电平)
}
void buzzRaw(bool on) {
  if (bzMute) on = false;
  uint32_t duty = on ? ((255u * (uint32_t)bzVol) / 100u) : BUZZ_IDLE_DUTY;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BUZZER, duty);
#else
  ledcWrite(4, duty);
#endif
}
void bzPlay(uint16_t on, uint16_t off, uint8_t cnt) {
  bzCur = { on, off, cnt };
  bzRemain = cnt;
  bzOn = true;
  bzT0 = millis();
  bzBusy = true;
  buzzRaw(true);
}
void bzUpdate() {
  if (!bzBusy) {
    if (bzReqAlarm) {
      bzReqAlarm = false;
      bzPlay(150, 100, 5);
      return;
    }
    if (bzReqRun) {
      bzReqRun = false;
      bzPlay(1000, 100, 1);
      return;
    }
    if (bzReqWifi) {
      bzReqWifi = false;
      bzPlay(1000, 300, 3);
      return;
    }
    if (bzReq485) {
      bzReq485 = false;
      bzPlay(100, 100, 3);
      return;
    }
    if (bzReqFocus) {
      bzReqFocus = false;
      bzPlay(30, 0, 1);
      return;
    }
  }
  if (!bzBusy) return;
  uint32_t el = millis() - bzT0;
  if (bzOn && el >= bzCur.onMs) {
    buzzRaw(false);
    bzOn = false;
    bzT0 = millis();
    if (--bzRemain <= 0) bzBusy = false;
  } else if (!bzOn && el >= bzCur.offMs) {
    buzzRaw(true);
    bzOn = true;
    bzT0 = millis();
  }
}

// ================= 主题色 (25项) =================
enum ThIdx : uint8_t {
  TH_BG = 0,
  TH_LV,
  TH_LA,
  TH_LW,
  TH_RV,
  TH_RA,
  TH_RTR,
  TH_RPM,
  TH_AH,
  TH_WH,
  TH_CNT,
  TH_ERR,
  TH_PR,
  TH_CC,
  TH_APOFF,
  TH_APON,
  TH_CLR,
  TH_LOCKOFF,
  TH_LOCKON,
  TH_RUNOFF,
  TH_RUNON,
  TH_OFF485,
  TH_OK485,
  TH_OFFWIFI,
  TH_OKWIFI,
  TH_N
};
const uint8_t themeDefRGB[TH_N][3] = {
  { 10, 12, 20 }, { 240, 180, 10 }, { 10, 170, 70 }, { 220, 45, 35 }, { 20, 120, 190 }, { 200, 130, 10 }, { 140, 60, 190 }, { 90, 150, 30 }, { 240, 130, 20 }, { 0, 100, 140 }, { 80, 60, 220 }, { 230, 40, 100 }, { 220, 40, 160 }, { 240, 180, 0 }, { 100, 60, 160 }, { 255, 60, 220 }, { 230, 220, 20 }, { 70, 130, 180 }, { 255, 40, 40 }, { 0, 170, 70 }, { 230, 25, 25 }, { 70, 72, 80 }, { 0, 200, 120 }, { 70, 72, 80 }, { 50, 120, 255 }
};
uint16_t themeC[TH_N];
uint16_t C_FOCUS, C_EDIT_HL, C_BACK_TXT, C_DIM;
uint16_t preColors[5];
uint16_t textColorForBg(uint16_t bg) {
  uint8_t r = ((bg >> 11) & 31) * 255 / 31, g = ((bg >> 5) & 63) * 255 / 63, b = (bg & 31) * 255 / 31;
  return ((uint16_t)(0.299f * r + 0.587f * g + 0.114f * b) > 140) ? TFT_BLACK : TFT_WHITE;
}
uint16_t blendC(uint16_t a, uint16_t b, float f) {
  uint8_t r = ((a >> 11) & 31) * (1 - f) + ((b >> 11) & 31) * f;
  uint8_t g = ((a >> 5) & 63) * (1 - f) + ((b >> 5) & 63) * f;
  uint8_t bl = (a & 31) * (1 - f) + (b & 31) * f;
  return (r << 11) | (g << 5) | bl;
}
void initColors() {
  for (int i = 0; i < TH_N; i++) themeC[i] = tft.color565(themeDefRGB[i][0], themeDefRGB[i][1], themeDefRGB[i][2]);
  C_FOCUS = tft.color565(0, 220, 255);
  C_EDIT_HL = tft.color565(255, 210, 0);
  C_BACK_TXT = tft.color565(0, 210, 230);
  C_DIM = tft.color565(140, 145, 160);
  preColors[0] = tft.color565(30, 90, 220);
  preColors[1] = tft.color565(10, 180, 90);
  preColors[2] = tft.color565(230, 170, 0);
  preColors[3] = tft.color565(240, 80, 20);
  preColors[4] = tft.color565(160, 40, 220);
}
#define BREATH_PERIOD_MS 2000
float breathVal() {
  float ph = (millis() % BREATH_PERIOD_MS) / (float)BREATH_PERIOD_MS;
  return 0.45f + 0.55f * (0.5f + 0.5f * sinf(ph * 6.28318f));
}
uint16_t breathColor(uint16_t base) {
  float f = breathVal();
  uint16_t r = (uint16_t)(((base >> 11) & 31) * f);
  uint16_t g = (uint16_t)(((base >> 5) & 63) * f);
  uint16_t b = (uint16_t)((base & 31) * f);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// ================= 数据模型 =================
struct Charger {
  float outV = 0, outA = 0, power = 0;
  uint16_t rawStatus = 0;
  float setV = 54.6, setA = 15.0, trickle = 0.5, pwrMax = 800;
  float ah = 0, wh = 0;
  uint16_t cnt = 0;
  uint8_t preset = 1, mode = 0;
  bool apMode = false, locked = false, running = false, runActive = false;
  bool rs485_ok = false, wifi_ok = false, estop = false;
  float preV[5] = { 48.2, 54.6, 60.0, 67.2, 71.4 };
  float preA[5] = { 10.0, 12.5, 15.0, 8.0, 5.0 };
  char ipStr[48] = "--";
  uint8_t modAddr = 0;
} chg;
portMUX_TYPE chgMux = portMUX_INITIALIZER_UNLOCKED;
char tripReason[8] = "";
uint16_t tripCount = 0;
// 诊断计数(掉电不保存): 充电器会话复位次数 / 485掉线次数
volatile uint16_t resetCnt = 0;  // 状态由 CHG(2) 变为其它(充电器复位/重启)
volatile uint16_t offCnt = 0;    // 485 通信由在线变离线
volatile uint32_t lastResetMs = 0, lastOffMs = 0;
uint32_t hbSec = 5;
uint16_t webPort = DEF_WEB_PORT;
uint8_t estopClicks = 10;
uint16_t rampMs = RAMP_MS_DEF;      // 爬坡节拍(ms), 网页可配
float rampStepA = RAMP_STEP_A_DEF;  // 每节拍电流步进(A), 网页可配
uint8_t slaveAddr = CHG_ADDR;
float maxV = 100.0f, maxA = 50.0f, maxP = 3000.0f;
volatile bool scanReq = false, scanDone = false, scanFound = false;
volatile uint8_t scanResult = 0;
volatile bool addrDirty = false;
String webPin = DEF_PIN;

enum FocID : uint8_t { FC_SETV = 0,
                       FC_SETA,
                       FC_TRICKLE,
                       FC_PWRMAX,
                       FC_PR,
                       FC_CC,
                       FC_AP,
                       FC_CLR,
                       FC_LOCK,
                       FC_RUN,
                       FC_TOTAL };
struct FR {
  int16_t x, y, w, h;
};
FR foc[FC_TOTAL];
uint8_t focIdx = FC_SETV;
bool editing = false, pagePreset = false;
int8_t preCur = 0;
#define PRE_BACK 10
uint8_t burstCnt = 0;
uint32_t burstStart = 0;
#define ESTOP_CLICKS 10
#define BURST_WINDOW_MS 3000
#define BURST_SUPPRESS 3

void applyModeCalc() {
  portENTER_CRITICAL(&chgMux);
  if (chg.mode == 0) chg.pwrMax = chg.setV * chg.setA;
  else if (chg.setV > 0.1f) {
    chg.setA = chg.pwrMax / chg.setV;
    if (chg.setA > 50.0f) chg.setA = 50.0f;
  }
  portEXIT_CRITICAL(&chgMux);
}
bool fieldEditable(uint8_t fid) {
  if (fid == FC_PWRMAX && chg.mode == 0) return false;
  if (fid == FC_SETA && chg.mode == 1) return false;
  return true;
}

// ================= NVS =================
Preferences prefs;
void nvsLoad() {
  prefs.begin("chg", true);
  chg.setV = prefs.getFloat("setV", 54.6);
  chg.setA = prefs.getFloat("setA", 15.0);
  chg.trickle = prefs.getFloat("trickle", 0.5);
  chg.pwrMax = prefs.getFloat("pwrMax", 800);
  chg.ah = prefs.getFloat("ah", 0);
  chg.wh = prefs.getFloat("wh", 0);
  chg.cnt = prefs.getUShort("cnt", 0);
  chg.modAddr = prefs.getUChar("modAddr", 0);
  chg.preset = prefs.getUChar("preset", 1);
  chg.mode = prefs.getUChar("mode", 0);
  hbSec = prefs.getUInt("hbSec2", 5);
  if (hbSec < HB_MIN || hbSec > HB_MAX) hbSec = 5;
  webPort = prefs.getUShort("webPort", DEF_WEB_PORT);
  if (webPort < 1024) webPort = DEF_WEB_PORT;
  bzMute = prefs.getBool("bzMute", false);
  bzVol = prefs.getUChar("bzVol", 60);
  if (bzVol > 100) bzVol = 60;
  estopClicks = prefs.getUChar("estopN", 10);
  if (estopClicks < 3) estopClicks = 3;
  if (estopClicks > 50) estopClicks = 50;
  slaveAddr = prefs.getUChar("slaveA", CHG_ADDR);
  chg.modAddr = slaveAddr;
  maxV = prefs.getFloat("maxV", 100);
  maxA = prefs.getFloat("maxA", 50);
  maxP = prefs.getFloat("maxP", 3000);
  rampMs = prefs.getUShort("rampMs", RAMP_MS_DEF);
  if (rampMs < 200) rampMs = 200;
  if (rampMs > 10000) rampMs = 10000;
  rampStepA = prefs.getFloat("rampSt", RAMP_STEP_A_DEF);
  if (rampStepA < 0.005f) rampStepA = 0.005f;
  if (rampStepA > 5.0f) rampStepA = 5.0f;
  tripCount = prefs.getUShort("tripN", 0);
  String tr = prefs.getString("tripR", "");
  strncpy(tripReason, tr.c_str(), 7);
  tripReason[7] = 0;
  String wp = prefs.getString("webPin", "");
  if (wp.length() >= 4 && wp.length() <= 16) webPin = wp;
  size_t tl = prefs.getBytesLength("theme");
  if (tl == TH_N * 2) prefs.getBytes("theme", themeC, tl);
  for (int i = 0; i < 5; i++) {
    char k[8];
    sprintf(k, "preV%d", i);
    chg.preV[i] = prefs.getFloat(k, chg.preV[i]);
    sprintf(k, "preA%d", i);
    chg.preA[i] = prefs.getFloat(k, chg.preA[i]);
  }
  prefs.end();
}
void nvsSave() {
  prefs.begin("chg", false);
  prefs.putFloat("setV", chg.setV);
  prefs.putFloat("setA", chg.setA);
  prefs.putFloat("trickle", chg.trickle);
  prefs.putFloat("pwrMax", chg.pwrMax);
  prefs.putUChar("preset", chg.preset);
  prefs.putUChar("mode", chg.mode);
  for (int i = 0; i < 5; i++) {
    char k[8];
    sprintf(k, "preV%d", i);
    prefs.putFloat(k, chg.preV[i]);
    sprintf(k, "preA%d", i);
    prefs.putFloat(k, chg.preA[i]);
  }
  prefs.end();
}
void nvsSaveStats() {
  prefs.begin("chg", false);
  prefs.putFloat("ah", chg.ah);
  prefs.putFloat("wh", chg.wh);
  prefs.putUShort("cnt", chg.cnt);
  prefs.putUShort("tripN", tripCount);
  prefs.putString("tripR", tripReason);
  prefs.end();
}
void nvsSaveTheme() {
  prefs.begin("chg", false);
  prefs.putBytes("theme", themeC, TH_N * 2);
  prefs.end();
}
void nvsSaveCfg() {
  prefs.begin("chg", false);
  prefs.putUInt("hbSec2", hbSec);
  prefs.putUShort("webPort", webPort);
  prefs.putBool("bzMute", bzMute);
  prefs.putUChar("bzVol", bzVol);
  prefs.putUChar("estopN", estopClicks);
  prefs.putUChar("slaveA", slaveAddr);
  prefs.putFloat("maxV", maxV);
  prefs.putFloat("maxA", maxA);
  prefs.putFloat("maxP", maxP);
  prefs.putUShort("rampMs", rampMs);
  prefs.putFloat("rampSt", rampStepA);
  prefs.putString("webPin", webPin);
  prefs.end();
}

// ================= Modbus RTU =================
HardwareSerial rs485(1);
uint8_t rs485ErrCnt = 0;
uint16_t modbusCRC16(const uint8_t* s, int n) {
  uint16_t c = 0xFFFF;
  for (int k = 0; k < n; k++) {
    uint8_t b = s[k];
    for (int i = 0; i < 8; i++) {
      c = ((b ^ c) & 1) ? ((c >> 1) ^ 0xA001) : (c >> 1);
      b >>= 1;
    }
  }
  return c;  // 标准CRC;发送时低字节在前
}
void rs485Write(const uint8_t* f, int n) {
#if PIN_RS485_DE >= 0
  digitalWrite(PIN_RS485_DE, HIGH);
#endif
  rs485.write(f, n);
  rs485.flush();
#if PIN_RS485_DE >= 0
  digitalWrite(PIN_RS485_DE, LOW);
#endif
}
int modbusXfer(const uint8_t* req, int reqLen, uint8_t* buf, int bufMax, uint32_t toMs = 200) {
  uint8_t frame[32];
  memcpy(frame, req, reqLen);
  uint16_t crc = modbusCRC16(frame, reqLen);
  frame[reqLen] = crc & 0xFF;
  frame[reqLen + 1] = crc >> 8;
  while (rs485.available()) rs485.read();
  rs485Write(frame, reqLen + 2);
  uint32_t t0 = millis();
  int n = 0;
  uint32_t lastByte = t0;
  while (millis() - t0 < toMs) {
    while (rs485.available() && n < bufMax) {
      buf[n++] = rs485.read();
      lastByte = millis();
    }
    if (n > 0 && millis() - lastByte > 10) break;
    vTaskDelay(1);  // 让出CPU: 否则485断开时本任务空转, 饿死IDLE任务触发看门狗重启
  }
  return n;
}
bool crcOK(const uint8_t* f, int n) {
  if (n < 4) return false;
  uint16_t crc = modbusCRC16(f, n - 2);
  return (f[n - 2] == (crc & 0xFF) && f[n - 1] == (crc >> 8));
}
bool readStatus() {
  uint8_t req[8] = { slaveAddr, 0x03, (uint8_t)(REG_READ >> 8), (uint8_t)(REG_READ & 0xFF), 0x00, 0x03, 0, 0 };
  uint8_t rsp[16];
  int n = modbusXfer(req, 6, rsp, sizeof(rsp));
  if (n < 11) return false;
  if (!crcOK(rsp, 11)) return false;
  if (rsp[1] != 0x03 || rsp[2] != 0x06) return false;
  uint16_t vRaw = (rsp[3] << 8) | rsp[4];                      // 电压: 大端
  uint16_t aRaw = (uint16_t)rsp[5] | ((uint16_t)rsp[6] << 8);  // 电流: 小端
  uint16_t st = (rsp[7] << 8) | rsp[8];
  portENTER_CRITICAL(&chgMux);
  chg.outV = vRaw / V_SCALE;
  chg.outA = aRaw / A_SCALE;
  chg.power = chg.outV * chg.outA;
  chg.rawStatus = st;
  portEXIT_CRITICAL(&chgMux);
  return true;
}
bool setPower(bool on) {
  uint16_t val = on ? POWER_ON_VAL : POWER_OFF_VAL;
  uint8_t req[8] = { slaveAddr, 0x06, (uint8_t)(REG_POWER >> 8), (uint8_t)(REG_POWER & 0xFF), (uint8_t)(val >> 8), (uint8_t)(val & 0xFF), 0, 0 };
  uint8_t rsp[16];
  int n = modbusXfer(req, 6, rsp, sizeof(rsp));
  return n >= 8 && crcOK(rsp, 8) && rsp[1] == 0x06;
}
bool setVI(float v, float a) {
  uint16_t vr = (uint16_t)(v * V_SCALE + 0.5f);
  uint16_t ar = (uint16_t)(a * A_SCALE + 0.5f);
  uint8_t req[12] = { slaveAddr, FC_WRITE_MULTI, (uint8_t)(REG_SETV >> 8), (uint8_t)(REG_SETV & 0xFF), 0x00, 0x02,
                      (uint8_t)(vr >> 8), (uint8_t)(vr & 0xFF),          // 电压 大端
                      (uint8_t)(ar >> 8), (uint8_t)(ar & 0xFF), 0, 0 };  // 电流 大端(高字节先)
  uint8_t rsp[16];
  int n = modbusXfer(req, 10, rsp, sizeof(rsp));
  return n >= 8 && crcOK(rsp, 8) && rsp[1] == FC_WRITE_MULTI;
}
bool queryModAddr() {
  uint8_t req[6] = { 0xDD, 0xDD, 0x00, 0x01, 0, 0 };
  uint16_t crc = modbusCRC16(req, 4);
  req[4] = crc & 0xFF;
  req[5] = crc >> 8;
  uint8_t rsp[16];
  while (rs485.available()) rs485.read();
  rs485Write(req, 6);
  uint32_t t0 = millis();
  int n = 0;
  while (millis() - t0 < 300 && n < sizeof(rsp)) {
    if (rs485.available()) rsp[n++] = rs485.read();
    else vTaskDelay(1);  // 让出CPU, 避免空转饿死IDLE
  }
  if (n >= 8 && rsp[0] == 0xDD && rsp[1] == 0xDD && rsp[2] == 0x00 && rsp[3] == 0x01 && crcOK(rsp, 8)) {
    portENTER_CRITICAL(&chgMux);
    chg.modAddr = (uint8_t)((rsp[4] << 8) | rsp[5]);
    portEXIT_CRITICAL(&chgMux);
    return true;
  }
  return false;
}
int scanSlave() {
  {
    uint8_t req[6] = { 0xDD, 0xDD, 0x00, 0x01, 0, 0 };
    uint16_t crc = modbusCRC16(req, 4);
    req[4] = crc & 0xFF;
    req[5] = crc >> 8;
    while (rs485.available()) rs485.read();
    rs485Write(req, 6);
    uint8_t rsp[16];
    int n = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 200) {
      if (n >= 8) break;
      if (rs485.available() && n < 16) rsp[n++] = rs485.read();
      else vTaskDelay(1);
    }
    if (n >= 8 && rsp[0] == 0xDD && rsp[1] == 0xDD && crcOK(rsp, 8)) return (int)((rsp[4] << 8) | rsp[5]);
  }
  for (int a = 0; a <= 247; a++) {
    esp_task_wdt_reset();
    {
      bool rk, ak;
      portENTER_CRITICAL(&chgMux);
      rk = chg.running;
      ak = chg.runActive;
      portEXIT_CRITICAL(&chgMux);
      if (rk && ak) setPower(true);
    }
    uint8_t req[8];
    req[0] = (uint8_t)a;
    req[1] = 0x03;
    req[2] = (uint8_t)(REG_READ >> 8);
    req[3] = (uint8_t)(REG_READ & 0xFF);
    req[4] = 0x00;
    req[5] = 0x03;
    uint16_t crc = modbusCRC16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = crc >> 8;
    while (rs485.available()) rs485.read();
    rs485Write(req, 8);
    uint8_t rsp[16];
    int n = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 80) {
      if (n >= 11) break;
      if (rs485.available() && n < 16) rsp[n++] = rs485.read();
      else vTaskDelay(1);
    }
    if (n >= 11 && rsp[0] == (uint8_t)a && rsp[1] == 0x03 && crcOK(rsp, 11)) return a;
  }
  return -1;
}
void decodeStatus(char* buf, int len) {
  uint16_t st;
  portENTER_CRITICAL(&chgMux);
  st = chg.rawStatus;
  portEXIT_CRITICAL(&chgMux);
  // 注: 本型号充电器正常充电时状态字为 0x0082 / 0x2082, bit7(0x80) 恒为1, 并非OVP(厂家模块同样如此),
  // 因此不能用 bit7 判 OVP, 否则正常充电也会显示 OVP. 状态取低4位.
  if (st & 0x40) snprintf(buf, len, "OCP");
  else if (st & 0x20) snprintf(buf, len, "SHORT");
  else if (st & 0x10) snprintf(buf, len, "OTP");
  else {
    static const char* tab[5] = { "IDLE", "BOOT", "CHG", "FULL", "LIM" };
    uint8_t s = st & 0x0F;
    if (s > 4) s = 0;
    snprintf(buf, len, "%s", tab[s]);
  }
}

// ================= WiFi / Web =================
WebServer server(DEF_WEB_PORT);
WebServer portalServer(80);
String wifiSSID, wifiPASS;
uint32_t lastWifiTry = 0;
bool portalActive = false, webReg = false, portalWeb = false, portalHandlers = false;
void wifiLoadCred() {
  prefs.begin("wifi", true);
  wifiSSID = "";
  wifiPASS = "";
  size_t l = prefs.getBytesLength("ssidB");
  if (l > 0 && l < 64) {
    char b[65];
    prefs.getBytes("ssidB", b, l);
    b[l] = 0;
    wifiSSID = String(b);
  }
  l = prefs.getBytesLength("passB");
  if (l > 0 && l < 64) {
    char b[65];
    prefs.getBytes("passB", b, l);
    b[l] = 0;
    wifiPASS = String(b);
  }
  prefs.end();
}
void wifiSaveCred(const String& s, const String& p) {
  prefs.begin("wifi", false);
  prefs.clear();
  if (s.length()) prefs.putBytes("ssidB", s.c_str(), s.length());
  if (p.length()) prefs.putBytes("passB", p.c_str(), p.length());
  prefs.end();
}
void startSTA() {
  if (wifiSSID.length() == 0) return;
  WiFi.mode(portalActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");  // NTP对时(东八区), 使串口时间戳可与抓包对时
  lastWifiTry = millis();
}
void startAP() {
  if (portalActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  portalActive = true;
  MDNS.begin("charger");
  if (!portalHandlers) {
    portalServer.onNotFound([] {
      String loc = "http://192.168.4.1:" + String((unsigned)webPort) + "/";
      portalServer.sendHeader("Location", loc, true);
      portalServer.send(302, "text/plain", "");
    });
    portalHandlers = true;
  }
  if (!portalWeb) {
    portalServer.begin();
    portalWeb = true;
  }
}
void stopAP() {
  if (portalWeb) {
    portalServer.stop();
    portalWeb = false;
  }
  portalActive = false;
  WiFi.softAPdisconnect(true);
  if (wifiSSID.length()) WiFi.mode(WIFI_STA);
  else WiFi.mode(WIFI_OFF);
}
bool pinOK() {
  String p = server.arg("pin");
  return (p == webPin || p == String(ADMIN_PIN));
}
void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

void hState() {
  char err[16];
  decodeStatus(err, sizeof(err));
  float v, a, w, sv, sa, tr, pm, ah, wh;
  uint16_t cnt, tripN, hb, port;
  uint8_t md, pr, addr;
  bool run, runAct, estop, lock, ap, rs, wf;
  char ip[48];
  portENTER_CRITICAL(&chgMux);
  v = chg.outV;
  a = chg.outA;
  w = chg.power;
  sv = chg.setV;
  sa = chg.setA;
  tr = chg.trickle;
  pm = chg.pwrMax;
  ah = chg.ah;
  wh = chg.wh;
  cnt = chg.cnt;
  md = chg.mode;
  pr = chg.preset;
  addr = chg.modAddr;
  run = chg.running;
  runAct = chg.runActive;
  estop = chg.estop;
  lock = chg.locked;
  ap = chg.apMode;
  rs = chg.rs485_ok;
  wf = chg.wifi_ok;
  strncpy(ip, chg.ipStr, sizeof(ip) - 1);
  ip[sizeof(ip) - 1] = 0;
  portEXIT_CRITICAL(&chgMux);
  tripN = tripCount;
  hb = hbSec;
  port = webPort;
  bool mute = bzMute;
  uint8_t vol = bzVol;
  char j[640];
  snprintf(j, sizeof(j),
           "{\"v\":%.1f,\"a\":%.2f,\"w\":%.0f,\"sv\":%.1f,\"sa\":%.1f,\"tr\":%.1f,\"pm\":%.0f,"
           "\"ah\":%.0f,\"wh\":%.0f,\"cnt\":%u,\"mode\":%u,\"preset\":%u,"
           "\"run\":%d,\"runActive\":%d,\"estop\":%d,\"lock\":%d,\"ap\":%d,\"rs\":%d,\"wf\":%d,"
           "\"ip\":\"%s\",\"st\":\"%s\",\"trip\":\"%s\",\"tripN\":%u,\"addr\":%u,"
           "\"hb\":%u,\"port\":%u,\"mute\":%d,\"vol\":%u,\"scan\":%u,\"scanFound\":%d,\"scanDone\":%d,"
           "\"rst\":%u,\"off\":%u}",
           v, a, w, sv, sa, tr, pm, ah, wh, cnt, md, pr,
           run ? 1 : 0, runAct ? 1 : 0, estop ? 1 : 0, lock ? 1 : 0, ap ? 1 : 0, rs ? 1 : 0, wf ? 1 : 0,
           ip, err, tripReason, tripN, addr, hb, port, mute ? 1 : 0, vol, (unsigned)scanResult, scanFound ? 1 : 0, scanDone ? 1 : 0,
           (unsigned)resetCnt, (unsigned)offCnt);
  cors();
  server.send(200, "application/json", j);
}
void hThemeGet() {
  char j[400];
  int p = 0;
  p += snprintf(j + p, sizeof(j) - p, "[");
  for (int i = 0; i < TH_N; i++) {
    uint16_t c;
    portENTER_CRITICAL(&chgMux);
    c = themeC[i];
    portEXIT_CRITICAL(&chgMux);
    p += snprintf(j + p, sizeof(j) - p, "%s\"#%02X%02X%02X\"", i ? "," : "",
                  (unsigned)(((c >> 11) & 31) * 255 / 31), (unsigned)(((c >> 5) & 63) * 255 / 63), (unsigned)((c & 31) * 255 / 31));
  }
  p += snprintf(j + p, sizeof(j) - p, "]");
  cors();
  server.send(200, "application/json", j);
}
void hThemeSet() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  String c = server.arg("c");
  int idx = 0;
  char buf[400];
  strncpy(buf, c.c_str(), sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char* tok = strtok(buf, ",");
  while (tok && idx < TH_N) {
    uint32_t rgb = strtoul(tok, NULL, 16);
    themeC[idx] = tft.color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    idx++;
    tok = strtok(NULL, ",");
  }
  if (idx == TH_N) {
    nvsSaveTheme();
    cors();
    server.send(200, "text/plain", "OK");
  } else {
    cors();
    server.send(400, "text/plain", "NEED 25");
  }
}
void hThemeReset() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  initColors();
  nvsSaveTheme();
  cors();
  server.send(200, "text/plain", "OK");
}
void hGetParam() {
  String f = server.arg("field");
  char b[16] = "0";
  portENTER_CRITICAL(&chgMux);
  if (f == "setV") snprintf(b, 16, "%.1f", chg.setV);
  else if (f == "setA") snprintf(b, 16, "%.1f", chg.setA);
  else if (f == "trickle") snprintf(b, 16, "%.1f", chg.trickle);
  else if (f == "pwrMax") snprintf(b, 16, "%.0f", chg.pwrMax);
  portEXIT_CRITICAL(&chgMux);
  cors();
  server.send(200, "text/plain", b);
}
void hSetParam() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  String f = server.arg("field");
  float v = server.arg("val").toFloat();
  if (v < 0) v = 0;
  portENTER_CRITICAL(&chgMux);
  if (f == "setV") {
    if (v > maxV) v = maxV;
    if (v >= 0.1f) chg.setV = v;
  } else if (f == "setA") {
    if (v > maxA) v = maxA;
    if (v >= 0.1f) chg.setA = v;
  } else if (f == "trickle") {
    if (v > maxA) v = maxA;
    chg.trickle = v;
  } else if (f == "pwrMax") {
    if (v > maxP) v = maxP;
    chg.pwrMax = v;
  }
  portEXIT_CRITICAL(&chgMux);
  applyModeCalc();
  nvsSave();
  cors();
  server.send(200, "text/plain", "OK");
}
void hBtn() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  String id = server.arg("id");
  bool acted = true, apNow = chg.apMode;
  portENTER_CRITICAL(&chgMux);
  if (id == "run") {
    if (!chg.estop) chg.running = !chg.running;
    else acted = false;
  } else if (id == "cc") chg.mode = (chg.mode == 0) ? 1 : 0;
  else if (id == "lock") chg.locked = !chg.locked;
  else if (id == "clr") {
    chg.ah = 0;
    chg.wh = 0;
  } else if (id == "clrall") {   // 全部清零: AH/WH + CNT (跳闸计数在下方一起清)
    chg.ah = 0;
    chg.wh = 0;
    chg.cnt = 0;
  } else if (id == "lcdinit") lcdReinitReq = true;   // 屏幕受干扰后重新初始化
  else if (id == "ap") chg.apMode = !chg.apMode;
  else acted = false;
  apNow = chg.apMode;
  portEXIT_CRITICAL(&chgMux);
  if (acted) {
    if (id == "cc") applyModeCalc();
    if (id == "ap") {
      if (apNow) startAP();
      else stopAP();
    }
    if (id == "clr") nvsSaveStats();
    if (id == "clrall") {
      tripCount = 0;
      tripReason[0] = 0;
      nvsSaveStats();
    }
    nvsSave();
  }
  cors();
  server.send(200, "text/plain", acted ? "OK" : "DENIED");
}
void hPreset() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  int n = server.arg("n").toInt();
  if (n >= 1 && n <= 5) {
    portENTER_CRITICAL(&chgMux);
    chg.setV = chg.preV[n - 1];
    chg.setA = chg.preA[n - 1];
    chg.preset = n;
    if (chg.mode == 0) chg.pwrMax = chg.setV * chg.setA;
    else if (chg.setV > 0.1f) chg.setA = chg.pwrMax / chg.setV;
    portEXIT_CRITICAL(&chgMux);
    nvsSave();
  }
  cors();
  server.send(200, "text/plain", "OK");
}
void hPresetsGet() {
  char j[200];
  int p = 0;
  p += snprintf(j + p, sizeof(j) - p, "[");
  for (int i = 0; i < 5; i++) {
    float v, a;
    portENTER_CRITICAL(&chgMux);
    v = chg.preV[i];
    a = chg.preA[i];
    portEXIT_CRITICAL(&chgMux);
    p += snprintf(j + p, sizeof(j) - p, "%s[%.1f,%.1f]", i ? "," : "", v, a);
  }
  p += snprintf(j + p, sizeof(j) - p, "]");
  cors();
  server.send(200, "application/json", j);
}
void hPresetSet() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  int n = server.arg("n").toInt();
  float v = server.arg("v").toFloat(), a = server.arg("a").toFloat();
  if (v < 0) v = 0;
  if (a < 0) a = 0;
  if (v > maxV) v = maxV;
  if (a > maxA) a = maxA;
  if (n >= 1 && n <= 5) {
    portENTER_CRITICAL(&chgMux);
    chg.preV[n - 1] = v;
    chg.preA[n - 1] = a;
    portEXIT_CRITICAL(&chgMux);
    nvsSave();
  }
  cors();
  server.send(200, "text/plain", "OK");
}
void hWifiSave() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  if (!server.hasArg("ssid")) {
    cors();
    server.send(400, "text/plain", "NO SSID");
    return;
  }
  String s = server.arg("ssid"), p = server.arg("pass");
  wifiSaveCred(s, p);
  wifiSSID = s;
  wifiPASS = p;
  WiFi.disconnect();
  startSTA();
  cors();
  server.send(200, "text/plain", "SAVED");
}
void hWifiGet() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  String j = "{\"ssid\":\"";
  for (unsigned i = 0; i < wifiSSID.length(); i++) {
    char c = wifiSSID[i];
    if (c == '\"' || c == '\\') j += '\\';
    if ((uint8_t)c >= 32) j += c;
  }
  j += "\",\"pass\":\"";
  for (unsigned i = 0; i < wifiPASS.length(); i++) {
    char c = wifiPASS[i];
    if (c == '\"' || c == '\\') j += '\\';
    if ((uint8_t)c >= 32) j += c;
  }
  j += "\"}";
  cors();
  server.send(200, "application/json", j);
}
void hPinSet() {
  String o = server.arg("old"), nw = server.arg("new");
  if (!(o == webPin || o == String(ADMIN_PIN))) {
    cors();
    server.send(401, "text/plain", "OLD PIN ERR");
    return;
  }
  if (nw.length() < 4 || nw.length() > 16) {
    cors();
    server.send(400, "text/plain", "LEN 4-16");
    return;
  }
  webPin = nw;
  nvsSaveCfg();
  cors();
  server.send(200, "text/plain", "OK");
}
void hSettings() {
  if (server.method() == HTTP_POST) {
    if (!pinOK()) {
      cors();
      server.send(401, "text/plain", "PIN ERR");
      return;
    }
    if (server.hasArg("hb")) {
      uint32_t h = server.arg("hb").toInt();
      if (h >= HB_MIN && h <= HB_MAX) hbSec = h;
    }
    if (server.hasArg("vol")) {
      int vv = server.arg("vol").toInt();
      if (vv >= 0 && vv <= 100) bzVol = (uint8_t)vv;
    }
    if (server.hasArg("mute")) bzMute = (server.arg("mute") == "1");
    if (server.hasArg("estop")) {
      int e = server.arg("estop").toInt();
      if (e >= 3 && e <= 50) estopClicks = (uint8_t)e;
    }
    if (server.hasArg("maxV")) {
      float x = server.arg("maxV").toFloat();
      if (x >= 1 && x <= 1000) maxV = x;
    }
    if (server.hasArg("maxA")) {
      float x = server.arg("maxA").toFloat();
      if (x >= 0.1f && x <= 1000) maxA = x;
    }
    if (server.hasArg("maxP")) {
      float x = server.arg("maxP").toFloat();
      if (x >= 0 && x <= 100000) maxP = x;
    }
    if (server.hasArg("rampMs")) {
      int x = server.arg("rampMs").toInt();
      if (x >= 200 && x <= 10000) rampMs = (uint16_t)x;
    }
    if (server.hasArg("rampSt")) {
      float x = server.arg("rampSt").toFloat();
      if (x >= 0.005f && x <= 5.0f) rampStepA = x;
    }
    if (server.hasArg("addr")) {
      int a = server.arg("addr").toInt();
      if (a >= 0 && a <= 247) {
        slaveAddr = (uint8_t)a;
        portENTER_CRITICAL(&chgMux);
        chg.modAddr = slaveAddr;
        portEXIT_CRITICAL(&chgMux);
      }
    }
    if (server.hasArg("port")) {
      uint16_t np = server.arg("port").toInt();
      if (np >= 1024) webPort = np;
    }
    nvsSaveCfg();
    cors();
    server.send(200, "text/plain", "OK");
    return;
  }
  char j[300];
  snprintf(j, sizeof(j), "{\"hb\":%u,\"port\":%u,\"mute\":%d,\"vol\":%u,\"estop\":%u,\"maxV\":%.1f,\"maxA\":%.1f,\"maxP\":%.0f,\"addr\":%u,\"rampMs\":%u,\"rampSt\":%.3f}", (unsigned)hbSec, (unsigned)webPort, bzMute ? 1 : 0, (unsigned)bzVol, (unsigned)estopClicks, maxV, maxA, maxP, (unsigned)slaveAddr, (unsigned)rampMs, rampStepA);
  cors();
  server.send(200, "application/json", j);
}
void hReboot() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  cors();
  server.send(200, "text/plain", "REBOOT");
  delay(300);
  ESP.restart();
}
void hScan() {
  if (!pinOK()) {
    cors();
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  scanDone = false;
  scanFound = false;
  scanResult = 0;
  scanReq = true;
  cors();
  server.send(200, "text/plain", "SCANNING");
}
bool otaAuth = false;
void handleOTAUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaAuth = pinOK();
    if (otaAuth && !Update.begin(UPDATE_SIZE_UNKNOWN)) Serial.println("OTA no space");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaAuth) Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaAuth) Update.end(true);
  }
}
void handleOTAPost() {
  if (!otaAuth) {
    server.send(401, "text/plain", "PIN ERR");
    return;
  }
  server.send(200, "text/plain", "OTA OK");
  delay(300);
  ESP.restart();
}
extern const char PAGE_ROOT[];
void hRoot() {
  cors();
  server.send_P(200, "text/html", PAGE_ROOT);
}
void webSetup() {
  server.on("/", hRoot);
  server.on("/state", hState);
  server.on("/theme", HTTP_GET, hThemeGet);
  server.on("/theme", HTTP_POST, hThemeSet);
  server.on("/theme/reset", hThemeReset);
  server.on("/getparam", hGetParam);
  server.on("/setparam", hSetParam);
  server.on("/btn", hBtn);
  server.on("/preset", hPreset);
  server.on("/presets", hPresetsGet);
  server.on("/preset_set", hPresetSet);
  server.on("/wifisave", hWifiSave);
  server.on("/wifiget", hWifiGet);
  server.on("/pinset", hPinSet);
  server.on("/settings", HTTP_GET, hSettings);
  server.on("/settings", HTTP_POST, hSettings);
  server.on("/reboot", hReboot);
  server.on("/scan", hScan);
  server.on("/update", HTTP_POST, handleOTAPost, handleOTAUpload);
  server.begin(webPort);
  webReg = true;
}
void wifiPoll() {
  static bool prevWifi = false;
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    portENTER_CRITICAL(&chgMux);
    chg.wifi_ok = true;
    strncpy(chg.ipStr, tmp, sizeof(chg.ipStr) - 1);
    chg.ipStr[sizeof(chg.ipStr) - 1] = 0;
    portEXIT_CRITICAL(&chgMux);
    if (!prevWifi) {
      prevWifi = true;
      bzReqWifi = true;
    }
  } else {
    prevWifi = false;
    portENTER_CRITICAL(&chgMux);
    chg.wifi_ok = false;
    strcpy(chg.ipStr, "--");
    portEXIT_CRITICAL(&chgMux);
    if (wifiSSID.length() && millis() - lastWifiTry > 15000) {
      WiFi.disconnect();
      WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
      lastWifiTry = millis();
    }
  }
  if (webReg) server.handleClient();
  if (portalActive) portalServer.handleClient();
}

// ================= 通信任务 =================
volatile bool cmdDirty = false;
float lastSentV = -1, lastSentA = -1;
float runI = 0;  // 本次充电已发出的电流设定值(只增不减)
uint32_t lastRunHeartbeat = 0, lastRunSetVI = 0, runStartMs = 0, lastIntMs = 0, lastStatSave = 0;
// 时间戳: 若已通过WiFi NTP对时则打印 HH:MM:SS.mmm, 否则退回开机毫秒
void timeStr(char* buf, int len) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) {  // 未对时
    snprintf(buf, len, "[t=%lum]", (unsigned long)millis());
    return;
  }
  struct tm tmv;
  time_t tt = tv.tv_sec;
  localtime_r(&tt, &tmv);
  snprintf(buf, len, "[%02d:%02d:%02d.%03d]", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(tv.tv_usec / 1000));
}
void protectShutdown(const char* reason) {
  setPower(false);
  portENTER_CRITICAL(&chgMux);
  chg.running = false;
  chg.runActive = false;
  portEXIT_CRITICAL(&chgMux);
  strncpy(tripReason, reason, 7);
  tripReason[7] = 0;
  tripCount++;
  nvsSaveStats();
  bzReqAlarm = true;
  Serial.printf("TRIP:%s #%u\n", reason, tripCount);
}
void commTask(void*) {
  rs485.begin(RS485_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
#if PIN_RS485_DE >= 0
  pinMode(PIN_RS485_DE, OUTPUT);
  digitalWrite(PIN_RS485_DE, LOW);
#endif
  delay(300);
  queryModAddr();
  esp_task_wdt_add(NULL);
  bool prev485 = false;
  uint8_t overCnt = 0;
  bool trickleSeen = false;
  bool bootShutdownDone = false;
  for (;;) {
    esp_task_wdt_reset();
    if (scanReq) {
      scanReq = false;
      Serial.println("SCAN start");
      int fa = scanSlave();
      scanFound = (fa >= 0);
      scanResult = (fa >= 0) ? (uint8_t)fa : 0;
      scanDone = true;
      Serial.printf("SCAN done found=%d addr=%d\n", scanFound ? 1 : 0, fa);
      if (fa >= 0) {
        slaveAddr = (uint8_t)fa;
        portENTER_CRITICAL(&chgMux);
        chg.modAddr = slaveAddr;
        portEXIT_CRITICAL(&chgMux);
        addrDirty = true;
      }
      rs485ErrCnt = 0;
      continue;
    }
    bool estop;
    portENTER_CRITICAL(&chgMux);
    estop = chg.estop;
    portEXIT_CRITICAL(&chgMux);
    if (estop) {
      setPower(false);
      portENTER_CRITICAL(&chgMux);
      chg.running = false;
      chg.runActive = false;
      portEXIT_CRITICAL(&chgMux);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    {
      bool rk, ak;
      portENTER_CRITICAL(&chgMux);
      rk = chg.running;
      ak = chg.runActive;
      portEXIT_CRITICAL(&chgMux);
      if (rk && ak) {
        setPower(true);
        lastRunHeartbeat = millis();
      }
    }
    bool ok = readStatus();
    portENTER_CRITICAL(&chgMux);
    chg.rs485_ok = ok;
    uint16_t rawSt = chg.rawStatus;
    bool runActNow = chg.runActive;
    float oV = chg.outV, oA = chg.outA, sVn = chg.setV, sAn = chg.setA;
    portEXIT_CRITICAL(&chgMux);
    if (ok && !prev485) bzReq485 = true;
    if (!ok && prev485) {
      offCnt++;
      lastOffMs = millis();
      char tsb[32];
      timeStr(tsb, sizeof(tsb));
      Serial.printf("%s 485 LOST #%u\n", tsb, offCnt);
    }
    prev485 = ok;
    if (!ok) {
      rs485ErrCnt++;
      if (rs485ErrCnt > 30) {
        rs485ErrCnt = 0;
        queryModAddr();
      }
      vTaskDelay(pdMS_TO_TICKS(20));  // 485离线时避免高频空转(防看门狗重启)
      continue;
    }
    rs485ErrCnt = 0;
    {  // 会话复位检测: RUN中状态由 CHG(2) 变为其它 = 充电器复位/重启
      static uint8_t prevS = 0xFF;
      uint8_t sNow = rawSt & 0x0F;
      if (runActNow && prevS == 2 && sNow != 2) {
        resetCnt++;
        lastResetMs = millis();
        char tsb[32];
        timeStr(tsb, sizeof(tsb));
        Serial.printf("%s SESSION RESET #%u raw=0x%04X state=%u V=%.1f A=%.2f | setV=%.1f setI=%.2f tgtI=%.2f\n",
                      tsb, resetCnt, (unsigned)rawSt, sNow, oV, oA, sVn, runI, sAn);
      }
      prevS = sNow;
    }
    if (!bootShutdownDone) {
      setPower(false);
      portENTER_CRITICAL(&chgMux);
      chg.running = false;
      chg.runActive = false;
      portEXIT_CRITICAL(&chgMux);
      bootShutdownDone = true;
      Serial.println("BOOT: safe power-OFF sent");
    }
    float sv, sa, tr, pm, rv, ra;
    bool run;
    bool active;
    portENTER_CRITICAL(&chgMux);
    sv = chg.setV;
    sa = chg.setA;
    tr = chg.trickle;
    pm = chg.pwrMax;
    run = chg.running;
    active = chg.runActive;
    rv = chg.outV;
    ra = chg.outA;
    portEXIT_CRITICAL(&chgMux);
    if (run && !active) {
      runI = RAMP_START_A;  // 电流从起始值开始, 之后只增不减
      if (runI > sa) runI = sa;
      setVI(sv, runI);
      vTaskDelay(pdMS_TO_TICKS(60));
      setPower(true);
      portENTER_CRITICAL(&chgMux);
      chg.runActive = true;
      if (chg.cnt < 65535) chg.cnt++;  // 计数封顶(不回绕); 手动清零见 长按CLR / 网页"清零记录"
      chg.ah = 0;
      chg.wh = 0;
      portEXIT_CRITICAL(&chgMux);
      tripReason[0] = 0;
      nvsSaveStats();
      lastRunHeartbeat = millis();
      lastRunSetVI = millis();
      runStartMs = millis();
      lastIntMs = millis();
      lastSentV = sv;
      lastSentA = runI;
      overCnt = 0;
      trickleSeen = false;
      bzReqRun = true;
    } else if (!run && active) {
      setPower(false);
      portENTER_CRITICAL(&chgMux);
      chg.runActive = false;
      portEXIT_CRITICAL(&chgMux);
      nvsSaveStats();
    } else if (run && active) {
      // 每 rampMs 发一次0x0F; 电流设定从RAMP_START_A单调爬升到目标, 到达后保持; 绝不下调
      // (下调/抖动会被充电器当成异常→复位充电会话, 小继电器动作后重启)
      if (millis() - lastRunSetVI > (uint32_t)rampMs) {
        if (runI < sa) {
          runI += rampStepA;
          if (runI > sa) runI = sa;
        }
        setVI(sv, runI);
        lastRunSetVI = millis();
        lastSentV = sv;
        lastSentA = runI;
        cmdDirty = false;
      }
      if (rv * ra > pm * PWR_PROT_RATIO) {
        if (++overCnt >= 2) {
          protectShutdown("PWR");
          vTaskDelay(pdMS_TO_TICKS(300));
          continue;
        }
      } else overCnt = 0;
      if (tr > 0.01f && ra >= tr) trickleSeen = true;
      if (millis() - runStartMs > TRICKLE_GUARD_MS && tr > 0.01f && trickleSeen && ra < tr) {
        protectShutdown("TRKL");
        vTaskDelay(pdMS_TO_TICKS(300));
        continue;
      }
      uint32_t now = millis();
      float dtH = (now - lastIntMs) / 3600000.0f;
      lastIntMs = now;
      if (dtH > 0 && dtH < 0.1f) {
        portENTER_CRITICAL(&chgMux);
        chg.wh += rv * ra * dtH;
        if (chg.setV > 0.1f) chg.ah = chg.wh / chg.setV;
        portEXIT_CRITICAL(&chgMux);
      }
      if (millis() - lastStatSave > 30000) {
        lastStatSave = millis();
        nvsSaveStats();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ================= 绘制 =================
static const int BOT_R = 3;
void card(int x, int y, int w, int h, uint16_t fill, bool focus = false, int r = 5) {
  sp.fillRoundRect(x, y, w, h, r, fill);
  if (focus) {
    sp.drawRoundRect(x - 1, y - 1, w + 2, h + 2, r + 1, C_FOCUS);
    sp.drawRoundRect(x - 2, y - 2, w + 4, h + 4, r + 2, C_FOCUS);
  }
}
void drawCenteredText(int x, int y, int w, int h, const char* txt, uint16_t color, int ts) {
  sp.setTextSize(ts);
  sp.setTextColor(color);
  int16_t tw = sp.textWidth(txt), realH = sp.fontHeight();
  sp.setCursor(x + (w - tw) / 2, y + (h - realH) / 2);
  sp.print(txt);
}
void bigNum(int x, int y, int w, int h, const char* val, const char* unit, uint16_t bg, int ts = 4) {
  uint16_t tc = textColorForBg(bg);
  sp.setTextSize(ts);
  sp.setTextColor(tc);
  int16_t vw = sp.textWidth(val), uw = sp.textWidth(unit), total = vw + 4 + uw;
  int16_t realH = sp.fontHeight(), cx = x + (w - total) / 2, cy = y + (h - realH) / 2;
  sp.setCursor(cx, cy);
  sp.print(val);
  sp.setCursor(cx + vw + 4, cy);
  sp.print(unit);
}
void statCard(int x, int y, int w, int h, const char* label, const char* val, uint16_t fill) {
  card(x, y, w, h, fill, false, BOT_R);
  uint16_t tc = textColorForBg(fill);
  sp.setTextSize(2);
  int16_t valH = sp.fontHeight();
  sp.setTextSize(1);
  int16_t lblH = sp.fontHeight();
  int16_t totalH = valH + 1 + lblH, startY = y + 2 + (h - 4 - totalH) / 2;
  sp.setTextSize(2);
  sp.setTextColor(tc);
  int16_t vw = sp.textWidth(val);
  sp.setCursor(x + (w - vw) / 2, startY);
  sp.print(val);
  sp.setTextSize(1);
  int16_t lw = sp.textWidth(label);
  sp.setCursor(x + (w - lw) / 2, startY + valH + 1);
  sp.print(label);
}
void btnCard(int x, int y, int w, int h, const char* label, uint16_t fill, bool focus = false) {
  card(x, y, w, h, fill, focus, BOT_R);
  drawCenteredText(x, y, w, h, label, textColorForBg(fill), 2);
}
void drawMain() {
  sp.fillSprite(themeC[TH_BG]);
  char buf[24];
  const int topY = 2, bottomStartY = 154, leftGap = 2, rightGap = 3;
  const int upperAvailH = bottomStartY - topY - 4;
  const int Lx = 4, Lw = 200, Lh = (upperAvailH - leftGap * 2) / 3;
  const int Rx = 208, Rw = 108;
  const int leftBottomEdge = topY + Lh * 3 + leftGap * 2;
  const int Rh = (leftBottomEdge - topY - rightGap * 3) / 4;
  float oV, oA, pw, sV, sA, tr, pm, ah, wh;
  uint16_t cnt;
  bool rs, wf, ap, lk, rn;
  uint8_t md, pr;
  portENTER_CRITICAL(&chgMux);
  oV = chg.outV;
  oA = chg.outA;
  pw = chg.power;
  sV = chg.setV;
  sA = chg.setA;
  tr = chg.trickle;
  pm = chg.pwrMax;
  ah = chg.ah;
  wh = chg.wh;
  cnt = chg.cnt;
  rs = chg.rs485_ok;
  wf = chg.wifi_ok;
  ap = chg.apMode;
  lk = chg.locked;
  rn = chg.running;
  md = chg.mode;
  pr = chg.preset;
  uint8_t addr = chg.modAddr;
  char ip[48];
  strncpy(ip, chg.ipStr, sizeof(ip) - 1);
  ip[sizeof(ip) - 1] = 0;
  portEXIT_CRITICAL(&chgMux);
  char err[16];
  decodeStatus(err, sizeof(err));
  int ly = topY;
  sprintf(buf, "%.1f", oV);
  card(Lx, ly, Lw, Lh, themeC[TH_LV]);
  bigNum(Lx, ly, Lw, Lh, buf, "V", themeC[TH_LV], 4);
  ly += Lh + leftGap;
  sprintf(buf, "%.2f", oA);
  card(Lx, ly, Lw, Lh, themeC[TH_LA]);
  bigNum(Lx, ly, Lw, Lh, buf, "A", themeC[TH_LA], 4);
  ly += Lh + leftGap;
  sprintf(buf, "%.0f", pw);
  card(Lx, ly, Lw, Lh, themeC[TH_LW]);
  bigNum(Lx, ly, Lw, Lh, buf, "W", themeC[TH_LW], 4);
  int ry = topY;
  sprintf(buf, "%.1f", sV);
  card(Rx, ry, Rw, Rh, themeC[TH_RV], focIdx == FC_SETV);
  bigNum(Rx, ry, Rw, Rh, buf, "V", themeC[TH_RV], 3);
  foc[FC_SETV] = { Rx, ry, Rw, Rh };
  ry += Rh + rightGap;
  bool aAuto = (md == 1);
  uint16_t raC = aAuto ? blendC(themeC[TH_RA], themeC[TH_BG], 0.5f) : themeC[TH_RA];
  sprintf(buf, "%.1f", sA);
  card(Rx, ry, Rw, Rh, raC, focIdx == FC_SETA);
  bigNum(Rx, ry, Rw, Rh, buf, "A", raC, 3);
  foc[FC_SETA] = { Rx, ry, Rw, Rh };
  ry += Rh + rightGap;
  sprintf(buf, "%.1f", tr);
  card(Rx, ry, Rw, Rh, themeC[TH_RTR], focIdx == FC_TRICKLE);
  bigNum(Rx, ry, Rw, Rh, buf, "A", themeC[TH_RTR], 3);
  foc[FC_TRICKLE] = { Rx, ry, Rw, Rh };
  ry += Rh + rightGap;
  int pmH = leftBottomEdge - ry;
  bool pmAuto = (md == 0);
  uint16_t pmC = pmAuto ? blendC(themeC[TH_RPM], themeC[TH_BG], 0.5f) : themeC[TH_RPM];
  sprintf(buf, "%.0f", pm);
  card(Rx, ry, Rw, pmH, pmC, focIdx == FC_PWRMAX);
  bigNum(Rx, ry, Rw, pmH, buf, "W", pmC, 3);
  foc[FC_PWRMAX] = { Rx, ry, Rw, pmH };
  const int bColW = 74, bColGap = 4, bRowH = 26, bRowGap = 3;
  const int bStartX = (320 - (bColW * 4 + bColGap * 3)) / 2;
  const int c0 = bStartX, c1 = c0 + bColW + bColGap, c2 = c1 + bColW + bColGap, c3 = c2 + bColW + bColGap;
  int sy = bottomStartY;
  sprintf(buf, "%.0f", ah);
  statCard(c0, sy, bColW, bRowH, "AH", buf, themeC[TH_AH]);
  sprintf(buf, "%.0f", wh);
  statCard(c1, sy, bColW, bRowH, "WH", buf, themeC[TH_WH]);
  card(c2, sy, bColW, bRowH, themeC[TH_PR], focIdx == FC_PR, BOT_R);
  sprintf(buf, "PRE%d", pr);
  drawCenteredText(c2, sy, bColW, bRowH, buf, textColorForBg(themeC[TH_PR]), 2);
  foc[FC_PR] = { c2, sy, bColW, bRowH };
  btnCard(c3, sy, bColW, bRowH, md == 0 ? "CC" : "CP", themeC[TH_CC], focIdx == FC_CC);
  foc[FC_CC] = { c3, sy, bColW, bRowH };
  sy += bRowH + bRowGap;
  card(c0, sy, bColW, bRowH, themeC[TH_CNT], false, BOT_R);
  sprintf(buf, "%u", cnt);
  drawCenteredText(c0, sy, bColW, bRowH, buf, textColorForBg(themeC[TH_CNT]), 2);
  card(c1, sy, bColW, bRowH, themeC[TH_ERR], false, BOT_R);
  drawCenteredText(c1, sy, bColW, bRowH, err, textColorForBg(themeC[TH_ERR]), 2);
  uint16_t apC = ap ? themeC[TH_APON] : themeC[TH_APOFF];
  btnCard(c2, sy, bColW, bRowH, "AP", apC, focIdx == FC_AP);
  foc[FC_AP] = { c2, sy, bColW, bRowH };
  btnCard(c3, sy, bColW, bRowH, "CLR", themeC[TH_CLR], focIdx == FC_CLR);
  foc[FC_CLR] = { c3, sy, bColW, bRowH };
  sy += bRowH + bRowGap;
  uint16_t c485 = rs ? breathColor(themeC[TH_OK485]) : themeC[TH_OFF485];
  card(c0, sy, bColW, bRowH, c485, false, BOT_R);
  drawCenteredText(c0, sy, bColW, bRowH, "485", rs ? TFT_WHITE : textColorForBg(c485), 2);
  uint16_t cWf = wf ? breathColor(themeC[TH_OKWIFI]) : themeC[TH_OFFWIFI];
  card(c1, sy, bColW, bRowH, cWf, false, BOT_R);
  drawCenteredText(c1, sy, bColW, bRowH, "WIFI", wf ? TFT_WHITE : textColorForBg(cWf), 2);
  uint16_t lkC = lk ? breathColor(themeC[TH_LOCKON]) : themeC[TH_LOCKOFF];
  card(c2, sy, bColW, bRowH, lkC, focIdx == FC_LOCK, BOT_R);
  drawCenteredText(c2, sy, bColW, bRowH, "LOCK", lk ? TFT_WHITE : textColorForBg(lkC), 2);
  foc[FC_LOCK] = { c2, sy, bColW, bRowH };
  uint16_t rnC = rn ? breathColor(themeC[TH_RUNON]) : themeC[TH_RUNOFF];
  card(c3, sy, bColW, bRowH, rnC, focIdx == FC_RUN, BOT_R);
  drawCenteredText(c3, sy, bColW, bRowH, rn ? "RUN" : "STOP", TFT_WHITE, 2);
  foc[FC_RUN] = { c3, sy, bColW, bRowH };
  if (editing && focIdx <= FC_PWRMAX) {
    FR r = foc[focIdx];
    sp.drawRoundRect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 8, C_EDIT_HL);
  }
  sp.setTextSize(1);
  sp.setTextColor(C_DIM);
  sp.setCursor(4, 233);
  sp.printf("ADR:%u %s %s TRIP:%s#%u", addr, ap ? "AP:ON" : "AP:OFF", wf ? ip : err, tripReason, tripCount);
}
void drawEstop() {
  float f = breathVal();
  uint8_t rb = (uint8_t)(255 * f);
  sp.fillScreen(tft.color565(30, 0, 0));
  sp.fillCircle(160, 105, 70, tft.color565(120, 0, 0));
  sp.fillCircle(160, 105, 62, tft.color565(rb, 10, 10));
  sp.fillCircle(160, 105, 50, tft.color565(rb, 40, 40));
  sp.fillCircle(160, 105, 20, tft.color565(60, 0, 0));
  sp.setTextSize(4);
  sp.setTextColor(TFT_WHITE);
  int16_t tw = sp.textWidth("E-STOP");
  sp.setCursor(160 - tw / 2, 185);
  sp.print("E-STOP");
  sp.setTextSize(1);
  sp.setTextColor(tft.color565(255, 180, 180));
  char hint[48];
  snprintf(hint, sizeof(hint), "%ux CLICK ENCODER TO RESET", (unsigned)estopClicks);
  tw = sp.textWidth(hint);
  sp.setCursor(160 - tw / 2, 218);
  sp.print(hint);
}
void drawPresetPage() {
  sp.fillSprite(themeC[TH_BG]);
  for (int i = 0; i < 5; i++) {
    int y = 10 + i * 42;
    bool isV = (preCur == i * 2), isA = (preCur == i * 2 + 1), rowActive = isV || isA;
    uint16_t bg = rowActive ? preColors[i] : tft.color565(22, 26, 40);
    sp.fillRoundRect(10, y, 300, 38, 6, bg);
    if (isV) sp.drawRoundRect(65, y + 2, 110, 34, 5, C_FOCUS);
    if (isA) sp.drawRoundRect(185, y + 2, 110, 34, 5, C_FOCUS);
    uint16_t tc = textColorForBg(bg);
    int16_t realH = sp.fontHeight();
    char nb[4];
    sprintf(nb, "P%d", i + 1);
    sp.setTextSize(2);
    sp.setTextColor(rowActive ? tc : C_DIM);
    sp.setCursor(22, y + (38 - realH) / 2);
    sp.print(nb);
    char vb[10];
    sprintf(vb, "%.1fV", chg.preV[i]);
    sp.setTextColor(isV ? C_EDIT_HL : tc);
    sp.setCursor(75, y + (38 - realH) / 2);
    sp.print(vb);
    char ab[10];
    sprintf(ab, "%.1fA", chg.preA[i]);
    sp.setTextColor(isA ? C_EDIT_HL : tc);
    sp.setCursor(195, y + (38 - realH) / 2);
    sp.print(ab);
  }
  int backY = 10 + 5 * 42;
  if (preCur == PRE_BACK) {
    sp.drawRoundRect(10, backY, 300, 26, 6, C_FOCUS);
    sp.setTextSize(2);
    sp.setTextColor(C_BACK_TXT);
    sp.setCursor(110, backY + (26 - sp.fontHeight()) / 2);
    sp.print("< BACK >");
  }
  sp.setTextSize(1);
  sp.setTextColor(C_DIM);
  sp.setCursor(8, 228);
  sp.print("SW:Edit | LongPress:Apply | ExtBtn:Back");
}

// ================= 交互 =================
void applyPresetAndExit(uint8_t idx) {
  if (idx > 4) idx = 0;
  portENTER_CRITICAL(&chgMux);
  chg.setV = chg.preV[idx];
  chg.setA = chg.preA[idx];
  chg.preset = idx + 1;
  if (chg.mode == 0) chg.pwrMax = chg.setV * chg.setA;
  else if (chg.setV > 0.1f) chg.setA = chg.pwrMax / chg.setV;
  portEXIT_CRITICAL(&chgMux);
  nvsSave();
  pagePreset = false;
  editing = false;
  clearButtonAndLock();
}
void handleInput() {
  int8_t step = getEncoderStep();
  readButtons();
  if (pageTransitionLock && isButtonFullyReleased(BTN_EC11_SW)) pageTransitionLock = false;
  bool shortP = getShortPress(BTN_EC11_SW), longP = getLongPress(BTN_EC11_SW), extP = getShortPress(BTN_EXT);
  if (shortP) {
    uint32_t now = millis();
    if (now - burstStart > BURST_WINDOW_MS) {
      burstStart = now;
      burstCnt = 1;
    } else burstCnt++;
    if (burstCnt >= estopClicks) {
      burstCnt = 0;
      portENTER_CRITICAL(&chgMux);
      chg.estop = !chg.estop;
      if (chg.estop) chg.running = false;
      portEXIT_CRITICAL(&chgMux);
      bzReqAlarm = true;
      editing = false;
      pagePreset = false;
      clearButtonAndLock();
      Serial.println(chg.estop ? "!!! E-STOP !!!" : "E-STOP cleared");
      return;
    }
    if (burstCnt >= BURST_SUPPRESS) return;
  } else {
    if (millis() - burstStart > BURST_WINDOW_MS) burstCnt = 0;
  }
  if (chg.estop) {
    getShortPress(BTN_EXT);
    getLongPress(BTN_EXT);
    step = 0;
    return;
  }
  if (pagePreset) {
    if (longP && preCur != PRE_BACK) {
      applyPresetAndExit(preCur / 2);
      return;
    }
    if (editing) {
      if (step) {
        bzReqFocus = true;
        if (preCur % 2 == 0) chg.preV[preCur / 2] = max(0.1f, min(maxV, chg.preV[preCur / 2] + step * 0.1f));
        else chg.preA[preCur / 2] = max(0.1f, min(maxA, chg.preA[preCur / 2] + step * 0.1f));
      }
      if (shortP) {
        editing = false;
        nvsSave();
      }
    } else {
      if (step) {
        bzReqFocus = true;
        int n = (int)preCur + step;
        if (n < 0) n = PRE_BACK;
        if (n > PRE_BACK) n = 0;
        preCur = (int8_t)n;
      }
      if (shortP) {
        if (preCur == PRE_BACK) {
          pagePreset = false;
          clearButtonAndLock();
        } else editing = true;
      }
      if (extP) {
        pagePreset = false;
        clearButtonAndLock();
      }
    }
    return;
  }
  if (editing) {
    if (step && fieldEditable(focIdx)) {
      bzReqFocus = true;
      portENTER_CRITICAL(&chgMux);
      switch (focIdx) {
        case FC_SETV: chg.setV = max(0.1f, min(maxV, chg.setV + step * 0.1f)); break;
        case FC_SETA: chg.setA = max(0.1f, min(maxA, chg.setA + step * 0.1f)); break;
        case FC_TRICKLE: chg.trickle = max(0.0f, min(maxA, chg.trickle + step * 0.1f)); break;
        case FC_PWRMAX: chg.pwrMax = max(0.0f, min(maxP, chg.pwrMax + step * 10.0f)); break;
        default: break;
      }
      portEXIT_CRITICAL(&chgMux);
      applyModeCalc();
      cmdDirty = true;
    }
    if (shortP) {
      editing = false;
      nvsSave();
    }
    return;
  }
  if (step) {
    bzReqFocus = true;
    if (!chg.locked) {
      int n = (int)focIdx + step;
      if (n < 0) n = FC_TOTAL - 1;
      if (n >= FC_TOTAL) n = 0;
      focIdx = (uint8_t)n;
    }
  }
  if (longP && focIdx == FC_CLR && !chg.locked) {
    // 长按 CLR: 全部清零(AH/WH + CNT + 跳闸计数)
    portENTER_CRITICAL(&chgMux);
    chg.ah = 0;
    chg.wh = 0;
    chg.cnt = 0;
    portEXIT_CRITICAL(&chgMux);
    tripCount = 0;
    tripReason[0] = 0;
    nvsSaveStats();
    bzReqFocus = true;
    return;
  }
  if (shortP) {
    if (focIdx <= FC_PWRMAX) {
      if (chg.locked) return;
      if (fieldEditable(focIdx)) editing = true;
      return;
    }
    if (chg.locked && focIdx != FC_LOCK) return;
    switch (focIdx) {
      case FC_PR:
        pagePreset = true;
        preCur = (chg.preset - 1) * 2;
        break;
      case FC_CC:
        {
          portENTER_CRITICAL(&chgMux);
          chg.mode = (chg.mode == 0) ? 1 : 0;
          portEXIT_CRITICAL(&chgMux);
          applyModeCalc();
          nvsSave();
          break;
        }
      case FC_AP:
        {
          portENTER_CRITICAL(&chgMux);
          chg.apMode = !chg.apMode;
          bool on = chg.apMode;
          portEXIT_CRITICAL(&chgMux);
          if (on) startAP();
          else stopAP();
          break;
        }
      case FC_CLR:
        {
          portENTER_CRITICAL(&chgMux);
          chg.ah = 0;
          chg.wh = 0;
          portEXIT_CRITICAL(&chgMux);
          nvsSaveStats();
          break;
        }
      case FC_LOCK:
        {
          portENTER_CRITICAL(&chgMux);
          chg.locked = !chg.locked;
          portEXIT_CRITICAL(&chgMux);
          break;
        }
      case FC_RUN:
        {
          portENTER_CRITICAL(&chgMux);
          chg.running = !chg.running;
          portEXIT_CRITICAL(&chgMux);
          break;
        }
      default: break;
    }
  }
  if (getLongPress(BTN_EXT)) nvsSave();
}

// ================= 内嵌网页 =================
const char PAGE_ROOT[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>充电器远程控制</title>
<style>
:root{
 --bg:#0b0e17;--bg2:#11141f;--card:#161a26;--card2:#1c2130;
 --fg:#e8ecf3;--muted:#8a93a6;--line:#252b3b;
 --acc:#2ea8ff;--acc2:#0b84e0;--ok:#18b26b;--red:#e5484d;--uiw:900px;--r:12px;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;min-height:100vh;background:radial-gradient(1200px 600px at 50% -10%,#16203a 0%,var(--bg) 60%) fixed;color:var(--fg);
 font-family:"Inter","Segoe UI",system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;
 display:flex;flex-direction:column;align-items:center;gap:10px;padding:14px 12px 24px}
.bar,#st,#panel{width:100%;max-width:var(--uiw)}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
input,select{font-size:14px;padding:9px 11px;border-radius:10px;border:1px solid var(--line);background:var(--card);color:var(--fg);outline:none;transition:.15s}
input::placeholder{color:#5b6579}
input:focus,select:focus{border-color:var(--acc);box-shadow:0 0 0 3px rgba(46,168,255,.18)}
input{flex:1;min-width:84px}
button{font-size:14px;font-weight:600;padding:9px 14px;border-radius:10px;border:0;cursor:pointer;color:#fff;
 background:linear-gradient(180deg,var(--acc),var(--acc2));transition:.15s;box-shadow:0 2px 10px rgba(0,0,0,.28)}
button:hover{filter:brightness(1.08);transform:translateY(-1px)}
button:active{transform:translateY(0);filter:brightness(.95)}
button.gray{background:linear-gradient(180deg,#3a4256,#2b3143)}
button.red{background:linear-gradient(180deg,#ff5a60,#d13438)}
button.green{background:linear-gradient(180deg,#23c97a,#12915a)}
#st{font-size:12.5px;color:var(--muted);background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:10px 12px;line-height:1.7;word-break:break-all}
canvas#ui{border-radius:14px;border:1px solid var(--line);box-shadow:0 12px 40px rgba(0,0,0,.5),0 0 24px rgba(46,168,255,.10);touch-action:manipulation;cursor:pointer;display:block;background:#000}
#panel{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:12px;box-shadow:0 8px 30px rgba(0,0,0,.35)}
#panel h3{margin:0 0 8px;color:#cfe4ff;font-size:14px;text-align:center;font-weight:600;letter-spacing:.3px}
#wave{width:100%;height:200px;background:#0b0f19;border:1px solid var(--line);border-radius:10px;display:block}
.ovl{position:fixed;inset:0;background:rgba(4,7,14,.66);backdrop-filter:blur(4px);-webkit-backdrop-filter:blur(4px);display:none;justify-content:center;align-items:center;z-index:50;padding:14px}
.box{background:linear-gradient(180deg,var(--card2),var(--card));border:1px solid var(--line);border-radius:16px;padding:18px;width:min(420px,94vw);max-height:92vh;overflow:auto;box-shadow:0 24px 60px rgba(0,0,0,.6)}
.box h3{margin:0 0 12px;color:#cfe4ff;font-size:15px;text-align:center;font-weight:700}
#npVal{width:100%;text-align:center;font-size:30px;font-weight:700;color:#ffd54a;background:#0b0f19;border:1px solid var(--line);border-radius:10px;padding:8px;margin-bottom:12px}
.keys{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.keys button{padding:15px 0;font-size:21px;background:linear-gradient(180deg,#333c52,#262d3f);font-weight:600}
.full{width:100%;padding:12px;margin-top:8px;font-size:15px}
.row{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:9px 0;border-bottom:1px solid var(--line)}
.row:last-child{border-bottom:0}
.row input,.row select{width:110px;flex:none}
.row label{min-width:60px;font-size:13.5px;color:#c3cbda}
#pickSw{height:46px;border-radius:12px;margin-bottom:12px;border:1px solid var(--line)}
#toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%);background:linear-gradient(180deg,var(--acc),var(--acc2));color:#fff;padding:10px 20px;border-radius:999px;display:none;z-index:99;font-size:13px;max-width:92vw;text-align:center;box-shadow:0 10px 30px rgba(0,0,0,.5)}
.hint{font-size:11.5px;color:var(--muted);margin-top:6px;text-align:center}
</style></head><body>
<div class="bar" id="modeBar">
 <button id="mHttp" class="green" onclick="setMode('http')">网页/HTTP</button>
 <button id="mSer" class="gray" onclick="setMode('serial')">串口485/DTU</button>
 <button class="gray" onclick="serialConnect()">选择串口</button>
 <span id="serialSt" class="hint" style="flex:1;text-align:left"></span>
</div>
<div class="bar" id="httpBar">
 <input id="addr" placeholder="IP:端口 / [IPv6]:端口 / 域名:端口（本机留空）">
 <button onclick="connect()">连接</button>
 <input id="pin" type="password" placeholder="PIN" style="max-width:90px">
</div>
<div class="bar" id="wifiBar">
 <input id="ssid" placeholder="WiFi SSID">
 <input id="pw" type="password" placeholder="WiFi密码" style="max-width:130px">
 <button onclick="saveWifi()">配网</button>
</div>
<div class="bar" id="toolBar">
 <button onclick="openPresets()">预设</button>
 <button onclick="openColor()">配色</button>
 <button onclick="openCfg()">设置</button>
 <button onclick="openPin()">改PIN</button>
 <button onclick="doOTA()">OTA</button>
 <button class="red" onclick="doReboot()">重启</button>
</div>
<div id="st">未连接</div>
<canvas id="ui"></canvas>
<div id="panel">
 <h3>电压 / 电流 实时波形</h3>
 <canvas id="wave"></canvas>
 <div class="hint"><span style="color:#fd0">&#9679;电压V(左轴)</span> &nbsp; <span style="color:#0f8">&#9679;电流A(右轴)</span></div>
</div>
<input type="file" id="otaFile" accept=".bin" style="display:none">
<div id="toast"></div>

<div class="ovl" id="npOvl"><div class="box">
 <h3 id="npTitle">设定值</h3>
 <input id="npVal" readonly>
 <div class="keys">
  <button onclick="nk('7')">7</button><button onclick="nk('8')">8</button><button onclick="nk('9')">9</button>
  <button onclick="nk('4')">4</button><button onclick="nk('5')">5</button><button onclick="nk('6')">6</button>
  <button onclick="nk('1')">1</button><button onclick="nk('2')">2</button><button onclick="nk('3')">3</button>
  <button onclick="nk('.')">.</button><button onclick="nk('0')">0</button><button onclick="nk('B')">退</button>
 </div>
 <button class="full green" onclick="npConfirm()">确认下发（二次确认）</button>
 <button class="full gray" onclick="hide('npOvl')">取消</button>
</div></div>

<div class="ovl" id="cfOvl"><div class="box">
 <h3 id="cfTitle">确认操作</h3>
 <div id="cfMsg" class="hint"></div>
 <button class="full green" onclick="cfYes()">确认</button>
 <button class="full gray" onclick="hide('cfOvl')">取消</button>
</div></div>

<div class="ovl" id="preOvl"><div class="box">
 <h3>选择预设</h3>
 <button class="full" style="background:#1e5adc" onclick="applyPre(1)">PRESET 1</button>
 <button class="full" style="background:#0ab45a" onclick="applyPre(2)">PRESET 2</button>
 <button class="full" style="background:#e6aa00;color:#000" onclick="applyPre(3)">PRESET 3</button>
 <button class="full" style="background:#f05014" onclick="applyPre(4)">PRESET 4</button>
 <button class="full" style="background:#a028dc" onclick="applyPre(5)">PRESET 5</button>
 <button class="full gray" onclick="hide('preOvl')">取消</button>
</div></div>

<div class="ovl" id="peOvl"><div class="box" style="width:min(520px,96vw)">
 <h3>预设编辑（与实机 5 组对应）</h3>
 <div id="peList"></div>
 <button class="full gray" onclick="hide('peOvl')">关闭</button>
</div></div>

<div class="ovl" id="coOvl"><div class="box" style="width:min(720px,96vw)">
 <h3>配色（点色块直接调色，双态色块左右两半分别对应 关/开）</h3>
 <canvas id="ui2" style="width:100%;max-width:660px;display:block;margin:0 auto;cursor:pointer"></canvas>
 <div class="bar" style="margin-top:8px">
  <button class="green" style="flex:1" onclick="colorSave()">保存配色</button>
  <button class="gray" style="flex:1" onclick="colorReset()">恢复默认</button>
  <button class="gray" style="flex:1" onclick="hide('coOvl')">关闭</button>
 </div>
</div></div>

<div class="ovl" id="pickOvl"><div class="box">
 <h3 id="pickTitle">调色</h3>
 <div id="pickSw"></div>
 <div class="row"><label>R</label><input id="pkR" type="range" min="0" max="255" style="flex:1"><span id="pkRv" style="width:34px;text-align:right;font-size:13px"></span></div>
 <div class="row"><label>G</label><input id="pkG" type="range" min="0" max="255" style="flex:1"><span id="pkGv" style="width:34px;text-align:right;font-size:13px"></span></div>
 <div class="row"><label>B</label><input id="pkB" type="range" min="0" max="255" style="flex:1"><span id="pkBv" style="width:34px;text-align:right;font-size:13px"></span></div>
 <input id="pkHex" placeholder="#RRGGBB" style="width:100%;text-align:center;margin-top:8px">
 <button class="full green" onclick="pickOk()">确定</button>
 <button class="full gray" onclick="hide('pickOvl')">取消</button>
</div></div>

<div class="ovl" id="cfGOvl"><div class="box">
 <h3>系统设置</h3>
 <div class="row"><label>保活s</label><input id="cfgHb" type="number" min="1" max="600"></div>
 <div class="row"><label>端口</label><input id="cfgPort" type="number" min="1024" max="65535"></div>
 <div class="row"><label>音量</label><input id="cfgVol" type="range" min="0" max="100" style="flex:1"></div>
 <div class="row"><label>静音</label><select id="cfgMute"><option value="0">关</option><option value="1">开</option></select></div>
 <div class="row"><label>急停连按</label><input id="cfgEstop" type="number" min="3" max="50"></div>
 <div class="row"><label>最大电压</label><input id="cfgMaxV" type="number" step="0.1" min="1" max="1000"></div>
 <div class="row"><label>最大电流</label><input id="cfgMaxA" type="number" step="0.1" min="0.1" max="1000"></div>
 <div class="row"><label>最大功率</label><input id="cfgMaxP" type="number" step="1" min="0" max="100000"></div>
 <div class="row"><label>爬坡间隔ms</label><input id="cfgRampMs" type="number" step="50" min="200" max="10000"></div>
 <div class="row"><label>爬坡步进A</label><input id="cfgRampSt" type="number" step="0.005" min="0.005" max="5"></div>
 <div class="row"><label>从机地址</label><input id="cfgAddr" type="number" min="0" max="247" style="width:70px"><button class="gray" onclick="doScan()">扫描</button><span id="scanRes" class="hint" style="text-align:right"></span></div>
 <button class="full gray" onclick="clrAll()">清零 AH/WH/CNT/TRIP</button>
 <button class="full gray" onclick="lcdInit()">重初始化屏幕</button>
<button class="full green" onclick="cfgSave()">保存（端口改动需重启）</button>
 <button class="full gray" onclick="hide('cfGOvl')">取消</button>
</div></div>

<div class="ovl" id="pnOvl"><div class="box">
 <h3>修改PIN</h3>
 <input id="pnOld" type="password" placeholder="当前PIN/管理员密码" style="width:100%;margin-bottom:8px">
 <input id="pnNew" type="password" placeholder="新PIN(4-16位)" style="width:100%;margin-bottom:10px">
 <button class="full green" onclick="pinSave()">确认修改</button>
 <button class="full gray" onclick="hide('pnOvl')">取消</button>
</div></div>

<div class="ovl" id="askOvl"><div class="box">
 <h3>需要 PIN 码</h3>
 <input id="askPin" type="password" placeholder="请输入PIN" style="width:100%;margin-bottom:10px">
 <button class="full green" onclick="askOk()">确定并重试</button>
 <button class="full gray" onclick="hide('askOvl')">取消</button>
</div></div>

<script>
var W=320,H=240,S=2,BASE='',st=null,curField=null,pendAction=null,coSel=0,chartAxis='v',mode='http',serialMode=false;
var DEFTHEME=[[10,12,20],[240,180,10],[10,170,70],[220,45,35],[20,120,190],[200,130,10],[140,60,190],[90,150,30],[240,130,20],[0,100,140],[80,60,220],[230,40,100],[220,40,160],[240,180,0],[100,60,160],[255,60,220],[230,220,20],[70,130,180],[255,40,40],[0,170,70],[230,25,25],[70,72,80],[0,200,120],[70,72,80],[50,120,255]];
var theme=DEFTHEME.map(function(a){return a.slice()});
var hist={v:[],a:[],w:[]};
var last={v:54.6,a:15.0,tr:0.5,pm:819,run:false,preset:1,mode:0};
var limV=100,limA=50,limP=3000;
var cv=document.getElementById('ui'),ctx=cv.getContext('2d');
var cv2=document.getElementById('ui2'),ctx2=cv2.getContext('2d');
function hide(id){document.getElementById(id).style.display='none'}
function show(id){document.getElementById(id).style.display='flex'}
function toast(m){var t=document.getElementById('toast');t.innerText=m;t.style.display='block';clearTimeout(t._h);t._h=setTimeout(function(){t.style.display='none'},2200)}
function pinv(){return document.getElementById('pin').value}
function rgb(a){return 'rgb('+a[0]+','+a[1]+','+a[2]+')'}
function bsel(base){var f=0.45+0.55*(0.5+0.5*Math.sin(Date.now()/2000*2*Math.PI));return [Math.round(base[0]*f),Math.round(base[1]*f),Math.round(base[2]*f)]}
function rr(c,x,y,w,h,r,fill){c.beginPath();c.moveTo(x+r,y);c.lineTo(x+w-r,y);c.quadraticCurveTo(x+w,y,x+w,y+r);c.lineTo(x+w,y+h-r);c.quadraticCurveTo(x+w,y+h,x+w-r,y+h);c.lineTo(x+r,y+h);c.quadraticCurveTo(x,y+h,x,y+h-r);c.lineTo(x,y+r);c.quadraticCurveTo(x,y,x+r,y);c.closePath();if(fill){c.fillStyle=fill;c.fill()}}
function txt(c,s,x,y,w,h,fs,col){c.fillStyle=col;c.font='bold '+fs+'px sans-serif';c.textAlign='center';c.textBaseline='middle';c.fillText(s,x+w/2,y+h/2)}
function fit(){
 var maxW=960;var avail=Math.max(240,window.innerWidth-24);var dispW=Math.min(avail,maxW);
 cv.width=960;cv.height=720;
 cv.style.width=Math.round(dispW)+'px';cv.style.height=Math.round(dispW*H/W)+'px';
 cv2.width=960;cv2.height=720;
 document.documentElement.style.setProperty('--uiw',Math.round(dispW)+'px');
}
window.addEventListener('resize',fit);
function authFail(){document.getElementById('askPin').value='';show('askOvl');}
function askOk(){var v=document.getElementById('askPin').value;document.getElementById('pin').value=v;localStorage.setItem('chgPin',v);hide('askOvl');toast('PIN已填入，请重试');pullWifiCred();}
function api(path,opt){return fetch(BASE+path,opt).then(function(r){if(r.status==401){authFail();throw new Error('PIN');}return r;});}
function requireHttp(){if(serialMode){toast('串口模式不支持该功能');return false;}return true;}
function setMode(m){mode=m;serialMode=(m==='serial');document.getElementById('mHttp').className=(m==='http')?'green':'gray';document.getElementById('mSer').className=(m==='serial')?'green':'gray';document.getElementById('httpBar').style.display=(m==='http')?'flex':'none';document.getElementById('wifiBar').style.display=(m==='http')?'flex':'none';if(m==='http'){st=null;pullTheme();}else{st=blankSt();}setTimeout(fit,30);toast(m==='http'?'HTTP模式':'串口模式(显示/改参数/CC-CP/开关机/波形)');}
function connect(){var v=document.getElementById('addr').value.trim().replace(/^https?:\/\//,'').replace(/\/$/,'');localStorage.setItem('chgAddr',v);BASE=v?('http://'+v):'';toast('目标: '+(BASE||'本机'));pullState();pullTheme();pullLimits();pullWifiCred();}
function pullLimits(){api('/settings').then(function(r){return r.json()}).then(function(d){if(d.maxV)limV=d.maxV;if(d.maxA)limA=d.maxA;if(d.maxP)limP=d.maxP;}).catch(function(){});}
function loadLocal(){var a=localStorage.getItem('chgAddr');if(a)document.getElementById('addr').value=a;var p=localStorage.getItem('chgPin');if(p)document.getElementById('pin').value=p;}
function saveWifi(){if(!requireHttp())return;var s=document.getElementById('ssid').value,p=document.getElementById('pw').value;if(!s){toast('请输入SSID');return;}
 api('/wifisave?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)+'&pin='+encodeURIComponent(pinv())).then(function(r){return r.text().then(function(t){toast(r.ok?('配网:'+t):('失败:'+t));if(r.ok)pullWifiCred();});}).catch(function(){});}
function pullWifiCred(){var p=pinv();if(!p)return;fetch(BASE+'/wifiget?pin='+encodeURIComponent(p)).then(function(r){return r.ok?r.json():null}).then(function(d){if(d){document.getElementById('ssid').value=d.ssid||'';document.getElementById('pw').value=d.pass||'';}}).catch(function(){});}
var REG=[
 {id:'setV',type:'num',name:'设定电压(V)',x:208,y:2,w:108,h:34,range:[1,100]},
 {id:'setA',type:'num',name:'设定电流(A)',x:208,y:39,w:108,h:34,range:[0.1,50]},
 {id:'trickle',type:'num',name:'停充电流(A)',x:208,y:76,w:108,h:34,range:[0,10]},
 {id:'pwrMax',type:'num',name:'功率保护(W)',x:208,y:113,w:108,h:37,range:[0,3000]},
 {id:'pr',type:'btn',name:'选择预设',x:162,y:154,w:74,h:26},
 {id:'cc',type:'btn',name:'切换 CC/CP',x:240,y:154,w:74,h:26},
 {id:'ap',type:'btn',name:'AP热点开关',x:162,y:183,w:74,h:26,confirm:true},
 {id:'clr',type:'btn',name:'清零 AH/WH',x:240,y:183,w:74,h:26,confirm:true},
 {id:'lock',type:'btn',name:'面板锁定',x:162,y:212,w:74,h:26},
 {id:'run',type:'btn',name:'运行/停止',x:240,y:212,w:74,h:26,confirm:true}
];
var COLREG=[
 {i:1,x:4,y:2,w:200,h:48},{i:2,x:4,y:52,w:200,h:48},{i:3,x:4,y:102,w:200,h:48},
 {i:4,x:208,y:2,w:108,h:34},{i:5,x:208,y:39,w:108,h:34},{i:6,x:208,y:76,w:108,h:34},{i:7,x:208,y:113,w:108,h:37},
 {i:8,x:6,y:154,w:74,h:26},{i:9,x:84,y:154,w:74,h:26},{i:12,x:162,y:154,w:74,h:26},{i:13,x:240,y:154,w:74,h:26},
 {i:10,x:6,y:183,w:74,h:26},{i:11,x:84,y:183,w:74,h:26},{i:16,x:240,y:183,w:74,h:26},
 {i:14,x:162,y:183,w:37,h:26},{i:15,x:199,y:183,w:37,h:26},
 {i:21,x:6,y:212,w:37,h:26},{i:22,x:43,y:212,w:37,h:26},
 {i:23,x:84,y:212,w:37,h:26},{i:24,x:121,y:212,w:37,h:26},
 {i:17,x:162,y:212,w:37,h:26},{i:18,x:199,y:212,w:37,h:26},
 {i:19,x:240,y:212,w:37,h:26},{i:20,x:277,y:212,w:37,h:26}
];
var TH_NAMES=['背景','电压V','电流A','功率W','设定V','设定A','停充A','功率保护','AH','WH','CNT','故障','预设','CC/CP','AP关','AP开','清零','锁定关','锁定开','RUN关','RUN开','485离线','485在线','WIFI离线','WIFI在线'];
function hitReg(c,e,arr,unit){var rc=c.getBoundingClientRect();var px=(e.clientX-rc.left)*(W/rc.width),py=(e.clientY-rc.top)*(H/rc.height);for(var i=0;i<arr.length;i++){var r=arr[i];if(px>=r.x&&px<r.x+r.w&&py>=r.y&&py<r.y+r.h)return r;}return null;}
cv.addEventListener('click',function(e){
 if(!st)return;var r=hitReg(cv,e,REG,W);if(!r)return;
 if(r.type==='num'){openNum(r);}
 else if(r.id==='pr'){if(serialMode){toast('串口模式仅支持:显示/改V·A/开关机/波形');return;}show('preOvl');}
 else{pendAction=r;document.getElementById('cfTitle').innerText=r.name;document.getElementById('cfMsg').innerText='确认执行该操作？';show('cfOvl');}
});
function openNum(r){
 var rng=r.range.slice();
 if(r.id==='setV')rng[1]=limV;else if(r.id==='setA')rng[1]=limA;else if(r.id==='trickle')rng[1]=limA;else if(r.id==='pwrMax')rng[1]=limP;
 var rf={id:r.id,type:r.type,name:r.name,range:rng};
 if(serialMode){curField=rf;document.getElementById('npTitle').innerText=r.name;document.getElementById('npVal').value=(r.id==='setV'?last.v.toFixed(1):r.id==='setA'?last.a.toFixed(1):r.id==='trickle'?last.tr.toFixed(1):last.pm.toFixed(0));show('npOvl');return;}
 api('/getparam?field='+r.id).then(function(x){return x.text()}).then(function(v){curField=rf;document.getElementById('npTitle').innerText=r.name;document.getElementById('npVal').value=v;show('npOvl');}).catch(function(){});
}
function nk(k){var el=document.getElementById('npVal');if(k==='B')el.value=el.value.slice(0,-1);else el.value+=k;}
function npConfirm(){
 if(!curField)return;var v=parseFloat(document.getElementById('npVal').value);
 if(isNaN(v)||v<curField.range[0]||v>curField.range[1]){toast('范围 '+curField.range[0]+' ~ '+curField.range[1]);return;}
 if(serialMode){
  if(curField.id==='setV'){last.v=v;if(last.mode===1&&last.v>0.1)last.a=Math.min(50,last.pm/last.v);if(last.run)serialSetVI(last.v,last.a);toast('已下发');}
  else if(curField.id==='setA'){if(last.mode===1){toast('CP模式电流自动计算');return;}last.a=v;last.pm=last.v*last.a;if(last.run)serialSetVI(last.v,last.a);toast('已下发');}
  else if(curField.id==='trickle'){last.tr=v;toast('已记录停充电流');}
  else if(curField.id==='pwrMax'){last.pm=v;if(last.mode===1&&last.v>0.1)last.a=Math.min(50,last.pm/last.v);if(last.run)serialSetVI(last.v,last.a);toast('已记录保护功率');}
  if(st){st.sv=last.v;st.sa=last.a;st.pm=last.pm;st.tr=last.tr;st.mode=last.mode;}
  hide('npOvl');return;
 }
 api('/setparam?field='+curField.id+'&val='+v+'&pin='+encodeURIComponent(pinv())).then(function(){toast('已下发');hide('npOvl');pullState();}).catch(function(){});
}
function cfYes(){
 if(!pendAction){hide('cfOvl');return;}
 var a=pendAction;pendAction=null;
 if(serialMode){
  if(a.id==='run'){last.run=!last.run;serialPower(last.run);if(last.run){serRunStart=Date.now();serOver=0;}toast(last.run?'开机':'关机');}
  else if(a.id==='cc'){last.mode=last.mode?0:1;serialApplyMode();}
  else toast('串口模式不支持该操作');
  hide('cfOvl');return;
 }
 api('/btn?id='+a.id+'&pin='+encodeURIComponent(pinv())).then(function(){toast('已执行');hide('cfOvl');pullState();}).catch(function(){hide('cfOvl');});
}
function serialApplyMode(){
 if(last.mode===1){if(last.v>0.1)last.a=Math.min(50,last.pm/last.v);}
 else{last.pm=last.v*last.a;}
 if(last.run)serialSetVI(last.v,last.a);
 if(st){st.sv=last.v;st.sa=last.a;st.pm=last.pm;st.tr=last.tr;st.mode=last.mode;}
 toast('模式:'+(last.mode===0?('CC 恒流 保护'+last.pm.toFixed(0)+'W'):('CP 恒功率 自动电流'+last.a.toFixed(1)+'A')));
}
function applyPre(n){if(!requireHttp())return;api('/preset?n='+n+'&pin='+encodeURIComponent(pinv())).then(function(){toast('预设'+n);hide('preOvl');hide('peOvl');pullState();}).catch(function(){});}
function openPresets(){if(!requireHttp())return;show('peOvl');api('/presets').then(function(r){return r.json()}).then(function(arr){
 var g=document.getElementById('peList');g.innerHTML='';
 arr.forEach(function(pa,i){
  var row=document.createElement('div');row.className='row';row.style.flexWrap='wrap';
  var lab=document.createElement('label');lab.innerText='P'+(i+1);row.appendChild(lab);
  var inpV=document.createElement('input');inpV.type='number';inpV.step='0.1';inpV.value=pa[0];inpV.id='peV'+i;row.appendChild(inpV);
  var inpA=document.createElement('input');inpA.type='number';inpA.step='0.1';inpA.value=pa[1];inpA.id='peA'+i;row.appendChild(inpA);
  var b1=document.createElement('button');b1.className='green';b1.innerText='应用';b1.onclick=(function(n){return function(){applyPre(n)}})(i+1);row.appendChild(b1);
  var b2=document.createElement('button');b2.className='gray';b2.innerText='保存';b2.onclick=(function(n){return function(){savePreset(n)}})(i);row.appendChild(b2);
  g.appendChild(row);
 });
 }).catch(function(){});}
function savePreset(i){var v=document.getElementById('peV'+i).value,a=document.getElementById('peA'+i).value;
 api('/preset_set?n='+(i+1)+'&v='+v+'&a='+a+'&pin='+encodeURIComponent(pinv())).then(function(r){return r.text().then(function(t){toast(r.ok?'已保存':'失败:'+t)})}).catch(function(){});}
function pullTheme(){api('/theme').then(function(r){return r.json()}).then(function(a){if(a&&a.length===25)theme=a.map(function(h){return [parseInt(h.substr(1,2),16),parseInt(h.substr(3,2),16),parseInt(h.substr(5,2),16)];});}).catch(function(){});}
function pullState(){if(serialMode)return;api('/state').then(function(r){return r.json()}).then(function(d){st=d;last.v=d.sv;last.a=d.sa;last.tr=d.tr;last.pm=d.pm;last.run=d.run;last.preset=d.preset;last.mode=d.mode;
 document.getElementById('st').innerHTML='输出 <b style="color:#fd0">'+d.v.toFixed(1)+'V '+d.a.toFixed(2)+'A '+d.w.toFixed(0)+'W</b> | '+d.st+' | 485:'+(d.rs?'<b style="color:#0f8">在线</b>':'<b style="color:#f44">离线</b>')+' | WiFi:'+(d.wf?'<b style="color:#4af">'+d.ip+'</b>':'<b style="color:#888">未连</b>')+' | RUN:'+(d.runActive?'<b style="color:#f44">运行</b>':(d.run?'<b style="color:#fa0">激活中</b>':'<b style="color:#0f8">停止</b>'))+(d.estop?' | <b style="color:#f00">急停</b>':'')+' | CNT:'+d.cnt+' WH:'+d.wh+(d.trip?' | TRIP:'+d.trip+'#'+d.tripN:'')+' | 复位:'+d.rst+' 485掉线:'+d.off;
 hist.v.push(d.v);hist.a.push(d.a);hist.w.push(d.w);if(hist.v.length>240){hist.v.shift();hist.a.shift();hist.w.shift();}
 drawWave();
 }).catch(function(){document.getElementById('st').innerText='无法连接 '+(BASE||'本机');});}
function cur(i){return theme[i]||DEFTHEME[i]}
function drawScreen(c){
 if(!st)return;var d=st;c.fillStyle=rgb(cur(0));c.fillRect(0,0,W,H);
 var lh=48,gap=2,cols=[rgb(cur(1)),rgb(cur(2)),rgb(cur(3))],lv=[d.v.toFixed(1)+'V',d.a.toFixed(2)+'A',d.w.toFixed(0)+'W'];
 for(var i=0;i<3;i++){rr(c,4,2+i*(lh+gap),200,lh,5,cols[i]);txt(c,lv[i],4,2+i*(lh+gap),200,lh,30,'#fff');}
 var rh=34,rg=4,rc=[rgb(cur(4)),rgb(cur(5)),rgb(cur(6)),rgb(cur(7))],rv=[d.sv.toFixed(1)+'V',d.sa.toFixed(1)+'A',d.tr.toFixed(1)+'A',d.pm.toFixed(0)+'W'];
 for(var i=0;i<4;i++){rr(c,208,2+i*(rh+rg),108,rh,5,rc[i]);txt(c,rv[i],208,2+i*(rh+rg),108,rh,20,'#fff');}
 var bw=74,bgp=4,bh=26,brg=3,bx=(320-(bw*4+bgp*3))/2,c0=bx,c1=bx+bw+bgp,c2=c1+bw+bgp,c3=c2+bw+bgp;
 function cell1(x,y,col,t,fc){rr(c,x,y,bw,bh,3,rgb(col));c.fillStyle=fc||'#fff';c.textAlign='center';c.textBaseline='middle';c.font='bold 16px sans-serif';c.fillText(t,x+bw/2,y+bh/2);}
 function cell2(x,y,col,v,lab,vc){rr(c,x,y,bw,bh,3,rgb(col));c.textAlign='center';c.textBaseline='middle';c.fillStyle=vc||'#fff';c.font='bold 16px sans-serif';c.fillText(v,x+bw/2,y+9);c.font='bold 9px sans-serif';c.fillText(lab,x+bw/2,y+20);}
 var by=154;
 cell2(c0,by,cur(8),d.ah.toFixed(0),'AH','#000');cell2(c1,by,cur(9),d.wh.toFixed(0),'WH','#fff');
 cell1(c2,by,cur(12),'PRE'+d.preset);cell1(c3,by,cur(13),d.mode===0?'CC':'CP');by+=bh+brg;
 cell1(c0,by,cur(10),String(d.cnt),'#fff');cell1(c1,by,cur(11),d.st);
 cell1(c2,by,d.ap?cur(15):cur(14),'AP');cell1(c3,by,cur(16),'CLR');by+=bh+brg;
 cell1(c0,by,d.rs?bsel(cur(22)):cur(21),'485');cell1(c1,by,d.wf?bsel(cur(24)):cur(23),'WIFI');
 cell1(c2,by,d.lock?bsel(cur(18)):cur(17),'LOCK');cell1(c3,by,d.run?bsel(cur(20)):cur(19),d.run?'RUN':'STOP');
 c.fillStyle='#8c91a0';c.font='10px sans-serif';c.textAlign='left';c.textBaseline='alphabetic';
 c.fillText('ADR:'+d.addr+' '+(d.ap?'AP:ON':'AP:OFF')+' '+((d.wf&&d.ip)?d.ip:d.st)+' TRIP:'+d.trip+'#'+d.tripN,6,235);
 if(d.estop){c.fillStyle='rgba(160,0,0,.85)';c.fillRect(0,0,W,H);c.fillStyle='#fff';c.textAlign='center';c.font='bold 40px sans-serif';c.fillText('E-STOP',W/2,H/2-8);}
}
(function loop(){if(cv.width){ctx.save();ctx.scale(cv.width/W,cv.height/H);drawScreen(ctx);ctx.restore();}requestAnimationFrame(loop);})();
function niceMax(m){if(!(m>0))return 1;var e=Math.pow(10,Math.floor(Math.log10(m)));var f=m/e;var nf=f<=1?1:f<=1.5?1.5:f<=2?2:f<=3?3:f<=5?5:f<=7.5?7.5:10;return nf*e;}
function fmt(v){if(v>=100)return v.toFixed(0);if(v>=10)return v.toFixed(1);return v.toFixed(2);}
function drawWave(){
 var wc=document.getElementById('wave');if(!wc)return;var w=wc.clientWidth||600,h=wc.clientHeight||200;if(wc.width!==w)wc.width=w;if(wc.height!==h)wc.height=h;
 var x=wc.getContext('2d'),L=46,R=48,T=12,B=20,pw=w-L-R,ph=h-T-B;if(pw<10||ph<10)return;
 x.fillStyle='#111';x.fillRect(0,0,w,h);
 var n=hist.v.length;
 var vmax=niceMax(Math.max(0.001,Math.max.apply(null,hist.v.concat([0]))));
 var amax=niceMax(Math.max(0.001,Math.max.apply(null,hist.a.concat([0]))));
 x.lineWidth=1;x.font='10px sans-serif';
 for(var i=0;i<=4;i++){var yy=T+ph*i/4;x.strokeStyle='#242838';x.beginPath();x.moveTo(L,yy);x.lineTo(L+pw,yy);x.stroke();
  x.fillStyle='#fd0';x.textAlign='right';x.textBaseline='middle';x.fillText(fmt(vmax*(1-i/4)),L-4,yy);
  x.fillStyle='#0f8';x.textAlign='left';x.fillText(fmt(amax*(1-i/4)),L+pw+4,yy);}
 x.fillStyle='#fd0';x.textAlign='left';x.fillText('V',L-4,T-4);
 x.fillStyle='#0f8';x.textAlign='right';x.fillText('A',L+pw+44,T-4);
 var step=Math.max(1,Math.round(n/6));x.textAlign='center';x.textBaseline='top';
 for(var i=0;i<n;i+=step){var xx=L+pw*(n<=1?0:i/(n-1));x.strokeStyle='#1a1e2b';x.beginPath();x.moveTo(xx,T);x.lineTo(xx,T+ph);x.stroke();x.fillStyle='#8c91a0';x.fillText('-'+Math.round(n-1-i)+'s',xx,T+ph+3);}
 function line(a,own,col){if(!a.length)return;x.strokeStyle=col;x.lineWidth=1.6;x.beginPath();for(var i=0;i<a.length;i++){var px=L+pw*(a.length<=1?0:i/(a.length-1));var py=T+ph-(a[i]/own)*ph*0.98;i?x.lineTo(px,py):x.moveTo(px,py);}x.stroke();}
 line(hist.v,vmax,'#fd0');line(hist.a,amax,'#0f8');
 x.strokeStyle='#3a3f52';x.lineWidth=1;x.strokeRect(L,T,pw,ph);
}
// ===== 配色 =====
function openColor(){if(!requireHttp())return;show('coOvl');drawColorPreview();}
function drawColorPreview(){
 ctx2.save();ctx2.scale(cv2.width/W,cv2.height/H);ctx2.fillStyle=rgb(cur(0));ctx2.fillRect(0,0,W,H);
 for(var i=0;i<3;i++)rr(ctx2,4,2+i*50,200,48,5,rgb(cur(1+i)));
 for(var i=0;i<4;i++){rr(ctx2,208,2+i*(34+4),108,34,5,rgb(cur(4+i)));}
 var bw=74,bgp=4,bh=26,brg=3,bx=(320-(bw*4+bgp*3))/2,c0=bx,c1=bx+bw+bgp,c2=c1+bw+bgp,c3=c2+bw+bgp,by=154;
 rr(ctx2,c0,by,bw,bh,3,rgb(cur(8)));rr(ctx2,c1,by,bw,bh,3,rgb(cur(9)));rr(ctx2,c2,by,bw,bh,3,rgb(cur(12)));rr(ctx2,c3,by,bw,bh,3,rgb(cur(13)));
 by+=bh+brg;rr(ctx2,c0,by,bw,bh,3,rgb(cur(10)));rr(ctx2,c1,by,bw,bh,3,rgb(cur(11)));rr(ctx2,c3,by,bw,bh,3,rgb(cur(16)));
 // AP 分半
 rr(ctx2,c2,by,37,bh,0,rgb(cur(14)));rr(ctx2,c2+37,by,37,bh,3,rgb(cur(15)));
 by+=bh+brg;
 rr(ctx2,c0,by,37,bh,0,rgb(cur(21)));rr(ctx2,c0+37,by,37,bh,3,rgb(cur(22)));
 rr(ctx2,c1,by,37,bh,0,rgb(cur(23)));rr(ctx2,c1+37,by,37,bh,3,rgb(cur(24)));
 rr(ctx2,c2,by,37,bh,0,rgb(cur(17)));rr(ctx2,c2+37,by,37,bh,3,rgb(cur(18)));
 rr(ctx2,c3,by,37,bh,0,rgb(cur(19)));rr(ctx2,c3+37,by,37,bh,3,rgb(cur(20)));
 ctx2.restore();
}
cv2.addEventListener('click',function(e){var r=hitReg(cv2,e,COLREG,W);if(!r)return;coSel=r.i;openPick();});
function openPick(){var c=cur(coSel);document.getElementById('pickTitle').innerText='调色 - '+TH_NAMES[coSel];document.getElementById('pkR').value=c[0];document.getElementById('pkG').value=c[1];document.getElementById('pkB').value=c[2];pickSync();show('pickOvl');}
function pickSync(){var r=+document.getElementById('pkR').value,g=+document.getElementById('pkG').value,b=+document.getElementById('pkB').value;var hx='#'+[r,g,b].map(function(v){var s=v.toString(16);return s.length<2?'0'+s:s}).join('');document.getElementById('pkRv').innerText=r;document.getElementById('pkGv').innerText=g;document.getElementById('pkBv').innerText=b;document.getElementById('pkHex').value=hx.toUpperCase();document.getElementById('pickSw').style.background='rgb('+r+','+g+','+b+')';}
['pkR','pkG','pkB'].forEach(function(id){document.getElementById(id).addEventListener('input',pickSync);});
document.getElementById('pkHex').addEventListener('change',function(e){var h=e.target.value.replace('#','');if(/^[0-9a-fA-F]{6}$/.test(h)){document.getElementById('pkR').value=parseInt(h.substr(0,2),16);document.getElementById('pkG').value=parseInt(h.substr(2,2),16);document.getElementById('pkB').value=parseInt(h.substr(4,2),16);pickSync();}});
function pickOk(){theme[coSel]=[+document.getElementById('pkR').value,+document.getElementById('pkG').value,+document.getElementById('pkB').value];hide('pickOvl');drawColorPreview();toast('已修改，点“保存配色”写入设备');}
function colorSave(){if(!theme||theme.length!==25){toast('配色数据异常');return;}
 var c=theme.map(function(a){return [a[0],a[1],a[2]].map(function(v){var s=v.toString(16);return s.length<2?'0'+s:s}).join('')}).join(',');
 api('/theme?pin='+encodeURIComponent(pinv()),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'c='+c}).then(function(r){return r.text().then(function(t){toast(r.ok?'配色已保存':'保存失败:'+t);});}).catch(function(){});}
function colorReset(){if(!requireHttp())return;api('/theme/reset?pin='+encodeURIComponent(pinv())).then(function(){toast('已恢复默认');pullTheme();setTimeout(drawColorPreview,300);}).catch(function(){});}
// ===== 设置/PIN/OTA =====
function openCfg(){if(!requireHttp())return;show('cfGOvl');if(st)document.getElementById('cfgAddr').value=st.addr;document.getElementById('scanRes').innerText='';api('/settings').then(function(r){return r.json()}).then(function(d){document.getElementById('cfgHb').value=d.hb;document.getElementById('cfgPort').value=d.port;document.getElementById('cfgVol').value=d.vol;document.getElementById('cfgMute').value=d.mute?'1':'0';document.getElementById('cfgEstop').value=d.estop;document.getElementById('cfgMaxV').value=d.maxV;document.getElementById('cfgMaxA').value=d.maxA;document.getElementById('cfgMaxP').value=d.maxP;document.getElementById('cfgRampMs').value=d.rampMs;document.getElementById('cfgRampSt').value=d.rampSt;if(d.addr!==undefined)document.getElementById('cfgAddr').value=d.addr;limV=d.maxV;limA=d.maxA;limP=d.maxP;}).catch(function(){});}
function doScan(){if(!requireHttp())return;document.getElementById('scanRes').innerText='扫描中...';api('/scan?pin='+encodeURIComponent(pinv())).then(function(){pollScan(0);}).catch(function(){});}
function pollScan(k){if(k>60){document.getElementById('scanRes').innerText='未找到';return;}api('/state').then(function(r){return r.json()}).then(function(d){if(d.scanDone){document.getElementById('scanRes').innerText=d.scanFound?('找到地址 '+d.scan):'未找到';if(d.scanFound)document.getElementById('cfgAddr').value=d.scan;pullState();}else setTimeout(function(){pollScan(k+1)},500);}).catch(function(){});}
function clrAll(){if(!requireHttp())return;api('/btn?id=clrall&pin='+encodeURIComponent(pinv())).then(function(){toast('已清零 AH/WH/CNT/TRIP');pullState();}).catch(function(){});}
function lcdInit(){if(!requireHttp())return;api('/btn?id=lcdinit&pin='+encodeURIComponent(pinv())).then(function(){toast('已发送屏幕重初始化');}).catch(function(){});}
function cfgSave(){var body='hb='+document.getElementById('cfgHb').value+'&port='+document.getElementById('cfgPort').value+'&vol='+document.getElementById('cfgVol').value+'&mute='+document.getElementById('cfgMute').value+'&estop='+document.getElementById('cfgEstop').value+'&maxV='+document.getElementById('cfgMaxV').value+'&maxA='+document.getElementById('cfgMaxA').value+'&maxP='+document.getElementById('cfgMaxP').value+'&rampMs='+document.getElementById('cfgRampMs').value+'&rampSt='+document.getElementById('cfgRampSt').value+'&addr='+document.getElementById('cfgAddr').value+'&pin='+encodeURIComponent(pinv());limV=parseFloat(document.getElementById('cfgMaxV').value)||limV;limA=parseFloat(document.getElementById('cfgMaxA').value)||limA;limP=parseFloat(document.getElementById('cfgMaxP').value)||limP;api('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.text().then(function(t){toast(r.ok?'设置已保存':'失败:'+t);hide('cfGOvl');});}).catch(function(){});}
function openPin(){if(!requireHttp())return;show('pnOvl');}
function pinSave(){var o=document.getElementById('pnOld').value,n=document.getElementById('pnNew').value;api('/pinset?old='+encodeURIComponent(o)+'&new='+encodeURIComponent(n)).then(function(r){return r.text().then(function(t){if(r.ok){toast('PIN已修改');document.getElementById('pin').value=n;localStorage.setItem('chgPin',n);hide('pnOvl');}else toast('失败:'+t);});}).catch(function(){});}
function doOTA(){if(!requireHttp())return;document.getElementById('otaFile').click();}
document.getElementById('otaFile').addEventListener('change',function(e){var f=e.target.files[0];if(!f)return;var fd=new FormData();fd.append('file',f);api('/update?pin='+encodeURIComponent(pinv()),{method:'POST',body:fd}).then(function(r){toast(r.ok?'OTA成功,重启中':'OTA失败');}).catch(function(){});e.target.value='';});
function doReboot(){if(!requireHttp())return;if(!confirm('确认重启开发板?'))return;api('/reboot?pin='+encodeURIComponent(pinv())).then(function(){toast('重启指令已发送');}).catch(function(){});}
// ===== 串口 (Web Serial) =====
var port=null,reading=false,rxBuf=[],serRunStart=0,serOver=0;
function crc16(b){var c=0xFFFF;for(var i=0;i<b.length;i++){c^=b[i];for(var j=0;j<8;j++){c=(c&1)?((c>>1)^0xA001):(c>>1);}}return c;}
async function serialConnect(){
 if(!('serial' in navigator)){toast('此浏览器不支持串口(需Chrome/Edge桌面版)');return;}
 try{port=await navigator.serial.requestPort();await port.open({baudRate:9600,dataBits:8,stopBits:1,parity:'none',flowControl:'none'});startReader();setMode('serial');document.getElementById('serialSt').innerText='串口已连接 9600 8N1';toast('串口已连接');}
 catch(e){toast('串口打开失败:'+e.message);}
}
async function startReader(){if(reading)return;reading=true;
 while(port&&port.readable){var rd=port.readable.getReader();
  try{while(true){var r=await rd.read();if(r.done)break;if(r.value){for(var i=0;i<r.value.length;i++)rxBuf.push(r.value[i]);if(rxBuf.length>4096)rxBuf.splice(0,rxBuf.length-2048);}}}
  catch(e){break}finally{try{rd.releaseLock()}catch(e){}}}
 reading=false;}
function waitBytes(n,to){return new Promise(function(res){var t0=Date.now();(function chk(){if(rxBuf.length>=n){res(true);return;}if(Date.now()-t0>to){res(false);return;}setTimeout(chk,10);})();});}
async function serialXfer(req,nResp,to){if(!port)return null;var f=new Uint8Array(req.length+2);f.set(req);var c=crc16(req);f[req.length]=c&0xFF;f[req.length+1]=c>>8;
 rxBuf=[];var w=port.writable.getWriter();await w.write(f);w.releaseLock();
 var ok=await waitBytes(nResp,to||400);if(!ok)return null;var r=rxBuf.slice(0,nResp);rxBuf=[];return r;}
async function serialSetVI(v,a){var vr=Math.round(v*10),ar=Math.round(a*100);await serialXfer([0x00,0x0F,0x00,0x01,0x00,0x02,(vr>>8)&0xFF,vr&0xFF,(ar>>8)&0xFF,ar&0xFF],8,400);}
async function serialPower(on){var val=on?0x01FF:0x0100;await serialXfer([0x00,0x06,0x00,0x00,(val>>8)&0xFF,val&0xFF],8,400);}
function blankSt(){return {v:0,a:0,w:0,sv:last.v,sa:last.a,tr:last.tr,pm:last.pm,mode:last.mode,preset:last.preset,run:last.run,runActive:last.run,estop:false,lock:false,ap:false,rs:false,wf:false,ip:'',st:'--',trip:'',tripN:0,addr:0,cnt:0,wh:0,ah:0,hb:0,port:0,mute:0,vol:0};}
async function serialPoll(){
 if(!serialMode||!port)return;
 var r=await serialXfer([0x00,0x03,0x00,0x01,0x00,0x03],11,400);
 var v,a,sName;
 if(!r||r[1]!==0x03){if(!st)st=blankSt();st.rs=false;st.st='OFFLINE';
  document.getElementById('st').innerHTML='[串口] 无应答（检查485接线/波特率/地址）';drawWave();return;}
 var s=(r[7]<<8)|r[8];v=((r[3]<<8)|r[4])/10;a=(r[5]|(r[6]<<8))/100;
 sName=(s&0x80)?'OVP':(s&0x40)?'OCP':(s&0x20)?'SHORT':(s&0x10)?'OTP':(['IDLE','BOOT','CHG','FULL','LIM'][s&0x0F]||'--');
 st=blankSt();st.v=v;st.a=a;st.w=v*a;st.rs=true;st.st=sName;st.run=last.run;st.runActive=last.run;
 if(last.run&&serRunStart){
  serOver=(v*a>last.pm*1.10)?serOver+1:0;
  if(serOver>=2){serialPower(false);last.run=false;st.run=false;st.runActive=false;serRunStart=0;toast('功率保护关断');}
  else if(Date.now()-serRunStart>10000&&last.tr>0.01&&a<last.tr){serialPower(false);last.run=false;st.run=false;st.runActive=false;serRunStart=0;toast('停充电流关断');}
 }
 hist.v.push(v);hist.a.push(a);hist.w.push(v*a);if(hist.v.length>240){hist.v.shift();hist.a.shift();hist.w.shift();}
 document.getElementById('st').innerHTML='[串口] 输出 <b style="color:#fd0">'+v.toFixed(1)+'V '+a.toFixed(2)+'A '+(v*a).toFixed(0)+'W</b> | '+sName+' | 设定 '+last.v.toFixed(1)+'V/'+last.a.toFixed(1)+'A | '+(last.run?'<b style="color:#f44">运行</b>':'<b style="color:#0f8">停止</b>');
 drawWave();
}
fit();loadLocal();
if(localStorage.getItem('chgAddr'))connect();
document.getElementById('pin').addEventListener('change',function(){localStorage.setItem('chgPin',pinv());pullWifiCred();});
setInterval(function(){if(!serialMode)pullState();else serialPoll();},1000);pullState();pullTheme();pullLimits();pullWifiCred();
</script></body></html>
)HTMLPAGE";

// ================= Setup / Loop =================
void builtinLedOff() {
#if defined(RGB_BUILTIN)
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif
  neopixelWrite(48, 0, 0, 0);
  neopixelWrite(38, 0, 0, 0);
}
void setup() {
  Serial.begin(115200);
  delay(300);
  builtinLedOff();
  pinMode(PIN_EC11_A, INPUT_PULLUP);
  pinMode(PIN_EC11_B, INPUT_PULLUP);
  pinMode(PIN_EC11_SW, INPUT_PULLUP);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, BUZZ_ACTIVE_LOW ? HIGH : LOW);
  attachInterrupt(digitalPinToInterrupt(PIN_EC11_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_EC11_B), encoderISR, CHANGE);
  encState = (((GPIO.in >> PIN_EC11_A) & 1) << 1) | ((GPIO.in >> PIN_EC11_B) & 1);
  buzzInit();
  lcdInit();
  sp.setPsram(true);
  sp.createSprite(320, 240);
  sp.setColorDepth(16);
  sp.setSwapBytes(true);
  initColors();
  nvsLoad();
  wifiLoadCred();
  applyModeCalc();
  WiFi.mode(WIFI_STA);
  if (wifiSSID.length()) startSTA();
  webSetup();
  xTaskCreatePinnedToCore(commTask, "comm", 8192, NULL, 1, NULL, 0);
  Serial.printf("[BOOT] PSRAM=%u Flash=%u Sketch=%u\n", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFlashChipSize(), (unsigned)ESP.getSketchSize());
  Serial.println("=== Charger v3.1 ===");
}
void loop() {
  if (lcdReinitReq) { lcdReinitReq = false; lcdReinitAt = millis() + 500; }   // 等瞬态过去再重初始化
  if (lcdReinitAt && (int32_t)(millis() - lcdReinitAt) >= 0) { lcdReinitAt = 0; lcdReinit(); }
  if (addrDirty) {
    addrDirty = false;
    nvsSaveCfg();
  }
  handleInput();
  wifiPoll();
  bzUpdate();
  if (chg.estop) drawEstop();
  else if (pagePreset) drawPresetPage();
  else drawMain();
  sp.pushSprite(0, 0);
  delay(20);
}