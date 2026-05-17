#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <jetgpio.h>

#define NUM_LEDS         12
#define BYTES_PER_LED    24
#define SPI_BUF_SIZE     (NUM_LEDS * BYTES_PER_LED)
#define SOCKET_PATH      "/run/leds.sock"
#define BLINK_INTERVAL_US  500000
#define EFFECT_TICK_MS   20      /* effect engine update rate (~50 Hz) */

/* ── Effect definitions ─────────────────────────────────────────────────── *
 *
 * Socket protocol: 7 bytes per command
 *   [index, r, g, b, effect, period_ms_hi, period_ms_lo]
 *
 *   index  0     : system LED (boot indicator) — always rejected
 *   index  1-11  : individual user LEDs
 *   index  0xFF  : apply to all user LEDs (1-11)
 *   index  0xFE  : clear all user LEDs (solid black)
 *
 *   effect 0 : SOLID  — static colour
 *   effect 1 : BLINK  — on/off at period_ms
 *   effect 2 : FADE   — sine-wave brightness at period_ms
 */
#define EFFECT_SOLID  0
#define EFFECT_BLINK  1
#define EFFECT_FADE   2

typedef struct {
    uint8_t  r, g, b;
    uint8_t  effect;
    uint32_t period_ms;
    uint32_t elapsed_ms;
} LedConfig;

static volatile sig_atomic_t booted  = 0;
static volatile sig_atomic_t running = 1;

static pthread_mutex_t spi_mutex = PTHREAD_MUTEX_INITIALIZER;
static int spi_handle = -1;

/* Physical render state: [led][g, r, b] — GRB order for WS2812B */
static uint8_t  led_state[NUM_LEDS][3];
/* Logical config per user LED (index 1-11) */
static LedConfig configs[NUM_LEDS];

/* ── WS2812B SPI encoding ───────────────────────────────────────────────── */

static void encode_led(char *buf, int idx, uint8_t g, uint8_t r, uint8_t b)
{
    char *p = buf + idx * BYTES_PER_LED;
    for (int bit = 0; bit < 8; bit++) {
        p[bit]      = (g & (1 << bit)) ? 0x06 : 0x04;
        p[bit + 8]  = (r & (1 << bit)) ? 0x06 : 0x04;
        p[bit + 16] = (b & (1 << bit)) ? 0x06 : 0x04;
    }
}

/* Must be called with spi_mutex held */
static void flush_leds(void)
{
    char tx[SPI_BUF_SIZE];
    char rx[SPI_BUF_SIZE];
    for (int i = 0; i < NUM_LEDS; i++)
        encode_led(tx, i, led_state[i][0], led_state[i][1], led_state[i][2]);
    spiXfer(spi_handle, tx, rx, SPI_BUF_SIZE);
}

/* Convenience wrapper — locks, updates LED 0, flushes, unlocks */
static void set_system_led(uint8_t g, uint8_t r, uint8_t b)
{
    pthread_mutex_lock(&spi_mutex);
    led_state[0][0] = g;
    led_state[0][1] = r;
    led_state[0][2] = b;
    flush_leds();
    pthread_mutex_unlock(&spi_mutex);
}

/* ── Effect engine ──────────────────────────────────────────────────────── */

static void *effect_thread(void *arg)
{
    (void)arg;
    while (running) {
        usleep(EFFECT_TICK_MS * 1000);

        pthread_mutex_lock(&spi_mutex);
        int dirty = 0;

        for (int i = 1; i < NUM_LEDS; i++) {
            LedConfig *c = &configs[i];
            if (c->effect == EFFECT_SOLID)
                continue;

            c->elapsed_ms = (c->elapsed_ms + EFFECT_TICK_MS) % c->period_ms;

            if (c->effect == EFFECT_BLINK) {
                int on = c->elapsed_ms < c->period_ms / 2;
                led_state[i][0] = on ? c->g : 0;
                led_state[i][1] = on ? c->r : 0;
                led_state[i][2] = on ? c->b : 0;

            } else if (c->effect == EFFECT_FADE) {
                /* sin over [0, π] → smooth 0→1→0 per period */
                float phase = (float)c->elapsed_ms * (float)M_PI / (float)c->period_ms;
                float brightness = sinf(phase);
                led_state[i][0] = (uint8_t)(c->g * brightness);
                led_state[i][1] = (uint8_t)(c->r * brightness);
                led_state[i][2] = (uint8_t)(c->b * brightness);
            }
            dirty = 1;
        }

        if (dirty)
            flush_leds();

        pthread_mutex_unlock(&spi_mutex);
    }
    return NULL;
}

/* ── Socket handler ─────────────────────────────────────────────────────── */

static void apply_config(int i, uint8_t r, uint8_t g, uint8_t b,
                         uint8_t effect, uint16_t period_ms)
{
    /* Must be called with spi_mutex held */
    configs[i].r          = r;
    configs[i].g          = g;
    configs[i].b          = b;
    configs[i].effect     = effect;
    configs[i].period_ms  = period_ms ? period_ms : 500;
    configs[i].elapsed_ms = 0;

    if (effect == EFFECT_SOLID) {
        led_state[i][0] = g;
        led_state[i][1] = r;
        led_state[i][2] = b;
    }
}

static void handle_command(const uint8_t cmd[7])
{
    uint8_t  idx       = cmd[0];
    uint8_t  r         = cmd[1];
    uint8_t  g         = cmd[2];
    uint8_t  b         = cmd[3];
    uint8_t  effect    = cmd[4];
    uint16_t period_ms = ((uint16_t)cmd[5] << 8) | cmd[6];

    if (idx == 0 || (idx > (uint8_t)(NUM_LEDS - 1) && idx != 0xFF && idx != 0xFE))
        return;

    pthread_mutex_lock(&spi_mutex);

    if (idx == 0xFF) {
        for (int i = 1; i < NUM_LEDS; i++)
            apply_config(i, r, g, b, effect, period_ms);
    } else if (idx == 0xFE) {
        for (int i = 1; i < NUM_LEDS; i++)
            apply_config(i, 0, 0, 0, EFFECT_SOLID, 0);
    } else {
        apply_config((int)idx, r, g, b, effect, period_ms);
    }

    flush_leds();
    pthread_mutex_unlock(&spi_mutex);
}

/* ── Socket listener thread ─────────────────────────────────────────────── */

static void *socket_thread(void *arg)
{
    (void)arg;

    unlink(SOCKET_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return NULL; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return NULL;
    }

    chmod(SOCKET_PATH, 0666);
    listen(server_fd, 5);

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        struct timeval tv = {1, 0};

        if (select(server_fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            continue;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        uint8_t cmd[7];
        ssize_t n;
        while ((n = read(client_fd, cmd, sizeof(cmd))) == (ssize_t)sizeof(cmd))
            handle_command(cmd);

        close(client_fd);
    }

    close(server_fd);
    unlink(SOCKET_PATH);
    return NULL;
}

/* ── Signal handling ────────────────────────────────────────────────────── */

static void signal_handler(int sig)
{
    if (sig == SIGUSR1)                   booted  = 1;
    else if (sig == SIGTERM || sig == SIGINT) running = 0;
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    int ret = gpioInitialise();
    if (ret < 0) {
        fprintf(stderr, "gpioInitialise failed: %d\n", ret);
        return 1;
    }

    spi_handle = spiOpen(0, 2500000, 0, 0, 8, 1, 1);
    if (spi_handle < 0) {
        fprintf(stderr, "spiOpen failed: %d\n", spi_handle);
        gpioTerminate();
        return 1;
    }

    memset(led_state, 0, sizeof(led_state));
    memset(configs,   0, sizeof(configs));
    pthread_mutex_lock(&spi_mutex);
    flush_leds();
    pthread_mutex_unlock(&spi_mutex);

    pthread_t sock_tid, fx_tid;
    pthread_create(&sock_tid, NULL, socket_thread, NULL);
    pthread_create(&fx_tid,   NULL, effect_thread,  NULL);

    /* Blink red on LED 0 until SIGUSR1 */
    int red_on = 0;
    while (!booted && running) {
        red_on = !red_on;
        set_system_led(0, red_on ? 40 : 0, 0);   /* GRB: G=0, R=40, B=0 */
        usleep(BLINK_INTERVAL_US);
    }

    if (running)
        set_system_led(40, 0, 0);   /* GRB: G=40, R=0, B=0 — solid green */

    while (running)
        pause();

    /* Cleanup: clear all LEDs */
    pthread_mutex_lock(&spi_mutex);
    memset(led_state, 0, sizeof(led_state));
    flush_leds();
    pthread_mutex_unlock(&spi_mutex);

    pthread_join(sock_tid, NULL);
    pthread_join(fx_tid,   NULL);
    spiClose(spi_handle);
    gpioTerminate();
    return 0;
}
