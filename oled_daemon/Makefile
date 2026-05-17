CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -ljetgpio

TARGET = oled_daemon

$(TARGET): oled_daemon.c ssd1306.h
	$(CC) $(CFLAGS) -o $@ oled_daemon.c $(LDFLAGS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 644 systemd/oled_daemon.service /etc/systemd/system/
	systemctl daemon-reload
	systemctl enable oled_daemon.service

uninstall:
	systemctl disable --now oled_daemon.service || true
	rm -f /usr/local/bin/$(TARGET)
	rm -f /etc/systemd/system/oled_daemon.service
	systemctl daemon-reload

clean:
	rm -f $(TARGET)
