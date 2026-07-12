// =====================================================================
//   AlbertMicro — ESP32-C3  "LIVING PET"  v5  (OOP + Text Shows)
// ---------------------------------------------------------------------
//   Object-oriented rebuild. One class per responsibility:
//
//     BrainMemory : persistent mood/bonding (flash NVS)
//     Face        : RoboEyes + emote overlays + TEXT BANNERS
//     Motion      : servo core, beats, gaits, poses (interruptible)
//     TouchPad    : TTP223 tap/hold/poke language -> events
//     Choreo      : every trick, dance and TEXT PERFORMANCE
//     AutoPilot   : activity-based autonomous life
//
//   NEW TEXT PERFORMANCES (screen + motion combos)
//     GACOR : eye intro -> spin -> screen shows big "5G" + "GACOR!"
//     TEAM  : flashes "GUNG MAYUN GABBY ABUI TIO" 3x with cha-cha steps
//     INTRO : eyes -> "HI" "AKU" "ALBERT" -> bow -> back to eyes
//
//   WIRING (v5)
//     OLED   : SDA=GPIO6  SCL/SCK=GPIO4
//     Servos : FR=GPIO0  BR=GPIO1  FL=GPIO2  BL=GPIO3
//              (edit SERVO_PINS if wiring changes)
//     Touch  : TTP223 OUT -> GPIO10
//
//   COMMANDS
//     move   : WALK BACK LEFT RIGHT SL SR STOP
//     pose   : UP DOWN SIT LIE LAYDOWN REST SLEEP
//     trick  : PUSHUPS SWING GALLOP BOUNCE STRETCH WAVE SHAKE WIGGLE
//              BOW BEG SPINPOSE YES NO SAD SCARED
//              PLAYDEAD SNEEZE KISS ZOOMIES TANTRUM SULK SHY
//     dance  : DANCE MOONWALK CHACHA HEADBANG STOMP SALUTE PEEKABOO
//              POUNCE TWIST PARTY BREAKDANCE ROBOT DISCO BALLET
//     shows  : GACOR TEAM INTRO DEMO
//     face   : HAPPY ANGRY GRUMPY FURIOUS POUT TIRED SLEEPY SURPRISED
//              SUS CUTE LOVE NERVOUS CYCLOPS LAUGH CONFUSED SCAN WINK FACE
//     gaze   : LOOKUP LOOKDOWN LOOKLEFT LOOKRIGHT
//     system : AUTO PET STATUS STATS SAVE RESET INFO
// =====================================================================

#include <ESP32Servo.h>
#include <math.h>
#include <Wire.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <FluxGarage_RoboEyes.h>
#undef N
#undef E
#undef S
#undef W
#undef NE
#undef NW
#undef SE
#undef SW

// =====================================================================
// PIN CONFIG — the only place you should ever need to edit wiring
// =====================================================================
#define I2C_SDA        6
#define I2C_SCL        4          // SCL/SCK back on GPIO4
#define OLED_ADDR      0x3C
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64

#define NUM_SERVOS     4
// order everywhere: 0=FL 1=FR 2=BL 3=BR
// ACTUAL WIRING: FR=GPIO0  BR=GPIO1  FL=GPIO2  BL=GPIO3
// (left/right servos are MIRRORED — FR horn faces front, BR horn faces
//  back — so left legs get CENTER-x while right legs get CENTER+x; the
//  pose math already handles that, this table only maps leg -> GPIO)
const int SERVO_PINS[NUM_SERVOS] = {2, 0, 3, 1};

#define TOUCH_ENABLED      true
#define TOUCH_PIN          10
#define TTP223_ACTIVE_HIGH true

// eye geometry — the consistent "polygon" look
#define EYE_W       36
#define EYE_H       36
#define EYE_RADIUS   7
#define EYE_GAP     10

// RoboEyes gaze codes via safe aliases
#define POS_N 1
#define POS_NE 2
#define POS_E 3
#define POS_SE 4
#define POS_S 5
#define POS_SW 6
#define POS_W 7
#define POS_NW 8
#define POS_CENTER 0

// servo motion constants
#define CENTER       90
#define SERVOMIN     0
#define SERVOMAX     180
// max pose stretch from CENTER for "fully" extended poses. 80 (not 90)
// keeps the servo just off its hard stop: pressing against the stop
// stalls the motor -> current spike -> BROWN-OUT reset.
#define STRETCH      80
#define DELAY_TIME   3   // ms per interpolation tick — lower = snappier
#define SPEED_FACTOR 0.5f
#define MAX_STEP     3
#define MOVE_STEPS   35

// ---------------------------------------------------------------------
// globals shared across classes (kept minimal on purpose)
// ---------------------------------------------------------------------
// I2C at 1 MHz during transfers: a full OLED frame drops from ~23 ms to
// ~9 ms, so face redraws stop starving the servo loop. SSD1306 modules
// handle 1 MHz fine; if yours ever glitches, lower 1000000UL to 800000UL.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1,
                         1000000UL, 400000UL);
RoboEyes<Adafruit_SSD1306> roboEyes(display);

bool   seqAbort  = false;    // any running sequence must bail out
bool   bleConnected = false;
BLECharacteristic* pTxCharacteristic = nullptr;

// ---- command FIFO ----
// BLE writes arrive on a different FreeRTOS task. The old single
// `bleBuffer` String got OVERWRITTEN when two commands landed close
// together (e.g. the app's STATUS poll right after you tap TEAM) — the
// show command was silently lost. A small queue never drops commands.
#define CMD_QUEUE_LEN 6
String cmdQueue[CMD_QUEUE_LEN];
volatile int cmdHead = 0, cmdCount = 0;
portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;

void pushCmd(const String& s) {
  taskENTER_CRITICAL(&cmdMux);
  if (cmdCount < CMD_QUEUE_LEN) {
    cmdQueue[(cmdHead + cmdCount) % CMD_QUEUE_LEN] = s;
    cmdCount++;
  }
  taskEXIT_CRITICAL(&cmdMux);
}

bool cmdPending() { return cmdCount > 0; }

String popCmd() {
  taskENTER_CRITICAL(&cmdMux);
  String s = "";
  if (cmdCount > 0) {
    s = cmdQueue[cmdHead];
    cmdQueue[cmdHead] = "";
    cmdHead = (cmdHead + 1) % CMD_QUEUE_LEN;
    cmdCount--;
  }
  taskEXIT_CRITICAL(&cmdMux);
  return s;
}

// peek without consuming (safe copy under lock)
String peekCmd() {
  taskENTER_CRITICAL(&cmdMux);
  String s = (cmdCount > 0) ? cmdQueue[cmdHead] : "";
  taskEXIT_CRITICAL(&cmdMux);
  return s;
}

void bleSend(const char* msg) {
  if (bleConnected && pTxCharacteristic) {
    pTxCharacteristic->setValue(msg);
    pTxCharacteristic->notify();
  }
}

void sendStatus();   // defined after AutoPilot

// if any new input is waiting, ask the current sequence to stop.
// EXCEPTION: "STATUS" is the app's 3-second telemetry poll — answer it
// right here and keep going, otherwise it kills every show and dance
// within seconds (sequences were constantly being aborted).
void checkInterrupt() {
  while (cmdPending()) {
    String peek = peekCmd();
    peek.trim(); peek.toUpperCase();
    if (peek == "STATUS") { popCmd(); sendStatus(); }
    else { seqAbort = true; break; }     // real command: abort, keep it queued
  }
  if (Serial.available() > 0) seqAbort = true;
}

// =====================================================================
// CLASS BrainMemory — persistent personality (flash NVS)
// =====================================================================
class BrainMemory {
public:
  float happiness = 60, energy = 70, boredom = 0, bonding = 0;
  unsigned long lifetimePets = 0, lifetimePokes = 0, bootCount = 0;
  unsigned long lastInteraction = 0;

  void load() {
    prefs.begin("albert", false);
    happiness    = prefs.getFloat("hap", 60);
    energy       = prefs.getFloat("ene", 70);
    boredom      = prefs.getFloat("bor", 0);
    bonding      = prefs.getFloat("bond", 0);
    lifetimePets = prefs.getULong("pets", 0);
    lifetimePokes= prefs.getULong("pokes", 0);
    bootCount    = prefs.getULong("boots", 0) + 1;
    prefs.putULong("boots", bootCount);
    prefs.end();
  }

  void save() {
    prefs.begin("albert", false);
    prefs.putFloat("hap",  happiness);
    prefs.putFloat("ene",  energy);
    prefs.putFloat("bor",  boredom);
    prefs.putFloat("bond", bonding);
    prefs.putULong("pets", lifetimePets);
    prefs.putULong("pokes",lifetimePokes);
    prefs.end();
  }

  void wipe() {
    prefs.begin("albert", false); prefs.clear(); prefs.end();
    happiness = 60; energy = 70; boredom = 0; bonding = 0;
    lifetimePets = 0; lifetimePokes = 0;
  }

  void maybeAutosave() {
    if (millis() - _lastSave > 30000) { save(); _lastSave = millis(); }
  }

  void clampAll() {
    happiness = constrain(happiness, 0, 100);
    energy    = constrain(energy,    0, 100);
    boredom   = constrain(boredom,   0, 100);
    bonding   = constrain(bonding,   0, 100);
  }

  void interact(float happyBoost, float energyCost) {
    happiness += happyBoost;
    energy    -= energyCost;
    boredom    = 0;
    lastInteraction = millis();
    clampAll();
  }

private:
  Preferences prefs;
  unsigned long _lastSave = 0;
};
BrainMemory brain;

// =====================================================================
// CLASS Face — eyes, emote overlays, and TEXT BANNERS
// ---------------------------------------------------------------------
// Owns everything on the OLED:
//   * consistent polygon-eye expressions
//   * the 12px top emote band (hearts / Zzz / ! / ? / notes / drops)
//   * big-text banners for the text performances (5G, team names, HI)
// =====================================================================
enum Overlay { OV_NONE, OV_ZZZ, OV_HEART, OV_SWEAT, OV_TEAR,
               OV_EXCLAIM, OV_QUESTION, OV_MUSIC,
               OV_ANGER,      // anime anger mark, top-left (grumpy)
               OV_RAGE };     // anger marks in BOTH corners (furious)

class Face {
public:
  void begin() {
    display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
    base();
    roboEyes.setAutoblinker(ON, 3, 2);
    roboEyes.setIdleMode(ON, 2, 2);
    roboEyes.setCuriosity(ON);
  }

  // TEXT STATE: while true the eyes do NOT draw at all, so banner text
  // owns the screen with zero overlap. Set by the text performances.
  bool textOnly = false;

  // ---- one frame: eyes + emote band ----
  // Display work is budgeted: render() is called from every servo step,
  // and each OLED push blocks the loop. Eyes-only = 1 push per frame at
  // 30 fps. With an overlay (music notes, hearts...) the eyes push PLUS
  // the overlay push = 2 pushes, so the frame rate halves to keep the
  // I2C time constant — this is what made dances/gallop crawl before.
  void render() {
    if (textOnly) return;              // text state: face stays off
    unsigned long now = millis();
    int interval = (_overlay == OV_NONE) ? 33 : 66;
    if (now - _lastFrame < (unsigned long)interval) return;
    _lastFrame = now;
    roboEyes.update();
    renderOverlay();
  }

  // =========== EXPRESSIONS (all share the polygon base) ===========
  void base() {
    roboEyes.setCyclops(OFF);
    roboEyes.setWidth(EYE_W, EYE_W);
    roboEyes.setHeight(EYE_H, EYE_H);
    roboEyes.setBorderradius(EYE_RADIUS, EYE_RADIUS);
    roboEyes.setSpacebetween(EYE_GAP);
  }

  void normal() {
    clearOverlay(); base();
    roboEyes.setMood(DEFAULT);
    roboEyes.setPosition(POS_CENTER);
    roboEyes.setAutoblinker(ON, 3, 2);
    roboEyes.setIdleMode(ON, 2, 2);
    roboEyes.setCuriosity(ON);
  }
  void happy()  { clearOverlay(); base(); roboEyes.setMood(HAPPY);  roboEyes.setPosition(POS_CENTER); roboEyes.setAutoblinker(ON, 2, 2); }
  void angry()  { clearOverlay(); base(); roboEyes.setMood(ANGRY);  roboEyes.setPosition(POS_CENTER); roboEyes.setAutoblinker(ON, 4, 1); }
  void tired()  { clearOverlay(); base(); roboEyes.setMood(TIRED);  roboEyes.setHeight(20, 20); roboEyes.setAutoblinker(ON, 2, 3); roboEyes.setIdleMode(OFF, 0, 0); }
  void sleepy() { base(); roboEyes.setMood(TIRED); roboEyes.setHeight(8, 8); roboEyes.setAutoblinker(ON, 5, 1); roboEyes.setIdleMode(OFF, 0, 0); setOverlay(OV_ZZZ); }
  void surprised() { base(); roboEyes.setMood(DEFAULT); roboEyes.setWidth(42, 42); roboEyes.setHeight(42, 42); roboEyes.setAutoblinker(OFF, 0, 0); setOverlay(OV_EXCLAIM, 900); }
  void suspicious(){ clearOverlay(); base(); roboEyes.setMood(DEFAULT); roboEyes.setHeight(14, 14); roboEyes.setPosition(POS_E); roboEyes.setAutoblinker(OFF, 0, 0); }
  void cute()   { clearOverlay(); base(); roboEyes.setMood(HAPPY); roboEyes.setWidth(38, 38); roboEyes.setHeight(40, 40); roboEyes.setAutoblinker(ON, 2, 1); }
  void love()   { base(); roboEyes.setMood(HAPPY); roboEyes.setWidth(38, 38); roboEyes.setHeight(38, 38); roboEyes.setSpacebetween(6); roboEyes.setAutoblinker(ON, 2, 1); roboEyes.setPosition(POS_CENTER); setOverlay(OV_HEART); }
  void nervous(){ base(); roboEyes.setMood(DEFAULT); roboEyes.setHeight(28, 28); roboEyes.setAutoblinker(ON, 1, 1); setOverlay(OV_SWEAT, 1500); }
  void cyclops(){ clearOverlay(); roboEyes.setCyclops(ON); roboEyes.setMood(DEFAULT); roboEyes.setWidth(44, 44); roboEyes.setHeight(44, 44); roboEyes.setBorderradius(EYE_RADIUS, EYE_RADIUS); roboEyes.setAutoblinker(ON, 3, 2); }
  void dancing(){ base(); roboEyes.setMood(HAPPY); roboEyes.setAutoblinker(ON, 2, 1); setOverlay(OV_MUSIC); }

  // grumpy: annoyed half-lidded glare to the side + anger mark top-left
  void grumpy() {
    base(); roboEyes.setMood(ANGRY);
    roboEyes.setHeight(18, 18);
    roboEyes.setPosition(POS_W);
    roboEyes.setAutoblinker(ON, 4, 2);
    roboEyes.setIdleMode(OFF, 0, 0);
    setOverlay(OV_ANGER);
  }
  // furious: MORE grumpy — wide angry stare, anger marks both corners
  void furious() {
    base(); roboEyes.setMood(ANGRY);
    roboEyes.setWidth(40, 40);
    roboEyes.setHeight(30, 30);
    roboEyes.setPosition(POS_CENTER);
    roboEyes.setAutoblinker(OFF, 0, 0);
    roboEyes.setIdleMode(OFF, 0, 0);
    setOverlay(OV_RAGE);
  }
  // excited: big bright eyes, rapid blinking, exclamation burst
  void excited() {
    base(); roboEyes.setMood(HAPPY);
    roboEyes.setWidth(42, 42);
    roboEyes.setHeight(44, 44);
    roboEyes.setAutoblinker(ON, 1, 1);
    roboEyes.setIdleMode(ON, 1, 1);
    setOverlay(OV_EXCLAIM, 1500);
  }
  // bored: flat half-lidded stare drifting downward
  void bored() {
    clearOverlay(); base();
    roboEyes.setMood(DEFAULT);
    roboEyes.setHeight(12, 12);
    roboEyes.setPosition(POS_S);
    roboEyes.setAutoblinker(ON, 5, 3);
    roboEyes.setIdleMode(OFF, 0, 0);
  }
  // dizzy: small wobbly eyes, question mark, confused shake
  void dizzy() {
    base(); roboEyes.setMood(DEFAULT);
    roboEyes.setWidth(24, 24);
    roboEyes.setHeight(24, 24);
    roboEyes.setAutoblinker(OFF, 0, 0);
    setOverlay(OV_QUESTION, 2500);
    roboEyes.anim_confused();
  }
  // proud: tall bright eyes, chin up, slow satisfied blinks
  void proud() {
    clearOverlay(); base();
    roboEyes.setMood(HAPPY);
    roboEyes.setHeight(44, 44);
    roboEyes.setPosition(POS_N);
    roboEyes.setAutoblinker(ON, 4, 2);
    roboEyes.setIdleMode(OFF, 0, 0);
  }

  // pout: droopy sad eyes looking down with a tear
  void pouty() {
    base(); roboEyes.setMood(TIRED);
    roboEyes.setHeight(24, 24);
    roboEyes.setPosition(POS_SW);
    roboEyes.setAutoblinker(ON, 3, 2);
    roboEyes.setIdleMode(OFF, 0, 0);
    setOverlay(OV_TEAR);
  }

  void laughBurst()   { roboEyes.setMood(HAPPY);  roboEyes.anim_laugh(); }
  void confusedBurst(){ roboEyes.setMood(DEFAULT); setOverlay(OV_QUESTION, 900); roboEyes.anim_confused(); }

  void gaze(int pos)  { roboEyes.setIdleMode(OFF, 0, 0); roboEyes.setPosition(pos); }

  // eyes closed / open primitives (used by winks & peekaboo)
  void eyesShut() { roboEyes.setAutoblinker(OFF, 0, 0); roboEyes.setHeight(4, 4); }
  void eyesOpen(int h = EYE_H) { roboEyes.setHeight(h, h); }

  // resting face matched to the mood
  void moodFace() {
    if (brain.energy < 25)         sleepy();
    else if (brain.happiness > 75) happy();
    else if (brain.happiness < 18) grumpy();   // properly annoyed
    else if (brain.happiness < 32) pouty();    // just a bit sad
    else if (brain.boredom > 70)   suspicious();
    else                           normal();
  }

  // =========== EMOTE BAND (top 12px, never touches eyes) ===========
  void setOverlay(Overlay ov, int durationMs = 0) {
    _overlay = ov;
    _overlayUntil = (durationMs > 0) ? millis() + durationMs : 0;
  }
  void clearOverlay() { _overlay = OV_NONE; _overlayUntil = 0; }

  // =========== TEXT BANNERS (for the text performances) ===========
  // one banner frame drawn straight to the OLED
  void drawBannerFrame(const char* line1, const char* line2,
                       int size1, int size2) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(size1);
    int w1 = strlen(line1) * 6 * size1;
    display.setCursor((SCREEN_WIDTH - w1) / 2,
                      line2 ? 8 : (SCREEN_HEIGHT - 8 * size1) / 2);
    display.print(line1);

    if (line2) {
      display.setTextSize(size2);
      int w2 = strlen(line2) * 6 * size2;
      display.setCursor((SCREEN_WIDTH - w2) / 2, 40);
      display.print(line2);
    }
    display.display();
  }

  // Fullscreen centered text, auto-sized. Interruptible hold.
  // The text is REPAINTED every ~120 ms during the hold so nothing
  // (eyes, overlays) can wipe it — text always shows on his face.
  bool banner(const char* line1, const char* line2, int holdTime,
              int size1 = 3, int size2 = 2) {
    clearOverlay();                          // overlays must not fight the text
    unsigned long start = millis(), lastDraw = 0;
    while (millis() - start < (unsigned long)holdTime) {
      if (millis() - lastDraw >= 120) {      // first pass draws immediately
        drawBannerFrame(line1, line2, size1, size2);
        lastDraw = millis();
      }
      checkInterrupt();
      if (seqAbort) return false;
      delay(10);
    }
    return true;
  }

  // flashing banner: blinks the text on/off `blinks` times
  bool bannerFlash(const char* line1, const char* line2,
                   int blinks, int onMs, int offMs,
                   int size1 = 3, int size2 = 2) {
    for (int i = 0; i < blinks; i++) {
      if (!banner(line1, line2, onMs, size1, size2)) return false;
      display.clearDisplay(); display.display();
      int elapsed = 0;
      while (elapsed < offMs) {
        checkInterrupt();
        if (seqAbort) return false;
        delay(10); elapsed += 10;
      }
    }
    return true;
  }

private:
  Overlay _overlay = OV_NONE;
  unsigned long _overlayUntil = 0;
  unsigned long _lastFrame = 0;

  void heartGlyph(int cx, int cy, int s) {
    display.fillCircle(cx - s/2, cy, s/2, SSD1306_WHITE);
    display.fillCircle(cx + s/2, cy, s/2, SSD1306_WHITE);
    display.fillTriangle(cx - s, cy, cx + s, cy, cx, cy + s, SSD1306_WHITE);
  }
  void zGlyph(int x, int y, int s) {
    display.drawLine(x, y, x + s, y, SSD1306_WHITE);
    display.drawLine(x + s, y, x, y + s, SSD1306_WHITE);
    display.drawLine(x, y + s, x + s, y + s, SSD1306_WHITE);
  }
  void dropGlyph(int cx, int cy) {
    display.fillCircle(cx, cy, 2, SSD1306_WHITE);
    display.fillTriangle(cx - 2, cy, cx + 2, cy, cx, cy - 4, SSD1306_WHITE);
  }
  void noteGlyph(int x, int y) {
    display.fillCircle(x, y + 6, 2, SSD1306_WHITE);
    display.drawLine(x + 2, y + 6, x + 2, y, SSD1306_WHITE);
    display.drawLine(x + 2, y, x + 5, y + 1, SSD1306_WHITE);
  }
  // anime-style anger vein: four short ticks radiating from a center
  void angerGlyph(int cx, int cy) {
    display.drawLine(cx - 5, cy - 5, cx - 2, cy - 2, SSD1306_WHITE);
    display.drawLine(cx + 5, cy - 5, cx + 2, cy - 2, SSD1306_WHITE);
    display.drawLine(cx - 5, cy + 5, cx - 2, cy + 2, SSD1306_WHITE);
    display.drawLine(cx + 5, cy + 5, cx + 2, cy + 2, SSD1306_WHITE);
  }

  void renderOverlay() {
    if (_overlay == OV_NONE) return;
    if (_overlayUntil != 0 && millis() > _overlayUntil) { clearOverlay(); return; }

    unsigned long t = millis();
    int bob = (int)((t / 250) % 3);

    switch (_overlay) {
      case OV_ZZZ:
        for (int i = 0; i <= (int)((t / 400) % 3); i++)
          zGlyph(100 + i * 9, 8 - i * 3, 4);
        break;
      case OV_HEART:
        heartGlyph(14, 4 + bob, 5);
        heartGlyph(64, 3 + bob, 6);
        heartGlyph(114, 4 + bob, 5);
        break;
      case OV_SWEAT:   dropGlyph(118, 5 + bob); break;
      case OV_TEAR:    dropGlyph(10, 5 + bob);  break;
      case OV_EXCLAIM:
        display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
        display.setCursor(61, 2); display.print("!");
        break;
      case OV_QUESTION:
        display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
        display.setCursor(61, 2); display.print("?");
        break;
      case OV_MUSIC:
        noteGlyph(12, 2 + bob);
        noteGlyph(110, 2 + (2 - bob));
        break;
      case OV_ANGER:                       // grumpy mark, TOP-LEFT
        angerGlyph(10, 6 + (bob & 1));
        break;
      case OV_RAGE:                        // furious: both corners throb
        angerGlyph(10,  6 + (bob & 1));
        angerGlyph(118, 6 + ((bob + 1) & 1));
        break;
      default: break;
    }
    display.display();
  }
};
Face face;

// =====================================================================
// CLASS Motion — servo core, beats, gait, poses (all interruptible)
// =====================================================================
struct Beat {
  float fl, fr, bl, br;
  int steps;
  int holdAfter;
};

class TouchPad;                    // fwd decl
extern TouchPad touchpad;
void pollTouchGlobal();            // fwd decl (defined after TouchPad)

class Motion {
public:
  float currentPos[NUM_SERVOS], targetPos[NUM_SERVOS];
  int   lastPulse[NUM_SERVOS];

  void begin() {
    for (int i = 0; i < NUM_SERVOS; i++) {
      servos[i].attach(SERVO_PINS[i]);
      currentPos[i] = targetPos[i] = lastPulse[i] = CENTER;
      write(i, CENTER);
      delay(100);
    }
  }

  void write(int i, float deg) {
    servos[i].write(constrain((int)deg, SERVOMIN, SERVOMAX));
  }

  void setTargets(float fl, float fr, float bl, float br) {
    targetPos[0] = fl; targetPos[1] = fr; targetPos[2] = bl; targetPos[3] = br;
  }

  void sync() {
    for (int i = 0; i < NUM_SERVOS; i++) {
      currentPos[i] = (float)lastPulse[i];
      targetPos[i]  = (float)lastPulse[i];
    }
  }

  // smooth interpolation to targets; false if interrupted
  bool moveSteps(int steps) {
    if (steps < 1) steps = 1;
    float startPos[NUM_SERVOS];
    for (int i = 0; i < NUM_SERVOS; i++) startPos[i] = currentPos[i];
    for (int s = 1; s <= steps; s++) {
      checkInterrupt();
      if (seqAbort) return false;
      float t = (float)s / (float)steps;
      for (int i = 0; i < NUM_SERVOS; i++) {
        currentPos[i] = startPos[i] + t * (targetPos[i] - startPos[i]);
        lastPulse[i]  = (int)currentPos[i];
        write(i, currentPos[i]);
      }
      face.render();
      pollTouchGlobal();
      delay(DELAY_TIME);
    }
    return true;
  }
  bool moveAll() { return moveSteps(MOVE_STEPS); }

  // drift-toward-target used by MODE_IDLE
  void relaxTick() {
    for (int i = 0; i < NUM_SERVOS; i++) {
      float diff = targetPos[i] - currentPos[i];
      if (fabsf(diff) > 0.5f)
        currentPos[i] += constrain(diff * SPEED_FACTOR, -(float)MAX_STEP, (float)MAX_STEP);
      write(i, currentPos[i]);
    }
    delay(DELAY_TIME);
  }

  // interruptible pause with live face + touch
  bool holdMs(int ms) {
    int elapsed = 0;
    while (elapsed < ms) {
      checkInterrupt();
      if (seqAbort) return false;
      face.render();
      pollTouchGlobal();
      delay(10);
      elapsed += 10;
    }
    return true;
  }

  // play a beat array (the choreography primitive)
  bool playBeats(const Beat* beats, int count, int repeats = 1) {
    for (int r = 0; r < repeats; r++)
      for (int i = 0; i < count; i++) {
        setTargets(beats[i].fl, beats[i].fr, beats[i].bl, beats[i].br);
        if (!moveSteps(beats[i].steps)) return false;
        if (beats[i].holdAfter > 0 && !holdMs(beats[i].holdAfter)) return false;
      }
    return true;
  }

  // one continuous-gait tick (walking / spinning modes).
  // `reverse` runs the gait wave BACKWARD in time — that is the correct
  // way to walk backward. (Negating all amplitudes, the old Gaits::BACK,
  // only shifts every leg by 180° and the robot still walks forward.)
  void gaitTick(const float ampScale[NUM_SERVOS], float tempo = 1.0f,
                bool reverse = false) {
    _oldMs = _curMs; _curMs = millis();
    _innerTime += (unsigned long)((_curMs - _oldMs) * tempo);
    float t = FREQUENCY * (float)_innerTime;
    if (reverse) t = -t;
    for (int s = 0; s < NUM_SERVOS; s++) {
      float deg = CENTER + off_set_walk[s] + ampScale[s] * AMPLITUDE * cosf(t + gPhase[s]);
      deg = constrain(deg, SERVOMIN, SERVOMAX);
      write(s, deg); lastPulse[s] = (int)deg;
    }
    delay(DELAY_TIME);
  }

  // run a gait for a duration; false if interrupted
  bool gaitFor(const float ampScale[NUM_SERVOS], int ms, float tempo = 1.0f,
               bool reverse = false) {
    unsigned long start = millis();
    while (millis() - start < (unsigned long)ms) {
      checkInterrupt();
      if (seqAbort) return false;
      gaitTick(ampScale, tempo, reverse);
      face.render();
      pollTouchGlobal();
    }
    return true;
  }

  // ---------------- POSES ----------------
  bool stand()  { setTargets(CENTER, CENTER, CENTER, CENTER); return moveAll(); }

  // "sploot" DOWN: front legs forward, rear legs backward — Albert can
  // always push back up from this. Flip the signs if yours goes wrong way.
  bool sploot() {
    setTargets(CENTER - 65, CENTER + 65, CENTER + 65, CENTER - 65);
    return moveAll();
  }

  bool sit() { setTargets(CENTER, CENTER, CENTER - 40, CENTER + 40); return moveAll(); }

  bool lie() {
    setTargets(CENTER - 50, CENTER + 50, CENTER + 50, CENTER - 50);
    return moveAll();
  }

  // FULL LAYDOWN: front legs stretched all the way forward and back legs
  // all the way back — flat on the ground. STAGED front pair first, then
  // back pair, so only two servos draw peak current at a time (brown-out
  // mitigation), and slightly off the hard stop (STRETCH, not 90).
  bool laydown() {
    setTargets(CENTER - STRETCH, CENTER + STRETCH, currentPos[2], currentPos[3]);
    if (!moveSteps(30)) return false;
    setTargets(CENTER - STRETCH, CENTER + STRETCH, CENTER + STRETCH, CENTER - STRETCH);
    return moveSteps(30);
  }

private:
  Servo servos[NUM_SERVOS];
  unsigned long _curMs = 0, _oldMs = 0, _innerTime = 0;

  static constexpr float FREQUENCY = 0.005f;
  static constexpr float AMPLITUDE = 30.0f;
  const float gPhase[NUM_SERVOS]       = { 0, 0, PI - PI/2, PI - PI/2 };
  const float off_set_walk[NUM_SERVOS] = { -10, 10, 40, -40 };
};
Motion motion;

// canonical gait patterns shared by modes / dances / autopilot
namespace Gaits {
  const float FWD[]   = {  1.0f,  1.0f,  1.0f,  1.0f };
  // NOTE: there is intentionally no BACK amplitude set — to go backward
  // use the FWD set with gaitTick/gaitFor's `reverse = true` argument.
  const float LEFT[]  = {  0.3f,  0.3f,  1.5f,  1.5f };
  const float RIGHT[] = {  1.5f,  1.5f,  0.3f,  0.3f };
  const float SPINL[] = {  1.2f, -1.2f,  1.2f, -1.2f };
  const float SPINR[] = { -1.2f,  1.2f, -1.2f,  1.2f };
}

// =====================================================================
// CLASS Choreo — every trick, dance and TEXT PERFORMANCE
// ---------------------------------------------------------------------
// Each routine: sets a face, plays beats/gaits through Motion, ends on
// a mood face. Every one bails out instantly if a new command arrives.
// =====================================================================
class Choreo {
public:
  // ---------------- classic tricks ----------------
  void pushups() {
    face.angry();
    static const Beat b[] = {
      { CENTER+30, CENTER-30, CENTER-30, CENTER+30, 18, 120 },
      { CENTER,    CENTER,    CENTER,    CENTER,    18, 120 },
    };
    if (!motion.playBeats(b, 2, 8)) return;
    motion.stand(); face.happy(); face.laughBurst(); done();
  }

  void swing() {
    face.dancing();
    static const Beat b[] = {
      { CENTER+35, CENTER,    CENTER,    CENTER-35, 20, 120 },
      { CENTER,    CENTER-35, CENTER+35, CENTER,    20, 120 },
    };
    if (!motion.playBeats(b, 2, 6)) return;
    motion.stand(); done();
  }

  // GALLOP v2 — a real performance:
  //   1) anticipation crouch (looks left/right like checking the track)
  //   2) three rounds of bounds that get FASTER and BIGGER each round
  //   3) a big spring-loaded finishing leap + proud landing
  void gallop() {
    face.surprised();
    face.gaze(POS_W);
    motion.setTargets(CENTER-25, CENTER+25, CENTER+25, CENTER-25);   // crouch
    if (!motion.moveSteps(22)) return;
    if (!motion.holdMs(250)) return;
    face.gaze(POS_E);
    if (!motion.holdMs(250)) return;
    face.gaze(POS_CENTER);

    face.dancing();
    for (int round = 0; round < 3; round++) {
      int   steps = 12 - round * 3;          // 12 -> 9 -> 6 : faster
      float amp   = 30 + round * 9;          // 30 -> 39 -> 48 : bigger
      Beat b[] = {
        { CENTER+amp, CENTER-amp, CENTER-amp, CENTER+amp, steps, 40 },
        { CENTER-amp, CENTER+amp, CENTER+amp, CENTER-amp, steps, 40 },
      };
      if (!motion.playBeats(b, 2, 4)) return;
    }

    // finishing leap: deep crouch, SPRING, stick the landing
    face.surprised();
    motion.setTargets(CENTER-45, CENTER+45, CENTER+45, CENTER-45);
    if (!motion.moveSteps(10)) return;
    motion.setTargets(CENTER+55, CENTER-55, CENTER-55, CENTER+55);
    if (!motion.moveSteps(4)) return;
    if (!motion.holdMs(300)) return;
    motion.stand(); face.happy(); face.laughBurst(); done();
  }

  void bounce() {
    face.happy();
    if (!face.banner("BOING!", nullptr, 400, 3)) return;
    static const Beat b[] = {
      { CENTER+45, CENTER-45, CENTER+45, CENTER-45, 8,  60 },
      { CENTER,    CENTER,    CENTER,    CENTER,    8, 120 },
    };
    if (!motion.playBeats(b, 2, 6)) return;
    motion.stand(); done();
  }

  // gentle cat stretch (feedback-tuned angles)
  void stretch() {
    face.tired();
    static const Beat b[] = {
      { CENTER-25, CENTER+25, CENTER+35, CENTER-35, 45, 700 },
      { CENTER,    CENTER,    CENTER,    CENTER,    30, 200 },
      { CENTER+30, CENTER-30, CENTER-20, CENTER+20, 45, 700 },
    };
    if (!motion.playBeats(b, 3, 1)) return;
    motion.stand(); slowBlink(); done();
  }

  // waves the FRONT-RIGHT leg
  void wave() {
    face.cute();
    if (!face.banner("HALO!", nullptr, 450, 3)) return;
    motion.setTargets(CENTER, CENTER, CENTER-35, CENTER+35);
    if (!motion.moveSteps(25)) return;
    for (int i = 0; i < 4; i++) {
      motion.targetPos[1] = CENTER + 55;
      if (!motion.moveSteps(12)) return;
      motion.targetPos[1] = CENTER + 15;
      if (!motion.moveSteps(12)) return;
    }
    motion.stand(); face.laughBurst(); done();
  }

  void shake() {
    face.surprised();
    if (!face.banner("BRRR!", nullptr, 350, 3)) return;
    static const Beat b[] = {
      { CENTER+18, CENTER-18, CENTER-18, CENTER+18, 5, 35 },
      { CENTER-18, CENTER+18, CENTER+18, CENTER-18, 5, 35 },
    };
    if (!motion.playBeats(b, 2, 10)) return;
    motion.stand(); face.confusedBurst(); done();
  }

  void wiggle() {
    face.dancing();
    motion.setTargets(CENTER, CENTER, CENTER-30, CENTER+30);
    if (!motion.moveSteps(20)) return;
    static const Beat b[] = {
      { CENTER, CENTER, CENTER-45, CENTER+15, 6, 40 },
      { CENTER, CENTER, CENTER-15, CENTER+45, 6, 40 },
    };
    if (!motion.playBeats(b, 2, 10)) return;
    motion.stand(); done();
  }

  void bow() {
    face.normal();
    motion.setTargets(CENTER+55, CENTER-55, CENTER-10, CENTER+10);
    if (!motion.moveSteps(40)) return;
    slowBlink();
    if (!motion.holdMs(900)) return;
    motion.stand(); face.happy(); done();
  }

  void beg() {
    face.cute();
    motion.setTargets(CENTER-30, CENTER+30, CENTER-60, CENTER+60);
    if (!motion.moveSteps(40)) return;
    static const Beat b[] = {
      { CENTER-45, CENTER+15, CENTER-60, CENTER+60, 12, 60 },
      { CENTER-15, CENTER+45, CENTER-60, CENTER+60, 12, 60 },
    };
    if (!motion.playBeats(b, 2, 3)) return;
    motion.stand(); face.laughBurst(); done();
  }

  void spinPose() {
    face.dancing();
    if (!motion.gaitFor(Gaits::SPINL, 1600)) return;
    motion.stand(); face.surprised();
    if (!motion.holdMs(700)) return;
    done();
  }

  void yes() {
    face.happy();
    if (!face.banner("YES!", nullptr, 400, 4)) return;
    static const Beat b[] = {
      { CENTER+35, CENTER-35, CENTER, CENTER, 14, 60 },
      { CENTER,    CENTER,    CENTER, CENTER, 14, 60 },
    };
    if (!motion.playBeats(b, 2, 4)) return;
    done();
  }

  void no() {
    face.angry();
    if (!face.banner("NO!", nullptr, 400, 4)) return;
    static const Beat b[] = {
      { CENTER+25, CENTER+25, CENTER-25, CENTER-25, 12, 60 },
      { CENTER-25, CENTER-25, CENTER+25, CENTER+25, 12, 60 },
    };
    if (!motion.playBeats(b, 2, 4)) return;
    motion.stand(); done();
  }

  void sad() {
    face.tired();
    face.setOverlay(OV_TEAR);
    face.gaze(POS_S);
    motion.setTargets(CENTER+30, CENTER-30, CENTER+30, CENTER-30);
    if (!motion.moveSteps(45)) return;
    if (!motion.holdMs(1200)) return;
    face.gaze(POS_CENTER);
    motion.stand(); face.happy();
    if (!motion.holdMs(400)) return;
    done();
  }

  void scared() {
    face.surprised();
    face.setOverlay(OV_SWEAT, 1200);
    if (!motion.gaitFor(Gaits::FWD, 500, 1.0f, true)) return;   // retreat!
    motion.stand(); face.suspicious();
    if (!motion.holdMs(900)) return;
    done();
  }

  // ---------------- NEW tricks ----------------

  // PLAY DEAD: dramatic wobble, flop completely flat, then pop back up
  void playDead() {
    face.surprised();
    face.setOverlay(OV_EXCLAIM, 800);
    static const Beat wobble[] = {
      { CENTER+20, CENTER-20, CENTER+20, CENTER-20, 8, 40 },
      { CENTER-20, CENTER+20, CENTER-20, CENTER+20, 8, 40 },
    };
    if (!motion.playBeats(wobble, 2, 3)) return;
    motion.setTargets(CENTER-STRETCH, CENTER+STRETCH,
                      CENTER+STRETCH, CENTER-STRETCH);               // FLOP
    if (!motion.moveSteps(12)) return;
    face.eyesShut();
    if (!face.banner("X   X", nullptr, 2200, 3)) return;             // very dead
    if (!face.banner("BOO!", nullptr, 400, 4)) return;               // alive!
    face.eyesOpen(42);
    face.setOverlay(OV_EXCLAIM, 600);
    motion.stand(); face.happy(); face.laughBurst(); done();
  }

  // SNEEZE: ah... ah... ACHOO! whole body snaps forward, then confusion
  void sneeze() {
    face.nervous();
    static const Beat buildup[] = {
      { CENTER+12, CENTER-12, CENTER-6,  CENTER+6,  14, 250 },
      { CENTER+24, CENTER-24, CENTER-12, CENTER+12, 14, 300 },
    };
    if (!motion.playBeats(buildup, 2, 1)) return;
    if (!face.banner("ACHOO!", nullptr, 350, 2)) return;
    motion.setTargets(CENTER-45, CENTER+45, CENTER+30, CENTER-30);   // snap!
    if (!motion.moveSteps(4)) return;
    static const Beat shiver[] = {
      { CENTER-15, CENTER+15, CENTER+15, CENTER-15, 4, 30 },
      { CENTER+15, CENTER-15, CENTER-15, CENTER+15, 4, 30 },
    };
    if (!motion.playBeats(shiver, 2, 3)) return;
    motion.stand(); face.confusedBurst(); done();
  }

  // KISS: sit up cutely, lean in, MUAH! with hearts
  void kiss() {
    face.cute();
    motion.setTargets(CENTER, CENTER, CENTER-30, CENTER+30);
    if (!motion.moveSteps(25)) return;
    face.love();
    if (!motion.holdMs(500)) return;
    motion.setTargets(CENTER+30, CENTER-30, CENTER-30, CENTER+30);   // lean in
    if (!motion.moveSteps(18)) return;
    if (!face.banner("MUAH!", nullptr, 600, 3)) return;
    face.love();
    if (!motion.holdMs(900)) return;
    motion.stand(); face.laughBurst(); done();
  }

  // ZOOMIES: sudden burst of chaotic energy — sprints and spins
  void zoomies() {
    face.surprised();
    if (!face.banner("ZOOM", "ZOOM!", 450, 3, 3)) return;
    face.dancing();
    if (!motion.gaitFor(Gaits::FWD,   900, 1.8f)) return;
    if (!motion.gaitFor(Gaits::SPINL, 700, 1.8f)) return;
    if (!motion.gaitFor(Gaits::FWD,   700, 1.7f, true)) return;   // backward!
    if (!motion.gaitFor(Gaits::SPINR, 700, 1.9f)) return;
    if (!motion.gaitFor(Gaits::FWD,   900, 2.0f)) return;
    motion.stand();
    face.confusedBurst();
    if (!motion.holdMs(500)) return;
    done();
  }

  // TANTRUM: furious fast stomping, then throws himself flat in a huff
  void tantrum() {
    face.furious();
    static const Beat stomps[] = {
      { CENTER+35, CENTER,    CENTER,    CENTER-35, 5, 30 },
      { CENTER,    CENTER-35, CENTER+35, CENTER,    5, 30 },
    };
    if (!motion.playBeats(stomps, 2, 8)) return;
    motion.setTargets(CENTER-60, CENTER+60, CENTER+60, CENTER-60);   // hmph!
    if (!motion.moveSteps(9)) return;
    if (!face.banner("HMPH!", nullptr, 700, 3)) return;
    face.grumpy();
    if (!motion.holdMs(1200)) return;
    motion.stand(); face.suspicious();
    if (!motion.holdMs(600)) return;
    done();
  }

  // SULK: grumpy — turns his back on you, glances over his shoulder
  void sulk() {
    face.grumpy();
    if (!motion.gaitFor(Gaits::SPINL, 800)) return;                  // turn away
    motion.stand();
    motion.setTargets(CENTER, CENTER, CENTER-40, CENTER+40);         // sit
    if (!motion.moveSteps(30)) return;
    face.gaze(POS_E);                                                // side-eye
    if (!motion.holdMs(900)) return;
    face.grumpy();
    if (!motion.holdMs(900)) return;
    motion.setTargets(CENTER+15, CENTER-15, CENTER-40, CENTER+40);   // "hmph"
    if (!motion.moveSteps(6)) return;
    motion.setTargets(CENTER, CENTER, CENTER-40, CENTER+40);
    if (!motion.moveSteps(6)) return;
    if (!motion.holdMs(700)) return;
    motion.stand(); done();
  }

  // SHY: hides his face behind his front legs, peeks out, melts
  void shy() {
    face.cute();
    face.gaze(POS_SW);
    motion.setTargets(CENTER-70, CENTER+70, CENTER-15, CENTER+15);   // hide
    if (!motion.moveSteps(25)) return;
    face.eyesShut();
    if (!motion.holdMs(800)) return;
    face.eyesOpen(20); face.gaze(POS_NE);                            // peek...
    if (!motion.holdMs(600)) return;
    face.eyesShut();
    if (!motion.holdMs(500)) return;
    face.eyesOpen(EYE_H); face.gaze(POS_CENTER);
    face.setOverlay(OV_HEART, 1000);
    motion.stand(); face.cute(); done();
  }

  // ---------------- dances ----------------
  void happyDance() {
    face.love();
    static const Beat b[] = {
      { CENTER+25, CENTER-25, CENTER-25, CENTER+25, 10, 70 },
      { CENTER-25, CENTER+25, CENTER+25, CENTER-25, 10, 70 },
    };
    if (!motion.playBeats(b, 2, 4)) return;
    motion.stand(); face.laughBurst(); done();
  }

  void moonwalk() {
    face.dancing();
    static const Beat b[] = {
      { CENTER-20, CENTER+35, CENTER-35, CENTER+20, 22, 40 },
      { CENTER-35, CENTER+20, CENTER-20, CENTER+35, 22, 40 },
    };
    if (!motion.playBeats(b, 2, 5)) return;
    static const float glide[] = {0.8f, 0.8f, 0.8f, 0.8f};
    if (!motion.gaitFor(glide, 900, 1.0f, true)) return;   // backward slide
    motion.setTargets(CENTER-30, CENTER+30, CENTER, CENTER);
    if (!motion.moveSteps(15)) return;
    if (!motion.holdMs(700)) return;
    motion.stand(); done();
  }

  void chaCha() {
    face.dancing();
    static const Beat b[] = {
      { CENTER+22, CENTER+22, CENTER-10, CENTER-10, 8,  60 },
      { CENTER-22, CENTER-22, CENTER+10, CENTER+10, 8,  60 },
      { CENTER+30, CENTER+30, CENTER-18, CENTER-18, 26, 250 },
      { CENTER-30, CENTER-30, CENTER+18, CENTER+18, 26, 250 },
    };
    if (!motion.playBeats(b, 4, 3)) return;
    motion.stand(); face.laughBurst(); done();
  }

  void headbang() {
    face.dancing();
    roboEyes.setMood(ANGRY);
    static const Beat b[] = {
      { CENTER+42, CENTER-42, CENTER-8, CENTER+8, 7, 50 },
      { CENTER-12, CENTER+12, CENTER-8, CENTER+8, 7, 50 },
    };
    if (!motion.playBeats(b, 2, 8)) return;
    motion.stand(); face.laughBurst(); done();
  }

  void stomp() {
    face.dancing();
    for (int round = 0; round < 3; round++) {
      int spd = 16 - round * 4;
      for (int leg = 0; leg < NUM_SERVOS; leg++) {
        motion.sync();
        float lift = (leg == 0 || leg == 2) ? -45 : +45;
        motion.targetPos[leg] = CENTER + lift;
        if (!motion.moveSteps(spd)) return;
        motion.targetPos[leg] = CENTER;
        if (!motion.moveSteps(spd)) return;
      }
    }
    motion.stand(); done();
  }

  void salute() {
    face.normal();
    motion.setTargets(CENTER, CENTER, CENTER-40, CENTER+40);
    if (!motion.moveSteps(25)) return;
    motion.targetPos[1] = CENTER + 75;
    if (!motion.moveSteps(18)) return;
    face.happy();
    if (!motion.holdMs(1200)) return;
    motion.targetPos[1] = CENTER;
    if (!motion.moveSteps(18)) return;
    motion.stand(); done();
  }

  void peekaboo() {
    face.cute();
    motion.setTargets(CENTER-70, CENTER+70, CENTER-20, CENTER+20);
    if (!motion.moveSteps(20)) return;
    face.eyesShut();
    if (!motion.holdMs(900)) return;
    motion.setTargets(CENTER, CENTER, CENTER, CENTER);
    if (!motion.moveSteps(6)) return;
    face.eyesOpen(42);
    face.setOverlay(OV_EXCLAIM, 700);
    face.laughBurst();
    motion.stand(); done();
  }

  void pounce() {
    face.suspicious();
    motion.setTargets(CENTER+35, CENTER-35, CENTER-25, CENTER+25);
    if (!motion.moveSteps(30)) return;
    static const Beat wig[] = {
      { CENTER+35, CENTER-35, CENTER-35, CENTER+15, 6, 40 },
      { CENTER+35, CENTER-35, CENTER-15, CENTER+35, 6, 40 },
    };
    if (!motion.playBeats(wig, 2, 4)) return;
    face.surprised();
    static const float fwd[] = {1.3f, 1.3f, 1.3f, 1.3f};
    if (!motion.gaitFor(fwd, 700, 1.6f)) return;
    motion.stand(); face.laughBurst(); done();
  }

  void twist() {
    face.dancing();
    static const Beat b[] = {
      { CENTER+30, CENTER+10, CENTER-10, CENTER-30, 12, 60 },
      { CENTER-10, CENTER-30, CENTER+30, CENTER+10, 12, 60 },
      { CENTER+20, CENTER+20, CENTER-20, CENTER-20, 10, 50 },
      { CENTER-20, CENTER-20, CENTER+20, CENTER+20, 10, 50 },
    };
    if (!motion.playBeats(b, 4, 4)) return;
    motion.stand(); done();
  }

  // ---------------- NEW dances ----------------

  // BREAKDANCE: freeze pose -> power spin -> freeze -> spin -> final freeze
  void breakdance() {
    face.dancing();
    motion.setTargets(CENTER+70, CENTER-20, CENTER-20, CENTER+20);   // freeze 1
    if (!motion.moveSteps(10)) return;
    if (!motion.holdMs(450)) return;
    if (!motion.gaitFor(Gaits::SPINR, 1400, 1.7f)) return;           // power spin
    motion.setTargets(CENTER-20, CENTER+70, CENTER+20, CENTER-20);   // freeze 2
    if (!motion.moveSteps(10)) return;
    if (!motion.holdMs(450)) return;
    if (!motion.gaitFor(Gaits::SPINL, 1400, 1.7f)) return;           // reverse spin
    motion.setTargets(CENTER+60, CENTER-60, CENTER-60, CENTER+60);   // final freeze
    if (!motion.moveSteps(7)) return;
    face.laughBurst();
    if (!motion.holdMs(800)) return;
    motion.stand(); done();
  }

  // ROBOT: stiff staccato moves, one joint at a time, deadpan stare
  void robotDance() {
    face.normal();
    roboEyes.setAutoblinker(OFF, 0, 0);
    roboEyes.setIdleMode(OFF, 0, 0);
    face.gaze(POS_W);
    static const Beat seq1[] = {
      { CENTER+40, CENTER,    CENTER,    CENTER,    4, 320 },
      { CENTER+40, CENTER-40, CENTER,    CENTER,    4, 320 },
      { CENTER+40, CENTER-40, CENTER+40, CENTER,    4, 320 },
      { CENTER+40, CENTER-40, CENTER+40, CENTER-40, 4, 320 },
      { CENTER,    CENTER,    CENTER,    CENTER,    4, 320 },
    };
    if (!motion.playBeats(seq1, 5, 1)) return;
    face.gaze(POS_E);
    static const Beat seq2[] = {
      { CENTER-40, CENTER,    CENTER,    CENTER,    4, 320 },
      { CENTER-40, CENTER+40, CENTER,    CENTER,    4, 320 },
      { CENTER-40, CENTER+40, CENTER-40, CENTER+40, 4, 320 },
      { CENTER,    CENTER,    CENTER,    CENTER,    4, 320 },
    };
    if (!motion.playBeats(seq2, 4, 1)) return;
    face.gaze(POS_CENTER);
    motion.stand(); face.laughBurst(); done();
  }

  // DISCO: point up left/right (Saturday Night Fever), hip sway, spin
  void disco() {
    face.dancing();
    static const Beat points[] = {
      { CENTER+75, CENTER-15, CENTER-15, CENTER+15, 12, 300 },
      { CENTER-15, CENTER+75, CENTER+15, CENTER-15, 12, 300 },
    };
    if (!motion.playBeats(points, 2, 4)) return;
    static const Beat sway[] = {
      { CENTER+15, CENTER+15, CENTER-25, CENTER-25, 10, 80 },
      { CENTER-15, CENTER-15, CENTER+25, CENTER+25, 10, 80 },
    };
    if (!motion.playBeats(sway, 2, 4)) return;
    if (!motion.gaitFor(Gaits::SPINL, 1000, 1.4f)) return;
    motion.stand(); face.laughBurst(); done();
  }

  // BALLET: rise on tiptoes, slow graceful spin, arabesque, curtsy
  void ballet() {
    face.cute();
    motion.setTargets(CENTER-30, CENTER+30, CENTER+30, CENTER-30);   // tiptoes
    if (!motion.moveSteps(35)) return;
    if (!motion.holdMs(500)) return;
    if (!motion.gaitFor(Gaits::SPINL, 2200, 0.6f)) return;           // slow spin
    motion.setTargets(CENTER-20, CENTER+20, CENTER+70, CENTER-10);   // arabesque
    if (!motion.moveSteps(30)) return;
    if (!motion.holdMs(900)) return;
    motion.setTargets(CENTER+45, CENTER-45, CENTER-10, CENTER+10);   // curtsy
    if (!motion.moveSteps(30)) return;
    slowBlink();
    if (!motion.holdMs(600)) return;
    motion.stand(); face.happy(); done();
  }

  // PARTY is a full text show on the OLED too: it opens with a flashing
  // "PARTY! TIME!" banner and announces each dance name on screen.
  void party() {
    face.dancing(); face.laughBurst();
    if (!face.bannerFlash("PARTY!", "TIME!", 2, 350, 120, 3, 2)) return;
    if (!face.banner("CHA-CHA",  nullptr, 500, 3)) return; chaCha();     if (seqAbort) return;
    if (!face.banner("BOUNCE",   nullptr, 500, 3)) return; bounce();     if (seqAbort) return;
    if (!face.banner("DISCO",    nullptr, 500, 3)) return; disco();      if (seqAbort) return;
    if (!face.banner("TWIST",    nullptr, 500, 3)) return; twist();      if (seqAbort) return;
    if (!face.banner("BREAK!",   nullptr, 500, 3)) return; breakdance(); if (seqAbort) return;
    if (!face.banner("MOONWALK", nullptr, 500, 2)) return; moonwalk();   if (seqAbort) return;
    if (!face.banner("SPIN!",    nullptr, 500, 3)) return; spinPose();   if (seqAbort) return;
    if (!face.banner("ROBOT",    nullptr, 500, 3)) return; robotDance(); if (seqAbort) return;
    if (!face.banner("HEADBANG", nullptr, 500, 2)) return; headbang();   if (seqAbort) return;
    if (!face.banner("BOW",      nullptr, 500, 3)) return; bow();        if (seqAbort) return;
    if (!face.bannerFlash("YEAY!", nullptr, 3, 300, 120, 3)) return;
    done();
  }

  // helpers shared by routines
  void slowBlink() {
    face.eyesShut();
    motion.holdMs(180);
    face.eyesOpen();
    motion.holdMs(180);
  }
  void done() { face.moodFace(); }
};
Choreo show;

// =====================================================================
// TEXT PERFORMANCES — the three requested shows
// ---------------------------------------------------------------------
// These extend Choreo via plain functions (kept separate so the show
// scripts read like scripts). Screen banners + synced movement.
// =====================================================================

// PERFORMANCE 1 — "5G GACOR"
// eyes intro -> spin move -> big "5G" on screen with "GACOR!" flashing,
// while Albert bounces on the beat.
void perfGacor() {
  // 1) eye intro: look around like "watch this..."
  face.normal();
  face.gaze(POS_W);  if (!motion.holdMs(350)) return;
  face.gaze(POS_E);  if (!motion.holdMs(350)) return;
  face.gaze(POS_CENTER);
  face.laughBurst();
  if (!motion.holdMs(400)) return;

  // 2) the move: quick spin + stomp finish
  face.dancing();
  if (!motion.gaitFor(Gaits::SPINR, 1200, 1.3f)) return;
  if (!motion.stand()) return;

  // 3) the reveal: TEXT STATE — eyes fully off, text owns the screen,
  //    body keeps bouncing underneath with no face overlap
  face.textOnly = true;
  if (!face.banner("5G", nullptr, 700, 4)) { face.textOnly = false; return; }
  for (int i = 0; i < 4; i++) {
    if (!face.banner("5G", "GACOR!", 350, 4, 2)) { face.textOnly = false; return; }
    motion.setTargets(CENTER+40, CENTER-40, CENTER+40, CENTER-40);
    if (!motion.moveSteps(7)) { face.textOnly = false; return; }
    motion.setTargets(CENTER, CENTER, CENTER, CENTER);
    if (!motion.moveSteps(7)) { face.textOnly = false; return; }
  }
  if (!face.bannerFlash("GACOR!", nullptr, 3, 300, 150, 3)) { face.textOnly = false; return; }
  face.textOnly = false;                 // text done -> face comes back

  motion.stand();
  face.happy(); face.laughBurst();
  face.moodFace();
}

// PERFORMANCE 2 — TEAM ROLL CALL
// flashes each member's name big on screen with a cha-cha step per
// name, the whole roll repeated 3x, ending on a group banner + bow.
void perfTeam() {
  static const char* names[] = { "GUNG", "MAYUN", "GABBY", "ABUI", "TIO" };
  const int nNames = 5;

  face.dancing();
  if (!motion.holdMs(300)) return;

  // TEXT STATE: names own the screen, cha-cha steps run underneath —
  // no eyes overlapping the text
  face.textOnly = true;
  for (int repeat = 0; repeat < 3; repeat++) {
    for (int i = 0; i < nNames; i++) {
      // name on screen
      if (!face.banner(names[i], nullptr, 450, 3)) { face.textOnly = false; return; }
      // one cha-cha step per name (alternating side)
      bool rightSide = (i % 2 == 0);
      motion.setTargets(
        CENTER + (rightSide ?  22 : -22),
        CENTER + (rightSide ?  22 : -22),
        CENTER + (rightSide ? -10 :  10),
        CENTER + (rightSide ? -10 :  10));
      if (!motion.moveSteps(8)) { face.textOnly = false; return; }
      motion.setTargets(CENTER, CENTER, CENTER, CENTER);
      if (!motion.moveSteps(8)) { face.textOnly = false; return; }
    }
  }

  // finale: everyone together + a bow
  if (!face.banner("Cacak Cang", "Nas C!", 900, 3, 2)) { face.textOnly = false; return; }
  face.textOnly = false;                 // text done -> face comes back
  motion.setTargets(CENTER+55, CENTER-55, CENTER-10, CENTER+10);   // bow
  if (!motion.moveSteps(40)) return;
  if (!motion.holdMs(800)) return;
  motion.stand();
  face.happy(); face.laughBurst();
  face.moodFace();
}

// PERFORMANCE 3 — INTRO "HI AKU ALBERT"
// eyes state -> "HI" -> "AKU" -> "ALBERT" -> wave -> back to eyes.
void perfIntro() {
  // start from a friendly eye state
  face.cute();
  if (!motion.holdMs(800)) return;
  face.eyesShut(); if (!motion.holdMs(150)) return;   // a wink beat
  face.eyesOpen(); if (!motion.holdMs(250)) return;

  // the words, one per beat, with a tiny body bob on each.
  // TEXT STATE: eyes off while the words show — no overlap.
  face.textOnly = true;
  const char* words[] = { "HI!", "I'M", "ALBERTO" };
  for (int i = 0; i < 3; i++) {
    if (!face.banner(words[i], nullptr, 650, i == 2 ? 3 : 4)) { face.textOnly = false; return; }
    motion.setTargets(CENTER+15, CENTER-15, CENTER+15, CENTER-15);
    if (!motion.moveSteps(8)) { face.textOnly = false; return; }
    motion.setTargets(CENTER, CENTER, CENTER, CENTER);
    if (!motion.moveSteps(8)) { face.textOnly = false; return; }
  }
  face.textOnly = false;                 // text done -> face comes back

  // wave hello with the front-right leg
  face.happy();
  motion.setTargets(CENTER, CENTER, CENTER-35, CENTER+35);
  if (!motion.moveSteps(20)) return;
  for (int i = 0; i < 3; i++) {
    motion.targetPos[1] = CENTER + 55; if (!motion.moveSteps(10)) return;
    motion.targetPos[1] = CENTER + 15; if (!motion.moveSteps(10)) return;
  }
  motion.stand();

  // back to living eyes
  face.laughBurst();
  face.moodFace();
}

// DEMO — guided tour of every expression
void demoExpressions() {
  face.normal();     if (!motion.holdMs(700)) return;
  face.happy();      if (!motion.holdMs(700)) return;
  face.laughBurst();
  face.angry();      if (!motion.holdMs(700)) return;
  face.grumpy();     if (!motion.holdMs(900)) return;
  face.furious();    if (!motion.holdMs(900)) return;
  face.pouty();      if (!motion.holdMs(900)) return;
  face.excited();    if (!motion.holdMs(900)) return;
  face.bored();      if (!motion.holdMs(900)) return;
  face.dizzy();      if (!motion.holdMs(1100)) return;
  face.proud();      if (!motion.holdMs(900)) return;
  face.tired();      if (!motion.holdMs(700)) return;
  face.surprised();  if (!motion.holdMs(900)) return;
  face.suspicious(); if (!motion.holdMs(700)) return;
  face.cute();       if (!motion.holdMs(700)) return;
  face.love();       if (!motion.holdMs(1000)) return;
  face.nervous();    if (!motion.holdMs(1000)) return;
  face.dancing();    if (!motion.holdMs(800)) return;
  face.confusedBurst();
  face.cyclops();    if (!motion.holdMs(900)) return;
  show.slowBlink();
  face.moodFace();
}

// =====================================================================
// CLASS TouchPad — TTP223 tap/hold language -> reactions
//   1 tap: attention   2 taps: random trick   3 taps: sleep
//   hold: pet          long hold: happy dance
// =====================================================================
void enterSleepGlobal();   // fwd decl

class TouchPad {
public:
  void begin() { if (TOUCH_ENABLED) pinMode(TOUCH_PIN, INPUT); }

  void poll() {
    if (!TOUCH_ENABLED) return;

    int raw = digitalRead(TOUCH_PIN);
    bool nowTouched = TTP223_ACTIVE_HIGH ? (raw == HIGH) : (raw == LOW);
    unsigned long now = millis();

    if (nowTouched && !_last) { _startMs = now; _holdHandled = false; }

    if (nowTouched && !_holdHandled) {
      unsigned long held = now - _startMs;
      if (held >= 2500)      { _holdHandled = true; longPet(); }
      else if (held >= 900)  { _holdHandled = true; pet(); }
    }

    if (!nowTouched && _last) {
      unsigned long held = now - _startMs;
      if (!_holdHandled && held < 400) {
        if (now - _lastTapMs < 500) _taps++;
        else _taps = 1;
        _lastTapMs = now;
      }
    }

    if (_taps > 0 && (now - _lastTapMs) > 550 && !nowTouched) {
      int t = _taps; _taps = 0;
      if      (t == 1) attention();
      else if (t == 2) randomTrick();
      else             { bleSend("zzz"); enterSleepGlobal(); }
    }

    _last = nowTouched;
  }

  // ---- reactions (public so the PET command can reuse pet()) ----
  void pet() {
    brain.lifetimePets++;
    brain.bonding += 1.5f;
    brain.interact(+18, 2);
    if (brain.happiness > 85 && brain.energy < 35) {
      face.love(); motion.holdMs(500);
      bleSend("zzz");
      enterSleepGlobal();
    } else {
      face.love();
      motion.holdMs(700);
      bleSend("<3");
      face.moodFace();
    }
    brain.save();
  }

  void longPet() {
    brain.lifetimePets++;
    brain.bonding += 3.0f;
    brain.interact(+25, 4);
    bleSend("happy!");
    motion.sync();
    show.happyDance();
    brain.save();
  }

  void poke() {
    brain.lifetimePokes++;
    brain.happiness -= 8; brain.boredom = 0; brain.clampAll();
    brain.lastInteraction = millis();
    if (brain.happiness < 18) {                       // poked too much: TANTRUM
      bleSend("GRRR!");
      motion.sync(); show.tantrum();
    } else if (brain.happiness < 35 || brain.bonding < 20) {
      bleSend("grr");
      motion.sync();
      if (random(0, 2) == 0) show.no(); else show.sulk();
    } else {
      bleSend("!");
      face.nervous();
      motion.sync(); show.scared();
    }
    brain.save();
  }

  void attention() {
    brain.interact(+6, 1);
    face.surprised(); motion.holdMs(300);
    face.moodFace();
  }

  void randomTrick() {
    brain.interact(+8, 2);
    motion.sync();
    switch (random(0, 13)) {
      case 0:  show.wave();       break;
      case 1:  show.bounce();     break;
      case 2:  show.yes();        break;
      case 3:  show.twist();      break;
      case 4:  show.spinPose();   break;
      case 5:  show.peekaboo();   break;
      case 6:  show.kiss();       break;
      case 7:  show.zoomies();    break;
      case 8:  show.playDead();   break;
      case 9:  show.sneeze();     break;
      case 10: show.disco();      break;
      case 11: show.shy();        break;
      default: show.happyDance(); break;
    }
  }

private:
  bool _last = false, _holdHandled = false;
  unsigned long _startMs = 0, _lastTapMs = 0;
  int _taps = 0;
};
TouchPad touchpad;

void pollTouchGlobal() { touchpad.poll(); }

// =====================================================================
// CLASS AutoPilot — activity-based autonomous life
//   CHILL / EXPLORE / PLAY / SEEK-ATTENTION / GROOM, weighted by mood,
//   plus micro-idle (weight shifts, glances) so he's never a statue.
// =====================================================================
enum Mode {
  MODE_REST, MODE_INFO, MODE_IDLE,
  MODE_WALK_FORWARD, MODE_WALK_BACKWARD,
  MODE_WALK_LEFT, MODE_WALK_RIGHT,
  MODE_SPIN_LEFT, MODE_SPIN_RIGHT,
  MODE_AUTO, MODE_SLEEP
};
Mode currentMode = MODE_REST;

class AutoPilot {
public:
  enum Activity { CHILL, EXPLORE, PLAY, SEEKATT, GROOM, ZOOM, SHOWOFF };
  static const int N_ACTIVITIES = 7;

  void start() { _locked = false; pickActivity(); }

  // AUTO with options: lock him into one activity — he keeps doing
  // actions from that activity only, until AUTO (free roam) or a
  // manual command changes things.
  void startLocked(Activity a) {
    _locked = true;
    _act = a;
    _nextAction = millis() + random(500, 1500);
    Serial.print("[auto] locked activity: ");
    Serial.println(activityName());
  }

  const char* activityName() {
    switch (_act) {
      case CHILL:   return "chill";
      case EXPLORE: return "explore";
      case PLAY:    return "play";
      case SEEKATT: return "seek-attention";
      case GROOM:   return "groom";
      case ZOOM:    return "zoomies!";
      case SHOWOFF: return "show-off";
    }
    return "?";
  }

  void tick() {
    microIdle();
    if (!_locked && millis() > _actUntil) pickActivity();
    if (millis() > _nextAction) doAction();
  }

  // per-second drive drift + sleep management (called from loop)
  void tickDrives() {
    unsigned long now = millis();
    if (now - _lastDrive < 1000) return;
    _lastDrive = now;

    if (now - brain.lastInteraction > 8000) brain.boredom += 0.6f;

    if (currentMode == MODE_SLEEP) brain.energy += 1.2f;
    else                           brain.energy -= 0.15f;

    float baseline = 50 + brain.bonding * 0.2f;
    if (brain.happiness > baseline)      brain.happiness -= 0.05f;
    else if (brain.happiness < baseline) brain.happiness += 0.05f;

    if (brain.boredom > 60) brain.happiness -= 0.2f;
    brain.clampAll();

    if (currentMode == MODE_AUTO && brain.energy < 18) enterSleepGlobal();

    if (currentMode == MODE_SLEEP && brain.energy > 85) {
      brain.energy = 90;
      Serial.println("Albert woke up refreshed!");
      bleSend("yawn");
      currentMode = MODE_AUTO;
      face.surprised(); motion.holdMs(400);
      motion.sync();
      show.stretch();
      seqAbort = false;
      pickActivity();
    }
  }

private:
  Activity _act = CHILL;
  bool _locked = false;
  unsigned long _actUntil = 0, _nextAction = 0, _nextMicro = 0, _lastDrive = 0;

  void pickActivity() {
    if (_locked) {   // locked: keep the same activity, just re-arm timers
      _actUntil   = millis() + 60000;
      _nextAction = millis() + random(800, 3500);
      return;
    }
    // WILDCARD: 30% of the time ignore mood entirely and pick a totally
    // random activity — keeps auto mode surprising instead of predictable.
    if (random(0, 100) < 30) {
      Activity prev = _act;
      do { _act = (Activity)random(0, N_ACTIVITIES); } while (_act == prev);
    } else {
      int w_chill   = 15;
      int w_explore = 12 + (int)(brain.boredom * 0.5f);
      int w_play    = (brain.energy > 40) ? 15 + (int)(brain.happiness * 0.5f) : 5;
      int w_seek    = 8 + (int)(brain.bonding * 0.4f) + (int)(brain.boredom * 0.3f);
      int w_groom   = (brain.energy < 45) ? 25 : 8;
      int w_zoom    = (brain.energy > 60) ? 18 : 3;
      int w_show    = (brain.happiness > 55) ? 15 : 4;

      int total = w_chill + w_explore + w_play + w_seek + w_groom + w_zoom + w_show;
      int roll  = random(0, total);

      if      ((roll -= w_chill)   < 0) _act = CHILL;
      else if ((roll -= w_explore) < 0) _act = EXPLORE;
      else if ((roll -= w_play)    < 0) _act = PLAY;
      else if ((roll -= w_seek)    < 0) _act = SEEKATT;
      else if ((roll -= w_groom)   < 0) _act = GROOM;
      else if ((roll -= w_zoom)    < 0) _act = ZOOM;
      else                              _act = SHOWOFF;
    }

    // randomized session lengths too — sometimes short bursts, sometimes long
    _actUntil   = millis() + random(8000, 30000);
    _nextAction = millis() + random(800, 3500);
    Serial.print("[auto] activity: ");
    Serial.println(activityName());
  }

  void microIdle() {
    if (millis() < _nextMicro) return;
    _nextMicro = millis() + random(2500, 6000);

    int p = random(0, 10);
    motion.sync();
    if (p < 4) {                                    // weight shift
      int dir = random(0, 2) ? 1 : -1;
      motion.setTargets(CENTER + 8*dir, CENTER + 8*dir,
                        CENTER - 6*dir, CENTER - 6*dir);
      motion.moveSteps(20);
      motion.setTargets(CENTER, CENTER, CENTER, CENTER);
      motion.moveSteps(20);
    } else if (p < 7) {                             // glance
      int dirs[] = { POS_E, POS_W, POS_NE, POS_NW, POS_N };
      face.gaze(dirs[random(0, 5)]);
      motion.holdMs(600);
      face.gaze(POS_CENTER);
    } else if (p < 9) {                             // random mood flash
      switch (random(0, 10)) {
        case 0: face.grumpy();    break;
        case 1: face.surprised(); break;
        case 2: face.cute();      break;
        case 3: face.suspicious();break;
        case 4: face.pouty();     break;
        case 5: face.excited();   break;
        case 6: face.bored();     break;
        case 7: face.dizzy();     break;
        case 8: face.proud();     break;
        default: face.love();     break;
      }
      motion.holdMs(random(900, 1800));
      face.moodFace();
    } else {                                        // slow blink
      show.slowBlink();
    }
    seqAbort = false;
  }

  void doAction() {
    _nextAction = millis() + random(3000, 7000);
    motion.sync();

    switch (_act) {
      case CHILL:
        switch (random(0, 7)) {
          case 0: scanAround();     break;
          case 1: show.yes();       break;
          case 2: show.slowBlink(); break;
          case 3: show.shy();       break;
          case 4: face.grumpy(); motion.holdMs(1400); face.moodFace(); break;
          case 5: show.sneeze();    break;
          default: motion.holdMs(800); break;
        }
        break;

      case EXPLORE: {
        int d = random(0, 4);
        const float* g = (d==0) ? Gaits::FWD : (d==1) ? Gaits::LEFT
                       : (d==2) ? Gaits::RIGHT : Gaits::SPINL;
        face.normal();
        motion.gaitFor(g, random(1200, 2600));
        motion.stand();
        scanAround();
        break;
      }

      case PLAY:
        switch (random(0, 16)) {
          case 0:  show.twist();      break;
          case 1:  show.bounce();     break;
          case 2:  show.chaCha();     break;
          case 3:  show.pounce();     break;
          case 4:  show.spinPose();   break;
          case 5:  show.moonwalk();   break;
          case 6:  show.peekaboo();   break;
          case 7:  show.stomp();      break;
          case 8:  show.zoomies();    break;
          case 9:  show.gallop();     break;
          case 10: show.breakdance(); break;
          case 11: show.disco();      break;
          case 12: show.robotDance(); break;
          case 13: show.headbang();   break;
          case 14: show.playDead();   break;
          default: show.happyDance(); break;
        }
        brain.energy -= 4;
        break;

      case SEEKATT:
        switch (random(0, 6)) {
          case 0:
            motion.gaitFor(Gaits::FWD, 1200);
            motion.stand();
            show.wave();
            break;
          case 1: show.beg();  break;
          case 2: show.kiss(); break;
          case 3: show.sulk(); break;   // guilt-trips you for attention
          case 4: show.ballet(); break;
          default: face.love(); motion.holdMs(1500); face.moodFace(); break;
        }
        break;

      case GROOM:
        switch (random(0, 3)) {
          case 0: show.stretch(); break;
          case 1: show.shake();   break;
          default:
            motion.sit(); show.slowBlink();
            motion.holdMs(1200); motion.stand();
            break;
        }
        break;

      case ZOOM:                              // sudden energy bursts
        switch (random(0, 3)) {
          case 0: show.zoomies(); break;
          case 1:
            face.dancing();
            motion.gaitFor(Gaits::FWD, random(1500, 3000), 1.6f);
            motion.stand();
            break;
          default: show.gallop(); break;
        }
        brain.energy -= 5;
        break;

      case SHOWOFF:                           // performs for an audience
        switch (random(0, 7)) {
          case 0: show.breakdance(); break;
          case 1: show.disco();      break;
          case 2: show.robotDance(); break;
          case 3: show.ballet();     break;
          case 4: show.salute();     break;
          case 5: show.pushups();    break;
          default: show.bow();       break;
        }
        brain.energy -= 5;
        break;
    }

    brain.energy -= 2; brain.happiness += 1.5f; brain.clampAll();
    seqAbort = false;
    face.moodFace();
  }

  void scanAround() {
    face.clearOverlay();
    roboEyes.setAutoblinker(OFF, 0, 0);
    roboEyes.setIdleMode(OFF, 0, 0);
    roboEyes.setMood(DEFAULT);
    int seq[] = { POS_E, POS_NE, POS_N, POS_NW, POS_W, POS_SW, POS_S, POS_SE, POS_CENTER };
    for (int i = 0; i < 9; i++) {
      roboEyes.setPosition(seq[i]);
      if (!motion.holdMs(220)) return;
    }
  }
};
AutoPilot pilot;

// ---------------------------------------------------------------------
// global helpers that classes forward-declared
// ---------------------------------------------------------------------
void sendStatus() {
  char buf[96];
  snprintf(buf, sizeof(buf), "H:%d E:%d B:%d Bond:%d pets:%lu age:%lu act:%s",
           (int)brain.happiness, (int)brain.energy, (int)brain.boredom,
           (int)brain.bonding, brain.lifetimePets, brain.bootCount,
           pilot.activityName());
  Serial.println(buf); bleSend(buf);
}

void enterSleepGlobal() {
  currentMode = MODE_SLEEP;
  motion.sync();
  motion.sploot();
  face.sleepy();
  Serial.println("Albert is napping...");
}

void enterRest() {
  currentMode = MODE_REST;
  motion.sync();
  motion.sploot();
  face.sleepy();
}

void showStatsScreen() {
  face.clearOverlay();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("ALBERT  age:");
  display.print(brain.bootCount);

  const char* labels[] = {"Hap", "Ene", "Bnd"};
  float vals[] = { brain.happiness, brain.energy, brain.bonding };
  for (int i = 0; i < 3; i++) {
    display.setCursor(0, 14 + i*12);
    display.print(labels[i]);
    display.drawRect(30, 14 + i*12, 90, 8, SSD1306_WHITE);
    display.fillRect(30, 14 + i*12, (int)(90 * vals[i] / 100.0f), 8, SSD1306_WHITE);
  }

  display.setCursor(0, 54);
  display.print("pets:");  display.print(brain.lifetimePets);
  display.print(" pokes:"); display.print(brain.lifetimePokes);
  display.display();
}

// =====================================================================
// BLE SERVER SETUP
// =====================================================================
#define BLE_DEVICE_NAME        "AlbertMini"
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected = true;
    Serial.println("BLE connected");
  }
  void onDisconnect(BLEServer* pServer) {
    bleConnected = false;
    Serial.println("BLE disconnected");
    pServer->startAdvertising();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String val = pCharacteristic->getValue().c_str();
    val.trim();
    if (val.length() > 0) pushCmd(val);   // queued — never overwritten
  }
};

void initBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic* pRx = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRx->setCallbacks(new RxCallbacks());
  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising as: " BLE_DEVICE_NAME);
}

// =====================================================================
// COMMAND HANDLER
// =====================================================================
void handleCommand(String input) {
  input.trim(); input.toUpperCase();
  Serial.println(input); bleSend(input.c_str());
  brain.lastInteraction = millis();
  brain.boredom = 0;
  seqAbort = false;
  face.textOnly = false;   // an interrupted show must never leave eyes off

  // ---- continuous movement ----
  if      (input == "WALK")  { currentMode = MODE_WALK_FORWARD;  face.moodFace(); motion.sync(); motion.stand(); }
  else if (input == "BACK")  { currentMode = MODE_WALK_BACKWARD; face.moodFace(); motion.sync(); motion.stand(); }
  else if (input == "LEFT")  { currentMode = MODE_WALK_LEFT;     face.moodFace(); }
  else if (input == "RIGHT") { currentMode = MODE_WALK_RIGHT;    face.moodFace(); }
  else if (input == "SL")    { currentMode = MODE_SPIN_LEFT;     face.moodFace(); }
  else if (input == "SR")    { currentMode = MODE_SPIN_RIGHT;    face.moodFace(); }
  else if (input == "STOP")  { currentMode = MODE_IDLE;          face.moodFace(); }

  // ---- poses ----
  else if (input == "UP")    { currentMode = MODE_IDLE; motion.sync(); motion.stand(); }
  else if (input == "DOWN")  { currentMode = MODE_IDLE; motion.sync(); motion.sploot(); }
  else if (input == "SIT")   { currentMode = MODE_IDLE; motion.sync(); motion.sit(); face.cute(); }
  else if (input == "LIE")   { currentMode = MODE_IDLE; motion.sync(); motion.lie(); face.tired(); }
  else if (input == "LAYDOWN") { currentMode = MODE_IDLE; motion.sync(); motion.laydown(); face.tired(); }
  else if (input == "REST")  { enterRest(); }
  else if (input == "SLEEP") { enterSleepGlobal(); }
  else if (input == "INFO")  { currentMode = MODE_INFO; motion.sync(); motion.sit(); face.cute(); }
  else if (input == "STATS") { currentMode = MODE_INFO; showStatsScreen(); }

  // ---- classic tricks ----
  else if (input == "PUSHUPS")  { currentMode = MODE_IDLE; motion.sync(); show.pushups();  }
  else if (input == "SWING")    { currentMode = MODE_IDLE; motion.sync(); show.swing();    }
  else if (input == "GALLOP")   { currentMode = MODE_IDLE; motion.sync(); show.gallop();   }
  else if (input == "BOUNCE")   { currentMode = MODE_IDLE; motion.sync(); show.bounce();   }
  else if (input == "STRETCH")  { currentMode = MODE_IDLE; motion.sync(); show.stretch();  }
  else if (input == "WAVE")     { currentMode = MODE_IDLE; motion.sync(); show.wave();     }
  else if (input == "SHAKE")    { currentMode = MODE_IDLE; motion.sync(); show.shake();    }
  else if (input == "WIGGLE")   { currentMode = MODE_IDLE; motion.sync(); show.wiggle();   }
  else if (input == "BOW")      { currentMode = MODE_IDLE; motion.sync(); show.bow();      }
  else if (input == "BEG")      { currentMode = MODE_IDLE; motion.sync(); show.beg();      }
  else if (input == "SPINPOSE") { currentMode = MODE_IDLE; motion.sync(); show.spinPose(); }
  else if (input == "YES")      { currentMode = MODE_IDLE; motion.sync(); show.yes();      }
  else if (input == "NO")       { currentMode = MODE_IDLE; motion.sync(); show.no();       }
  else if (input == "SAD")      { currentMode = MODE_IDLE; motion.sync(); show.sad();      }
  else if (input == "SCARED")   { currentMode = MODE_IDLE; motion.sync(); show.scared();   }
  else if (input == "PLAYDEAD") { currentMode = MODE_IDLE; motion.sync(); show.playDead(); }
  else if (input == "SNEEZE")   { currentMode = MODE_IDLE; motion.sync(); show.sneeze();   }
  else if (input == "KISS")     { currentMode = MODE_IDLE; motion.sync(); show.kiss();     }
  else if (input == "ZOOMIES")  { currentMode = MODE_IDLE; motion.sync(); show.zoomies();  }
  else if (input == "TANTRUM")  { currentMode = MODE_IDLE; motion.sync(); show.tantrum();  }
  else if (input == "SULK")     { currentMode = MODE_IDLE; motion.sync(); show.sulk();     }
  else if (input == "SHY")      { currentMode = MODE_IDLE; motion.sync(); show.shy();      }

  // ---- dances ----
  else if (input == "DANCE")    { currentMode = MODE_IDLE; motion.sync(); show.happyDance(); }
  else if (input == "MOONWALK") { currentMode = MODE_IDLE; motion.sync(); show.moonwalk();   }
  else if (input == "CHACHA")   { currentMode = MODE_IDLE; motion.sync(); show.chaCha();     }
  else if (input == "HEADBANG") { currentMode = MODE_IDLE; motion.sync(); show.headbang();   }
  else if (input == "STOMP")    { currentMode = MODE_IDLE; motion.sync(); show.stomp();      }
  else if (input == "SALUTE")   { currentMode = MODE_IDLE; motion.sync(); show.salute();     }
  else if (input == "PEEKABOO") { currentMode = MODE_IDLE; motion.sync(); show.peekaboo();   }
  else if (input == "POUNCE")   { currentMode = MODE_IDLE; motion.sync(); show.pounce();     }
  else if (input == "TWIST")    { currentMode = MODE_IDLE; motion.sync(); show.twist();      }
  else if (input == "PARTY")    { currentMode = MODE_IDLE; motion.sync(); show.party();      }
  else if (input == "BREAKDANCE") { currentMode = MODE_IDLE; motion.sync(); show.breakdance(); }
  else if (input == "ROBOT")    { currentMode = MODE_IDLE; motion.sync(); show.robotDance(); }
  else if (input == "DISCO")    { currentMode = MODE_IDLE; motion.sync(); show.disco();      }
  else if (input == "BALLET")   { currentMode = MODE_IDLE; motion.sync(); show.ballet();     }

  // ---- text performances ----
  else if (input == "GACOR")    { currentMode = MODE_IDLE; motion.sync(); perfGacor(); }
  else if (input == "TEAM")     { currentMode = MODE_IDLE; motion.sync(); perfTeam();  }
  else if (input == "INTRO")    { currentMode = MODE_IDLE; motion.sync(); perfIntro(); }
  else if (input == "DEMO")     { currentMode = MODE_IDLE; motion.sync(); demoExpressions(); }

  // ---- pure expressions ----
  else if (input == "HAPPY")     { currentMode = MODE_IDLE; brain.happiness = min(100.0f, brain.happiness + 20); face.happy(); }
  else if (input == "ANGRY")     { currentMode = MODE_IDLE; face.angry();      }
  else if (input == "GRUMPY")    { currentMode = MODE_IDLE; face.grumpy();     }
  else if (input == "FURIOUS")   { currentMode = MODE_IDLE; face.furious();    }
  else if (input == "POUT")      { currentMode = MODE_IDLE; face.pouty();      }
  else if (input == "EXCITED")   { currentMode = MODE_IDLE; face.excited();    }
  else if (input == "BORED")     { currentMode = MODE_IDLE; face.bored();      }
  else if (input == "DIZZY")     { currentMode = MODE_IDLE; face.dizzy();      }
  else if (input == "PROUD")     { currentMode = MODE_IDLE; face.proud();      }
  else if (input == "TIRED")     { currentMode = MODE_IDLE; face.tired();      }
  else if (input == "SLEEPY")    { currentMode = MODE_IDLE; face.sleepy();     }
  else if (input == "SURPRISED") { currentMode = MODE_IDLE; face.surprised();  }
  else if (input == "SUS")       { currentMode = MODE_IDLE; face.suspicious(); }
  else if (input == "CUTE")      { currentMode = MODE_IDLE; face.cute();       }
  else if (input == "LOVE")      { currentMode = MODE_IDLE; face.love();       }
  else if (input == "NERVOUS")   { currentMode = MODE_IDLE; face.nervous();    }
  else if (input == "CYCLOPS")   { currentMode = MODE_IDLE; face.cyclops();    }
  else if (input == "LAUGH")     { currentMode = MODE_IDLE; face.laughBurst(); motion.holdMs(600); face.moodFace(); }
  else if (input == "CONFUSED")  { currentMode = MODE_IDLE; face.confusedBurst(); motion.holdMs(600); face.moodFace(); }
  else if (input == "WINK")      { currentMode = MODE_IDLE; show.slowBlink(); face.moodFace(); }
  else if (input == "SCAN")      { currentMode = MODE_IDLE;
                                   int seq[] = { POS_E, POS_N, POS_W, POS_S, POS_CENTER };
                                   face.normal();
                                   for (int i = 0; i < 5; i++) { face.gaze(seq[i]); if (!motion.holdMs(250)) break; }
                                   face.moodFace(); }
  else if (input == "FACE")      { currentMode = MODE_IDLE; face.moodFace(); }

  // ---- gaze ----
  else if (input == "LOOKUP")    { currentMode = MODE_IDLE; face.gaze(POS_N); }
  else if (input == "LOOKDOWN")  { currentMode = MODE_IDLE; face.gaze(POS_S); }
  else if (input == "LOOKLEFT")  { currentMode = MODE_IDLE; face.gaze(POS_W); }
  else if (input == "LOOKRIGHT") { currentMode = MODE_IDLE; face.gaze(POS_E); }

  // ---- autonomy / system ----
  else if (input == "AUTO")   { currentMode = MODE_AUTO; face.moodFace(); pilot.start(); }
  else if (input.startsWith("AUTO ")) {           // AUTO with a locked activity
    String a = input.substring(5); a.trim();
    AutoPilot::Activity act = AutoPilot::CHILL;
    bool known = true;
    if      (a == "CHILL")   act = AutoPilot::CHILL;
    else if (a == "EXPLORE") act = AutoPilot::EXPLORE;
    else if (a == "PLAY")    act = AutoPilot::PLAY;
    else if (a == "SEEK")    act = AutoPilot::SEEKATT;
    else if (a == "GROOM")   act = AutoPilot::GROOM;
    else if (a == "ZOOM")    act = AutoPilot::ZOOM;
    else if (a == "SHOW")    act = AutoPilot::SHOWOFF;
    else known = false;
    if (known) { currentMode = MODE_AUTO; face.moodFace(); pilot.startLocked(act); }
    else       { bleSend("? unknown auto activity"); }
  }
  else if (input == "PET")    { touchpad.pet(); }
  else if (input == "STATUS") { sendStatus(); }
  else if (input == "SAVE")  { brain.save(); bleSend("saved"); }
  else if (input == "RESET") { brain.wipe(); bleSend("memory wiped"); }

  else { Serial.print("Unknown: "); Serial.println(input); bleSend("?"); }

  seqAbort = false;
}

// =====================================================================
// SETUP & LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  // BROWN-OUT MITIGATION: if the last reset was a brown-out, this boot
  // goes into SAFE MODE — extra-long settle, no greeting, no AUTO life.
  // Albert waits quietly for a command so the battery can recover.
  bool safeBoot = (esp_reset_reason() == ESP_RST_BROWNOUT);

  brain.load();
  touchpad.begin();

  Wire.begin(I2C_SDA, I2C_SCL);
  delay(250);
  face.begin();

  initBLE();
  motion.begin();   // servos settle to CENTER one at a time

  brain.lastInteraction = millis();

  if (safeBoot) {
    // brown-out recovery only: short settle, then gentle idle — face
    // alive, servos parked at center — until you send a command
    face.sleepy();
    motion.holdMs(2500);
    seqAbort = false;
    Serial.println("!! brown-out recovery: SAFE MODE (send any command)");
    bleSend("brownout: safe mode");
    face.nervous();
    currentMode = MODE_IDLE;
  } else {
    // wake-up greeting — FACE ONLY, still no big servo bursts
    face.surprised(); motion.holdMs(400);
    if (brain.bonding > 40) { face.love(); motion.holdMs(600); }
    else                    { face.happy(); }
    face.laughBurst();
    seqAbort = false;

    currentMode = MODE_AUTO;
    pilot.start();
    face.moodFace();
  }

  Serial.print("Albert v5 alive. Age: ");
  Serial.println(brain.bootCount);
  Serial.println("Shows: GACOR / TEAM / INTRO. STATUS shows his mind.");
}

void loop() {
  // inputs — commands handled the moment they arrive
  if (Serial.available() > 0) handleCommand(Serial.readStringUntil('\n'));
  while (cmdPending()) handleCommand(popCmd());
  touchpad.poll();

  // background life
  pilot.tickDrives();
  brain.maybeAutosave();

  switch (currentMode) {
    case MODE_REST:
    case MODE_SLEEP:
    case MODE_INFO:
      face.render();
      break;

    case MODE_AUTO:
      face.render();
      pilot.tick();
      break;
      
    case MODE_WALK_FORWARD:  motion.gaitTick(Gaits::FWD);   face.render(); break;
    case MODE_WALK_BACKWARD: motion.gaitTick(Gaits::FWD, 1.0f, true); face.render(); break;
    case MODE_WALK_LEFT:     motion.gaitTick(Gaits::LEFT);  face.render(); break;
    case MODE_WALK_RIGHT:    motion.gaitTick(Gaits::RIGHT); face.render(); break;
    case MODE_SPIN_LEFT:     motion.gaitTick(Gaits::SPINL); face.render(); break;
    case MODE_SPIN_RIGHT:    motion.gaitTick(Gaits::SPINR); face.render(); break;

    default:
      motion.relaxTick();
      face.render();
      break;
  }
}
