PREFIX    ?= $(HOME)/.local
BIN        = aerospace-swipe-intercept
LABEL      = com.aerospace-swipe-intercept
AGENT_DIR  = $(HOME)/Library/LaunchAgents
PLIST      = $(AGENT_DIR)/$(LABEL).plist
SRC        = aerospace-swipe-intercept.c

CFLAGS    ?= -std=c11 -O3 -march=native -pipe -flto -Wall -Wextra -Wpedantic
LDFLAGS    = -framework ApplicationServices -framework CoreFoundation \
             -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-x

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	codesign -fs - $(PREFIX)/bin/$(BIN)
	xattr -d com.apple.quarantine $(PREFIX)/bin/$(BIN) 2>/dev/null || true
	mkdir -p $(AGENT_DIR)
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"' \
	  '  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '  <key>Label</key>' \
	  '  <string>$(LABEL)</string>' \
	  '  <key>ProgramArguments</key>' \
	  '  <array>' \
	  '    <string>$(PREFIX)/bin/$(BIN)</string>' \
	  '  </array>' \
	  '  <key>RunAtLoad</key>' \
	  '  <true/>' \
	  '  <key>KeepAlive</key>' \
	  '  <true/>' \
	  '</dict>' \
	  '</plist>' > $(PLIST)
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	launchctl bootstrap gui/$$(id -u) $(PLIST)
	@echo "Installed. Unload any prior aerospace-swipe (AerospaceSwipe.app) to avoid double-switching."

uninstall:
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	rm -f $(PREFIX)/bin/$(BIN) $(PLIST)

clean:
	rm -f $(BIN)

.PHONY: install uninstall clean
