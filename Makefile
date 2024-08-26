SOURCES = $(wildcard src/*.c src/wlr-protocols/*.c src/wayland-protocols/*.c)
OBJECTS = $(SOURCES:.c=.o)

TARGET = obs-wlroots-screencopy

CC = gcc
CFLAGS = -Wno-unused-parameter -Wall -Wextra -std=gnu17 -fPIC
LDFLAGS = -shared
LIBS = -lobs -lwayland-client -lgbm

ifndef PROD
CFLAGS += -g
else
CFLAGS += -O3 -march=native -mtune=native
LDLAGS += -flto=auto
endif

all: $(TARGET).so

scanner: /usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml
	mkdir -p src/wlr-protocols
	mkdir -p src/wayland-protocols
	wayland-scanner client-header /usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml src/wlr-protocols/wlr-screencopy-unstable-v1.h
	wayland-scanner private-code /usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml src/wlr-protocols/wlr-screencopy-unstable-v1.c
	wayland-scanner client-header /usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml src/wayland-protocols/linux-dmabuf-unstable-v1.h
	wayland-scanner private-code /usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml src/wayland-protocols/linux-dmabuf-unstable-v1.c

%.o: %.c scanner
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).so: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

link: $(TARGET).so
	mkdir -p "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit"
	ln -s "$(PWD)/$(TARGET).so" "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit/$(TARGET).so"

run: $(TARGET).so
	obs

debug: $(TARGET).so
	gdb obs

clean:
	rm -f $(OBJECTS) $(TARGET).so

.PHONY: all clean run debug link scanner