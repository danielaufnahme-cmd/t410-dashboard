# t410dash - see README.md
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=

CC      ?= cc
PKG     ?= pkg-config
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -pedantic $(shell $(PKG) --cflags x11 xft)
LDLIBS  += $(shell $(PKG) --libs x11 xft) -lm

SRC = src/main.c src/config.c src/json.c src/weather.c src/thermal.c
OBJ = $(SRC:.c=.o)
BIN = t410dash

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all install uninstall clean
