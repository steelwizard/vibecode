CXX ?= g++
PKG = x11 xrandr xinerama
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += $(shell pkg-config --cflags $(PKG))
LIBS = $(shell pkg-config --libs $(PKG))
SRCS = src/main.cpp src/draw.cpp src/wm.cpp
HDRS = src/wm.h src/theme.h

.PHONY: all wm image run run-dual clean

all: chime cabinet

wm: chime

chime: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LIBS)

cabinet: src/cabinet.cpp src/theme.h
	$(CXX) $(CXXFLAGS) -o $@ src/cabinet.cpp $(shell pkg-config --libs x11)

image: chime cabinet
	./scripts/mkimage.sh

run: image
	./scripts/run.sh

run-dual: image
	DUAL=1 ./scripts/run.sh

clean:
	rm -f chime cabinet win95wm
	rm -f chime.iso chime.img win95.iso win95.img
	rm -rf build
