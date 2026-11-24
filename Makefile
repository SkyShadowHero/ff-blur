# Build the ext-background-effect-v1 runtime patch for Firefox (or any client).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -fPIC
LDFLAGS ?= -shared -Wl,-z,nodelete
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client)
WAYLAND_LIBS   := $(shell pkg-config --libs wayland-client)

all: libffblur.so

ext-background-effect-v1-client-protocol.h: ext-background-effect-v1.xml
	wayland-scanner client-header $< $@

ext-background-effect-v1-protocol.c: ext-background-effect-v1.xml
	wayland-scanner private-code $< $@

libffblur.so: ffblur.c ext-background-effect-v1-client-protocol.h ext-background-effect-v1-protocol.c
	$(CC) $(CFLAGS) $(WAYLAND_CFLAGS) -o $@ ffblur.c ext-background-effect-v1-protocol.c $(LDFLAGS) $(WAYLAND_LIBS) -ldl -lm

clean:
	rm -f libffblur.so ext-background-effect-v1-client-protocol.h ext-background-effect-v1-protocol.c

.PHONY: all clean
