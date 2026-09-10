# Host binaries plus TinyCore remaster. `make` builds chime (WM), cabinet
# (file manager), volicon (tray speaker), and editor (notepad). `make image`
# rebuilds the WM without wlroots so the live ISO stays X11-only.
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

# Wayland/wlroots: on if headers + lib are present, unless CHIME_WL=0.
WL_PC := $(firstword $(foreach p,wlroots-0.19 wlroots,$(if $(shell pkg-config --exists $(p) && echo y),$(p))))
HAVE_WL_DEPS := $(wildcard deps/wlroots-0.19/wlr/backend.h)
HAVE_WL_LIB := $(wildcard /usr/lib/*/libwlroots-0.19.so)
ifeq ($(origin CHIME_WL), undefined)
  ifneq ($(shell command -v wayland-scanner 2>/dev/null),)
    ifneq ($(WL_PC),)
      CHIME_WL := 1
    else ifneq ($(HAVE_WL_DEPS),)
      ifneq ($(HAVE_WL_LIB),)
        CHIME_WL := 1
      endif
    endif
  endif
endif
CHIME_WL ?= 0

SRCS = src/main.cpp src/draw.cpp src/wm.cpp src/xdg.cpp src/sni.cpp src/plat.cpp
HDRS = src/wm.h src/theme.h src/sni.h src/plat_wl.h
WL_PROTO =

ifeq ($(CHIME_WL),1)
SRCS += src/plat_wl.cpp
CXXFLAGS += -DCHIME_WAYLAND -DWLR_USE_UNSTABLE
# System wlroots headers are C-only (`float [static 4]`, `char *class`). Use
# the C++-safe copies in deps/ and only take libs from pkg-config.
CXXFLAGS += -Ideps/wlroots-0.19 -Ideps/xcb
CXXFLAGS += $(shell pkg-config --cflags pixman-1 libdrm wayland-server xkbcommon 2>/dev/null)
ifneq ($(WL_PC),)
LIBS += $(shell pkg-config --libs $(WL_PC))
else
LIBS += -lwlroots-0.19
endif
CXXFLAGS += -Ibuild
LIBS += -lwayland-server -lxkbcommon
WL_PROTO = build/xdg-shell-protocol.h
WAYLAND_PROTOCOLS ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_XML = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
endif

PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all wm image run run-dual install uninstall wayland-session clean

all: chime cabinet volicon editor

wm: chime

# Rebuild if Wayland linkage flips (an X11-only ./chime looks up-to-date after
# installing libwlroots, which is how the login session used to dump to a TTY).
WL_STAMP := build/chime-wl
$(shell mkdir -p build && printf '%s\n' '$(CHIME_WL)' > $(WL_STAMP).tmp && { cmp -s $(WL_STAMP) $(WL_STAMP).tmp || mv $(WL_STAMP).tmp $(WL_STAMP); } ; rm -f $(WL_STAMP).tmp)

build/xdg-shell-protocol.h:
	mkdir -p build
	wayland-scanner server-header "$(XDG_XML)" $@

chime: $(SRCS) $(HDRS) $(WL_PROTO) $(WL_STAMP)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LIBS)

# File manager, tray icon, and notepad only need libX11 (no RandR).
cabinet: src/cabinet.cpp src/theme.h
	$(CXX) $(CXXFLAGS) -o $@ src/cabinet.cpp $(shell pkg-config --libs x11)

volicon: src/volicon.cpp
	$(CXX) $(CXXFLAGS) -o $@ src/volicon.cpp $(shell pkg-config --libs x11)

editor: src/editor.cpp src/theme.h
	$(CXX) $(CXXFLAGS) -o $@ src/editor.cpp $(shell pkg-config --libs x11)

# TinyCore/Xfbdev has no wlroots; force an X11-only binary into the ISO.
image:
	rm -f chime
	$(MAKE) CHIME_WL=0 chime cabinet volicon editor
	./scripts/mkimage.sh

run: image
	./scripts/run.sh

run-dual: image
	DUAL=1 ./scripts/run.sh

# Host install: binaries under PREFIX, plus xsessions / wayland-sessions
# desktop files so SDDM/GDM/LightDM can offer "Chime".
install: all
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" ./scripts/install.sh --from-make

uninstall:
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" ./scripts/install.sh --uninstall

# Register this tree's ./chime as "Chime (Wayland)" on the login screen.
wayland-session:
	$(MAKE) CHIME_WL=1 chime cabinet volicon editor
	./scripts/install.sh --local-wayland --skip-build --skip-deps

clean:
	rm -f chime cabinet volicon editor win95wm
	rm -f chime.iso chime.img win95.iso win95.img
	rm -rf build
