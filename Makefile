CC ?= cc
CFLAGS ?= -Os -pipe
CPPFLAGS += -D_GNU_SOURCE
# GTK2 headers use types which modern GLib marks deprecated.  Keep useful
# warnings for our code, but do not emit unavoidable warnings from GTK2.
WARNINGS := -Wall -Wextra -Wshadow -Wformat=2 -Wno-deprecated-declarations
PKGS := gtk+-2.0 gthread-2.0 libcurl json-c

all: gtk2aichat

gtk2aichat: src/main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< -o $@ $$(pkg-config --cflags --libs $(PKGS))

clean:
	rm -f gtk2aichat

.PHONY: all clean
