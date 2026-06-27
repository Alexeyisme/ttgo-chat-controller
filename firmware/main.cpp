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
 *   Pi→TTGO:  {"type":"chat_started","session_id":"..."}
 *              {"type":"chat_stats","messages":N,"tokens":T,"context_pct":P}
 *              {"type":"ack","text":"..."}   (short status banner)
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

// Ack banner
char   ackText[32]    = "";
unsigned long ackUntilMs = 0;

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
#define MARGIN       8          // Side margin for cards

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

    // Left = TAP / NEW (GPIO0)
    tft.setTextColor(COL_HINT_NEW, COL_HINT_BG);
    tft.drawString("TAP", half / 2, y + 10);
    tft.setTextColor(COL_LABEL, COL_HINT_BG);
    tft.drawString("New chat", half / 2, y + 21);

    // Right = HOLD / TALK (GPIO35)
    tft.setTextColor(COL_HINT_TALK, COL_HINT_BG);
    tft.drawString("HOLD", half + half / 2, y + 10);
    tft.setTextColor(COL_LABEL, COL_HINT_BG);
    tft.drawString("Talk", half + half / 2, y + 21);
}

// ── Screen renderers ──────────────────────────────────────────────────────────
// Idle is a card dashboard: a telemetry card (battery + CPU temp) on top and an
// animation card below. The animation is centered in its card at GLYPH_CY and
// owns a clear box of half-size GLYPH_CLEAR.
#define IDLE_TELE_Y  28         // telemetry card top
#define IDLE_TELE_H  44         // telemetry card height
#define IDLE_ANIM_Y  80         // animation card top
#define IDLE_ANIM_H  124        // animation card height (down to just above hints)
#define GLYPH_CY     (IDLE_ANIM_Y + IDLE_ANIM_H / 2)   // = 142
#define GLYPH_CLEAR  50         // half-size of the square cleared each frame

// ── Idle animation ──────────────────────────────────────────────────────────
float idleT = 0.0f;     // generic phase, advances each tick

// Firefly swarm persistent state
float fireX[8], fireY[8], fireVX[8], fireVY[8];
bool  fireInit = false;

static inline void clearGlyph(int cx, int cy) {
    // Animation lives inside the animation card — clear to the card background.
    tft.fillRect(cx - GLYPH_CLEAR, cy - GLYPH_CLEAR,
                 2 * GLYPH_CLEAR, 2 * GLYPH_CLEAR, COL_CARD_BG);
}

// Telemetry card: BATTERY (left half, value + bar) | CPU (right half).
// A metric that hasn't been reported (value < 0) shows a muted "--".
static void drawIdleTelemetry() {
    int w  = tft.width();
    int x  = MARGIN;
    int cw = w - 2 * MARGIN;
    int y  = IDLE_TELE_Y;
    int half = x + cw / 2;

    drawCard(x, y, cw, IDLE_TELE_H);
    tft.drawFastVLine(half, y + 8, IDLE_TELE_H - 16, COL_CARD_BORDER);
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
        tft.drawString(b, x + 10, y + 30);
        tft.setTextSize(1);
        // slim charge bar under the value
        drawBar(x + 10, y + IDLE_TELE_H - 9, cw / 2 - 20, 4, teleBattery, col);
    } else {
        tft.setTextColor(COL_CARD_BORDER, COL_CARD_BG);
        tft.setTextSize(2);
        tft.drawString("--", x + 10, y + 30);
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
        tft.drawString(t, half + 10, y + 30);
        tft.setTextSize(1);
    } else {
        tft.setTextColor(COL_CARD_BORDER, COL_CARD_BG);
        tft.setTextSize(2);
        tft.drawString("--", half + 10, y + 30);
        tft.setTextSize(1);
    }
}

// Firefly swarm drifting within a soft circle (the idle animation)
static void animFireflies(int cx, int cy) {
    if (!fireInit) {
        for (int i = 0; i < 8; i++) {
            float a = i * 0.785f;
            fireX[i] = cx + 16 * cosf(a); fireY[i] = cy + 16 * sinf(a);
            fireVX[i] = 0.9f * cosf(a * 2.3f); fireVY[i] = 0.9f * sinf(a * 1.7f);
        }
        fireInit = true;
    }
    clearGlyph(cx, cy);
    for (int i = 0; i < 8; i++) {
        fireX[i] += fireVX[i]; fireY[i] += fireVY[i];
        float dx = fireX[i] - cx, dy = fireY[i] - cy;
        if (dx * dx + dy * dy > 46.0f * 46.0f) { fireVX[i] = -fireVX[i]; fireVY[i] = -fireVY[i]; }
        uint16_t c = (i & 1) ? COL_ACCENT : COL_CYAN;
        tft.fillCircle((int)fireX[i], (int)fireY[i], (i % 3 == 0) ? 3 : 2, c);
    }
}

void renderIdle() {
    static int lastTeleTemp = -2, lastTeleBatt = -2;
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
        lastTeleTemp = -2; lastTeleBatt = -2;   // force telemetry redraw
    }

    // Telemetry card — redraw only when a value changes
    if (teleCpuTemp != lastTeleTemp || teleBattery != lastTeleBatt) {
        lastTeleTemp = teleCpuTemp;
        lastTeleBatt = teleBattery;
        drawIdleTelemetry();
    }

    // ── Animated layer ───────────────────────────────────────────────────────
    if (millis() - lastAnimMs > ANIMATION_TICK_MS) {
        lastAnimMs = millis();
        idleT += 0.20f;
        animFireflies(cx, cy);
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

void renderStats() {
    static int lastMsgs = -1;
    static int lastTokens = -1;
    static int lastPct = -1;
    static bool lastAck = false;

    int w = tft.width();
    int h = tft.height();
    bool force = (screen != prevScr);
    bool ackActive = (millis() < ackUntilMs && ackText[0]);

    // Card geometry
    int cardX = MARGIN;
    int cardW = w - 2 * MARGIN;
    int cardH = 38;
    int gap   = 8;
    int y0    = HEADER_H + 6;          // msgs card top
    int y1    = y0 + cardH + gap;      // tokens card top
    int y2    = y1 + cardH + gap;      // context card top
    int ctxH  = 50;                    // context card is taller (holds bar)
    int ackY  = y2 + ctxH + gap;       // ack/status line

    if (force) {
        tft.fillScreen(COL_BG);
        drawHeader("SESSION");
        drawConnDot(true);
        drawHintBar();
        // Draw static card frames once
        drawCard(cardX, y2, cardW, ctxH);
        lastMsgs = -1; lastTokens = -1; lastPct = -1; lastAck = false;
    }

    if (force || statsMessages != lastMsgs) {
        lastMsgs = statsMessages;
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", statsMessages);
        drawStatCard(cardX, y0, cardW, cardH, "MESSAGES", buf);
    }

    if (force || statsTokens != lastTokens) {
        lastTokens = statsTokens;
        char buf[24];
        if (statsTokens >= 1000)
            snprintf(buf, sizeof(buf), "%d.%dk", statsTokens / 1000, (statsTokens % 1000) / 100);
        else
            snprintf(buf, sizeof(buf), "%d", statsTokens);
        drawStatCard(cardX, y1, cardW, cardH, "TOKENS", buf);
    }

    int pct = statsPct < 0 ? 0 : (statsPct > 100 ? 100 : statsPct);
    if (force || pct != lastPct) {
        lastPct = pct;
        // Label + percent inside the context card
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(COL_LABEL, COL_CARD_BG);
        tft.setTextSize(1);
        tft.drawString("CONTEXT", cardX + 10, y2 + 13);
        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
        // clear old percent area before redraw
        tft.fillRect(cardX + cardW - 44, y2 + 6, 36, 14, COL_CARD_BG);
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(COL_VALUE, COL_CARD_BG);
        tft.drawString(pctBuf, cardX + cardW - 10, y2 + 13);
        uint16_t barCol = (pct >= 85) ? COL_RED : (pct >= 60 ? COL_ORANGE : COL_ACCENT);
        drawBar(cardX + 10, y2 + ctxH - 16, cardW - 20, 8, pct, barCol);
    }

    if (ackActive || lastAck != ackActive) {
        lastAck = ackActive;
        tft.fillRect(MARGIN, ackY, cardW, 12, COL_BG);
        if (ackActive) {
            tft.setTextColor(COL_ACCENT, COL_BG);
            tft.setTextSize(1);
            tft.setTextDatum(ML_DATUM);
            char statusBuf[40];
            snprintf(statusBuf, sizeof(statusBuf), "> %s", ackText);
            tft.drawString(statusBuf, MARGIN + 2, ackY + 5);
        }
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
        statsMessages = 0;
        statsTokens   = 0;
        statsPct      = 0;
        lastStatsMs   = millis();
        statsStale    = false;
        startingSinceMs = 0;
        newChatPending = false;
        newChatLastAckSeq = newChatSeq;
        screen        = SCR_STATS;
        btn1Fired = false;
        btn1LastAcceptedMs = millis();
        ackText[0] = '\0';
        ackUntilMs = 0;

    } else if (strcmp(type, "chat_stats") == 0) {
        statsMessages = doc["messages"] | statsMessages;
        statsTokens   = doc["tokens"]   | statsTokens;
        statsPct      = doc["context_pct"] | statsPct;
        lastStatsMs   = millis();
        statsStale    = false;
        if (screen != SCR_PTT) screen = SCR_STATS;

    } else if (strcmp(type, "telemetry") == 0) {
        // Host CPU temp / battery for the idle status strip.
        // Missing fields keep their previous value (-1 = unknown).
        teleCpuTemp = doc["cpu_temp"] | teleCpuTemp;
        teleBattery = doc["battery"]  | teleBattery;

    } else if (strcmp(type, "ack") == 0) {
        const char* txt = doc["text"] | "";
        strncpy(ackText, txt, sizeof(ackText) - 1);
        ackUntilMs = millis() + 3000;

    } else if (strcmp(type, "error") == 0) {
        const char* txt = doc["text"] | "error";
        strncpy(ackText, txt, sizeof(ackText) - 1);
        ackUntilMs = millis() + 4000;
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
