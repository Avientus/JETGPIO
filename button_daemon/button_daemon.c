#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <jetgpio.h>

/* ── Pin configuration ──────────────────────────────────────────────────── *
 * Circuit: button between pin and GND, with a 10kΩ pull-up to 3.3V.
 * Active-low: pin reads 0 when pressed, 1 when released.
 *
 * Any unused input pin on the 40-pin header works.
 * Default assignments:
 *   Pin 15 — reboot   (avoid 29/31: CAN bus on Orin AGX)
 *   Pin 13 — power off (avoid 29/31: CAN bus on Orin AGX)
 */
#define REBOOT_PIN       15
#define POWER_PIN        13

/* ── Timing (milliseconds) ──────────────────────────────────────────────── */
#define DEBOUNCE_MS      50      /* ignore bounces shorter than this */
#define REBOOT_HOLD_MS  1000     /* hold reboot button for 1s  */
#define POWER_HOLD_MS   3000     /* hold power button for 3s   */
#define POLL_MS           10     /* main loop poll interval    */

/* ── ISR flags (set by jetgpio ISR thread, read by main loop) ───────────── */
static volatile int reboot_pressed = 0;
static volatile int power_pressed  = 0;

static unsigned long reboot_ts;
static unsigned long power_ts;

static void reboot_isr(void) { reboot_pressed = 1; }
static void power_isr(void)  { power_pressed  = 1; }

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ── Hold-detection helper ──────────────────────────────────────────────── *
 * Returns 1 if 'pin' stays low for at least 'required_ms', 0 otherwise.
 * Polls at POLL_MS intervals so the loop stays responsive.
 */
static int held_for(int pin, int required_ms)
{
    int elapsed = 0;
    while (gpioRead(pin) == 0 && running) {
        usleep(POLL_MS * 1000);
        elapsed += POLL_MS;
        if (elapsed >= required_ms)
            return 1;
    }
    return 0;
}

/* Wait until pin returns high (button released) */
static void wait_release(int pin)
{
    while (gpioRead(pin) == 0 && running)
        usleep(POLL_MS * 1000);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    int ret = gpioInitialise();
    if (ret < 0) {
        fprintf(stderr, "gpioInitialise failed: %d\n", ret);
        return 1;
    }

    gpioSetMode(REBOOT_PIN, JET_INPUT);
    gpioSetMode(POWER_PIN,  JET_INPUT);

    /* Use falling-edge ISR with debounce to catch initial press.
     * Long-press timing is handled by held_for() in the main loop. */
    gpioSetISRFunc(REBOOT_PIN, FALLING_EDGE, DEBOUNCE_MS * 1000, &reboot_ts, reboot_isr);
    gpioSetISRFunc(POWER_PIN,  FALLING_EDGE, DEBOUNCE_MS * 1000, &power_ts,  power_isr);

    printf("Button daemon running — reboot pin %d, power pin %d\n",
           REBOOT_PIN, POWER_PIN);

    while (running) {
        if (reboot_pressed) {
            reboot_pressed = 0;
            printf("Reboot button: checking hold (%d ms required)...\n", REBOOT_HOLD_MS);
            if (held_for(REBOOT_PIN, REBOOT_HOLD_MS)) {
                printf("Reboot button held — rebooting\n");
                fflush(stdout);
                wait_release(REBOOT_PIN);
                system("systemctl reboot");
            } else {
                printf("Reboot button released early — ignoring\n");
            }
        }

        if (power_pressed) {
            power_pressed = 0;
            printf("Power button: checking hold (%d ms required)...\n", POWER_HOLD_MS);
            if (held_for(POWER_PIN, POWER_HOLD_MS)) {
                printf("Power button held — powering off\n");
                fflush(stdout);
                wait_release(POWER_PIN);
                system("systemctl poweroff");
            } else {
                printf("Power button released early — ignoring\n");
            }
        }

        usleep(POLL_MS * 1000);
    }

    gpioTerminate();
    return 0;
}
