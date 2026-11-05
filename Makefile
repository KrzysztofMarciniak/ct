CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wno-missing-field-initializers -I/usr/include/freetype2
LDLIBS  += -lX11 -lXft -lpng -ljpeg -lwebp -lutil -lm
MPV_CFLAGS := $(shell pkg-config --cflags mpv)
MPV_LIBS   := $(shell pkg-config --libs mpv)
CFLAGS  += $(MPV_CFLAGS)
LDLIBS  += $(MPV_LIBS)

CLAY_URL = https://github.com/nicbarker/clay/releases/download/v0.14/clay.h
BIN      = ct
OBJS     = main.o term.o image.o video.o
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=

all: $(BIN)

get: clay.h

clay.h:
	curl --fail --location --silent --show-error "$(CLAY_URL)" --output $@.tmp
	mv $@.tmp $@

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

main.o:  main.c clay.h term.h video.h
term.o:  term.c term.h image.h video.h
image.o: image.c image.h
video.o: video.c video.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN)

install: $(BIN)
	mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m 755 "$(BIN)" "$(DESTDIR)$(BINDIR)/$(BIN)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(BIN)"

clean:
	rm -f $(BIN) $(OBJS) clay.h.tmp

.PHONY: all get run install uninstall clean
