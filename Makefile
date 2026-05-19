# Makefile for Super-Synth
#
# Dependencies (Debian/Ubuntu):
#   sudo apt install libncurses-dev libasound2-dev libresid-dev
#   (or libresid-builder-dev if the above isn't available)
#
# Adjust RESID_INC / RESID_LIB if reSID is installed in a non-standard path.

CXX      = g++
CC       = gcc
CFLAGS   = -O2 -Wall -Wextra
CXXFLAGS = $(CFLAGS)

# Vendored reSID headers live in ./resid/ (from sidplay-libs 2.1.1 source)
RESID_INC = -I.
# RESID_LIB = -L/usr/local/lib

LIBS     = -lncurses -lasound -lresid-builder -lm -lstdc++

TARGET   = supersynth
OBJS     = supersynth.o resid_wrap.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CFLAGS) $(RESID_LIB) -o $@ $^ $(LIBS)

supersynth.o: supersynth.c
	$(CC) $(CFLAGS) $(RESID_INC) -c -o $@ $<

resid_wrap.o: resid_wrap.cpp
	$(CXX) $(CXXFLAGS) $(RESID_INC) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
