// aerospace-swipe-intercept — AeroSpace workspace switcher via 3-finger trackpad swipe
//
// Forks the interception core from joshuarli/iss (Instant Space Switcher).
// Instead of posting synthetic gesture events for native space switching,
// this daemon suppresses the dock-swipe event and sends an AeroSpace
// workspace command over its Unix socket.
//
// How it works:
//   1. An active CGEventTap intercepts trackpad dock-swipe gesture events
//      (private CGS event type 30, HID type 23) before the Dock sees them.
//   2. On a committed horizontal swipe, the event is suppressed (NULL) and
//      an AeroSpace workspace command is dispatched off-thread via GCD.
//   3. Vertical swipes (Mission Control, App Exposé) pass through unchanged.
//   4. A watchdog re-enables the tap after timeout/userinput events and
//      notifies the user if it stays disabled for ~3 seconds.
//
// Does not require disabling SIP.
// Does not require any change to System Settings > Trackpad.
// AeroSpace must be running; swipes are silent no-ops when it is not.

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// --- Private CGEvent field constants -----------------------------------------
// Discovered via reverse engineering; stable across macOS versions (iss).

static const CGEventField kCGSEventTypeField            = 55;
static const CGEventField kCGEventGestureHIDType        = 110;
static const CGEventField kCGEventGestureSwipeMotion    = 123;
static const CGEventField kCGEventGestureSwipeProgress  = 124;
static const CGEventField kCGEventGestureSwipeVelocityX = 129;
static const CGEventField kCGEventGesturePhase          = 132;

enum { kCGSEventGesture = 29, kCGSEventDockControl = 30 };
enum { kIOHIDEventTypeDockSwipe = 23 };
enum { kCGGestureMotionHorizontal = 1 };
enum { kGestureBegan = 1, kGestureChanged = 2, kGestureEnded = 4, kGestureCancelled = 8 };

// --- State -------------------------------------------------------------------

static CFMachPortRef tap;
static bool          swipeTracking;
static bool          swipeFired;
static struct timespec swipeStartTime;

static dispatch_queue_t  socket_q;
static char              aerospace_sock[256]; // /tmp/bobko.aerospace-$USER.sock

// Watchdog
static int  watchdog_disabled_ticks;
static bool watchdog_notified;

// --- Logging -----------------------------------------------------------------
// Two levels: always-on (lifecycle + errors) and debug (verbose per-event trace).
// Debug is off by default and toggled at runtime via SIGUSR1
// (`killall -USR1 aerospace-swipe-intercept`). stderr is redirected by the
// LaunchAgent plist to ~/Library/Logs/aerospace-swipe-intercept.log (no sudo).

static volatile sig_atomic_t debug_enabled = 0; // verbose logging, off by default
static volatile sig_atomic_t debug_toggled = 0; // set by SIGUSR1 handler, drained in run loop

#define log_always(...) fprintf(stderr, "aerospace-swipe-intercept: " __VA_ARGS__)
#define log_debug(...) \
    do { if (debug_enabled) fprintf(stderr, "aerospace-swipe-intercept: " __VA_ARGS__); } while (0)

// --- AeroSpace socket client -------------------------------------------------

static void send_aerospace(bool right) {
    log_debug("send_aerospace(%s)\n", right ? "next" : "prev");
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_always("socket(): %s\n", strerror(errno));
        return;
    }

    // Fail fast — a hung AeroSpace must not stall the serial queue indefinitely
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, aerospace_sock, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_always("connect(%s): %s\n", aerospace_sock, strerror(errno));
        close(fd);
        return;
    }
    log_debug("connected to %s\n", aerospace_sock);

    // AeroSpace 0.20.x socket protocol (version-coupled; stdin key is required)
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"args\":[\"workspace\",\"--wrap-around\",\"%s\"],"
        "\"stdin\":\"\",\"windowId\":null,\"workspace\":null}",
        right ? "next" : "prev");
    if (write(fd, buf, (size_t)len) < 0)
        log_always("write(): %s\n", strerror(errno));
    else
        log_debug("sent: %s\n", buf);
    // Reply carries serverVersionAndHash; ignored — we don't validate version here
    close(fd);
}

// Called from the tap callback thread. Dispatches the socket write asynchronously
// so that socket I/O can never block event delivery and trigger DisabledByTimeout.
// The serial queue serializes concurrent writes without needing an in-flight guard.
static void perform_switch(bool right) {
    log_debug("perform_switch(%s) — dispatching\n", right ? "next" : "prev");
    dispatch_async(socket_q, ^{ send_aerospace(right); });
}

// --- Watchdog notification ---------------------------------------------------

static void notify_tap_disabled(void) {
    system("osascript -e 'display notification \"3-finger swipes are inactive\" "
           "with title \"aerospace-swipe-intercept\" "
           "subtitle \"Gesture tap disabled — check Accessibility permission\"'");
}

// --- Event tap callback ------------------------------------------------------

static CGEventRef cb(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ctx) {
    (void)proxy; (void)ctx;

    // Re-enable after system-imposed disablement; reset any in-flight swipe state
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        log_always("tap disabled by %s — re-enabling\n",
                   type == kCGEventTapDisabledByTimeout ? "timeout" : "user input");
        swipeTracking = swipeFired = false;
        CGEventTapEnable(tap, true);
        return ev;
    }

    int et = (int)CGEventGetIntegerValueField(ev, kCGSEventTypeField);
    int hidType = (int)CGEventGetIntegerValueField(ev, kCGEventGestureHIDType);
    int motion  = (int)CGEventGetIntegerValueField(ev, kCGEventGestureSwipeMotion);

    log_debug("event type=%d cgtype=%d hidType=%d motion=%d\n",
              (int)type, et, hidType, motion);

    // Only intercept horizontal dock-swipe events (not Mission Control, App Exposé, etc.)
    if (et == kCGSEventDockControl
        && hidType == kIOHIDEventTypeDockSwipe
        && motion == kCGGestureMotionHorizontal) {

        int phase = (int)CGEventGetIntegerValueField(ev, kCGEventGesturePhase);
        double progress = CGEventGetDoubleValueField(ev, kCGEventGestureSwipeProgress);
        double velocity = CGEventGetDoubleValueField(ev, kCGEventGestureSwipeVelocityX);
        log_debug("dock-swipe phase=%d progress=%.3f velocity=%.3f tracking=%d fired=%d\n",
                  phase, progress, velocity, swipeTracking, swipeFired);

        if (phase == kGestureBegan) {
            swipeTracking = true;
            swipeFired    = false;
            clock_gettime(CLOCK_MONOTONIC, &swipeStartTime);
            log_debug("swipe began — suppressing\n");
            return NULL;
        }
        if (phase == kGestureChanged && swipeTracking) {
            if (!swipeFired) {
                double p = CGEventGetDoubleValueField(ev, kCGEventGestureSwipeProgress);
                if (p != 0.0) {
                    log_debug("swipe changed — firing (progress=%.3f)\n", p);
                    swipeFired = true;
                    perform_switch(p > 0);
                }
            }
            return NULL;
        }
        if (phase == kGestureEnded && swipeTracking) {
            if (!swipeFired) {
                double v = CGEventGetDoubleValueField(ev, kCGEventGestureSwipeVelocityX);
                if (v != 0.0) {
                    log_debug("swipe ended — firing via velocity (%.3f)\n", v);
                    swipeFired = true;
                    perform_switch(v > 0);
                } else {
                    log_debug("swipe ended — no velocity, no switch\n");
                }
            }
            swipeTracking = swipeFired = false;
            return NULL;
        }
        if (phase == kGestureCancelled) {
            log_debug("swipe cancelled\n");
            swipeTracking = swipeFired = false;
            return NULL;
        }
        return swipeTracking ? NULL : ev;
    }

    // Suppress companion kCGSEventGesture events paired with the dock swipe
    if (et == kCGSEventGesture && swipeTracking) return NULL;
    return ev;
}

static volatile sig_atomic_t running = 1;
static void stop(int s) { (void)s; running = 0; }

// SIGUSR1 toggles verbose debug logging at runtime. Async-signal-safe: it only
// flips flags — the state-change line is logged from the run loop, not here.
static void toggle_debug(int s) { (void)s; debug_enabled = !debug_enabled; debug_toggled = 1; }

int main(void) {
    // Resolve and cache AeroSpace socket path: /tmp/bobko.aerospace-$USER.sock
    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        log_always("getpwuid failed\n");
        return 1;
    }
    snprintf(aerospace_sock, sizeof(aerospace_sock),
             "/tmp/bobko.aerospace-%s.sock", pw->pw_name);

    // Single-instance guard: hold an exclusive flock for the process lifetime.
    // A second launch (double-click, `open`, accidental double-bootstrap) fails
    // to acquire the lock and exits cleanly — before the Accessibility prompt, so
    // a duplicate never triggers a second TCC dialog. The kernel releases the lock
    // on exit (incl. crash/SIGKILL), so it self-heals across restarts.
    char lock_dir[512], lock_path[600];
    snprintf(lock_dir, sizeof(lock_dir),
             "%s/Library/Application Support/aerospace-swipe-intercept", pw->pw_dir);
    if (mkdir(lock_dir, 0755) != 0 && errno != EEXIST) {
        log_always("mkdir(%s): %s\n", lock_dir, strerror(errno));
        return 1;
    }
    snprintf(lock_path, sizeof(lock_path), "%s/instance.lock", lock_dir);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock_fd < 0) {
        log_always("open(%s): %s\n", lock_path, strerror(errno));
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        log_always("another instance is already running — exiting\n");
        return 0; // intentional no-op; leave lock_fd to the running instance
    }
    // lock_fd intentionally left open for the process lifetime to hold the lock.

    socket_q = dispatch_queue_create(
        "com.aerospace-swipe-intercept.socket", DISPATCH_QUEUE_SERIAL);

    // Prompt for Accessibility permission if not already granted, then poll until
    // it is — never exit, so KeepAlive doesn't restart us in an infinite prompt loop.
    if (!AXIsProcessTrusted()) {
        const void *keys[] = { kAXTrustedCheckOptionPrompt };
        const void *vals[] = { kCFBooleanTrue };
        CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        AXIsProcessTrustedWithOptions(opts); // show the dialog once
        CFRelease(opts);
        log_always("waiting for Accessibility permission...\n");
        while (!AXIsProcessTrusted()) sleep(2);
        log_always("Accessibility granted\n");
    }

    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        (1ULL << kCGSEventGesture) | (1ULL << kCGSEventDockControl),
        cb, NULL);
    if (!tap) {
        log_always("Failed to create event tap.\n");
        return 1;
    }

    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    signal(SIGUSR1, toggle_debug);

    log_always("active (socket: %s, debug %s — toggle with `killall -USR1 %s`)\n",
               aerospace_sock, debug_enabled ? "on" : "off", "aerospace-swipe-intercept");

    while (running) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);

        // Report SIGUSR1 debug toggles from the run loop (handler stays I/O-free)
        if (debug_toggled) {
            debug_toggled = 0;
            log_always("debug logging %s\n", debug_enabled ? "enabled" : "disabled");
        }

        // Watchdog: check tap health every ~1s
        if (!CGEventTapIsEnabled(tap)) {
            CGEventTapEnable(tap, true);
            watchdog_disabled_ticks++;
            if (watchdog_disabled_ticks >= 3 && !watchdog_notified) {
                watchdog_notified = true;
                // Notify on a global queue, not socket_q — a stuck send must not
                // swallow the alert that reports the degradation
                dispatch_async(
                    dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
                    ^{ notify_tap_disabled(); });
            }
        } else {
            watchdog_disabled_ticks = 0;
            watchdog_notified = false;
        }

        // Unwedge: if Began arrived but no Ended/Cancelled within ~1.5s, reset
        // (guards against a lost terminator that would leave us suppressing all gestures)
        if (swipeTracking) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - swipeStartTime.tv_sec)
                           + (double)(now.tv_nsec - swipeStartTime.tv_nsec) * 1e-9;
            if (elapsed > 1.5) swipeTracking = swipeFired = false;
        }
    }

    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
    CFRelease(src);
    CFRelease(tap);
    return 0;
}
