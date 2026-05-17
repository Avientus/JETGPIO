# led_daemon

## Overview

`led_daemon` is a C daemon that owns the SPI bus and drives a 12-LED WS2812B strip. It runs as a systemd service and fulfills two roles:

1. **Boot indicator (LED 0):** Blinks red while the system is booting, then turns solid green once the system is ready.
2. **User LED controller (LEDs 1–11):** Accepts color and effect commands from other processes via a Unix domain socket at `/run/leds.sock`.

## Hardware & Wiring

The daemon uses the SPI0 peripheral. Connect the WS2812B strip as follows:

| Jetson Pin | Signal | WS2812B |
|------------|--------|---------|
| 19         | MOSI   | DIN     |
| 21         | MISO   | —       |
| 23         | SCK    | —       |
| 24         | CS0    | —       |

- Power the strip from a **5 V / GND** supply (not from the Jetson GPIO header unless current draw is within spec).
- Place a **300–500 Ω series resistor** on the data line (pin 19 → DIN) to protect against signal reflections and overvoltage.

## Dependencies

- **jetgpio** library — build and install from the repository root:
  ```
  sudo make && sudo make install
  ```
- **gcc**, **pthread**, **libm** — available in standard build toolchains (`sudo apt install build-essential`).

## Build & Install

All build and install steps must be run as root or with `sudo`.

```
cd led_daemon
sudo make
sudo make install
```

`sudo make install` copies the binary to `/usr/local/bin` and enables two systemd services:

- **`led_daemon.service`** — starts early in the boot sequence and owns the SPI bus for the lifetime of the session.
- **`led_ready.service`** — sends `SIGUSR1` to `led_daemon` when `multi-user.target` is reached, triggering the green "system ready" state on LED 0.

If you want the green indicator to mean "ROS2 ready" instead of "multi-user reached", edit the `After=` directive in `led_ready.service` to point to your ROS2 launch service before installing.

## LED Allocation

| Index   | Owner          | Description                                          |
|---------|----------------|------------------------------------------------------|
| 0       | System         | Boot indicator — commands targeting index 0 are silently ignored |
| 1 – 11  | User           | Freely controllable via the socket protocol          |

## Socket Protocol

Connect to `/run/leds.sock` (type `SOCK_STREAM`) and send **7-byte** command packets:

```
[ index, r, g, b, effect, period_ms_hi, period_ms_lo ]
```

| Byte | Field         | Description                                            |
|------|---------------|--------------------------------------------------------|
| 0    | `index`       | LED index (1–11), or special value (see below)         |
| 1    | `r`           | Red channel (0–255)                                    |
| 2    | `g`           | Green channel (0–255)                                  |
| 3    | `b`           | Blue channel (0–255)                                   |
| 4    | `effect`      | Effect type (see Effects table)                        |
| 5    | `period_ms_hi`| High byte of period in milliseconds (big-endian)       |
| 6    | `period_ms_lo`| Low byte of period in milliseconds (big-endian)        |

**Special index values:**

| Value  | Meaning                         |
|--------|---------------------------------|
| `0xFF` | Apply command to all user LEDs (1–11) |
| `0xFE` | Clear all user LEDs (turn off)  |

`period_ms` is a `uint16` in big-endian byte order across bytes 5–6. Set both bytes to `0` to use the default period of **500 ms**.

## Effects

| Value | Name    | Behaviour                              |
|-------|---------|----------------------------------------|
| 0     | `SOLID` | Static colour, no animation            |
| 1     | `BLINK` | On/off toggle at the specified period  |
| 2     | `FADE`  | Sine-wave brightness at the specified period |

## Uninstall

```
sudo make uninstall
```

Stops and disables both `led_daemon.service` and `led_ready.service`, then removes the installed binary and service files.

## Integration with ros2_led_bridge

See the `ros2_led_bridge` package in `../ros2_led_bridge`. It provides a ROS2 node and a `LedCommand` message type that handle the socket protocol automatically, so ROS2 nodes can control the LED strip without managing raw socket communication.
