# button_daemon

## Overview

A C daemon that monitors two GPIO buttons and triggers a systemd `reboot` or `poweroff` when either is held for a configurable duration. The hold requirement prevents accidental triggers from brief or unintended contact.

The daemon is installed as a systemd service and starts early in the boot sequence so the power-off button is available as soon as the system is running.

## Hardware & Wiring

Both buttons are **active-low**: the pin reads 1 at idle and 0 when the button is pressed. Each button pin requires a **10 kΩ pull-up resistor** connected from the pin to 3.3 V. jetgpio does not configure internal pull-ups, so the external resistor is mandatory.

```
3.3V ── 10kΩ ── [GPIO pin] ──┬── [button] ── GND
                              │
                        reads 1 (idle)
                        reads 0 (pressed)
```

### Default pin assignments

| Button  | Pin | Hold duration | Action             |
|---------|-----|---------------|--------------------|
| Reboot  | 29  | 1 second      | systemctl reboot   |
| Power   | 31  | 3 seconds     | systemctl poweroff |

Pins 29 and 31 are available on both the Jetson Nano and Jetson Orin 40-pin headers. Any unused input-capable pin on the 40-pin header can be used instead.

## Behaviour

The interrupt service routine (ISR) catches the **falling edge** (button press) with a 50 ms debounce filter. The main loop then polls the pin to confirm it remains held for the required duration. Releasing the button before the threshold is reached cancels the action — nothing happens. All events (press detected, threshold reached, action triggered) are written to the systemd journal.

| Button | Default pin | Hold duration | Action              |
|--------|-------------|---------------|---------------------|
| Reboot | 29          | 1 second      | systemctl reboot    |
| Power  | 31          | 3 seconds     | systemctl poweroff  |

## Dependencies

The **jetgpio** library must be installed on the system before building:

```
sudo make && sudo make install
```

Run the above from the jetgpio repository root.

## Build & Install

```
cd button_daemon
sudo make
sudo make install
```

`sudo make install` copies the binary and installs the systemd unit file, then enables and starts the service.

## Changing Pins or Hold Durations

Edit the `#define` block at the top of `button_daemon.c`, then rebuild and reinstall:

```c
#define REBOOT_PIN      29
#define POWER_PIN       31
#define REBOOT_HOLD_MS  1000
#define POWER_HOLD_MS   3000
```

## Logs

Follow live output from the daemon:

```
journalctl -u button_daemon -f
```

## Uninstall

```
sudo make uninstall
```

This stops and disables the service, removes the unit file, and deletes the installed binary.
