SOURCES = $(wildcard src/*.c)
OBJECTS = $(SOURCES:.c=.o)

TARGET = obs-wlroots-screencopy

CC = gcc
CFLAGS = -Wno-unused-parameter -Wall -Wextra -std=gnu17 -fPIC
LDFLAGS = -shared
LIBS = -lobs

ifndef PROD
CFLAGS += -g
else
CFLAGS += -O3 -march=native -mtune=native
LDLAGS += -flto=auto
endif

%.o: %.c
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

.PHONY: link run debug clean