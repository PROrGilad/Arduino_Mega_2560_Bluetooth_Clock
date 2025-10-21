/*
  ===== MCUFRIEND 2.8" — Bluetooth Seal HUD Clock (Flicker-free TEMP/HUM + Solid Seconds) =====
  - Time over Bluetooth (HC-05) via Serial1
  - Accepts "HH:MM:SS" or "YYYY-MM-DD HH:MM:SS" (newline-terminated)
  - Ocean + seal background, pulsing colons, seconds bar, two-line date
  - DHT11 TEMP/HUM under "BT SYNC" (Arduino Mega2560 pin 48)
  - SD chip select lines forced HIGH to keep SD off the SPI bus
  - Hard-refresh for HOUR + MINUTE + SECOND digits (clean, reliable updates)
  - TEMP/HUM updates are flicker-free (opaque per-glyph redraw + fixed width + hysteresis)
*/

#include <MCUFRIEND_kbv.h>
#include <Adafruit_GFX.h>
#include <string.h>
#include <math.h>        // cosf, fabs
#include <stdlib.h>      // dtostrf
#include "DHT.h"         // DHT11 sensor

// ---------------- Forward declarations ----------------
struct ClockTime;
uint16_t oceanColorAtY(int y);
void drawSealBackground();
void drawTitleStrip();
void setupLayout();
void drawSecondsBarFrameOnce();
void drawSecondsBarFill(uint8_t sec);
void drawDateLine(const ClockTime &c);
void drawTimeDigits(const ClockTime &c);
void animateScanline();
void updateColonPulse(unsigned long ms);
void drawEnvLine(const char* s);
void updateEnv();
void forceRedrawAll();

// ---------------- Bluetooth I/O ----------------
#define BT_SERIAL Serial1

// ---------------- DHT11 (moved to Mega2560 pin 48) ----------------
#define DHTPIN   48
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// ---------------- Config toggles ----------------
#define ENABLE_SCANLINE         0   // subtle shimmer line
#define HARD_REFRESH_MINUTES    1   // hard-refresh minute digits
#define HARD_REFRESH_SECONDS    1   // hard-refresh second digits

// ---------------- Clock state ----------------
struct ClockTime {
  int year = 0, month = 0, day = 0; // optional (0 when unknown)
  int hour = 0, minute = 0, second = 0;
  bool hasDate = false;
  bool isSet = false;
};
ClockTime ct;

// ---------------- Input buffer ----------------
char str_in[128];
int in_i = 0;

// ---------------- Parse helpers ----------------
static bool isDigitN(char c) { return (c >= '0' && c <= '9'); }
static int to2(const char *p) { if (!isDigitN(p[0]) || !isDigitN(p[1])) return -1; return (p[0]-'0')*10 + (p[1]-'0'); }
static int to4(const char *p) { for (int k=0; k<4; ++k) if (!isDigitN(p[k])) return -1; return (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0'); }

static bool parseTime(const char* s, ClockTime &out) {
  while (*s == ' ' || *s == '\t') s++;
  ClockTime temp;
  int len = strlen(s);

  if (len >= 8 && isDigitN(s[0]) && isDigitN(s[1]) && s[2]==':' && isDigitN(s[3]) && isDigitN(s[4]) && s[5]==':' && isDigitN(s[6]) && isDigitN(s[7])) {
    temp.hour = to2(s+0);
    temp.minute = to2(s+3);
    temp.second = to2(s+6);
    temp.hasDate = false;
    if (temp.hour<0 || temp.hour>23 || temp.minute<0 || temp.minute>59 || temp.second<0 || temp.second>59) return false;
  }
  else if (len >= 19 && isDigitN(s[0])) {
    if (!(s[4]=='-' && s[7]=='-' && s[10]==' ' && s[13]==':' && s[16]==':')) return false;
    temp.year  = to4(s+0);
    temp.month = to2(s+5);
    temp.day   = to2(s+8);
    temp.hour  = to2(s+11);
    temp.minute= to2(s+14);
    temp.second= to2(s+17);
    temp.hasDate = (temp.year>0);
    if (temp.year<1970 || temp.month<1 || temp.month>12 || temp.day<1 || temp.day>31) return false;
    if (temp.hour<0 || temp.hour>23 || temp.minute<0 || temp.minute>59 || temp.second<0 || temp.second>59) return false;
  } else {
    return false;
  }

  temp.isSet = true;
  out = temp;
  return true;
}

static bool isLeap(int y) { return ( (y%4==0 && y%100!=0) || (y%400==0) ); }
static int daysInMonth(int y, int m) {
  static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (m==2) return isLeap(y)?29:28;
  return d[m-1];
}
static void tickOneSecond(ClockTime &t) {
  t.second++;
  if (t.second >= 60) { t.second = 0; t.minute++; }
  if (t.minute >= 60) { t.minute = 0; t.hour++; }
  if (t.hour >= 24)   {
    t.hour = 0;
    if (t.hasDate && t.year >= 1970) {
      t.day++;
      int dim = daysInMonth(t.year, t.month);
      if (t.day > dim) { t.day = 1; t.month++; if (t.month>12) { t.month=1; t.year++; } }
    }
  }
}

// ---------------- TFT + HUD Look ----------------
MCUFRIEND_kbv tft;
uint16_t W, H;

// ---------- Colors ----------
const uint16_t COL_BG      = 0x0000; // black (used minimally)
const uint16_t COL_TEXT    = 0xFFFF; // white
const uint16_t COL_BAR     = 0x05BF; // cyan-ish
const uint16_t COL_GLOW    = 0x07FF; // neon cyan (digit on)
const uint16_t COL_GLOW2   = 0x07E0; // neon green
const uint16_t COL_GLOW_DIM= 0x025F; // dim cyan (colon min)

// Ocean palette
const uint16_t OCEAN_TOP   = 0x0255;
const uint16_t OCEAN_MID   = 0x039B;
const uint16_t OCEAN_BOT   = 0x043F;

// Seal palette
const uint16_t SEAL_DARK   = 0x632C;
const uint16_t SEAL_MID    = 0x8410;
const uint16_t SEAL_LIGHT  = 0xAD55;
const uint16_t SEAL_EYE    = 0x0000;
const uint16_t SEAL_WHISK  = 0xFFFF;

// ---------- Seven-seg (VARIABLE for auto-fit) ----------
int SEG_THICK = 5;
int SEG_LEN   = 28;
int SEG_HGAP  = 7;
int SEG_VLEN  = 28;

// ---------- Layout ----------
int DIGIT_Y;
int DIG_X[6];
int COLON_X[2];
int COLON_Y;
int COLON_W = 8;
int E_SP    = 2;   // tight spacing

// ---------- Segment indices ----------
enum { SEG_A=0, SEG_B=1, SEG_C=2, SEG_D=3, SEG_E=4, SEG_F=5, SEG_G=6 };

// ---------- Segment masks ----------
const uint8_t DIGIT_MASK[10] = {
  /*0*/ 0b0111111, /*1*/ 0b0000110, /*2*/ 0b1011011, /*3*/ 0b1001111, /*4*/ 0b1100110,
  /*5*/ 0b1101101, /*6*/ 0b1111101, /*7*/ 0b0000111, /*8*/ 0b1111111, /*9*/ 0b1101111
};

// ---------- Caches ----------
char prevTimeStr[9]  = "??:??:??";
char prevDateStr[16] = "";
uint8_t prevDigitMask[6] = {0,0,0,0,0,0};  // delta segment drawing

// ---------- Date helpers ----------
int weekdayIndex(int y,int m,int d){static int t[]={0,3,2,5,0,3,5,1,4,6,2,4};if(m<3)y-=1;return(y+y/4-y/100+y/400+t[m-1]+d)%7;}
const char* WEEKDAY_NAME[7]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
const char* MONTH_NAME[12]={"January","February","March","April","May","June","July","August","September","October","November","December"};

// ---------- Ocean gradient ----------
int oceanMidY() { return (int)(H * 2 / 5); }

uint16_t lerp565(uint16_t c1, uint16_t c2, uint8_t t){
  uint16_t r1=(c1>>11)&0x1F, g1=(c1>>5)&0x3F, b1=c1&0x1F;
  uint16_t r2=(c2>>11)&0x1F, g2=(c2>>5)&0x3F, b2=c2&0x1F;
  uint16_t r = ( (r1*(255-t) + r2*t) / 255 );
  uint16_t g = ( (g1*(255-t) + g2*t) / 255 );
  uint16_t b = ( (b1*(255-t) + b2*t) / 255 );
  return (r<<11) | (g<<5) | b;
}

uint16_t oceanColorAtY(int y) {
  int midY = oceanMidY();
  if (y < 0) y = 0;
  if (y >= (int)H) y = H - 1;
  uint8_t t;
  if (y < midY) {
    t = (uint32_t)y * 255 / (midY ? midY : 1);
    return lerp565(OCEAN_TOP, OCEAN_MID, t);
  } else {
    t = (uint32_t)(y - midY) * 255 / ((H - midY) ? (H - midY) : 1);
    return lerp565(OCEAN_MID, OCEAN_BOT, t);
  }
}

void fillRectWithOceanBG(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > (int)H) h = H - y;
  for (int yy = y; yy < y + h; ++yy) {
    uint16_t col = oceanColorAtY(yy);
    tft.drawFastHLine(x, yy, w, col);
  }
}

// ---------- Auto-size ----------
int totalRowWidth(){
  int dcellW = SEG_LEN + 2*SEG_HGAP;
  return 6*dcellW + 2*COLON_W + 7*E_SP;
}
void autoSizeToFit(){
  const int MARGIN=6,MIN_SP=0,MIN_COLON=4,MIN_THICK=4,MIN_LEN=16,MIN_VLEN=16;
  for(int g=0;g<200;++g){
    int need=totalRowWidth();
    if(need<=W-2*MARGIN)break;
    if(E_SP>MIN_SP){E_SP--;continue;}
    if(COLON_W>MIN_COLON){COLON_W--;continue;}
    if(SEG_LEN>MIN_LEN){SEG_LEN--;continue;}
    if(SEG_VLEN>MIN_VLEN){SEG_VLEN--;continue;}
    if(SEG_THICK>MIN_THICK){SEG_THICK--;continue;}
    break;
  }
}

// ---------- Background (Ocean + Seal) ----------
void drawOceanGradient() {
  int midY = oceanMidY();
  for (int y = 0; y < midY; ++y)
    tft.drawFastHLine(0, y, W, lerp565(OCEAN_TOP, OCEAN_MID, (uint32_t)y * 255 / (midY ? midY : 1)));
  for (int y = midY; y < (int)H; ++y)
    tft.drawFastHLine(0, y, W, lerp565(OCEAN_MID, OCEAN_BOT, (uint32_t)(y - midY) * 255 / ((H - midY) ? (H - midY) : 1)));
}
void ftri(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){ tft.fillTriangle(x0,y0,x1,y1,x2,y2,c); }
void drawSeal(int cx, int cy, int scale) {
  int br = 28*scale/10;
  tft.fillCircle(cx, cy, br+6, SEAL_DARK);
  tft.fillCircle(cx+8*scale/10, cy+2*scale/10, br+4, SEAL_DARK);
  tft.fillCircle(cx-12*scale/10, cy+2*scale/10, br, SEAL_DARK);
  tft.fillCircle(cx-2*scale/10, cy+4*scale/10, br-8, SEAL_LIGHT);
  int hr = 14*scale/10; int hx = cx + 24*scale/10, hy = cy - 10*scale/10;
  tft.fillCircle(hx, hy, hr, SEAL_MID);
  tft.fillCircle(hx + hr/3, hy + hr/3, 2*scale/10 + 1, SEAL_EYE);
  tft.fillCircle(hx - hr/3, hy - hr/4, 2, SEAL_EYE);
  tft.fillCircle(hx + hr/6, hy - hr/3, 2, SEAL_EYE);
  for (int i=-1;i<=1;i++){
    tft.drawLine(hx - 2, hy + 2 + 3*i, hx - 10 - 6*i, hy + 2 + 3*i, SEAL_WHISK);
    tft.drawLine(hx + 2, hy + 2 + 3*i, hx + 10 + 6*i, hy + 2 + 3*i, SEAL_WHISK);
  }
  ftri(cx-6*scale/10, cy+br-8, cx-20*scale/10, cy+br+6, cx-2*scale/10, cy+br+6, SEAL_MID);
  ftri(cx+10*scale/10, cy+br-6, cx+22*scale/10, cy+br+6, cx+2*scale/10, cy+br+6, SEAL_MID);
  int tx = cx - br - 10*scale/10, ty = cy + 2*scale/10;
  ftri(tx, ty, tx-12*scale/10, ty-8*scale/10, tx-12*scale/10, ty+8*scale/10, SEAL_MID);
  tft.drawCircle(cx, cy, br+7, SEAL_MID);
}
void drawSealBackground() {
  drawOceanGradient();
  int baseY = H*3/4;
  tft.fillRoundRect(W/4, baseY, W/2, 12, 6, 0xC618);
  tft.drawRoundRect(W/4, baseY, W/2, 12, 6, 0x8410);
  drawSeal(W/2 - 10, baseY - 18, 12);
}

// ---------- Seven-seg helpers ----------
void drawHSeg(int x,int y,int len,uint16_t c){ tft.fillRoundRect(x,y,len,SEG_THICK,SEG_THICK/2,c); }
void drawVSeg(int x,int y,int len,uint16_t c){ tft.fillRoundRect(x,y,SEG_THICK,len,SEG_THICK/2,c); }

// erase helpers: restore ocean gradient in the segment area
void eraseHSegBG(int x,int y,int len){ fillRectWithOceanBG(x, y, len, SEG_THICK); }
void eraseVSegBG(int x,int y,int len){ fillRectWithOceanBG(x, y, SEG_THICK, len); }

// draw only the segments contained in 'mask'
void drawDigitSegments(int x0,int y0,uint8_t mask,bool erase){
  if (erase) {
    if(mask & (1<<SEG_A)) eraseHSegBG(x0+SEG_HGAP, y0, SEG_LEN);
    if(mask & (1<<SEG_G)) eraseHSegBG(x0+SEG_HGAP, y0+SEG_VLEN+SEG_THICK, SEG_LEN);
    if(mask & (1<<SEG_D)) eraseHSegBG(x0+SEG_HGAP, y0+2*SEG_VLEN+2*SEG_THICK, SEG_LEN);
    if(mask & (1<<SEG_B)) eraseVSegBG(x0+SEG_HGAP+SEG_LEN, y0+SEG_THICK/2, SEG_VLEN);
    if(mask & (1<<SEG_C)) eraseVSegBG(x0+SEG_HGAP+SEG_LEN, y0+SEG_VLEN+(3*SEG_THICK)/2, SEG_VLEN);
    if(mask & (1<<SEG_F)) eraseVSegBG(x0, y0+SEG_THICK/2, SEG_VLEN);
    if(mask & (1<<SEG_E)) eraseVSegBG(x0, y0+SEG_VLEN+(3*SEG_THICK)/2, SEG_VLEN);
  } else {
    uint16_t col = COL_GLOW;
    if(mask & (1<<SEG_A)) drawHSeg(x0+SEG_HGAP, y0, SEG_LEN, col);
    if(mask & (1<<SEG_G)) drawHSeg(x0+SEG_HGAP, y0+SEG_VLEN+SEG_THICK, SEG_LEN, col);
    if(mask & (1<<SEG_D)) drawHSeg(x0+SEG_HGAP, y0+2*SEG_VLEN+2*SEG_THICK, SEG_LEN, col);
    if(mask & (1<<SEG_B)) drawVSeg(x0+SEG_HGAP+SEG_LEN, y0+SEG_THICK/2, SEG_VLEN, col);
    if(mask & (1<<SEG_C)) drawVSeg(x0+SEG_HGAP+SEG_LEN, y0+SEG_VLEN+(3*SEG_THICK)/2, SEG_VLEN, col);
    if(mask & (1<<SEG_F)) drawVSeg(x0, y0+SEG_THICK/2, SEG_VLEN, col);
    if(mask & (1<<SEG_E)) drawVSeg(x0, y0+SEG_VLEN+(3*SEG_THICK)/2, SEG_VLEN, col);
  }
}

// ---------- Hour/Minute/Second digit hard refresh ----------
void digitBounds(int di, int &x, int &y, int &w, int &h){
  x = DIG_X[di];
  y = DIGIT_Y;
  // widened by SEG_THICK so the right vertical segment is fully inside
  w = SEG_LEN + 2*SEG_HGAP + SEG_THICK;
  h = 2*SEG_VLEN + 3*SEG_THICK;
}

void hardRefreshDigit(int di, uint8_t newMask){
  int x,y,w,h;
  digitBounds(di,x,y,w,h);
  fillRectWithOceanBG(x, y, w, h);
  if (newMask) drawDigitSegments(x, y, newMask, /*erase=*/false);
  prevDigitMask[di] = newMask;
}

// ---------- Pulsing colons ----------
void drawColonColor(uint16_t col){
  int r = SEG_THICK/2 + 1;
  for(int i=0;i<2;i++){
    tft.fillCircle(COLON_X[i], COLON_Y, r, col);
    tft.fillCircle(COLON_X[i], COLON_Y + SEG_VLEN, r, col);
  }
}

// ---------- Timers & flags ----------
unsigned long lastTickMs = 0;   // aligned to the start of the current second
unsigned long subAnimMs  = 0;   // ~30ms updates
bool repaintAll = false;

// ---------- Colon pulse ----------
void updateColonPulse(unsigned long ms){
  unsigned long frac = (ms >= lastTickMs) ? (ms - lastTickMs) : 0;
  if (frac > 999) frac = 999;
  float phase = frac / 1000.0f;
  float bright = 0.5f * (1.0f - cosf(2.0f * 3.1415926f * phase)); // 0→1→0
  uint8_t t = (uint8_t)(bright * 255.0f + 0.5f);
  uint16_t col = lerp565(COL_GLOW_DIM, COL_GLOW, t);
  drawColonColor(col);
}

// --- Seconds bar (globals & functions) ---
int barX=10, barY, barW, barH=10;
int prevFillW = -1;

void drawSecondsBarFrameOnce(){
  barY = H - 26; barW = W - 20;
  tft.drawRoundRect(barX-1,barY-1,barW+2,barH+2,4,COL_GLOW2);
}
void drawSecondsBarFill(uint8_t sec){
  int fillW = map(sec,0,59,0,barW);
  if (fillW == prevFillW) return;
  if (prevFillW < 0) {
    tft.fillRect(barX, barY, fillW, barH, COL_BAR);
    prevFillW = fillW;
    return;
  }
  if (fillW > prevFillW) {
    tft.fillRect(barX + prevFillW, barY, fillW - prevFillW, barH, COL_BAR);
  } else {
    fillRectWithOceanBG(barX + fillW, barY, (prevFillW - fillW), barH);
  }
  prevFillW = fillW;
}

// ---------- Title strip ----------
char lastStatus[48] = "Waiting for time";
void drawTitleStrip() {
  tft.fillRect(0,0,W,24,COL_GLOW2);
  tft.setTextSize(2);tft.setTextColor(0x0000, COL_GLOW2);
  tft.setCursor(6,4);tft.print("BT SYNC: ");
  tft.print(lastStatus);
}

/* ======================= ENVIRONMENT (TEMP/HUM) LINE — FLICKER-FREE ======================= */
const int ENV_X = 6;
const int ENV_Y = 26;
const int ENV_TEXT_SIZE = 2;
const int CHAR_W = 6 * ENV_TEXT_SIZE;
const int ENV_LINE_LEN = 26;
uint16_t ENV_BG = 0;

float envTemp = NAN, envHum = NAN;
unsigned long lastEnvRead = 0;
const unsigned long ENV_INTERVAL = 2000;    // 2s
char lastEnvStr[ENV_LINE_LEN + 1] = "";

void drawEnvLine(const char* s){
  char buf[ENV_LINE_LEN + 1];
  size_t n = strnlen(s, ENV_LINE_LEN);
  strncpy(buf, s, ENV_LINE_LEN);
  for (size_t i = n; i < ENV_LINE_LEN; ++i) buf[i] = ' ';
  buf[ENV_LINE_LEN] = '\0';

  tft.setTextSize(ENV_TEXT_SIZE);
  tft.setTextColor(COL_TEXT, ENV_BG);
  tft.setCursor(ENV_X, ENV_Y);
  tft.print(buf);
}

void updateEnv(){
  unsigned long ms = millis();
  if (ms - lastEnvRead < ENV_INTERVAL) return;
  lastEnvRead = ms;

  float h = dht.readHumidity();
  float t = dht.readTemperature(); // °C

  char line[ENV_LINE_LEN + 1];

  if (!isnan(h) && !isnan(t)) {
    char tbuf[8], hbuf[8];
    dtostrf(t, 4, 1, tbuf);  // "24.3" or " 9.8"
    dtostrf(h, 3, 0, hbuf);  // " 56"
    snprintf(line, sizeof(line), "TEMP %4s C   HUM %3s %%", tbuf, hbuf);

    static float lastT = NAN, lastH = NAN;
    bool changedValues = isnan(lastT) || isnan(lastH) ||
                         fabs(t - lastT) >= 0.2 || fabs(h - lastH) >= 1.0;
    bool changedText = (strncmp(line, lastEnvStr, ENV_LINE_LEN) != 0);

    if (changedValues || changedText) {
      drawEnvLine(line);
      strncpy(lastEnvStr, line, ENV_LINE_LEN);
      lastEnvStr[ENV_LINE_LEN] = '\0';
      lastT = t; lastH = h;
    }
  } else {
    snprintf(line, sizeof(line), "TEMP --.- C   HUM -- %%");
    if (strncmp(line, lastEnvStr, ENV_LINE_LEN) != 0) {
      drawEnvLine(line);
      strncpy(lastEnvStr, line, ENV_LINE_LEN);
      lastEnvStr[ENV_LINE_LEN] = '\0';
    }
  }
}
/* =========================================================================================== */

// ---------- Layout ----------
void setupLayout(){
  DIGIT_Y = 56;  // space under the env line
  int dcellW = SEG_LEN + 2*SEG_HGAP;
  int totalW = 6*dcellW + 2*COLON_W + 7*E_SP;
  int startX = max(2, (int)((W - totalW) > 0 ? (W - totalW)/2 : 2));
  int x = startX;

  DIG_X[0]=x; x+=dcellW+E_SP; DIG_X[1]=x; x+=dcellW+E_SP;
  COLON_X[0]=x+COLON_W/2; x+=COLON_W+E_SP;
  DIG_X[2]=x; x+=dcellW+E_SP; DIG_X[3]=x; x+=dcellW+E_SP;
  COLON_X[1]=x+COLON_W/2; x+=COLON_W+E_SP;
  DIG_X[4]=x; x+=dcellW+E_SP; DIG_X[5]=x;
  COLON_Y = DIGIT_Y + SEG_VLEN/2 - 2;
}

// ---------- Date ----------
void drawDateLine(const ClockTime &c){
  tft.setTextSize(2); tft.setTextColor(COL_TEXT, COL_BG);

  char line1[16];
  snprintf(line1,sizeof(line1),"%02d-%02d-%04d",
           c.hasDate && c.day?c.day:0,
           c.hasDate && c.month?c.month:0,
           c.hasDate && c.year?c.year:0);

  if(strcmp(line1,prevDateStr)!=0){
    fillRectWithOceanBG(0, H-70, W, 45);
    int16_t x1,y1; uint16_t w1,h1; tft.getTextBounds(line1,0,0,&x1,&y1,&w1,&h1);
    tft.setCursor((W-w1)/2, H-70); tft.print(line1);

    int wd=0; const char* wn=""; const char* mn="";
    if (c.hasDate && c.year>=1970 && c.month>=1 && c.month<=12 && c.day>=1) {
      wd=weekdayIndex(c.year,c.month,c.day);
      wn=WEEKDAY_NAME[wd]; mn=MONTH_NAME[c.month-1];
    }
    char line2[32];
    if (*wn) snprintf(line2,sizeof(line2),"%s , %s %d",wn,mn,c.day);
    else snprintf(line2,sizeof(line2),"%s","");

    int16_t x2,y2; uint16_t w2,h2; tft.getTextBounds(line2,0,0,&x2,&y2,&w2,&h2);
    tft.setCursor((W-w2)/2, H-50); tft.print(line2);

    strncpy(prevDateStr,line1,sizeof(prevDateStr));
  }
}

// ---------- Digits ----------
void drawTimeDigits(const ClockTime &c){
  char cur[9]; snprintf(cur,sizeof(cur),"%02d:%02d:%02d",c.hour,c.minute,c.second);

  for(int i=0;i<8;i++){
    if(i==2||i==5) continue;        // skip colons
    if(cur[i]==prevTimeStr[i]) continue;

    int di=(i<2)?i:(i<5?i-1:i-2);   // map to digit index
    uint8_t newMask = (cur[i]>='0'&&cur[i]<='9')? DIGIT_MASK[cur[i]-'0'] : 0;
    uint8_t oldMask = prevDigitMask[di];

    if (di == 0 || di == 1 ||                                // hours
        (HARD_REFRESH_MINUTES  && (di == 2 || di == 3)) ||   // minutes
        (HARD_REFRESH_SECONDS  && (di == 4 || di == 5)) ) {  // seconds
      hardRefreshDigit(di, newMask);
    } else {
      uint8_t offMask = (uint8_t)(oldMask & ~newMask);
      if (offMask) drawDigitSegments(DIG_X[di], DIGIT_Y, offMask, /*erase=*/true);
      uint8_t onMask  = (uint8_t)(newMask & ~oldMask);
      if (onMask)  drawDigitSegments(DIG_X[di], DIGIT_Y, onMask, /*erase=*/false);
      prevDigitMask[di] = newMask;
    }
  }
  strncpy(prevTimeStr,cur,sizeof(prevTimeStr));
}

// ---------- Optional scanline shimmer ----------
void animateScanline(){
#if ENABLE_SCANLINE
  static int y=28; tft.drawFastHLine(5,y,W-10,oceanColorAtY(y)); // clean previous
  y+=2; if(y>(int)(H-30)) y=28;
  tft.drawFastHLine(5,y,W-10,COL_GLOW);
#endif
}

// ---------- Force a first draw ----------
void forceRedrawAll() {
  memset(prevTimeStr,0,sizeof(prevTimeStr));
  memset(prevDateStr,0,sizeof(prevDateStr));
  for (int i=0;i<6;i++) prevDigitMask[i]=0;
  prevFillW = -1;

  drawTimeDigits(ct);
  drawDateLine(ct);
  drawSecondsBarFrameOnce();
  drawSecondsBarFill(ct.second);

  drawEnvLine("TEMP --.- C   HUM -- %");
}

// ---------------- Arduino ----------------
void setup() {
  BT_SERIAL.begin(9600);

  uint16_t id = tft.readID();
  if (id == 0xD3D3) id = 0x9486;
  tft.begin(id);
  tft.setRotation(1);  // Landscape
  W = tft.width();
  H = tft.height();

  // --- Keep SD off SPI bus (helps avoid interference) ---
  pinMode(10, OUTPUT); digitalWrite(10, HIGH); // many shields use D10 as SD CS (Uno layout)
  pinMode(53, OUTPUT); digitalWrite(53, HIGH); // Mega hardware SS — keep HIGH

  // Compute env-line background once (gradient only depends on Y)
  ENV_BG = oceanColorAtY(ENV_Y);

  autoSizeToFit();
  drawSealBackground();
  drawTitleStrip();
  setupLayout();

  forceRedrawAll();

  lastTickMs = millis();
  subAnimMs  = lastTickMs;

  dht.begin();
}

void loop() {
  // ---------- Read Bluetooth line ----------
  if (BT_SERIAL.available()) {
    in_i = 0;
    while (BT_SERIAL.available()) {
      char c = BT_SERIAL.read();
      if (c == '\n' || c == '\r') break;
      if (in_i < (int)sizeof(str_in)-1) str_in[in_i++] = c;
      delay(2);
    }
    str_in[in_i] = '\0';

    if (in_i > 0) {
      ClockTime parsed;
      if (parseTime(str_in, parsed)) {
        ct = parsed;
        ct.isSet = true;
        lastTickMs = millis();   // align animations to this second
        repaintAll = true;

        BT_SERIAL.print("SYNCED: ");
        BT_SERIAL.println(str_in);

        snprintf(lastStatus, sizeof(lastStatus), "Time synced");
        drawTitleStrip();
      } else {
        snprintf(lastStatus, sizeof(lastStatus), "Invalid time format!");
        drawTitleStrip();
        BT_SERIAL.print("INVALID: ");
        BT_SERIAL.println(str_in);
      }
    }
  }

  unsigned long ms = millis();

  // ---------- ~30ms UI animations ----------
  if(ms - subAnimMs >= 30UL){
    subAnimMs = ms;
    animateScanline();
    updateColonPulse(ms); // smooth pulsing colons
    updateEnv();          // DHT line (2s cadence, flicker-free)
  }

  // ---------- 1 Hz: advance time when set, redraw ----------
  if (ct.isSet) {
    while (ms - lastTickMs >= 1000UL) {
      lastTickMs += 1000UL;
      tickOneSecond(ct);

      drawTimeDigits(ct);              // HH:MM:SS hard-refresh
      drawSecondsBarFill(ct.second);

      if(ct.hour==0 && ct.minute==0 && ct.second==0){
        drawDateLine(ct);
      }
    }
  }

  if (repaintAll) {
    forceRedrawAll();
    repaintAll = false;
  }

  delay(6);
}
