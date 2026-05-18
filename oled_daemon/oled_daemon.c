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
#include <pthread.h>
#include <gpiod.h>

#include "ssd1306.h"

/* ── Configuration ──────────────────────────────────────────────────────── */

/* I2C device for OLED.
 * On Jetson Orin:  pins 3(SDA)/5(SCL)   →  /dev/i2c-7  (use this — i2c-1 shares bus with system devices)
 *                  pins 27(SDA)/28(SCL)  →  /dev/i2c-1  (avoid — disrupts network on Orin)
 * On Jetson Nano:  pins 27/28            →  /dev/i2c-1 */
#define I2C_DEV      "/dev/i2c-7"
#define I2C_ADDR      0x3C   /* 0x3D if ADDR pin is high */

/* GPIO button (active-low, pull-up to 3.3 V) via libgpiod.
 *
 * Find the right values by running on the Jetson:
 *   gpioinfo tegra-gpio | grep -n "G4_SOC_GPIO21"
 *   (the line number printed is GPIO_LINE_OFFSET)
 *
 * On Orin AGX: pin 18 = G4_SOC_GPIO21, chip "tegra-gpio"
 * On Orin Nano/NX: pin 18 = G2_SPI3_CS0, chip "tegra-gpio"
 */
#define GPIO_CHIP_LABEL   "tegra-gpio"
#define GPIO_LINE_OFFSET  106   /* verify with: gpioinfo tegra-gpio | grep G4_SOC_GPIO21 */

/* Set to 1 to enable the internal pull-up resistor on the button pin.
 * Requires kernel ≥ 5.5 and a GPIO driver that supports bias config.
 * If the daemon fails to start with this enabled, use an external pull-up
 * and set this to 0. */
#define USE_INTERNAL_PULLUP  1

/* File other programs can write to update the custom text screen */
#define CUSTOM_TEXT_FILE  "/run/oled_text"

/* How often to refresh IP addresses (ms) */
#define REFRESH_MS    5000

/* Debounce for cycle button (ms) */
#define DEBOUNCE_MS   50

/* Number of display screens */
#define NUM_SCREENS    2

/* ── State ──────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t running       = 1;
static volatile int          screen_dirty  = 1;  /* redraw on next tick */
static volatile int          screen_idx    = 0;

static struct gpiod_chip *gpio_chip = NULL;
static struct gpiod_line *gpio_line = NULL;
static pthread_t          btn_thread;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void *button_monitor(void *arg)
{
    (void)arg;
    struct gpiod_line_event ev;
    struct timespec timeout = {0, 100 * 1000000L};  /* 100 ms poll interval */

    while (running) {
        int r = gpiod_line_event_wait(gpio_line, &timeout);
        if (r != 1)
            continue;
        if (gpiod_line_event_read(gpio_line, &ev) != 0)
            continue;
        if (ev.event_type != GPIOD_LINE_EVENT_FALLING_EDGE)
            continue;

        struct timespec debounce = {0, DEBOUNCE_MS * 1000000L};
        nanosleep(&debounce, NULL);

        screen_idx   = (screen_idx + 1) % NUM_SCREENS;
        screen_dirty = 1;
    }
    return NULL;
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

    /* Open OLED over Linux i2c — do this before touching any GPIO */
    int fd_oled = ssd1306_open(I2C_DEV, I2C_ADDR);
    if (fd_oled < 0) {
        fprintf(stderr, "Cannot open %s (addr 0x%02X)\n", I2C_DEV, I2C_ADDR);
        return 1;
    }
    ssd1306_init(fd_oled);

    /* Set up cycle button via libgpiod (kernel GPIO interface, no /dev/mem) */
    gpio_chip = gpiod_chip_open_by_label(GPIO_CHIP_LABEL);
    if (!gpio_chip) {
        fprintf(stderr, "Warning: cannot open GPIO chip \"%s\", button disabled\n",
                GPIO_CHIP_LABEL);
    } else {
        gpio_line = gpiod_chip_get_line(gpio_chip, GPIO_LINE_OFFSET);
        if (!gpio_line) {
            fprintf(stderr, "Warning: cannot get GPIO line %d, button disabled\n",
                    GPIO_LINE_OFFSET);
            gpiod_chip_close(gpio_chip);
            gpio_chip = NULL;
        } else {
            struct gpiod_line_request_config btn_cfg = {
                .consumer     = "oled_daemon",
                .request_type = GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE,
                .flags        = USE_INTERNAL_PULLUP ? GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP : 0,
            };
            if (gpiod_line_request(gpio_line, &btn_cfg, 0) < 0) {
                fprintf(stderr, "Warning: cannot request GPIO events, button disabled\n");
                gpiod_chip_close(gpio_chip);
                gpio_chip = NULL;
                gpio_line = NULL;
            }
        }
        if (gpio_line) {
            pthread_create(&btn_thread, NULL, button_monitor, NULL);
        }
    }

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

    if (gpio_line) {
        pthread_join(btn_thread, NULL);
        gpiod_line_release(gpio_line);
    }
    if (gpio_chip)
        gpiod_chip_close(gpio_chip);

    return 0;
}
