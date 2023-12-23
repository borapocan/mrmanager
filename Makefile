CC = gcc
CFLAGS = `pkg-config --cflags gtk4`
LIBS = `pkg-config --libs gtk4`

all: mrmanager

mrmanager: mrmanager.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f mrmanager
