#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 600

#include "mdf_internal.h"

#include <errno.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef LIBMDF_PAGER_TESTING
/* Private test-build hook from pager.c; it is deliberately not a public API. */
void mdf_pager_test_set_termination_handler_install_failure(int enabled);
void mdf_pager_test_allow_small_cache(int enabled);
#endif

typedef struct capture {
    char *data;
    size_t len;
    size_t cap;
    size_t frame_start;
    size_t frame_end;
} capture;

typedef struct pager_source {
    const char *data;
    size_t len;
    size_t offset;
    size_t chunk;
} pager_source;

typedef struct pager_streaming_sanitize_source {
    const char *data;
    size_t len;
    size_t offset;
    size_t chunk;
    size_t reads;
    size_t writes;
    capture output;
} pager_streaming_sanitize_source;

typedef struct pager_failing_allocator {
    int fail;
    int remaining_successes;
} pager_failing_allocator;

typedef struct pager_oom_source {
    pager_failing_allocator *allocator;
    int sent;
} pager_oom_source;

typedef struct pager_view_oom_source {
    pager_failing_allocator *allocator;
    int sent;
    int remaining_successes;
    const char *data;
    size_t len;
} pager_view_oom_source;

typedef struct pager_route_allocation {
    void *ptr;
    size_t size;
} pager_route_allocation;

typedef struct pager_route_allocator {
    pager_route_allocation allocations[64];
    size_t allocation_count;
    size_t active_count;
    volatile int fail_allocations;
    int foreign_free;
} pager_route_allocator;

static int capture_append(capture *out, const char *src, size_t len)
{
    char *next;
    size_t required;
    size_t cap;

    required = out->len + len + 1;
    if (required < out->len) return -1;
    if (required > out->cap) {
        cap = out->cap == 0 ? 4096 : out->cap;
        while (cap < required) cap *= 2;
        next = (char *)realloc(out->data, cap);
        if (next == NULL) return -1;
        out->data = next;
        out->cap = cap;
    }
    memcpy(out->data + out->len, src, len);
    out->len += len;
    out->data[out->len] = '\0';
    return 0;
}

static size_t pager_streaming_sanitize_read(void *userdata, char *dst, size_t cap, int *err)
{
    pager_streaming_sanitize_source *source;
    size_t len;

    source = (pager_streaming_sanitize_source *)userdata;
    if (source->reads > source->writes) {
        *err = EIO;
        return 0;
    }
    if (source->offset == source->len) {
        *err = 0;
        return 0;
    }
    len = source->len - source->offset;
    if (len > cap) len = cap;
    if (len > source->chunk) len = source->chunk;
    memcpy(dst, source->data + source->offset, len);
    source->offset += len;
    source->reads++;
    *err = 0;
    return len;
}

static int pager_streaming_sanitize_write(void *userdata, const char *src, size_t len)
{
    pager_streaming_sanitize_source *source;

    source = (pager_streaming_sanitize_source *)userdata;
    if (capture_append(&source->output, src, len) != 0) return -1;
    source->writes++;
    return 0;
}

static int capture_find_from(const capture *out, const char *needle, size_t start, size_t *position)
{
    size_t i;
    size_t needle_len;

    if (out->data == NULL || start > out->len) return 0;
    needle_len = strlen(needle);
    if (needle_len > out->len - start) return 0;
    for (i = start; i <= out->len - needle_len; i++) {
        if (memcmp(out->data + i, needle, needle_len) == 0) {
            if (position != NULL) *position = i;
            return 1;
        }
    }
    return 0;
}

static int capture_contains(const capture *out, const char *needle)
{
    return capture_find_from(out, needle, 0, NULL);
}

static long elapsed_milliseconds(const struct timespec *start, const struct timespec *end)
{
    time_t seconds;
    long nanoseconds;

    seconds = end->tv_sec - start->tv_sec;
    nanoseconds = end->tv_nsec - start->tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }
    return seconds < 0 ? -1 : seconds * 1000L + nanoseconds / 1000000L;
}

static int wait_for_output(int fd, capture *out, long milliseconds)
{
    struct timespec started;

    if (fd < 0) return -1;
    out->frame_start = out->frame_end = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return -1;
    for (;;) {
        fd_set reads;
        struct timespec now;
        struct timeval timeout;
        char bytes[1024];
        long elapsed;
        long remaining;
        ssize_t n;
        int ready;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        elapsed = elapsed_milliseconds(&started, &now);
        if (elapsed < 0 || elapsed >= milliseconds) return 0;
        remaining = milliseconds - elapsed;
        if (remaining > 20) remaining = 20;
        FD_ZERO(&reads);
        FD_SET(fd, &reads);
        timeout.tv_sec = remaining / 1000;
        timeout.tv_usec = (remaining % 1000) * 1000;
        ready = select(fd + 1, &reads, NULL, NULL, &timeout);
        if (ready < 0 && errno != EINTR) return -1;
        if (ready > 0) {
            n = read(fd, bytes, sizeof(bytes));
            if (n > 0 && capture_append(out, bytes, (size_t)n) != 0) return -1;
        }
    }
}

static int wait_for_marker_after(int fd, capture *out, const char *marker,
                                 size_t start, size_t *marker_end)
{
    struct timespec started;
    size_t position;

    out->frame_start = out->frame_end = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return -1;
    for (;;) {
        struct timespec now;
        long elapsed;

        if (capture_find_from(out, marker, start, &position)) {
            if (marker_end != NULL) *marker_end = position + strlen(marker);
            return 0;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        elapsed = elapsed_milliseconds(&started, &now);
        if (elapsed < 0 || elapsed >= 2000) return -1;
        if (wait_for_output(fd, out, 20) != 0) return -1;
    }
}

static int wait_for_marker(int fd, capture *out, const char *marker)
{
    return wait_for_marker_after(fd, out, marker, 0, NULL);
}

static int capture_range_contains(const capture *out, const char *needle, size_t start, size_t end)
{
    size_t needle_len;
    size_t i;

    if (out->data == NULL || start > end || end > out->len) return 0;
    needle_len = strlen(needle);
    if (needle_len > end - start) return 0;
    for (i = start; i <= end - needle_len; i++) {
        if (memcmp(out->data + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

/* A pager redraw homes the cursor and ends with the status-row terminator.
 * Accept only a complete frame containing the expected marker: a marker from
 * a live-search redraw must not be paired with a later redraw's terminator. */
static int wait_for_redraw(int fd, capture *out, const char *marker)
{
    static const char redraw_start[] = "\033[H";
    /* The default pager theme starts its inverse status row with this exact
     * sequence.  Content rows can end in the same reset sequence as
     * the status row, so use the status-row start to locate the final one. */
    static const char status_start[] = "\033[7m\033[1;32m";
    static const char redraw_end[] = "\033[0m";
    struct timespec started;
    size_t scan;

    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return -1;
    scan = 0;
    for (;;) {
        struct timespec now;
        size_t start;
        size_t status;
        size_t end;
        long elapsed;

        if (capture_find_from(out, redraw_start, scan, &start) &&
            capture_find_from(out, status_start, start + strlen(redraw_start), &status) &&
            capture_find_from(out, redraw_end, status + strlen(status_start), &end)) {
            end += strlen(redraw_end);
            if (capture_range_contains(out, marker, start, end)) {
                out->frame_start = start;
                out->frame_end = end;
                return 0;
            }
            scan = end;
            continue;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        elapsed = elapsed_milliseconds(&started, &now);
        if (elapsed < 0 || elapsed >= 2000) return -1;
        if (wait_for_output(fd, out, 20) != 0) return -1;
    }
}

static int write_all(int fd, const char *src, size_t len)
{
    while (len > 0) {
        ssize_t n;

        n = write(fd, src, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        src += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int ignored_trace_emit(void *userdata, mdf_format format, const char *src, size_t len)
{
    (void)userdata;
    (void)format;
    (void)src;
    (void)len;
    return 0;
}

static size_t pager_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    pager_source *source;
    size_t n;

    source = (pager_source *)userdata;
    if (source->offset >= source->len) return 0;
    n = source->len - source->offset;
    if (n > source->chunk) n = source->chunk;
    if (n > cap) n = cap;
    memcpy(dst, source->data + source->offset, n);
    source->offset += n;
    *err = 0;
    return n;
}

static int pager_source_seek(void *userdata, const mdf_source_offset *offset)
{
    pager_source *source;

    source = (pager_source *)userdata;
    if (offset->high != 0 || offset->low > source->len) return -1;
    source->offset = (size_t)offset->low;
    return 0;
}

static void *pager_failing_alloc(void *userdata, size_t size)
{
    pager_failing_allocator *allocator;

    allocator = (pager_failing_allocator *)userdata;
    if (allocator->fail || allocator->remaining_successes == 0) return NULL;
    if (allocator->remaining_successes > 0) allocator->remaining_successes--;
    return malloc(size);
}

static void *pager_failing_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    pager_failing_allocator *allocator;

    (void)old_size;
    allocator = (pager_failing_allocator *)userdata;
    if (allocator->fail || allocator->remaining_successes == 0) return NULL;
    if (allocator->remaining_successes > 0) allocator->remaining_successes--;
    return realloc(ptr, new_size);
}

static void pager_failing_free(void *userdata, void *ptr, size_t size)
{
    (void)userdata;
    (void)size;
    free(ptr);
}

/* Deliberately provide no realloc hook. It detects the inherited libc realloc
 * from mdf_options_init and proves that cancelling a search preserves the
 * caller allocator for the next query. */
static void *pager_route_alloc(void *userdata, size_t size)
{
    pager_route_allocator *allocator;
    void *ptr;

    allocator = (pager_route_allocator *)userdata;
    if (allocator->fail_allocations || allocator->allocation_count == 64) return NULL;
    ptr = malloc(size);
    if (ptr == NULL) return NULL;
    allocator->allocations[allocator->allocation_count].ptr = ptr;
    allocator->allocations[allocator->allocation_count].size = size;
    allocator->allocation_count++;
    allocator->active_count++;
    return ptr;
}

static void pager_route_free(void *userdata, void *ptr, size_t size)
{
    pager_route_allocator *allocator;
    size_t i;

    allocator = (pager_route_allocator *)userdata;
    for (i = 0; i < allocator->allocation_count; i++) {
        if (allocator->allocations[i].ptr != ptr) continue;
        free(ptr);
        allocator->allocations[i].ptr = NULL;
        allocator->active_count--;
        (void)size;
        return;
    }
    allocator->foreign_free = 1;
}

static size_t pager_oom_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    pager_oom_source *source;

    source = (pager_oom_source *)userdata;
    if (source->sent) {
        *err = 0;
        return 0;
    }
    if (cap == 0) {
        *err = 1;
        return 0;
    }
    source->sent = 1;
    source->allocator->fail = 1;
    dst[0] = 'x';
    *err = 0;
    return 1;
}

static size_t pager_view_oom_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    pager_view_oom_source *source;

    source = (pager_view_oom_source *)userdata;
    if (source->sent) {
        source->allocator->remaining_successes = source->remaining_successes;
        *err = 0;
        return 0;
    }
    if (source->len > cap) {
        *err = 1;
        return 0;
    }
    memcpy(dst, source->data, source->len);
    source->sent = 1;
    *err = 0;
    return source->len;
}

typedef struct pager_cancel_probe {
    int fd;
    int phase;
    int requested;
    int reads;
    int eof;
    int checks_after_eof;
    size_t remaining;
    mdf *receiver;
    int reentry_checked;
    int reentry_failed;
    void *owned_emission;
    int owned_released;
} pager_cancel_probe;

static void *pager_cancel_alloc(void *userdata, size_t size)
{
    (void)userdata;
    return malloc(size);
}

static void *pager_cancel_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    pager_cancel_probe *probe;
    void *next;
    int owned;

    (void)old_size;
    probe = (pager_cancel_probe *)userdata;
    owned = ptr == probe->owned_emission;
    next = realloc(ptr, new_size);
    if (owned && next != NULL) probe->owned_emission = next;
    return next;
}

static void pager_cancel_free(void *userdata, void *ptr, size_t size)
{
    pager_cancel_probe *probe;

    (void)size;
    probe = (pager_cancel_probe *)userdata;
    if (ptr == probe->owned_emission) {
        probe->owned_emission = NULL;
        probe->owned_released++;
    }
    free(ptr);
}

static int pager_cancel_requested(void *userdata)
{
    pager_cancel_probe *probe;
    char byte;

    probe = (pager_cancel_probe *)userdata;
    if (probe->receiver != NULL && !probe->reentry_checked) {
        probe->reentry_checked = 1;
        if (probe->receiver->reset(probe->receiver) != MDF_ERROR_INVALID ||
            probe->receiver->pager_file(probe->receiver, "unused", MDF_PAGER_FORMAT_TEXT, NULL) != MDF_ERROR_INVALID)
            probe->reentry_failed = 1;
        probe->receiver->destroy(probe->receiver);
    }
    if (probe->phase == 3) return 1;
    if ((probe->phase == 2 || probe->phase == 4) && probe->eof &&
        ++probe->checks_after_eof >= (probe->phase == 4 ? 40 : 4)) return 1;
    if (probe->fd >= 0 && read(probe->fd, &byte, 1) == 1) probe->requested = 1;
    return probe->requested;
}

static size_t pager_cancel_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    pager_cancel_probe *probe;
    size_t n;

    probe = (pager_cancel_probe *)userdata;
    *err = 0;
    if (probe->remaining == 0) { probe->eof = 1; return 0; }
    if (probe->phase == 5) {
        if (cap < 13) { *err = 1; return 0; }
        memcpy(dst, "abcdefghijkl\n", 13);
        probe->remaining = 0;
        probe->reads++;
        return 13;
    }
    n = probe->remaining < cap ? probe->remaining : cap;
    memset(dst, 'x', n);
    /* Phase 4 cancels during a genuinely undecided word, including its spool.
     * Read-ahead may now pause as soon as a complete viewport is available. */
    if (probe->phase != 4 || probe->remaining == n) dst[n - 1] = '\n';
    probe->remaining -= n;
    probe->reads++;
    if (probe->phase == 1) probe->requested = 1;
    return n;
}

static pid_t start_cancelable_pager(const char *path, int from_source, int receiver,
                                    int phase, mdf_pager_format format, int *master, int *cancel_fd)
{
    static const char text[] = "Literal **log**\nabcdefghijklmnopqrstuvwxyz01234567890123456789012345\nneedle entry\n";
    int control_pipe[2];
    struct winsize size;
    pid_t pid;

    if (pipe(control_pipe) != 0) return -1;
    memset(&size, 0, sizeof(size));
    size.ws_col = 20;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) {
        close(control_pipe[0]);
        *cancel_fd = control_pipe[1];
        return pid;
    }
    {
        pager_cancel_probe probe;
        pager_source source_data;
        mdf_pager_control control;
        mdf_options opts;
        mdf_source source;
        mdf_status status;
        struct sigaction before_term, before_winch, after;
        char *rendered;
        int ok;

        close(control_pipe[1]);
        if (fcntl(control_pipe[0], F_SETFL, O_NONBLOCK) != 0) _exit(2);
        memset(&probe, 0, sizeof(probe));
        probe.fd = control_pipe[0];
        probe.phase = phase;
        probe.remaining = 65536;
        control.userdata = &probe;
        control.should_cancel = pager_cancel_requested;
        mdf_options_init(&opts);
        opts.ansi_mode = MDF_ANSI_OFF;
        opts.allocator.userdata = &probe;
        opts.allocator.alloc = pager_cancel_alloc;
        opts.allocator.realloc = pager_cancel_realloc;
        opts.allocator.free = pager_cancel_free;
        if (sigaction(SIGTERM, NULL, &before_term) != 0 || sigaction(SIGWINCH, NULL, &before_winch) != 0) _exit(2);
        opts.emission_buffer.data = malloc(128);
        if (opts.emission_buffer.data == NULL) _exit(2);
        opts.emission_buffer.cap = 128;
        opts.emission_buffer.take_ownership = 1;
        probe.owned_emission = opts.emission_buffer.data;
        if (phase == 5) {
            opts.emission_buffer.max_cap = 8;
            opts.emission_buffer.fixed = 1;
        }
        if (receiver) {
            if (mdf_create(MDF_FORMAT_ANSI, &opts, &probe.receiver) != MDF_OK) _exit(2);
        }
        memset(&source_data, 0, sizeof(source_data));
        source_data.data = text;
        source_data.len = sizeof(text) - 1;
        source_data.chunk = 4096;
        source.userdata = phase == 0 ? (void *)&source_data : (void *)&probe;
        source.read = phase == 0 ? pager_source_read : pager_cancel_source_read;
        if (receiver) {
            status = from_source ? probe.receiver->pager_source(probe.receiver, "log.md", &source, format, &control) :
                                   probe.receiver->pager_file(probe.receiver, path, format, &control);
        } else {
            status = from_source ? mdf_pager_source("log.md", &source, &opts, format, &control) :
                                   mdf_pager_file(path, &opts, format, &control);
        }
        ok = status == MDF_CANCELLED && !probe.reentry_failed;
        if (phase == 5) ok = status != MDF_OK && status != MDF_CANCELLED && !probe.reentry_failed;
        if (phase == 1) ok = ok && probe.reads == 1;
        if (phase == 3) ok = ok && probe.reads == 0;
        if (phase == 2) ok = ok && probe.checks_after_eof == 4;
        if (phase == 4) ok = ok && probe.checks_after_eof == 40;
        if (sigaction(SIGTERM, NULL, &after) != 0 || after.sa_handler != before_term.sa_handler) ok = 0;
        if (sigaction(SIGWINCH, NULL, &after) != 0 || after.sa_handler != before_winch.sa_handler) ok = 0;
        if (probe.receiver != NULL) {
            rendered = NULL;
            if (probe.receiver->render_cstr(probe.receiver, "ok\n", &rendered) != MDF_OK ||
                rendered == NULL || strstr(rendered, "ok") == NULL) ok = 0;
            probe.receiver->string_free(probe.receiver, rendered);
            probe.receiver->destroy(probe.receiver);
        }
        ok = ok && probe.owned_released == 1;
        close(control_pipe[0]);
        _exit(ok ? 0 : 1);
    }
}

static pid_t start_pager_at_size(const char *cmdf, const char *path, int direct, int osc8, int traced,
                                 unsigned short columns, unsigned short rows, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = columns;
    size.ws_row = rows;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    if (direct) {
        mdf_options opts;

        mdf_options_init(&opts);
        opts.osc8 = osc8;
        if (traced) opts.write_trace.emit = ignored_trace_emit;
        _exit(mdf_pager_file(path, &opts, MDF_PAGER_FORMAT_AUTO, NULL) == MDF_OK ? 0 : 1);
    }
    execl(cmdf, cmdf, "-p", path, (char *)NULL);
    _exit(127);
}

static pid_t start_pager(const char *cmdf, const char *path, int direct, int osc8, int traced, int *master)
{
    return start_pager_at_size(cmdf, path, direct, osc8, traced, 40, 10, master);
}

static pid_t start_selected_pager(const char *cmdf, const char *path, const char *format,
                                  int incremental, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    if (incremental) execl(cmdf, cmdf, "--pager", "--pager-format", format, "--incremental", path, (char *)NULL);
    else execl(cmdf, cmdf, "--pager", "--pager-format", format, path, (char *)NULL);
    _exit(127);
}

static pid_t start_lua_pager(const char *lua, const char *script, const char *path,
                             const char *mode, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    if (strcmp(mode, "oversize") == 0) { size.ws_col = 256; size.ws_row = 258; }
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    execl(lua, lua, script, path, mode, (char *)NULL);
    _exit(127);
}

static pid_t start_incremental_pager(const char *cmdf, const char *path, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    execl(cmdf, cmdf, "-p", "--incremental", path, (char *)NULL);
    _exit(127);
}

static pid_t start_pager_with_expected_status(const char *path, mdf_status expected, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;

        mdf_options_init(&opts);
        _exit(mdf_pager_file(path, &opts, MDF_PAGER_FORMAT_AUTO, NULL) == expected ? 0 : 1);
    }
}

static pid_t start_pager_with_theme(const char *path, const char *theme_name, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;

        mdf_options_init(&opts);
        opts.theme_name = theme_name;
        _exit(mdf_pager_file(path, &opts, MDF_PAGER_FORMAT_TEXT, NULL) == MDF_OK ? 0 : 1);
    }
}

static pid_t start_pager_source(const char *name, const char *data, size_t len, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;
        mdf_source source;
        pager_source source_data;

        mdf_options_init(&opts);
        memset(&source_data, 0, sizeof(source_data));
        source_data.data = data;
        source_data.len = len;
        source_data.chunk = 2;
        source.userdata = &source_data;
        source.read = pager_source_read;
        _exit(mdf_pager_source(name, &source, &opts, MDF_PAGER_FORMAT_AUTO, NULL) == MDF_OK ? 0 : 1);
    }
}

static pid_t start_pager_source_with_oom(int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;
        mdf_source source;
        pager_failing_allocator allocator;
        pager_oom_source source_data;

        memset(&allocator, 0, sizeof(allocator));
        allocator.remaining_successes = -1;
        memset(&source_data, 0, sizeof(source_data));
        source_data.allocator = &allocator;
        source.userdata = &source_data;
        source.read = pager_oom_source_read;
        mdf_options_init(&opts);
        opts.allocator.userdata = &allocator;
        opts.allocator.alloc = pager_failing_alloc;
        opts.allocator.realloc = pager_failing_realloc;
        opts.allocator.free = pager_failing_free;
        _exit(mdf_pager_source("oom.txt", &source, &opts, MDF_PAGER_FORMAT_TEXT, NULL) == MDF_ERROR_NOMEM ? 0 : 1);
    }
}

static pid_t start_pager_source_with_view_oom(int markdown, int *master)
{
    static const char text[] = "needle\n";
    static const char markdown_text[] = "# needle\n";
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;
        mdf_source source;
        pager_failing_allocator allocator;
        pager_view_oom_source source_data;

        memset(&allocator, 0, sizeof(allocator));
        allocator.remaining_successes = -1;
        memset(&source_data, 0, sizeof(source_data));
        source_data.allocator = &allocator;
        source_data.remaining_successes = markdown ? 0 : 3;
        source_data.data = markdown ? markdown_text : text;
        source_data.len = markdown ? sizeof(markdown_text) - 1 : sizeof(text) - 1;
        source.userdata = &source_data;
        source.read = pager_view_oom_source_read;
        mdf_options_init(&opts);
        opts.allocator.userdata = &allocator;
        opts.allocator.alloc = pager_failing_alloc;
        opts.allocator.realloc = pager_failing_realloc;
        opts.allocator.free = pager_failing_free;
        alarm(2);
        _exit(mdf_pager_source(markdown ? "oom.md" : "oom.txt", &source, &opts,
                               markdown ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL) == MDF_ERROR_NOMEM ? 0 : 1);
    }
}

static pid_t start_pager_with_allocator_route_probe(pager_route_allocator *allocator, int *master)
{
    static const char text[] = "abc abc\n";
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;
        mdf_source source;
        pager_source source_data;
        mdf_status status;

        memset(&source_data, 0, sizeof(source_data));
        source_data.data = text;
        source_data.len = sizeof(text) - 1;
        source_data.chunk = 2;
        source.userdata = &source_data;
        source.read = pager_source_read;
        mdf_options_init(&opts);
        opts.allocator.userdata = allocator;
        opts.allocator.alloc = pager_route_alloc;
        opts.allocator.free = pager_route_free;
        status = mdf_pager_source("allocator.txt", &source, &opts, MDF_PAGER_FORMAT_TEXT, NULL);
        _exit(status == MDF_ERROR_NOMEM && allocator->foreign_free == 0 &&
              allocator->active_count == 0 ? 0 : 1);
    }
}

static pid_t start_pager_with_handler_install_failure(pager_route_allocator *allocator, int *master)
{
    static const char text[] = "cleanup\n";
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;
        mdf_source source;
        pager_source source_data;
        mdf_status status;

        memset(&source_data, 0, sizeof(source_data));
        source_data.data = text;
        source_data.len = sizeof(text) - 1;
        source_data.chunk = 2;
        source.userdata = &source_data;
        source.read = pager_source_read;
        mdf_options_init(&opts);
        opts.allocator.userdata = allocator;
        opts.allocator.alloc = pager_route_alloc;
        opts.allocator.free = pager_route_free;
        opts.emission_buffer.data = (char *)pager_route_alloc(allocator, 128);
        if (opts.emission_buffer.data == NULL) _exit(1);
        opts.emission_buffer.cap = 128;
        opts.emission_buffer.take_ownership = 1;
#ifdef LIBMDF_PAGER_TESTING
        mdf_pager_test_set_termination_handler_install_failure(1);
        status = mdf_pager_source("handler-failure.txt", &source, &opts, MDF_PAGER_FORMAT_TEXT, NULL);
        mdf_pager_test_set_termination_handler_install_failure(0);
        _exit(status == MDF_ERROR_IO && allocator->foreign_free == 0 &&
              allocator->active_count == 0 ? 0 : 1);
#else
        (void)status;
        _exit(1);
#endif
    }
}

static pid_t start_pager_with_owned_emission_buffer(const char *path, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;

        mdf_options_init(&opts);
        opts.emission_buffer.data = (char *)malloc(128);
        if (opts.emission_buffer.data == NULL) _exit(1);
        opts.emission_buffer.cap = 128;
        opts.emission_buffer.take_ownership = 1;
        _exit(mdf_pager_file(path, &opts, MDF_PAGER_FORMAT_MARKDOWN, NULL) == MDF_OK ? 0 : 1);
    }
}

static pid_t start_pager_with_fixed_owned_emission_buffer(const char *path,
                                                           unsigned short columns, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = columns;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    {
        mdf_options opts;

        mdf_options_init(&opts);
        opts.emission_buffer.data = (char *)malloc(32);
        if (opts.emission_buffer.data == NULL) _exit(1);
        opts.emission_buffer.cap = 32;
        opts.emission_buffer.fixed = 1;
        opts.emission_buffer.take_ownership = 1;
        _exit(mdf_pager_file(path, &opts, MDF_PAGER_FORMAT_MARKDOWN, NULL) == MDF_OK ? 0 : 1);
    }
}

static int wait_for_exit(pid_t pid)
{
    int status;

    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int wait_for_failure(pid_t pid)
{
    int status;

    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) != 0 ? 0 : -1;
}

static int wait_for_signal(pid_t pid, int signal_number)
{
    int status;

    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFSIGNALED(status) && WTERMSIG(status) == signal_number ? 0 : -1;
}

static int terminal_is_cooked(int fd)
{
    struct termios state;

    if (tcgetattr(fd, &state) != 0) return 0;
    return (state.c_lflag & (ECHO | ICANON | ISIG)) == (ECHO | ICANON | ISIG);
}

static int write_fixture(char *path, size_t cap, const char *suffix, int markdown)
{
    int fd;
    int i;
    char line[64];
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (markdown) {
        if (write_all(fd, "# rendered heading\n\n", strlen("# rendered heading\n\n")) != 0) return -1;
    } else if (write_all(fd, "# raw heading left **literal** right\n",
                          strlen("# raw heading left **literal** right\n")) != 0) {
        return -1;
    }
    if (write_all(fd, "S\303\244kerhet S\303\204KERHET \346\227\245\346\234\254\350\252\236\346\244\234\347\264\242 \346\227\245\346\234\254\350\252\236\346\244\234\347\264\242\n",
                  strlen("S\303\244kerhet S\303\204KERHET \346\227\245\346\234\254\350\252\236\346\244\234\347\264\242 \346\227\245\346\234\254\350\252\236\346\244\234\347\264\242\n")) != 0) {
        return -1;
    }
    for (i = 1; i <= 60; i++) {
        int len;

        len = snprintf(line, sizeof(line), "line-%02d narrow-resize-anchor-content\n", i);
        if (len < 0 || write_all(fd, line, (size_t)len) != 0) return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.%s", temporary, suffix) >= (int)cap ||
        rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_osc8_fixture(char *path, size_t cap)
{
    int fd;
    const char *source;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];

    source = "Regeringens [molnpolicy f\303\266r Sverige](https://www.regeringen.se/contentassets/112463c2b5404f84826545ba01967f28/en-molnpolicy-for-sverige--for-okad-sakerhet-effektivitet-och-innovation-i-den-offentliga-forvaltningen1.pdf) f\303\266rbjuder inte Azure och kr\303\244ver inte ett svenskt eller suver\303\244nt moln. Den betonar d\303\244remot kontroll, portabilitet, ers\303\244ttningsstrategier, *exit* och kontinuitet.\n";
    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-osc8-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, strlen(source)) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_osc8_reflow_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] = "[abc](https://example.com)\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-osc8-reflow-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_incremental_resize_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] =
        "| first column | second column |\n"
        "| --- | --- |\n"
        "| alpha beta gamma | delta epsilon zeta |\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-incremental-resize-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_wide_fixture(char *path, size_t cap)
{
    int fd;
    const char *source;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];

    source = "\346\227\245\346\234\254\350\252\236\346\227\245\346\234\254\350\252\236\n";
    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-wide-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, strlen(source)) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.txt", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_wide_tab_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] = "\346\227\245\tX\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-wide-tab-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.txt", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_utf8_search_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] = "abc\346\227\245\346\234\254\350\252\236 abc\346\227\245\346\234\254\350\252\236\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-utf8-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.txt", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_unicode_casefold_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] = "\316\221\316\222\316\223\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-casefold-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.txt", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_initial_resize_fixture(char *path, size_t cap)
{
    int fd;
    int i;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char line[] = "0123456789012345678901234567890123456789 initial resize rendering payload\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-initial-resize-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    for (i = 0; i < 500; i++) {
        if (write_all(fd, line, sizeof(line) - 1) != 0) {
            close(fd);
            unlink(temporary);
            return -1;
        }
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.txt", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_status_path_fixture(char *path, size_t cap, const char *suffix)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, "status fixture\n", strlen("status fixture\n")) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s%s", temporary, suffix) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_styled_search_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-styled-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, "foo `bar` baz\n", strlen("foo `bar` baz\n")) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_long_word_markdown_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-long-word-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_nul_markdown_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char before[] = "# Before NUL\n\n";
    static const char after[] = "After NUL\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-nul-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, before, sizeof(before) - 1) != 0 || write_all(fd, "\0", 1) != 0 ||
        write_all(fd, after, sizeof(after) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_fifo_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-fifo-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (close(fd) != 0 || snprintf(path, cap, "%s.fifo", temporary) >= (int)cap) {
        unlink(temporary);
        return -1;
    }
    if (unlink(temporary) != 0 || mkfifo(path, 0600) != 0) {
        unlink(path);
        return -1;
    }
    return 0;
}

static int write_c1_text_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] = "before \23552;c;INJECT\a after\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-c1-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.txt", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_carriage_return_text_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] = "crlf line\r\nstandalone\rreturn\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-cr-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.txt", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_control_markdown_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    static const char source[] = "# Markdown controls\n\nbefore \033]52;c;INJECT\a and \23552;c;C1\a after\n";

    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-markdown-control-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, sizeof(source) - 1) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int require_contains(const capture *out, const char *needle)
{
    if (out->frame_end > out->frame_start)
        return capture_range_contains(out, needle, out->frame_start, out->frame_end) ? 0 : -1;
    return capture_contains(out, needle) ? 0 : -1;
}

static int require_full_status_bar(const capture *out, const char *path)
{
    const char *prefix;
    const char *suffix;
    char expected[128];
    size_t path_len;
    size_t shown_path_len;
    size_t position;

    prefix = "\033[7m\033[1;32m ";
    suffix = "0% \033[0m";
    path_len = strlen(path);
    shown_path_len = path_len > 35 ? 35 : path_len;
    position = 0;
    memcpy(expected + position, prefix, strlen(prefix));
    position += strlen(prefix);
    memcpy(expected + position, path, shown_path_len);
    position += shown_path_len;
    memset(expected + position, ' ', 36 - shown_path_len);
    position += 36 - shown_path_len;
    memcpy(expected + position, suffix, strlen(suffix));
    position += strlen(suffix);
    expected[position] = '\0';
    return require_contains(out, expected);
}

static int clear_capture(capture *out)
{
    out->frame_start = out->frame_end = 0;
    out->len = 0;
    if (out->data != NULL) out->data[0] = '\0';
    return 0;
}

static int check_redraw_assertion_scope(void)
{
    static const char frames[] = "\033[Hfirst passage\r\n\033[7m\033[1;32m 1/2\033[0m"
        "\033[Hsecond passage\r\n\033[7m\033[1;32m 2/2\033[0m";
    capture out;
    int result;

    memset(&out, 0, sizeof(out));
    result = capture_append(&out, frames, sizeof(frames) - 1) == 0 &&
        wait_for_redraw(-1, &out, "2/2") == 0 && require_contains(&out, "second passage") == 0 &&
        require_contains(&out, "first passage") != 0 && require_contains(&out, "1/2") != 0 ? 0 : -1;
    free(out.data);
    return result;
}

static size_t count_occurrences(const char *text, const char *needle)
{
    size_t count;
    size_t len;

    count = 0;
    len = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += len;
    }
    return count;
}

static void sleep_ms(long milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000L;
    delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int require_overwrite_frame(const capture *out, const char *body)
{
    /* Erasing the display or a whole row before painting recreates the flash.
     * Exact row suffixes also catch EL erasing a full-width row's last cell. */
    if (out->data == NULL || count_occurrences(out->data, "\033[H") != 1 ||
        strstr(out->data, "\033[J") != NULL || strstr(out->data, "\033[0J") != NULL ||
        strstr(out->data, "\033[1J") != NULL || strstr(out->data, "\033[2J") != NULL ||
        strstr(out->data, "\033[2K") != NULL || require_contains(out, body) != 0) return -1;
    return 0;
}

static int check_overwrite_redraws(const char *cmdf)
{
    static const char source[] = "0123456789ABCDEF\nshort\n\nABCDEFGHIJKLMNO\ntail\n";
    static const char initial[] = "\033[H0123456789ABCDEF\033[0m\r\nshort\033[0m\033[K\r\n\033[0m\033[K\r\n";
    static const char down[] = "\033[Hshort\033[0m\033[K\r\n\033[0m\033[K\r\nABCDEFGHIJKLMNO\033[0m\033[K\r\n";
    static const char last[] = "\033[H\033[0m\033[K\r\nABCDEFGHIJKLMNO\033[0m\033[K\r\ntail\033[0m\033[K\r\n";
    static const char expanded[] = "\033[H0123456789ABCDEF\033[0m\r\nshort\033[0m\033[K\r\n\033[0m\033[K\r\nABCDEFGHIJKLMNO\033[0m\033[K\r\ntail\033[0m\033[K\r\n\033[0m\033[K\r\n\033[0m\033[K\r\n";
    static const char *const bodies[] = {initial, down, initial, last, initial};
    static const char *const markers[] = {"short", "short", "short", "tail", "short"};
    static const char keys[] = "jkGg";
    char temporary[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 132];
    struct winsize size;
    capture out;
    pid_t pid;
    int fd;
    int master;
    int direct;
    int result;
    int exited;
    size_t step;

    memset(&out, 0, sizeof(out));
    temporary[0] = '\0';
    path[0] = '\0';
    pid = -1;
    master = -1;
    result = -1;
    if (snprintf(temporary, sizeof(temporary), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-overwrite-XXXXXX") >= (int)sizeof(temporary)) goto done;
    fd = mkstemp(temporary);
    if (fd < 0) goto done;
    if (write_all(fd, source, sizeof(source) - 1) != 0) { close(fd); goto done; }
    if (close(fd) != 0 || snprintf(path, sizeof(path), "%s.txt", temporary) >= (int)sizeof(path) ||
        rename(temporary, path) != 0) goto done;
    for (direct = 0; direct <= 1; direct++) {
        pid = start_pager_at_size(cmdf, path, direct, 0, 0, 16, 4, &master);
        if (pid < 0) goto done;
        for (step = 0; step < sizeof(bodies) / sizeof(bodies[0]); step++) {
            if (step > 0) {
                clear_capture(&out);
                if (write_all(master, keys + step - 1, 1) != 0) goto done;
            }
            if (wait_for_redraw(master, &out, markers[step]) != 0 ||
                require_overwrite_frame(&out, bodies[step]) != 0) goto done;
        }
        clear_capture(&out);
        memset(&size, 0, sizeof(size));
        size.ws_col = 16;
        size.ws_row = 8;
        if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
            wait_for_redraw(master, &out, "tail") != 0 || require_overwrite_frame(&out, expanded) != 0) goto done;
        clear_capture(&out);
        size.ws_col = 1;
        if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
            wait_for_redraw(master, &out, "0") != 0 ||
            require_overwrite_frame(&out, "\033[H0\033[0m\r\n1\033[0m\r\n") != 0 ||
            require_contains(&out, "\033[7m\033[1;32m0\033[0m") != 0) goto done;
        if (write_all(master, "q", 1) != 0) goto done;
        exited = wait_for_exit(pid);
        pid = -1;
        if (exited != 0 || !terminal_is_cooked(master)) goto done;
        close(master);
        master = -1;
        clear_capture(&out);
    }
    result = 0;
done:
    if (result != 0 && out.len > 0) fwrite(out.data, 1, out.len, stderr);
    if (pid > 0) { kill(pid, SIGTERM); (void)waitpid(pid, NULL, 0); }
    if (master >= 0) close(master);
    if (temporary[0] != '\0') unlink(temporary);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return result;
}

static volatile sig_atomic_t pager_sigint_delivered;

static void pager_returning_sigint_handler(int signal_number)
{
    if (signal_number == SIGINT) pager_sigint_delivered++;
}

static int check_sigint_return_status(const char *text_path, const char *markdown_path)
{
    static const char source_text[] = "signal fixture\n";
    capture out;
    struct winsize size;
    pid_t pid;
    int master;
    int mode;
    int rc;
    int child_result;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = -1;
    master = -1;
    rc = -1;
    for (mode = 0; mode < 16; mode++) {
        const char *path;
        const char *marker;
        mdf_pager_format format;

        format = mode & 4 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT;
        path = mode & 4 ? markdown_path : text_path;
        marker = mode & 1 ? "signal fixture" : (mode & 4 ? "rendered heading" : "# raw heading");
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf *receiver;
            mdf_source source;
            pager_source input;
            struct sigaction action, saved_int, saved_winch, restored;
            sigset_t saved_mask, restored_mask;
            mdf_status status;
            char *rendered;
            int ok;

            memset(&action, 0, sizeof(action));
            action.sa_handler = mode & 8 ? SIG_IGN : pager_returning_sigint_handler;
            sigemptyset(&action.sa_mask);
            pager_sigint_delivered = 0;
            if (sigaction(SIGINT, &action, NULL) != 0 ||
                sigaction(SIGINT, NULL, &saved_int) != 0 ||
                sigaction(SIGWINCH, NULL, &saved_winch) != 0 ||
                sigprocmask(SIG_SETMASK, NULL, &saved_mask) != 0) _exit(2);
            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            receiver = NULL;
            if ((mode & 2) && mdf_create(MDF_FORMAT_ANSI, &opts, &receiver) != MDF_OK) _exit(2);
            memset(&input, 0, sizeof(input));
            input.data = source_text;
            input.len = sizeof(source_text) - 1;
            input.chunk = 3;
            source.userdata = &input;
            source.read = pager_source_read;
            if (receiver != NULL) {
                status = mode & 1 ? receiver->pager_source(receiver, "signal.txt", &source, format, NULL) :
                                    receiver->pager_file(receiver, path, format, NULL);
            } else {
                status = mode & 1 ? mdf_pager_source("signal.txt", &source, &opts, format, NULL) :
                                    mdf_pager_file(path, &opts, format, NULL);
            }
            ok = status == MDF_CANCELLED && pager_sigint_delivered == (mode & 8 ? 0 : 1);
            if (sigaction(SIGINT, NULL, &restored) != 0 || restored.sa_handler != saved_int.sa_handler) ok = 0;
            if (sigaction(SIGWINCH, NULL, &restored) != 0 || restored.sa_handler != saved_winch.sa_handler) ok = 0;
            if (sigprocmask(SIG_SETMASK, NULL, &restored_mask) != 0 ||
                sigismember(&restored_mask, SIGINT) != sigismember(&saved_mask, SIGINT) ||
                sigismember(&restored_mask, SIGTERM) != sigismember(&saved_mask, SIGTERM)) ok = 0;
            if (receiver != NULL) {
                rendered = NULL;
                if (receiver->render_cstr(receiver, "usable\n", &rendered) != MDF_OK ||
                    rendered == NULL || strstr(rendered, "usable") == NULL) ok = 0;
                receiver->string_free(receiver, rendered);
                receiver->destroy(receiver);
            }
            if (!ok) fprintf(stderr, "SIGINT pager mode=%d returned status=%d, signals=%d\n",
                             mode, (int)status, (int)pager_sigint_delivered);
            _exit(ok ? 0 : 1);
        }
        if (wait_for_redraw(master, &out, marker) != 0 || write_all(master, "\003", 1) != 0) goto done;
        child_result = wait_for_exit(pid);
        pid = -1;
        if (wait_for_marker(master, &out, "\033[0m\033[?25h\033[?1049l") != 0 || child_result != 0 ||
            !terminal_is_cooked(master)) goto done;
        close(master);
        master = -1;
        clear_capture(&out);
    }
    rc = 0;
done:
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (master >= 0) close(master);
    if (rc != 0 && out.len > 0) fwrite(out.data, 1, out.len, stderr);
    free(out.data);
    return rc;
}

typedef struct pager_large_source {
    uint64_t offset;
    uint64_t length;
    size_t reads;
    size_t bytes_read;
    size_t seeks;
    size_t live;
    size_t peak;
    size_t allocation_limit;
    size_t expected_content_allocation;
    size_t content_allocations;
    size_t content_frees;
    void *content_pointer;
    size_t chunk;
    size_t record_width;
    unsigned long marker_row;
    int extra_marker;
    int fail_read;
    int fail_seek;
    int fail_alloc;
    int close_stdout;
    int cancel;
    size_t cancel_after;
    int markdown;
    const char *literal_record;
    size_t literal_labels;
} pager_large_source;

static size_t pager_large_read(void *userdata, char *dst, size_t cap, int *err)
{
    pager_large_source *source;
    size_t n;
    size_t i;
    char label[32];
    int label_len;
    int have_label;
    unsigned long previous_row;

    source = (pager_large_source *)userdata;
    *err = 0;
    if (source->fail_read) { *err = EIO; return 0; }
    n = cap;
    if (n > source->chunk) n = source->chunk;
    if ((uint64_t)n > source->length - source->offset) n = (size_t)(source->length - source->offset);
    label_len = 0;
    have_label = 0;
    previous_row = 0;
    for (i = 0; i < n; i++) {
        uint64_t offset;
        unsigned long row;
        size_t column;

        offset = source->offset + i;
        if (source->literal_record != NULL) {
            dst[i] = source->literal_record[(size_t)(offset % source->record_width)];
            if (source->literal_labels != 0 && offset / source->record_width >= source->literal_labels &&
                offset % source->record_width == 1) dst[i] = 'b';
            continue;
        }
        if (source->markdown && (offset < 4 || offset >= source->length - 4)) {
            dst[i] = "```\n"[(size_t)(offset % 4)];
            continue;
        }
        if (source->markdown) offset -= 4;
        row = (unsigned long)(offset / source->record_width);
        column = (size_t)(offset % source->record_width);
        if (!have_label || row != previous_row) {
            label_len = snprintf(label, sizeof(label), "row%08lu %s", row,
                                  source->extra_marker && row == source->marker_row ? "XYZ" :
                                  (row % 32 == 0 ? "needle" : "plain"));
            previous_row = row;
            have_label = 1;
        }
        dst[i] = column == source->record_width - 1 ? '\n' :
            (column < (size_t)label_len ? label[column] : ' ');
    }
    source->offset += n;
    source->reads++;
    source->bytes_read += n;
    return n;
}

static int pager_large_seek(void *userdata, const mdf_source_offset *offset)
{
    pager_large_source *source;
    uint64_t value;

    source = (pager_large_source *)userdata;
    if (source->fail_seek) return -1;
    value = ((uint64_t)offset->high << 32) | offset->low;
    if (value > source->length) return -1;
    source->offset = value;
    source->seeks++;
    return 0;
}

static void *pager_large_alloc(void *userdata, size_t size)
{
    pager_large_source *source;
    void *ptr;

    source = (pager_large_source *)userdata;
    if (source->fail_alloc) return NULL;
    if (size > source->allocation_limit - source->live) return NULL;
    ptr = malloc(size);
    if (ptr != NULL) {
        if (size == source->expected_content_allocation && source->content_pointer == NULL) {
            source->content_allocations++;
            source->content_pointer = ptr;
        }
        source->live += size;
        if (source->live > source->peak) source->peak = source->live;
    }
    return ptr;
}

static void pager_large_free(void *userdata, void *ptr, size_t size)
{
    pager_large_source *source;

    source = (pager_large_source *)userdata;
    if (ptr == NULL) return;
    if (ptr == source->content_pointer) {
        source->content_frees++;
        source->content_pointer = NULL;
    }
    source->live -= size;
    free(ptr);
}

static int pager_large_cancel(void *userdata)
{
    pager_large_source *source;

    source = (pager_large_source *)userdata;
    if (source->close_stdout) { close(STDOUT_FILENO); source->close_stdout = 0; }
    return source->cancel || (source->cancel_after != 0 && source->bytes_read >= source->cancel_after);
}

static int check_pager_cache_growth(const char *cmdf, int compact)
{
    static const size_t budgets[] = { 65536, 65537, 65599, 98305, 131071, 131072, 131073 };
    static const size_t small_budgets[] = { 1024, 1025, 1087, 1537, 2047, 2048, 2049 };
    pager_large_source *state;
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char chunk[256];
    char marker[64];
    char budget_argument[32];
    size_t budget;
    size_t remaining;
    size_t count;
    size_t index;
    int fd;
    int master;
    int rc;
    int cli;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 10;
    memset(chunk, '\n', sizeof(chunk));
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (state == MAP_FAILED) return -1;
    fd = master = -1;
    pid = -1;
    rc = -1;
    budget = 0;
    path[0] = '\0';
    stage = "cache-growth fixture";
    for (index = 0; index < sizeof(budgets) / sizeof(*budgets) + (compact ? 0 : 1); index++) {
        cli = index == sizeof(budgets) / sizeof(*budgets);
        budget = cli ? 65537 : (compact ? small_budgets[index] : budgets[index]);
        memset(state, 0, sizeof(*state));
        state->expected_content_allocation = budget + 1;
        state->allocation_limit = budget + 1 + 64 * budget + 32768;
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-cache-growth-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        for (remaining = budget; remaining != 0; remaining -= count) {
            count = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            if (write_all(fd, chunk, count) != 0) goto done;
        }
        if (close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf *receiver;
            mdf_status status;

            if (cli) {
                snprintf(budget_argument, sizeof(budget_argument), "%lu", (unsigned long)budget);
                execl(cmdf, cmdf, "--pager", "--pager-format", "text", "--pager-cache", budget_argument, path, (char *)NULL);
                _exit(2);
            }
            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            opts.allocator.userdata = state;
            opts.allocator.alloc = pager_large_alloc;
            opts.allocator.realloc = NULL;
            opts.allocator.free = pager_large_free;
            cache.cache_budget = budget;
            if (compact) mdf_pager_test_allow_small_cache(1);
            if (index & 1) {
                receiver = NULL;
                if (mdf_create(MDF_FORMAT_ANSI, &opts, &receiver) != MDF_OK) _exit(2);
                status = receiver->pager_file_with_options(receiver, path, MDF_PAGER_FORMAT_TEXT, NULL, &cache);
                receiver->destroy(receiver);
            } else status = mdf_pager_file_with_options(path, &opts, MDF_PAGER_FORMAT_TEXT, NULL, &cache);
            if (status != MDF_OK || state->live != 0 || state->content_frees != 1) {
                fprintf(stderr, "cache-growth status=%d live=%lu content frees=%lu\n", (int)status,
                    (unsigned long)state->live, (unsigned long)state->content_frees);
                _exit(1);
            }
            _exit(0);
        }
        stage = "the entire content allowance is retained and EOF is known";
        if (wait_for_redraw(master, &out, " 0% ") != 0 || strstr(out.data, "--%") != NULL) goto done;
        if (!cli && (state->content_allocations != 1 || state->content_pointer == NULL)) goto done;
        clear_capture(&out);
        stage = "one-space search grows every dense index within the workspace ceiling";
        snprintf(marker, sizeof(marker), "  1/%lu", (unsigned long)budget);
        if (write_all(master, "/ \r", 3) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "n", 1) != 0) goto done;
        snprintf(marker, sizeof(marker), "  2/%lu", (unsigned long)budget);
        if (wait_for_redraw(master, &out, marker) != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "N", 1) != 0) goto done;
        snprintf(marker, sizeof(marker), "  1/%lu", (unsigned long)budget);
        if (wait_for_redraw(master, &out, marker) != 0) goto done;
        clear_capture(&out);
        stage = "replacing a dense query releases and regrows its match list";
        snprintf(marker, sizeof(marker), "  1/%lu", (unsigned long)(budget - 1));
        if (write_all(master, "q/  \r", 5) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        if (!cli && (state->content_allocations != 1 || state->content_pointer == NULL ||
                     state->peak > state->allocation_limit)) goto done;
        if (write_all(master, "qq", 2) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        pid = -1;
        if (!cli && (state->live != 0 || state->content_frees != 1)) goto done;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "cache-growth failed: %s (budget=%lu)\n", stage, (unsigned long)budget);
        if (master >= 0) (void)wait_for_output(master, &out, 40);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    munmap(state, sizeof(*state));
    free(out.data);
    return rc;
}

static int check_pager_large_matches(const char *cmdf, int compact)
{
    struct winsize size;
    capture out;
    pager_large_source *state;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char uri[501];
    char link[600];
    char query[400];
    char marker[40];
    int fd;
    int master;
    int mode;
    int i;
    int total;
    int labels;
    int wanted;
    int exited;
    int rc;
    size_t length;
    size_t offset;
    size_t take;
    size_t budget;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (state == MAP_FAILED) return -1;
    memset(uri, 'x', compact ? 200 : 500);
    uri[compact ? 200 : 500] = '\0';
    labels = compact ? 80 : 400;
    wanted = labels / 2;
    total = labels - wanted + 1;
    for (i = 0; i < wanted; i++) { query[i * 2] = 'a'; query[i * 2 + 1] = ' '; }
    length = (size_t)(wanted * 2 - 1);
    query[length] = '\0';
    snprintf(link, sizeof(link), "[a](https://example.com/%s) ", uri);
    path[0] = '\0';
    fd = master = -1;
    pid = -1;
    rc = -1;
    stage = "styled-match fixture";
    for (mode = 0; mode < 9; mode++) {
        if (compact && mode == 2) continue;
        /* The full-cache oracle needs identical visible labels, not megabytes
         * of hidden destinations on every live-search keystroke. Keep the
         * original long URLs in every bounded-cache regression. */
        snprintf(link, sizeof(link), "[a](https://example.com/%s) ", mode == 0 ? "x" : uri);
        total = mode == 8 ? 1 : labels - wanted + 1;
        memset(&size, 0, sizeof(size));
        size.ws_col = mode == 0 ? (compact ? 20 : 32) : 10;
        size.ws_row = 3;
        budget = mode == 0 ? (compact ? 32768 : 1048576) : (compact ? 8192 : 65536);
        memset(state, 0, sizeof(*state));
        state->expected_content_allocation = budget + 1;
        state->allocation_limit = budget * 65 + 32769;
        state->literal_record = link;
        state->literal_labels = mode == 8 ? (size_t)wanted : 0;
        state->record_width = strlen(link);
        state->length = (uint64_t)labels * state->record_width;
        state->chunk = 1024;
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-large-match-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        for (i = 0; i < labels; i++) if (write_all(fd, link, strlen(link)) != 0) goto done;
        if (close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;
            mdf_status expected;
            mdf_seekable_source source;
            mdf_pager_control control;

            if (mode == 2) {
                execl(cmdf, cmdf, "--pager", "--pager-format", "markdown", "--pager-cache", "65536",
                      "--osc8", "on", path, (char *)NULL);
                _exit(2);
            }
            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_ON;
            opts.osc8 = 1;
            opts.allocator.userdata = state;
            opts.allocator.alloc = pager_large_alloc;
            opts.allocator.realloc = NULL;
            opts.allocator.free = pager_large_free;
            cache.cache_budget = budget;
            if (compact) mdf_pager_test_allow_small_cache(1);
            if (mode >= 3) {
                source.userdata = state;
                source.read = pager_large_read;
                source.seek = pager_large_seek;
                control.userdata = state;
                control.should_cancel = pager_large_cancel;
                status = mdf_pager_seekable_source("styled.md", &source, &opts,
                                                   MDF_PAGER_FORMAT_MARKDOWN, &control, &cache);
            } else status = mdf_pager_file_with_options(path, &opts, MDF_PAGER_FORMAT_MARKDOWN, NULL, &cache);
            expected = mode == 4 || mode == 5 ? MDF_ERROR_IO :
                (mode == 6 ? MDF_CANCELLED : (mode == 7 ? MDF_ERROR_NOMEM : MDF_OK));
            if (mode >= 4 && mode < 8) printf("SEARCH_RESULT:%d\n", (int)status);
            fflush(stdout);
            if (status != expected || state->live != 0 || state->content_frees != 1) _exit(1);
            _exit(0);
        }
        stage = "styled-match startup";
        if (wait_for_redraw(master, &out, "\033]8;;https://example.com/") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "/", 1) != 0) goto done;
        for (offset = 0; offset < length; offset += take) {
            take = length - offset > 16 ? 16 : length - offset;
            if (write_all(master, query + offset, take) != 0 ||
                wait_for_output(master, &out, mode == 0 ? 50 : 20) != 0) goto done;
        }
        if (write_all(master, "\r", 1) != 0) goto done;
        stage = "confirm the query in the reference or bounded styled cache";
        if (mode == 0) snprintf(marker, sizeof(marker), "  1/%d", total);
        else snprintf(marker, sizeof(marker), "  0/");
        if (wait_for_redraw(master, &out, marker) != 0) goto done;
        if (mode >= 4 && mode < 8) {
            stage = "oversized-match scan failure and interruption cleanup";
            clear_capture(&out);
            if (mode == 4) state->fail_read = 1;
            if (mode == 5) state->fail_seek = 1;
            if (mode == 6) state->cancel_after = state->bytes_read + (compact ? 512 : 4096);
            if (mode == 7) state->fail_alloc = 1;
            snprintf(marker, sizeof(marker), "SEARCH_RESULT:%d", mode == 6 ? MDF_CANCELLED :
                (mode == 7 ? MDF_ERROR_NOMEM : MDF_ERROR_IO));
            if (write_all(master, mode == 5 ? "N" : "n", 1) != 0 ||
                wait_for_marker(master, &out, marker) != 0) goto done;
            goto close_child;
        }
        if (mode != 0) {
            clear_capture(&out);
            stage = "forward search advances beyond its retained styled window";
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "  1/") != 0 ||
                require_contains(&out, "\033[7ma") != 0) goto done;
        }
        if (mode != 0) {
            stage = "scrolling retains the visible part after the match start is evicted";
            for (i = 0; i < (compact ? 5 : 30); i++) {
                clear_capture(&out);
                if (write_all(master, "j", 1) != 0 || wait_for_redraw(master, &out, "  1/") != 0 ||
                    require_contains(&out, "\033[7ma") != 0) goto done;
            }
            clear_capture(&out);
            stage = "selection remains navigable after its start has been evicted";
            if (write_all(master, "n", 1) != 0 ||
                wait_for_redraw(master, &out, mode == 8 ? "  1/" : "  2/") != 0) goto done;
            clear_capture(&out);
            if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, "  1/") != 0) goto done;
            clear_capture(&out);
            stage = "resize preserves selection without retaining the complete styled match";
            size.ws_col = compact ? 20 : 32;
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                wait_for_redraw(master, &out, "  1/") != 0) goto done;
        }
        clear_capture(&out);
        stage = "End establishes the same total as a fully cached rendering";
        snprintf(marker, sizeof(marker), "/%d", total);
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, marker) != 0 ||
            require_contains(&out, "100%") != 0) goto done;
        clear_capture(&out);
        stage = "backward search locates an oversized match and highlights its visible portion";
        if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0 ||
            require_contains(&out, "\033[7ma") != 0) goto done;
        stage = "advance to the final overlapping match";
        snprintf(marker, sizeof(marker), "  %d/%d", total, total);
        for (i = 0; !capture_contains(&out, marker); i++) {
            if (i >= total) goto done;
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "/%d", total);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
            snprintf(marker, sizeof(marker), "  %d/%d", total, total);
        }
        clear_capture(&out);
        stage = "cyclic search wraps oversized matches";
        snprintf(marker, sizeof(marker), "  1/%d", total);
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        clear_capture(&out);
        stage = "backward cyclic search restores the last oversized match";
        snprintf(marker, sizeof(marker), "  %d/%d", total, total);
        if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        clear_capture(&out);
        stage = "hidden OSC8 destinations never produce matches";
        if (write_all(master, "q/example\r", 10) != 0 || wait_for_redraw(master, &out, "  0/") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "  0/0") != 0) goto done;
        if (mode != 2 && (state->content_allocations != 1 || state->content_pointer == NULL ||
                          state->peak > state->allocation_limit)) goto done;
        stage = "close search and drain the final styled redraw";
        clear_capture(&out);
        if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0 ||
            write_all(master, "q", 1) != 0) goto done;
close_child:
        exited = wait_for_exit(pid);
        pid = -1;
        if (exited != 0 || !terminal_is_cooked(master)) goto done;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "large-match pager failed: %s (mode=%d)\n", stage, mode);
        if (master >= 0) (void)wait_for_output(master, &out, 40);
        if (out.data != NULL) {
            size_t tail;
            tail = out.len > 8192 ? out.len - 8192 : 0;
            fwrite(out.data + tail, 1, out.len - tail, stderr);
        }
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    munmap(state, sizeof(*state));
    free(out.data);
    return rc;
}

static int check_pager_row_bytes(int compact)
{
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char accents[256];
    size_t budget;
    size_t pairs;
    size_t remaining;
    size_t count;
    int mode;
    int fd;
    int master;
    int exited;
    int rc;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 2;
    for (count = 0; count < sizeof(accents); count += 2) {
        accents[count] = '\314'; accents[count + 1] = '\201';
    }
    budget = compact ? 1024 : 65536;
    path[0] = '\0';
    fd = master = -1;
    pid = -1;
    rc = -1;
    stage = "row-byte fixture";
    for (mode = 0; mode < 9; mode++) {
        pairs = budget / 2 - (mode % 3 == 0 ? 1 : 0);
        if (mode % 3 == 2) pairs += compact ? 80 : 2232;
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-row-bytes-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        if (mode >= 6 && write_all(fd, "ok\n", 3) != 0) goto done;
        if (write_all(fd, "X", 1) != 0) goto done;
        for (remaining = pairs * 2; remaining != 0; remaining -= count) {
            count = remaining < sizeof(accents) ? remaining : sizeof(accents);
            if (write_all(fd, accents, count) != 0) goto done;
        }
        if (close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;
            mdf *receiver;

            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            cache.cache_budget = budget;
            if (compact) mdf_pager_test_allow_small_cache(1);
            if (mode & 1) {
                receiver = NULL;
                if (mdf_create(MDF_FORMAT_ANSI, &opts, &receiver) != MDF_OK) _exit(2);
                status = receiver->pager_file_with_options(receiver, path,
                    mode >= 3 && mode < 6 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL, &cache);
                receiver->destroy(receiver);
            } else status = mdf_pager_file_with_options(path, &opts,
                mode >= 3 && mode < 6 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL, &cache);
            printf("ROW_RESULT:%s\n", mdf_status_string(status));
            fflush(stdout);
            _exit(status == (mode % 3 == 0 ? MDF_OK : MDF_ERROR_TERMINAL_SIZE) ? 0 : 1);
        }
        if (mode >= 6) {
            stage = "later oversized rows are rejected on navigation";
            if (wait_for_redraw(master, &out, "ok") != 0 || write_all(master, "j", 1) != 0) goto done;
            clear_capture(&out);
        }
        if (mode == 4 || mode == 5) {
            stage = "an undecided Markdown word pauses at the input bound before its size is known";
            if (wait_for_redraw(master, &out, "--%") != 0 || write_all(master, "j", 1) != 0) goto done;
            clear_capture(&out);
        }
        if (mode % 3 == 0) {
            stage = "a nearly full row with zero-width characters remains visible";
            if (wait_for_redraw(master, &out, "X\314\201") != 0 || write_all(master, "q", 1) != 0) goto done;
        } else {
            stage = "a row larger than the byte cache returns the explicit size error";
            if (wait_for_marker(master, &out, "ROW_RESULT:terminal too large for pager content cache") != 0) goto done;
        }
        exited = wait_for_exit(pid);
        pid = -1;
        if (exited != 0 || !terminal_is_cooked(master)) goto done;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "row-byte pager failed: %s (mode=%d)\n", stage, mode);
        if (master >= 0) (void)wait_for_output(master, &out, 40);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

static int check_pager_content_limits(void)
{
    pager_large_source *state;
    struct winsize size;
    capture out;
    pid_t pid;
    int master;
    int mode;
    int rc;
    int fits;
    int resize;
    int entered;
    mdf_status expected;
    const char *stage;

    memset(&out, 0, sizeof(out));
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (state == MAP_FAILED) return -1;
    pid = -1;
    master = -1;
    rc = -1;
    stage = "viewport boundary";
    for (mode = 0; mode < 10; mode++) {
        memset(state, 0, sizeof(*state));
        memset(&size, 0, sizeof(size));
        entered = 0;
        size.ws_col = mode == 5 ? 1025 : (mode == 4 ? 1024 : 256);
        size.ws_row = mode == 4 || mode == 5 ? 1025 : (mode == 0 ? 256 : 257);
        if (mode == 2 || mode == 3) size.ws_row = 258;
        resize = mode == 6 || mode == 7;
        if (resize || mode == 9) { size.ws_col = 80; size.ws_row = 10; }
        fits = mode <= 1 || mode == 4;
        expected = mode == 9 ? MDF_ERROR_INVALID : (fits ? MDF_OK : MDF_ERROR_TERMINAL_SIZE);
        state->record_width = mode == 8 ? 257 : 256;
        state->length = mode == 4 || mode == 5 ? 0 : state->record_width * 256;
        state->markdown = mode == 3 || mode == 7;
        if (state->markdown) state->length += 8;
        state->chunk = 509;
        state->allocation_limit = 8u * 1024u * 1024u;
        state->expected_content_allocation = mode == 4 || mode == 5 ? 1048577 : 65537;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_seekable_source source;
            mdf_status status;
            mdf *receiver;

            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            opts.allocator.userdata = state;
            opts.allocator.alloc = pager_large_alloc;
            opts.allocator.realloc = NULL;
            opts.allocator.free = pager_large_free;
            cache.cache_budget = mode == 4 || mode == 5 ? 0 : (mode == 9 ? 1024 : 65536);
            source.userdata = state;
            source.read = pager_large_read;
            source.seek = pager_large_seek;
            receiver = NULL;
            if (mode == 1) {
                if (mdf_create(MDF_FORMAT_ANSI, &opts, &receiver) != MDF_OK) _exit(2);
                status = receiver->pager_seekable_source(receiver, "content-limit", &source,
                    MDF_PAGER_FORMAT_TEXT, NULL, &cache);
                receiver->destroy(receiver);
            } else status = mdf_pager_seekable_source("content-limit", &source, &opts,
                state->markdown ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL, &cache);
            if (status != expected || state->live != 0) {
                fprintf(stderr, "content limit mode=%d status=%d expected=%d live=%lu\n", mode,
                    (int)status, (int)expected, (unsigned long)state->live);
                _exit(1);
            }
            if (status != MDF_OK) printf("LIMIT_RESULT:%s\n", mdf_status_string(status));
            fflush(stdout);
            _exit(0);
        }
        if (fits || resize) {
            stage = "a screen within the content allowance renders";
            if (wait_for_redraw(master, &out, mode == 4 ? "100%" : "row00000000") != 0) goto done;
            entered = strstr(out.data, "\033[?1049h") != NULL;
            if (mode <= 1 && (require_contains(&out, mode == 0 ? "row00000254" : "row00000255") != 0 ||
                             strstr(out.data, "--%") != NULL)) goto done;
            if (state->content_allocations != 1) goto done;
            if (resize) {
                size_t previous_reads;

                stage = "oversized resize fails before consuming more source data";
                previous_reads = state->bytes_read;
                clear_capture(&out);
                size.ws_col = 256;
                size.ws_row = 258;
                if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                    wait_for_marker(master, &out, "LIMIT_RESULT:terminal too large for pager content cache") != 0 ||
                    state->bytes_read != previous_reads) goto done;
            } else if (write_all(master, "q", 1) != 0) goto done;
        } else {
            stage = "oversized screen returns a specific error and restores the terminal";
            if (wait_for_marker(master, &out, mode == 9 ? "LIMIT_RESULT:invalid argument" :
                "LIMIT_RESULT:terminal too large for pager content cache") != 0) goto done;
            if (mode != 8 && (state->bytes_read != 0 || state->content_allocations != 0)) goto done;
        }
        if (wait_for_exit(pid) != 0) goto done;
        pid = -1;
        if (!terminal_is_cooked(master) || state->live != 0) goto done;
        if (mode != 9) {
            if (wait_for_marker(master, &out, "\033[?1049l") != 0) goto done;
            if (!entered && strstr(out.data, "\033[?1049h") == NULL) goto done;
        } else if (strstr(out.data, "\033[?1049h") != NULL) {
            goto done;
        }
        close(master);
        master = -1;
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "content-limit pager failed: %s, mode=%d\n", stage, mode);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (master >= 0) close(master);
    munmap(state, sizeof(*state));
    free(out.data);
    return rc;
}

static int check_large_pager(const char *cmdf, int memory_fixture)
{
    pager_large_source *state;
    struct winsize size;
    capture out;
    pid_t pid;
    int master;
    int mode;
    int rc;
    const char *stage;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    int fd;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 10;
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (state == MAP_FAILED) return -1;
    pid = -1;
    master = -1;
    fd = -1;
    path[0] = '\0';
    rc = -1;
    for (mode = 0; mode < 24; mode++) {
        memset(state, 0, sizeof(*state));
        state->markdown = mode & 1;
        state->length = mode >= 4 ? ((uint64_t)1 << 40) : 65536 + (state->markdown ? 8 : 0);
        state->chunk = mode == 2 ? 1 : (mode == 3 ? 3 : 509);
        state->record_width = 64;
        state->expected_content_allocation = mode < 4 || mode >= 12 ? 1025 : (mode == 4 || memory_fixture ? 65537 : 1048577);
        state->extra_marker = mode == 4 || mode == 5 || mode == 8;
        state->marker_row = mode == 4 ? 900 : (memory_fixture ? 800 : 16000);
        state->allocation_limit = mode <= 4 ? 8u * 1024u * 1024u : 80u * 1024u * 1024u;
        size.ws_row = mode == 4 ? 24 : 10;
        stage = "bounded seekable source startup";
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_pager_control control;
            mdf_seekable_source source;
            mdf *receiver;
            mdf_status status;
            mdf_status expected_status;

            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            opts.allocator.userdata = state;
            if (mode == 3) opts.ansi_mode = MDF_ANSI_ON;
            opts.allocator.alloc = pager_large_alloc;
            opts.allocator.realloc = NULL;
            opts.allocator.free = pager_large_free;
            memset(&cache, 0, sizeof(cache));
            cache.cache_budget = mode < 4 || mode >= 12 ? 1024 : (mode == 4 || memory_fixture ? 65536 : 0);
            if (mode < 4 || mode >= 12) mdf_pager_test_allow_small_cache(1);
            control.userdata = state;
            control.should_cancel = pager_large_cancel;
            source.userdata = state;
            source.read = pager_large_read;
            source.seek = pager_large_seek;
            receiver = NULL;
            if (mode == 10 || mode == 11) {
                mdf_source forward;

                forward.userdata = state;
                forward.read = pager_large_read;
                if (mode == 11) {
                    if (mdf_create(MDF_FORMAT_ANSI, &opts, &receiver) != MDF_OK) _exit(2);
                    status = receiver->pager_source(receiver, "large", &forward, MDF_PAGER_FORMAT_AUTO, &control);
                    receiver->destroy(receiver);
                } else status = mdf_pager_source("large", &forward, &opts, MDF_PAGER_FORMAT_AUTO, &control);
            } else if (mode & 2) {
                if (mdf_create(MDF_FORMAT_ANSI, &opts, &receiver) != MDF_OK) _exit(2);
                status = receiver->pager_seekable_source(receiver, "large", &source,
                    state->markdown ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, &control, &cache);
                receiver->destroy(receiver);
            } else status = mdf_pager_seekable_source("large", &source, &opts,
                state->markdown ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, &control, &cache);
            expected_status = MDF_OK;
            if (mode == 6 || mode == 7 || (mode >= 12 && mode < 16) || mode >= 22) expected_status = MDF_ERROR_IO;
            else if (mode == 8 || mode == 9 || mode == 16 || mode == 17) expected_status = MDF_CANCELLED;
            else if (mode >= 10) expected_status = MDF_ERROR_NOMEM;
            if (status != expected_status || state->live != 0) {
                fprintf(stderr, "large mode=%d status=%d live=%lu peak=%lu reads=%lu seeks=%lu\n", mode,
                    (int)status, (unsigned long)state->live, (unsigned long)state->peak,
                    (unsigned long)state->bytes_read, (unsigned long)state->seeks);
                _exit(1);
            }
            _exit(0);
        }
        if (mode == 10 || mode == 11) {
            stage = "forward-only compatibility input is bounded";
            if (wait_for_exit(pid) != 0 || state->live != 0 ||
                state->bytes_read > 1024u * 1024u + 4096 || !terminal_is_cooked(master)) goto done;
            pid = -1;
            close(master);
            master = -1;
            continue;
        }
        if (wait_for_redraw(master, &out, "row00000000") != 0 ||
            require_contains(&out, "--%") != 0 ||
            state->bytes_read > (mode <= 4 || memory_fixture ? 65537u : 1048577u) || state->seeks != 1) goto done;
        if (mode < 4 && state->bytes_read > 2048) goto done;
        if (state->content_allocations != 1) goto done;
        clear_capture(&out);
        if (state->extra_marker) {
            size_t previous_reads;
            size_t previous_seeks;
            char marker[32];

            stage = "the complete configured content allowance is retained";
            previous_reads = state->bytes_read;
            previous_seeks = state->seeks;
            snprintf(marker, sizeof(marker), "row%08lu", state->marker_row);
            if (write_all(master, "/XYZ\r", 5) != 0 || wait_for_redraw(master, &out, marker) != 0 ||
                require_contains(&out, "\033[7mXYZ\033[27m") != 0 || state->bytes_read != previous_reads ||
                state->seeks != previous_seeks) goto done;
            clear_capture(&out);
            if (write_all(master, "qg", 2) != 0 || wait_for_redraw(master, &out, "row00000000") != 0 ||
                state->bytes_read != previous_reads || state->seeks != previous_seeks) goto done;
            clear_capture(&out);
        }
        if (mode < 4) {
            size_t forward_seeks;
            int hit;
            char marker[40];

            stage = "search ahead continues live parser";
            if (write_all(master, "/needle\rn", 9) != 0 ||
                wait_for_redraw(master, &out, "row00000032") != 0 || state->seeks > (mode == 3 ? 2u : 1u) ||
                state->bytes_read >= state->length || require_contains(&out, "\033[7mneedle\033[27m") != 0) goto done;
            /* Styled eager loading may overrun one initial decision before
             * a query exists. Recover that evicted context once, then require
             * the same live parser across subsequent cache boundaries. */
            forward_seeks = state->seeks;
            clear_capture(&out);
            stage = "subsequent forward search keeps parser and refreshes cache matches";
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "row00000064") != 0 ||
                require_contains(&out, "\033[7mneedle\033[27m") != 0 || state->seeks != forward_seeks) goto done;
            if (mode == 3) {
                stage = "styled forward searches do not repeat the initial overflow replay";
                for (hit = 96; hit <= 256; hit += 32) {
                    clear_capture(&out);
                    snprintf(marker, sizeof(marker), "row%08d", hit);
                    if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0 ||
                        state->seeks != forward_seeks) goto done;
                }
                for (hit = 224; hit >= 64; hit -= 32) {
                    clear_capture(&out);
                    snprintf(marker, sizeof(marker), "row%08d", hit);
                    if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
                }
            }
            clear_capture(&out);
            stage = "resize preserves source passage after eviction";
            size.ws_col = 50;
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                wait_for_redraw(master, &out, "row00000064") != 0) goto done;
            clear_capture(&out);
            size.ws_col = 80;
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                wait_for_redraw(master, &out, "row00000064") != 0) goto done;
            clear_capture(&out);
            if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "row00000064") != 0) goto done;
            clear_capture(&out);
            if (mode == 0) {
                int hit;
                char marker[40];

                stage = "dense search selection survives cache eviction without skipping hits";
                if (write_all(master, "g", 1) != 0 || wait_for_redraw(master, &out, "\033[Hrow00000000") != 0)
                    goto done;
                clear_capture(&out);
                if (write_all(master, "/row\r", 5) != 0 ||
                    wait_for_redraw(master, &out, "\033[H\033[7mrow\033[27m00000000") != 0) goto done;
                clear_capture(&out);
                for (hit = 1; hit <= 40; hit++) {
                    snprintf(marker, sizeof(marker), "\033[H\033[7mrow\033[27m%08d", hit);
                    if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
                    snprintf(marker, sizeof(marker), "  %d/", hit + 1);
                    stage = "document-wide ordinal survives cache eviction";
                    if (require_contains(&out, marker) != 0) goto done;
                    clear_capture(&out);
                }
                for (hit = 39; hit >= 0; hit--) {
                    snprintf(marker, sizeof(marker), "\033[H\033[7mrow\033[27m%08d", hit);
                    if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
                    clear_capture(&out);
                }
                if (write_all(master, "q/needle\r", 9) != 0 ||
                    wait_for_redraw(master, &out, "\033[Hrow00000000 \033[7mneedle\033[27m") != 0) goto done;
                clear_capture(&out);
            }
            stage = "end keeps bounded tail cache and discovers EOF";
            {
                size_t previous_reads;

                /* Fragmented styled search and resize have already run.
                 * Keep fragmentation through EOF in the native test; batch
                 * this tail under Valgrind instead of timing tiny callbacks. */
                if (memory_fixture && mode == 3) state->chunk = 509;
                previous_reads = state->reads;
                if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "row00001023") != 0 ||
                    require_contains(&out, "100%") != 0 ||
                    (mode < 2 && state->reads - previous_reads > 512)) goto done;
            }
            if (mode == 0) {
                if (require_contains(&out, "/needle  0/32") != 0) goto done;
                clear_capture(&out);
                if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "row00001023") != 0)
                    goto done;
            }
            clear_capture(&out);
            stage = "backward cache miss uses bounded text checkpoints";
            {
                size_t previous_reads;

                previous_reads = state->bytes_read;
                if (write_all(master, "b", 1) != 0 || wait_for_redraw(master, &out, "row00001006") != 0 ||
                    (!state->markdown && state->bytes_read - previous_reads > 16384)) goto done;
            }
            clear_capture(&out);
            stage = "home recovers evicted content and preserves known percentage";
            if (write_all(master, "g", 1) != 0 || wait_for_redraw(master, &out, "row00000000") != 0 ||
                require_contains(&out, "0%") != 0 || state->seeks < 2) goto done;
            clear_capture(&out);
        }
        if (mode >= 12) {
            stage = "search counting failure and cancellation cleanup";
            if (write_all(master, "/missing\r", mode == 18 || mode == 19 ? 8 : 9) != 0 ||
                wait_for_redraw(master, &out, mode == 18 || mode == 19 ? "  /missing" : "  0/?") != 0) goto done;
            clear_capture(&out);
            if (mode >= 22) {
                state->close_stdout = 1;
                if (write_all(master, "j", 1) != 0) goto done;
            } else if (mode >= 20) {
                state->fail_alloc = 1;
                if (write_all(master, "q", 1) != 0) goto done;
            } else if (mode >= 18) {
                state->fail_alloc = 1;
                if (write_all(master, "\177", 1) != 0) goto done;
            } else if (mode >= 16) {
                state->cancel_after = state->bytes_read + 4096;
                if (write_all(master, "n", 1) != 0) goto done;
            } else if (mode >= 14) {
                if (write_all(master, "                    ", 20) != 0 ||
                    wait_for_redraw(master, &out, "row00000180") != 0) goto done;
                state->fail_seek = 1;
                if (write_all(master, "N", 1) != 0) goto done;
            } else {
                state->fail_read = 1;
                if (write_all(master, "n", 1) != 0) goto done;
            }
        }
        else if (mode == 8) state->cancel = 1;
        else if (mode == 9) {
            state->cancel_after = state->bytes_read + 4096;
            if (write_all(master, "G", 1) != 0) goto done;
        }
        else if (mode == 6) {
            state->fail_seek = 1;
            size.ws_col = 79;
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0) goto done;
            size.ws_col = 80;
        }
        else if (mode >= 6) {
            state->fail_read = 1;
            if (write_all(master, "G", 1) != 0) goto done;
        } else if (write_all(master, "q", 1) != 0) goto done;
        stage = "large source exit cleanup";
        {
            int exited;

            exited = wait_for_exit(pid);
            pid = -1;
            if (exited != 0) { (void)wait_for_output(master, &out, 40); goto done; }
        }
        if ((mode < 22 && wait_for_marker(master, &out, "\033[?1049l") != 0) || !terminal_is_cooked(master) ||
            state->live != 0 || state->peak > state->allocation_limit) goto done;
        close(master);
        master = -1;
        clear_capture(&out);
    }
    stage = "seekable text preserves fragmented UTF-8, CRLF and control bytes";
    for (mode = 1; mode <= 7; mode++) {
        static const char fragmented[] = "first \346\227\245\360\237\230\200\r\nliteral\rreturn \200 end\nlast \351\233\250";

        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            pager_source input;
            mdf_seekable_source source;

            memset(&input, 0, sizeof(input));
            input.data = fragmented;
            input.len = sizeof(fragmented) - 1;
            input.chunk = (size_t)mode;
            source.userdata = &input;
            source.read = pager_source_read;
            source.seek = pager_source_seek;
            _exit(mdf_pager_seekable_source("fragmented.txt", &source, NULL, MDF_PAGER_FORMAT_TEXT,
                                             NULL, NULL) == MDF_OK ? 0 : 1);
        }
        if (wait_for_redraw(master, &out, "last \351\233\250") != 0 ||
            require_contains(&out, "first \346\227\245\360\237\230\200") != 0 ||
            require_contains(&out, "literal^Mreturn M-^@ end") != 0 ||
            strstr(out.data, "\360\237\230\200^M") != NULL ||
            write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        pid = -1;
        close(master);
        master = -1;
        clear_capture(&out);
    }
    stage = "sparse 1 TiB named file";
    if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-terabyte-XXXXXX") >= (int)sizeof(path) ||
        (fd = mkstemp(path)) < 0 || write_all(fd, "```\n", 4) != 0) goto done;
    for (mode = 0; mode < 80; mode++) {
        if (write_all(fd, "first sparse page\n", strlen("first sparse page\n")) != 0) goto done;
    }
    if (ftruncate(fd, (off_t)((uint64_t)1 << 40)) != 0 || close(fd) != 0) goto done;
    fd = -1;
    for (mode = 0; mode < 4; mode++) {
        pid = start_selected_pager(cmdf, path, mode & 1 ? "markdown" : "text", mode & 2, &master);
        if (pid < 0 || wait_for_redraw(master, &out, "first sparse page") != 0 ||
            require_contains(&out, "--%") != 0 || write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0)
            goto done;
        pid = -1;
        if (!terminal_is_cooked(master)) goto done;
        close(master);
        master = -1;
        clear_capture(&out);
    }
    stage = "CLI reports oversized terminal explicitly";
    size.ws_col = 256;
    size.ws_row = 258;
    pid = forkpty(&master, NULL, NULL, &size);
    if (pid < 0) goto done;
    if (pid == 0) {
        execl(cmdf, cmdf, "--pager", "--pager-cache", "65536", "--pager-format", "text", path, (char *)NULL);
        _exit(127);
    }
    if (wait_for_marker(master, &out, "terminal too large for pager content cache") != 0 ||
        wait_for_failure(pid) != 0 || !terminal_is_cooked(master)) goto done;
    pid = -1;
    close(master);
    master = -1;
    clear_capture(&out);
    stage = "rendered content capacity and terminal size boundaries";
    if (check_pager_content_limits() != 0) goto done;
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "large pager failed: %s (mode %d, reads=%lu seeks=%lu peak=%lu)\n", stage, mode,
            (unsigned long)state->bytes_read, (unsigned long)state->seeks, (unsigned long)state->peak);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (master >= 0) close(master);
    if (fd >= 0) close(fd);
    if (path[0] != '\0') unlink(path);
    munmap(state, sizeof(*state));
    free(out.data);
    return rc;
}

static int check_generated_search_wrap(int small_cache)
{
    static const char *labels[] = { "FIRST", "SECOND", "THIRD", "FOURTH" };
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char record[64];
    char prefix[48];
    char marker[48];
    size_t positions[4];
    size_t records;
    size_t i;
    pid_t pid;
    int master;
    int fd;
    int mode;
    int placement;
    int total;
    int hit;
    int cycle;
    int rc;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 10;
    path[0] = '\0';
    pid = -1;
    master = -1;
    fd = -1;
    rc = -1;
    stage = "generated fixture";
    for (mode = 0; mode < 10; mode++) {
        placement = mode % 5;
        records = small_cache ? 64 : 2400;
        total = placement == 4 ? 0 : (placement == 3 ? 1 : 4);
        for (hit = 0; hit < 4; hit++) {
            if (placement == 1) positions[hit] = (small_cache ? 50 : 2280) + (size_t)hit * (small_cache ? 3 : 30);
            else if (placement == 2) positions[hit] = (size_t)hit * (records / 4) + 1;
            else positions[hit] = (small_cache ? 1 : 10) + (size_t)hit * (small_cache ? 3 : 30);
        }
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-search-wrap-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        for (i = 0; i < records; i++) {
            size_t length;
            size_t j;

            snprintf(prefix, sizeof(prefix), "passage%08lu ordinary narrative ", (unsigned long)i);
            for (hit = 0; hit < total; hit++) {
                if (i == positions[hit]) snprintf(prefix, sizeof(prefix), "%s future of jobs ", labels[hit]);
            }
            length = strlen(prefix);
            memcpy(record, prefix, length);
            for (j = length; j < 62; j++) record[j] = " filler text"[(j - length) % 12];
            record[62] = '\n';
            record[63] = '\n';
            if (write_all(fd, record, sizeof(record)) != 0) goto done;
        }
        if (close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_pager_format format;
            mdf_status status;
            mdf *receiver;

            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            cache.cache_budget = small_cache ? 1024 : 65536;
            if (small_cache) mdf_pager_test_allow_small_cache(1);
            format = mode < 5 ? MDF_PAGER_FORMAT_TEXT : MDF_PAGER_FORMAT_MARKDOWN;
            if (placement & 1) {
                receiver = NULL;
                if (mdf_create(MDF_FORMAT_ANSI, &opts, &receiver) != MDF_OK) _exit(2);
                status = receiver->pager_file_with_options(receiver, path, format, NULL, &cache);
                receiver->destroy(receiver);
            } else status = mdf_pager_file_with_options(path, &opts, format, NULL, &cache);
            _exit(status == MDF_OK ? 0 : 1);
        }
        stage = "generated startup";
        if (wait_for_redraw(master, &out, "passage00000000") != 0) goto done;
        clear_capture(&out);
        stage = "confirm generated query";
        snprintf(marker, sizeof(marker), "/future of jobs  %d/", total == 0 || placement == 1 ? 0 : 1);
        if (write_all(master, "/future of jobs\r", 16) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        if (total == 0) {
            for (cycle = 0; cycle < 2; cycle++) {
                clear_capture(&out);
                stage = "absent query makes a bounded round trip and restores the original viewport";
                if (write_all(master, cycle == 0 ? "n" : "N", 1) != 0 ||
                    wait_for_redraw(master, &out, "0/0") != 0 ||
                    require_contains(&out, "\033[Hpassage00000000") != 0) goto done;
            }
        } else {
            if (strstr(out.data, "0/?") != NULL) {
                clear_capture(&out);
                if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "\033[HFIRST") != 0) goto done;
            }
            for (cycle = 0; cycle < 2; cycle++) {
                for (hit = 1; hit < total; hit++) {
                    clear_capture(&out);
                    stage = "next generated hit across slices";
                    snprintf(marker, sizeof(marker), "\033[H%s", labels[hit]);
                    if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
                    snprintf(marker, sizeof(marker), "  %d/", hit + 1);
                    stage = "document-wide ordinal survives cache eviction";
                    if (require_contains(&out, marker) != 0) goto done;
                }
                clear_capture(&out);
                stage = "forward wrap across empty slices";
                if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "\033[HFIRST") != 0) goto done;
                snprintf(marker, sizeof(marker), "  1/%d", total);
                stage = "a complete query traversal establishes a decisive total";
                if (require_contains(&out, marker) != 0 || strstr(out.data, " cached") != NULL) goto done;
                clear_capture(&out);
                stage = "backward wrap across empty slices";
                snprintf(marker, sizeof(marker), "\033[H%s", labels[total - 1]);
                if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
                snprintf(marker, sizeof(marker), "  %d/%d", total, total);
                if (require_contains(&out, marker) != 0) goto done;
                for (hit = total - 2; hit >= 0; hit--) {
                    clear_capture(&out);
                    stage = "previous generated hit across slices";
                    snprintf(marker, sizeof(marker), "\033[H%s", labels[hit]);
                    if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
                    snprintf(marker, sizeof(marker), "  %d/%d", hit + 1, total);
                    if (require_contains(&out, marker) != 0) goto done;
                }
            }
        }
        if (write_all(master, "qq", 2) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        pid = -1;
        close(master);
        master = -1;
        clear_capture(&out);
        unlink(path);
        path[0] = '\0';
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "generated search wrap failed: %s, mode=%d\n", stage, mode);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (master >= 0) close(master);
    if (fd >= 0) close(fd);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

typedef struct numbering_source {
    FILE *file;
    size_t chunk;
    pager_large_source *probe;
} numbering_source;

static size_t numbering_read(void *userdata, char *dst, size_t cap, int *err)
{
    numbering_source *source;
    size_t bytes;

    source = (numbering_source *)userdata;
    if (source->probe != NULL && source->probe->fail_read) { *err = EIO; return 0; }
    if (cap > source->chunk) cap = source->chunk;
    bytes = fread(dst, 1, cap, source->file);
    *err = ferror(source->file) ? EIO : 0;
    if (source->probe != NULL) {
        source->probe->reads++;
        source->probe->bytes_read += bytes;
    }
    return bytes;
}

static int numbering_seek(void *userdata, const mdf_source_offset *offset)
{
    numbering_source *source;

    source = (numbering_source *)userdata;
    if (source->probe != NULL) {
        if (source->probe->fail_seek) return -1;
        source->probe->seeks++;
    }
    return fseeko(source->file, (off_t)(((uint64_t)offset->high << 32) | offset->low), SEEK_SET);
}

/* Exercise UI transitions, rather than merely waiting for any successful
 * redraw. Every selected passage has an independent, explicit ordinal. */
static int check_resize_cache_expansion(int compact)
{
    capture out;
    struct winsize size;
    pager_large_source *probe;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char record[80];
    char passage[24];
    char next_passage[24];
    size_t budget;
    int count;
    int target;
    int mode;
    int step;
    int i;
    int fd;
    int master;
    int rc;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    probe = mmap(NULL, sizeof(*probe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED) return -1;
    path[0] = '\0';
    fd = master = -1;
    pid = -1;
    rc = -1;
    mode = 0;
    budget = compact ? 4096 : 65536;
    stage = "cache-expanding resize fixture";
    for (mode = 0; mode < 4; mode++) {
        memset(probe, 0, sizeof(*probe));
        probe->expected_content_allocation = budget + 1;
        probe->allocation_limit = budget * 65 + 32768;
        count = mode & 2 ? (compact ? 50 : 790) : (compact ? 51 : 819);
        size.ws_col = 80;
        size.ws_row = compact ? 3 : 10;
        target = count - (size.ws_row - 1);
        snprintf(passage, sizeof(passage), "\033[HROW%05d", target);
        snprintf(next_passage, sizeof(next_passage), "ROW%05d", target + 1);
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-resize-expansion-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        for (i = 0; i < count; i++) {
            memset(record, 'x', sizeof(record));
            snprintf(record, sizeof(record), "ROW%05d %s", i,
                i == target || i == target + 1 ? "TARGET " : "ordinary ");
            record[strlen(record)] = 'x';
            record[79] = '\n';
            if (write_all(fd, record, sizeof(record)) != 0 ||
                ((mode & 2) && write_all(fd, "\n", 1) != 0)) goto done;
        }
        if (close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            numbering_source input;
            mdf_seekable_source source;
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;

            memset(&input, 0, sizeof(input));
            input.file = fopen(path, "rb");
            if (input.file == NULL) _exit(2);
            input.chunk = mode & 2 ? 3 : 509;
            input.probe = probe;
            source.userdata = &input;
            source.read = numbering_read;
            source.seek = numbering_seek;
            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            opts.allocator.userdata = probe;
            opts.allocator.alloc = pager_large_alloc;
            opts.allocator.realloc = NULL;
            opts.allocator.free = pager_large_free;
            cache.cache_budget = budget;
            if (compact) mdf_pager_test_allow_small_cache(1);
            status = mdf_pager_seekable_source(path, &source, &opts,
                mode & 2 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL, &cache);
            fclose(input.file);
            _exit(status == MDF_OK && probe->live == 0 ? 0 : 1);
        }
        stage = "the original width caches the entire document";
        if (wait_for_redraw(master, &out, "0%") != 0 || require_contains(&out, "--%") == 0 ||
            probe->bytes_read != (size_t)count * (mode & 2 ? 81u : 80u) ||
            probe->content_allocations != 1 || probe->seeks != 1) goto done;
        probe->fail_read = probe->fail_seek = 1;
        clear_capture(&out);
        if (write_all(master, "/TARGET\r", 8) != 0 || wait_for_redraw(master, &out, "  1/2") != 0 ||
            require_contains(&out, passage) != 0) goto done;
        if (!(mode & 1)) {
            clear_capture(&out);
            if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, passage) != 0) goto done;
        }
        probe->fail_read = probe->fail_seek = 0;
        for (step = 0; step < 4; step++) {
            stage = "narrowing beyond cache capacity preserves the viewed passage and selection";
            size.ws_col = step & 1 ? 80 : (compact ? 12 : 20);
            clear_capture(&out);
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                wait_for_redraw(master, &out, mode & 1 ? "  1/" : passage) != 0 ||
                require_contains(&out, passage) != 0 || probe->content_allocations != 1 ||
                probe->peak > probe->allocation_limit) goto done;
            if (mode & 1) {
                stage = "search navigation retains the recovered selection";
                if (require_contains(&out, compact && !(mode & 2) && !(step & 1) ?
                    "\033[7mTAR" : "\033[7mTARGET\033[27m") != 0) goto done;
                clear_capture(&out);
                if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "  2/") != 0) goto done;
                if (compact && (mode & 2) && !(step & 1)) {
                    /* At twelve columns Markdown moves TARGET to its own
                     * row. Look up one row to verify its paragraph label. */
                    clear_capture(&out);
                    if (write_all(master, "k", 1) != 0 || wait_for_redraw(master, &out, "  2/") != 0) goto done;
                }
                if (require_contains(&out, next_passage) != 0) goto done;
                clear_capture(&out);
                if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, "  1/") != 0) goto done;
                if (compact && (mode & 2) && !(step & 1)) {
                    clear_capture(&out);
                    if (write_all(master, "k", 1) != 0 || wait_for_redraw(master, &out, "  1/") != 0) goto done;
                }
                if (require_contains(&out, passage) != 0) goto done;
            } else if (!(step & 1)) {
                clear_capture(&out);
                if (write_all(master, "j", 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0) goto done;
                clear_capture(&out);
                if (write_all(master, "k", 1) != 0 || wait_for_redraw(master, &out, passage) != 0) goto done;
            }
        }
        if (write_all(master, mode & 1 ? "qq" : "q", mode & 1 ? 2 : 1) != 0 || wait_for_exit(pid) != 0 ||
            !terminal_is_cooked(master) || probe->live != 0) goto done;
        pid = -1;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "cache expansion resize failed: %s (mode=%d)\n", stage, mode);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    munmap(probe, sizeof(*probe));
    free(out.data);
    return rc;
}

static int check_repeated_resize(int compact)
{
    static const char paragraph[] = "repeated ordinary paragraph text\n\n";
    static const int widths[] = { 60, 80, 25, 80 };
    capture out;
    struct winsize size;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char marker[40];
    int mode;
    int i;
    int step;
    int target;
    int fd;
    int master;
    int rc;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    path[0] = '\0';
    fd = master = -1;
    pid = -1;
    rc = -1;
    mode = 0;
    stage = "repeated passage fixture";
    for (mode = 0; mode < 4; mode++) {
        target = compact ? (mode == 3 ? 51 : 21) : 101;
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-repeated-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        for (i = 0; i < (compact ? 500 : 5000); i++)
            if (write_all(fd, paragraph, sizeof(paragraph) - 1) != 0) goto done;
        if (close(fd) != 0) goto done;
        fd = -1;
        size.ws_col = 80;
        size.ws_row = 10;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            numbering_source input;
            mdf_seekable_source source;
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;

            memset(&input, 0, sizeof(input));
            input.file = fopen(path, "rb");
            if (input.file == NULL) _exit(2);
            input.chunk = mode == 2 ? 3 : 509;
            source.userdata = &input;
            source.read = numbering_read;
            source.seek = numbering_seek;
            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            cache.cache_budget = mode == 3 ? 1024 : (compact ? 4096 : 65536);
            if (compact || mode == 3) mdf_pager_test_allow_small_cache(1);
            status = mdf_pager_seekable_source(path, &source, &opts,
                mode == 0 ? MDF_PAGER_FORMAT_TEXT : MDF_PAGER_FORMAT_MARKDOWN, NULL, &cache);
            fclose(input.file);
            _exit(status == MDF_OK ? 0 : 1);
        }
        stage = "select a cached repeated occurrence";
        if (wait_for_redraw(master, &out, "repeated") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "/repeated\r", 10) != 0 || wait_for_redraw(master, &out, "  1/") != 0) goto done;
        for (i = 2; i <= target; i++) {
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", i);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        }
        for (step = 0; step < 4; step++) {
            stage = "duplicate context retains its source occurrence after resize";
            size.ws_col = (unsigned short)widths[step];
            snprintf(marker, sizeof(marker), "  %d/", target);
            clear_capture(&out);
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                wait_for_redraw(master, &out, marker) != 0 ||
                require_contains(&out, "\033[H\033[7mrepeated\033[27m") != 0) goto done;
            stage = "duplicate navigation retains the new selection";
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", target + 1);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", target);
            if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        }
        if (write_all(master, "qq", 2) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        pid = -1;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "repeated resize failed: %s (mode=%d)\n", stage, mode);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

static int check_sparse_read_ahead(int compact)
{
    capture out;
    struct winsize size;
    pager_large_source *probe;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char gap[128];
    size_t budget;
    size_t previous;
    size_t remaining;
    size_t n;
    int fd;
    int master;
    int mode;
    int i;
    int rc;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 10;
    budget = compact ? 1024 : 65536;
    probe = mmap(NULL, sizeof(*probe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED) return -1;
    path[0] = '\0';
    fd = master = -1;
    pid = -1;
    rc = -1;
    mode = 0;
    stage = "sparse input fixture";
    for (mode = 0; mode < 4; mode++) {
        memset(probe, 0, sizeof(*probe));
        probe->expected_content_allocation = budget + 1;
        probe->allocation_limit = budget * 65 + 32768;
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-read-ahead-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        if ((mode & 1) && write_all(fd, "LEAD\n\nSTART\n\n", 13) != 0) goto done;
        memset(gap, '\n', sizeof(gap));
        if (mode & 2) for (i = 0; i < (int)sizeof(gap); i += 2) gap[i] = ' ';
        for (remaining = budget * 16; remaining != 0; remaining -= n) {
            n = remaining < sizeof(gap) ? remaining : sizeof(gap);
            if (write_all(fd, gap, n) != 0) goto done;
        }
        if (write_all(fd, "VISIBLE paragraph\n\n", 19) != 0 || close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            numbering_source input;
            mdf_seekable_source source;
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;

            memset(&input, 0, sizeof(input));
            input.file = fopen(path, "rb");
            if (input.file == NULL) _exit(2);
            input.chunk = mode & 2 ? 3 : 509;
            input.probe = probe;
            source.userdata = &input;
            source.read = numbering_read;
            source.seek = numbering_seek;
            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            opts.allocator.userdata = probe;
            opts.allocator.alloc = pager_large_alloc;
            opts.allocator.realloc = NULL;
            opts.allocator.free = pager_large_free;
            cache.cache_budget = budget;
            if (compact) mdf_pager_test_allow_small_cache(1);
            status = mdf_pager_seekable_source(path, &source, &opts, MDF_PAGER_FORMAT_MARKDOWN, NULL, &cache);
            fclose(input.file);
            _exit(status == MDF_OK && probe->live == 0 ? 0 : 1);
        }
        stage = "initial loading stops at the input allowance even with a short or empty viewport";
        if (wait_for_redraw(master, &out, "\033[H") != 0 || require_contains(&out, "--%") != 0 ||
            probe->bytes_read > budget + 1 || probe->seeks != 1 || probe->content_allocations != 1) goto done;
        previous = probe->bytes_read;
        stage = "status and query edits do not silently resume input";
        if (mode & 1) {
            clear_capture(&out);
            if (write_all(master, "/START\r", 7) != 0 || wait_for_redraw(master, &out, "/START  1/") != 0 ||
                probe->bytes_read != previous) goto done;
            clear_capture(&out);
            if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0) goto done;
        }
        clear_capture(&out);
        if (write_all(master, "/VISIBLE\r", 9) != 0 || wait_for_redraw(master, &out, "/VISIBLE  0/") != 0 ||
            probe->bytes_read != previous) goto done;
        for (i = 0; i < 3; i++) {
            stage = "scrolling continues the same parser with another bounded input allowance";
            clear_capture(&out);
            if (write_all(master, "j", 1) != 0 || wait_for_redraw(master, &out, "--%") != 0 ||
                probe->bytes_read <= previous || probe->bytes_read - previous > budget + 1 || probe->seeks != 1)
                goto done;
            previous = probe->bytes_read;
        }
        stage = "explicit search scans ahead and finds the deferred paragraph";
        clear_capture(&out);
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "\033[7mVISIBLE\033[27m") != 0)
            goto done;
        clear_capture(&out);
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "100%") != 0 ||
            require_contains(&out, "VISIBLE") != 0 || write_all(master, "qq", 2) != 0 ||
            wait_for_exit(pid) != 0 || !terminal_is_cooked(master) || probe->live != 0) goto done;
        pid = -1;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "sparse read ahead failed: %s (mode=%d, read=%lu)\n", stage, mode,
            (unsigned long)probe->bytes_read);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    munmap(probe, sizeof(*probe));
    free(out.data);
    return rc;
}

static int check_resize_match_ranks(int compact)
{
    static const char table[] = "| A | B | C |\n| --- | --- | --- |\n"
        "| longneedle | alpha beta gamma delta | alpha beta gamma delta |\n\n";
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char line[96];
    char marker[32];
    const char *passage;
    const char *stage;
    int fd;
    int master;
    int mode;
    int i;
    int step;
    int second_hits;
    int ordinal;
    int rc;
    pid_t pid;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    path[0] = '\0';
    fd = master = -1;
    pid = -1;
    rc = -1;
    mode = 0;
    stage = "rank-changing resize fixture";
    for (mode = 0; mode < 8; mode++) {
        second_hits = mode & 2 ? 2 : 1;
        passage = mode & 4 ? "THIRD" : "SECOND";
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-resize-ranks-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        if (mode & 1) for (i = 0; i < (compact ? 160 : 3000); i++) {
            snprintf(line, sizeof(line), "prefix%04d ordinary filler words\n\n", i);
            if (write_all(fd, line, strlen(line)) != 0) goto done;
        }
        if (write_all(fd, table, sizeof(table) - 1) != 0) goto done;
        snprintf(line, sizeof(line), "SECOND longneedle%s\n\nTHIRD longneedle\n\n",
            second_hits == 2 ? " longneedle" : "");
        if (write_all(fd, line, strlen(line)) != 0) goto done;
        for (i = 0; i < 30; i++) {
            snprintf(line, sizeof(line), "after%02d ordinary filler\n\n", i);
            if (write_all(fd, line, strlen(line)) != 0) goto done;
        }
        if (close(fd) != 0) goto done;
        fd = -1;
        size.ws_col = 80;
        size.ws_row = 10;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;

            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            cache.cache_budget = compact ? 4096 : 65536;
            if (compact) mdf_pager_test_allow_small_cache(1);
            status = mdf_pager_file_with_options(path, &opts, MDF_PAGER_FORMAT_MARKDOWN, NULL, &cache);
            _exit(status == MDF_OK ? 0 : 1);
        }
        if (wait_for_redraw(master, &out, "\033[H") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "100%") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "/longneedle\r", 12) != 0 || wait_for_redraw(master, &out, "/longneedle  ") != 0)
            goto done;
        stage = "select a passage after an earlier table-cell match";
        for (i = 0; i < second_hits + (mode & 4 ? 1 : 0); i++) {
            clear_capture(&out);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0) goto done;
        }
        snprintf(line, sizeof(line), "\033[H%s", passage);
        if (require_contains(&out, line) != 0) goto done;
        for (step = 0; step < 4; step++) {
            stage = "resize preserves the passage while earlier matches disappear or reappear";
            size.ws_col = step & 1 ? 80 : 25;
            ordinal = (step & 1 ? 1 : 0) + second_hits + (mode & 4 ? 1 : 0);
            snprintf(marker, sizeof(marker), "  %d/", ordinal);
            clear_capture(&out);
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                wait_for_redraw(master, &out, marker) != 0 || require_contains(&out, line) != 0) goto done;
            stage = "navigation uses the passage's new rank";
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", mode & 4 ? 1 : ordinal + 1);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
            if (!(mode & 4) && require_contains(&out, "\033[HTHIRD") != 0) goto done;
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", ordinal);
            if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        }
        clear_capture(&out);
        if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0 ||
            write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        pid = -1;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "resize match ranks failed: %s (mode=%d)\n", stage, mode);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

static int check_cached_search_resize(int compact)
{
    static const int widths[] = { 25, 80, 17, 80 };
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char record[96];
    char marker[48];
    char passage[24];
    int mode;
    int fd;
    int master;
    int i;
    int row;
    int hits;
    int ordinal;
    int step;
    int rc;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    path[0] = '\0';
    fd = master = -1;
    pid = -1;
    rc = -1;
    mode = 0;
    stage = "cached resize fixture";
    for (mode = 0; mode < 8; mode++) {
        hits = mode & 4 ? 2 : 1;
        size.ws_col = 80;
        size.ws_row = 10;
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-cached-resize-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        for (i = 0; i < (compact ? 400 : 10000); i++) {
            snprintf(record, sizeof(record), "ID%04d needle filler %s filler\n\n", i,
                hits == 2 ? "needle" : "filler");
            if (write_all(fd, record, strlen(record)) != 0) goto done;
        }
        if (close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;

            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            cache.cache_budget = compact ? 1024 : 65536;
            if (compact) mdf_pager_test_allow_small_cache(1);
            status = mdf_pager_file_with_options(path, &opts,
                mode & 1 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL, &cache);
            _exit(status == MDF_OK ? 0 : 1);
        }
        stage = "select a hit from a cached suffix";
        if (wait_for_redraw(master, &out, "ID0000") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "100%") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "/needle\r", 8) != 0 || wait_for_redraw(master, &out, "?/?") != 0 ||
            sscanf(out.data + out.frame_start, "\033[HID%d", &row) != 1 || row < 2) goto done;
        if (!(mode & 2)) {
            /* Establish a global ordinal, then deliberately take the cached
             * fast path instead of retaining the replay-selected match. */
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", row * hits);
            if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        }
        clear_capture(&out);
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "\033[7mneedle") != 0) goto done;
        if (!(mode & 2) && hits == 2) {
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", row * hits + 2);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        } else if ((mode & 2) && hits == 1) row++;
        ordinal = row * hits + hits;
        snprintf(passage, sizeof(passage), "ID%04d", row);
        if (require_contains(&out, passage) != 0) goto done;
        for (step = 0; step < 4; step++) {
            stage = "resize keeps the cached passage and its document-wide ordinal";
            clear_capture(&out);
            size.ws_col = (unsigned short)widths[step];
            snprintf(marker, sizeof(marker), "  %d/", ordinal);
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                wait_for_redraw(master, &out, marker) != 0 ||
                ((hits == 1 || widths[step] == 80) && require_contains(&out, passage) != 0)) goto done;
            stage = "next and previous use the same selected passage after reflow";
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", ordinal + 1);
            snprintf(record, sizeof(record), "ID%04d", row + 1);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0 ||
                require_contains(&out, record) != 0) goto done;
            clear_capture(&out);
            snprintf(marker, sizeof(marker), "  %d/", ordinal);
            if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        }
        clear_capture(&out);
        if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0 ||
            write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        pid = -1;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "cached search resize failed: %s (mode=%d)\n", stage, mode);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

static int check_cached_document(const char *path, size_t budget, size_t chunk, int styled, int text, int sparse)
{
    static const char keys[] = "GkkkkkkkkkkkkkkkkkkkkjjjjjjjjjjggGkkkkkkkkkkkk";
    struct winsize size;
    capture out;
    pager_large_source *state;
    size_t reads;
    size_t seeks;
    size_t i;
    int master;
    int rc;
    int phase;
    pid_t pid;
    const char *stage;
    const char *navigation;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = sparse ? 3 : 10;
    master = -1;
    rc = -1;
    phase = -1;
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (state == MAP_FAILED) return -1;
    memset(state, 0, sizeof(*state));
    state->expected_content_allocation = (budget == 0 ? 1048576 : budget) + 1;
    state->allocation_limit = state->expected_content_allocation * 65 + 32768;
    stage = "cache startup";
    pid = forkpty(&master, NULL, NULL, &size);
    if (pid < 0) goto done;
    if (pid == 0) {
        numbering_source input;
        mdf_seekable_source source;
        mdf_options opts;
        mdf_pager_options cache;
        mdf_status status;

        input.file = fopen(path, "rb");
        if (input.file == NULL) _exit(2);
        input.chunk = chunk;
        input.probe = state;
        source.userdata = &input;
        source.read = numbering_read;
        source.seek = numbering_seek;
        mdf_options_init(&opts);
        opts.ansi_mode = styled ? MDF_ANSI_ON : MDF_ANSI_OFF;
        opts.allocator.userdata = state;
        opts.allocator.alloc = pager_large_alloc;
        opts.allocator.realloc = NULL;
        opts.allocator.free = pager_large_free;
        cache.cache_budget = budget;
        if (budget != 0 && budget < 65536) mdf_pager_test_allow_small_cache(1);
        status = mdf_pager_seekable_source(path, &source, &opts,
            text ? MDF_PAGER_FORMAT_TEXT : MDF_PAGER_FORMAT_MARKDOWN, NULL, &cache);
        if (fclose(input.file) != 0) _exit(2);
        _exit(status == MDF_OK && state->live == 0 && state->content_frees == 1 ? 0 : 1);
    }
    if (wait_for_redraw(master, &out, "\033[H") != 0) goto done;
    if (sparse) {
        stage = "bounded initial input read-ahead retains a sparse document prefix";
        if (require_contains(&out, "--%") != 0) goto done;
        for (i = 0; i < 48; i++) {
            clear_capture(&out);
            if (write_all(master, "j", 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0) goto done;
        }
        clear_capture(&out);
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "100%") != 0) goto done;
        stage = "extending a sparse view does not evict retained rows while capacity is available";
        clear_capture(&out);
        if (write_all(master, "g", 1) != 0 || wait_for_redraw(master, &out, "\033[Hr") != 0 ||
            state->seeks != 1) goto done;
    }
    stage = "a fitting document reaches EOF on opening";
    if (require_contains(&out, "--%") == 0 || state->seeks != 1 || state->content_allocations != 1) goto done;
    clear_capture(&out);
    if (write_all(master, "/cache\r", 7) != 0 || wait_for_redraw(master, &out, "/cache  ") != 0) goto done;
    for (phase = 0; phase < (budget == 0 ? 3 : 1); phase++) {
        if (phase != 0) {
            stage = "resize caches fitting content once while preserving its search selection";
            seeks = state->seeks;
            state->fail_read = state->fail_seek = 0;
            clear_capture(&out);
            size.ws_col = phase == 1 ? 40 : 80;
            if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
                wait_for_redraw(master, &out, "/cache  ") != 0 || state->seeks != seeks + 1 ||
                require_contains(&out, "--%") == 0) goto done;
        }
        reads = state->reads;
        seeks = state->seeks;
        /* Fail immediately on a replay, rather than accepting a slow redraw. */
        state->fail_read = state->fail_seek = 1;
        stage = "cached scrolling through tables never reads or rerenders input";
        navigation = budget != 0 && budget < 65536 ? "GkjgGkj" : keys;
        for (i = 0; i < strlen(navigation); i++) {
            clear_capture(&out);
            if (write_all(master, navigation + i, 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0 ||
                state->reads != reads || state->seeks != seeks || state->content_allocations != 1 ||
                state->peak > state->allocation_limit) goto done;
        }
    }
    clear_capture(&out);
    if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "\033[H") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
    pid = -1;
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "cached document failed: %s (%s, chunk=%lu, phase=%d, seeks=%lu)\n",
            stage, path, (unsigned long)chunk, phase, (unsigned long)state->seeks);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (master >= 0) close(master);
    munmap(state, sizeof(*state));
    free(out.data);
    return rc;
}

static int check_pager_preload(int compact)
{
    static const char section[] = "## Component\n\nA paragraph with **styled** words and [a link](https://example.com).\n\n"
        "| Item | Description |\n| --- | --- |\n| cache | Bounded memory for the complete small document |\n"
        "| stream | The renderer keeps its normal emission decisions |\n\n";
    static const char cache_table[] =
        "| a | b | c | d | e | f | g | h |\n"
        "| --- | --- | --- | --- | --- | --- | --- | --- |\n"
        "| cache | cache | cache | cache | cache | cache | cache | cache |\n";
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char row[80];
    int mode;
    int fd;
    int i;
    int rc;

    for (mode = 0; mode < 9; mode++) {
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-preload-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) return -1;
        if (mode == 8) {
            for (i = 0; i < 24; i++)
                if (write_all(fd, "r\n\n", 3) != 0) { close(fd); unlink(path); return -1; }
            if (write_all(fd, cache_table, sizeof(cache_table) - 1) != 0) { close(fd); unlink(path); return -1; }
        } else if (mode < 4) {
            for (i = 0; i < (compact ? 16 : 100); i++)
                if (write_all(fd, section, sizeof(section) - 1) != 0) { close(fd); unlink(path); return -1; }
        } else if (mode < 6) {
            memset(row, 'x', 70);
            row[70] = '\n';
            if (write_all(fd, "```\n", 4) != 0) { close(fd); unlink(path); return -1; }
            for (i = 0; i < (compact ? 13 : 860); i++)
                if (write_all(fd, row, 71) != 0) { close(fd); unlink(path); return -1; }
            if (write_all(fd, "```\n", 4) != 0) { close(fd); unlink(path); return -1; }
        } else {
            size_t gap;
            size_t remaining;
            size_t bytes;

            /* Keep the next table beyond even the last bounded input chunk. */
            gap = (compact ? 2048 : 65536) + 1024;
            for (i = 0; i < 24; i++)
                if (write_all(fd, "r\n\n", 3) != 0) { close(fd); unlink(path); return -1; }
            memset(row, '\n', sizeof(row));
            for (remaining = gap; remaining != 0; remaining -= bytes) {
                bytes = remaining < sizeof(row) ? remaining : sizeof(row);
                if (write_all(fd, row, bytes) != 0) { close(fd); unlink(path); return -1; }
            }
            if (write_all(fd, "| cache | state |\n| --- | --- |\n| a | b |\n| c | d |\n| e | f |\n| g | h |\n",
                strlen("| cache | state |\n| --- | --- |\n| a | b |\n| c | d |\n| e | f |\n| g | h |\n")) != 0) {
                close(fd); unlink(path); return -1;
            }
        }
        if (close(fd) != 0) { unlink(path); return -1; }
        rc = check_cached_document(path, mode < 4 || mode == 8 ? 0 : (compact ? (mode < 6 ? 1024 : 2048) : 65536),
            mode == 0 ? 1 : (mode == 1 ? 3 : 512), mode == 2 || mode == 7,
            mode == 3 || mode == 5, mode >= 6 && mode < 8);
        unlink(path);
        if (rc != 0) return -1;
    }
    return 0;
}

static int check_search_numbering_states(int compact)
{
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char record[64];
    char prefix[64];
    char marker[32];
    int mode;
    int fd;
    int master;
    int i;
    int hit;
    int positions[17];
    int initial_hit;
    int rc;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 10;
    path[0] = '\0';
    fd = master = -1;
    pid = -1;
    rc = -1;
    mode = 0;
    stage = "numbering fixture";
    for (hit = 0; hit < 17; hit++) positions[hit] = hit < 13 ? hit + 1 :
        (hit == 16 ? (compact ? 120 : 240) : 64 + (hit - 13) * (compact ? 16 : 48));
    if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-numbering-XXXXXX") >= (int)sizeof(path) ||
        (fd = mkstemp(path)) < 0) goto done;
    for (i = 0; i < (compact ? 128 : 256); i++) {
        size_t length;
        size_t j;

        snprintf(prefix, sizeof(prefix), "passage%08d ordinary words ", i);
        for (hit = 0; hit < 17; hit++) if (i == positions[hit])
            snprintf(prefix, sizeof(prefix), "ID%02d @ abcdefghijklmnopqrstuv Full Stra\303\237e ", hit);
        length = strlen(prefix);
        memcpy(record, prefix, length);
        for (j = length; j < 62; j++) record[j] = " filler text"[(j - length) % 12];
        record[62] = record[63] = '\n';
        if (write_all(fd, record, sizeof(record)) != 0) goto done;
    }
    if (close(fd) != 0) goto done;
    fd = -1;
    for (mode = 0; mode < (compact ? 2 : 6); mode++) {
        size.ws_col = 80;
        size.ws_row = 10;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;
            numbering_source input;
            mdf_seekable_source source;

            mdf_options_init(&opts);
            opts.ansi_mode = mode & 2 ? MDF_ANSI_ON : MDF_ANSI_OFF;
            cache.cache_budget = 2048;
            mdf_pager_test_allow_small_cache(1);
            if (mode < 4) status = mdf_pager_file_with_options(path, &opts,
                mode & 1 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL, &cache);
            else {
                input.file = fopen(path, "rb");
                if (input.file == NULL) _exit(2);
                input.chunk = mode == 4 ? 1 : 3;
                input.probe = NULL;
                source.userdata = &input;
                source.read = numbering_read;
                source.seek = numbering_seek;
                status = mdf_pager_seekable_source("case", &source, &opts,
                    mode & 1 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL, &cache);
                if (fclose(input.file) != 0) _exit(2);
            }
            _exit(status == MDF_OK ? 0 : 1);
        }
        stage = "numbering startup";
        if (wait_for_redraw(master, &out, "passage00000000") != 0) goto done;
        clear_capture(&out);
        stage = "EOF discovered before a query does not establish its total";
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "100%") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "/Full\r", 6) != 0 || wait_for_redraw(master, &out, "?/?") != 0) goto done;
        if (sscanf(out.data + out.frame_start, "\033[HID%d", &initial_hit) != 1 || initial_hit < 1) goto done;
        clear_capture(&out);
        stage = "first backward search establishes the global ordinal from an initially unknown prefix";
        snprintf(marker, sizeof(marker), "  %d/?", initial_hit);
        if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        snprintf(marker, sizeof(marker), "\033[HID%02d", initial_hit - 1);
        if (require_contains(&out, marker) != 0) goto done;
        clear_capture(&out);
        stage = "Home recovers the first global ordinal without scanning to EOF";
        if (write_all(master, "g", 1) != 0 || wait_for_redraw(master, &out, "  1/?") != 0) goto done;
        clear_capture(&out);
        for (hit = 1; hit < 17; hit++) {
            stage = "Full search increments globally beyond the first thirteen cached hits";
            snprintf(marker, sizeof(marker), "  %d/", hit + 1);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
            snprintf(marker, sizeof(marker), "\033[HID%02d", hit);
            if (require_contains(&out, marker) != 0 || strstr(out.data, " cached") != NULL) goto done;
            clear_capture(&out);
        }
        stage = "a full query pass makes the total decisive, and n wraps only at its real end";
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "  1/17") != 0 ||
            require_contains(&out, "\033[HID00") != 0) goto done;
        clear_capture(&out);
        for (hit = 16; hit >= 0; hit--) {
            stage = "backward traversal retains global ordinals and the established total";
            snprintf(marker, sizeof(marker), "  %d/17", hit + 1);
            if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
            snprintf(marker, sizeof(marker), "\033[HID%02d", hit);
            if (require_contains(&out, marker) != 0) goto done;
            clear_capture(&out);
        }
        stage = "changing the query discards totals; a one-character confirmed query works";
        if (write_all(master, "q/@\r", 4) != 0 || wait_for_redraw(master, &out, "  /@  1/?") != 0) goto done;
        clear_capture(&out);
        stage = "End counts matches in evicted rendered content";
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "/17") != 0 ||
            require_contains(&out, "100%") != 0) goto done;
        clear_capture(&out);
        stage = "editing and backspacing recomputes the query without reusing an old total";
        if (write_all(master, "qg/FullX\177\r", 10) != 0 || wait_for_redraw(master, &out, "  /Full  1/?") != 0) goto done;
        clear_capture(&out);
        stage = "Unicode folding agrees between cached highlights and global counting";
        if (write_all(master, "q/STRASSE\r", 10) != 0 || wait_for_redraw(master, &out, "  /STRASSE  1/?") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "/17") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "g", 1) != 0 || wait_for_redraw(master, &out, "  1/17") != 0) goto done;
        clear_capture(&out);
        stage = "resize invalidates width-dependent counts but retains the selected passage";
        size.ws_col = 40;
        if (ioctl(master, TIOCSWINSZ, &size) != 0 || kill(pid, SIGWINCH) != 0 ||
            wait_for_redraw(master, &out, "  1/?") != 0 || require_contains(&out, "ID00") != 0) goto done;
        clear_capture(&out);
        stage = "a wrapped multiword Unicode query has the same complete total";
        if (write_all(master, "q/FULL STRASSE\r", 15) != 0 || wait_for_redraw(master, &out, "  1/?") != 0) goto done;
        clear_capture(&out);
        for (hit = 1; hit < 17; hit++) {
            stage = "multirow matches straddling cache seams retain every ordinal";
            snprintf(marker, sizeof(marker), "  %d/", hit + 1);
            if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
            clear_capture(&out);
        }
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "/17") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "g", 1) != 0 || wait_for_redraw(master, &out, "  1/17") != 0) goto done;
        clear_capture(&out);
        for (hit = 16; hit >= 0; hit--) {
            stage = "backward multirow matches straddling cache seams retain every ordinal";
            snprintf(marker, sizeof(marker), "  %d/17", hit + 1);
            if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
            clear_capture(&out);
        }
        stage = "a replacement absent query makes a complete round trip and restores its viewport";
        if (write_all(master, "qg/missing\r", 11) != 0 || wait_for_redraw(master, &out, "  0/?") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "  0/0") != 0 ||
            require_contains(&out, "passage00000000") != 0) goto done;
        if (write_all(master, "qq", 2) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        pid = -1;
        close(master);
        master = -1;
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "search numbering failed: %s (mode %d)\n", stage, mode);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

static int check_search_boundary_documents(void)
{
    static const int counts[] = { 0, 0, 1, 4, 1024, 1024, 8192, 32, 64 };
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char expected[32];
    int fd;
    int master;
    int mode;
    int i;
    int rc;
    pid_t pid;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 8;
    fd = master = -1;
    pid = -1;
    path[0] = '\0';
    rc = -1;
    stage = "boundary fixture";
    for (mode = 0; mode < 9; mode++) {
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-boundary-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        if (mode == 1 && write_all(fd, "\n \t\n", 4) != 0) goto done;
        if (mode == 2 && write_all(fd, "Full", 4) != 0) goto done;
        if (mode == 3 && write_all(fd, "Full\tFull\r\nFull\rFull", 20) != 0) goto done;
        if (mode == 4 || mode == 5) {
            if (write_all(fd, "| label | value |\n|---|---|\n", 28) != 0) goto done;
            for (i = 0; i < 1024; i++) if (write_all(fd, "| Full | text |\n", 16) != 0) goto done;
        }
        if (mode == 6) for (i = 0; i < 8192; i++) if (write_all(fd, "Full", 4) != 0) goto done;
        if (mode >= 7) for (i = 0; i < 32; i++) {
            static const char link[] = "[Full](https://example.com/Full)\n\n";
            if (write_all(fd, link, sizeof(link) - 1) != 0) goto done;
        }
        if (close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf_status status;

            mdf_options_init(&opts);
            opts.ansi_mode = mode == 7 ? MDF_ANSI_ON : MDF_ANSI_OFF;
            opts.osc8 = mode == 7;
            opts.table_buffer_mode = mode == 5 ? MDF_TABLE_BUFFER_ROW : MDF_TABLE_BUFFER_FULL;
            cache.cache_budget = 2048;
            mdf_pager_test_allow_small_cache(1);
            status = mdf_pager_file_with_options(path, &opts,
                mode >= 4 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT, NULL, &cache);
            _exit(status == MDF_OK ? 0 : 1);
        }
        stage = "empty, whitespace, control, no-final-newline, table or file-backed-word startup";
        if (wait_for_redraw(master, &out, "\033[7m\033[1;32m") != 0) goto done;
        clear_capture(&out);
        /* Full-table and unfinished-word decisions can outlive the initial
         * input allowance. Explicit End resolves them and counts all output. */
        snprintf(expected, sizeof(expected), "/Full  %d/", counts[mode] == 0 || mode == 4 || mode == 6 ? 0 : 1);
        if (write_all(master, "/Full\r", 6) != 0 || wait_for_redraw(master, &out, expected) != 0) goto done;
        if (mode < 4) {
            stage = "fully cached boundary cases establish the total immediately";
            snprintf(expected, sizeof(expected), "  %d/%d", mode < 2 ? 0 : 1, counts[mode]);
            if (require_contains(&out, expected) != 0) goto done;
        }
        clear_capture(&out);
        stage = "End counts discarded table output and backed words using bounded query state";
        if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "100%") != 0) goto done;
        snprintf(expected, sizeof(expected), "/%d", counts[mode]);
        if (require_contains(&out, expected) != 0) goto done;
        clear_capture(&out);
        stage = "global total survives recovery of evicted initial output";
        if (write_all(master, "g", 1) != 0 || wait_for_redraw(master, &out, expected) != 0) goto done;
        if (write_all(master, "qq", 2) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        pid = -1;
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "search boundary failed: %s (mode %d)\n", stage, mode);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

static int check_wrapped_origin_seams(const char *cmdf, int compact)
{
    static const char query[] = "ABCDEFGHIJKLMNOPQRSTUVWX";
    static const char negative[] = "ABCDEFGHIJKLMNOPQRSTUVWZ";
    static const char keys[] = "nnNnNN";
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char chunk[256];
    char highlight[40];
    char budget_argument[32];
    size_t budget;
    size_t prefix;
    size_t remaining;
    size_t count;
    size_t visible;
    size_t step;
    pid_t pid;
    int fd;
    int master;
    int mode;
    int rc;
    const char *stage;

    memset(&out, 0, sizeof(out));
    memset(chunk, '.', sizeof(chunk));
    path[0] = '\0';
    pid = -1;
    fd = master = -1;
    rc = -1;
    stage = "origin-seam fixture";
    for (mode = 0; mode < 16; mode++) {
        memset(&size, 0, sizeof(size));
        size.ws_col = (unsigned short)(12 + ((mode >> 1) & 1));
        size.ws_row = (unsigned short)(2 + ((mode >> 2) & 1));
        budget = (compact ? 1024 : 65536) + (size_t)((mode >> 3) & 1);
        /* The cache counts its inserted wrap newlines too. Leave only the
         * first 1 or 11 query bytes retained by the initial read-ahead. */
        prefix = (budget / (size.ws_col + 1)) * size.ws_col + budget % (size.ws_col + 1) -
            (mode & 1 ? 11 : 1);
        visible = size.ws_col - prefix % size.ws_col;
        snprintf(highlight, sizeof(highlight), "\033[7m%.*s\033[27m", (int)visible, query);
        if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-origin-seam-XXXXXX") >= (int)sizeof(path) ||
            (fd = mkstemp(path)) < 0) goto done;
        for (remaining = prefix; remaining != 0; remaining -= count) {
            count = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            if (write_all(fd, chunk, count) != 0) goto done;
        }
        if (write_all(fd, query, sizeof(query) - 1) != 0) goto done;
        for (remaining = budget; remaining != 0; remaining -= count) {
            count = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            if (write_all(fd, chunk, count) != 0) goto done;
        }
        /* An unfinished candidate at EOF must not prevent a bounded cycle. */
        if (write_all(fd, query, sizeof(query) - 2) != 0 || close(fd) != 0) goto done;
        fd = -1;
        pid = forkpty(&master, NULL, NULL, &size);
        if (pid < 0) goto done;
        if (pid == 0) {
            mdf_options opts;
            mdf_pager_options cache;
            mdf *receiver;
            mdf_status status;

            if (!compact && mode == 1) {
                snprintf(budget_argument, sizeof(budget_argument), "%lu", (unsigned long)budget);
                execl(cmdf, cmdf, "--pager", "--pager-format", "text", "--pager-cache", budget_argument, path, (char *)NULL);
                _exit(2);
            }
            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            cache.cache_budget = budget;
            if (compact) mdf_pager_test_allow_small_cache(1);
            if (mode & 1) {
                receiver = NULL;
                if (mdf_create(MDF_FORMAT_ANSI, &opts, &receiver) != MDF_OK) _exit(2);
                status = receiver->pager_file_with_options(receiver, path, MDF_PAGER_FORMAT_TEXT, NULL, &cache);
                receiver->destroy(receiver);
            } else status = mdf_pager_file_with_options(path, &opts, MDF_PAGER_FORMAT_TEXT, NULL, &cache);
            _exit(status == MDF_OK ? 0 : 1);
        }
        stage = "initial read retains an unresolved query prefix at the cache seam";
        if (wait_for_redraw(master, &out, "--%") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "/", 1) != 0 || write_all(master, query, sizeof(query) - 1) != 0 ||
            write_all(master, "\r", 1) != 0 || wait_for_redraw(master, &out, "/?") != 0) goto done;
        clear_capture(&out);
        stage = "first forward traversal finds and highlights the sole complete match";
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "  1/") != 0 ||
            require_contains(&out, highlight) != 0) goto done;
        for (step = 0; step < sizeof(keys) - 1; step++) {
            clear_capture(&out);
            stage = "cyclic traversal resolves the origin match before stopping";
            if (write_all(master, keys + step, 1) != 0 || wait_for_redraw(master, &out, "  1/1") != 0 ||
                require_contains(&out, highlight) != 0) goto done;
        }
        clear_capture(&out);
        stage = "a failed candidate and unfinished EOF prefix still terminate a no-match cycle";
        if (write_all(master, "q/", 2) != 0 || write_all(master, negative, sizeof(negative) - 1) != 0 ||
            write_all(master, "\r", 1) != 0 || wait_for_redraw(master, &out, "  0/?") != 0) goto done;
        clear_capture(&out);
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "  0/0") != 0 ||
            capture_range_contains(&out, highlight, out.frame_start, out.frame_end)) goto done;
        if (write_all(master, "qq", 2) != 0) goto done;
        {
            int exited;

            exited = wait_for_exit(pid);
            pid = -1;
            if (exited != 0 || !terminal_is_cooked(master)) goto done;
        }
        close(master);
        master = -1;
        unlink(path);
        path[0] = '\0';
        clear_capture(&out);
    }
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "wrapped origin failed: %s (mode=%d)\n", stage, mode);
        if (master >= 0) (void)wait_for_output(master, &out, 40);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

static int check_long_query_seams(int compact)
{
    struct winsize size;
    capture out;
    char path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 64];
    char query[1201];
    char record[1250];
    char marker[32];
    int fd;
    int master;
    int hit;
    int rc;
    pid_t pid;
    const char *stage;
    size_t query_length;
    size_t record_length;
    unsigned long seed;
    int step;

    memset(&out, 0, sizeof(out));
    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = compact ? 2 : 8;
    query_length = compact ? 80 : 1200;
    record_length = query_length + 50;
    memset(query, 'a', query_length);
    query[0] = '~'; query[query_length - 1] = '!'; query[query_length] = '\0';
    path[0] = '\0';
    pid = -1;
    master = fd = -1;
    rc = -1;
    stage = "long-query fixture";
    if (snprintf(path, sizeof(path), LIBMDF_TEST_SCRATCH_DIR "/pager-long-query-XXXXXX") >= (int)sizeof(path) ||
        (fd = mkstemp(path)) < 0) goto done;
    for (hit = 0; hit < 18; hit++) {
        snprintf(record, sizeof(record), "ID%02d ", hit);
        memcpy(record + 5, query, query_length);
        memset(record + 5 + query_length, 'x', 43);
        record[record_length - 2] = record[record_length - 1] = '\n';
        if (write_all(fd, record, record_length) != 0) goto done;
    }
    if (close(fd) != 0) goto done;
    fd = -1;
    pid = forkpty(&master, NULL, NULL, &size);
    if (pid < 0) goto done;
    if (pid == 0) {
        mdf_options opts;
        mdf_pager_options cache;

        mdf_options_init(&opts);
        opts.ansi_mode = MDF_ANSI_OFF;
        cache.cache_budget = compact ? 1024 : 2048;
        mdf_pager_test_allow_small_cache(1);
        _exit(mdf_pager_file_with_options(path, &opts, MDF_PAGER_FORMAT_TEXT, NULL, &cache) == MDF_OK ? 0 : 1);
    }
    stage = "a query longer than the viewport keeps its global count visible";
    if (wait_for_redraw(master, &out, "ID00") != 0) goto done;
    clear_capture(&out);
    if (write_all(master, "/", 1) != 0 || write_all(master, query, query_length) != 0 || write_all(master, "\r", 1) != 0 ||
        wait_for_redraw(master, &out, "  1/?") != 0) goto done;
    for (hit = 1; hit < 18; hit++) {
        clear_capture(&out);
        stage = "forward search preserves long matches straddling cache seams";
        snprintf(marker, sizeof(marker), "  %d/", hit + 1);
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        snprintf(marker, sizeof(marker), "\033[HID%02d", hit);
        if (require_contains(&out, marker) != 0) goto done;
    }
    clear_capture(&out);
    if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "  1/18") != 0) goto done;
    for (hit = 17; hit >= 0; hit--) {
        clear_capture(&out);
        stage = "backward search preserves long matches straddling cache seams";
        snprintf(marker, sizeof(marker), "  %d/18", hit + 1);
        if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        snprintf(marker, sizeof(marker), "\033[HID%02d", hit);
        if (require_contains(&out, marker) != 0) goto done;
    }
    hit = 0;
    seed = 89;
    for (step = 0; step < (compact ? 16 : 64); step++) {
        int backward;

        clear_capture(&out);
        seed = (seed * 251 + 89) & 65535;
        backward = (int)((seed >> 8) & 1);
        hit = (hit + (backward ? 17 : 1)) % 18;
        stage = "deterministic direction changes agree with the independent cyclic match model";
        snprintf(marker, sizeof(marker), "  %d/18", hit + 1);
        if (write_all(master, backward ? "N" : "n", 1) != 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        snprintf(marker, sizeof(marker), "\033[HID%02d", hit);
        if (require_contains(&out, marker) != 0) goto done;
    }
    if (write_all(master, "qq", 2) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
    pid = -1;
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "long-query search failed: %s\n", stage);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (fd >= 0) close(fd);
    if (master >= 0) close(master);
    if (path[0] != '\0') unlink(path);
    free(out.data);
    return rc;
}

static int capture_first_row(capture *row, const capture *frame)
{
    size_t i;

    row->len = 0;
    if (row->data != NULL) row->data[0] = '\0';
    for (i = frame->frame_start; i < frame->len && frame->data[i] != '\r' && frame->data[i] != '\n';) {
        if (frame->data[i] == '\033' && i + 1 < frame->len && frame->data[i + 1] == '[') {
            i += 2;
            while (i < frame->len) {
                unsigned char byte;

                byte = (unsigned char)frame->data[i++];
                if (byte >= 0x40 && byte <= 0x7e) break;
            }
        } else {
            if (capture_append(row, frame->data + i, 1) != 0) return -1;
            i++;
        }
    }
    return row->len == 0 ? -1 : 0;
}

static int check_essay_search_wrap(const char *cmdf, const char *path)
{
    struct winsize size;
    capture out;
    capture first_row;
    capture last_row;
    capture current_row;
    pid_t pid;
    int master;
    int hit;
    int rc;
    const char *stage;
    char count[32];

    memset(&out, 0, sizeof(out));
    memset(&first_row, 0, sizeof(first_row));
    memset(&last_row, 0, sizeof(last_row));
    memset(&current_row, 0, sizeof(current_row));
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 24;
    master = -1;
    rc = -1;
    stage = "essay startup with a 65536-byte content cache";
    pid = forkpty(&master, NULL, NULL, &size);
    if (pid < 0) goto done;
    if (pid == 0) {
        execl(cmdf, cmdf, "--pager", "--pager-cache", "65536", "--ansi", "on", "--osc8", "off",
              "--pager-format", "markdown", path, (char *)NULL);
        _exit(127);
    }
    if (wait_for_redraw(master, &out, "Coordination Shift") != 0) goto done;
    clear_capture(&out);
    stage = "query initially absent from the first cached slice";
    if (write_all(master, "/future of jobs\r", 16) != 0 || wait_for_redraw(master, &out, "0/?") != 0)
        goto done;
    clear_capture(&out);
    for (hit = 1; hit <= 4; hit++) {
        stage = "advance through the essay's four matches";
        snprintf(count, sizeof(count), "  %d/", hit);
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, count) != 0) goto done;
        if (hit == 1 && capture_first_row(&first_row, &out) != 0) goto done;
        if (hit == 4 && capture_first_row(&last_row, &out) != 0) goto done;
        clear_capture(&out);
    }
    for (hit = 0; hit < 2; hit++) {
        stage = "n wraps from the fourth hit through an empty tail slice to the first hit";
        if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "1/4") != 0 ||
            capture_first_row(&current_row, &out) != 0 || strcmp(current_row.data, first_row.data) != 0) goto done;
        clear_capture(&out);
        stage = "N wraps from the first hit through empty earlier slices to the fourth hit";
        if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, "4/4") != 0 ||
            capture_first_row(&current_row, &out) != 0 || strcmp(current_row.data, last_row.data) != 0) goto done;
        clear_capture(&out);
    }
    if (write_all(master, "qq", 2) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
    pid = -1;
    rc = 0;
done:
    if (rc != 0) {
        fprintf(stderr, "essay search wrap failed: %s\n", stage);
        if (out.data != NULL) fwrite(out.data, 1, out.len, stderr);
    }
    if (pid > 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    if (master >= 0) close(master);
    free(out.data);
    free(first_row.data);
    free(last_row.data);
    free(current_row.data);
    return rc;
}

int main(int argc, char **argv)
{
    char text_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char markdown_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char osc8_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char osc8_reflow_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char wide_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char wide_tab_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char utf8_search_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char unicode_casefold_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char initial_resize_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char incremental_resize_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char control_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char unicode_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char styled_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char long_word_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char nul_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char fifo_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char c1_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char carriage_return_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char control_markdown_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    char missing_path[sizeof(LIBMDF_TEST_SCRATCH_DIR) + 128];
    capture out;
    pid_t pid;
    int fd;
    int master;
    int rc;
    int i;
    int mode;
    int cancel_fd;
    struct winsize resized;
    struct timespec resize_started;
    struct timespec resize_finished;
    pager_route_allocator *route_allocator;
    const char *stage;

    if (check_redraw_assertion_scope() != 0) return 1;
    if (argc == 3 && (strcmp(argv[2], "--resize-expansion") == 0 || strcmp(argv[2], "--resize-expansion-memory") == 0))
        return check_resize_cache_expansion(strcmp(argv[2], "--resize-expansion-memory") == 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--repeated-resize") == 0 || strcmp(argv[2], "--repeated-resize-memory") == 0))
        return check_repeated_resize(strcmp(argv[2], "--repeated-resize-memory") == 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--sparse-read-ahead") == 0 || strcmp(argv[2], "--sparse-read-ahead-memory") == 0))
        return check_sparse_read_ahead(strcmp(argv[2], "--sparse-read-ahead-memory") == 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--resize-ranks") == 0 || strcmp(argv[2], "--resize-ranks-memory") == 0))
        return check_resize_match_ranks(strcmp(argv[2], "--resize-ranks-memory") == 0) == 0 ? 0 : 1;
    if (argc == 4 && strcmp(argv[2], "--cached-file") == 0)
        return check_cached_document(argv[3], 0, 512, 1, 0, 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--preload") == 0 || strcmp(argv[2], "--preload-memory") == 0))
        return check_pager_preload(strcmp(argv[2], "--preload-memory") == 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--cached-resize") == 0 || strcmp(argv[2], "--cached-resize-memory") == 0))
        return check_cached_search_resize(strcmp(argv[2], "--cached-resize-memory") == 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--large-matches") == 0 || strcmp(argv[2], "--large-matches-memory") == 0))
        return check_pager_large_matches(argv[1], strcmp(argv[2], "--large-matches-memory") == 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--row-bytes") == 0 || strcmp(argv[2], "--row-bytes-memory") == 0))
        return check_pager_row_bytes(strcmp(argv[2], "--row-bytes-memory") == 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--search-origin") == 0 || strcmp(argv[2], "--search-origin-memory") == 0))
        return check_wrapped_origin_seams(argv[1], strcmp(argv[2], "--search-origin-memory") == 0) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp(argv[2], "--cache-growth") == 0 || strcmp(argv[2], "--cache-growth-memory") == 0))
        return check_pager_cache_growth(argv[1], strcmp(argv[2], "--cache-growth-memory") == 0) == 0 ? 0 : 1;
    if (argc == 4 && (strcmp(argv[2], "--search-wrap") == 0 || strcmp(argv[2], "--search-wrap-memory") == 0)) {
        if (check_generated_search_wrap(strcmp(argv[2], "--search-wrap-memory") == 0) != 0) return 1;
        if (check_search_numbering_states(strcmp(argv[2], "--search-wrap-memory") == 0) != 0) return 1;
        if (check_search_boundary_documents() != 0) return 1;
        if (check_long_query_seams(strcmp(argv[2], "--search-wrap-memory") == 0) != 0) return 1;
        return check_essay_search_wrap(argv[1], argv[3]) == 0 ? 0 : 1;
    }
    if (argc == 3 && (strcmp(argv[2], "--large-files") == 0 || strcmp(argv[2], "--large-files-memory") == 0))
        return check_large_pager(argv[1], strcmp(argv[2], "--large-files-memory") == 0) == 0 ? 0 : 1;
    if (argc != 2 && argc != 4 && !(argc == 3 && strcmp(argv[2], "--features") == 0)) return 2;
    memset(&out, 0, sizeof(out));
    route_allocator = MAP_FAILED;
    missing_path[0] = '\0';
    rc = 1;
    {
        static const char markdown[] = "a\033]52;c;INJECT\a \346\227\245\n";
        static const char expected[] = "a^[]52;c;INJECT^G \346\227\245\n";
        pager_streaming_sanitize_source source_data;
        mdf_source source;
        mdf_sink sink;
        mdf_status status;

        memset(&source_data, 0, sizeof(source_data));
        source_data.data = markdown;
        source_data.len = sizeof(markdown) - 1;
        source_data.chunk = 3;
        source.userdata = &source_data;
        source.read = pager_streaming_sanitize_read;
        sink.userdata = &source_data;
        sink.write = pager_streaming_sanitize_write;
        stage = "streaming markdown sanitizer";
        status = mdf_pager_sanitize_markdown_source(&source, &sink, NULL);
        if (status != MDF_OK || source_data.reads < 2 || source_data.writes < 2 ||
            source_data.output.data == NULL || strcmp(source_data.output.data, expected) != 0) {
            free(source_data.output.data);
            goto done;
        }
        free(source_data.output.data);
    }
    stage = "fixtures";
    if (write_fixture(text_path, sizeof(text_path), "text.txt", 0) != 0 ||
        write_fixture(markdown_path, sizeof(markdown_path), "markdown.md", 1) != 0 ||
        write_osc8_fixture(osc8_path, sizeof(osc8_path)) != 0 ||
        write_osc8_reflow_fixture(osc8_reflow_path, sizeof(osc8_reflow_path)) != 0 ||
        write_wide_fixture(wide_path, sizeof(wide_path)) != 0 ||
        write_wide_tab_fixture(wide_tab_path, sizeof(wide_tab_path)) != 0 ||
        write_utf8_search_fixture(utf8_search_path, sizeof(utf8_search_path)) != 0 ||
        write_unicode_casefold_fixture(unicode_casefold_path, sizeof(unicode_casefold_path)) != 0 ||
        write_initial_resize_fixture(initial_resize_path, sizeof(initial_resize_path)) != 0 ||
        write_incremental_resize_fixture(incremental_resize_path, sizeof(incremental_resize_path)) != 0 ||
        write_status_path_fixture(control_path, sizeof(control_path), "\033]52;c;INJECT\a.txt") != 0 ||
        write_status_path_fixture(unicode_path, sizeof(unicode_path),
                                  "\346\227\245\346\234\254\350\252\236\346\227\245.txt") != 0 ||
        write_styled_search_fixture(styled_path, sizeof(styled_path)) != 0 ||
        write_long_word_markdown_fixture(long_word_path, sizeof(long_word_path)) != 0 ||
        write_nul_markdown_fixture(nul_path, sizeof(nul_path)) != 0 ||
        write_fifo_fixture(fifo_path, sizeof(fifo_path)) != 0 ||
        write_c1_text_fixture(c1_path, sizeof(c1_path)) != 0 ||
        write_carriage_return_text_fixture(carriage_return_path, sizeof(carriage_return_path)) != 0 ||
        write_control_markdown_fixture(control_markdown_path, sizeof(control_markdown_path)) != 0) goto done;
    if (snprintf(missing_path, sizeof(missing_path), LIBMDF_TEST_SCRATCH_DIR "/libmdf-pager-missing-XXXXXX") >= (int)sizeof(missing_path) ||
        (fd = mkstemp(missing_path)) < 0 || close(fd) != 0 || unlink(missing_path) != 0) goto done;

    stage = "overwrite redraw protocol";
    if (check_overwrite_redraws(argv[1]) != 0) goto done;
    for (mode = 0; mode < (argc == 4 ? 2 : 4); mode++) {
        stage = "explicit CLI pager text/markdown format, ordinary and incremental";
        pid = start_selected_pager(argv[1], mode & 1 ? text_path : markdown_path,
                                   mode & 1 ? "markdown" : "text", mode & 2, &master);
        if (pid < 0 || wait_for_redraw(master, &out, mode & 1 ? "raw heading" : "# rendered heading") != 0 ||
            (mode & 1 && strstr(out.data, "**literal**") != NULL) ||
            write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0 || !terminal_is_cooked(master)) goto done;
        close(master);
        clear_capture(&out);
    }
    if (argc == 4) {
        for (mode = 0; mode < 4; mode++) {
            stage = "Lua pager cancellation and callback error cleanup";
            pid = start_lua_pager(argv[2], argv[3], text_path,
                                  mode == 0 ? "cancel" : (mode == 1 ? "error" : (mode == 2 ? "early" : "oversize")), &master);
            if (pid < 0 || wait_for_exit(pid) != 0 || wait_for_output(master, &out, 40) != 0 ||
                require_contains(&out, "LUA_PAGER_DONE") != 0 || !terminal_is_cooked(master)) goto done;
            if (mode != 2 && (require_contains(&out, "\033[?1049h") != 0 ||
                             require_contains(&out, "\033[?1049l") != 0)) goto done;
            if (mode == 2 && strstr(out.data, "\033[?1049h") != NULL) goto done;
            close(master);
            clear_capture(&out);
        }
        rc = 0;
        goto done;
    }
    stage = "SIGINT cancellation with returning or ignored application handlers";
    if (check_sigint_return_status(text_path, markdown_path) != 0) goto done;
    for (mode = 0; mode < 8; mode++) {
        mdf_pager_format pager_format;
        const char *path;
        const char *marker;

        pager_format = mode & 4 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT;
        path = mode & 4 ? markdown_path : text_path;
        marker = mode & 1 ? "Literal" : (mode & 4 ? "rendered heading" : "# raw heading");
        stage = "cooperative cancellation in search, free/receiver file/source";
        pid = start_cancelable_pager(path, mode & 1, mode & 2, 0, pager_format, &master, &cancel_fd);
        if (pid < 0 || wait_for_redraw(master, &out, marker) != 0) goto done;
        if (mode == 1 && (require_contains(&out, "**log**") != 0 ||
                         require_contains(&out, "abcdefghijklmnopqrst") != 0)) goto done;
        clear_capture(&out);
        if (mode == 1) {
            stage = "text search crosses a hard-wrap boundary";
            if (write_all(master, "/stuv", 5) != 0 ||
                wait_for_redraw(master, &out, "\033[7muv\033[27m") != 0 ||
                require_contains(&out, "abcdefghijklmnopqr\033[7mst\033[27m") != 0) goto done;
            clear_capture(&out);
            if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "abcdefghijklmnopqrst") != 0) goto done;
            clear_capture(&out);
        }
        if (mode == 0) {
            stage = "cancellation during a partial escape key";
            if (write_all(master, "\033[", 2) != 0) goto done;
        } else if (mode == 2) {
            stage = "cancellation during a pending resize";
            memset(&resized, 0, sizeof(resized));
            resized.ws_col = 30;
            resized.ws_row = 10;
            if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0) goto done;
        } else if (write_all(master, "/", 1) != 0 || wait_for_marker(master, &out, "  /") != 0) goto done;
        if (write_all(cancel_fd, "x", 1) != 0 || wait_for_exit(pid) != 0 ||
            wait_for_marker(master, &out, "\033[?1049l") != 0 || !terminal_is_cooked(master)) goto done;
        close(cancel_fd);
        close(master);
        clear_capture(&out);
    }
    for (mode = 0; mode < 11; mode++) {
        int phase;
        mdf_pager_format pager_format;

        phase = mode < 4 ? (mode & 1 ? 3 : 1) : (mode < 8 ? 2 : (mode < 10 ? 4 : 5));
        pager_format = mode >= 8 || mode & 1 ? MDF_PAGER_FORMAT_MARKDOWN : MDF_PAGER_FORMAT_TEXT;
        stage = "cooperative cancellation before input, during input and during view generation";
        pid = start_cancelable_pager(text_path, 1, mode == 10 ? 1 : (mode >= 8 ? mode & 1 : mode & 2),
                                     phase, pager_format, &master, &cancel_fd);
        if (pid < 0 || wait_for_exit(pid) != 0 || wait_for_output(master, &out, 40) != 0 ||
            !terminal_is_cooked(master)) goto done;
        if (phase == 2 || phase == 4 || phase == 5) {
            if (require_contains(&out, "\033[?1049h") != 0 || require_contains(&out, "\033[?1049l") != 0 ||
                strstr(out.data, "\033[H") != NULL) goto done;
        } else if (out.len != 0) goto done;
        close(cancel_fd);
        close(master);
        clear_capture(&out);
    }

    if (argc == 3) { rc = 0; goto done; }
    stage = "text startup";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        wait_for_marker(master, &out, "\033[7m\033[1;32m") != 0 ||
        require_contains(&out, "# raw heading") != 0 ||
        require_contains(&out, "\033[7m\033[1;32m ") != 0 ||
        require_full_status_bar(&out, text_path) != 0 ||
        strstr(out.data, "\033[1;32m# raw heading") != NULL) goto done;
    clear_capture(&out);
    stage = "down";
    if (write_all(master, "j", 1) != 0 || wait_for_marker(master, &out, "line-08") != 0) goto done;
    clear_capture(&out);
    stage = "up";
    if (write_all(master, "k", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "page down";
    if (write_all(master, "\033[6~", 4) != 0 || wait_for_marker(master, &out, "line-10") != 0) goto done;
    clear_capture(&out);
    stage = "page up";
    if (write_all(master, "\033[5~", 4) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "half down";
    if (write_all(master, "\004", 1) != 0 || wait_for_marker(master, &out, "line-05") != 0) goto done;
    clear_capture(&out);
    stage = "half up";
    if (write_all(master, "\025", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "space page down";
    if (write_all(master, " ", 1) != 0 || wait_for_marker(master, &out, "line-08") != 0) goto done;
    clear_capture(&out);
    stage = "b page up";
    if (write_all(master, "b", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "ctrl-f page down";
    if (write_all(master, "\006", 1) != 0 || wait_for_marker(master, &out, "line-08") != 0) goto done;
    clear_capture(&out);
    stage = "ctrl-b page up";
    if (write_all(master, "\002", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "d half down";
    if (write_all(master, "d", 1) != 0 || wait_for_marker(master, &out, "line-03") != 0) goto done;
    clear_capture(&out);
    stage = "u half up";
    if (write_all(master, "u", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "enter down";
    if (write_all(master, "\r", 1) != 0 || wait_for_marker(master, &out, "S\303\244kerhet") != 0) goto done;
    clear_capture(&out);
    stage = "g home";
    if (write_all(master, "g", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "G end";
    if (write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "line-60") != 0) goto done;
    clear_capture(&out);
    stage = "end";
    if (write_all(master, "\033[F", 3) != 0 || wait_for_redraw(master, &out, "line-60") != 0 ||
        require_contains(&out, "line-60") != 0) goto done;
    clear_capture(&out);
    stage = "home";
    if (write_all(master, "\033[H", 3) != 0 || wait_for_redraw(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "live Swedish search";
    if (write_all(master, "/S\303\204K", strlen("/S\303\204K")) != 0 ||
        wait_for_redraw(master, &out, "\033[7mS\303\244k\033[27m") != 0) goto done;
    clear_capture(&out);
    stage = "Swedish case-insensitive search";
    if (write_all(master, "ERHET", strlen("ERHET")) != 0 ||
        wait_for_redraw(master, &out, "\033[7mS\303\244kerhet\033[27m") != 0) goto done;
    clear_capture(&out);
    stage = "confirm Swedish search";
    if (write_all(master, "\r", 1) != 0 ||
        wait_for_redraw(master, &out, "/S\303\204KERHET  1/2") != 0 ||
        require_contains(&out, "\033[7mS\303\244kerhet\033[27m") != 0 ||
        require_contains(&out, "/S\303\204KERHET  1/2") != 0) goto done;
    clear_capture(&out);
    stage = "next Swedish search hit";
    if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "/S\303\204KERHET  2/2") != 0 ||
        require_contains(&out, "\033[7mS\303\204KERHET\033[27m") != 0 ||
        require_contains(&out, "/S\303\204KERHET  2/2") != 0) goto done;
    clear_capture(&out);
    stage = "previous Swedish search hit with N";
    if (write_all(master, "N", 1) != 0 || wait_for_redraw(master, &out, "/S\303\204KERHET  1/2") != 0 ||
        require_contains(&out, "/S\303\204KERHET  1/2") != 0) goto done;
    clear_capture(&out);
    stage = "previous Swedish search hit with p";
    if (write_all(master, "p", 1) != 0 || wait_for_redraw(master, &out, "/S\303\204KERHET  2/2") != 0 ||
        require_contains(&out, "/S\303\204KERHET  2/2") != 0) goto done;
    clear_capture(&out);
    stage = "exit Swedish search";
    if (write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "line-01") != 0 ||
        require_contains(&out, "line-01") != 0) goto done;
    clear_capture(&out);
    stage = "Japanese UTF-8 search";
    if (write_all(master, "/\346\227\245\346\234\254\350\252\236\r", strlen("/\346\227\245\346\234\254\350\252\236\r")) != 0 ||
        wait_for_redraw(master, &out, "/\346\227\245\346\234\254\350\252\236  1/2") != 0 ||
        require_contains(&out, "\033[7m\346\227\245\346\234\254\350\252\236\033[27m") != 0 ||
        require_contains(&out, "/\346\227\245\346\234\254\350\252\236  1/2") != 0) goto done;
    clear_capture(&out);
    stage = "next Japanese search hit";
    if (write_all(master, "n", 1) != 0 || wait_for_redraw(master, &out, "/\346\227\245\346\234\254\350\252\236  2/2") != 0 ||
        require_contains(&out, "/\346\227\245\346\234\254\350\252\236  2/2") != 0) goto done;
    clear_capture(&out);
    stage = "resize preserves Japanese search hit";
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 40;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out, "/\346\227\245\346\234\254\350\252\236  2/2") != 0 ||
        require_contains(&out, "/\346\227\245\346\234\254\350\252\236  2/2") != 0) goto done;
    clear_capture(&out);
    stage = "next Japanese search hit after resize";
    if (write_all(master, "n", 1) != 0 || wait_for_marker(master, &out, "/\346\227\245\346\234\254\350\252\236  1/2") != 0 ||
        require_contains(&out, "/\346\227\245\346\234\254\350\252\236  1/2") != 0) goto done;
    clear_capture(&out);
    stage = "narrow Japanese search status";
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 10;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out, "  1/2 ") != 0 ||
        require_contains(&out, "  1/2 ") != 0) goto done;
    clear_capture(&out);
    stage = "escape exits Japanese search";
    if (write_all(master, "\033", 1) != 0 || wait_for_redraw(master, &out, "line-01") != 0) goto done;
    stage = "escape";
    if (write_all(master, "\033", 1) != 0 || wait_for_exit(pid) != 0 ||
        wait_for_marker(master, &out, "\033[?1049l") != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "Unicode casefold search startup";
    pid = start_pager(argv[1], unicode_casefold_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "\316\221\316\222\316\223") != 0) goto done;
    clear_capture(&out);
    stage = "Greek Unicode case-insensitive search";
    if (write_all(master, "/\316\261\316\262\316\263\r", strlen("/\316\261\316\262\316\263\r")) != 0 ||
        wait_for_redraw(master, &out, "/\316\261\316\262\316\263  1/1") != 0 ||
        require_contains(&out, "\033[7m\316\221\316\222\316\223\033[27m") != 0 ||
        require_contains(&out, "/\316\261\316\262\316\263  1/1") != 0) goto done;
    clear_capture(&out);
    stage = "exit Greek search";
    if (write_all(master, "q", 1) != 0 ||
        wait_for_redraw(master, &out, "\316\221\316\222\316\223") != 0) goto done;
    clear_capture(&out);
    stage = "quit Greek search pager";
    if (write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "UTF-8 search startup";
    pid = start_pager(argv[1], utf8_search_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "abc") != 0) goto done;
    stage = "ASCII UTF-8 search";
    if (write_all(master, "/abc", 4) != 0 ||
        wait_for_redraw(master, &out, "\033[7mabc\033[27m") != 0) goto done;
    clear_capture(&out);
    stage = "incomplete UTF-8 search first byte";
    if (write_all(master, "\346q", 2) != 0) goto done;
    if (wait_for_redraw(master, &out, "abc") != 0) {
        stage = "incomplete UTF-8 search first-byte cancellation redraw";
        goto done;
    }
    if (capture_contains(&out, "\033[7mabc")) {
        stage = "incomplete UTF-8 search first-byte partial redraw";
        goto done;
    }
    clear_capture(&out);
    stage = "incomplete UTF-8 search second byte";
    if (write_all(master, "/abc", 4) != 0 ||
        wait_for_redraw(master, &out, "\033[7mabc\033[27m") != 0) goto done;
    clear_capture(&out);
    if (write_all(master, "\346\227q", 3) != 0) goto done;
    if (wait_for_redraw(master, &out, "abc") != 0) {
        stage = "incomplete UTF-8 search second-byte cancellation redraw";
        goto done;
    }
    if (capture_contains(&out, "\033[7mabc")) {
        stage = "incomplete UTF-8 search second-byte partial redraw";
        goto done;
    }
    clear_capture(&out);
    stage = "complete UTF-8 search";
    if (write_all(master, "/abc\346\227\245", strlen("/abc\346\227\245")) != 0 ||
        wait_for_redraw(master, &out, "\033[7mabc\346\227\245\033[27m") != 0 ||
        require_contains(&out, "\033[7mabc\346\227\245\033[27m") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_redraw(master, &out, "abc") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown direct";
    pid = start_pager(argv[1], markdown_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        require_contains(&out, "\033[1;32m# ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown owned emission buffer";
    pid = start_pager_with_owned_emission_buffer(markdown_path, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 20;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_redraw(master, &out, "rendered heading") != 0 ||
        require_contains(&out, "rendered heading") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown fixed owned emission buffer resize";
    pid = start_pager_with_fixed_owned_emission_buffer(long_word_path, 20, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "abcdefghijklmnopqrst") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 80;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_failure(pid) != 0 || wait_for_marker(master, &out, "\033[?1049l") != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "text invalid theme";
    pid = start_pager_with_theme(text_path, "not-a-theme", &master);
    if (pid < 0 || wait_for_failure(pid) != 0 || wait_for_output(master, &out, 40) != 0 || out.len != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "nonregular file";
    pid = start_pager(argv[1], fifo_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_failure(pid) != 0 || wait_for_output(master, &out, 40) != 0 || out.len != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "missing regular file";
    pid = start_pager_with_expected_status(missing_path, MDF_ERROR_IO, &master);
    if (pid < 0 || wait_for_exit(pid) != 0 || wait_for_output(master, &out, 40) != 0 || out.len != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "pager source input allocation failure";
    pid = start_pager_source_with_oom(&master);
    if (pid < 0 || wait_for_exit(pid) != 0 || wait_for_output(master, &out, 40) != 0 || out.len != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "pager text view allocation failure";
    pid = start_pager_source_with_view_oom(0, &master);
    if (pid < 0 || wait_for_exit(pid) != 0 || wait_for_marker(master, &out, "\033[?1049l") != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "pager markdown sanitizer allocation failure";
    pid = start_pager_source_with_view_oom(1, &master);
    if (pid < 0 || wait_for_exit(pid) != 0 || wait_for_marker(master, &out, "\033[?1049l") != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown NUL file";
    pid = start_pager(argv[1], nul_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "Before NUL") != 0 ||
        wait_for_marker(master, &out, "After NUL") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    {
        static const char source_markdown[] = "# Before source NUL\n\n\0After source NUL\n";

        stage = "markdown source";
        pid = start_pager_source("stream.md", source_markdown, sizeof(source_markdown) - 1, &master);
        if (pid < 0 || wait_for_marker(master, &out, "Before source NUL") != 0 ||
            wait_for_marker(master, &out, "After source NUL") != 0 ||
            write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
        close(master);
        clear_capture(&out);
    }

    stage = "write trace rejected";
    pid = start_pager(argv[1], markdown_path, 1, 0, 1, &master);
    if (pid < 0 || wait_for_failure(pid) != 0 || wait_for_output(master, &out, 40) != 0 || out.len != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown cmdf";
    pid = start_pager(argv[1], markdown_path, 0, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        require_contains(&out, "\033[1;32m# ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    /* --incremental completes the document through the normal decision sink
     * before the finite pager consumes its rendered ANSI. The screen must keep
     * the ANSI heading rather than displaying escape bytes as plain text. */
    stage = "incremental markdown cmdf";
    pid = start_incremental_pager(argv[1], markdown_path, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        require_contains(&out, "\033[1;32m# ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    /* The 20-column table has distinct cell widths and borders. Its complete
     * narrow layout proves SIGWINCH drives a fresh incremental Markdown render,
     * rather than reflowing the ANSI emitted at 40 columns. */
    stage = "incremental markdown resize rerender";
    pid = start_incremental_pager(argv[1], incremental_resize_path, &master);
    if (pid < 0 || wait_for_marker(master, &out, "first") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 20;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_redraw(master, &out,
                        "\342\224\214\342\224\200\342\224\200\342\224\200\342\224\200"
                        "\342\224\200\342\224\200\342\224\200\342\224\200\342\224\254"
                        "\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200"
                        "\342\224\200\342\224\200\342\224\200\342\224\200\342\224\220") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "incremental text cmdf";
    pid = start_incremental_pager(argv[1], text_path, &master);
    if (pid < 0 || wait_for_marker(master, &out, "left **literal** right") != 0 ||
        strstr(out.data, "\033[1;32m# raw heading") != NULL ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "incremental nonregular file";
    pid = start_incremental_pager(argv[1], fifo_path, &master);
    if (pid < 0 || wait_for_failure(pid) != 0 ||
        wait_for_marker(master, &out, "--pager requires a named regular input file") != 0 ||
        capture_contains(&out, "\033[?1049h")) goto done;
    close(master);
    clear_capture(&out);

    stage = "osc8 long URL";
    pid = start_pager(argv[1], osc8_path, 1, 1, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "molnpolicy") != 0 ||
        wait_for_redraw(master, &out, "molnpolicy") != 0 ||
        require_contains(&out, "\033]8;;https://www.regeringen.se/contentassets/112463c2b5404f84826545ba01967f28/en-molnpolicy-for-sverige--for-okad-sakerhet-effektivitet-och-innovation-i-den-offentliga-forvaltningen1.pdf\033\\") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "osc8 narrow reflow";
    pid = start_pager(argv[1], osc8_reflow_path, 1, 1, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "abc") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 1;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out, "a\033]8;;\033\\\033[0m\r\n") != 0 ||
        require_contains(&out, "a\033]8;;\033\\\033[0m\r\n") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "styled narrow reflow";
    pid = start_pager_at_size(argv[1], styled_path, 1, 0, 0, 16, 10, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "foo ") != 0 ||
        require_contains(&out, "baz\033[0m\033[K\r\n") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 2;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_redraw(master, &out, "\033[35mba") != 0 ||
        require_contains(&out, "\033[35mr\033[0m") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "resize";
    pid = start_pager(argv[1], markdown_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "rendered heading") != 0) goto done;
    clear_capture(&out);
    if (write_all(master, "\033[F", 3) != 0 ||
        wait_for_redraw(master, &out, "line-52") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 20;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0) goto done;
    for (i = 0; i < 3; i++) {
        if (kill(pid, SIGWINCH) != 0) goto done;
        if (i + 1 < 3) sleep_ms(50);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &resize_started) != 0) goto done;
    stage = "resize settled redraw";
    if (wait_for_redraw(master, &out, "line-52") != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &resize_finished) != 0 ||
        elapsed_milliseconds(&resize_started, &resize_finished) < 250 ||
        count_occurrences(out.data, "\033[H") != 1 ||
        require_contains(&out, "line-52") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "vertical resize clamps viewport";
    pid = start_pager_at_size(argv[1], text_path, 1, 0, 0, 40, 4, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "# raw heading") != 0 ||
        write_all(master, "G", 1) != 0 || wait_for_redraw(master, &out, "line-60") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 40;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_redraw(master, &out, "line-52") != 0 ||
        require_contains(&out, "line-60") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "resize interrupts escape sequence";
    pid = start_pager(argv[1], markdown_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "rendered heading") != 0) goto done;
    clear_capture(&out);
    /* Send ESC first, interrupt the bounded sequence read, then complete Page
     * Down. The partially consumed escape sequence must survive SIGWINCH. */
    if (write_all(master, "\033", 1) != 0) goto done;
    sleep_ms(5);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 20;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        write_all(master, "[6~", 3) != 0 ||
        wait_for_redraw(master, &out, "line-07") != 0 ||
        capture_contains(&out, "\033[?1049l") ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "initial resize redraw";
    pid = start_pager(argv[1], initial_resize_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "\033[?1049h\033[?25l") != 0) goto done;
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 20;
    resized.ws_row = 10;
    /* The alternate-screen marker is emitted after SIGWINCH is installed and
     * immediately before the first view is built.  Resize from that observed
     * protocol point instead of racing a fixed burst against slow sanitizers. */
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0) goto done;
    if (wait_for_redraw(master, &out, "\033[H01234567890123456789\033[0m\r\n") != 0 ||
        require_contains(&out, "\033[H01234567890123456789\033[0m\r\n") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "narrow status bar";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 1;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out, "\033[H") != 0 || out.len > 16384 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "narrow markdown";
    pid = start_pager(argv[1], markdown_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 1;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out, "\033[H") != 0 || out.len > 16384 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "wide utf8 reflow";
    pid = start_pager(argv[1], wide_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "\346\227\245\346\234\254\350\252\236\346\227\245\346\234\254\350\252\236") != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 4;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out,
                        "\346\227\245\346\234\254\033[0m\r\n\350\252\236\346\227\245\033[0m\r\n\346\234\254\350\252\236") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "wide UTF-8 tab reflow";
    pid = start_pager_at_size(argv[1], wide_tab_path, 1, 0, 0, 1, 10, &master);
    if (pid < 0 ||
        wait_for_redraw(master, &out,
                        "\346\227\245\033[0m\r\n \033[0m\r\n") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "Markdown controls";
    pid = start_pager(argv[1], control_markdown_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "^[]52;c;INJECT^G") != 0 ||
        wait_for_marker(master, &out, "M-^]52") != 0 ||
        strstr(out.data, "\033]52;c;INJECT\a") != NULL ||
        strstr(out.data, "\23552;c;C1\a") != NULL ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "incremental Markdown controls";
    pid = start_incremental_pager(argv[1], control_markdown_path, &master);
    if (pid < 0 || wait_for_marker(master, &out, "^[]52;c;INJECT^G") != 0 ||
        wait_for_marker(master, &out, "M-^]52") != 0 ||
        strstr(out.data, "\033]52;c;INJECT\a") != NULL ||
        strstr(out.data, "\23552;c;C1\a") != NULL ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "C1 text control";
    pid = start_pager(argv[1], c1_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "M-^]52;c;INJECT^G") != 0 ||
        strstr(out.data, "\23552;c;INJECT\a") != NULL ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "text carriage returns";
    pid = start_pager(argv[1], carriage_return_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "crlf line") != 0 ||
        wait_for_marker(master, &out, "standalone^Mreturn") != 0 ||
        strstr(out.data, "crlf line^M") != NULL ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "control filename status";
    /* Use the basename so the filename's controls remain inside the narrow
     * status bar even when the owned build directory has a long path. */
    pid = start_pager(argv[1], strrchr(control_path, '/') + 1, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "status fixture") != 0 ||
        wait_for_marker(master, &out, "^[]52;c") != 0 ||
        require_contains(&out, "^[]52;c") != 0 ||
        strstr(out.data, "\033]52;c;INJECT\a") != NULL ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "unicode filename status";
    pid = start_pager(argv[1], strrchr(unicode_path, '/') + 1, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "status fixture") != 0 ||
        wait_for_marker(master, &out, "\346\227\245\346\234\254\350\252\236\346\227\245") != 0 ||
        require_contains(&out, "\346\227\245\346\234\254\350\252\236\346\227\245") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "styled search reset";
    pid = start_pager(argv[1], styled_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "foo ") != 0 ||
        clear_capture(&out) != 0 ||
        write_all(master, "/bar", strlen("/bar")) != 0 ||
        wait_for_redraw(master, &out, "\033[7mbar\033[27m") != 0 ||
        clear_capture(&out) != 0 ||
        write_all(master, " baz\r", strlen(" baz\r")) != 0 ||
        wait_for_redraw(master, &out, "bar\033[0m\033[7m baz") != 0) goto done;
    clear_capture(&out);
    if (write_all(master, "q", 1) != 0 || wait_for_marker(master, &out, "foo ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "custom allocator search routing";
    route_allocator = (pager_route_allocator *)mmap(NULL, sizeof(*route_allocator),
                                                     PROT_READ | PROT_WRITE,
                                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (route_allocator == MAP_FAILED) goto done;
    memset(route_allocator, 0, sizeof(*route_allocator));
    pid = start_pager_with_allocator_route_probe(route_allocator, &master);
    if (pid < 0 || wait_for_redraw(master, &out, "abc abc") != 0) goto done;
    stage = "custom allocator initial search";
    if (clear_capture(&out) != 0 || write_all(master, "/abc\r", strlen("/abc\r")) != 0 ||
        wait_for_redraw(master, &out, "\033[7mabc\033[27m") != 0) goto done;
    stage = "custom allocator search reset";
    if (clear_capture(&out) != 0 || write_all(master, "q", 1) != 0 ||
        wait_for_redraw(master, &out, "abc abc") != 0) goto done;
    stage = "custom allocator next query";
    if (clear_capture(&out) != 0 || write_all(master, "/", 1) != 0 ||
        wait_for_marker(master, &out, "  /") != 0) goto done;
    stage = "custom allocator enforced query";
    route_allocator->fail_allocations = 1;
    if (write_all(master, "zqq", 3) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);
    if (munmap(route_allocator, sizeof(*route_allocator)) != 0) goto done;
    route_allocator = MAP_FAILED;

    stage = "termination handler setup cleanup";
    route_allocator = (pager_route_allocator *)mmap(NULL, sizeof(*route_allocator),
                                                     PROT_READ | PROT_WRITE,
                                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (route_allocator == MAP_FAILED) goto done;
    memset(route_allocator, 0, sizeof(*route_allocator));
    pid = start_pager_with_handler_install_failure(route_allocator, &master);
    if (pid < 0 || wait_for_exit(pid) != 0 || wait_for_output(master, &out, 40) != 0 || out.len != 0) goto done;
    close(master);
    clear_capture(&out);
    if (munmap(route_allocator, sizeof(*route_allocator)) != 0) goto done;
    route_allocator = MAP_FAILED;

    stage = "SIGTERM cleanup";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        kill(pid, SIGTERM) != 0 || wait_for_signal(pid, SIGTERM) != 0 ||
        wait_for_marker(master, &out, "\033[?1049l") != 0 ||
        !terminal_is_cooked(master)) goto done;
    close(master);
    clear_capture(&out);

    stage = "ctrl-c cleanup";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        write_all(master, "\003", 1) != 0 || wait_for_signal(pid, SIGINT) != 0 ||
        wait_for_marker(master, &out, "\033[?1049l") != 0 ||
        !terminal_is_cooked(master)) goto done;
    close(master);
    clear_capture(&out);

    stage = "SIGTSTP cleanup";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        kill(pid, SIGTSTP) != 0 || wait_for_failure(pid) != 0 ||
        wait_for_marker(master, &out, "\033[?1049l") != 0 ||
        !terminal_is_cooked(master)) goto done;
    close(master);
    rc = 0;

done:
    if (rc != 0) {
        fprintf(stderr, "test_pager failed at %s\n", stage);
        if (out.len > 0) fwrite(out.data, 1, out.len, stderr);
    }
    unlink(text_path);
    unlink(markdown_path);
    unlink(osc8_path);
    unlink(osc8_reflow_path);
    unlink(wide_path);
    unlink(wide_tab_path);
    unlink(utf8_search_path);
    unlink(unicode_casefold_path);
    unlink(initial_resize_path);
    unlink(incremental_resize_path);
    unlink(control_path);
    unlink(unicode_path);
    unlink(styled_path);
    unlink(long_word_path);
    unlink(nul_path);
    unlink(fifo_path);
    unlink(c1_path);
    unlink(carriage_return_path);
    unlink(control_markdown_path);
    unlink(missing_path);
    if (route_allocator != MAP_FAILED) (void)munmap(route_allocator, sizeof(*route_allocator));
    free(out.data);
    return rc;
}
