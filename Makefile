CC ?= cc
CFLAGS ?= -Os -pipe
CPPFLAGS += -D_GNU_SOURCE
# GTK2 headers use types which modern GLib marks deprecated.  Keep useful
# warnings for our code, but do not emit unavoidable warnings from GTK2.
WARNINGS := -Wall -Wextra -Wshadow -Wformat=2 -Wno-deprecated-declarations
PKGS := gtk+-2.0 gthread-2.0 libcurl json-c

all: gtk2aichat

SOURCES := src/main.c src/agent_tools.c

gtk2aichat: $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(SOURCES) -o $@ $$(pkg-config --cflags --libs $(PKGS))

test-agent-tools: tests/test_agent_tools.c src/agent_tools.c src/agent_tools.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) tests/test_agent_tools.c src/agent_tools.c -o $@ $$(pkg-config --cflags --libs glib-2.0 json-c)

test: test-agent-tools
	./test-agent-tools

clean:
	rm -f gtk2aichat test-agent-tools

.PHONY: all test clean
