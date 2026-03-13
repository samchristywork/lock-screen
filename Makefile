PREFIX ?= /usr/local

CFLAGS = -std=c99 -pedantic -Wall -Wextra -O2 $(shell pkg-config --cflags x11 xrandr xft)
LIBS = $(shell pkg-config --libs x11 xrandr xft) -lpam

all: build/lock

build/lock: lock.c | build
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

build:
	mkdir -p build

install: build/lock
	install -Dm755 build/lock $(PREFIX)/bin/lock
	chown root:root $(PREFIX)/bin/lock
	chmod u+s $(PREFIX)/bin/lock
	install -Dm644 pam/lock /etc/pam.d/lock

uninstall:
	rm -f $(PREFIX)/bin/lock /etc/pam.d/lock

clean:
	rm -rf build

.PHONY: all install uninstall clean
