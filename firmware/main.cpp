/*
 * ttgo-chat — Chat Controller Firmware
 * TTGO T-Display ESP32  (135×240 ST7789)
 *
 * BTN1 (GPIO35, LEFT)  — hold → ptt_start, release → ptt_stop
 * BTN2 (GPIO0,  RIGHT) — single press → new_chat
 *
 * Serial protocol (115200 baud, newline-delimited JSON):
 *   TTGO→Pi:  {"event":"new_chat"}
 *              {"event":"ptt_start"}
 *              {"event":"ptt_stop"}
 *              {"event":"device_ready"}
 *   Pi→TTGO:  {"type":"chat_started","session_id":"...","name":"..."}
 *              {"type":"session_name","name":"..."}  (learned from 1st utterance)
 *              {"type":"chat_stats","messages":N,"tokens":T,"context_pct":P}
 *              {"type":"status","text":"...","busy":true}  (live STATUS card)
 *              {"type":"telemetry","cpu_temp":C,"battery":P}  (host metrics)
 *              {"type":"ack","text":"..."}   (short status banner)
 *              {"type":"error","text":"..."}
 *
 * Idle screen shows a SESSION card (name + N msg · Nk/M tok) when a chat is
 * active; "No active session." otherwise. After IDLE_TIMEOUT_MS of no activity
 * (any PTT / new_chat / stats) the stats screen falls back to idle.
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include "config.h"

// ── TFT ───────────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// ── Display state ─────────────────────────────────────────────────────────────
enum ScreenState {
    SCR_IDLE,       // Waiting — no active session
    SCR_STARTING,   // "STARTING NEW CHAT…" animation
    SCR_STATS,      // Chat stats: messages + context
    SCR_PTT,        // PTT overlay (during recording)
    SCR_NONE,       // Used to force first redraw
};
ScreenState screen    = SCR_IDLE;
ScreenState prevScr   = SCR_NONE;

// Chat stats
int  statsMessages    = 0;
int  statsTokens      = 0;
int  statsPct         = 0;
char statsSessionId[40] = "";
bool statsStale       = false;
unsigned long lastStatsMs = 0;

// Session identity (shown on the idle SESSION card)
bool sessionActive    = false;          // a chat exists on the host
char sessionName[28]  = "";             // friendly name; empty until 1st utterance
unsigned long lastActivityMs = 0;       // any PTT / new_chat / stats — drives auto-idle

// Ack banner
char   ackText[32]    = "";
unsigned long ackUntilMs = 0;

// Live STATUS card (chat screen): current phase/tool, persists until replaced.
// `statusBusy` drives a small animated spinner dot while the agent is working.
char   statusText[28] = "";
bool   statusBusy     = false;

// Host telemetry (Pi CPU temp + battery), -1 = unknown/not reported
int  teleCpuTemp      = -1;     // °C
int  teleBattery      = -1;     // %

// PTT waveform animation
unsigned long lastAnimMs  = 0;
int  wavePhase            = 0;

// "STARTING" dot animation
int  startingDots         = 0;
unsigned long lastDotMs   = 0;

// ── Serial input ──────────────────────────────────────────────────────────────
char   inBuf[SP_BUF_SIZE];
int    inLen = 0;

// ── Button state ──────────────────────────────────────────────────────────
volatile bool btn1Fired    = false;
volatile unsigned long btn1LastMs = 0;
volatile bool btn2Fired    = false;
volatile unsigned long btn2LastMs = 0;
unsigned long btn1PressMs = 0;      // BTN1 now acts as PTT (poll/release)
bool          btn1WasPtt   = false;
bool          btn1WasDown  = false;
bool          btn1RawLast  = false;
unsigned long btn1LastLogMs = 0;
unsigned long btn1DebounceMs = 0;
unsigned long btn1LastAcceptedMs = 0;

unsigned long btn2PressMs  = 0;      // BTN2 now acts as new_chat (press)
bool          btn2WasPtt   = false;  // kept for existing state reset paths
bool          btn2WasDown  = false;
bool          btn2RawLast  = false;
unsigned long btn2LastLogMs = 0;
unsigned long btn2DebounceMs = 0;

bool          newChatPending = false;
unsigned long  newChatPendingSinceMs = 0;
unsigned long  newChatPressAcceptedMs = 0;
unsigned long  newChatSeq = 0;
unsigned long  newChatLastAckSeq = 0;
bool          newChatPressSeen = false;
unsigned long bootMs = 0;
bool          bootDiagSent = false;
unsigned long startingSinceMs = 0;

// ── ISRs ──────────────────────────────────────────────────────────────────────
void IRAM_ATTR onBtn1() {
    unsigned long now = millis();
    if (now - btn1LastMs > BTN_DEBOUNCE_MS) {
        btn1LastMs = now;
        btn1Fired  = true;
    }
}

void IRAM_ATTR onBtn2() {
    unsigned long now = millis();
    if (now - btn2LastMs > BTN_DEBOUNCE_MS) {
        btn2LastMs = now;
        btn2Fired  = true;
    }
}

// ── Serial send ───────────────────────────────────────────────────────────────
void sendEvent(const char* name) {
    StaticJsonDocument<64> doc;
    doc["event"] = name;
    serializeJson(doc, Serial);
    Serial.println();
}

// ── Layout constants (portrait 135×240) ────────────────────────────────────────
#define HEADER_H     22         // Top header strip height
#define HINT_H       30         // Bottom button-hint bar height
#define MARGIN       3          // Side margin for cards (smaller = wider cards)

void drawHeader(const char* title) {
    // Floating header title, baseline-aligned near the top edge
    tft.setTextColor(COL_HEADER_TXT, COL_BG);
    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(title, MARGIN, HEADER_H / 2);
}

void drawBar(int x, int y, int w, int h, int pct, uint16_t col) {
    tft.fillRoundRect(x, y, w, h, h / 2, COL_BAR_BG);
    int fill = (w * pct) / 100;
    if (fill > 0) tft.fillRoundRect(x, y, fill < h ? h : fill, h, h / 2, col);
}

void drawConnDot(bool connected) {
    uint16_t col = connected ? COL_GREEN : COL_RED;
    tft.fillCircle(tft.width() - 12, HEADER_H / 2, 4, col);
}

// Rounded card panel with a subtle border. Returns nothing; caller fills content.
void drawCard(int x, int y, int w, int h) {
    tft.fillRoundRect(x, y, w, h, 6, COL_CARD_BG);
    tft.drawRoundRect(x, y, w, h, 6, COL_CARD_BORDER);
}

// Format a token count compactly: 2_400_000 → "2M", 5_400 → "5k", 850 → "850".
static void fmtTokens(int tokens, char* out, size_t n) {
    if (tokens >= 1000000)      snprintf(out, n, "%d.%dM", tokens / 1000000, (tokens % 1000000) / 100000);
    else if (tokens >= 1000)    snprintf(out, n, "%d.%dk", tokens / 1000, (tokens % 1000) / 100);
    else                        snprintf(out, n, "%d", tokens);
}

// Bottom hint bar: two zones aligned to the physical buttons below the screen.
// Left zone sits above GPIO35 (Talk), right zone above GPIO0 (New Chat).
void drawHintBar() {
    int w = tft.width();
    int h = tft.height();
    int y = h - HINT_H;
    int half = w / 2;

    tft.fillRect(0, y, w, HINT_H, COL_HINT_BG);
    tft.drawFastHLine(0, y, w, COL_CARD_BORDER);
    tft.drawFastVLine(half, y + 4, HINT_H - 8, COL_CARD_BORDER);

    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);

    // Two rows centered in the bar (y0=210, h=30, center=225): +10 / +20.
    // Left = TAP / NEW (GPIO0)
    tft.setTextColor(COL_HINT_NEW, COL_HINT_BG);
    tft.drawString("TAP", half / 2, y + 10);
    tft.setTextColor(COL_LABEL, COL_HINT_BG);
    tft.drawString("New chat", half / 2, y + 20);

    // Right = HOLD / TALK (GPIO35)
    tft.setTextColor(COL_HINT_TALK, COL_HINT_BG);
    tft.drawString("HOLD", half + half / 2, y + 10);
    tft.setTextColor(COL_LABEL, COL_HINT_BG);
    tft.drawString("Talk", half + half / 2, y + 20);
}

// ── Screen renderers ──────────────────────────────────────────────────────────
// Idle is a card dashboard, top → bottom: a telemetry card (battery + CPU temp),
// a smaller animation card, then a SESSION card (name + N msg · Nk/M tok, or
// "No active session."). The animation is centered in its card at GLYPH_CY and
// owns a clear box of half-size GLYPH_CLEAR.
#define IDLE_TELE_Y  24         // telemetry card top (just under header)
#define IDLE_TELE_H  40         // telemetry card height
#define IDLE_ANIM_Y  68         // animation card top
#define IDLE_ANIM_H  80         // animation card height
#define IDLE_SESS_Y  152        // session card top
#define IDLE_SESS_H  56         // session card height (152+56 = 208, 2px to hints)
#define GLYPH_CY     (IDLE_ANIM_Y + IDLE_ANIM_H / 2)   // = 108
// Fireflies fill the whole animation card interior (inside the border + pad).
#define ANIM_PAD     4          // inset from card edges for the clear box
#define GLYPH_HW     ((SCREEN_W - 2 * MARGIN) / 2 - ANIM_PAD)   // clear half-width
#define GLYPH_HH     (IDLE_ANIM_H / 2 - ANIM_PAD)               // clear half-height

// ── Idle animation ──────────────────────────────────────────────────────────
float idleT = 0.0f;     // generic phase, advances each tick

// Firefly swarm persistent state
float fireX[8], fireY[8], fireVX[8], fireVY[8];
bool  fireInit = false;

static inline void clearGlyph(int cx, int cy) {
    // Clear the whole animation-card interior so fireflies can roam the card.
    tft.fillRect(cx - GLYPH_HW, cy - GLYPH_HH,
                 2 * GLYPH_HW, 2 * GLYPH_HH, COL_CARD_BG);
}

// Telemetry card: BATTERY (left half) | CPU (right half). A metric that hasn't
// been reported (value < 0) shows a muted "--". Shared by the idle dashboard
// and the chat screen (any y, height h). `showBar` draws the slim charge bar
// under the battery value (idle only; chat screen is compact and omits it).
static void drawTelemetryCard(int y, int h, bool showBar) {
    int w  = tft.width();
    int x  = MARGIN;
    int cw = w - 2 * MARGIN;
    int half = x + cw / 2;
    int valY = y + h / 2 + 4;       // value baseline, vertically centered-ish

    drawCard(x, y, cw, h);
    tft.drawFastVLine(half, y + 8, h - 16, COL_CARD_BORDER);
    tft.setTextSize(1);

    // ── Battery (left half) ──────────────────────────────────────────────
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(COL_LABEL, COL_CARD_BG);
    tft.drawString("BATTERY", x + 10, y + 12);
    if (teleBattery >= 0) {
        uint16_t col = (teleBattery <= 15) ? COL_RED
                     : (teleBattery <= 40) ? COL_ORANGE : COL_GREEN;
        char b[8];
        snprintf(b, sizeof(b), "%d%%", teleBattery);
        tft.setTextColor(COL_VALUE, COL_CARD_BG);
        tft.setTextSize(2);
        tft.drawString(b, x + 10, valY);
        tft.setTextSize(1);
        if (showBar) drawBar(x + 10, y + h - 9, cw / 2 - 20, 4, teleBattery, col);
    } else {
        tft.setTextColor(COL_CARD_BORDER, COL_CARD_BG);
        tft.setTextSize(2);
        tft.drawString("--", x + 10, valY);
        tft.setTextSize(1);
    }

    // ── CPU temp (right half) ────────────────────────────────────────────
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(COL_LABEL, COL_CARD_BG);
    tft.drawString("CPU", half + 10, y + 12);
    if (teleCpuTemp >= 0) {
        uint16_t col = (teleCpuTemp >= 75) ? COL_RED
                     : (teleCpuTemp >= 60) ? COL_ORANGE : COL_VALUE;
        char t[12];
        snprintf(t, sizeof(t), "%dC", teleCpuTemp);
        tft.setTextColor(col, COL_CARD_BG);
        tft.setTextSize(2);
        tft.drawString(t, half + 10, valY);
        tft.setTextSize(1);
    } else {
        tft.setTextColor(COL_CARD_BORDER, COL_CARD_BG);
        tft.setTextSize(2);
        tft.drawString("--", half + 10, valY);
        tft.setTextSize(1);
    }
}

static inline void drawIdleTelemetry() { drawTelemetryCard(IDLE_TELE_Y, IDLE_TELE_H, true); }

// Firefly swarm drifting across the whole animation card. The swarm fills the
// card's rectangular interior (GLYPH_HW × GLYPH_HH around the center), bouncing
// off each wall independently so it covers corners too.
static void animFireflies(int cx, int cy) {
    // Keep the swarm a couple of px inside the clear box, which itself sits
    // ANIM_PAD inside the card — so a firefly (radius ≤3) never paints onto the
    // rounded border. We also re-stroke the border below as a safety net.
    int bx = GLYPH_HW - 5;
    int by = GLYPH_HH - 5;
    if (!fireInit) {
        for (int i = 0; i < 8; i++) {
            float a = i * 0.785f;
            fireX[i] = cx + (bx * 0.6f) * cosf(a);
            fireY[i] = cy + (by * 0.6f) * sinf(a);
            fireVX[i] = 1.1f * cosf(a * 2.3f); fireVY[i] = 0.9f * sinf(a * 1.7f);
        }
        fireInit = true;
    }
    clearGlyph(cx, cy);
    for (int i = 0; i < 8; i++) {
        fireX[i] += fireVX[i]; fireY[i] += fireVY[i];
        if (fireX[i] < cx - bx) { fireX[i] = cx - bx; fireVX[i] = -fireVX[i]; }
        if (fireX[i] > cx + bx) { fireX[i] = cx + bx; fireVX[i] = -fireVX[i]; }
        if (fireY[i] < cy - by) { fireY[i] = cy - by; fireVY[i] = -fireVY[i]; }
        if (fireY[i] > cy + by) { fireY[i] = cy + by; fireVY[i] = -fireVY[i]; }
        uint16_t c = (i & 1) ? COL_ACCENT : COL_CYAN;
        tft.fillCircle((int)fireX[i], (int)fireY[i], (i % 3 == 0) ? 3 : 2, c);
    }
    // Safety net: redraw the card's rounded border so any grazing pixel is
    // overwritten (the rect clear can't restore the curved corners otherwise).
    tft.drawRoundRect(MARGIN, IDLE_ANIM_Y, SCREEN_W - 2 * MARGIN, IDLE_ANIM_H,
                      6, COL_CARD_BORDER);
}

// SESSION card: pulsing blue dot + name + "N msg · Nk/M tok" when a chat is
// active, else a muted dot + "No active session." The dot pulses on the anim
// clock, so its small area is redrawn each tick (see renderIdle).
static void drawIdleSessionDot(int x, int y) {
    // Clear the dot's cell, then draw it at a radius that breathes with idleT.
    tft.fillRect(x - 6, y - 6, 13, 13, COL_CARD_BG);
    if (sessionActive) {
        int r = 3 + (int)roundf(1.5f * (0.5f + 0.5f * sinf(idleT * 1.6f)));  // 3..6
        tft.fillCircle(x, y, r, COL_ACCENT);
    } else {
        tft.fillCircle(x, y, 3, COL_CARD_BORDER);
    }
}

static void drawIdleSession() {
    int w  = tft.width();
    int x  = MARGIN;
    int cw = w - 2 * MARGIN;
    int y  = IDLE_SESS_Y;

    drawCard(x, y, cw, IDLE_SESS_H);
    int dotX = x + 14;
    int dotY = y + 18;
    int txtX = dotX + 12;
    tft.setTextSize(1);

    if (sessionActive) {
        // Name (or a placeholder until the first utterance names the session).
        const char* nm = sessionName[0] ? sessionName : "(naming…)";
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(COL_VALUE, COL_CARD_BG);
        // Truncate to the card width: ~6px per char at size 1.
        char line[28];
        int maxChars = (x + cw - 6 - txtX) / 6;
        if (maxChars > (int)sizeof(line) - 1) maxChars = sizeof(line) - 1;
        if ((int)strlen(nm) > maxChars) {
            int keep = maxChars - 1;
            if (keep < 1) keep = 1;
            snprintf(line, sizeof(line), "%.*s.", keep, nm);
        } else {
            snprintf(line, sizeof(line), "%s", nm);
        }
        tft.drawString(line, txtX, dotY);

        // Meta: "N msg · Nk/M tok"
        char tok[16];
        fmtTokens(statsTokens, tok, sizeof(tok));
        char meta[40];
        snprintf(meta, sizeof(meta), "%d msg  %s tok", statsMessages, tok);
        tft.setTextColor(COL_LABEL, COL_CARD_BG);
        tft.drawString(meta, x + 14, y + IDLE_SESS_H - 16);
    } else {
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(COL_LABEL, COL_CARD_BG);
        tft.drawString("Idle", txtX, dotY);
    }

    drawIdleSessionDot(dotX, dotY);
}

void renderIdle() {
    static int  lastTeleTemp = -2, lastTeleBatt = -2;
    static bool lastSessActive = false;
    static char lastSessName[28] = "\x01";   // impossible initial value
    static int  lastSessMsgs = -1, lastSessTokens = -1;
    int w = tft.width();
    int cx = w / 2;
    int cy = GLYPH_CY;

    // ── Static layer: draw once on entry ────────────────────────────────────
    if (screen != prevScr) {
        tft.fillScreen(COL_BG);
        drawHeader("TULPA");
        drawConnDot(true);

        // Animation card frame (content drawn each tick inside it)
        drawCard(MARGIN, IDLE_ANIM_Y, w - 2 * MARGIN, IDLE_ANIM_H);

        drawHintBar();
        fireInit = false;
        lastTeleTemp = -2; lastTeleBatt = -2;     // force telemetry redraw
        lastSessName[0] = '\x01';                 // force session-card redraw
        lastSessMsgs = -1; lastSessTokens = -1;
    }

    // Telemetry card — redraw only when a value changes
    if (teleCpuTemp != lastTeleTemp || teleBattery != lastTeleBatt) {
        lastTeleTemp = teleCpuTemp;
        lastTeleBatt = teleBattery;
        drawIdleTelemetry();
    }

    // Session card — redraw when active state, name, or counters change
    if (sessionActive != lastSessActive
        || strncmp(sessionName, lastSessName, sizeof(lastSessName)) != 0
        || statsMessages != lastSessMsgs || statsTokens != lastSessTokens) {
        lastSessActive = sessionActive;
        strncpy(lastSessName, sessionName, sizeof(lastSessName) - 1);
        lastSessName[sizeof(lastSessName) - 1] = '\0';
        lastSessMsgs = statsMessages;
        lastSessTokens = statsTokens;
        drawIdleSession();
    }

    // ── Animated layer ───────────────────────────────────────────────────────
    if (millis() - lastAnimMs > ANIMATION_TICK_MS) {
        lastAnimMs = millis();
        idleT += 0.20f;
        animFireflies(cx, cy);
        // Pulse the session dot in lockstep with the swarm.
        drawIdleSessionDot(MARGIN + 14, IDLE_SESS_Y + 18);
    }
}

void renderStarting() {
    static int lastDots = -1;
    int w = tft.width();
    int h = tft.height();
    int cy = (HEADER_H + (h - HINT_H)) / 2;

    if (screen != prevScr) {
        tft.fillScreen(COL_BG);
        drawHeader("NEW SESSION");
        drawConnDot(true);
        drawHintBar();
        lastDots = -1; // Force dots redraw
    }
    if (startingDots != lastDots || screen != prevScr) {
        lastDots = startingDots;

        // Spinner-ish dot row beneath the word
        tft.fillRect(0, cy - 24, w, 48, COL_BG);
        tft.setTextColor(COL_STARTING, COL_BG);
        tft.setTextSize(2);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Tulpa", w / 2, cy - 8);

        int active = startingDots % 4;       // 0..3
        int dotR = 3, gap = 14;
        int n = 3;
        int startX = w / 2 - ((n - 1) * gap) / 2;
        for (int i = 0; i < n; i++) {
            uint16_t c = (i < ((active == 0) ? 0 : active)) ? COL_STARTING : COL_CARD_BORDER;
            tft.fillCircle(startX + i * gap, cy + 18, dotR, c);
        }
    }
}

// One stat card: label (small, muted) on top, value (large, white) below.
static void drawStatCard(int x, int y, int w, int h, const char* label, const char* value) {
    drawCard(x, y, w, h);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(COL_LABEL, COL_CARD_BG);
    tft.setTextSize(1);
    tft.drawString(label, x + 10, y + 13);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(COL_VALUE, COL_CARD_BG);
    tft.setTextSize(2);
    tft.drawString(value, x + w - 10, y + h / 2 + 2);
}

// Two metrics side-by-side in one card (label on top, value below, each half).
// Each value renders at size 2, dropping to size 1 if it would overflow its
// half-column (e.g. a long "12.3k" token count).
static void drawDuoStatCard(int x, int y, int w, int h,
                            const char* lA, const char* vA,
                            const char* lB, const char* vB) {
    drawCard(x, y, w, h);
    int half = x + w / 2;
    int avail = w / 2 - 14;        // usable width per half (inset on both sides)
    tft.drawFastVLine(half, y + 8, h - 16, COL_CARD_BORDER);
    int valY = y + h / 2 + 6;

    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL, COL_CARD_BG);
    tft.drawString(lA, x + 10, y + 12);
    tft.drawString(lB, half + 10, y + 12);

    tft.setTextColor(COL_VALUE, COL_CARD_BG);
    tft.setTextSize(((int)strlen(vA) * 12 <= avail) ? 2 : 1);
    tft.drawString(vA, x + 10, valY);
    tft.setTextSize(((int)strlen(vB) * 12 <= avail) ? 2 : 1);
    tft.drawString(vB, half + 10, valY);
    tft.setTextSize(1);
}

// ── Chat-screen layout (header 22 → hints 210) ──────────────────────────────
#define CHAT_TELE_Y  24         // battery|cpu strip on top
#define CHAT_TELE_H  34
#define CHAT_DUO_Y   62         // msgs|tokens
#define CHAT_DUO_H   38
#define CHAT_CTX_Y   104        // context one-liner
#define CHAT_CTX_H   24
#define CHAT_STAT_Y  132        // STATUS card (dynamic, animated)
#define CHAT_STAT_H  76         // 132+76 = 208, 2px to hints

// STATUS card: label + current phase/tool text, plus an animated spinner dot
// while busy. The spinner area is redrawn each anim tick (see renderStats).
static void drawStatusSpinner(int cx, int cy) {
    tft.fillRect(cx - 7, cy - 7, 15, 15, COL_CARD_BG);
    if (statusBusy) {
        // 3 orbiting dots; phase advances with wavePhase.
        for (int i = 0; i < 3; i++) {
            float a = idleT * 1.4f + i * 2.094f;        // 120° apart
            int px = cx + (int)roundf(5 * cosf(a));
            int py = cy + (int)roundf(5 * sinf(a));
            uint16_t c = (i == (wavePhase % 3)) ? COL_ACCENT : COL_CARD_BORDER;
            tft.fillCircle(px, py, 2, c);
        }
    } else if (statusText[0]) {
        tft.fillCircle(cx, cy, 3, COL_GREEN);           // steady = done/idle
    }
}

static void drawStatusCard() {
    int x = MARGIN, cw = tft.width() - 2 * MARGIN;
    int y = CHAT_STAT_Y;
    drawCard(x, y, cw, CHAT_STAT_H);

    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL, COL_CARD_BG);
    tft.drawString("STATUS", x + 10, y + 13);

    // Phase/tool text, centered in the card body (size 2, may wrap to size 1).
    const char* s = statusText[0] ? statusText : "Ready";
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(statusBusy ? COL_ACCENT : COL_VALUE, COL_CARD_BG);
    // Pick size 2 if it fits (~10px/char), else size 1.
    int len = (int)strlen(s);
    tft.setTextSize((len * 12 <= cw - 16) ? 2 : 1);
    tft.drawString(s, x + cw / 2, y + CHAT_STAT_H / 2 + 4);
    tft.setTextSize(1);
}

void renderStats() {
    static int lastMsgs = -1;
    static int lastTokens = -1;
    static int lastPct = -1;
    static int lastTeleTemp = -2, lastTeleBatt = -2;
    static char lastStatus[28] = "\x01";
    static bool lastBusy = false;

    int w = tft.width();
    bool force = (screen != prevScr);

    int cardX = MARGIN;
    int cardW = w - 2 * MARGIN;

    if (force) {
        tft.fillScreen(COL_BG);
        drawHeader("SESSION");
        drawConnDot(true);
        drawHintBar();
        lastMsgs = -1; lastTokens = -1; lastPct = -1;
        lastTeleTemp = -2; lastTeleBatt = -2;
        lastStatus[0] = '\x01'; lastBusy = !statusBusy;
    }

    // Battery + CPU temp on top (compact, no charge bar) — redraw on change
    if (force || teleCpuTemp != lastTeleTemp || teleBattery != lastTeleBatt) {
        lastTeleTemp = teleCpuTemp;
        lastTeleBatt = teleBattery;
        drawTelemetryCard(CHAT_TELE_Y, CHAT_TELE_H, false);
    }

    // Messages + Tokens in one card — redraw when either changes
    if (force || statsMessages != lastMsgs || statsTokens != lastTokens) {
        lastMsgs = statsMessages;
        lastTokens = statsTokens;
        char msgBuf[12], tokBuf[24];
        snprintf(msgBuf, sizeof(msgBuf), "%d", statsMessages);
        fmtTokens(statsTokens, tokBuf, sizeof(tokBuf));
        drawDuoStatCard(cardX, CHAT_DUO_Y, cardW, CHAT_DUO_H, "MSGS", msgBuf, "TOKENS", tokBuf);
    }

    // Context one-liner: "CTX   12.3k/200k"
    int pct = statsPct < 0 ? 0 : (statsPct > 100 ? 100 : statsPct);
    if (force || pct != lastPct) {
        lastPct = pct;
        drawCard(cardX, CHAT_CTX_Y, cardW, CHAT_CTX_H);
        tft.setTextDatum(ML_DATUM);
        tft.setTextSize(1);
        tft.setTextColor(COL_LABEL, COL_CARD_BG);
        tft.drawString("CTX", cardX + 10, CHAT_CTX_Y + CHAT_CTX_H / 2);
        char used[16], win[16], ctxBuf[40];
        fmtTokens(statsTokens, used, sizeof(used));
        fmtTokens(CTX_WINDOW_TOKENS, win, sizeof(win));
        snprintf(ctxBuf, sizeof(ctxBuf), "%s/%s  %d%%", used, win, pct);
        uint16_t col = (pct >= 85) ? COL_RED : (pct >= 60 ? COL_ORANGE : COL_VALUE);
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(col, COL_CARD_BG);
        tft.drawString(ctxBuf, cardX + cardW - 10, CHAT_CTX_Y + CHAT_CTX_H / 2);
    }

    // STATUS card — redraw text when status/busy changes
    if (force || strncmp(statusText, lastStatus, sizeof(lastStatus)) != 0
        || statusBusy != lastBusy) {
        strncpy(lastStatus, statusText, sizeof(lastStatus) - 1);
        lastStatus[sizeof(lastStatus) - 1] = '\0';
        lastBusy = statusBusy;
        drawStatusCard();
    }

    // Animated spinner in the STATUS card
    if (millis() - lastAnimMs > ANIMATION_TICK_MS) {
        lastAnimMs = millis();
        idleT += 0.20f;
        wavePhase = (wavePhase + 1) % 8;
        drawStatusSpinner(MARGIN + cardW - 16, CHAT_STAT_Y + 14);
    }
}

void renderPtt() {
    int w = tft.width();
    int h = tft.height();
    int contentBottom = h - HINT_H;

    if (screen != prevScr) {
        tft.fillScreen(COL_PTT_BG);
        drawHeader("LISTENING");
        drawConnDot(true);

        // Mic icon near the top of the content area
        int cx = w / 2;
        int cy = HEADER_H + 36;
        tft.fillRoundRect(cx - 8, cy - 15, 16, 25, 8, COL_PTT_WAVE);
        tft.drawCircle(cx, cy, 18, COL_PTT_WAVE);
        tft.drawFastVLine(cx, cy + 18, 5, COL_PTT_WAVE);
        tft.drawFastHLine(cx - 10, cy + 23, 20, COL_PTT_WAVE);

        tft.setTextColor(COL_PTT_WAVE, COL_PTT_BG);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Release to send", w / 2, contentBottom - 14);
    }

    // Animated waveform bars, centered in the lower content area
    if (millis() - lastAnimMs > ANIMATION_TICK_MS) {
        lastAnimMs = millis();
        wavePhase  = (wavePhase + 1) % 8;
        const int heights[] = {8, 16, 28, 40, 28, 16, 8, 6};
        int numBars = 7;                       // fewer bars — narrow portrait
        int barW = 10, gap = 6;
        int totalW = numBars * barW + (numBars - 1) * gap;
        int startX = (w - totalW) / 2;
        int baseY  = (HEADER_H + 72 + contentBottom - 30) / 2;

        tft.fillRect(0, baseY - 24, w, 48, COL_PTT_BG);

        for (int i = 0; i < numBars; i++) {
            int currentH = heights[(wavePhase + i) % 8];
            int x = startX + i * (barW + gap);
            tft.fillRoundRect(x, baseY - currentH / 2, barW, currentH, 3, COL_PTT_WAVE);
        }
    }
}

// ── Tick: decide what to draw ──────────────────────────────────────────────────
void displayTick() {
    // Stale check
    if (screen == SCR_STATS && lastStatsMs > 0) {
        statsStale = (millis() - lastStatsMs > STALE_DATA_MS);
    }

    // Auto-return to idle after IDLE_TIMEOUT_MS of no activity (any PTT /
    // new_chat / incoming stats). The session stays active and is shown on the
    // idle SESSION card. Never times out while actively talking or starting.
    if (screen == SCR_STATS && lastActivityMs > 0
        && millis() - lastActivityMs > IDLE_TIMEOUT_MS) {
        screen  = SCR_IDLE;
        prevScr = SCR_STATS;
    }

    // Starting animation dot update
    if (screen == SCR_STARTING) {
        if (startingSinceMs > 0 && millis() - startingSinceMs > STARTING_TIMEOUT_MS) {
            screen = SCR_IDLE;
            ackText[0] = '\0';
            ackUntilMs = 0;
            statsStale = false;
            prevScr = SCR_STARTING;
            startingSinceMs = 0;
        } else if (millis() - lastDotMs > 400) {
            lastDotMs = millis();
            startingDots++;
        }
    }

    // Render
    switch (screen) {
        case SCR_IDLE:     renderIdle();     break;
        case SCR_STARTING: renderStarting(); break;
        case SCR_STATS:    renderStats();    break;
        case SCR_PTT:      renderPtt();      break;
    }
    prevScr = screen;
}

// ── Serial JSON parser ────────────────────────────────────────────────────────
void handleMessage(const char* line) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;

    const char* type = doc["type"];
    if (!type) return;

    if (strcmp(type, "chat_started") == 0) {
        // New session confirmed
        const char* sid = doc["session_id"] | "";
        strncpy(statsSessionId, sid, sizeof(statsSessionId) - 1);
        const char* nm = doc["name"] | "";          // usually empty at start
        strncpy(sessionName, nm, sizeof(sessionName) - 1);
        sessionName[sizeof(sessionName) - 1] = '\0';
        sessionActive = true;
        statsMessages = 0;
        statsTokens   = 0;
        statsPct      = 0;
        lastStatsMs   = millis();
        lastActivityMs = millis();
        statsStale    = false;
        startingSinceMs = 0;
        newChatPending = false;
        newChatLastAckSeq = newChatSeq;
        screen        = SCR_STATS;
        btn1Fired = false;
        btn1LastAcceptedMs = millis();
        ackText[0] = '\0';
        ackUntilMs = 0;
        statusText[0] = '\0';
        statusBusy = false;

    } else if (strcmp(type, "session_name") == 0) {
        // Name learned from the first utterance — refresh the SESSION card.
        const char* nm = doc["name"] | "";
        strncpy(sessionName, nm, sizeof(sessionName) - 1);
        sessionName[sizeof(sessionName) - 1] = '\0';
        sessionActive = true;

    } else if (strcmp(type, "chat_stats") == 0) {
        statsMessages = doc["messages"] | statsMessages;
        statsTokens   = doc["tokens"]   | statsTokens;
        statsPct      = doc["context_pct"] | statsPct;
        lastStatsMs   = millis();
        lastActivityMs = millis();
        statsStale    = false;
        sessionActive = true;
        if (screen != SCR_PTT) screen = SCR_STATS;

    } else if (strcmp(type, "telemetry") == 0) {
        // Host CPU temp / battery for the idle status strip.
        // Missing fields keep their previous value (-1 = unknown).
        teleCpuTemp = doc["cpu_temp"] | teleCpuTemp;
        teleBattery = doc["battery"]  | teleBattery;

    } else if (strcmp(type, "status") == 0) {
        // Live phase/tool for the STATUS card — persists until the next status.
        const char* txt = doc["text"] | "";
        strncpy(statusText, txt, sizeof(statusText) - 1);
        statusText[sizeof(statusText) - 1] = '\0';
        statusBusy = doc["busy"] | false;
        lastActivityMs = millis();

    } else if (strcmp(type, "ack") == 0) {
        const char* txt = doc["text"] | "";
        strncpy(ackText, txt, sizeof(ackText) - 1);
        ackUntilMs = millis() + 3000;

    } else if (strcmp(type, "error") == 0) {
        const char* txt = doc["text"] | "error";
        strncpy(ackText, txt, sizeof(ackText) - 1);
        ackUntilMs = millis() + 4000;
        sessionActive = false;
        sessionName[0] = '\0';
        statusText[0] = '\0';
        statusBusy = false;
        screen = SCR_IDLE;
    }
}

void pollSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (inLen > 0) {
                inBuf[inLen] = '\0';
                handleMessage(inBuf);
                inLen = 0;
            }
        } else if (inLen < SP_BUF_SIZE - 1) {
            inBuf[inLen++] = c;
        }
    }
}

// ── setup / loop ──────────────────────────────────────────────────────────────
void setup() {
    bootMs = millis();
    Serial.begin(SERIAL_BAUD_RATE);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(COL_BG);

    // ── Splash (portrait) ─────────────────────────────────────────────────────
    {
        int w  = tft.width();
        int h  = tft.height();
        int cx = w / 2;
        int cy = h / 2 - 20;

        // Winged-helmet nod: a hexagon emblem with a cyan core, drawn ring-by-ring
        for (int r = 34; r >= 22; r -= 4) {
            uint16_t c = (r <= 22) ? COL_ACCENT : COL_CARD_BORDER;
            tft.drawCircle(cx, cy, r, c);
        }
        // Small "wing" ticks flanking the emblem
        tft.drawFastHLine(cx - 50, cy, 12, COL_ACCENT);
        tft.drawFastHLine(cx + 38, cy, 12, COL_ACCENT);
        tft.fillTriangle(cx - 7, cy - 9, cx - 7, cy + 9, cx + 9, cy, COL_ACCENT);

        // Wordmark
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(COL_CYAN, COL_BG);
        tft.setTextSize(2);
        tft.drawString("Tulpa", cx, cy + 56);
        tft.setTextSize(1);
        tft.setTextColor(COL_LABEL, COL_BG);
        tft.drawString("Chat Controller", cx, cy + 78);

        // Brief loading underline that fills left-to-right
        int barW = w - 40, barX = 20, barY = cy + 96;
        tft.drawRoundRect(barX, barY, barW, 6, 3, COL_CARD_BORDER);
        for (int f = 0; f <= barW - 2; f += 6) {
            tft.fillRoundRect(barX + 1, barY + 1, f, 4, 2, COL_ACCENT);
            delay(18);
        }
    }
    delay(250);

    pinMode(BTN1_PIN, INPUT);           // GPIO35 — input only, no pull-up
    pinMode(BTN2_PIN, INPUT_PULLUP);    // GPIO0  — has internal pull-up

    attachInterrupt(digitalPinToInterrupt(BTN1_PIN), onBtn1, FALLING);
    attachInterrupt(digitalPinToInterrupt(BTN2_PIN), onBtn2, FALLING);

    btn2RawLast = digitalRead(BTN2_PIN);
    btn2WasDown = (btn2RawLast == LOW);
    btn2WasPtt = false;
    btn2PressMs = 0;

    Serial.print("{\"debug\":\"boot_diag\",\"btn1\":");
    Serial.print(digitalRead(BTN1_PIN));
    Serial.print(",\"btn2\":");
    Serial.print(btn2RawLast ? 1 : 0);
    Serial.print(",\"gpio0_pullup\":1,\"uptime_ms\":");
    Serial.print((unsigned long)(millis() - bootMs));
    Serial.println("}");
    bootDiagSent = true;

    screen = SCR_IDLE;
    sendEvent("device_ready");
}

void loop() {
    pollSerial();

    // ── BTN1 — PTT hold/release (polled, debounced) ──────────────────────────
    bool btn1RawDown = (digitalRead(BTN1_PIN) == LOW);  // active-low

    if (btn1RawDown != btn1RawLast) {
        btn1DebounceMs = millis();
        btn1RawLast = btn1RawDown;
    }

    if ((millis() - btn1DebounceMs) >= BTN2_RAW_SETTLE_MS) {
        if (btn1RawDown) {
            if (!btn1WasDown) {
                btn1WasDown = true;
                btn1PressMs = millis();
                Serial.print("{\"debug\":\"btn1_pressed\",\"raw\":1,\"uptime_ms\":");
                Serial.print((unsigned long)(millis() - bootMs));
                Serial.println("}");
            }
            if (!btn1WasPtt && btn1PressMs > 0) {
                unsigned long holdTime = millis() - btn1PressMs;
                if (holdTime >= BTN2_HOLD_MS) {
                    btn1WasPtt = true;
                    screen = SCR_PTT;
                    prevScr = SCR_STATS;
                    lastAnimMs = 0;
                    lastActivityMs = millis();
                    Serial.print("{\"debug\":\"btn1_ptt_start\",\"hold_ms\":");
                    Serial.print(holdTime);
                    Serial.println("}");
                    sendEvent("ptt_start");
                }
            }
        } else if (btn1WasDown) {
            btn1WasDown = false;
            Serial.print("{\"debug\":\"btn1_released\",\"raw\":0,\"uptime_ms\":");
            Serial.print((unsigned long)(millis() - bootMs));
            Serial.println("}");
            if (btn1WasPtt) {
                btn1WasPtt = false;
                screen = (statsMessages > 0) ? SCR_STATS : SCR_IDLE;
                prevScr = SCR_PTT;
                lastActivityMs = millis();
                sendEvent("ptt_stop");
            }
            btn1PressMs = 0;
        }
    }

    if (btn1RawDown != btn1RawLast || (millis() - btn1LastLogMs) > BTN2_LOG_EVERY_MS) {
        btn1LastLogMs = millis();
        Serial.print("{\"debug\":\"btn1_state\",\"raw\":");
        Serial.print(btn1RawDown ? 1 : 0);
        Serial.print(",\"stable\":");
        Serial.print(btn1WasDown ? 1 : 0);
        Serial.print(",\"ptt\":");
        Serial.print(btn1WasPtt ? 1 : 0);
        Serial.print(",\"uptime_ms\":");
        Serial.print((unsigned long)(millis() - bootMs));
        Serial.println("}");
    }

    // ── BTN2 — new chat ───────────────────────────────────────────────────────
    if (btn2Fired) {
        btn2Fired = false;
        unsigned long now = millis();
        // Debounce against bounces/EMI: ignore fresh presses within 1s of last accepted one.
        bool cooldownOk = (newChatPressAcceptedMs == 0) || (now - newChatPressAcceptedMs) > 1000;
        if (!newChatPending && screen != SCR_STARTING && cooldownOk) {
            newChatPending = true;
            newChatPendingSinceMs = now;
            newChatPressAcceptedMs = now;
            newChatSeq++;
            screen    = SCR_STARTING;
            startingDots = 0;
            lastDotMs    = now;
            lastActivityMs = now;
            prevScr      = SCR_STATS; // force full redraw on next tick
            sendEvent("new_chat");
        }
    }

    if (newChatPending && (millis() - newChatPendingSinceMs) > STARTING_TIMEOUT_MS) {
        newChatPending = false;
        // If we already have an active session, fall back to stats, not idle
        screen = (statsMessages > 0) ? SCR_STATS : SCR_IDLE;
        prevScr = SCR_STARTING;
        startingSinceMs = 0;
    }

    displayTick();
    delay(30);
}
