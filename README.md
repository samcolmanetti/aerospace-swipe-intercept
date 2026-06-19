# aerospace-swipe-intercept

A small macOS daemon that maps 3-finger horizontal trackpad swipes to [AeroSpace](https://github.com/nikitabobko/AeroSpace) workspace switches.

No System Settings changes. No native full-screen animation. No fragile threshold tuning.

## Why not aerospace-swipe?

[aerospace-swipe](https://github.com/acsandmann/aerospace-swipe) taps gesture events in listen-only mode and reconstructs swipes with a hand-tuned state machine. Because it can't suppress the native event, a 3-finger swipe fires both the native macOS space switch *and* the AeroSpace switch — you have to rebind the native gesture to 4-finger in System Settings to avoid it.

This daemon works differently: it intercepts the internal dock-swipe event that macOS generates *after* recognizing the gesture, suppresses it before the Dock sees it, and sends the AeroSpace command instead. macOS does the gesture recognition; this only redirects the result.

```
3-finger swipe
      │
      ▼
 macOS HID → dock-swipe event (private CGS type 30)
                    │
      [aerospace-swipe-intercept] ← active CGEventTap
                    │
        ┌───────────┴────────────┐
    return NULL              AeroSpace socket
  (Dock never fires)       workspace next/prev
```

## Install

```sh
git clone https://github.com/samcolmanetti/aerospace-swipe-intercept
cd aerospace-swipe-intercept
make
make install
```

Requires Xcode Command Line Tools (`xcode-select --install`).

`make install` builds the binary, signs it, installs it to `~/.local/bin`, and registers a `RunAtLoad`/`KeepAlive` LaunchAgent. It starts immediately and runs on every login.

### Uninstall aerospace-swipe first

If `aerospace-swipe` (AerospaceSwipe.app) is already running, both daemons will react to every swipe and double-switch. Remove it first:

```sh
osascript -e 'quit app "AerospaceSwipe"'
launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/com.acsandmann.swipe.plist 2>/dev/null || true
rm -f ~/Library/LaunchAgents/com.acsandmann.swipe.plist
```

## Permissions

Grant **Accessibility** access when prompted on first run. macOS may also prompt for **Input Monitoring** — grant that too if asked.

No SIP disabling required.

## Behavior

| | |
|---|---|
| Swipe right | `workspace --wrap-around next` |
| Swipe left | `workspace --wrap-around prev` |
| Swipe up/down | Unchanged (Mission Control, App Exposé) |
| AeroSpace not running | Swipe is a no-op (event suppressed, nothing sent) |
| Tap disabled | Watchdog re-enables it; notifies you after ~3s of downtime |

The daemon connects to AeroSpace's Unix socket directly (`/tmp/bobko.aerospace-$USER.sock`) rather than shelling out to the CLI, so it's not affected by CLI behavior changes across AeroSpace versions.

## Uninstall

```sh
make uninstall
```

## Credits

Interception core adapted from [joshuarli/iss](https://github.com/joshuarli/iss) — the tap placement, private CGEvent field indices, phase handling, and debounce logic come from there. Licensed under [0BSD](LICENSE).
