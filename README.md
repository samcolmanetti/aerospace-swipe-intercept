# AerospaceSwipeIntercept

A macOS background app that maps 3-finger horizontal trackpad swipes to [AeroSpace](https://github.com/nikitabobko/AeroSpace) workspace switches.

No System Settings changes. No native full-screen animation. No fragile threshold tuning.

## Install

```sh
brew install --cask samcolmanetti/tap/aerospace-swipe-intercept
```

After installing, grant **Accessibility** access in System Settings > Privacy & Security > Accessibility.

### Upgrading

```sh
brew upgrade aerospace-swipe-intercept
```

After upgrading, toggle Accessibility permission off and back on in System Settings. The new binary has a different code signature and macOS blocks it until re-authorized.

## How it works

Most swipe daemons tap gesture events in listen-only mode and reconstruct swipes from private HID events. Because they can't suppress the native event, a 3-finger swipe fires both the native macOS space switch *and* the AeroSpace switch — you have to rebind the native gesture to 4-finger in System Settings to avoid it.

This daemon works differently: it intercepts the internal dock-swipe event macOS generates *after* gesture recognition, suppresses it before the Dock sees it, and sends the AeroSpace command instead. macOS does all gesture recognition; this only redirects the result.

```
3-finger swipe
      │
      ▼
 macOS HID -> dock-swipe event (private CGS type 30)
                    │
      [AerospaceSwipeIntercept] <- active CGEventTap
                    │
        +-----------+------------+
    return NULL              AeroSpace socket
  (Dock never fires)       workspace next/prev
```

## Behavior

| Gesture | Action |
|---|---|
| Swipe right | `workspace --wrap-around next` |
| Swipe left | `workspace --wrap-around prev` |
| Swipe up/down | Unchanged (Mission Control, App Expose) |
| AeroSpace not running | Swipe is a no-op (event suppressed, nothing sent) |
| Tap disabled by macOS | Watchdog re-enables it; notifies after ~3s of downtime |

AeroSpace is contacted via its Unix socket directly (`/tmp/bobko.aerospace-$USER.sock`), not the CLI, so there's no process-spawn latency on every swipe.

## Debugging

The daemon logs to `~/Library/Logs/aerospace-swipe-intercept.log` (no `sudo` needed; also visible in Console.app). Normal operation logs only lifecycle and error lines.

Verbose per-event tracing is **off by default**. Toggle it at runtime without restarting:

```sh
killall -USR1 aerospace-swipe-intercept   # toggles debug logging on, then off again
tail -f ~/Library/Logs/aerospace-swipe-intercept.log
```

With debug on, each swipe emits its gesture phase, progress, and velocity, plus a `sent:` line when the AeroSpace command is dispatched. To confirm a swipe is firing, enable debug, swipe, and look for the swipe-phase and `sent:` lines. Errors (socket/connect/write failures, tap disablement) are always logged, even with debug off.

## Uninstall

```sh
brew uninstall aerospace-swipe-intercept
```

### Migrate from aerospace-swipe

If [aerospace-swipe](https://github.com/acsandmann/aerospace-swipe) (AerospaceSwipe.app) is already running, both daemons will react to every swipe and double-switch. Remove it first:

```sh
osascript -e 'quit app "AerospaceSwipe"'
launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/com.acsandmann.swipe.plist 2>/dev/null || true
rm -f ~/Library/LaunchAgents/com.acsandmann.swipe.plist
```

## Build from source

Requires Xcode Command Line Tools (`xcode-select --install`).

```sh
git clone https://github.com/samcolmanetti/aerospace-swipe-intercept
cd aerospace-swipe-intercept
make install
```

For stable Accessibility (TCC) permission across rebuilds, create a self-signed certificate first:

1. Open Keychain Access > Certificate Assistant > Create a Certificate
2. Name: `aerospace-swipe-intercept-cert`, Type: Code Signing, Self Signed Root
3. Run `make install` - it will use that cert automatically

Without the cert, `make install` falls back to ad-hoc signing and macOS will revoke the Accessibility grant on every rebuild (the binary's code identity changes).

The self-signed cert is for local development only. Release builds (and the Homebrew cask) use ad-hoc signing, which is stable per release artifact but changes across versions — hence the re-authorize step after `brew upgrade`.

## Credits

Interception core adapted from [joshuarli/iss](https://github.com/joshuarli/iss) - tap placement, private CGEvent field indices, phase handling, and debounce logic. Licensed under [0BSD](LICENSE).
