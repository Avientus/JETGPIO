# oled_daemon

## Overview

C daemon that drives a 128×64 SSD1306 OLED display over I2C on a Jetson board. The daemon cycles between screens when a GPIO button is pressed and displays network IP addresses by default. Custom text can be pushed to the display by writing to a file. Runs as a systemd service.

## Implementation Note

The OLED uses Linux `/dev/i2c-*` directly. This allows blasting the full 1025-byte framebuffer in a single write (~10 ms per frame). The GPIO cycle button uses libgpiod via the kernel character device interface (`/dev/gpiochipN`), which avoids the direct hardware register access that caused hard lockups with jetgpio on Orin AGX.

## Hardware & Wiring

**OLED (SSD1306 128×64):**

| OLED pin | Jetson pin | Notes                              |
|----------|------------|------------------------------------|
| SDA      | Pin 3      | I2C bus → `/dev/i2c-7` on Orin     |
| SCL      | Pin 5      |                                    |
| VCC      | 3.3 V      |                                    |
| GND      | GND        |                                    |

Alternative I2C bus: pins 27 (SDA) / 28 (SCL) → `/dev/i2c-1`. Avoid on Orin — this bus is shared with system devices and disrupts networking.

**Cycle button:** Active-low. Default pin: 18.

Pull-up resistors can be provided either internally (via the GPIO driver) or externally:

- **Internal pull-up (default):** `USE_INTERNAL_PULLUP 1` in `oled_daemon.c`. Requires kernel ≥ 5.5 and a GPIO driver that supports bias configuration. Works on Jetson with JetPack 5 and 6.
- **External pull-up:** Set `USE_INTERNAL_PULLUP 0` and wire a 10 kΩ resistor from the button pin to 3.3 V.

```
3.3V ── 10kΩ ── [GPIO pin] ──┬── [button] ── GND
                              │
                        reads 1 (idle)
                        reads 0 (pressed)
```

## Finding the GPIO line offset

The daemon addresses the button GPIO by its offset within the `tegra-gpio` chip, **not** by 40-pin header number. Find the correct offset on the Jetson before building:

```bash
gpioinfo tegra-gpio | grep G4_SOC_GPIO21   # pin 18 on Orin AGX
gpioinfo tegra-gpio | grep G2_SPI3_CS0     # pin 18 on Orin Nano/NX
```

Update `GPIO_LINE_OFFSET` in `oled_daemon.c` to match.

## Display Layout

5×8 pixel font, 21 characters × 8 rows. A screen indicator is shown in the bottom-right corner.

**Screen 0 — Network IPs:**
```
-- NETWORK IPS --
eth0  192.168.1.42
wlan0 10.0.0.5

                 1/2
```

**Screen 1 — Custom text:**
```
Robot: READY
Mode: Autonomous
Batt: 87%

                 2/2
```

## Screens

| Screen | Source            | Description                                             |
|--------|-------------------|---------------------------------------------------------|
| 0      | `getifaddrs()`    | All non-loopback IPv4 addresses, refreshed every 5 s    |
| 1      | `/run/oled_text`  | Up to 7 lines of text, refreshed every 5 s              |

## Updating Custom Text

Write up to 7 lines (21 characters each) to `/run/oled_text`:

```bash
echo -e "Robot: READY\nMode: Auto\nBatt: 87%" > /run/oled_text
```

The display refreshes within 5 seconds, or immediately when the button cycles to that screen.

## Dependencies

- `libgpiod` for the cycle button
- `i2c-dev` kernel module for the OLED (usually present on Jetson)

```bash
sudo apt install libgpiod-dev
```

## Build & Install

```bash
cd oled_daemon
sudo make
sudo make install
```

The service waits for `network-online.target` before starting so IP addresses are populated on first draw.

## Configuration

Edit the `#define` block at the top of `oled_daemon.c`, then rebuild and reinstall:

```c
#define I2C_DEV             "/dev/i2c-7"   /* /dev/i2c-1 for pins 27/28 */
#define I2C_ADDR             0x3C          /* 0x3D if ADDR pin is high   */

#define GPIO_CHIP_LABEL     "tegra-gpio"
#define GPIO_LINE_OFFSET     106           /* verify with gpioinfo       */

#define USE_INTERNAL_PULLUP  1             /* 0 = use external pull-up   */

#define CUSTOM_TEXT_FILE    "/run/oled_text"
#define REFRESH_MS           5000
```

## Uninstall

```bash
sudo make uninstall
```

The daemon clears the display and turns it off on exit.
