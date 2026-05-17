CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -ljetgpio

TARGET = button_daemon

$(TARGET): button_daemon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 644 systemd/button_daemon.service /etc/systemd/system/
	systemctl daemon-reload
	systemctl enable button_daemon.service

uninstall:
	systemctl disable --now button_daemon.service || true
	rm -f /usr/local/bin/$(TARGET)
	rm -f /etc/systemd/system/button_daemon.service
	systemctl daemon-reload

clean:
	rm -f $(TARGET)
