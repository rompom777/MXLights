/*
 *  WHAT IT DOES
 *  ------------
 *  Run it in a terminal and it keeps the keyboard's backlight lit; press Ctrl+C
 *  and it stops.
 *
 *  Uses only Windows' built-in HID APIs - no third-party dependencies.
 *  MIT License. Copyright (c) 2026 Roman Zrajevsky.
 */

// Silences warnings for functions like strcpy, scanf, etc
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>  // Core Win32 types/handles: HANDLE, DWORD, GUID, CreateFileA, etc
#include <hidsdi.h>   // HidD_*, HidP_*
#include <setupapi.h> // SetupDi* device enumeration
#include <stdio.h>    // printf / fprintf / setvbuf
#include <stdint.h>   // fixed-width integer types: uint8_t, uint16_t, etc

// Links the following libraries when compiling
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// USB Vendor ID assigned to Logitech
#define LOGITECH_VID 0x046D
/* Logitech carries its HID++ control protocol on this page, so it marks the right
 * interface (instead of the generic keyboard/mouse collections). */
#define HIDPP_USAGE_PAGE 0xFF00

#define REPORT_SHORT 0x10 // HID++ short report id: 7-byte message
#define REPORT_LONG 0x11  // HID++ long report id: 20-byte message

#define SW_ID 0x0B // This "software id": nonzero tag ORed into byte 3

#define ROOT_FEATURE_INDEX 0x00   // The Root feature always lives at feature index 0
#define FUNC_ROOT_GETFEATURE 0x00 // Root function 0: look up a feature's runtime index
#define FUNC_ROOT_PING 0x10       // Root function 1 (<<4): ping / get protocol version

#define FEATURE_BACKLIGHT2 0x1982 // The backlight feature we drive
#define FUNC_BL_SET_CONFIG 0x10   // BACKLIGHT2 function 1 (<<4): set backlight config

#define PULSE_INTERVAL_MS 4000 // Keepalive period; must be < the ~5s firmware fade
#define RETRY_INTERVAL_MS 1000 // How often to re-check when the keyboard is offline
#define REQ_TIMEOUT_MS 600     // How long we wait for a reply before giving up

#define MAX_CANDIDATES 64 // Upper bound on HID interfaces we'll collect
#define MAX_GROUPS 16     // Upper bound on grouped logical devices
#define MAX_CHAN 4        // Max collections (pipes) we track per device

/* Device slots to probe: 1..6 are devices on a receiver, 0xFF is the receiver. */
/* 0xFF addresses the receiver itself. A wired/direct device typically answers on
 * slot 0xFF too. We try all of them and see which ones reply. */
static const uint8_t SLOTS[] = {1, 2, 3, 4, 5, 6, 0xFF};

// Signaled by the console control handler so the main loop can exit cleanly
// (close device handles) instead of being killed mid-operation. It is global
// because the handler runs on its own OS thread and has no other way in.
static HANDLE g_stop = NULL;

// Windows calls this (on a thread it spins up) for console control events
static BOOL WINAPI ctrl_handler(DWORD type)
{
    // Treat Ctrl+C, Ctrl+Break and the console-close button as "stop"
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
    {
        if (g_stop)
            SetEvent(g_stop); // wake the main loop so it can wind down
        return TRUE;          // we handled it; don't run the default terminator
    }
    return FALSE; // not ours; let the next handler / default action run
}

// Wait up to ms milliseconds, returning 1 if Ctrl+C was pressed meanwhile.
// Like Sleep(ms), but wakes early the instant g_stop is signaled.
static int interrupted(DWORD ms)
{
    // WAIT_OBJECT_0 means the event fired before the timeout; else it timed out
    return WaitForSingleObject(g_stop, ms) == WAIT_OBJECT_0;
}

/* One "channel" = one opened HID collection (one Win32 file handle). A single
 * physical interface exposes two of these: a short pipe and a long pipe. */
typedef struct
{
    HANDLE h;              /* File handle from CreateFile (opened OVERLAPPED)  */
    USHORT usage;          /* 1 = short pipe, 2 = long pipe */
    USHORT inLen, outLen;  /* HID input/output report byte lengths       */
    OVERLAPPED ov;         /* Async I/O control block for the pending read     */
    HANDLE ev;             /* Manual-reset event signaled when the read done   */
    unsigned char buf[64]; /* Receive buffer for one incoming report           */
    int pending;           /* 1 while an overlapped ReadFile is in flight      */
} Chan;

/* A logical device = the set of channels that share one interface path. */
typedef struct
{
    Chan ch[MAX_CHAN]; /* The grouped collections (short + long + ...)     */
    int nch;           /* How many of ch[] are actually in use            */
} Dev;

/* A candidate is a raw enumeration hit before we group/open it for real. */
typedef struct
{
    char path[512];              /* The device interface path from SetupAPI          */
    USHORT usage, inLen, outLen; /* Capabilities read during enumeration        */
    int grouped;                 /* Marked 1 once this candidate has been consumed   */
} Cand;

/* Produce a canonical "group key" for a device path: lowercased and truncated
 * at the "&col" marker. Two collections of the same interface differ only in the
 * "&colXX" suffix, so stripping it makes them compare equal. */
static void group_key(char *dst, const char *src)
{
    size_t i = 0;
    // Copy each character, stopping at the NUL or the 510-char safety cap
    for (; src[i] && i < 510; i++)
    {
        char c = src[i];
        // Uppercase A-Z -> lowercase by adding 32 (ASCII gap); copy others as-is
        dst[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    dst[i] = 0;                    // NUL-terminate the destination string
    char *p = strstr(dst, "&col"); // find the marker that distinguishes collections
    if (p)
        *p = 0; // cut the string there so only the shared prefix remains
}

// Arm an overlapped (asynchronous) read on this channel. Returns 0 if a read is
// now in flight (or was already), -1 on hard failure. Overlapped I/O lets us
// wait on multiple channels at once and apply a timeout.
static int start_read(Chan *c)
{
    if (c->pending)
        return 0;                     // already have a read queued; don't double-arm
    memset(&c->ov, 0, sizeof(c->ov)); // OVERLAPPED must start fully zeroed
    c->ov.hEvent = c->ev;             // tie this I/O to our event so we can wait on it
    ResetEvent(c->ev);                // clear the event so a stale signal can't fool us
    DWORD n;                          // receives byte count on synchronous completion
    /* ReadFile on an overlapped handle usually returns FALSE with ERROR_IO_
     * PENDING, but can complete synchronously (TRUE) if data is already there. */
    if (ReadFile(c->h, c->buf, c->inLen, &n, &c->ov))
    {
        c->pending = 1; // completed immediately but the event still signals
        return 0;
    }
    if (GetLastError() == ERROR_IO_PENDING)
    {
        c->pending = 1; // the normal async case: queued, will signal later
        return 0;
    }
    return -1; // real failure (e.g. device unplugged)
}

// Cancel a pending read and wait for the cancellation to settle, so the
// OVERLAPPED/buffer are safe to reuse (avoids the kernel writing into them later)
static void cancel_read(Chan *c)
{
    if (!c->pending)
        return;               // nothing in flight
    CancelIoEx(c->h, &c->ov); // ask the OS to cancel this specific overlapped op
    DWORD n;
    GetOverlappedResult(c->h, &c->ov, &n, TRUE); // block until the cancel is reaped
    c->pending = 0;                              // channel is idle again
}

// Write a report to a collection, waiting (up to REQ_TIMEOUT_MS) for it to
// complete. Returns 0 on success, -1 on failure/timeout.
static int write_report(Chan *c, const unsigned char *buf, int len)
{
    OVERLAPPED wo;
    memset(&wo, 0, sizeof(wo));
    wo.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // temporary event for this write
    DWORD n;
    int rc = 0;
    if (!WriteFile(c->h, buf, len, &n, &wo))
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            // async: wait for it, then confirm it actually completed
            if (WaitForSingleObject(wo.hEvent, REQ_TIMEOUT_MS) != WAIT_OBJECT_0)
                rc = -1; // timed out
            else if (!GetOverlappedResult(c->h, &wo, &n, FALSE))
                rc = -1; // completed but with an error
        }
        else
        {
            rc = -1; // failed to even start
        }
    }
    CloseHandle(wo.hEvent);
    return rc;
}

/*
 * Build a request report, write it to the appropriate collection, then wait for
 * a matching reply on ANY collection (replies can come back on the sibling pipe).
 * Returns:
 *      >= 0 : number of parameter bytes copied into out[] (the reply payload)
 *        -1 : timed out with no matching reply
 *        -2 : HID++ 2.0 error reply (code stored in *errcode if not NULL)
 *        -3 : HID++ 1.0 error reply (code stored in *errcode if not NULL)
 *
 * mark/check_mark: used only by ping(), which puts a marker byte in the request
 * and requires the reply to echo it (so a stale reply can't be mistaken for the
 * pong).
 */
static int hidpp_request(Dev *d, uint8_t devindex, uint8_t feat_idx, uint8_t function,
                         const uint8_t *params, int nparams,
                         uint8_t *out, int *outlen, uint8_t *errcode,
                         uint8_t mark, int check_mark)
{
    uint8_t funcsw = (uint8_t)(function | SW_ID); // pack function number + our software id

    // 3 or fewer params fit in a short report; otherwise use a long report
    uint8_t report_id = (nparams <= 3) ? REPORT_SHORT : REPORT_LONG;

    // Pick the collection to write on: usage 1 = short pipe, usage 2 = long pipe
    Chan *wc = NULL;
    for (int i = 0; i < d->nch; i++)
    {
        USHORT want = (report_id == REPORT_SHORT) ? 1 : 2;
        if (d->ch[i].usage == want)
        {
            wc = &d->ch[i];
            break;
        }
    }
    if (!wc)
    { // fall back by report length, then to the first collection
        for (int i = 0; i < d->nch; i++)
        {
            if ((report_id == REPORT_SHORT && d->ch[i].outLen <= 8) ||
                (report_id == REPORT_LONG && d->ch[i].outLen > 8))
            {
                wc = &d->ch[i];
                break;
            }
        }
    }
    if (!wc)
        wc = &d->ch[0];

    // Assemble the request report (zero-padded to the pipe's report length)
    unsigned char req[64];
    memset(req, 0, sizeof(req));
    req[0] = report_id; // byte 0: report id (short/long)
    req[1] = devindex;  // byte 1: which device/slot on the receiver
    req[2] = feat_idx;  // byte 2: feature index
    req[3] = funcsw;    // byte 3: function number | software id
    for (int i = 0; i < nparams; i++)
        req[4 + i] = params[i]; // bytes 4..: parameters

    // Post reads on every collection BEFORE writing, so we can't miss a fast
    // reply that arrives before we start listening
    for (int i = 0; i < d->nch; i++)
        start_read(&d->ch[i]);

    if (write_report(wc, req, wc->outLen) != 0)
    {
        for (int i = 0; i < d->nch; i++)
            cancel_read(&d->ch[i]);
        return -1;
    }

    // Wait for a matching reply until our deadline
    ULONGLONG deadline = GetTickCount64() + REQ_TIMEOUT_MS;
    int rc = -1;
    for (;;)
    {
        // Collect the events of all collections with a read in flight
        HANDLE evs[MAX_CHAN];
        Chan *map[MAX_CHAN];
        int ne = 0;
        for (int i = 0; i < d->nch; i++)
            if (d->ch[i].pending)
            {
                evs[ne] = d->ch[i].ev;
                map[ne] = &d->ch[i];
                ne++;
            }
        if (ne == 0)
            break;

        // Wait for whichever channel signals first, but no later than the deadline
        ULONGLONG now = GetTickCount64();
        DWORD wait = (now >= deadline) ? 0 : (DWORD)(deadline - now);
        DWORD w = WaitForMultipleObjects(ne, evs, FALSE, wait);
        if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + (DWORD)ne)
            break; // timeout (or a wait error): give up

        Chan *c = map[w - WAIT_OBJECT_0]; // the channel that completed
        DWORD nbytes;
        if (!GetOverlappedResult(c->h, &c->ov, &nbytes, FALSE))
        {
            c->pending = 0;
            start_read(c); // read failed; re-arm and keep waiting
            continue;
        }
        c->pending = 0;

        /* A reply report looks like: [id][devindex][b2][b3][b4]...
         * For devices behind a receiver the device index matches; some report it
         * inverted (devindex ^ 0xFF), which we also accept. */
        unsigned char *b = c->buf;
        if (nbytes >= 4 && (b[1] == devindex || b[1] == (devindex ^ 0xFF)))
        {
            // Normal reply: feature index and function/sw byte echo our request
            if (b[2] == feat_idx && b[3] == funcsw)
            {
                // For a ping, also require the echoed marker byte to match
                if (check_mark && nbytes > 6 && b[6] != mark)
                {
                    start_read(c);
                    continue;
                }
                int n = (int)nbytes - 4; // reply payload is everything after byte 3
                if (n > 0)
                    memcpy(out, b + 4, (size_t)n);
                *outlen = n > 0 ? n : 0;
                rc = *outlen;
                break;
            }
            // HID++ 2.0 error: b2 == 0xFF, then our echoed request bytes + code
            if (nbytes >= 6 && b[2] == 0xFF && b[3] == feat_idx && b[4] == funcsw)
            {
                if (errcode)
                    *errcode = b[5];
                rc = -2;
                break;
            }
            // HID++ 1.0 error: b2 == 0x8F, then our echoed request bytes + code
            if (nbytes >= 6 && b[2] == 0x8F && b[3] == feat_idx && b[4] == funcsw)
            {
                if (errcode)
                    *errcode = b[5];
                rc = -3;
                break;
            }
        }
        start_read(c); // unrelated notification - keep listening
    }

    for (int i = 0; i < d->nch; i++)
        cancel_read(&d->ch[i]);
    return rc;
}

// Find every Logitech HID++ control collection (vendor 0x046D, usage page
// 0xFF00). Fills cands[] and returns how many were found.
static int enumerate(Cand *cands, int max)
{
    // Every HID device interface is registered under this GUID
    GUID guid;
    // HidD_GetHidGuid fills 'guid' with the GUID Windows uses to tag all HID device interfaces
    HidD_GetHidGuid(&guid);

    /* Ask Windows for the set of HID interfaces that are present right now. */
    /* SetupDiGetClassDevs returns an HDEVINFO "device information set" handle.
     *   arg1 &guid  : restrict to the HID interface class we just fetched.
     *   arg2 NULL   : no Plug-and-Play enumerator filter.
     *   arg3 NULL   : no owner window (for any UI).
     *   arg4 flags  : DIGCF_PRESENT      -> only devices currently attached;
     *                 DIGCF_DEVICEINTERFACE -> enumerate interfaces, not nodes. */
    HDEVINFO di = SetupDiGetClassDevs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    // On failure the function returns INVALID_HANDLE_VALUE rather than NULL
    if (di == INVALID_HANDLE_VALUE)
        return 0;

    int count = 0; // how many matching collections we've recorded
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd); // versioned struct: set size before the API fills it

    /* Iterate every HID interface index until the enumeration runs out. */
    /* SetupDiEnumDeviceInterfaces walks the set 'di' one interface at a time:
     *   arg2 NULL  : enumerate across the whole set (not a single device)
     *   arg3 &guid : the interface class to match
     *   arg4 idx   : zero-based index; increment each pass
     *   arg5 &ifd  : receives this interface's data
     * It returns FALSE when the index is past the end (last error
     * ERROR_NO_MORE_ITEMS), which ends the loop. */
    for (DWORD idx = 0; count < max && SetupDiEnumDeviceInterfaces(di, NULL, &guid, idx, &ifd); idx++)
    {
        DWORD need = 0; // receives the required buffer size in bytes
        /* First call: passing NULL buffer / 0 size makes it fail on purpose but
         * write the needed byte count into 'need' (the two-call size probe). */
        SetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
        if (need == 0)
            continue; // nothing to retrieve; skip this one

        /* Allocate exactly 'need' bytes. The detail struct has a fixed header
         * followed by the device path string that extends past its declared end. */
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        if (!det)
            continue; // out of memory: skip this interface, keep going
        /* cbSize must be the size of the FIXED header only (not 'need'). The API
         * uses it to locate the struct version, not the total allocation. */
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        // Second call: now with a real buffer, fills det->DevicePath
        if (SetupDiGetDeviceInterfaceDetailA(di, &ifd, det, need, NULL, NULL))
        {
            /* Open the collection briefly to read its vendor id and capabilities.
             * Shared read+write so we don't fight Logi Options+ for it. */
            /* CreateFileA opens the HID device by its path and returns a HANDLE:
             *   access    : GENERIC_READ | GENERIC_WRITE (we only query here).
             *   share     : FILE_SHARE_READ|WRITE lets other apps keep it open.
             *   OPEN_EXISTING : fail if the device isn't there (don't create).
             * Returns INVALID_HANDLE_VALUE on failure. */
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE)
            {
                HIDD_ATTRIBUTES attr; // receives VendorID/ProductID/etc.
                attr.Size = sizeof(attr);
                PHIDP_PREPARSED_DATA pp = NULL; // parsed HID report descriptor

                // Keep only Logitech devices and only the HID++ usage page
                if (HidD_GetAttributes(h, &attr) &&  // read VID/PID
                    attr.VendorID == LOGITECH_VID && // must be Logitech
                    HidD_GetPreparsedData(h, &pp))   // get parsed descriptor
                {
                    HIDP_CAPS caps; // top-level collection capabilities
                    /* HidP_GetCaps decodes the preparsed data into caps: usage
                     * page/usage and the input/output report byte lengths. It
                     * returns HIDP_STATUS_SUCCESS on success. */
                    if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                        caps.UsagePage == HIDPP_USAGE_PAGE)
                    {
                        // Record this collection as a candidate to group/open later
                        Cand *c = &cands[count++];
                        memset(c, 0, sizeof(*c));
                        strncpy(c->path, det->DevicePath, sizeof(c->path) - 1);
                        c->usage = caps.Usage;
                        c->inLen = caps.InputReportByteLength;
                        c->outLen = caps.OutputReportByteLength;
                    }
                    HidD_FreePreparsedData(pp);
                }
                CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(di); // release the device information set
    return count;
}

// Group candidate collections into devices (one Dev per HID++ interface) and
// open each collection for overlapped I/O. Returns the number of devices.
static int build_devices(Cand *cands, int ncand, Dev *devs, int maxdev)
{
    int ndev = 0;
    for (int i = 0; i < ncand && ndev < maxdev; i++)
    {
        if (cands[i].grouped)
            continue; // already folded into an earlier device

        char ki[512], kj[512];
        group_key(ki, cands[i].path); // the key this new device will match on

        Dev *d = &devs[ndev];
        memset(d, 0, sizeof(*d));

        // Gather every not-yet-grouped collection sharing this interface key
        for (int j = i; j < ncand; j++)
        {
            if (cands[j].grouped)
                continue;
            group_key(kj, cands[j].path);
            if (strcmp(ki, kj) != 0)
                continue; // different interface

            cands[j].grouped = 1; // claim it even if we can't open it
            if (d->nch >= MAX_CHAN)
                continue; // no room; still marked consumed so it isn't re-grouped

            HANDLE h = CreateFileA(cands[j].path, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (h == INVALID_HANDLE_VALUE)
                continue;

            // Record the opened channel
            Chan *ch = &d->ch[d->nch++];
            memset(ch, 0, sizeof(*ch));
            ch->h = h;
            ch->usage = cands[j].usage;
            ch->inLen = cands[j].inLen ? cands[j].inLen : 20;    // fall back to a sane size
            ch->outLen = cands[j].outLen ? cands[j].outLen : 20; // if caps were 0
            ch->ev = CreateEvent(NULL, TRUE, FALSE, NULL);       // per-channel read event
        }

        if (d->nch > 0)
            ndev++; // keep the device only if at least one channel opened
    }
    return ndev;
}

// Close all handles/events of a device
static void close_device(Dev *d)
{
    for (int i = 0; i < d->nch; i++)
    {
        if (d->ch[i].h && d->ch[i].h != INVALID_HANDLE_VALUE)
            CloseHandle(d->ch[i].h);
        if (d->ch[i].ev)
            CloseHandle(d->ch[i].ev);
    }
    d->nch = 0;
}

// Resolve a feature id to its runtime feature index on this device.
// Returns the index (>0), 0 if the feature is unsupported, or -1 on no reply.
static int get_feature_index(Dev *d, uint8_t devindex, uint16_t feature)
{
    // Root getFeature takes the 16-bit feature id, big-endian, as two params
    uint8_t params[2] = {(uint8_t)(feature >> 8), (uint8_t)(feature & 0xFF)};
    uint8_t out[32];
    int ol = 0;
    int r = hidpp_request(d, devindex, ROOT_FEATURE_INDEX, FUNC_ROOT_GETFEATURE,
                          params, 2, out, &ol, NULL, 0, 0);
    return (r >= 1) ? out[0] : -1; // out[0] = feature index (0 = unsupported)
}

// Ping one device slot. Returns 1 if a connected HID++ 2.0+ device answered.
static int ping(Dev *d, uint8_t devindex)
{
    uint8_t mark = (uint8_t)(0x50 ^ devindex); // a per-slot marker byte to echo back
    uint8_t params[3] = {0, 0, mark};
    uint8_t out[32];
    int ol = 0;
    int r = hidpp_request(d, devindex, ROOT_FEATURE_INDEX, FUNC_ROOT_PING,
                          params, 3, out, &ol, NULL, mark, 1);
    // On success out[0] is the major protocol version; require >= 2 (HID++ 2.0)
    return (r >= 2 && out[0] >= 2);
}

/*
 * Scan every receiver/slot for a connected device that supports BACKLIGHT2.
 * On success returns 1 and sets *outdev / *outidx / *outbl. One quick pass; the
 * caller retries on its own schedule. Offline/empty slots answer with an error
 * almost immediately, so this is fast.
 */
static int find_backlight(Dev *devs, int ndev, Dev **outdev, uint8_t *outidx, uint8_t *outbl)
{
    for (int i = 0; i < ndev; i++)
    {
        for (size_t s = 0; s < sizeof(SLOTS); s++)
        {
            uint8_t idx = SLOTS[s];
            if (!ping(devs + i, idx))
                continue; // nothing live at this slot
            int bl = get_feature_index(devs + i, idx, FEATURE_BACKLIGHT2);
            if (bl > 0) // this device has the backlight feature: we're done
            {
                *outdev = devs + i;
                *outidx = idx;
                *outbl = (uint8_t)bl;
                return 1;
            }
        }
    }
    return 0;
}

// Send one backlight command as a 3-byte short pulse: [enabled, 0x00, effect].
//   enabled : 0 = off, 1 = on
//   effect  : 0xFF = leave the effect unchanged, 0x00 = static (no animation)
static void bl_pulse(Dev *d, uint8_t idx, uint8_t bl, uint8_t enabled, uint8_t effect)
{
    uint8_t p[3] = {enabled, 0x00, effect};
    uint8_t out[32];
    int ol = 0;
    hidpp_request(d, idx, bl, FUNC_BL_SET_CONFIG, p, 3, out, &ol, NULL, 0, 0);
}

// One keepalive: OFF immediately followed by ON. The off->on transition is what
// relights the LEDs and resets the firmware's fade timer (a lone "on" does not).
// The ON uses the static effect (0x00) so it snaps back to full brightness with
// no fade-in, making the unavoidable off-gap as brief as possible (just the
// wireless round-trip, a few milliseconds).
static void bl_keepalive(Dev *d, uint8_t idx, uint8_t bl)
{
    bl_pulse(d, idx, bl, 0x00, 0xFF); // OFF (effect left unchanged)
    bl_pulse(d, idx, bl, 0x01, 0x00); // ON, static effect, no gap
}

int main(void)
{
    // Manual-reset event used to break out of the loop on Ctrl+C
    g_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_stop)
    {
        fprintf(stderr, "Failed to create stop event (error %lu).\n", GetLastError());
        return 1;
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE); // register our Ctrl+C handler

    // Print status immediately instead of buffering it (matters when output is
    // piped/redirected rather than going to a live console)
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("mxlight: keeping the Logitech keyboard backlight on. Press Ctrl+C to stop.\n");

    Cand cands[MAX_CANDIDATES];
    Dev devs[MAX_GROUPS];
    int ndev = 0;   // number of open devices
    int have = 0;   // 1 once we have the device list open
    int misses = 0; // consecutive "keyboard not found" scans
    int state = -1; // last reported state: -1 unknown, 0 offline, 1 live

    for (;;)
    {
        // (Re)build the device list if we don't have one (startup, or after the
        // receiver seems to have gone away)
        if (!have)
        {
            for (int i = 0; i < ndev; i++)
                close_device(&devs[i]); // close any stale handles first
            int ncand = enumerate(cands, MAX_CANDIDATES);
            ndev = build_devices(cands, ncand, devs, MAX_GROUPS);
            have = (ndev > 0);
            if (!have)
            {
                fprintf(stderr, "No Logitech HID++ receiver found. Is it plugged in? Retrying...\n");
                if (interrupted(3000)) // wait 3s (or until Ctrl+C) then rescan
                    break;
                continue;
            }
        }

        // Find the keyboard (it may be asleep or on another Easy-Switch host)
        Dev *d;
        uint8_t idx, bl;
        if (find_backlight(devs, ndev, &d, &idx, &bl))
        {
            bl_keepalive(d, idx, bl); // one off->on pulse
            misses = 0;
            if (state != 1) // only announce on a state change
            {
                printf("Keyboard found - backlight will stay on.\n");
                state = 1;
            }
            if (interrupted(PULSE_INTERVAL_MS)) // sleep until the next pulse
                break;
        }
        else
        {
            // Not reachable right now. After several misses, assume the receiver
            // changed and rebuild the device list; otherwise just retry sooner.
            if (state != 0)
            {
                printf("Keyboard not reachable yet (asleep or on another device). Waiting...\n");
                state = 0;
            }
            if (++misses >= 5)
            {
                have = 0; // force a re-enumerate next loop
                misses = 0;
            }
            if (interrupted(RETRY_INTERVAL_MS))
                break;
        }
    }

    // Clean shutdown on Ctrl+C
    for (int i = 0; i < ndev; i++)
        close_device(&devs[i]);
    CloseHandle(g_stop);
    printf("\nStopped. The backlight will return to its normal behaviour.\n");
    return 0;
}
