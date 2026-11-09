# wled-audiosync
#
#   make              build/wled-audiosync
#   make test         build and run the selftest (no audio hardware, no network needed)
#   make CC=aarch64-linux-gnu-gcc    cross build, e.g. for a Raspberry Pi
#
# Needs a C99 compiler with GNU extensions (gcc) and libm. Linux only (uses pipe2, poll).
BUILD ?= build
CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS ?= -lm
BIN = $(BUILD)/wled-audiosync

all: $(BIN)

$(BIN): src/wled_audiosync.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu11 src/wled_audiosync.c -o $@ $(LDLIBS)

test: $(BIN)
	$(BIN) --selftest

clean:
	$(RM) -r $(BUILD)

.PHONY: all test clean
