CC     = gcc
CFLAGS = `pkg-config --cflags gtk4` -g -std=gnu99 -Wall -Wno-deprecated-declarations
LIBS   = `pkg-config --libs gtk4`

all: mrmanager

mrmanager: mrmanager.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: mrmanager
	install -Dm755 mrmanager /usr/bin/mrmanager
	install -Dm644 mrmanager.desktop /usr/share/applications/mrmanager.desktop

uninstall:
	rm -f /usr/bin/mrmanager
	rm -f /usr/share/applications/mrmanager.desktop

clean:
	rm -f mrmanager

.PHONY: all install uninstall clean
