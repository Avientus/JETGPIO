# Control an LED or group of LEDs on the WS2812B strip.
#
# index constants
uint8 SYSTEM_LED=0     # boot indicator — commands to this index are ignored
uint8 ALL_USER=255     # apply to all user LEDs (1-11)
uint8 CLEAR_ALL=254    # clear all user LEDs (solid black)
#
# effect constants
uint8 SOLID=0          # static colour
uint8 BLINK=1          # on/off toggle at period_ms
uint8 FADE=2           # sine-wave brightness fade at period_ms

uint8   index       # 0=system, 1-11=user, 254=clear all, 255=all user
uint8   r
uint8   g
uint8   b
uint8   effect      # SOLID | BLINK | FADE
uint16  period_ms   # animation period in ms (ignored for SOLID; default 500)
