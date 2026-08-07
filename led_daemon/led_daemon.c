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
#include <dirent.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>

#include <jetgpio.h>

#define NUM_LEDS         12
#define BYTES_PER_LED    24
#define SPI_BUF_SIZE     (NUM_LEDS * BYTES_PER_LED)
#define SOCKET_PATH      "/run/leds.sock"
#define BLINK_INTERVAL_US  500000
#define EFFECT_TICK_MS   20      /* effect engine update rate (~50 Hz) */
#define NET_LED_INDEX    1       /* LED 1 reserved for network status */
#define NET_POLL_INTERVAL_S 5   /* network state poll period */
#define INTERNET_CHECK_IP   "8.8.8.8"
#define INTERNET_CHECK_PORT 53

/* ── Effect definitions ─────────────────────────────────────────────────── *
 *
 * Socket protocol: 7 bytes per command
 *   [index, r, g, b, effect, period_ms_hi, period_ms_lo]
 *
 *   index  0     : system LED (boot indicator) — always rejected
 *   index  1     : network status LED — always rejected (managed internally)
 *   index  2-11  : individual user LEDs
 *   index  0xFF  : apply to all user LEDs (2-11)
 *   index  0xFE  : clear all user LEDs (solid black, 2-11)
 *
 *   effect 0 : SOLID  — static colour
 *   effect 1 : BLINK  — on/off at period_ms
 *   effect 2 : FADE   — sine-wave brightness at period_ms
 *
 * LED 1 network status colours (managed by net_thread):
 *   off    — no WiFi or Ethernet interface up
 *   blue   — WiFi connected (no Ethernet, no internet)
 *   yellow — Ethernet connected (no internet)
 *   green  — internet reachable (overrides all)
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

/* forward declaration — defined in "Socket handler" section below */
static void apply_config(int i, uint8_t r, uint8_t g, uint8_t b,
                         uint8_t effect, uint16_t period_ms);

/* ── Network status monitor ─────────────────────────────────────────────── */

typedef enum { NET_NONE, NET_WIFI, NET_ETHERNET, NET_INTERNET } NetState;

/* Returns 1 if the interface has a real hardware device (excludes lo, veth, docker, etc.) */
static int is_physical(const char *iface)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", iface);
    struct stat st;
    return stat(path, &st) == 0;
}

/* Returns 1 if the interface is wireless (has /sys/class/net/<iface>/wireless dir) */
static int is_wireless(const char *iface)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", iface);
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Returns 1 if the interface has an assigned IPv4 address (true active connection) */
static int has_ip(const char *iface)
{
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) < 0) return 0;
    int found = 0;
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, iface) == 0) { found = 1; break; }
    }
    freeifaddrs(ifap);
    return found;
}

/* Non-blocking TCP connect to INTERNET_CHECK_IP:PORT; returns 1 on success */
static int has_internet(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(INTERNET_CHECK_PORT);
    inet_pton(AF_INET, INTERNET_CHECK_IP, &addr.sin_addr);

    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) { close(sock); return 1; }
    if (errno != EINPROGRESS) { close(sock); return 0; }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = {2, 0};
    int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (sel <= 0) { close(sock); return 0; }

    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen);
    close(sock);
    return err == 0;
}

static NetState check_network(void)
{
    int has_eth = 0, has_wifi = 0;

    DIR *d = opendir("/sys/class/net");
    if (!d) return NET_NONE;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (!is_physical(name) || !has_ip(name)) continue;
        if (is_wireless(name))
            has_wifi = 1;
        else
            has_eth = 1;
    }
    closedir(d);

    if (!has_eth && !has_wifi) return NET_NONE;
    if (has_internet())        return NET_INTERNET;
    if (has_eth)               return NET_ETHERNET;
    return NET_WIFI;
}

static void set_net_led(NetState state)
{
    uint8_t r, g, b;
    switch (state) {
        case NET_INTERNET: r =  0; g = 40; b =  0; break;  /* green  */
        case NET_ETHERNET: r = 40; g = 40; b =  0; break;  /* yellow */
        case NET_WIFI:     r =  0; g =  0; b = 40; break;  /* blue   */
        default:           r =  0; g =  0; b =  0; break;  /* off    */
    }
    pthread_mutex_lock(&spi_mutex);
    apply_config(NET_LED_INDEX, r, g, b, EFFECT_SOLID, 0);
    flush_leds();
    pthread_mutex_unlock(&spi_mutex);
}

static void *net_thread(void *arg)
{
    (void)arg;
    while (running) {
        set_net_led(check_network());
        /* Sleep in 100 ms slices so we respond promptly to running=0 */
        for (int i = 0; i < NET_POLL_INTERVAL_S * 10 && running; i++)
            usleep(100000);
    }
    return NULL;
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

    if (idx == 0 || idx == NET_LED_INDEX ||
        (idx > (uint8_t)(NUM_LEDS - 1) && idx != 0xFF && idx != 0xFE))
        return;

    pthread_mutex_lock(&spi_mutex);

    if (idx == 0xFF) {
        for (int i = NET_LED_INDEX + 1; i < NUM_LEDS; i++)
            apply_config(i, r, g, b, effect, period_ms);
    } else if (idx == 0xFE) {
        for (int i = NET_LED_INDEX + 1; i < NUM_LEDS; i++)
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

    pthread_t sock_tid, fx_tid, net_tid;
    pthread_create(&sock_tid, NULL, socket_thread, NULL);
    pthread_create(&fx_tid,   NULL, effect_thread,  NULL);
    pthread_create(&net_tid,  NULL, net_thread,     NULL);

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

    pthread_join(sock_tid, NULL);
    pthread_join(fx_tid,   NULL);
    pthread_join(net_tid,  NULL);

    /* Set LED 0 solid red: Jetson is off.
     * WS2812B holds this colour while the strip has 5V from the drone battery. */
    pthread_mutex_lock(&spi_mutex);
    memset(led_state, 0, sizeof(led_state));
    led_state[0][1] = 40;   /* GRB layout: index 1 is red */
    flush_leds();
    pthread_mutex_unlock(&spi_mutex);

    spiClose(spi_handle);
    gpioTerminate();
    return 0;
}
