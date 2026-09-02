# Host binaries plus TinyCore remaster. `make` builds chime (WM), cabinet
# (file manager), volicon (tray speaker), and editor (notepad). `make image`
# needs the ISO tooling listed in README.md.
CXX ?= g++
PKG = x11 xrandr xinerama
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += $(shell pkg-config --cflags $(PKG))
LIBS = $(shell pkg-config --libs $(PKG))
DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS := $(shell pkg-config --libs dbus-1 2>/dev/null)
ifneq ($(strip $(DBUS_LIBS)),)
CXXFLAGS += -DCHIME_SNI $(DBUS_CFLAGS)
LIBS += $(DBUS_LIBS)
endif
SRCS = src/main.cpp src/draw.cpp src/wm.cpp src/xdg.cpp src/sni.cpp
HDRS = src/wm.h src/theme.h src/sni.h

PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all wm image run run-dual install uninstall clean

all: chime cabinet volicon editor

wm: chime

chime: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LIBS)

# File manager, tray icon, and notepad only need libX11 (no RandR).
cabinet: src/cabinet.cpp src/theme.h
	$(CXX) $(CXXFLAGS) -o $@ src/cabinet.cpp $(shell pkg-config --libs x11)

volicon: src/volicon.cpp
	$(CXX) $(CXXFLAGS) -o $@ src/volicon.cpp $(shell pkg-config --libs x11)

editor: src/editor.cpp src/theme.h
	$(CXX) $(CXXFLAGS) -o $@ src/editor.cpp $(shell pkg-config --libs x11)

image: chime cabinet volicon editor
	./scripts/mkimage.sh

run: image
	./scripts/run.sh

run-dual: image
	DUAL=1 ./scripts/run.sh

# Host install: binaries under PREFIX, plus an xsessions desktop file so
# SDDM/GDM/LightDM can offer "Chime" on the login screen.
install: all
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" ./scripts/install.sh --from-make

uninstall:
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" ./scripts/install.sh --uninstall

clean:
	rm -f chime cabinet volicon editor win95wm
	rm -f chime.iso chime.img win95.iso win95.img
	rm -rf build
