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
};
ScreenState screen    = SCR_IDLE;
ScreenState prevScr   = SCR_IDLE;

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
    tft.fillRect(0, 0, SCREEN_W, 28, COL_HEADER);
    tft.setTextColor(COL_HEADER_TXT, COL_HEADER);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(title, SCREEN_W / 2, 14);
}

void drawBar(int x, int y, int w, int h, int pct, uint16_t col) {
    tft.fillRect(x, y, w, h, COL_BAR_BG);
    int fill = (w * pct) / 100;
    if (fill > 0) tft.fillRect(x, y, fill, h, col);
    // Percentage label
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setTextSize(1);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(buf, x + w + 28, y + h / 2);
}

void drawConnDot(bool connected) {
    uint16_t col = connected ? COL_GREEN : COL_RED;
    tft.fillCircle(SCREEN_W - 8, 14, 4, col);
}

// ── Screen renderers ──────────────────────────────────────────────────────────
void renderIdle() {
    tft.fillScreen(COL_BG);
    drawHeader("CHAT CTL");
    drawConnDot(true);

    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Press LEFT to", SCREEN_W / 2, 100);
    tft.drawString("start new chat", SCREEN_W / 2, 116);
    tft.drawString("Hold RIGHT to", SCREEN_W / 2, 150);
    tft.drawString("talk (PTT)", SCREEN_W / 2, 166);
}

void renderStarting() {
    // Only redraw the dot line to avoid flicker
    static int lastDots = -1;
    if (screen != prevScr) {
        tft.fillScreen(COL_BG);
        drawHeader("NEW CHAT");
        drawConnDot(true);
    }
    if (startingDots != lastDots || screen != prevScr) {
        lastDots = startingDots;
        tft.fillRect(0, 90, SCREEN_W, 60, COL_BG);
        tft.setTextColor(COL_STARTING, COL_BG);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Starting new", SCREEN_W / 2, 100);

        char dotLine[8] = "chat";
        for (int i = 0; i < (startingDots % 4); i++) dotLine[4 + i] = '.';
        dotLine[4 + (startingDots % 4)] = '\0';
        tft.drawString(dotLine, SCREEN_W / 2, 116);
    }
}

void renderStats() {
    if (screen != prevScr) {
        tft.fillScreen(COL_BG);
        drawHeader("CHAT STATS");
        drawConnDot(true);
    }

    // Messages
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("Messages", 8, 52);

    tft.setTextColor(COL_VALUE, COL_BG);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    char msgBuf[12];
    snprintf(msgBuf, sizeof(msgBuf), "%d", statsMessages);
    tft.fillRect(0, 62, SCREEN_W, 22, COL_BG);
    tft.drawString(msgBuf, SCREEN_W / 2, 73);

    // Context tokens
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("Context", 8, 102);

    char tokBuf[24];
    if (statsTokens >= 1000)
        snprintf(tokBuf, sizeof(tokBuf), "%dk tok", statsTokens / 1000);
    else
        snprintf(tokBuf, sizeof(tokBuf), "%d tok", statsTokens);

    tft.setTextColor(COL_VALUE, COL_BG);
    tft.setTextSize(1);
    tft.setTextDatum(MR_DATUM);
    tft.fillRect(0, 96, SCREEN_W - 30, 14, COL_BG);
    tft.drawString(tokBuf, SCREEN_W - 32, 103);

    // Context bar
    int pct = statsPct < 0 ? 0 : (statsPct > 100 ? 100 : statsPct);
    drawBar(8, 118, 88, 10, pct, COL_ORANGE);

    // Stale indicator
    if (statsStale) {
        tft.setTextColor(0x632C, COL_BG);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("stale", SCREEN_W / 2, 144);
    } else {
        tft.fillRect(0, 138, SCREEN_W, 14, COL_BG);
    }

    // Ack banner
    if (millis() < ackUntilMs && ackText[0]) {
        tft.fillRect(0, 158, SCREEN_W, 18, 0x07E0); // green bg
        tft.setTextColor(COL_BG, 0x07E0);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(ackText, SCREEN_W / 2, 167);
    } else {
        tft.fillRect(0, 158, SCREEN_W, 18, COL_BG);
    }

    // Hint
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Hold RIGHT: talk", SCREEN_W / 2, 210);
    tft.drawString("LEFT: new chat", SCREEN_W / 2, 224);
}

void renderPtt() {
    if (screen != prevScr) {
        tft.fillScreen(COL_PTT_BG);
        drawHeader("LISTENING");
        drawConnDot(true);
        // Mic icon (simple circle + stand)
        tft.drawCircle(SCREEN_W / 2, 85, 18, COL_PTT_WAVE);
        tft.drawCircle(SCREEN_W / 2, 85, 17, COL_PTT_WAVE);
        tft.drawFastVLine(SCREEN_W / 2, 103, 12, COL_PTT_WAVE);
        tft.drawLine(SCREEN_W / 2 - 10, 115, SCREEN_W / 2 + 10, 115, COL_PTT_WAVE);
        tft.setTextColor(COL_PTT_WAVE, COL_PTT_BG);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Release to send", SCREEN_W / 2, 210);
    }

    // Animated waveform bars
    if (millis() - lastAnimMs > ANIMATION_TICK_MS) {
        lastAnimMs = millis();
        wavePhase  = (wavePhase + 1) % 8;
        const int heights[] = {6, 12, 20, 28, 20, 12, 6, 4};
        int barW = 8, gap = 4;
        int totalW = 5 * barW + 4 * gap;
        int startX = (SCREEN_W - totalW) / 2;
        int baseY  = 160;
        tft.fillRect(0, baseY - 30, SCREEN_W, 62, COL_PTT_BG);
        for (int i = 0; i < 5; i++) {
            int h = heights[(wavePhase + i) % 8];
            int x = startX + i * (barW + gap);
            tft.fillRoundRect(x, baseY - h / 2, barW, h, 2, COL_PTT_WAVE);
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
        if (!newChatPending && screen != SCR_STARTING) {
            newChatPending = true;
            newChatPendingSinceMs = millis();
            newChatPressAcceptedMs = millis();
            newChatSeq++;
            screen    = SCR_STARTING;
            startingDots = 0;
            lastDotMs    = millis();
            prevScr      = SCR_STATS; // force full redraw on next tick
            sendEvent("new_chat");
        }
    }

    if (newChatPending && (millis() - newChatPendingSinceMs) > STARTING_TIMEOUT_MS) {
        newChatPending = false;
        screen = SCR_IDLE;
        prevScr = SCR_STARTING;
    }

    displayTick();
    delay(30);
}
