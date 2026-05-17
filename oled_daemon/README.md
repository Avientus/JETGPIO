# oled_daemon

## Overview

C daemon that drives a 128x64 SSD1306 OLED display over I2C on a Jetson board. The daemon cycles between screens when a GPIO button is pressed and displays network IP addresses by default. Custom text can be pushed to the display by writing to a file. Runs as a systemd service.

## Implementation Note

The OLED uses Linux `/dev/i2c-*` directly rather than jetgpio's I2C API. This is because jetgpio only supports single-byte writes, which would take approximately 0.8 s per frame. Using `/dev/i2c-*` allows blasting the full 1025-byte framebuffer in a single write, taking approximately 10 ms. The GPIO cycle button still uses jetgpio.

## Hardware & Wiring

**OLED (SSD1306 128x64):**

| OLED Pin | Jetson Pin | Notes |
|----------|------------|-------|
| SDA | Pin 27 | I2C bus 0, `/dev/i2c-1` on Orin |
| SCL | Pin 28 | |
| VCC | 3.3 V | |
| GND | GND | |

**Cycle button:** Active-low with 10 kΩ pull-up to 3.3 V. Default pin: 37.

**Alternative I2C bus:** Pins 3 (SDA) / 5 (SCL) map to `/dev/i2c-7` on Orin.

## Display Layout

5x8 pixel font, 21 characters x 8 rows. A screen indicator is shown in the bottom-right corner.

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

| Screen | Source | Description |
|--------|--------|-------------|
| 0 | `getifaddrs()` | All non-loopback IPv4 addresses, refreshed every 5 s |
| 1 | `/run/oled_text` | Up to 7 lines of text, refreshed every 5 s |

## Updating Custom Text

Write up to 7 lines (21 characters each) to `/run/oled_text` from any script or program. The display refreshes within 5 seconds, or immediately when the button cycles back to that screen.

```bash
echo -e "Robot: READY\nMode: Auto\nBatt: 87%" > /run/oled_text
```

## Dependencies

- jetgpio library installed
- `i2c-dev` kernel module (usually present on Jetson)

## Build & Install

```
cd oled_daemon
sudo make
sudo make install
```

The service waits for `network-online.target` before starting so IP addresses are populated on first draw.

## Configuration

The following defines at the top of `oled_daemon.c` control runtime behaviour:

```c
#define I2C_DEV          "/dev/i2c-1"   // /dev/i2c-7 for pins 3/5
#define I2C_ADDR          0x3C          // 0x3D if ADDR pin is high
#define CYCLE_PIN          37
#define CUSTOM_TEXT_FILE  "/run/oled_text"
#define REFRESH_MS        5000
```

## Uninstall

```
sudo make uninstall
```

The daemon clears the display and turns it off on exit.
