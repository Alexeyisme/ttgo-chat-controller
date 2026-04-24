#ifndef CONFIG_H
#define CONFIG_H

// ── Serial ─────────────────────────────────────────────────────────────────────
#define SERIAL_BAUD_RATE    115200
#define SP_BUF_SIZE         512     // Input line buffer (small — no images here)

// ── TFT ────────────────────────────────────────────────────────────────────────
#define TFT_ROTATION    3           // Trying 3 (flipped landscape)
#define SCREEN_W        240
#define SCREEN_H        135
#define TFT_BL          4           // Backlight GPIO

// ── Buttons ────────────────────────────────────────────────────────────────────
#define BTN1_PIN        35          // GPIO35 — Left side, Bottom (usually)
#define BTN2_PIN        0           // GPIO0  — Left side, Top (near USB)
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
#define COL_ACCENT      0x07FF      // Cyan
#define COL_HEADER      0x0000      // Transparent/Black header
#define COL_HEADER_TXT  0x7BEF      // Light gray secondary
#define COL_LABEL       0xBDD7      // Muted blue/gray
#define COL_VALUE       0xFFFF      // White
#define COL_GREEN       0x07E0      // Neon Green (connected)
#define COL_RED         0xF800      // Red (disconnected)
#define COL_CYAN        0x07FF      // Cyan (bars)
#define COL_ORANGE      0xFD20      // Orange (context bar)
#define COL_BAR_BG      0x2104      // Dark bar background
#define COL_PTT_BG      0x0000
#define COL_PTT_WAVE    0x07FF      // Cyan pulses
#define COL_STARTING    0x07FF      // Cyan "STARTING" text
#define COL_ACK_BG      0x07E0      // Green banner
#define COL_ACK_TXT     0x0000      // Black text on green

// ── Timing ─────────────────────────────────────────────────────────────────────
#define ANIMATION_TICK_MS   80      // Slightly faster animations
#define STALE_DATA_MS       30000   // Stale indicator after 30s no update

#endif // CONFIG_H
