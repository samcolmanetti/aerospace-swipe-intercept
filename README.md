# aerospace-swipe-intercept

Turns 3-finger horizontal trackpad swipes into [AeroSpace](https://github.com/nikitabobko/AeroSpace) workspace switches — with no System Settings changes and no native full-screen animation.

## How it works

An active `CGEventTap` intercepts the private dock-swipe event (`kCGSEventDockControl`, HID type 23) that macOS generates when you 3-finger swipe. The event is suppressed before the Dock sees it, and an AeroSpace workspace command is sent over its Unix socket instead.

This is different from [aerospace-swipe](https://github.com/acsandmann/aerospace-swipe), which taps in listen-only mode and can't suppress the native gesture (causing dual-fire without a System Settings change). This daemon lets macOS do the gesture recognition and only redirects the result.

```
3-finger swipe → macOS HID → dock-swipe event
                                    │
                           [aerospace-swipe-intercept]
                                    │
                    ┌───────────────┴───────────────┐
                return NULL (suppress)          send to AeroSpace socket
                Dock never switches             workspace next / prev
```

Vertical swipes (Mission Control, App Exposé) are left untouched.

## Requirements

- macOS with AeroSpace installed and running
- AeroSpace 0.20.x (socket protocol is version-coupled)
- Xcode Command Line Tools (`xcode-select --install`)

## Install

```sh
make
make install
```

`make install` builds the binary, signs it, installs it to `~/.local/bin`, and registers a `RunAtLoad`/`KeepAlive` LaunchAgent. Swipes are active on next login (or immediately after install).

### Unload the incumbent tool first

If `AerospaceSwipe.app` (aerospace-swipe) is running, both daemons will react to every swipe — causing double-switches. Quit it and remove its LaunchAgent before installing this one:

```sh
# Quit AerospaceSwipe.app from the menu bar or:
osascript -e 'quit app "AerospaceSwipe"'

# Remove its LaunchAgent (label varies — check ~/Library/LaunchAgents/)
launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/com.acsandmann.AerospaceSwipe.plist 2>/dev/null || true
```

## Permissions

- **Accessibility** — required to create an active event tap. Prompted automatically on first run.
- **Input Monitoring** — may be prompted by macOS; grant it if asked.

No SIP disabling required.

## Behavior and limitations

**No System Settings change needed.** The daemon suppresses the native dock-swipe event upstream, so the System Settings > Trackpad gesture toggle stays on and doesn't conflict.

**Accepted dead-air.** When AeroSpace is not running or the tap is disabled, horizontal 3-finger swipes do nothing (the event is suppressed but not redirected). The watchdog (see below) notifies you if the tap goes down.

**Wrap-around.** Swiping past the last/first workspace wraps around. This is delegated to AeroSpace's `--wrap-around` flag.

**Watchdog.** If the tap stays disabled for ~3 seconds (e.g. after revoking Accessibility permission), a macOS notification is shown: "Gesture tap disabled — check Accessibility permission". After sleep/wake, the tap re-enables automatically without manual restart.

## Uninstall

```sh
make uninstall
```

## Credits

Interception core (tap setup, field indices, phase handling, debounce) adapted from [joshuarli/iss](https://github.com/joshuarli/iss) (BSD Zero Clause License).
