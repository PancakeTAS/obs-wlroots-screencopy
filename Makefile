SOURCES = $(wildcard src/*.c) protocols/wlroots/wlr-screencopy-unstable-v1.c protocols/wayland/linux-dmabuf-unstable-v1.c
OBJECTS = $(SOURCES:.c=.o)

TARGET = obs-wlroots-screencopy

CC = gcc
CFLAGS = -Wno-unused-parameter -Wall -Wextra -std=gnu17 -fPIC -Iprotocols
LDFLAGS = -shared
LIBS = -lobs -lwayland-client -lgbm

ifndef PROD
CFLAGS += -g
else
CFLAGS += -O2 -march=native -mtune=native
LDLAGS += -flto=auto
endif

all: protocols $(TARGET).so

# protocol prepare targets
protocols: protocols/wlroots/wlr-screencopy-unstable-v1.h protocols/wayland/linux-dmabuf-unstable-v1.h

protocols/wlroots/wlr-screencopy-unstable-v1.c: /usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml
	mkdir -p protocols/wlroots
	wayland-scanner private-code /usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml protocols/wlroots/wlr-screencopy-unstable-v1.c

protocols/wayland/linux-dmabuf-unstable-v1.c: /usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
	mkdir -p protocols/wayland
	wayland-scanner private-code /usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml protocols/wayland/linux-dmabuf-unstable-v1.c

protocols/wlroots/wlr-screencopy-unstable-v1.h: /usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml
	mkdir -p protocols/wlroots
	wayland-scanner client-header /usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml protocols/wlroots/wlr-screencopy-unstable-v1.h

protocols/wayland/linux-dmabuf-unstable-v1.h: /usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
	mkdir -p protocols/wayland
	wayland-scanner client-header /usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml protocols/wayland/linux-dmabuf-unstable-v1.h

# compile targets
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).so: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

# install target
install: $(TARGET).so
	mkdir -p "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit"
	ln -s "$(PWD)/$(TARGET).so" "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit/$(TARGET).so"

# obs launch targets
run: $(TARGET).so
	obs

debug: $(TARGET).so
	gdb obs

# clean target
clean:
	rm -f $(OBJECTS) $(TARGET).so
	rm -rf protocols

.PHONY: all clean run debug link scanner