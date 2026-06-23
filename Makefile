BIN        = aerospace-swipe-intercept
APP        = AerospaceSwipeIntercept.app
LABEL      = com.samcolmanetti.aerospace-swipe-intercept
APP_DEST   = /Applications/$(APP)
AGENT_DIR  = $(HOME)/Library/LaunchAgents
PLIST      = $(AGENT_DIR)/$(LABEL).plist
BUNDLE_BIN = $(APP)/Contents/MacOS/$(BIN)

# For stable Accessibility (TCC) permission across rebuilds on your dev machine:
# 1. Open Keychain Access > Certificate Assistant > Create a Certificate
# 2. Name: "aerospace-swipe-intercept-cert", Type: Code Signing, Self Signed Root
# Sign with that cert to avoid TCC invalidation on every rebuild.
# CI/release builds use ad-hoc signing (-), which is stable per release artifact.
CERT      ?= aerospace-swipe-intercept-cert

CFLAGS    ?= -std=c11 -O3 -march=native -pipe -flto -Wall -Wextra -Wpedantic
LDFLAGS    = -framework ApplicationServices -framework CoreFoundation \
             -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-x

$(BIN): $(BIN).c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(APP): $(BIN) Info.plist $(BIN).entitlements
	mkdir -p $(APP)/Contents/MacOS
	install -m 755 $(BIN) $(BUNDLE_BIN)
	cp Info.plist $(APP)/Contents/Info.plist
	codesign -fs "$(CERT)" --entitlements $(BIN).entitlements $(APP) 2>/dev/null || \
	codesign -fs - --entitlements $(BIN).entitlements $(APP)

bundle: $(APP)

install: bundle
	@# Unload old label if present (migration from prior installs)
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	cp -r $(APP) /Applications/
	codesign -vfs "$(CERT)" --entitlements $(BIN).entitlements $(APP_DEST) 2>/dev/null || \
	codesign -vfs - --entitlements $(BIN).entitlements $(APP_DEST)
	xattr -rd com.apple.quarantine $(APP_DEST) 2>/dev/null || true
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
	  '    <string>$(APP_DEST)/Contents/MacOS/$(BIN)</string>' \
	  '  </array>' \
	  '  <key>RunAtLoad</key>' \
	  '  <true/>' \
	  '  <key>KeepAlive</key>' \
	  '  <true/>' \
	  '  <key>StandardErrorPath</key>' \
	  '  <string>$(HOME)/Library/Logs/aerospace-swipe-intercept.log</string>' \
	  '  <key>StandardOutPath</key>' \
	  '  <string>$(HOME)/Library/Logs/aerospace-swipe-intercept.log</string>' \
	  '</dict>' \
	  '</plist>' > $(PLIST)
	launchctl bootstrap gui/$$(id -u) $(PLIST)
	@echo "Installed. Grant Accessibility in System Settings > Privacy & Security > Accessibility."

uninstall:
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	rm -rf $(APP_DEST) $(PLIST)

clean:
	rm -f $(BIN)
	rm -rf $(APP)

.PHONY: bundle install uninstall clean
