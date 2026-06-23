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

On each committed swipe it asks AeroSpace (over the same socket) which non-empty workspaces live on the monitor under the cursor and which one is visible, computes the neighbor in the swipe direction, and switches to it by name — so swipes skip empty workspaces and follow your cursor's monitor.

```
3-finger swipe
      │
      ▼
 macOS HID -> dock-swipe event (private CGS type 30)
                    │
      [AerospaceSwipeIntercept] <- active CGEventTap
                    │
        +-----------+--------------------------+
    return NULL              query AeroSpace socket:
  (Dock never fires)         non-empty workspaces on
                             monitor under cursor +
                             visible one -> switch to
                             the neighbor by name
```

## Behavior

| Gesture | Action |
|---|---|
| Swipe right | Next **non-empty** workspace on the monitor under the cursor (wraps around) |
| Swipe left | Previous **non-empty** workspace on the monitor under the cursor (wraps around) |
| Swipe up/down | Unchanged (Mission Control, App Expose) |
| AeroSpace not running / query fails | Swipe is a no-op (event suppressed, nothing sent) |
| Only one (or zero) non-empty workspace on that monitor | No-op — there's nowhere to move |
| Tap disabled by macOS | Watchdog re-enables it; notifies after ~3s of downtime |

Targeting is computed live from AeroSpace, per swipe:

- **Non-empty only.** Empty workspaces are skipped, so a swipe always lands on a workspace you're actually using — never an empty `Z`/`Y`/`X`.
- **Monitor under the cursor.** The swipe acts on whichever monitor your pointer is on, not AeroSpace's focused monitor. Move the cursor to the other display and swipe there.
- **By name, not `next`/`prev`.** The daemon resolves the destination workspace and switches to it by name, rather than walking AeroSpace's full workspace order. (It no longer relies on `workspace --wrap-around`; wrap-around is handled in the daemon.)

AeroSpace is contacted via its Unix socket directly (`/tmp/bobko.aerospace-$USER.sock`), not the CLI, so there's no process-spawn latency on every swipe. The daemon reads the socket reply to learn the workspace layout; if AeroSpace is unreachable or returns nothing, the swipe is a silent no-op rather than a guess.

### Some apps swallow the gesture (IntelliJ / JetBrains)

In a few apps — notably JetBrains IDEs (IntelliJ, etc.) — a 3-finger swipe does nothing. Those apps consume the trackpad gesture themselves before macOS turns it into the dock-swipe event this daemon intercepts, so **no event reaches the daemon at all**. This is a macOS-level limitation, not something the daemon can override without a fragile, double-firing reconstruction approach.

If swipes are inert with such an app focused, try:

- **System Settings ▸ Trackpad ▸ More Gestures ▸ "Swipe between pages"** — change the finger count (or turn it off) so the OS routes the horizontal swipe to Spaces instead of in-app page navigation.
- **The app's own trackpad/gesture binding** — some JetBrains versions map horizontal swipe to back/forward navigation; disable it in the app's settings.

To confirm an app is the cause: enable debug logging (`killall -USR1 aerospace-swipe-intercept`), focus the app, swipe, and check the log — if no swipe-phase lines appear, the app swallowed the gesture.

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
