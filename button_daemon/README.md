# button_daemon

## Overview

A C daemon that monitors two GPIO buttons and triggers a systemd `reboot` or `poweroff` when either is held for a configurable duration. The hold requirement prevents accidental triggers from brief or unintended contact.

The daemon is installed as a systemd service and starts early in the boot sequence so the power-off button is available as soon as the system is running.

## Hardware & Wiring

Both buttons are **active-low**: the pin reads 1 at idle and 0 when the button is pressed.

Pull-up resistors can be provided either internally (via the GPIO driver) or externally:

- **Internal pull-up (default):** `USE_INTERNAL_PULLUP 1` in `button_daemon.c`. Requires kernel ≥ 5.5 and a GPIO driver that supports bias configuration. Works on Jetson with JetPack 5 and 6.
- **External pull-up:** Set `USE_INTERNAL_PULLUP 0` and wire a 10 kΩ resistor from each button pin to 3.3 V.

```
3.3V ── 10kΩ ── [GPIO pin] ──┬── [button] ── GND
                              │
                        reads 1 (idle)
                        reads 0 (pressed)
```

### Default pin assignments

| Button | Header pin | Hold duration | Action             |
|--------|------------|---------------|--------------------|
| Reboot | 15         | 1 second      | systemctl reboot   |
| Power  | 13         | 3 seconds     | systemctl poweroff |

Avoid pins 29, 31, and 37 on the Orin AGX — these are used by the CAN bus.

## Dependencies

- `libgpiod` (replaces jetgpio for GPIO access)

```
sudo apt install libgpiod-dev
```

## Finding GPIO line offsets

The daemon addresses GPIO lines by their offset within the `tegra-gpio` chip, **not** by 40-pin header number. Find the correct offsets on the Jetson before building:

```bash
# List all lines with their offsets
gpioinfo tegra-gpio

# Find a specific line by signal name
gpioinfo tegra-gpio | grep EDP_SOC_GPIO39   # pin 15 on Orin AGX
gpioinfo tegra-gpio | grep G3_SOC_GPIO37    # pin 13 on Orin AGX
```

Update `REBOOT_LINE_OFFSET` and `POWER_LINE_OFFSET` in `button_daemon.c` to match.

## Behaviour

Each button runs in a dedicated thread. A **falling-edge event** (button press) wakes the thread, which then polls the line level to confirm the button stays held for the required duration. Releasing before the threshold cancels the action silently. All events are written to the systemd journal.

## Build & Install

```bash
cd button_daemon
sudo make
sudo make install
```

`sudo make install` copies the binary, installs the systemd unit file, then enables and starts the service.

## Configuration

Edit the `#define` block at the top of `button_daemon.c`, then rebuild and reinstall:

```c
#define GPIO_CHIP_LABEL      "tegra-gpio"
#define REBOOT_LINE_OFFSET    88      /* verify with gpioinfo */
#define POWER_LINE_OFFSET     80      /* verify with gpioinfo */

#define USE_INTERNAL_PULLUP   1       /* 0 = use external pull-up resistors */

#define REBOOT_HOLD_MS       1000
#define POWER_HOLD_MS        3000
```

## Logs

Follow live output from the daemon:

```bash
journalctl -u button_daemon -f
```

## Uninstall

```bash
sudo make uninstall
```

Stops and disables the service, removes the unit file, and deletes the installed binary.
