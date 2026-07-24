CC ?= cc
CFLAGS ?= -Os -pipe
CPPFLAGS += -D_GNU_SOURCE
# GTK2 headers use types which modern GLib marks deprecated.  Keep useful
# warnings for our code, but do not emit unavoidable warnings from GTK2.
WARNINGS := -Wall -Wextra -Wshadow -Wformat=2 -Wno-deprecated-declarations
PKGS := gtk+-2.0 gthread-2.0 libcurl json-c

all: gtk2aichat

SOURCES := src/main.c src/agent_tools.c src/mcp_client.c

gtk2aichat: $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(SOURCES) -o $@ $$(pkg-config --cflags --libs $(PKGS))

test-agent-tools: tests/test_agent_tools.c src/agent_tools.c src/agent_tools.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) tests/test_agent_tools.c src/agent_tools.c -o $@ $$(pkg-config --cflags --libs glib-2.0 json-c)

fake-mcp-server: tests/fake_mcp_server.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< -o $@ $$(pkg-config --cflags --libs json-c)

test-mcp-client: tests/test_mcp_client.c src/mcp_client.c src/mcp_client.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) tests/test_mcp_client.c src/mcp_client.c -o $@ $$(pkg-config --cflags --libs glib-2.0 json-c)

test: test-agent-tools fake-mcp-server test-mcp-client
	./test-agent-tools
	./test-mcp-client ./fake-mcp-server

clean:
	rm -f gtk2aichat test-agent-tools test-mcp-client fake-mcp-server

.PHONY: all test clean
