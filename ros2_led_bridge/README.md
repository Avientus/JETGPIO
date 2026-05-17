# ros2_led_bridge

## Overview

ROS2 Humble package that bridges the `/led_strip` topic to the `led_daemon` Unix socket. Provides a custom `LedCommand` message type. Other ROS2 nodes publish to `/led_strip` to control individual LEDs or groups with optional animation effects. The `led_daemon` (`../led_daemon`) must be running for this to work.

## Architecture

```
[Your ROS2 nodes]
      │ publish LedCommand to /led_strip
      ▼
[led_bridge_node] ── persistent Unix socket ──▶ [led_daemon] ── SPI ──▶ [WS2812B strip]
```

Only one process (`led_daemon`) owns the SPI device. The bridge node reconnects to the socket automatically if the daemon restarts.

## Dependencies

- ROS2 Humble
- `led_daemon` running (`../led_daemon`)
- colcon build tools

## Build

```bash
cp -r ros2_led_bridge ~/ros2_ws/src/
cd ~/ros2_ws
colcon build --packages-select ros2_led_bridge
source install/setup.bash
```

## Running

```bash
ros2 run ros2_led_bridge led_bridge_node
```

## LedCommand Message

Definition (`msg/LedCommand.msg`):

```
uint8   index       # 0=system LED (ignored), 1-11=user, 254=clear all, 255=all user
uint8   r
uint8   g
uint8   b
uint8   effect      # 0=SOLID, 1=BLINK, 2=FADE
uint16  period_ms   # animation period in ms (ignored for SOLID; 0 = default 500 ms)
```

Message constants (usable from code):

| Constant  | Value | Meaning                        |
|-----------|-------|--------------------------------|
| SOLID     | 0     | Static colour                  |
| BLINK     | 1     | On/off at period_ms            |
| FADE      | 2     | Sine-wave brightness at period_ms |
| ALL_USER  | 255   | Apply to LEDs 1–11             |
| CLEAR_ALL | 254   | Turn off LEDs 1–11             |

## CLI Examples

```bash
# Solid blue on LED 3
ros2 topic pub --once /led_strip ros2_led_bridge/msg/LedCommand \
  "{index: 3, r: 0, g: 0, b: 200, effect: 0, period_ms: 0}"

# Blink red on LED 5 at 2 Hz (500 ms period)
ros2 topic pub --once /led_strip ros2_led_bridge/msg/LedCommand \
  "{index: 5, r: 200, g: 0, b: 0, effect: 1, period_ms: 500}"

# Fade green across all user LEDs, 1-second cycle
ros2 topic pub --once /led_strip ros2_led_bridge/msg/LedCommand \
  "{index: 255, r: 0, g: 150, b: 0, effect: 2, period_ms: 1000}"

# Clear all user LEDs
ros2 topic pub --once /led_strip ros2_led_bridge/msg/LedCommand \
  "{index: 254, r: 0, g: 0, b: 0, effect: 0, period_ms: 0}"
```

## C++ Publisher Example

```cpp
#include "ros2_led_bridge/msg/led_command.hpp"
using LedCommand = ros2_led_bridge::msg::LedCommand;

// In your node:
auto pub = create_publisher<LedCommand>("/led_strip", 10);

auto msg = LedCommand();
msg.index     = LedCommand::ALL_USER;
msg.r = 0; msg.g = 150; msg.b = 0;
msg.effect    = LedCommand::FADE;
msg.period_ms = 1000;
pub->publish(msg);
```

## LED 0 Note

LED 0 is the system boot indicator managed exclusively by `led_daemon`. Commands targeting index 0 are silently ignored by the daemon regardless of what is published.
