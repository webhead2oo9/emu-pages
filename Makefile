CORE_NAME := emu_pages

# Source files
SOURCES := src/emu_pages.c src/renderer.c

# Detect platform
UNAME := $(shell uname -s)

ifeq ($(UNAME), Darwin)
    TARGET := $(CORE_NAME)_libretro.dylib
    SHARED := -dynamiclib
    PLATFORM_FLAGS := -mmacosx-version-min=10.13
else ifeq ($(UNAME), Linux)
    TARGET := $(CORE_NAME)_libretro.so
    SHARED := -shared -Wl,--no-undefined
    PLATFORM_FLAGS :=
else
    # Windows (MSYS2/MinGW)
    TARGET := $(CORE_NAME)_libretro.dll
    SHARED := -shared
    PLATFORM_FLAGS :=
endif

CC      := cc
CFLAGS  := -Wall -Wextra -O2 -fPIC $(PLATFORM_FLAGS)
LDFLAGS := $(SHARED)

OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean install wiki-data sprite deps

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Fetch libretro.h from RetroArch v1.7.5
deps:
	curl -sL -o src/libretro.h \
	  https://raw.githubusercontent.com/libretro/RetroArch/v1.7.5/libretro-common/include/libretro.h

# Fetch wiki data and regenerate C header (requires network access)
wiki-data:
	python3 tools/fetch_wiki.py

# Convert mascot sprite to C header (requires Pillow)
sprite:
	python3 tools/convert_sprite.py

clean:
	rm -f $(OBJECTS) $(TARGET)

# Install core + ROM to RetroArch directories
install: $(TARGET)
ifeq ($(UNAME), Darwin)
	cp $(TARGET) ~/Library/Application\ Support/RetroArch/cores/
	cp $(CORE_NAME)_libretro.info ~/Library/Application\ Support/RetroArch/info/ 2>/dev/null || true
	cp emuvr.emupages ~/Library/Application\ Support/RetroArch/content/ 2>/dev/null || true
else
	cp $(TARGET) ~/.config/retroarch/cores/
	cp $(CORE_NAME)_libretro.info ~/.config/retroarch/info/ 2>/dev/null || true
endif
	@echo "Installed $(TARGET)"
