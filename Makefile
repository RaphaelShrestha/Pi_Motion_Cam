# pi-motion-cam

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wshadow -Wpointer-arith
CFLAGS  += -D_DEFAULT_SOURCE
LDFLAGS ?=
LDLIBS  ?= -lm

BIN     := build/pi-motion-cam
OBJDIR  := build/obj

# All sources live in the repo root. test_motion.c has its own main(), so
# it is excluded from the normal object list and linked only by `make test`.
SRCS := $(filter-out test_motion.c,$(wildcard *.c))
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

ifeq ($(DEBUG),1)
CFLAGS += -O0 -g -DDEBUG
else
CFLAGS += -O2
endif

# Vectorised inner loops for the difference and blur passes. Only useful
# on the Pi; the compiler will reject it elsewhere.
ifeq ($(NEON),1)
CFLAGS += -mfpu=neon -ftree-vectorize
endif

PREFIX ?= /usr/local

.PHONY: all clean install uninstall test

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

test: build/test_motion
	./build/test_motion

build/test_motion: test_motion.c $(filter-out $(OBJDIR)/main.o,$(OBJS))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/pi-motion-cam

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pi-motion-cam

clean:
	rm -rf build

-include $(DEPS)
