CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -ljetgpio -lpthread -lm

TARGET = led_daemon

$(TARGET): led_daemon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 644 systemd/led_daemon.service /etc/systemd/system/
	install -m 644 systemd/led_ready.service  /etc/systemd/system/
	systemctl daemon-reload
	systemctl enable led_daemon.service led_ready.service

uninstall:
	systemctl disable --now led_daemon.service led_ready.service || true
	rm -f /usr/local/bin/$(TARGET)
	rm -f /etc/systemd/system/led_daemon.service
	rm -f /etc/systemd/system/led_ready.service
	systemctl daemon-reload

clean:
	rm -f $(TARGET)
