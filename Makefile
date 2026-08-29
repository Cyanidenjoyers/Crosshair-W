.PHONY: all clean install uninstall

CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0 gtk-layer-shell-0) -lm
LDFLAGS = $(shell pkg-config --libs gtk+-3.0 gtk-layer-shell-0) -ljson-c

all: crosshaird crosshair-w

crosshaird: crosshaird.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

crosshair-w: crosshair-w.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: all
	install -Dm755 crosshaird "$(DESTDIR)$(PREFIX)/bin/crosshaird"
	install -Dm755 crosshair-w "$(DESTDIR)$(PREFIX)/bin/crosshair-w"

uninstall:
	rm -f /usr/bin/crosshaird
	rm -f /usr/bin/crosshair-w

clean:
	rm -f crosshaird crosshair-w