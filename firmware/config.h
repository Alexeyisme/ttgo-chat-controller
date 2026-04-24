#ifndef CONFIG_H
#define CONFIG_H

// ── Serial ─────────────────────────────────────────────────────────────────────
#define SERIAL_BAUD_RATE    115200
#define SP_BUF_SIZE         512     // Input line buffer (small — no images here)

// ── TFT ────────────────────────────────────────────────────────────────────────
#define TFT_ROTATION    0           // Portrait: 135 wide x 240 tall
#define SCREEN_W        135
#define SCREEN_H        240
#define TFT_BL          4           // Backlight GPIO

// ── Buttons ────────────────────────────────────────────────────────────────────
#define BTN1_PIN        35          // Left button  — new chat (active LOW, input-only)
#define BTN2_PIN        0           // Right button — PTT hold  (active LOW, has pull-up)
#define BTN_DEBOUNCE_MS 120
#define BTN2_HOLD_MS    250         // ms hold before PTT activates
#define BTN2_RAW_SETTLE_MS 20
#define BTN2_LOG_EVERY_MS 500
#define NEW_CHAT_COOLDOWN_MS 1200
#define STARTING_TIMEOUT_MS  8000

// ── Context window assumption (tokens) ─────────────────────────────────────────
// Used to compute percentage bar. DeepSeek-V3 / Claude typical context window.
#define CTX_WINDOW_TOKENS   200000

// ── Colors (RGB565) ────────────────────────────────────────────────────────────
#define COL_BG          0x0000      // Black
#define COL_HEADER      0x04B4      // Dark teal
#define COL_HEADER_TXT  0xFFFF      // White
#define COL_LABEL       0x7BEF      // Light gray
#define COL_VALUE       0xFFFF      // White
#define COL_GREEN       0x07E0      // Green (connected, PTT wave)
#define COL_RED         0xF800      // Red (disconnected)
#define COL_CYAN        0x07FF      // Cyan (bars)
#define COL_ORANGE      0xFD20      // Orange (context bar)
#define COL_BAR_BG      0x2104      // Dark bar background
#define COL_PTT_BG      0x0000
#define COL_PTT_WAVE    0x07E0
#define COL_STARTING    0x07FF      // Cyan "STARTING" text

// ── Timing ─────────────────────────────────────────────────────────────────────
#define ANIMATION_TICK_MS   120
#define STALE_DATA_MS       30000   // Stale indicator after 30s no update

#endif // CONFIG_H
