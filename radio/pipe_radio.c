#include "pipe_radio.h"
#include "pipe_protocol.h"
#include <string.h>
#include <stdlib.h>

static void close_pipe(void *ctx) {
    struct pipe_radio *r = ctx;
    if (r->pipe && r->pipe != INVALID_HANDLE_VALUE) CloseHandle(r->pipe);
    r->pipe = INVALID_HANDLE_VALUE;
    if (r->io_event) CloseHandle(r->io_event);
    r->io_event = NULL;
    free(r->message); r->message = NULL;
    r->ack_armed = false;
}

/* A dead bridge must not hang the scheduler. Cancel and reap before freeing the OVERLAPPED. */
static int transfer(struct pipe_radio *r, void *data, DWORD size, int writing, DWORD *done, DWORD timeout) {
    OVERLAPPED ov = {0};
    ov.hEvent = r->io_event;
    if (!ResetEvent(ov.hEvent)) return -1;
    BOOL ok = writing ? WriteFile(r->pipe, data, size, done, &ov) : ReadFile(r->pipe, data, size, done, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeout) != WAIT_OBJECT_0) {
            CancelIoEx(r->pipe, &ov);
            GetOverlappedResult(r->pipe, &ov, done, TRUE);
            ok = FALSE;
        } else ok = GetOverlappedResult(r->pipe, &ov, done, FALSE);
    }
    return ok ? 0 : -1;
}

static int request(struct pipe_radio *r, unsigned cmd, unsigned arg, const void *in,
                   unsigned len, void *out, unsigned cap) {
    if (!r->pipe || r->pipe == INVALID_HANDLE_VALUE || len > RADIO_PIPE_MAX) return -1;
    unsigned char *msg = r->message;
    if (!msg) return -1;
    struct radio_message h = {RADIO_PIPE_VERSION, cmd, len, arg};
    memcpy(msg, &h, sizeof h);
    if (len) memcpy(msg + sizeof h, in, len);
    DWORD n = 0;
    int result = -1;
    if (transfer(r, msg, sizeof h + len, 1, &n, 3000) || n != sizeof h + len) goto end;
    /* Firmware loading and RF calibration can exceed a normal request's budget. */
    /* Measured cold T3U initialization exceeded20 s; only HELLO gets the bounded cold-start budget. */
    if (transfer(r, msg, sizeof h + RADIO_PIPE_MAX, 0, &n, cmd == RADIO_HELLO ? 45000 : 3000) || n < sizeof h) goto end;
    memcpy(&h, msg, sizeof h);
    if (h.version != RADIO_PIPE_VERSION || h.command != cmd || h.length > cap ||
        n != sizeof h + h.length) goto end;
    /* A well-framed error status (e.g. one failed USB send) fails this request only. */
    if (h.argument == UINT32_MAX) return -1;
    if (h.length) memcpy(out, msg + sizeof h, h.length);
    result = cmd == RADIO_HELLO ? (int)h.argument : (int)h.length;
end:
    if (result < 0) close_pipe(r); /* transport or framing failure: the bridge is unusable */
    return result;
}

static int open_pipe(void *ctx) {
    struct pipe_radio *r = ctx;
    if (!r->name || strncmp(r->name, "\\\\.\\pipe\\", 9) || !radio_channel_valid(r->channel) || (r->mac[0] & 1)) return -1;
    r->pipe = CreateFileA(r->name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, NULL);
    if (r->pipe == INVALID_HANDLE_VALUE) return -1;
    r->message = malloc(sizeof(struct radio_message) + RADIO_PIPE_MAX);
    r->io_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!r->message || !r->io_event) { close_pipe(r); return -1; }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(r->pipe, &mode, NULL, NULL)) { close_pipe(r); return -1; }
    int flags = request(r, RADIO_HELLO, r->channel, r->mac, 6, NULL, 0);
    r->ack_armed = flags >= 0 && (flags & 1);
    if (flags < 0) close_pipe(r);
    return flags < 0 ? -1 : 0;
}
static int capture(void *ctx, unsigned char *b, unsigned cap) {
    return request(ctx, RADIO_RX, 0, NULL, 0, b, cap);
}
static int transmit(void *ctx, const unsigned char *f, unsigned len) {
    if (!f || len < 8 || len > RADIO_PIPE_MAX) return -1;
    return request(ctx, RADIO_TX, 0, f, len, NULL, 0);
}
static int channel(void *ctx, unsigned ch) {
    struct pipe_radio *r = ctx;
    if (!radio_channel_valid(ch) || (r->fixed_channel && r->channel != ch)) return -1;
    int result = request(ctx, RADIO_CHANNEL, ch, NULL, 0, NULL, 0);
    if (!result) ((struct pipe_radio *)ctx)->channel = ch;
    return result;
}
static int capabilities(void *ctx, struct RadioCapabilities *out) {
    (void)ctx;
    memset(out, 0, sizeof *out);
    out->monitor_capture = true; /* physical T3U capture, 2026-10-15 */
    return 0;
}
const struct RadioBackend pipe_radio_backend = {open_pipe, capture, transmit, channel, capabilities, close_pipe};
