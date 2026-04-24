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

// ── Drawing helpers ───────────────────────────────────────────────────────────
void drawHeader(const char* title) {
    // Elegant floating header (no block bg)
    tft.setTextColor(COL_HEADER_TXT, COL_BG);
    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(title, 10, 14);
}

void drawBar(int x, int y, int w, int h, int pct, uint16_t col) {
    // Modern segmented bar with rounded corners for the background
    tft.fillRoundRect(x, y, w, h, 2, COL_BAR_BG);
    int fill = (w * pct) / 100;
    if (fill > 0) tft.fillRoundRect(x, y, fill, h, 2, col);
    
    // Tiny label above or below could be added if needed, but let's keep it clean
}

void drawConnDot(bool connected) {
    uint16_t col = connected ? COL_GREEN : COL_RED;
    tft.fillCircle(tft.width() - 12, 14, 4, col);
}

// ── Screen renderers ──────────────────────────────────────────────────────────
void renderIdle() {
    if (screen == prevScr) return; // Prevent flickering

    tft.fillScreen(COL_BG);
    drawHeader("HERMES CORE");
    drawConnDot(true);

    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("READY", tft.width() / 2, 60);

    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setTextSize(1);
    tft.drawString("Tap TOP for New Chat", tft.width() / 2, 95);
    tft.drawString("Hold BTM to Talk", tft.width() / 2, 110);
}

void renderStarting() {
    static int lastDots = -1;
    if (screen != prevScr) {
        tft.fillScreen(COL_BG);
        drawHeader("INITIALIZING");
        drawConnDot(true);
        lastDots = -1; // Force dots redraw
    }
    if (startingDots != lastDots || screen != prevScr) {
        lastDots = startingDots;
        tft.fillRect(0, 50, tft.width(), 60, COL_BG);
        tft.setTextColor(COL_STARTING, COL_BG);
        tft.setTextSize(2);
        tft.setTextDatum(MC_DATUM);
        
        char dotLine[16] = "Hermes";
        for (int i = 0; i < (startingDots % 4); i++) strcat(dotLine, ".");
        tft.drawString(dotLine, tft.width() / 2, 75);
    }
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

    if (force) {
        tft.fillScreen(COL_BG);
        drawHeader("SESSION STATS");
        drawConnDot(true);
        tft.drawFastVLine(w / 2, 35, 60, COL_BAR_BG);
        lastMsgs = -1; lastTokens = -1; lastPct = -1;
    }

    if (force || statsMessages != lastMsgs) {
        lastMsgs = statsMessages;
        tft.setTextColor(COL_LABEL, COL_BG);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("MESSAGES", w / 4, 45);
        tft.setTextColor(COL_VALUE, COL_BG);
        tft.setTextSize(2);
        char msgBuf[12];
        snprintf(msgBuf, sizeof(msgBuf), "%d", statsMessages);
        tft.fillRect(10, 55, (w / 2) - 20, 25, COL_BG);
        tft.drawString(msgBuf, w / 4, 70);
    }

    if (force || statsTokens != lastTokens) {
        lastTokens = statsTokens;
        tft.setTextColor(COL_LABEL, COL_BG);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("TOKENS", (w * 3) / 4, 45);
        char tokBuf[24];
        if (statsTokens >= 1000)
            snprintf(tokBuf, sizeof(tokBuf), "%d.%dk", statsTokens / 1000, (statsTokens % 1000) / 100);
        else
            snprintf(tokBuf, sizeof(tokBuf), "%d", statsTokens);
        tft.setTextColor(COL_VALUE, COL_BG);
        tft.setTextSize(2);
        tft.fillRect((w / 2) + 10, 55, (w / 2) - 20, 25, COL_BG);
        tft.drawString(tokBuf, (w * 3) / 4, 70);
    }

    int pct = statsPct < 0 ? 0 : (statsPct > 100 ? 100 : statsPct);
    if (force || pct != lastPct) {
        lastPct = pct;
        tft.setTextColor(COL_LABEL, COL_BG);
        tft.setTextSize(1);
        tft.setTextDatum(ML_DATUM);
        tft.drawString("Context", 15, 107);
        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(pctBuf, w - 15, 107);
        drawBar(15, 117, w - 30, 8, pct, COL_ORANGE);
    }

    if (ackActive || lastAck != ackActive) {
        lastAck = ackActive;
        // Clear status region (y=86 to 98) — stays away from "Context" at 107
        tft.fillRect(15, 86, w - 30, 14, COL_BG); 
        
        if (ackActive) {
            tft.setTextColor(COL_ACCENT, COL_BG);
            tft.setTextSize(1);
            tft.setTextDatum(ML_DATUM);
            char statusBuf[40];
            snprintf(statusBuf, sizeof(statusBuf), "> %s", ackText);
            tft.drawString(statusBuf, 15, 91);
        }
    }
}

void renderPtt() {
    int w = tft.width();
    int h = tft.height();

    if (screen != prevScr) {
        tft.fillScreen(COL_PTT_BG);
        drawHeader("LISTENING");
        drawConnDot(true);
        
        // Mic icon
        int cx = w / 2;
        int cy = 45; // Moved up slightly for landscape
        tft.fillRoundRect(cx - 8, cy - 15, 16, 25, 8, COL_PTT_WAVE);
        tft.drawCircle(cx, cy, 18, COL_PTT_WAVE);
        tft.drawFastVLine(cx, cy + 18, 5, COL_PTT_WAVE);
        tft.drawFastHLine(cx - 10, cy + 23, 20, COL_PTT_WAVE);

        tft.setTextColor(COL_PTT_WAVE, COL_PTT_BG);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("RELEASE TO SEND", w / 2, h - 15);
    }

    // Animated waveform bars
    if (millis() - lastAnimMs > ANIMATION_TICK_MS) {
        lastAnimMs = millis();
        wavePhase  = (wavePhase + 1) % 8;
        const int heights[] = {8, 16, 28, 40, 28, 16, 8, 6};
        int numBars = 9;
        int barW = 10, gap = 6;
        int totalW = numBars * barW + (numBars - 1) * gap;
        int startX = (w - totalW) / 2;
        int baseY  = h - 50; 
        
        tft.fillRect(0, baseY - 22, w, 45, COL_PTT_BG);
        
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

    // Splash
    tft.setTextColor(COL_CYAN, COL_BG);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("HERMES", SCREEN_W / 2, 100);
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.drawString("Chat Controller", SCREEN_W / 2, 124);
    delay(800);

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
