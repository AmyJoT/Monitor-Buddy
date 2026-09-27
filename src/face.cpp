// Face rendering (DESKBUDDY-style expressive eyes).
// Ported and scaled up from DESKBUDDY-1.0 (Edison Science Corner):
//   https://github.com/EDISON-SCIENCE-CORNER/DESKBUDDY-1.0
// The original targets a 128x64 mono OLED; here everything is scaled
// ~2.5x for this 320x172 colour panel and driven by spring physics:
// springy eyes, laggy pupils, blink, saccades and breathing.
// IMU tilt is folded into the gaze so the eyes still follow the board.
//
// The canvas, colours, and page-dot helper stay in main.cpp. This file
// only owns the face.

#include "face.h"

#include <Arduino_GFX_Library.h>
#include <math.h>

extern Arduino_Canvas *gfx;
extern const int SCREEN_W;
extern const uint16_t FG;
extern const uint16_t BG;
extern float pressPulse;

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b);
void drawPageDots();
void drawBitmapScaled(int x, int y, const uint8_t *bmp, int w, int h, int scale, uint16_t color);

uint8_t faceMood = 0;
static uint8_t dizzyReturnMood = 0;
static uint32_t dizzyUntil = 0;
static bool dizzyFromShake = false;

// Emotion particle bitmaps (16x16, 1-bit), drawn scaled 2x.
static const unsigned char bmp_heart[] PROGMEM = {
  0x00,0x00,0x0c,0x60,0x1e,0xf0,0x3f,0xf8,0x7f,0xfc,0x7f,0xfc,0x7f,0xfc,0x3f,0xf8,
  0x1f,0xf0,0x0f,0xe0,0x07,0xc0,0x03,0x80,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
static const unsigned char bmp_zzz[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x00,0x0c,0x00,0x18,0x00,0x30,0x00,0x7e,
  0x00,0x00,0x3c,0x00,0x0c,0x00,0x18,0x00,0x30,0x00,0x7c,0x00,0x00,0x00,0x00,0x00 };
static const unsigned char bmp_anger[] PROGMEM = {
  0x00,0x00,0x11,0x10,0x2a,0x90,0x44,0x40,0x80,0x20,0x80,0x20,0x44,0x40,0x2a,0x90,
  0x11,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };

// Eye geometry on the 320x172 canvas (centres, not top-left).
static const float FACE_LEFT_CX  = 105.0f;
static const float FACE_RIGHT_CX = 215.0f;
static const float FACE_EYES_CY  = 80.0f;

// One eye: animated CENTRE (x,y) + size (w,h), plus a pupil that lags behind.
// Each quantity is a critically-ish damped spring toward its target.
struct Eye {
  float x, y, w, h;
  float targetX, targetY, targetW, targetH;
  float pupilX, pupilY, targetPupilX, targetPupilY;
  float velX = 0, velY = 0, velW = 0, velH = 0, pVelX = 0, pVelY = 0;
  float k  = 0.12f;   // eye spring
  float d  = 0.60f;   // eye damping (heavier feel)
  float pk = 0.08f;   // pupil spring (softer/laggier)
  float pd = 0.50f;   // pupil damping
  bool  blinking = false;
  unsigned long lastBlink = 0, nextBlinkTime = 0;

  void init(float _x, float _y, float _w, float _h) {
    x = targetX = _x; y = targetY = _y;
    w = targetW = _w; h = targetH = _h;
    pupilX = targetPupilX = 0; pupilY = targetPupilY = 0;
    nextBlinkTime = millis() + random(1000, 4000);
  }
  void update() {
    velX = (velX + (targetX - x) * k) * d;
    velY = (velY + (targetY - y) * k) * d;
    velW = (velW + (targetW - w) * k) * d;
    velH = (velH + (targetH - h) * k) * d;
    x += velX; y += velY; w += velW; h += velH;
    pVelX = (pVelX + (targetPupilX - pupilX) * pk) * pd;
    pVelY = (pVelY + (targetPupilY - pupilY) * pk) * pd;
    pupilX += pVelX; pupilY += pVelY;
  }
};

static Eye leftEye, rightEye;
static unsigned long lastSaccade     = 0;
static unsigned long saccadeInterval = 3000;
static unsigned long nextWink = 0;
static unsigned long winkStart = 0;
static const uint8_t WINK_LEFT  = 0;
static const uint8_t WINK_RIGHT = 1;
static const uint8_t WINK_BOTH  = 2;
static uint8_t winkEye = WINK_LEFT;
static float winkCover = 0.0f;
static float gazeLX = 0.0f, gazeLY = 0.0f;
static float breathVal = 0.0f;
static const unsigned long WINK_CLOSE_MS = 240;
static const unsigned long WINK_HOLD_MS  = 80;
static const unsigned long WINK_OPEN_MS  = 320;
static const unsigned long WINK_TOTAL_MS = WINK_CLOSE_MS + WINK_HOLD_MS + WINK_OPEN_MS;
static const float WINK_PEAK = 0.72f;

static const uint8_t LID_NONE        = 0;
static const uint8_t LID_ANGRY       = 1;
static const uint8_t LID_SAD         = 2;
static const uint8_t LID_SLEEPY      = 3;
static const uint8_t LID_SUSPICIOUS  = 4;
static const int8_t  JITTER_NONE     = 0;
static const int8_t  JITTER_X        = 1;   // surprised: random pupil X
static const int8_t  JITTER_Y        = 2;   // excited: random pupil Y
// Hard-coded face colours (not affected by THEME). 0 sclera means FG.
static const uint16_t SCLERA_ANGRY   = 0xF88B;   // rgb(255, 80, 90)
static const uint16_t SCLERA_DIZZY   = 0xBFF7;   // rgb(190, 255, 190)

typedef void (*MoodPupilFn)(Eye &e, bool isLeft, int ix, int iy, int iw, int ih,
                            int cx, int cy, int r, int pw, int ph);
typedef void (*MoodExtraFn)();

// One row per MOOD_* id. See face.h for how to add the next mood.
struct MoodSpec {
  float w, h;            // both eyes; h is used unless leftH/rightH are set
  float leftH, rightH;   // >0 overrides h for that eye (suspicious)
  float pupilYBias;
  float breath;          // multiplied by breathVal and added to height
  int8_t jitter;
  uint8_t lids;
  uint16_t sclera;       // 0 = FG
  bool orbit;            // counter-rotating pupils + eye-centre wobble
  MoodPupilFn drawPupil; // null = default round pupil + glint
  MoodExtraFn beforeEyes;
  MoodExtraFn afterEyes;
};

static void drawHappyPupil(Eye &e, bool isLeft, int ix, int iy, int iw, int ih, int cx, int cy, int r, int pw, int ph);
static void drawExcitedPupil(Eye &e, bool isLeft, int ix, int iy, int iw, int ih, int cx, int cy, int r, int pw, int ph);
static void drawLovePupil(Eye &e, bool isLeft, int ix, int iy, int iw, int ih, int cx, int cy, int r, int pw, int ph);
static void drawLoveBits();
static void drawSleepyBits();
static void drawAngryBits();
static void drawHappyBlush();
static void drawDizzySparkles();

static const MoodSpec kMoods[FACE_MOOD_COUNT] = {
  /* NORMAL     */ { 90, 90,  0,   0,   0, 1, JITTER_NONE, LID_NONE,       0,            false, nullptr,          nullptr,          nullptr },
  /* HAPPY      */ { 92, 92,  0,   0,  -6, 0, JITTER_NONE, LID_NONE,       0,            false, drawHappyPupil,   nullptr,          drawHappyBlush },
  /* SURPRISED  */ { 75, 112, 0,   0,   0, 0, JITTER_X,    LID_NONE,       0,            false, nullptr,          nullptr,          nullptr },
  /* SLEEPY     */ { 95, 75,  0,   0,   0, 0, JITTER_NONE, LID_SLEEPY,     0,            false, nullptr,          drawSleepyBits,   nullptr },
  /* ANGRY      */ { 85, 80,  0,   0,   0, 0, JITTER_NONE, LID_ANGRY,      SCLERA_ANGRY, false, nullptr,          drawAngryBits,    nullptr },
  /* SAD        */ { 85, 100, 0,   0,   0, 0, JITTER_NONE, LID_SAD,        0,            false, nullptr,          nullptr,          nullptr },
  /* EXCITED    */ { 90, 106, 0,   0,   0, 1, JITTER_Y,    LID_NONE,       0,            false, drawExcitedPupil, nullptr,          nullptr },
  /* LOVE       */ { 96, 96,  0,   0,   0, 0, JITTER_NONE, LID_NONE,       0,            false, drawLovePupil,    drawLoveBits,     nullptr },
  /* SUSPICIOUS */ { 90, 0,   50,  105, 0, 0, JITTER_NONE, LID_SUSPICIOUS, 0,            false, nullptr,          nullptr,          nullptr },
  /* DIZZY      */ { 80, 80,  0,   0,   0, 0, JITTER_NONE, LID_NONE,       SCLERA_DIZZY, true,  nullptr,          drawDizzySparkles, nullptr },
};

static const MoodSpec &moodOf(uint8_t mood) {
  if (mood >= FACE_MOOD_COUNT) mood = MOOD_NORMAL;
  return kMoods[mood];
}

static void drawEyelidMask(int ix, int iy, int iw, int ih, uint8_t lids, bool isLeft) {
  int slant = (int)(iw * 0.167f);
  int band  = (int)(ih * 0.44f);
  if (lids == LID_ANGRY) {
    for (int i = 0; i < band; i++)
      if (isLeft) gfx->drawLine(ix, iy + i,         ix + iw, iy - slant + i, BG);
      else        gfx->drawLine(ix, iy - slant + i, ix + iw, iy + i,         BG);
  } else if (lids == LID_SAD) {
    for (int i = 0; i < band; i++)
      if (isLeft) gfx->drawLine(ix, iy - slant + i, ix + iw, iy + i,         BG);
      else        gfx->drawLine(ix, iy + i,         ix + iw, iy - slant + i, BG);
  } else if (lids == LID_SLEEPY) {
    gfx->fillRect(ix, iy, iw, ih / 2 + 5, BG);
  } else if (lids == LID_SUSPICIOUS) {
    if (isLeft) gfx->fillRect(ix, iy, iw, ih / 2 - 5, BG);
    else        gfx->fillRect(ix, iy + ih - (int)(ih * 0.22f), iw, (int)(ih * 0.22f), BG);
  }
}

static void placePupil(int ix, int iy, int iw, int ih, int cx, int cy,
                       float pupX, float pupY, int pw, int ph, int &px, int &py) {
  px = cx + (int)pupX - pw / 2;
  py = cy + (int)pupY - ph / 2;
  if (px < ix) px = ix;
  if (px + pw > ix + iw) px = ix + iw - pw;
  if (py < iy) py = iy;
  if (py + ph > iy + ih) py = iy + ih - ph;
}

static void drawDefaultPupil(Eye &e, int ix, int iy, int iw, int ih, int cx, int cy, int r, int pw, int ph) {
  int px, py;
  placePupil(ix, iy, iw, ih, cx, cy, e.pupilX, e.pupilY, pw, ph, px, py);
  gfx->fillRoundRect(px, py, pw, ph, r / 2, BG);
  if (iw > 30 && ih > 30) gfx->fillCircle(px + pw - 10, py + 10, 5, FG);
}

static void drawHappyPupil(Eye &e, bool, int ix, int iy, int iw, int ih, int cx, int cy, int r, int pw, int ph) {
  int px, py;
  placePupil(ix, iy, iw, ih, cx, cy, e.pupilX, e.pupilY, pw, ph, px, py);
  gfx->fillRoundRect(px, py, pw, ph, r / 2, BG);
  gfx->fillCircle(px + pw - 10, py + 10, 5, FG);
  gfx->fillCircle(px + 9, py + ph - 11, 3, FG);
}

static void drawExcitedPupil(Eye &e, bool isLeft, int ix, int iy, int iw, int ih, int cx, int cy, int r, int, int) {
  uint16_t gold = rgb(255, 210, 60);
  int pw = (int)(iw / 1.85f), ph = (int)(ih / 1.85f);
  int px, py;
  placePupil(ix, iy, iw, ih, cx, cy, e.pupilX, e.pupilY, pw, ph, px, py);
  gfx->fillRoundRect(px, py, pw, ph, r / 2, BG);
  gfx->fillCircle(px + pw - 10, py + 10, 5, gold);
  gfx->fillCircle(px + 9, py + ph - 11, 3, gold);
  int sx = isLeft ? (ix - 2) : (ix + iw + 2);
  int sy = iy + 2;
  gfx->fillRect(sx - 7, sy - 1, 15, 3, gold);
  gfx->fillRect(sx - 1, sy - 7, 3, 15, gold);
}

static void drawLovePupil(Eye &e, bool, int ix, int iy, int iw, int ih, int cx, int cy, int, int pw, int) {
  int scale = pw / 6;
  if (scale < 2) scale = 2;
  if (scale > 4) scale = 4;
  int hw = 16 * scale, hh = 16 * scale;
  int hx, hy;
  placePupil(ix, iy, iw, ih, cx, cy, e.pupilX, e.pupilY, hw, hh, hx, hy);
  drawBitmapScaled(hx, hy, bmp_heart, 16, 16, scale, BG);
}

static void drawLoveBits() {
  drawBitmapScaled(18,  8, bmp_heart, 16, 16, 2, FG);
  drawBitmapScaled(270, 8, bmp_heart, 16, 16, 2, FG);
}
static void drawSleepyBits() {
  drawBitmapScaled(144, 6, bmp_zzz, 16, 16, 2, FG);
}
static void drawAngryBits() {
  drawBitmapScaled(144, 8, bmp_anger, 16, 16, 2, FG);
}
static void drawHappyBlush() {
  uint16_t blush = rgb(255, 120, 150);
  gfx->fillCircle(70,  126, 13, blush);
  gfx->fillCircle(250, 126, 13, blush);
}
static void drawCross(int x, int y, uint16_t color) {
  gfx->fillRect(x - 6, y - 1, 13, 3, color);
  gfx->fillRect(x - 1, y - 6, 3, 13, color);
}
static void drawWinkLid(int ix, int iy, int iw, int ih, int r, float cover);
static float winkCoverAt(unsigned long now);

static void drawDizzySparkles() {
  float ang = millis() / 140.0f;
  uint16_t c = rgb(220, 255, 90);
  int x1 = 105 + (int)(cos(ang) * 18);
  int y1 = 22 + (int)(sin(ang) * 8);
  int x2 = 215 + (int)(cos(ang + 3.14159265f) * 18);
  int y2 = 22 + (int)(sin(ang + 3.14159265f) * 8);
  drawCross(x1, y1, c);
  drawCross(x2, y2, c);
}

static void drawEyeShape(Eye &e, bool isLeft) {
  int iw = (int)e.w, ih = (int)e.h;
  if (iw < 2 || ih < 2) return;
  int ix = (int)(e.x - e.w / 2.0f);
  int iy = (int)(e.y - e.h / 2.0f);

  int r  = (iw < 50) ? 6 : 16;
  const MoodSpec &mood = moodOf(faceMood);
  uint16_t sclera = mood.sclera ? mood.sclera : FG;
  gfx->fillRoundRect(ix, iy, iw, ih, r, sclera);

  int cx = ix + iw / 2, cy = iy + ih / 2;
  int pw = (int)(iw / 2.2f), ph = (int)(ih / 2.2f);
  if (mood.drawPupil) mood.drawPupil(e, isLeft, ix, iy, iw, ih, cx, cy, r, pw, ph);
  else                drawDefaultPupil(e, ix, iy, iw, ih, cx, cy, r, pw, ph);
  drawEyelidMask(ix, iy, iw, ih, mood.lids, isLeft);
  bool thisEye = winkEye == WINK_BOTH || isLeft == (winkEye == WINK_LEFT);
  if (winkCover > 0.02f && thisEye)
    drawWinkLid(ix, iy, iw, ih, r, winkCover);
}

// Lid drops over a full-size eye. Squashing targetH reads as a squish, not a wink.
static void drawWinkLid(int ix, int iy, int iw, int ih, int r, float cover) {
  if (cover > 0.92f) {
    gfx->fillRoundRect(ix - 1, iy - 1, iw + 2, ih + 2, r, BG);
    return;
  }
  int lid = (int)(ih * cover);
  if (lid < 1) return;
  gfx->fillRect(ix - 1, iy - 1, iw + 2, lid + 1, BG);
  // Shallow curve. A deep ellipse eats the pupil and reads as a slam.
  int ry = 3 + (int)(4.0f * cover);
  gfx->fillEllipse(ix + iw / 2, iy + lid, iw / 2 + 1, ry, BG);
}

static float smooth01(float u) {
  if (u < 0.0f) return 0.0f;
  if (u > 1.0f) return 1.0f;
  return u * u * (3.0f - 2.0f * u);
}

static float winkCoverAt(unsigned long now) {
  if (winkStart == 0 || now < winkStart) return 0.0f;
  unsigned long t = now - winkStart;
  if (t >= WINK_TOTAL_MS) return 0.0f;
  if (t < WINK_CLOSE_MS) return smooth01((float)t / WINK_CLOSE_MS) * WINK_PEAK;
  t -= WINK_CLOSE_MS;
  if (t < WINK_HOLD_MS) return WINK_PEAK;
  t -= WINK_HOLD_MS;
  return (1.0f - smooth01((float)t / WINK_OPEN_MS)) * WINK_PEAK;
}

static void updateFacePhysics(float tiltX, float tiltY) {
  unsigned long now = millis();
  breathVal = sin(now / 800.0f) * 3.5f;

  if (now > leftEye.nextBlinkTime) {
    leftEye.blinking = rightEye.blinking = true;
    leftEye.lastBlink = now;
    leftEye.nextBlinkTime = now + random(2000, 6000);
    winkStart = 0;
    winkCover = 0;
  }
  if (leftEye.blinking && now - leftEye.lastBlink > 120)
    leftEye.blinking = rightEye.blinking = false;

  if (!leftEye.blinking && now - lastSaccade > saccadeInterval) {
    lastSaccade = now;
    saccadeInterval = random(500, 3000);
    int dir = random(0, 10);
    if      (dir < 4)  { gazeLX = 0;   gazeLY = 0;   }
    else if (dir == 4) { gazeLX = -15; gazeLY = -10; }
    else if (dir == 5) { gazeLX = 15;  gazeLY = -10; }
    else if (dir == 6) { gazeLX = -15; gazeLY = 10;  }
    else if (dir == 7) { gazeLX = 15;  gazeLY = 10;  }
    else if (dir == 8) { gazeLX = 20;  gazeLY = 0;   }
    else               { gazeLX = -20; gazeLY = 0;   }
  }

  const MoodSpec &mood = moodOf(faceMood);
  float pupX = gazeLX + tiltX * 20.0f;
  float pupY = gazeLY + tiltY * 12.0f;
  leftEye.targetPupilX  = rightEye.targetPupilX = pupX;
  leftEye.targetPupilY  = rightEye.targetPupilY = pupY;
  leftEye.targetX  = FACE_LEFT_CX  + gazeLX * 0.3f + tiltX * 8.0f;
  rightEye.targetX = FACE_RIGHT_CX + gazeLX * 0.3f + tiltX * 8.0f;
  leftEye.targetY  = rightEye.targetY = FACE_EYES_CY + gazeLY * 0.3f + tiltY * 6.0f;

  if (!leftEye.blinking && mood.orbit) {
    float ang = now / 80.0f;
    const float radius = 10.0f;
    leftEye.targetPupilX  += cos(ang) * radius;
    leftEye.targetPupilY  += sin(ang) * radius;
    rightEye.targetPupilX += cos(-ang) * radius;
    rightEye.targetPupilY += sin(-ang) * radius;
    float wobX = sin(now / 280.0f) * 3.5f;
    float wobY = cos(now / 340.0f) * 2.5f;
    leftEye.targetX  += wobX;
    rightEye.targetX -= wobX;
    leftEye.targetY  += wobY;
    rightEye.targetY += wobY;
  }

  if (leftEye.blinking) {
    leftEye.targetH = rightEye.targetH = 4;
  } else {
    float lh = mood.leftH > 0.0f ? mood.leftH : mood.h;
    float rh = mood.rightH > 0.0f ? mood.rightH : mood.h;
    lh += mood.breath * breathVal;
    rh += mood.breath * breathVal;
    leftEye.targetW = rightEye.targetW = mood.w;
    leftEye.targetH = lh;
    rightEye.targetH = rh;
    leftEye.targetPupilY  += mood.pupilYBias;
    rightEye.targetPupilY += mood.pupilYBias;
    if (mood.jitter == JITTER_X) {
      leftEye.targetPupilX  += random(-3, 4);
      rightEye.targetPupilX += random(-3, 4);
    } else if (mood.jitter == JITTER_Y) {
      leftEye.targetPupilY  += random(-2, 3);
      rightEye.targetPupilY += random(-2, 3);
    }
  }

  // Idle wink: a soft lid over a full-size eye, sometimes both.
  // Skip moods that already hide the eye.
  bool canWink = !leftEye.blinking && !mood.orbit
                 && mood.lids != LID_SLEEPY && mood.lids != LID_SUSPICIOUS;
  bool winkBusy = winkStart != 0 && now < winkStart + WINK_TOTAL_MS;
  if (!canWink) {
    winkStart = 0;
    winkCover = 0;
  } else if (!winkBusy && now > nextWink) {
    int pick = random(0, 5);          // both is rarer than a single eye
    winkEye = pick == 4 ? WINK_BOTH : (pick < 2 ? WINK_LEFT : WINK_RIGHT);
    winkStart = now;
    nextWink = now + WINK_TOTAL_MS + random(14000, 26000);
    winkCover = 0;
  } else {
    winkCover = winkCoverAt(now);
  }

  leftEye.update();
  rightEye.update();
}

void faceInit() {
  leftEye.init(FACE_LEFT_CX,  FACE_EYES_CY, 90, 90);
  rightEye.init(FACE_RIGHT_CX, FACE_EYES_CY, 90, 90);
  nextWink = millis() + random(12000, 22000);
  winkStart = 0;
  winkCover = 0;
}

void drawFace(float tx, float ty, bool wifiConnected, bool wifiHasCreds) {
  gfx->fillScreen(BG);
  updateFacePhysics(tx, ty);

  const MoodSpec &mood = moodOf(faceMood);
  if (mood.beforeEyes) mood.beforeEyes();
  drawEyeShape(leftEye, true);
  drawEyeShape(rightEye, false);
  if (mood.afterEyes) mood.afterEyes();

  if (!wifiConnected) {
    gfx->setTextSize(1);
    gfx->setTextColor(rgb(150,150,150));
    const char *msg = wifiHasCreds ? "WIFI OFFLINE" : "HOLD TO SET UP WIFI";
    int w = (int)strlen(msg)*6;
    gfx->setCursor((SCREEN_W-w)/2, 6);
    gfx->print(msg);
  }
  drawPageDots();
}

void cancelShakeDizzy() {
  dizzyFromShake = false;
  dizzyUntil = 0;
}

void enterDizzyFromShake() {
  if (faceMood == MOOD_DIZZY && !dizzyFromShake) return;
  if (!dizzyFromShake) {
    dizzyReturnMood = faceMood;
    faceMood = MOOD_DIZZY;
    dizzyFromShake = true;
    pressPulse = 1.0f;
  }
  dizzyUntil = millis() + 2500;
}

void expireShakeDizzy() {
  if (!dizzyFromShake) return;
  if ((int32_t)(millis() - dizzyUntil) < 0) return;
  faceMood = dizzyReturnMood;
  dizzyFromShake = false;
  dizzyUntil = 0;
}

void cycleFaceMood() {
  faceMood = (faceMood + 1) % FACE_MOOD_COUNT;
}
