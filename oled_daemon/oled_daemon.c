#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <time.h>

#include <jetgpio.h>
#include "ssd1306.h"

/* ── Configuration ──────────────────────────────────────────────────────── */

/* I2C device for OLED.
 * On Jetson Orin:  pins 3(SDA)/5(SCL)   →  /dev/i2c-7  (use this — i2c-1 shares bus with system devices)
 *                  pins 27(SDA)/28(SCL)  →  /dev/i2c-1  (avoid — disrupts network on Orin)
 * On Jetson Nano:  pins 27/28            →  /dev/i2c-1 */
#define I2C_DEV      "/dev/i2c-7"
#define I2C_ADDR      0x3C   /* 0x3D if ADDR pin is high */

/* GPIO pin for display cycle button (active-low, pull-up to 3.3 V) */
#define CYCLE_PIN        37

/* File other programs can write to update the custom text screen */
#define CUSTOM_TEXT_FILE  "/run/oled_text"

/* How often to refresh IP addresses (ms) */
#define REFRESH_MS    5000

/* Debounce for cycle button (µs) */
#define DEBOUNCE_US   50000

/* Number of display screens */
#define NUM_SCREENS    2

/* ── State ──────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t running       = 1;
static volatile int          screen_dirty  = 1;  /* redraw on next tick */
static volatile int          screen_idx    = 0;

static unsigned long cycle_ts;

static void cycle_isr(void)
{
    screen_idx  = (screen_idx + 1) % NUM_SCREENS;
    screen_dirty = 1;
}

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ── Screen builders ────────────────────────────────────────────────────── *
 * Each function fills 'framebuf' (from ssd1306.h) with its content.
 * fb_puts(col_char, page, text) — col_char: 0-20, page: 0-7
 */

/* Page indicator in bottom-right corner: "1/2" etc. */
static void draw_page_indicator(int page, int row)
{
    char indicator[5];
    snprintf(indicator, sizeof(indicator), "%d/%d", page + 1, NUM_SCREENS);
    /* right-align within 21 columns */
    int start = FONT_COLS - (int)strlen(indicator);
    fb_puts(start, row, indicator);
}

/* ── Screen 0: network IP addresses ──────────────────────────────────────── */

static void screen_ip(void)
{
    fb_clear();
    fb_puts(0, 0, "-- NETWORK IPS --");

    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) < 0) {
        fb_puts(0, 1, "getifaddrs error");
        goto done;
    }

    int row = 1;
    for (ifa = ifaddr; ifa && row < 7; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(ifa->ifa_name, "lo") == 0)
            continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                  ip, sizeof(ip));

        char line[22];
        snprintf(line, sizeof(line), "%-6s%s", ifa->ifa_name, ip);
        fb_puts(0, row++, line);
    }

    if (row == 1)
        fb_puts(0, 1, "no interfaces up");

    freeifaddrs(ifaddr);
done:
    draw_page_indicator(0, 7);
}

/* ── Screen 1: custom text from CUSTOM_TEXT_FILE ─────────────────────────── */

static void screen_custom(void)
{
    fb_clear();

    FILE *f = fopen(CUSTOM_TEXT_FILE, "r");
    if (!f) {
        fb_puts(0, 0, "-- CUSTOM TEXT --");
        char hint[22];
        snprintf(hint, sizeof(hint), "write to:");
        fb_puts(0, 2, hint);
        snprintf(hint, sizeof(hint), CUSTOM_TEXT_FILE);
        fb_puts(0, 3, hint);
        draw_page_indicator(1, 7);
        return;
    }

    char line[22];
    int row = 0;
    while (row < 7 && fgets(line, sizeof(line), f)) {
        /* strip trailing newline */
        line[strcspn(line, "\n")] = '\0';
        fb_puts(0, row++, line);
    }
    fclose(f);

    draw_page_indicator(1, 7);
}

/* ── Display update ─────────────────────────────────────────────────────── */

static void (*screens[])(void) = {
    screen_ip,
    screen_custom,
};

static void redraw(int fd_oled)
{
    screens[screen_idx]();
    ssd1306_flush(fd_oled);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Initialise jetgpio for GPIO button only */
    int ret = gpioInitialise();
    if (ret < 0) {
        fprintf(stderr, "gpioInitialise failed: %d\n", ret);
        return 1;
    }

    gpioSetMode(CYCLE_PIN, JET_INPUT);
    gpioSetISRFunc(CYCLE_PIN, FALLING_EDGE, DEBOUNCE_US, &cycle_ts, cycle_isr);

    /* Open OLED over Linux i2c */
    int fd_oled = ssd1306_open(I2C_DEV, I2C_ADDR);
    if (fd_oled < 0) {
        fprintf(stderr, "Cannot open %s (addr 0x%02X)\n", I2C_DEV, I2C_ADDR);
        gpioTerminate();
        return 1;
    }

    ssd1306_init(fd_oled);

    int elapsed_ms = REFRESH_MS;   /* force draw on first tick */

    while (running) {
        if (screen_dirty || elapsed_ms >= REFRESH_MS) {
            redraw(fd_oled);
            screen_dirty = 0;
            elapsed_ms   = 0;
        }

        usleep(100000);   /* 100 ms tick */
        elapsed_ms += 100;
    }

    /* Clear display on exit */
    fb_clear();
    ssd1306_flush(fd_oled);
    ssd1306_cmd(fd_oled, 0xAE);  /* display off */

    close(fd_oled);
    gpioTerminate();
    return 0;
}
