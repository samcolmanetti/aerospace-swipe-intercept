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
// Generalized request/response over the AeroSpace Unix socket. Unlike the
// original fire-and-forget send, the daemon now both *queries* AeroSpace
// (`list-workspaces ...`) and *commands* it (`workspace <name>`), so it must
// read and parse the reply it previously discarded.
//
// Reply envelope (AeroSpace 0.20.x; version-coupled):
//   {"stdout":"1\n2","exitCode":0,"stderr":"","serverVersionAndHash":"..."}
// Key order is NOT stable across replies, so values are extracted by key name,
// never by position. `stdout` is newline-separated workspace names.

enum { ASI_MAX_WORKSPACES = 64, ASI_STDOUT_MAX = 2048, ASI_REPLY_MAX = 4096 };

typedef struct {
    bool ok;                    // round-tripped: connected, wrote, parsed a reply
    int  exit_code;             // AeroSpace exitCode (meaningful only when ok)
    char out[ASI_STDOUT_MAX];   // unescaped stdout payload
} asi_reply;

// Serialize args into {"args":[...],"stdin":"","windowId":null,"workspace":null}.
// Args are short tokens the daemon controls (workspace names originate from
// AeroSpace's own output), but " and \ are escaped defensively. Returns the
// payload length, or -1 if it would not fit.
static int asi_build_payload(char *buf, size_t cap, const char *const *args, int nargs) {
    size_t n = 0;
    const char *prefix = "{\"args\":[";
    const char *suffix = "],\"stdin\":\"\",\"windowId\":null,\"workspace\":null}";
    size_t plen = strlen(prefix);
    if (plen >= cap) return -1;
    memcpy(buf, prefix, plen);
    n = plen;
    for (int i = 0; i < nargs; i++) {
        if (i) { if (n + 1 >= cap) return -1; buf[n++] = ','; }
        if (n + 1 >= cap) return -1; buf[n++] = '"';
        for (const char *p = args[i]; *p; p++) {
            if (*p == '"' || *p == '\\') { if (n + 1 >= cap) return -1; buf[n++] = '\\'; }
            if (n + 1 >= cap) return -1; buf[n++] = *p;
        }
        if (n + 1 >= cap) return -1; buf[n++] = '"';
    }
    size_t slen = strlen(suffix);
    if (n + slen >= cap) return -1;
    memcpy(buf + n, suffix, slen);
    n += slen;
    buf[n] = '\0';
    return (int)n;
}

// Extract a JSON string value for `key` from the flat reply envelope, unescaping
// \n \t \r \" \\. Returns false if the key (as a string-valued field) is absent.
static bool asi_extract_string(const char *json, const char *key, char *dst, size_t cap) {
    char pat[40];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    if (pn < 0 || (size_t)pn >= sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += (size_t)pn;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default:  c = *p;   break; // \" \\ / and others → literal
            }
        }
        if (n + 1 >= cap) break;
        dst[n++] = c;
        p++;
    }
    dst[n] = '\0';
    return true;
}

// Extract an integer value for `key` from the flat reply envelope.
static bool asi_extract_int(const char *json, const char *key, int *out) {
    char pat[40];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pn < 0 || (size_t)pn >= sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (!p) return false;
    *out = atoi(p + pn);
    return true;
}

// Split a newline-separated buffer in place into a token array, dropping empty
// lines (e.g. a trailing newline). Returns the number of tokens.
static int asi_split_lines(char *buf, char *lines[], int max_lines) {
    int n = 0;
    char *p = buf;
    while (*p && n < max_lines) {
        char *start = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') *p++ = '\0';
        if (*start) lines[n++] = start;
    }
    return n;
}

// Send an AeroSpace command over the Unix socket and read its reply. Fills
// `reply` and returns true on a successful round-trip; returns false (caller
// treats as a no-op) on any socket failure, logging via log_always. Must run on
// socket_q — never the event-tap thread.
static bool aerospace_request(const char *const *args, int nargs, asi_reply *reply) {
    reply->ok = false;
    reply->exit_code = -1;
    reply->out[0] = '\0';

    char payload[512];
    int plen = asi_build_payload(payload, sizeof(payload), args, nargs);
    if (plen < 0) {
        log_always("request payload too large\n");
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_always("socket(): %s\n", strerror(errno));
        return false;
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
        return false;
    }

    if (write(fd, payload, (size_t)plen) < 0) {
        log_always("write(): %s\n", strerror(errno));
        close(fd);
        return false;
    }
    log_debug("sent: %s\n", payload);

    // Read the reply. AeroSpace leaves the connection open after replying, so we
    // stop at the end of the first complete JSON object (brace depth back to 0)
    // rather than blocking until EOF or the 250ms recv timeout fires.
    char raw[ASI_REPLY_MAX];
    size_t total = 0;
    int  depth = 0;
    bool in_str = false, esc = false, started = false, complete = false;
    while (total < sizeof(raw) - 1) {
        ssize_t r = read(fd, raw + total, sizeof(raw) - 1 - total);
        if (r <= 0) break; // EOF, timeout (EAGAIN), or error
        for (ssize_t i = 0; i < r; i++) {
            char c = raw[total + i];
            if (esc) { esc = false; continue; }
            if (in_str) {
                if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
            } else if (c == '"') {
                in_str = true;
            } else if (c == '{') {
                depth++;
                started = true;
            } else if (c == '}') {
                if (--depth == 0 && started) { complete = true; break; }
            }
        }
        total += (size_t)r;
        if (complete) break;
    }
    close(fd);
    raw[total] = '\0';

    if (!complete) {
        log_always("incomplete/empty reply (%zu bytes)\n", total);
        return false;
    }

    asi_extract_int(raw, "exitCode", &reply->exit_code);
    asi_extract_string(raw, "stdout", reply->out, sizeof(reply->out));
    reply->ok = true;
    return true;
}

#define ASI_NARGS(a) ((int)(sizeof(a) / sizeof((a)[0])))

// Resolve and switch to the next/previous non-empty workspace on the monitor
// under the cursor. Runs on socket_q (off the event-tap thread).
//
// Algorithm: query the monitor's full ordered workspace list and its non-empty
// subset, then walk the full order from the currently visible workspace in the
// swipe direction (wrapping) until landing on a non-empty workspace. Walking the
// full order means an empty current workspace still steps to the nearest used
// one in the swipe direction, and reduces to "adjacent non-empty" when the
// current workspace is itself non-empty. On any query failure or an empty set
// the daemon does nothing — a no-op is less surprising than landing on an empty
// workspace (and never the old empty-cycling behavior).
static void resolve_and_switch(bool right) {
    // Ordered non-empty workspaces on the mouse monitor — the switch cycle.
    static const char *const q_nonempty[] = {
        "list-workspaces", "--monitor", "mouse", "--empty", "no" };
    asi_reply ne;
    if (!aerospace_request(q_nonempty, ASI_NARGS(q_nonempty), &ne)) {
        log_always("could not query non-empty workspaces — no switch\n");
        return;
    }
    char *nonempty[ASI_MAX_WORKSPACES];
    int nne = asi_split_lines(ne.out, nonempty, ASI_MAX_WORKSPACES);
    if (nne == 0) {
        log_always("no non-empty workspaces on monitor under cursor — no switch\n");
        return;
    }
    if (nne == 1) {
        log_debug("only one non-empty workspace (%s) — no switch\n", nonempty[0]);
        return;
    }

    // Full ordered workspace set on the same monitor — lets us resolve the
    // anchor's position so an empty anchor still steps the right direction.
    static const char *const q_full[] = {
        "list-workspaces", "--monitor", "mouse" };
    asi_reply full;
    char *order[ASI_MAX_WORKSPACES];
    int nord = 0;
    if (aerospace_request(q_full, ASI_NARGS(q_full), &full))
        nord = asi_split_lines(full.out, order, ASI_MAX_WORKSPACES);

    // Anchor = the workspace currently visible on the monitor under the cursor.
    static const char *const q_visible[] = {
        "list-workspaces", "--monitor", "mouse", "--visible" };
    asi_reply vis;
    char anchor[64] = "";
    if (aerospace_request(q_visible, ASI_NARGS(q_visible), &vis)) {
        char *v[1];
        if (asi_split_lines(vis.out, v, 1) == 1)
            snprintf(anchor, sizeof(anchor), "%s", v[0]);
    }

    const char *target = NULL;

    // Preferred: walk the full order from the anchor in the swipe direction,
    // wrapping, landing on the first non-empty workspace.
    if (nord > 0 && anchor[0]) {
        int ai = -1;
        for (int i = 0; i < nord; i++)
            if (strcmp(order[i], anchor) == 0) { ai = i; break; }
        if (ai >= 0) {
            for (int step = 1; step <= nord && !target; step++) {
                int j = right ? (ai + step) % nord
                              : ((ai - step) % nord + nord) % nord;
                for (int k = 0; k < nne; k++)
                    if (strcmp(order[j], nonempty[k]) == 0) { target = nonempty[k]; break; }
            }
        }
    }

    // Fallback: anchor unknown or absent from the order list — step within the
    // non-empty list, or default to the first/last in the swipe direction.
    if (!target) {
        int idx = -1;
        for (int i = 0; i < nne; i++)
            if (anchor[0] && strcmp(nonempty[i], anchor) == 0) { idx = i; break; }
        target = (idx >= 0) ? nonempty[right ? (idx + 1) % nne : (idx - 1 + nne) % nne]
                            : (right ? nonempty[0] : nonempty[nne - 1]);
    }

    log_debug("resolve_and_switch(%s): anchor=%s target=%s (%d non-empty)\n",
              right ? "right" : "left", anchor[0] ? anchor : "?", target, nne);

    const char *cmd[] = { "workspace", target };
    asi_reply sw;
    if (!aerospace_request(cmd, ASI_NARGS(cmd), &sw) || sw.exit_code != 0)
        log_always("switch to workspace %s failed (exit=%d)\n",
                   target, sw.ok ? sw.exit_code : -1);
    else
        log_debug("switched to workspace %s\n", target);
}

// Called from the tap callback thread. Dispatches the query-compute-switch work
// asynchronously so socket I/O can never block event delivery and trigger
// DisabledByTimeout. The serial queue serializes concurrent swipes without
// needing an in-flight guard.
static void perform_switch(bool right) {
    log_debug("perform_switch(%s) — dispatching\n", right ? "right" : "left");
    dispatch_async(socket_q, ^{ resolve_and_switch(right); });
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
