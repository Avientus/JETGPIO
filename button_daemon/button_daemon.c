#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <gpiod.h>

/* ── Pin configuration ──────────────────────────────────────────────────── *
 * Circuit: button between pin and GND, with a 10kΩ pull-up to 3.3V.
 * Active-low: pin reads 0 when pressed, 1 when released.
 *
 * Find GPIO line offsets by running on the Jetson:
 *   gpioinfo tegra-gpio
 *
 * On Orin AGX:
 *   Pin 13 = G3_SOC_GPIO37  →  verify offset with gpioinfo
 *   Pin 15 = EDP_SOC_GPIO39 →  verify offset with gpioinfo
 *
 * Avoid pins 29/31/37: CAN bus on Orin AGX.
 */
#define GPIO_CHIP_LABEL     "tegra-gpio"

#define REBOOT_LINE_OFFSET   88   /* pin 15: verify with: gpioinfo tegra-gpio | grep EDP_SOC_GPIO39 */
#define POWER_LINE_OFFSET    80   /* pin 13: verify with: gpioinfo tegra-gpio | grep G3_SOC_GPIO37  */

/* Set to 1 to enable the internal pull-up resistor on the button pins.
 * Requires kernel ≥ 5.5 and a GPIO driver that supports bias config.
 * If the daemon fails to start with this enabled, use external pull-ups
 * and set this to 0. */
#define USE_INTERNAL_PULLUP  1

/* ── Timing (milliseconds) ──────────────────────────────────────────────── */
#define DEBOUNCE_MS      50      /* ignore bounces shorter than this */
#define REBOOT_HOLD_MS  1000     /* hold reboot button for 1 s  */
#define POWER_HOLD_MS   3000     /* hold power button for 3 s   */
#define POLL_MS           10     /* hold-check poll interval    */

/* ── Shared state ───────────────────────────────────────────────────────── */
static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ── Hold-detection ─────────────────────────────────────────────────────── *
 * After a falling edge is detected, poll the line level every POLL_MS.
 * Returns 1 if the pin stays low for at least required_ms, 0 if released.
 */
static int held_for(struct gpiod_line *line, int required_ms)
{
    int elapsed = 0;
    while (running) {
        int val = gpiod_line_get_value(line);
        if (val != 0)        /* released */
            return 0;
        if (elapsed >= required_ms)
            return 1;
        struct timespec ts = {0, POLL_MS * 1000000L};
        nanosleep(&ts, NULL);
        elapsed += POLL_MS;
    }
    return 0;
}

static void wait_release(struct gpiod_line *line)
{
    while (running && gpiod_line_get_value(line) == 0) {
        struct timespec ts = {0, POLL_MS * 1000000L};
        nanosleep(&ts, NULL);
    }
}

/* ── Button thread ──────────────────────────────────────────────────────── */
typedef struct {
    struct gpiod_line *line;
    int                hold_ms;
    const char        *name;
    const char        *systemctl_cmd;  /* "reboot" or "poweroff" */
} BtnCtx;

static void *button_thread(void *arg)
{
    BtnCtx *ctx = arg;
    struct gpiod_line_event ev;
    struct timespec timeout = {0, 100 * 1000000L};  /* 100 ms */

    while (running) {
        int r = gpiod_line_event_wait(ctx->line, &timeout);
        if (r != 1)
            continue;
        if (gpiod_line_event_read(ctx->line, &ev) != 0)
            continue;
        if (ev.event_type != GPIOD_LINE_EVENT_FALLING_EDGE)
            continue;

        /* Debounce: wait, then confirm pin is still low */
        struct timespec deb = {0, DEBOUNCE_MS * 1000000L};
        nanosleep(&deb, NULL);
        if (gpiod_line_get_value(ctx->line) != 0)
            continue;   /* bounce, not a real press */

        printf("%s button pressed, checking hold (%d ms required)...\n",
               ctx->name, ctx->hold_ms);
        fflush(stdout);

        if (held_for(ctx->line, ctx->hold_ms)) {
            printf("%s button held — triggering %s\n",
                   ctx->name, ctx->systemctl_cmd);
            fflush(stdout);
            wait_release(ctx->line);
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "systemctl %s", ctx->systemctl_cmd);
            system(cmd);
        } else {
            printf("%s button released early — ignoring\n", ctx->name);
            fflush(stdout);
        }
    }
    return NULL;
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    struct gpiod_chip *chip = gpiod_chip_open_by_label(GPIO_CHIP_LABEL);
    if (!chip) {
        fprintf(stderr, "Cannot open GPIO chip \"%s\"\n", GPIO_CHIP_LABEL);
        return 1;
    }

    struct gpiod_line *reboot_line = gpiod_chip_get_line(chip, REBOOT_LINE_OFFSET);
    struct gpiod_line *power_line  = gpiod_chip_get_line(chip, POWER_LINE_OFFSET);

    if (!reboot_line || !power_line) {
        fprintf(stderr, "Cannot get GPIO lines (reboot=%d, power=%d) — "
                "check offsets with: gpioinfo %s\n",
                REBOOT_LINE_OFFSET, POWER_LINE_OFFSET, GPIO_CHIP_LABEL);
        gpiod_chip_close(chip);
        return 1;
    }

    struct gpiod_line_request_config btn_cfg = {
        .consumer     = "button_daemon",
        .request_type = GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE,
        .flags        = USE_INTERNAL_PULLUP ? GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP : 0,
    };

    if (gpiod_line_request(reboot_line, &btn_cfg, 0) < 0 ||
        gpiod_line_request(power_line,  &btn_cfg, 0) < 0) {
        fprintf(stderr, "Cannot request GPIO edge events — "
                "another process may own the lines\n");
        gpiod_chip_close(chip);
        return 1;
    }

    BtnCtx reboot_ctx = {
        .line          = reboot_line,
        .hold_ms       = REBOOT_HOLD_MS,
        .name          = "Reboot",
        .systemctl_cmd = "reboot",
    };
    BtnCtx power_ctx = {
        .line          = power_line,
        .hold_ms       = POWER_HOLD_MS,
        .name          = "Power",
        .systemctl_cmd = "poweroff",
    };

    pthread_t reboot_thread, power_thread_id;
    pthread_create(&reboot_thread,    NULL, button_thread, &reboot_ctx);
    pthread_create(&power_thread_id,  NULL, button_thread, &power_ctx);

    printf("Button daemon running — reboot line %d, power line %d\n",
           REBOOT_LINE_OFFSET, POWER_LINE_OFFSET);
    fflush(stdout);

    pthread_join(reboot_thread,   NULL);
    pthread_join(power_thread_id, NULL);

    gpiod_line_release(reboot_line);
    gpiod_line_release(power_line);
    gpiod_chip_close(chip);
    return 0;
}
