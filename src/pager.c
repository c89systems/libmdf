/**
 * @file pager.c
 * @brief Interactive Markdown/text paging with bounded rendered-content caches.
 *
 * Named files and seekable callbacks are consumed on demand; evicted passages
 * are recovered by cancellable replay. Search uses bounded state instead of a
 * document-wide match list. The forward-only compatibility entry point buffers
 * a finite input. All exits restore terminal and signal state.
 */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "mdf_internal.h"
#include "pager_query.h"
#include "render_internal.h"
#include "unicode_casefold.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MDF_PAGER_RESIZE_DEBOUNCE_MS 250L
#define MDF_PAGER_TERMINATION_SIGNAL_COUNT 7
#define MDF_PAGER_MIN_RENDER_WIDTH 3
#define MDF_PAGER_DEFAULT_CACHE (1024u * 1024u)
#define MDF_PAGER_MIN_CACHE 65536u
#define MDF_PAGER_POSITION_MAX ((uint64_t)-1)

typedef struct mdf_pager_cancel_state {
    const mdf_pager_control *control;
    size_t work;
    int requested;
} mdf_pager_cancel_state;

static int mdf_pager_cancel_check(mdf_pager_cancel_state *cancel)
{
    if (cancel == NULL) return 0;
    if (!cancel->requested && cancel->control != NULL && cancel->control->should_cancel != NULL)
        cancel->requested = cancel->control->should_cancel(cancel->control->userdata) != 0;
    return cancel->requested;
}

/* Bound callback overhead while still checking CPU-bound view/search work. */
static int mdf_pager_cancel_work(mdf_pager_cancel_state *cancel, size_t amount)
{
    if (cancel == NULL) return 0;
    if (cancel->requested) return 1;
    if (cancel->control == NULL || cancel->control->should_cancel == NULL) return 0;
    if (amount < 4096 && cancel->work < 4096 - amount) {
        cancel->work += amount;
        return 0;
    }
    cancel->work = 0;
    return mdf_pager_cancel_check(cancel);
}

typedef struct mdf_pager_buffer {
    char *data;
    size_t len;
    size_t cap;
    mdf_allocator allocator;
    mdf_pager_cancel_state *cancel;
    size_t limit;
    int discard;
    int (*make_room)(void *userdata, size_t len);
    void *room_userdata;
} mdf_pager_buffer;

typedef struct mdf_pager_buffer_source {
    const mdf_pager_buffer *buffer;
    size_t offset;
} mdf_pager_buffer_source;

typedef struct mdf_pager_anchor {
    mdf_pager_buffer text;
    size_t prefix;
    uint64_t source_offset;
    uint64_t text_offset;
    uint64_t replay_text_offset;
    int replayed;
    int complete_cache;
} mdf_pager_anchor;

/* A row's first visible byte belongs to one input feed. Its text position is
 * relative to that feed, independently of cache size and earlier reflow. */
typedef struct mdf_pager_origin {
    uint64_t source_offset;
    uint64_t text_offset;
} mdf_pager_origin;

typedef struct mdf_pager_markdown_sanitizer {
    char pending_utf8[4];
    size_t pending_utf8_len;
} mdf_pager_markdown_sanitizer;

typedef struct mdf_pager_search mdf_pager_search;

typedef struct mdf_pager_view {
    mdf_allocator allocator;
    mdf_pager_buffer bytes;
    mdf_pager_buffer searchable;
    size_t *search_offsets;
    size_t search_offset_cap;
    size_t *lines;
    size_t line_count;
    size_t line_cap;
    size_t *soft_wraps;
    size_t soft_wrap_len;
    size_t soft_wrap_cap;
    uint64_t first_line;
    uint64_t next_line;
    uint64_t collect_from;
    uint64_t total_lines;
    uint64_t protected_first;
    size_t byte_limit;
    size_t row_start;
    int full;
    int row_too_large;
    int eof_known;
    uint64_t input_offset;
    uint64_t input_text_offset;
    uint64_t text_count;
    uint64_t retained_text_end;
    uint64_t row_text_start;
    mdf_pager_origin row_origin;
    mdf_pager_origin *origins;
    size_t origin_count;
    size_t origin_cap;
    mdf_pager_query *query;
    mdf_pager_search *search;
    size_t query_column;
    uint64_t last_match;
    uint64_t completed_last_match;
    int match_prefix_known;
} mdf_pager_view;

typedef struct mdf_pager_reflow_state {
    mdf_pager_view *view;
    mdf_pager_buffer osc8_start;
    mdf_pager_buffer sgr_start;
    int width;
    int styled;
    int col;
    int osc8_active;
} mdf_pager_reflow_state;

typedef struct mdf_pager_match {
    size_t start;
    size_t end;
} mdf_pager_match;

struct mdf_pager_search {
    mdf_allocator allocator;
    mdf_pager_buffer query;
    mdf_pager_match *matches;
    size_t count;
    size_t cap;
    size_t selected;
    int editing;
    int active;
    mdf_pager_query scan;
    mdf_pager_query_match selection;
    mdf_pager_query_match candidate;
    mdf_pager_query_position origin;
    int have_selection;
    int have_candidate;
    int seeking;
    int backward;
    int wrapped;
    int inclusive;
};

static void mdf_pager_search_observe(mdf_pager_search *search)
{
    mdf_pager_query_match match;
    int order;
    int eligible;

    match = search->scan.last;
    if (search->have_selection &&
        mdf_pager_query_position_compare(match.start, search->selection.start) == 0 && match.ordinal != 0)
        search->selection.ordinal = match.ordinal;
    if (!search->seeking) return;
    order = mdf_pager_query_position_compare(match.start, search->origin);
    eligible = search->backward ? (search->wrapped ? order >= 0 : order < 0) :
        (search->wrapped ? order <= 0 : (order > 0 || (order == 0 && search->inclusive)));
    if (eligible && (!search->have_candidate || search->backward)) {
        search->candidate = match;
        search->have_candidate = 1;
    }
}

typedef enum mdf_pager_key {
    MDF_PAGER_KEY_NONE,
    MDF_PAGER_KEY_QUIT,
    MDF_PAGER_KEY_UP,
    MDF_PAGER_KEY_DOWN,
    MDF_PAGER_KEY_PAGE_UP,
    MDF_PAGER_KEY_PAGE_DOWN,
    MDF_PAGER_KEY_HOME,
    MDF_PAGER_KEY_END,
    MDF_PAGER_KEY_HALF_UP,
    MDF_PAGER_KEY_HALF_DOWN
} mdf_pager_key;

#define MDF_PAGER_READ_INTERRUPTED (-2)

typedef struct mdf_pager_termination_handlers {
    struct sigaction old_actions[MDF_PAGER_TERMINATION_SIGNAL_COUNT];
    size_t count;
} mdf_pager_termination_handlers;

static const int mdf_pager_termination_signals[MDF_PAGER_TERMINATION_SIGNAL_COUNT] = {
    SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU
};

static volatile sig_atomic_t mdf_pager_winch_count;
static volatile sig_atomic_t mdf_pager_termination_signal;

#ifdef LIBMDF_PAGER_TESTING
static volatile sig_atomic_t mdf_pager_test_fail_termination_handler_install;
static int mdf_pager_test_small_cache;

/* Exercise real content-cache seams below the public configuration floor. */
void mdf_pager_test_allow_small_cache(int enabled)
{
    mdf_pager_test_small_cache = enabled;
}

/* Test-only fault injection keeps the cleanup path deterministically
 * observable without changing the public pager API or relying on host signal
 * policy.  This symbol is compiled only into test builds. */
void mdf_pager_test_set_termination_handler_install_failure(int enabled)
{
    mdf_pager_test_fail_termination_handler_install = enabled ? 1 : 0;
}
#endif

static void mdf_pager_on_winch(int signal_number)
{
    (void)signal_number;
    mdf_pager_winch_count++;
}

static void mdf_pager_on_termination(int signal_number)
{
    if (mdf_pager_termination_signal == 0) {
        mdf_pager_termination_signal = signal_number;
    }
}

static int mdf_pager_install_termination_handlers(mdf_pager_termination_handlers *handlers)
{
    struct sigaction action;
    size_t i;

#ifdef LIBMDF_PAGER_TESTING
    if (mdf_pager_test_fail_termination_handler_install) {
        errno = EPERM;
        return -1;
    }
#endif
    memset(handlers, 0, sizeof(*handlers));
    memset(&action, 0, sizeof(action));
    action.sa_handler = mdf_pager_on_termination;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < MDF_PAGER_TERMINATION_SIGNAL_COUNT; i++) {
        if (sigaction(mdf_pager_termination_signals[i], &action, &handlers->old_actions[i]) != 0) {
            while (i > 0) {
                i--;
                (void)sigaction(mdf_pager_termination_signals[i], &handlers->old_actions[i], NULL);
            }
            return -1;
        }
        handlers->count++;
    }
    return 0;
}

static void mdf_pager_restore_termination_handlers(const mdf_pager_termination_handlers *handlers)
{
    size_t i;

    for (i = 0; i < handlers->count; i++) {
        (void)sigaction(mdf_pager_termination_signals[i], &handlers->old_actions[i], NULL);
    }
}

/* Keep the pager handlers from observing a signal after cleanup has chosen
 * which one to re-raise.  Signals arriving while this mask is active remain
 * pending and are delivered to the restored caller handlers on unmask. */
static int mdf_pager_block_termination_signals(sigset_t *old_mask)
{
    sigset_t signals;
    size_t i;

    sigemptyset(&signals);
    for (i = 0; i < MDF_PAGER_TERMINATION_SIGNAL_COUNT; i++) {
        if (sigaddset(&signals, mdf_pager_termination_signals[i]) != 0) return -1;
    }
    return sigprocmask(SIG_BLOCK, &signals, old_mask);
}

static int mdf_pager_now(struct timespec *now)
{
    return clock_gettime(CLOCK_MONOTONIC, now) == 0 ? 0 : -1;
}

static long mdf_pager_resize_wait_ms(const struct timespec *last_winch)
{
    struct timespec now;
    uintmax_t seconds;
    long elapsed_ns;
    long remaining_ns;

    if (mdf_pager_now(&now) != 0 || now.tv_sec < last_winch->tv_sec) return 0;
    seconds = (uintmax_t)now.tv_sec - (uintmax_t)last_winch->tv_sec;
    if (seconds > 1) return 0;
    if (seconds == 0) {
        if (now.tv_nsec < last_winch->tv_nsec) return 0;
        elapsed_ns = now.tv_nsec - last_winch->tv_nsec;
    } else {
        elapsed_ns = 1000000000L - last_winch->tv_nsec + now.tv_nsec;
    }
    if (elapsed_ns >= MDF_PAGER_RESIZE_DEBOUNCE_MS * 1000000L) return 0;
    remaining_ns = MDF_PAGER_RESIZE_DEBOUNCE_MS * 1000000L - elapsed_ns;
    return (remaining_ns + 999999L) / 1000000L;
}

static void mdf_pager_note_resize(sig_atomic_t *seen_winch, int *resize_pending,
                                  struct timespec *last_winch)
{
    *seen_winch = mdf_pager_winch_count;
    *resize_pending = 1;
    if (mdf_pager_now(last_winch) != 0) {
        last_winch->tv_sec = 0;
        last_winch->tv_nsec = 0;
    }
}

static void mdf_pager_buffer_destroy(mdf_pager_buffer *buffer)
{
    mdf_free_mem(&buffer->allocator, buffer->data, buffer->cap);
    memset(buffer, 0, sizeof(*buffer));
}

static void mdf_pager_buffer_init_with_allocator(mdf_pager_buffer *buffer,
                                                  const mdf_allocator *allocator)
{
    memset(buffer, 0, sizeof(*buffer));
    if (allocator != NULL) {
        buffer->allocator = *allocator;
    }
    mdf_allocator_normalize(&buffer->allocator);
}

static void mdf_pager_buffer_init(mdf_pager_buffer *buffer, const mdf_options *opts)
{
    mdf_pager_buffer_init_with_allocator(buffer, opts == NULL ? NULL : &opts->allocator);
}

/* Geometric spare capacity must not exceed the cache's useful entry count.
 * Otherwise a budget just above a power of two doubles several dense indexes
 * at once, exhausting their workspace even though all content fits. */
static void *mdf_pager_array_grow(mdf_allocator *allocator, void *ptr, size_t *capacity,
                                  size_t initial, size_t required, size_t limit, size_t element_size)
{
    size_t next_count;
    void *next;

    if (required > limit) return NULL;
    next_count = *capacity == 0 ? initial : *capacity;
    if (next_count > limit) next_count = limit;
    while (next_count < required) {
        if (next_count > limit / 2) { next_count = limit; break; }
        next_count *= 2;
    }
    /* A fallback realloc holds old and new arrays simultaneously. Avoid a
     * second nearly full-size allocation just to add the last few entries. */
    if (next_count >= limit - limit / 4) next_count = limit;
    if (*capacity > (size_t)-1 / element_size || next_count > (size_t)-1 / element_size) return NULL;
    next = mdf_realloc_mem(allocator, ptr, *capacity * element_size, next_count * element_size);
    if (next != NULL) *capacity = next_count;
    return next;
}

static int mdf_pager_buffer_append(mdf_pager_buffer *buffer, const char *src, size_t len)
{
    size_t required;
    size_t next_cap;
    char *next;

    if (mdf_pager_cancel_work(buffer->cancel, len)) return -1;
    if (buffer->discard) return 0;
    if (buffer->make_room != NULL && buffer->make_room(buffer->room_userdata, len) != 0) return -1;
    if (buffer->discard) return 0;
    if (len > (size_t)-1 - buffer->len - 1) {
        return -1;
    }
    required = buffer->len + len + 1;
    if (buffer->limit != 0 && required > buffer->limit) return -1;
    if (required > buffer->cap) {
        next_cap = buffer->cap == 0 ? 1024 : buffer->cap;
        while (next_cap < required) {
            if (next_cap > (size_t)-1 / 2) {
                next_cap = required;
                break;
            }
            next_cap *= 2;
        }
        if (buffer->limit != 0 && next_cap > buffer->limit) next_cap = buffer->limit;
        next = (char *)mdf_realloc_mem(&buffer->allocator, buffer->data, buffer->cap, next_cap);
        if (next == NULL) {
            return -1;
        }
        buffer->data = next;
        buffer->cap = next_cap;
    }
    if (len > 0) {
        memcpy(buffer->data + buffer->len, src, len);
        buffer->len += len;
    }
    buffer->data[buffer->len] = '\0';
    return 0;
}

static int mdf_pager_buffer_append_byte(mdf_pager_buffer *buffer, unsigned char byte)
{
    char c;

    c = (char)byte;
    return mdf_pager_buffer_append(buffer, &c, 1);
}

static mdf_status mdf_pager_read_source(mdf_source *source, mdf_pager_buffer *out)
{
    char chunk[4096];

    for (;;) {
        int err;
        size_t n;

        err = 0;
        if (mdf_pager_cancel_check(out->cancel)) return MDF_CANCELLED;
        n = source->read(source->userdata, chunk, sizeof(chunk), &err);
        if (mdf_pager_cancel_check(out->cancel)) return MDF_CANCELLED;
        if (n > sizeof(chunk) || err != 0) return MDF_ERROR_IO;
        if (n == 0) return MDF_OK;
        if (mdf_pager_buffer_append(out, chunk, n) != 0) return MDF_ERROR_NOMEM;
    }
}

static size_t mdf_pager_buffer_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    mdf_pager_buffer_source *source;
    size_t n;

    source = (mdf_pager_buffer_source *)userdata;
    if (mdf_pager_cancel_check(source->buffer->cancel)) {
        *err = 1;
        return 0;
    }
    if (source->offset >= source->buffer->len) {
        *err = 0;
        return 0;
    }
    n = source->buffer->len - source->offset;
    if (n > cap) n = cap;
    memcpy(dst, source->buffer->data + source->offset, n);
    source->offset += n;
    *err = 0;
    return n;
}

static int mdf_pager_is_markdown_path(const char *path)
{
    const char *dot;

    dot = strrchr(path, '.');
    if (dot == NULL || dot[1] == '\0' || dot[2] == '\0' || dot[3] != '\0') {
        return 0;
    }
    return (dot[1] == 'm' || dot[1] == 'M') && (dot[2] == 'd' || dot[2] == 'D');
}

static void mdf_pager_retain_emission_buffer(mdf_options *opts, mdf *renderer)
{
    mdf_impl *impl;

    if (opts == NULL || renderer == NULL || renderer->impl == NULL ||
        opts->emission_buffer.data == NULL || !opts->emission_buffer.take_ownership) {
        return;
    }
    impl = (mdf_impl *)renderer->impl;
    if (!impl->emit_owns_buf) {
        return;
    }
    opts->emission_buffer.data = impl->emit_buf;
    opts->emission_buffer.cap = impl->emit_cap;
    opts->emission_buffer.fixed = impl->emit_fixed_mode;
    impl->emit_owns_buf = 0;
    impl->emit_workspace_owned = 0;
}

static void mdf_pager_release_emission_buffer(mdf_options *opts)
{
    if (opts == NULL || opts->emission_buffer.data == NULL ||
        !opts->emission_buffer.take_ownership) {
        return;
    }
    mdf_free_mem(&opts->allocator, opts->emission_buffer.data, opts->emission_buffer.cap);
    memset(&opts->emission_buffer, 0, sizeof(opts->emission_buffer));
}

static int mdf_pager_arguments_valid(const char *name, const mdf_options *render_options,
                                     mdf_pager_format format)
{
    return name != NULL && name[0] != '\0' &&
        (format == MDF_PAGER_FORMAT_AUTO || format == MDF_PAGER_FORMAT_TEXT ||
         format == MDF_PAGER_FORMAT_MARKDOWN) &&
        isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
        (render_options == NULL || render_options->write_trace.emit == NULL);
}

static mdf_status mdf_pager_validate_options(const mdf_options *render_options)
{
    mdf_options opts;
    mdf *renderer;
    mdf_status st;

    mdf_options_init(&opts);
    if (render_options != NULL) opts = *render_options;
    /* Validation must not consume a buffer the caller transfers to the pager. */
    opts.emission_buffer.take_ownership = 0;
    if (opts.margin_left >= 0 && opts.margin_right >= 0) {
        if (opts.margin_left > INT_MAX - opts.margin_right - MDF_PAGER_MIN_RENDER_WIDTH) {
            return MDF_ERROR_INVALID;
        }
        opts.width = opts.margin_left + opts.margin_right + MDF_PAGER_MIN_RENDER_WIDTH;
    } else {
        opts.width = MDF_PAGER_MIN_RENDER_WIDTH;
    }
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st == MDF_OK) renderer->destroy(renderer);
    return st;
}

static int mdf_pager_render_width(const mdf_options *opts, int width)
{
    int minimum_width;

    if (width < MDF_PAGER_MIN_RENDER_WIDTH) width = MDF_PAGER_MIN_RENDER_WIDTH;
    if (opts == NULL || opts->margin_left < 0 || opts->margin_right < 0) return width;
    if (opts->margin_left > INT_MAX - opts->margin_right - MDF_PAGER_MIN_RENDER_WIDTH) return width;
    minimum_width = opts->margin_left + opts->margin_right + MDF_PAGER_MIN_RENDER_WIDTH;
    return width >= minimum_width ? width : minimum_width;
}

static int mdf_pager_append_escape(mdf_pager_buffer *out, const char *src, size_t len, size_t *index)
{
    size_t i;

    i = *index;
    if (mdf_pager_buffer_append_byte(out, (unsigned char)src[i]) != 0) {
        return -1;
    }
    i++;
    if (i < len && src[i] == '[') {
        if (mdf_pager_buffer_append_byte(out, (unsigned char)src[i]) != 0) {
            return -1;
        }
        i++;
        while (i < len) {
            unsigned char byte;

            byte = (unsigned char)src[i];
            if (mdf_pager_buffer_append_byte(out, byte) != 0) {
                return -1;
            }
            i++;
            if (byte >= 0x40 && byte <= 0x7e) {
                break;
            }
        }
    } else if (i < len && src[i] == ']') {
        if (mdf_pager_buffer_append_byte(out, (unsigned char)src[i]) != 0) {
            return -1;
        }
        i++;
        while (i < len) {
            unsigned char byte;

            byte = (unsigned char)src[i];
            if (mdf_pager_buffer_append_byte(out, byte) != 0) {
                return -1;
            }
            i++;
            if (byte == '\a') {
                break;
            }
            if (byte == 0x1b && i < len && src[i] == '\\') {
                if (mdf_pager_buffer_append_byte(out, (unsigned char)src[i]) != 0) {
                    return -1;
                }
                i++;
                break;
            }
        }
    }
    *index = i;
    return 0;
}

static int mdf_pager_escape_is_osc8_start(const char *src, size_t len)
{
    static const char prefix[] = "\033]8;;";
    static const char close[] = "\033]8;;\033\\";

    return len > sizeof(close) - 1 && len >= sizeof(prefix) - 1 &&
        memcmp(src, prefix, sizeof(prefix) - 1) == 0;
}

static int mdf_pager_escape_is_osc8_close(const char *src, size_t len)
{
    static const char close[] = "\033]8;;\033\\";

    return len == sizeof(close) - 1 && memcmp(src, close, sizeof(close) - 1) == 0;
}

static int mdf_pager_escape_is_sgr(const char *src, size_t len)
{
    return len >= 3 && src[0] == '\033' && src[1] == '[' && src[len - 1] == 'm';
}

static int mdf_pager_sgr_has_nonreset_parameter(const char *src, size_t len)
{
    size_t i;
    int value;
    int have_value;

    if (!mdf_pager_escape_is_sgr(src, len)) return 0;
    value = 0;
    have_value = 0;
    for (i = 2; i + 1 < len; i++) {
        unsigned char byte;

        byte = (unsigned char)src[i];
        if (byte >= '0' && byte <= '9') {
            value = value * 10 + (int)(byte - '0');
            have_value = 1;
            continue;
        }
        if (byte == ';') {
            if (have_value && value != 0) return 1;
            value = 0;
            have_value = 0;
            continue;
        }
        return 1;
    }
    return have_value && value != 0;
}

static int mdf_pager_sgr_has_reset_parameter(const char *src, size_t len)
{
    size_t i;
    int value;
    int have_value;

    if (!mdf_pager_escape_is_sgr(src, len)) return 0;
    if (len == 3) return 1;
    value = 0;
    have_value = 0;
    for (i = 2; i + 1 < len; i++) {
        unsigned char byte;

        byte = (unsigned char)src[i];
        if (byte >= '0' && byte <= '9') {
            value = value * 10 + (int)(byte - '0');
            have_value = 1;
            continue;
        }
        if (byte == ';') {
            if (!have_value || value == 0) return 1;
            value = 0;
            have_value = 0;
            continue;
        }
        return 0;
    }
    return !have_value || value == 0;
}

static int mdf_pager_reflow_track_escape(mdf_pager_reflow_state *reflow, const char *src, size_t len)
{
    if (mdf_pager_escape_is_sgr(src, len)) {
        if (mdf_pager_sgr_has_reset_parameter(src, len)) {
            reflow->sgr_start.len = 0;
        }
        if (mdf_pager_sgr_has_nonreset_parameter(src, len) &&
            mdf_pager_buffer_append(&reflow->sgr_start, src, len) != 0) {
            return -1;
        }
    }
    if (mdf_pager_escape_is_osc8_close(src, len)) {
        reflow->osc8_active = 0;
        return 0;
    }
    if (mdf_pager_escape_is_osc8_start(src, len)) {
        reflow->osc8_start.len = 0;
        if (mdf_pager_buffer_append(&reflow->osc8_start, src, len) != 0) return -1;
        reflow->osc8_active = 1;
    }
    return 0;
}

/* Count logical visible text even when its display rows have been evicted. */
static int mdf_pager_query_visible(mdf_pager_view *view, unsigned long codepoint,
                                  uint64_t row, int retained)
{
    int result;

    if (view->query == NULL || view->query->length == 0) return 0;
    result = mdf_pager_query_push(view->query, codepoint, row, view->query_column++);
    if (result < 0) return -1;
    if (result != 0) mdf_pager_search_observe(view->search);
    if (result != 0 && retained && view->query->last.start.row >= view->first_line) {
        view->last_match = view->query->count;
        view->match_prefix_known = view->query->prefix_known;
    }
    return 0;
}

/* An emission can contain many rows. Retain only complete cache rows. */
static int mdf_pager_next_row(mdf_pager_reflow_state *reflow, int soft)
{
    mdf_pager_view *view;
    int was_discarded;

    view = reflow->view;
    view->completed_last_match = view->last_match;
    was_discarded = view->bytes.discard;
    if (!was_discarded) {
        if (view->origin_count == view->origin_cap) {
            mdf_pager_origin *next;
            next = (mdf_pager_origin *)mdf_pager_array_grow(&view->allocator, view->origins,
                &view->origin_cap, 64, view->origin_count + 1, view->byte_limit + 1, sizeof(*next));
            if (next == NULL) return -1;
            view->origins = next;
        }
        view->origins[view->origin_count++] = view->row_origin;
    }
    view->next_line++;
    view->query_column = 0;
    view->row_text_start = view->text_count;
    view->row_origin.source_offset = view->input_offset;
    view->row_origin.text_offset = view->text_count - view->input_text_offset;
    view->row_start = view->bytes.len;
    view->bytes.discard = view->next_line < view->collect_from || view->full;
    if (was_discarded && !view->bytes.discard && !soft) {
        if (reflow->osc8_active && mdf_pager_buffer_append(&view->bytes,
                reflow->osc8_start.data, reflow->osc8_start.len) != 0) return -1;
        if (mdf_pager_buffer_append(&view->bytes, reflow->sgr_start.data,
                reflow->sgr_start.len) != 0) return -1;
    }
    return 0;
}

static int mdf_pager_reflow_append_wrap(mdf_pager_reflow_state *reflow)
{
    static const char close[] = "\033]8;;\033\\";
    mdf_pager_view *view;
    size_t *next;

    view = reflow->view;
    if (reflow->osc8_active && mdf_pager_buffer_append(&reflow->view->bytes, close, sizeof(close) - 1) != 0) {
        return -1;
    }
    if (!view->bytes.discard && view->soft_wrap_len == view->soft_wrap_cap) {
        next = (size_t *)mdf_pager_array_grow(&view->allocator, view->soft_wraps,
            &view->soft_wrap_cap, 64, view->soft_wrap_len + 1, view->byte_limit + 1, sizeof(*next));
        if (next == NULL) return -1;
        view->soft_wraps = next;
    }
    if (!view->bytes.discard) view->soft_wraps[view->soft_wrap_len++] = view->bytes.len;
    if (mdf_pager_buffer_append_byte(&reflow->view->bytes, '\n') != 0) return -1;
    if (mdf_pager_next_row(reflow, 1) != 0) return -1;
    if (reflow->osc8_active &&
        mdf_pager_buffer_append(&reflow->view->bytes, reflow->osc8_start.data, reflow->osc8_start.len) != 0) {
        return -1;
    }
    if (reflow->sgr_start.len > 0 &&
        mdf_pager_buffer_append(&reflow->view->bytes, reflow->sgr_start.data, reflow->sgr_start.len) != 0) {
        return -1;
    }
    return 0;
}

static int mdf_pager_append_text_byte(mdf_pager_buffer *out, unsigned char byte)
{
    if (byte >= 0x80 && byte <= 0x9f) {
        if (mdf_pager_buffer_append(out, "M-", 2) != 0) return -1;
        return mdf_pager_append_text_byte(out, (unsigned char)(byte - 0x80));
    }
    if (byte < 32 || byte == 127) {
        if (mdf_pager_buffer_append_byte(out, '^') != 0 ||
            mdf_pager_buffer_append_byte(out, byte == 127 ? '?' : (unsigned char)(byte + 64)) != 0) {
            return -1;
        }
        return 0;
    }
    return mdf_pager_buffer_append_byte(out, byte);
}

static size_t mdf_pager_escape_end(const char *src, size_t len, size_t i)
{
    if (i + 1 >= len || src[i] != '\033') return i + 1;
    i += 2;
    if (src[i - 1] == '[') {
        while (i < len) {
            unsigned char byte;

            byte = (unsigned char)src[i++];
            if (byte >= 0x40 && byte <= 0x7e) break;
        }
    } else if (src[i - 1] == ']') {
        while (i < len) {
            if (src[i] == '\a') return i + 1;
            if (src[i] == '\033' && i + 1 < len && src[i + 1] == '\\') return i + 2;
            i++;
        }
    }
    return i;
}

static int mdf_pager_view_build_searchable(mdf_pager_view *view)
{
    size_t i;
    size_t wrap;

    wrap = 0;
    for (i = 0; i < view->bytes.len;) {
        size_t start;
        unsigned char byte;

        start = i;
        byte = (unsigned char)view->bytes.data[i];
        if (byte == 0x1b) {
            i = mdf_pager_escape_end(view->bytes.data, view->bytes.len, i);
            continue;
        }
        if (byte == '\n') {
            /* Display wraps do not insert characters into the logical text. */
            if (wrap < view->soft_wrap_len && view->soft_wraps[wrap] == i) {
                wrap++;
                i++;
                continue;
            }
            byte = ' ';
        }
        if (mdf_pager_buffer_append_byte(&view->searchable, byte) != 0) return -1;
        if (view->searchable.len > view->search_offset_cap) {
            size_t *next;

            next = (size_t *)mdf_pager_array_grow(&view->allocator, view->search_offsets,
                &view->search_offset_cap, 1024, view->searchable.len, view->byte_limit + 1, sizeof(*next));
            if (next == NULL) return -1;
            view->search_offsets = next;
        }
        view->search_offsets[view->searchable.len - 1] = start;
        i++;
    }
    return 0;
}

static int mdf_pager_search_equal(const char *haystack, size_t haystack_len, size_t at,
                                  const char *needle, size_t needle_len, size_t *end,
                                  mdf_pager_cancel_state *cancel)
{
    unsigned long haystack_folded[3];
    unsigned long needle_folded[3];
    unsigned long codepoint;
    size_t haystack_offset;
    size_t needle_offset;
    size_t haystack_folded_len;
    size_t needle_folded_len;
    size_t haystack_folded_index;
    size_t needle_folded_index;
    size_t consumed;

    haystack_offset = at;
    needle_offset = 0;
    haystack_folded_len = 0;
    needle_folded_len = 0;
    haystack_folded_index = 0;
    needle_folded_index = 0;
    for (;;) {
        if (mdf_pager_cancel_work(cancel, 1)) return 0;
        if (needle_folded_index == needle_folded_len) {
            if (needle_offset == needle_len) {
                if (haystack_folded_index != haystack_folded_len) return 0;
                if (end != NULL) *end = haystack_offset;
                return 1;
            }
            consumed = utf8_decode_codepoint(needle + needle_offset,
                                              needle_len - needle_offset, &codepoint);
            if (consumed == 0) return 0;
            needle_offset += consumed;
            needle_folded_len = mdf_unicode_casefold(codepoint, needle_folded);
            needle_folded_index = 0;
        }
        if (haystack_folded_index == haystack_folded_len) {
            if (haystack_offset == haystack_len) return 0;
            consumed = utf8_decode_codepoint(haystack + haystack_offset,
                                              haystack_len - haystack_offset, &codepoint);
            if (consumed == 0) return 0;
            haystack_offset += consumed;
            haystack_folded_len = mdf_unicode_casefold(codepoint, haystack_folded);
            haystack_folded_index = 0;
        }
        if (haystack_folded[haystack_folded_index] != needle_folded[needle_folded_index]) return 0;
        haystack_folded_index++;
        needle_folded_index++;
    }
}

static int mdf_pager_view_find_anchor(const mdf_pager_view *view, const mdf_pager_buffer *anchor,
                                      size_t expected, size_t *offset)
{
    size_t i;
    size_t best;
    size_t best_distance;
    int found;

    if (anchor->len == 0) return 0;
    found = 0;
    best = 0;
    best_distance = 0;
    for (i = 0; i < view->searchable.len; i++) {
        size_t distance;

        if (mdf_pager_cancel_work(view->bytes.cancel, 1)) return -1;
        if (anchor->len > view->searchable.len - i ||
            memcmp(view->searchable.data + i, anchor->data, anchor->len) != 0) continue;
        distance = i > expected ? i - expected : expected - i;
        if (!found || distance < best_distance) {
            found = 1;
            best = i;
            best_distance = distance;
        }
    }
    if (found) *offset = best;
    return found;
}

static void mdf_pager_search_destroy(mdf_pager_search *search)
{
    mdf_pager_query_destroy(&search->scan);
    mdf_pager_buffer_destroy(&search->query);
    mdf_free_mem(&search->allocator, search->matches, search->cap * sizeof(*search->matches));
    memset(search, 0, sizeof(*search));
}

static void mdf_pager_search_init(mdf_pager_search *search, const mdf_options *opts)
{
    memset(search, 0, sizeof(*search));
    if (opts != NULL) {
        search->allocator = opts->allocator;
    }
    mdf_allocator_normalize(&search->allocator);
    search->scan.allocator = search->allocator;
    mdf_pager_buffer_init_with_allocator(&search->query, &search->allocator);
}

static int mdf_pager_search_add_match(mdf_pager_search *search, size_t limit, size_t start, size_t end)
{
    mdf_pager_match *next;

    if (search->count == search->cap) {
        next = (mdf_pager_match *)mdf_pager_array_grow(&search->allocator, search->matches,
            &search->cap, 16, search->count + 1, limit, sizeof(*next));
        if (next == NULL) return -1;
        search->matches = next;
    }
    search->matches[search->count].start = start;
    search->matches[search->count].end = end;
    search->count++;
    return 0;
}

static int mdf_pager_search_refresh(mdf_pager_search *search, const mdf_pager_view *view)
{
    size_t i;

    mdf_free_mem(&search->allocator, search->matches, search->cap * sizeof(*search->matches));
    search->matches = NULL;
    search->count = 0;
    search->cap = 0;
    search->selected = 0;
    if (search->query.len == 0) return 0;
    for (i = 0; i < view->searchable.len; i++) {
        size_t searchable_end;

        if (mdf_pager_cancel_work(view->bytes.cancel, 1)) return -1;
        if (((unsigned char)view->searchable.data[i] & 0xc0) == 0x80) continue;
        if (mdf_pager_search_equal(view->searchable.data, view->searchable.len, i,
                                   search->query.data, search->query.len, &searchable_end, view->bytes.cancel)) {
            size_t start;
            size_t end;

            start = view->search_offsets[i];
            end = view->search_offsets[searchable_end - 1] + 1;
            if (mdf_pager_search_add_match(search, view->byte_limit + 1, start, end) != 0) return -1;
        }
    }
    return 0;
}

/* A changed query can inspect only the retained window. Its prefix is known
 * only when that window starts at the beginning; later replay counts evicted
 * output too, without saving an unbounded list of match locations. */
static int mdf_pager_search_begin(mdf_pager_search *search, mdf_pager_view *view)
{
    size_t i;
    size_t wrap;
    size_t consumed;
    uint64_t row;
    unsigned long codepoint;

    if (mdf_pager_query_set(&search->scan, search->query.data, search->query.len) != 0) return -1;
    search->have_selection = 0;
    mdf_pager_query_restart(&search->scan, view->first_line == 0 && !view->full, 0);
    view->last_match = 0;
    view->completed_last_match = 0;
    row = view->first_line;
    view->query_column = 0;
    wrap = 0;
    for (i = 0; i < view->bytes.len;) {
        if (mdf_pager_cancel_work(view->bytes.cancel, 1)) return -1;
        if (view->bytes.data[i] == '\033') {
            i = mdf_pager_escape_end(view->bytes.data, view->bytes.len, i);
        } else if (view->bytes.data[i] == '\n') {
            if (wrap < view->soft_wrap_len && view->soft_wraps[wrap] == i) wrap++;
            else if (mdf_pager_query_visible(view, ' ', row, 1) != 0) return -1;
            view->completed_last_match = view->last_match;
            row++;
            view->query_column = 0;
            i++;
        } else {
            consumed = utf8_decode_codepoint(view->bytes.data + i, view->bytes.len - i, &codepoint);
            if (consumed == 0 || mdf_pager_query_visible(view, codepoint, row, 1) != 0) return -1;
            i += consumed;
        }
    }
    view->match_prefix_known = view->first_line == 0;
    if (view->eof_known && view->first_line == 0 && view->line_count >= view->total_lines)
        mdf_pager_query_finish(&search->scan);
    return mdf_pager_search_refresh(search, view);
}

static size_t mdf_pager_utf8_complete_prefix(const char *text, size_t len)
{
    size_t i;

    for (i = 0; i < len;) {
        unsigned char byte;
        size_t expected;
        size_t j;

        byte = (unsigned char)text[i];
        if (byte < 0x80) {
            i++;
            continue;
        }
        if (byte < 0xc2 || byte > 0xf4) {
            i++;
            continue;
        }
        expected = byte < 0xe0 ? 2 : (byte < 0xf0 ? 3 : 4);
        if (i + expected > len) break;
        for (j = 1; j < expected; j++) {
            if (((unsigned char)text[i + j] & 0xc0) != 0x80) break;
        }
        if (j != expected) break;
        i += expected;
    }
    return i;
}

static size_t mdf_pager_utf8_display_prefix(const char *text, size_t len, size_t columns)
{
    size_t i;
    size_t used;

    i = 0;
    used = 0;
    while (i < len) {
        unsigned long codepoint;
        size_t unit_len;
        size_t unit_width;

        unit_len = utf8_decode_codepoint(text + i, len - i, &codepoint);
        if (unit_len == 0) break;
        unit_width = utf8_display_width(codepoint);
        if (unit_width > columns - used) break;
        used += unit_width;
        i += unit_len;
    }
    return i;
}

static size_t mdf_pager_utf8_display_width(const char *text, size_t len)
{
    size_t prefix;
    size_t width;
    size_t i;

    prefix = mdf_pager_utf8_display_prefix(text, len, (size_t)-1);
    width = 0;
    for (i = 0; i < prefix;) {
        unsigned long codepoint;
        size_t unit_len;

        unit_len = utf8_decode_codepoint(text + i, prefix - i, &codepoint);
        if (unit_len == 0) break;
        width += utf8_display_width(codepoint);
        i += unit_len;
    }
    return width;
}

static int mdf_pager_status_append_sanitized(mdf_pager_buffer *out, const char *text, size_t len)
{
    size_t i;

    i = 0;
    while (i < len) {
        unsigned char byte;

        byte = (unsigned char)text[i];
        if (byte < 32 || byte == 127 || (byte >= 0x80 && byte <= 0x9f)) {
            if (mdf_pager_append_text_byte(out, byte) != 0) return -1;
            i++;
            continue;
        }
        if (byte < 0x80) {
            if (mdf_pager_buffer_append_byte(out, byte) != 0) return -1;
            i++;
        } else {
            unsigned long codepoint;
            size_t unit_len;

            unit_len = utf8_decode_codepoint(text + i, len - i, &codepoint);
            if (unit_len == 0 || codepoint < 32 || (codepoint >= 127 && codepoint <= 159)) {
                if (mdf_pager_buffer_append_byte(out, '?') != 0) return -1;
                i++;
                continue;
            }
            if (mdf_pager_buffer_append(out, text + i, unit_len) != 0) return -1;
            i += unit_len;
        }
    }
    return 0;
}

static int mdf_pager_sanitize_markdown_byte(mdf_pager_buffer *out, unsigned char byte)
{
    if ((byte < 32 && byte != '\n' && byte != '\r' && byte != '\t') || byte == 127 ||
        (byte >= 0x80 && byte <= 0x9f)) {
        return mdf_pager_append_text_byte(out, byte);
    }
    return mdf_pager_buffer_append_byte(out, byte);
}

static size_t mdf_pager_sanitize_markdown_utf8_width(unsigned char byte)
{
    if (byte >= 0xc2 && byte <= 0xdf) return 2;
    if (byte >= 0xe0 && byte <= 0xef) return 3;
    if (byte >= 0xf0 && byte <= 0xf4) return 4;
    return 1;
}

static int mdf_pager_sanitize_markdown_pending(mdf_pager_markdown_sanitizer *state,
                                                mdf_pager_buffer *out, int final)
{
    while (state->pending_utf8_len > 0) {
        unsigned char byte;
        unsigned long codepoint;
        size_t width;
        size_t decoded;

        byte = (unsigned char)state->pending_utf8[0];
        width = mdf_pager_sanitize_markdown_utf8_width(byte);
        if (state->pending_utf8_len < width && !final) {
            return 0;
        }
        decoded = utf8_decode_codepoint(state->pending_utf8, state->pending_utf8_len, &codepoint);
        if (decoded == width && width > 1) {
            if (codepoint >= 0x80 && codepoint <= 0x9f) {
                if (mdf_pager_append_text_byte(out, (unsigned char)codepoint) != 0) return -1;
            } else if (mdf_pager_buffer_append(out, state->pending_utf8, width) != 0) {
                return -1;
            }
            memmove(state->pending_utf8, state->pending_utf8 + width,
                    state->pending_utf8_len - width);
            state->pending_utf8_len -= width;
            continue;
        }
        if (mdf_pager_sanitize_markdown_byte(out, byte) != 0) return -1;
        memmove(state->pending_utf8, state->pending_utf8 + 1,
                state->pending_utf8_len - 1);
        state->pending_utf8_len--;
    }
    return 0;
}

static int mdf_pager_sanitize_markdown_bytes(mdf_pager_markdown_sanitizer *state,
                                             mdf_pager_buffer *out,
                                             const char *src, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (state->pending_utf8_len == sizeof(state->pending_utf8)) return -1;
        state->pending_utf8[state->pending_utf8_len++] = src[i];
        if (mdf_pager_sanitize_markdown_pending(state, out, 0) != 0) return -1;
    }
    return 0;
}

mdf_status mdf_pager_sanitize_markdown_source(mdf_source *source, mdf_sink *sink,
                                              const mdf_options *render_options)
{
    mdf_pager_buffer sanitized;
    mdf_pager_markdown_sanitizer sanitizer;
    mdf_options opts;
    mdf_status st;
    char input[4096];
    size_t len;
    int err;

    if (source == NULL || source->read == NULL || sink == NULL || sink->write == NULL) {
        return MDF_ERROR_INVALID;
    }
    mdf_options_init(&opts);
    if (render_options != NULL) opts = *render_options;
    mdf_allocator_prepare(&opts.allocator);
    mdf_pager_buffer_init(&sanitized, &opts);
    memset(&sanitizer, 0, sizeof(sanitizer));
    st = MDF_OK;
    for (;;) {
        err = 0;
        len = source->read(source->userdata, input, sizeof(input), &err);
        if (len == 0) {
            if (err != 0) {
                st = MDF_ERROR_IO;
            } else if (mdf_pager_sanitize_markdown_pending(&sanitizer, &sanitized, 1) != 0) {
                st = MDF_ERROR_NOMEM;
            }
            break;
        }
        if (len > sizeof(input) ||
            mdf_pager_sanitize_markdown_bytes(&sanitizer, &sanitized, input, len) != 0) {
            st = MDF_ERROR_NOMEM;
            break;
        }
        if (sanitized.len > 0 && sink->write(sink->userdata, sanitized.data, sanitized.len) != 0) {
            st = MDF_ERROR_IO;
            break;
        }
        sanitized.len = 0;
    }
    if (st == MDF_OK && sanitized.len > 0 && sink->write(sink->userdata, sanitized.data, sanitized.len) != 0) {
        st = MDF_ERROR_IO;
    }
    mdf_pager_buffer_destroy(&sanitized);
    return st;
}

static size_t mdf_pager_utf8_character_count(const mdf_pager_buffer *text)
{
    size_t i;
    size_t count;
    size_t complete_len;

    complete_len = mdf_pager_utf8_complete_prefix(text->data, text->len);
    count = 0;
    for (i = 0; i < complete_len; i++) {
        if (((unsigned char)text->data[i] & 0xc0) != 0x80) count++;
    }
    return count;
}

static void mdf_pager_search_clear_matches(mdf_pager_search *search)
{
    mdf_free_mem(&search->allocator, search->matches, search->cap * sizeof(*search->matches));
    search->matches = NULL;
    search->count = 0;
    search->cap = 0;
    search->selected = 0;
}

/* Cancelling or replacing a query is part of the same pager session. Keep
 * its configured allocator so subsequent live-search buffers obey the
 * caller's ownership and allocation policy. */
static void mdf_pager_search_reset(mdf_pager_search *search)
{
    mdf_pager_query_destroy(&search->scan);
    mdf_pager_query_invalidate(&search->scan);
    mdf_pager_buffer_destroy(&search->query);
    mdf_pager_search_clear_matches(search);
    search->editing = 0;
    search->active = 0;
    search->have_selection = 0;
    search->seeking = 0;
    mdf_pager_buffer_init_with_allocator(&search->query, &search->allocator);
    search->query.limit = 4096;
}

static int mdf_pager_search_update_live(mdf_pager_search *search, mdf_pager_view *view)
{
    if (mdf_pager_utf8_complete_prefix(search->query.data, search->query.len) != search->query.len) {
        return 0;
    }
    if (mdf_pager_utf8_character_count(&search->query) < 3) {
        mdf_pager_query_destroy(&search->scan);
        mdf_pager_query_invalidate(&search->scan);
        mdf_pager_search_clear_matches(search);
        search->active = 0;
        return 0;
    }
    search->active = 1;
    return mdf_pager_search_begin(search, view);
}

static void mdf_pager_search_backspace(mdf_pager_search *search)
{
    if (search->query.len == 0) return;
    search->query.len--;
    while (search->query.len > 0 &&
           ((unsigned char)search->query.data[search->query.len] & 0xc0) == 0x80) {
        search->query.len--;
    }
    search->query.data[search->query.len] = '\0';
}

/* Cache capacity counts rendered content. Evict before appending, never after
 * overshooting the contiguous allocation. Metadata has its own allocator. */
static int mdf_pager_view_make_room(void *userdata, size_t incoming)
{
    mdf_pager_view *view;

    view = (mdf_pager_view *)userdata;
    if (incoming > view->byte_limit - (view->bytes.len - view->row_start)) {
        view->row_too_large = 1;
        return -1;
    }
    while (incoming > view->byte_limit - view->bytes.len && view->first_line < view->protected_first) {
        char *newline;
        size_t drop;
        size_t j;
        size_t kept;
        size_t dropped_rows;
        size_t wanted_rows;

        wanted_rows = view->protected_first == MDF_PAGER_POSITION_MAX ? view->origin_count / 4 : 1;
        if (wanted_rows == 0) wanted_rows = 1;
        dropped_rows = 0;
        drop = 0;
        while (dropped_rows < wanted_rows) {
            newline = (char *)memchr(view->bytes.data + drop, '\n', view->row_start - drop);
            if (newline == NULL) break;
            drop = (size_t)(newline - view->bytes.data) + 1;
            dropped_rows++;
        }
        if (drop == 0) break;
        memmove(view->bytes.data, view->bytes.data + drop, view->bytes.len - drop);
        view->bytes.len -= drop;
        view->bytes.data[view->bytes.len] = '\0';
        view->row_start -= drop;
        view->first_line += dropped_rows;
        if (view->origin_count >= dropped_rows) {
            view->origin_count -= dropped_rows;
            memmove(view->origins, view->origins + dropped_rows,
                view->origin_count * sizeof(*view->origins));
        }
        kept = 0;
        for (j = 0; j < view->soft_wrap_len; j++) {
            if (view->soft_wraps[j] >= drop) view->soft_wraps[kept++] = view->soft_wraps[j] - drop;
        }
        view->soft_wrap_len = kept;
    }
    if (incoming > view->byte_limit - view->bytes.len) {
        view->last_match = view->completed_last_match;
        view->bytes.len = view->row_start;
        view->retained_text_end = view->row_text_start;
        if (view->bytes.data != NULL) view->bytes.data[view->bytes.len] = '\0';
        while (view->soft_wrap_len > 0 && view->soft_wraps[view->soft_wrap_len - 1] >= view->bytes.len)
            view->soft_wrap_len--;
        view->full = 1;
        view->bytes.discard = 1;
    }
    return 0;
}

static int mdf_pager_query_text(mdf_pager_view *view, unsigned long codepoint)
{
    if (codepoint >= 0x80 && codepoint <= 0x9f) {
        if (mdf_pager_query_visible(view, 'M', view->next_line, !view->bytes.discard) != 0 ||
            mdf_pager_query_visible(view, '-', view->next_line, !view->bytes.discard) != 0) return -1;
        return mdf_pager_query_text(view, codepoint - 0x80);
    }
    if (codepoint < 32 || codepoint == 127) {
        if (mdf_pager_query_visible(view, '^', view->next_line, !view->bytes.discard) != 0) return -1;
        codepoint = codepoint == 127 ? '?' : codepoint + 64;
    }
    return mdf_pager_query_visible(view, codepoint, view->next_line, !view->bytes.discard);
}

/* Count actual visible output, not query reindexing or terminal soft wraps. */
static void mdf_pager_track_text(mdf_pager_view *view, size_t len)
{
    if (view->text_count == view->row_text_start) {
        view->row_origin.source_offset = view->input_offset;
        view->row_origin.text_offset = view->text_count - view->input_text_offset;
    }
    view->text_count += len;
    if (!view->bytes.discard) view->retained_text_end = view->text_count;
}

static int mdf_pager_reflow_chunk(mdf_pager_reflow_state *reflow, const char *src, size_t len)
{
    size_t i;
    mdf_pager_view *view;
    int width;
    int styled;
    int col;

    view = reflow->view;
    width = reflow->width;
    styled = reflow->styled;
    col = reflow->col;
    i = 0;
    while (i < len) {
        unsigned char byte;
        unsigned long codepoint;
        size_t unit_len;
        int unit_width;
        int text_c1;

        if (mdf_pager_termination_signal != 0 || mdf_pager_cancel_work(view->bytes.cancel, 1))
            return -1;
        byte = (unsigned char)src[i];
        /* Normalize CRLF only. A standalone carriage return in ordinary text
         * is content and must take the same safe caret-notation path as every
         * other C0 control. */
        if (!styled && byte == '\r' && i + 1 < len && src[i + 1] == '\n') {
            i++;
            continue;
        }
        if (byte == '\n') {
            if (mdf_pager_buffer_append_byte(&view->bytes, byte) != 0) return -1;
            mdf_pager_track_text(view, 1);
            if (mdf_pager_query_visible(view, ' ', view->next_line, !view->bytes.discard) != 0) return -1;
            if (mdf_pager_next_row(reflow, 0) != 0) return -1;
            col = 0;
            i++;
            continue;
        }
        if (styled && byte == 0x1b) {
            size_t escape_start;

            escape_start = i;
            if (mdf_pager_append_escape(&view->bytes, src, len, &i) != 0) return -1;
            if (mdf_pager_reflow_track_escape(reflow, src + escape_start, i - escape_start) != 0) return -1;
            continue;
        }
        if (byte == '\t') {
            int spaces;

            spaces = 8 - (col % 8);
            while (spaces-- > 0) {
                if (col >= width) {
                    if (mdf_pager_reflow_append_wrap(reflow) != 0) return -1;
                    col = 0;
                }
                if (mdf_pager_buffer_append_byte(&view->bytes, ' ') != 0) return -1;
                mdf_pager_track_text(view, 1);
                if (mdf_pager_query_visible(view, ' ', view->next_line, !view->bytes.discard) != 0) return -1;
                col++;
            }
            i++;
            continue;
        }
        text_c1 = 0;
        {
            unit_len = utf8_decode_codepoint(src + i, len - i, &codepoint);
            if (unit_len == 0) return -1;
            unit_width = (int)utf8_display_width(codepoint);
            if (!styled && (byte < 32 || byte == 127)) unit_width = 2;
            if (!styled && byte >= 0x80 && byte <= 0x9f) unit_width = 4;
            if (!styled && codepoint >= 0x80 && codepoint <= 0x9f) {
                text_c1 = 1;
                unit_width = 4;
            }
        }
        if (unit_width > 0 && col > 0 && (col >= width || col + unit_width > width)) {
            if (mdf_pager_reflow_append_wrap(reflow) != 0) return -1;
            col = 0;
        }
        if (styled) {
            if (mdf_pager_buffer_append(&view->bytes, src + i, unit_len) != 0) return -1;
        } else {
            if (text_c1) {
                if (mdf_pager_append_text_byte(&view->bytes, (unsigned char)codepoint) != 0) return -1;
            } else if (unit_len == 1) {
                if (mdf_pager_append_text_byte(&view->bytes, byte) != 0) return -1;
            } else {
                if (mdf_pager_buffer_append(&view->bytes, src + i, unit_len) != 0) return -1;
            }
        }
        mdf_pager_track_text(view, !styled && (text_c1 || byte < 32 || byte == 127 ||
            (byte >= 0x80 && byte <= 0x9f)) ? (size_t)unit_width : unit_len);
        if (styled) {
            if (mdf_pager_query_visible(view, codepoint, view->next_line, !view->bytes.discard) != 0) return -1;
        } else if (mdf_pager_query_text(view, codepoint) != 0) return -1;
        col += unit_width;
        i += unit_len;
    }
    reflow->col = col;
    return 0;
}

static int mdf_pager_reflow_sink_write(void *userdata, const char *src, size_t len)
{
    return mdf_pager_reflow_chunk((mdf_pager_reflow_state *)userdata, src, len);
}

static void mdf_pager_view_destroy(mdf_pager_view *view)
{
    mdf_pager_buffer_destroy(&view->bytes);
    mdf_pager_buffer_destroy(&view->searchable);
    mdf_free_mem(&view->allocator, view->search_offsets,
                 view->search_offset_cap * sizeof(*view->search_offsets));
    mdf_free_mem(&view->allocator, view->lines, view->line_cap * sizeof(*view->lines));
    mdf_free_mem(&view->allocator, view->soft_wraps, view->soft_wrap_cap * sizeof(*view->soft_wraps));
    mdf_free_mem(&view->allocator, view->origins, view->origin_cap * sizeof(*view->origins));
    memset(view, 0, sizeof(*view));
}

static void mdf_pager_view_init(mdf_pager_view *view, const mdf_options *opts)
{
    memset(view, 0, sizeof(*view));
    if (opts != NULL) {
        view->allocator = opts->allocator;
    }
    mdf_allocator_normalize(&view->allocator);
    mdf_pager_buffer_init_with_allocator(&view->bytes, &view->allocator);
    mdf_pager_buffer_init_with_allocator(&view->searchable, &view->allocator);
}

static int mdf_pager_view_add_line(mdf_pager_view *view, size_t offset)
{
    size_t *next;

    if (view->line_count == view->line_cap) {
        next = (size_t *)mdf_pager_array_grow(&view->allocator, view->lines,
            &view->line_cap, 128, view->line_count + 1, view->byte_limit + 1, sizeof(*next));
        if (next == NULL) {
            return -1;
        }
        view->lines = next;
    }
    view->lines[view->line_count++] = offset;
    return 0;
}

static int mdf_pager_view_index(mdf_pager_view *view)
{
    size_t i;

    if (mdf_pager_view_add_line(view, 0) != 0) {
        return -1;
    }
    for (i = 0; i < view->bytes.len; i++) {
        if (mdf_pager_cancel_work(view->bytes.cancel, 1)) return -1;
        if (view->bytes.data[i] == '\n' && i + 1 < view->bytes.len &&
            mdf_pager_view_add_line(view, i + 1) != 0) {
            return -1;
        }
    }
    return 0;
}

static size_t mdf_pager_view_line_for_offset(const mdf_pager_view *view, size_t offset)
{
    size_t low;
    size_t high;

    if (view->line_count == 0) return 0;
    low = 0;
    high = view->line_count;
    while (low + 1 < high) {
        size_t middle;

        middle = low + (high - low) / 2;
        if (view->lines[middle] <= offset) low = middle;
        else high = middle;
    }
    return low;
}

/* Positions count visible codepoints, independently of UTF-8 and OSC8 bytes. */
static mdf_pager_query_position mdf_pager_search_at(const mdf_pager_view *view, size_t offset)
{
    mdf_pager_query_position position;
    size_t local_line;
    size_t i;
    size_t consumed;
    unsigned long codepoint;

    local_line = mdf_pager_view_line_for_offset(view, offset);
    position.row = view->first_line + local_line;
    position.column = 0;
    for (i = view->lines[local_line]; i < offset;) {
        if (view->bytes.data[i] == '\033') i = mdf_pager_escape_end(view->bytes.data, view->bytes.len, i);
        else {
            consumed = utf8_decode_codepoint(view->bytes.data + i, view->bytes.len - i, &codepoint);
            if (consumed == 0) break;
            position.column++;
            i += consumed;
        }
    }
    return position;
}

static size_t mdf_pager_search_offset(const mdf_pager_view *view, mdf_pager_query_position position)
{
    size_t i;
    size_t column;
    size_t consumed;
    unsigned long codepoint;

    if (position.row < view->first_line) return 0;
    if (position.row - view->first_line >= view->line_count) return view->bytes.len;
    i = view->lines[(size_t)(position.row - view->first_line)];
    column = 0;
    while (i < view->bytes.len && view->bytes.data[i] != '\n') {
        if (view->bytes.data[i] == '\033') i = mdf_pager_escape_end(view->bytes.data, view->bytes.len, i);
        else {
            if (column == position.column) break;
            consumed = utf8_decode_codepoint(view->bytes.data + i, view->bytes.len - i, &codepoint);
            if (consumed == 0) break;
            i += consumed;
            column++;
        }
    }
    return i;
}

static mdf_pager_query_match mdf_pager_search_cached_match(const mdf_pager_view *view,
                                                           const mdf_pager_search *search, size_t index)
{
    mdf_pager_query_match match;

    match.start = mdf_pager_search_at(view, search->matches[index].start);
    match.end = mdf_pager_search_at(view, search->matches[index].end);
    match.ordinal = view->match_prefix_known && view->last_match >= search->count ?
        view->last_match - search->count + index + 1 : 0;
    return match;
}

/* Keep at most 128 visible bytes around a passage. Include preceding context
 * so a continuation row can still identify its source paragraph after reflow.
 * Blank lines bound paragraphs; terminal soft wraps add no searchable bytes. */
static int mdf_pager_anchor_capture(mdf_pager_anchor *anchor, const mdf_pager_view *view, size_t raw_offset)
{
    size_t at;
    size_t start;
    size_t end;
    size_t i;
    size_t row;
    size_t row_start;
    mdf_pager_origin origin;

    at = 0;
    while (at < view->searchable.len && view->search_offsets[at] < raw_offset) at++;
    start = at > 64 ? at - 64 : 0;
    while (start < at && ((unsigned char)view->searchable.data[start] & 0xc0) == 0x80) start++;
    for (i = start + 1; i < at; i++) {
        if (view->bytes.data[view->search_offsets[i - 1]] == '\n' &&
            view->bytes.data[view->search_offsets[i]] == '\n') start = i + 1;
    }
    end = view->searchable.len - at > 64 ? at + 64 : view->searchable.len;
    for (i = at; i + 1 < end; i++) {
        if (view->bytes.data[view->search_offsets[i]] == '\n' &&
            view->bytes.data[view->search_offsets[i + 1]] == '\n') { end = i; break; }
    }
    while (end < view->searchable.len && end > at &&
           ((unsigned char)view->searchable.data[end] & 0xc0) == 0x80) end--;
    anchor->prefix = at - start;
    row = mdf_pager_view_line_for_offset(view, raw_offset);
    origin = row < view->origin_count ? view->origins[row] : view->row_origin;
    row_start = at;
    while (row_start != 0 && view->search_offsets[row_start - 1] >= view->lines[row]) row_start--;
    anchor->source_offset = origin.source_offset;
    anchor->text_offset = origin.text_offset + (at - row_start);
    anchor->complete_cache = view->eof_known && view->first_line == 0 && view->line_count >= view->total_lines;
    anchor->text.len = 0;
    return end > start ? mdf_pager_buffer_append(&anchor->text, view->searchable.data + start, end - start) : 0;
}

static int mdf_pager_anchor_find(const mdf_pager_anchor *anchor, const mdf_pager_view *view,
                                 int word_fallback, size_t *offset)
{
    mdf_pager_buffer word;
    mdf_pager_buffer context;
    size_t expected;
    size_t start;
    uint64_t first;
    uint64_t target;
    int found;

    *offset = 0;
    if (!anchor->replayed) return 0;
    first = view->retained_text_end - view->searchable.len;
    target = anchor->text_offset > MDF_PAGER_POSITION_MAX - anchor->replay_text_offset ?
        MDF_PAGER_POSITION_MAX : anchor->replay_text_offset + anchor->text_offset;
    expected = target <= first ? 0 : (target - first < view->searchable.len ?
        (size_t)(target - first) : view->searchable.len);
    start = expected > anchor->prefix ? expected - anchor->prefix : 0;
    context = anchor->text;
    found = mdf_pager_view_find_anchor(view, &context, start, &start);
    if (found < 0) return -1;
    /* Hidden OSC8 bytes can consume the source neighbourhood while only a
     * few label bytes become visible. Compare the available context without
     * requiring its unseen suffix, but always include the anchored byte. */
    if (!found && context.len > view->searchable.len - start) {
        context.len = view->searchable.len - start;
        while (context.len > anchor->prefix && context.len < anchor->text.len &&
               ((unsigned char)context.data[context.len] & 0xc0) == 0x80) context.len--;
        if (context.len > anchor->prefix) found = mdf_pager_view_find_anchor(view, &context, start, &start);
    }
    if (found < 0) return -1;
    if (found) {
        *offset = start + anchor->prefix;
        return 1;
    }
    *offset = expected;
    if (!word_fallback || anchor->prefix >= anchor->text.len) return 0;
    word = anchor->text;
    word.data += anchor->prefix;
    word.len = 0;
    while (word.len < anchor->text.len - anchor->prefix && word.data[word.len] != ' ' &&
           word.data[word.len] != '\t') word.len++;
    return mdf_pager_view_find_anchor(view, &word, expected, offset);
}

/* Cache-local match ordinals change after eviction. Preserve the selected
 * source passage using its absolute display row and position within that row. */
static void mdf_pager_search_select_anchor(mdf_pager_search *search, const mdf_pager_view *view,
                                            uint64_t anchor_line, size_t anchor_column)
{
    size_t i;
    uint64_t best_line;
    size_t best_column;

    best_line = MDF_PAGER_POSITION_MAX;
    best_column = (size_t)-1;
    for (i = 0; i < search->count; i++) {
        size_t local_line;
        size_t column;
        size_t column_distance;
        uint64_t line;
        uint64_t distance;

        local_line = mdf_pager_view_line_for_offset(view, search->matches[i].start);
        line = view->first_line + local_line;
        column = search->matches[i].start - view->lines[local_line];
        distance = line > anchor_line ? line - anchor_line : anchor_line - line;
        column_distance = column > anchor_column ? column - anchor_column : anchor_column - column;
        if (distance < best_line || (distance == best_line && column_distance < best_column)) {
            best_line = distance;
            best_column = column_distance;
            search->selected = i;
        }
    }
}

static size_t mdf_pager_view_top_for_anchor(const mdf_pager_view *view, size_t anchor)
{
    if (view->searchable.len == 0 || view->line_count == 0) return 0;
    if (anchor >= view->searchable.len) anchor = view->searchable.len - 1;
    return mdf_pager_view_line_for_offset(view, view->search_offsets[anchor]);
}

static int mdf_pager_window_size(int *columns, int *rows)
{
    struct winsize size;

    memset(&size, 0, sizeof(size));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
        *columns = 80;
        *rows = 24;
    } else {
        *columns = size.ws_col == 0 ? 80 : (int)size.ws_col;
        *rows = size.ws_row == 0 ? 24 : (int)size.ws_row;
    }
    if (*columns < 1) *columns = 1;
    if (*rows < 2) *rows = 2;
    return 0;
}

static int mdf_pager_write(const char *src, size_t len)
{
    size_t off;

    off = 0;
    while (off < len) {
        ssize_t n;

        n = write(STDOUT_FILENO, src + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                /* A job-control stop can interrupt a terminal write. Do not
                 * retry after our handler has recorded it: leave the loop so
                 * pager cleanup restores the terminal and re-raises it. */
                if (mdf_pager_termination_signal != 0) return -1;
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int mdf_pager_write_spaces(size_t count)
{
    static const char spaces[] = "                                                                ";

    while (count > 0) {
        size_t chunk;

        chunk = count < sizeof(spaces) - 1 ? count : sizeof(spaces) - 1;
        if (mdf_pager_write(spaces, chunk) != 0) return -1;
        count -= chunk;
    }
    return 0;
}

static int mdf_pager_write_highlighted_range(const mdf_pager_view *view, size_t start, size_t end)
{
    size_t position;

    position = start;
    while (position < end) {
        size_t next;

        if ((unsigned char)view->bytes.data[position] == 0x1b) {
            next = mdf_pager_escape_end(view->bytes.data, view->bytes.len, position);
            if (next > end) next = end;
            if (mdf_pager_write(view->bytes.data + position, next - position) != 0) return -1;
            if (next > position + 1 && view->bytes.data[position + 1] == '[' &&
                mdf_pager_write("\033[7m", strlen("\033[7m")) != 0) return -1;
            position = next;
            continue;
        }
        next = position + 1;
        while (next < end && (unsigned char)view->bytes.data[next] != 0x1b) next++;
        if (mdf_pager_write(view->bytes.data + position, next - position) != 0) return -1;
        position = next;
    }
    return 0;
}

static int mdf_pager_draw_highlighted(const mdf_pager_view *view, const mdf_pager_search *search,
                                      size_t start, size_t end)
{
    size_t position;
    size_t match_index;
    size_t selected_start;
    size_t selected_end;
    int extra;

    position = start;
    extra = search != NULL && search->active && search->have_selection;
    selected_start = extra ? mdf_pager_search_offset(view, search->selection.start) : 0;
    selected_end = extra ? mdf_pager_search_offset(view, search->selection.end) : 0;
    match_index = 0;
    while (search != NULL && search->active && (match_index < search->count || extra)) {
        size_t match_start;
        size_t match_end;

        if (extra && (match_index == search->count || selected_start <= search->matches[match_index].start)) {
            match_start = selected_start;
            match_end = selected_end;
            extra = 0;
        } else {
            match_start = search->matches[match_index].start;
            match_end = search->matches[match_index++].end;
        }
        if (match_end <= position || match_start >= end) continue;
        if (match_start < position) match_start = position;
        if (match_end > end) match_end = end;
        if (match_start > position &&
            mdf_pager_write(view->bytes.data + position, match_start - position) != 0) return -1;
        if (mdf_pager_write("\033[7m", strlen("\033[7m")) != 0 ||
            mdf_pager_write_highlighted_range(view, match_start, match_end) != 0 ||
            mdf_pager_write("\033[27m", strlen("\033[27m")) != 0) return -1;
        position = match_end;
    }
    return position >= end || mdf_pager_write(view->bytes.data + position, end - position) == 0 ? 0 : -1;
}

static size_t mdf_pager_line_columns(const mdf_pager_view *view, size_t start, size_t end)
{
    size_t columns;
    size_t length;
    unsigned long codepoint;

    columns = 0;
    while (start < end) {
        if (view->bytes.data[start] == '\033') {
            start = mdf_pager_escape_end(view->bytes.data, end, start);
            continue;
        }
        length = utf8_decode_codepoint(view->bytes.data + start, end - start, &codepoint);
        if (length == 0) break;
        columns += utf8_display_width(codepoint);
        start += length;
    }
    return columns;
}

static void mdf_pager_status_number(char out[21], uint64_t value, int known)
{
    size_t length;
    size_t i;
    char byte;

    if (!known) { strcpy(out, "?"); return; }
    length = 0;
    do {
        out[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    out[length] = '\0';
    for (i = 0; i < length / 2; i++) {
        byte = out[i]; out[i] = out[length - i - 1]; out[length - i - 1] = byte;
    }
}

static mdf_status mdf_pager_draw(const mdf_pager_view *view, const char *path, const mdf_options *opts,
                          const mdf_pager_search *search, int columns, int rows, size_t top)
{
    const mdf_theme_style *theme;
    size_t content_rows;
    size_t row;
    size_t max_top;
    size_t available;
    size_t path_len;
    size_t path_width;
    size_t search_len;
    size_t search_width;
    size_t body_width;
    size_t percent_len;
    size_t right_margin;
    size_t padding;
    int percent;
    mdf_status result;
    mdf_pager_buffer status_path;
    mdf_pager_buffer search_status;
    char percent_status[8];
    char search_count_status[64];
    int percent_status_len;

    content_rows = (size_t)(rows - 1);
    max_top = view->line_count > content_rows ? view->line_count - content_rows : 0;
    percent = max_top == 0 || top >= max_top ? 100 : (int)((top * 100) / max_top);
    if (view->eof_known) {
        uint64_t total_top;
        uint64_t global_top;

        total_top = view->total_lines > content_rows ? view->total_lines - content_rows : 0;
        global_top = view->first_line + top;
        percent = total_top == 0 || global_top >= total_top ? 100 :
            (int)((long double)global_top * 100 / (long double)total_top);
    }
    if (mdf_pager_write("\033[H", strlen("\033[H")) != 0) return MDF_ERROR_IO;
    for (row = 0; row < content_rows; row++) {
        size_t index;
        size_t start;
        size_t end;
        size_t line_columns;

        line_columns = 0;
        index = top + row;
        if (index < view->line_count) {
            start = view->lines[index];
            end = index + 1 < view->line_count ? view->lines[index + 1] : view->bytes.len;
            if (end > start && view->bytes.data[end - 1] == '\n') end--;
            line_columns = mdf_pager_line_columns(view, start, end);
            if (end > start && mdf_pager_draw_highlighted(view, search, start, end) != 0) return MDF_ERROR_IO;
        }
        if (mdf_pager_write("\033[0m", strlen("\033[0m")) != 0) return MDF_ERROR_IO;
        /* EL at the right edge would erase the last glyph while autowrap is
         * pending. Full rows have no remainder; reset colors before clearing
         * short/empty rows so old highlighting cannot color their blank tail. */
        if (line_columns < (size_t)columns &&
            mdf_pager_write("\033[K", strlen("\033[K")) != 0) return MDF_ERROR_IO;
        if (mdf_pager_write("\r\n", strlen("\r\n")) != 0) return MDF_ERROR_IO;
    }
    mdf_pager_buffer_init(&status_path, opts);
    mdf_pager_buffer_init(&search_status, opts);
    if (search != NULL && search->editing) {
        size_t query_len;

        query_len = search->query.data == NULL ? 0 :
            mdf_pager_utf8_complete_prefix(search->query.data, search->query.len);
        if (mdf_pager_buffer_append(&search_status, "  /", strlen("  /")) != 0 ||
            mdf_pager_status_append_sanitized(&search_status,
                                              search->query.data == NULL ? "" : search->query.data,
                                              query_len) != 0) {
            result = MDF_ERROR_NOMEM;
            goto done;
        }
    } else if (search != NULL && search->active) {
        size_t query_len;
        int count_len;
        char ordinal[21];
        char total[21];
        uint64_t selected;
        int known;

        query_len = search->query.data == NULL ? 0 :
            mdf_pager_utf8_complete_prefix(search->query.data, search->query.len);
        selected = 0;
        known = search->count == 0 || (view->match_prefix_known && view->last_match >= search->count);
        if (search->count != 0 && known) selected = view->last_match - search->count + search->selected + 1;
        if (search->have_selection) {
            selected = search->selection.ordinal;
            known = selected != 0;
        }
        mdf_pager_status_number(ordinal, selected, known);
        mdf_pager_status_number(total, search->scan.total, search->scan.total_known);
        count_len = snprintf(search_count_status, sizeof(search_count_status), "  %s/%s", ordinal, total);
        if (count_len < 0 || count_len >= (int)sizeof(search_count_status) ||
            mdf_pager_buffer_append(&search_status, "  /", strlen("  /")) != 0 ||
            mdf_pager_status_append_sanitized(&search_status,
                                              search->query.data == NULL ? "" : search->query.data,
                                              query_len) != 0) {
            result = MDF_ERROR_NOMEM;
            goto done;
        }
        /* Preserve the decisive count when a long query needs truncation. */
        search_status.len = mdf_pager_utf8_display_prefix(search_status.data, search_status.len,
            columns > count_len + 7 ? (size_t)(columns - count_len - 7) : 0);
        search_status.data[search_status.len] = '\0';
        if (mdf_pager_buffer_append(&search_status, search_count_status, (size_t)count_len) != 0) {
            result = MDF_ERROR_NOMEM;
            goto done;
        }
    }
    if (mdf_pager_status_append_sanitized(&status_path, path, strlen(path)) != 0) {
        result = MDF_ERROR_NOMEM;
        goto done;
    }
    percent_status_len = view->eof_known ?
        snprintf(percent_status, sizeof(percent_status), "%d%%", percent) :
        snprintf(percent_status, sizeof(percent_status), "--%%");
    if (percent_status_len < 0) {
        result = MDF_ERROR_INVALID;
        goto done;
    }
    if (percent_status_len >= (int)sizeof(percent_status)) percent_status_len = (int)sizeof(percent_status) - 1;
    percent_len = (size_t)percent_status_len;
    if (percent_len > (size_t)columns) percent_len = (size_t)columns;
    right_margin = (size_t)columns > percent_len ? 1 : 0;
    available = (size_t)columns - percent_len - right_margin;
    search_len = 0;
    path_len = 0;
    padding = 0;
    if (available > 0) {
        body_width = available > 1 ? available - 2 : 0;
        search_len = mdf_pager_utf8_display_prefix(search_status.data == NULL ? "" : search_status.data,
                                                    search_status.len, body_width);
        search_width = mdf_pager_utf8_display_width(search_status.data == NULL ? "" : search_status.data,
                                                     search_len);
        path_len = mdf_pager_utf8_display_prefix(status_path.data == NULL ? "" : status_path.data,
                                                  status_path.len, body_width - search_width);
        path_width = mdf_pager_utf8_display_width(status_path.data == NULL ? "" : status_path.data,
                                                   path_len);
        padding = body_width - path_width - search_width;
    }
    theme = mdf_theme_resolve(opts == NULL ? NULL : opts->theme_name);
    if (theme == NULL) theme = mdf_theme_resolve("default");
    if (mdf_pager_write("\033[7m", strlen("\033[7m")) != 0) {
        result = MDF_ERROR_IO;
        goto done;
    }
    if (opts == NULL || !opts->boring) {
        if (mdf_pager_write(theme->heading[0], strlen(theme->heading[0])) != 0) {
            result = MDF_ERROR_IO;
            goto done;
        }
    }
    if ((available > 0 && mdf_pager_write(" ", 1) != 0) ||
        (path_len > 0 && mdf_pager_write(status_path.data, path_len) != 0) ||
        (search_len > 0 && mdf_pager_write(search_status.data, search_len) != 0) ||
        mdf_pager_write_spaces(padding) != 0 ||
        (available > 1 && mdf_pager_write(" ", 1) != 0) ||
        mdf_pager_write(percent_status, percent_len) != 0 ||
        mdf_pager_write_spaces(right_margin) != 0 ||
        /* Padding fills every status cell, including at width one. There is
         * no remainder to erase, and EL here could erase the final cell. */
        mdf_pager_write("\033[0m", strlen("\033[0m")) != 0) {
        result = MDF_ERROR_IO;
        goto done;
    }
    result = MDF_OK;

done:
    mdf_pager_buffer_destroy(&status_path);
    mdf_pager_buffer_destroy(&search_status);
    return result;
}

static int mdf_pager_set_raw_mode(struct termios *saved)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, saved) != 0) return -1;
    raw = *saved;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static int mdf_pager_read_with_timeout(unsigned char *out, long milliseconds)
{
    fd_set reads;
    struct timeval timeout;
    int ready;
    ssize_t n;

    FD_ZERO(&reads);
    FD_SET(STDIN_FILENO, &reads);
    timeout.tv_sec = milliseconds / 1000L;
    timeout.tv_usec = (milliseconds % 1000L) * 1000L;
    ready = select(STDIN_FILENO + 1, &reads, NULL, NULL, &timeout);
    if (ready < 0) return errno == EINTR ? MDF_PAGER_READ_INTERRUPTED : -1;
    if (ready == 0) return 0;
    n = read(STDIN_FILENO, out, 1);
    if (n == 1) return 1;
    if (n == 0) return 0;
    return errno == EINTR ? MDF_PAGER_READ_INTERRUPTED : -1;
}

/* SIGWINCH may arrive after ESC (or ESC [) has already been consumed. Keep
 * the partial escape sequence and resume its bounded read instead of turning
 * the remaining bytes into unrelated pager input. */
static int mdf_pager_read_sequence_byte(unsigned char *out, long milliseconds,
                                        mdf_pager_cancel_state *cancel)
{
    int result;

    do {
        if (mdf_pager_cancel_check(cancel) || mdf_pager_termination_signal != 0) return -1;
        result = mdf_pager_read_with_timeout(out, milliseconds);
        if (mdf_pager_cancel_check(cancel) || mdf_pager_termination_signal != 0) return -1;
    } while (result == MDF_PAGER_READ_INTERRUPTED);
    return result;
}

static mdf_pager_key mdf_pager_read_key(unsigned char byte, mdf_pager_cancel_state *cancel)
{
    unsigned char sequence[8];
    size_t len;
    int result;

    if (byte == 'q') return MDF_PAGER_KEY_QUIT;
    if (byte == 'j' || byte == '\r' || byte == '\n' || byte == 14) return MDF_PAGER_KEY_DOWN;
    if (byte == 'k' || byte == 16) return MDF_PAGER_KEY_UP;
    if (byte == ' ' || byte == 6) return MDF_PAGER_KEY_PAGE_DOWN;
    if (byte == 'b' || byte == 2) return MDF_PAGER_KEY_PAGE_UP;
    if (byte == 'd' || byte == 4) return MDF_PAGER_KEY_HALF_DOWN;
    if (byte == 'u' || byte == 21) return MDF_PAGER_KEY_HALF_UP;
    if (byte == 'g') return MDF_PAGER_KEY_HOME;
    if (byte == 'G') return MDF_PAGER_KEY_END;
    if (byte != 0x1b) return MDF_PAGER_KEY_NONE;
    result = mdf_pager_read_sequence_byte(&byte, 25, cancel);
    if (result != 1) return MDF_PAGER_KEY_QUIT;
    if (byte != '[' && byte != 'O') return MDF_PAGER_KEY_NONE;
    len = 0;
    while (len < sizeof(sequence)) {
        result = mdf_pager_read_sequence_byte(&byte, 10, cancel);
        if (result != 1) break;
        sequence[len++] = byte;
        if ((byte >= 'A' && byte <= 'Z') || byte == '~') break;
    }
    if (len == 0) return MDF_PAGER_KEY_NONE;
    if (sequence[len - 1] == 'A') return MDF_PAGER_KEY_UP;
    if (sequence[len - 1] == 'B') return MDF_PAGER_KEY_DOWN;
    if (sequence[len - 1] == 'H') return MDF_PAGER_KEY_HOME;
    if (sequence[len - 1] == 'F') return MDF_PAGER_KEY_END;
    if (sequence[len - 1] == '~' && len >= 2) {
        if (sequence[0] == '5') return MDF_PAGER_KEY_PAGE_UP;
        if (sequence[0] == '6') return MDF_PAGER_KEY_PAGE_DOWN;
        if (sequence[0] == '1' || sequence[0] == '7') return MDF_PAGER_KEY_HOME;
        if (sequence[0] == '4' || sequence[0] == '8') return MDF_PAGER_KEY_END;
    }
    return MDF_PAGER_KEY_NONE;
}

typedef struct mdf_pager_cache_allocator {
    mdf_allocator parent;
    size_t used;
    size_t budget;
} mdf_pager_cache_allocator;

static void *mdf_pager_cache_alloc(void *userdata, size_t size)
{
    mdf_pager_cache_allocator *cache;
    void *ptr;

    cache = (mdf_pager_cache_allocator *)userdata;
    if (size > cache->budget - cache->used) return NULL;
    ptr = mdf_alloc(&cache->parent, size);
    if (ptr != NULL) cache->used += size;
    return ptr;
}

static void mdf_pager_cache_free(void *userdata, void *ptr, size_t size)
{
    mdf_pager_cache_allocator *cache;

    cache = (mdf_pager_cache_allocator *)userdata;
    if (ptr == NULL) return;
    mdf_free_mem(&cache->parent, ptr, size);
    cache->used -= size;
}

/* The live parser belongs to the document, not to a cache window. Forward
 * navigation resumes it. Only an evicted prefix or a resize requires replay. */
typedef struct mdf_pager_document {
    mdf_seekable_source *source;
    mdf_pager_view *view;
    mdf_pager_search *search;
    mdf_options *opts;
    mdf_options cache_opts;
    mdf_pager_cancel_state *cancel;
    mdf *renderer;
    mdf_pager_reflow_state reflow;
    mdf_pager_markdown_sanitizer sanitizer;
    mdf_pager_buffer sanitized;
    mdf_sink sink;
    size_t content_capacity;
    size_t prefetch_reserve;
    uint64_t offset;
    int markdown;
    int width;
    int eof;
    int scanning;
    int replaying;
    int source_eof;
    int has_lookahead;
    char lookahead;
    char pending[5];
    size_t pending_len;
    struct mdf_pager_checkpoint *checkpoints;
    size_t checkpoint_count;
    uint64_t checkpoint_stride;
    mdf_pager_anchor *anchors[2];
} mdf_pager_document;

typedef struct mdf_pager_checkpoint {
    uint64_t offset;
    uint64_t line;
    int col;
    char pending[5];
    size_t pending_len;
    uint64_t query_generation;
    uint64_t query_count;
    size_t query_column;
} mdf_pager_checkpoint;

#define MDF_PAGER_CHECKPOINTS 128

static void mdf_pager_document_stop(mdf_pager_document *doc)
{
    if (doc->renderer != NULL) {
        mdf_pager_retain_emission_buffer(doc->opts, doc->renderer);
        doc->renderer->destroy(doc->renderer);
        doc->renderer = NULL;
    }
    mdf_pager_buffer_destroy(&doc->reflow.osc8_start);
    mdf_pager_buffer_destroy(&doc->reflow.sgr_start);
    mdf_pager_buffer_destroy(&doc->sanitized);
}

static mdf_status mdf_pager_document_restart(mdf_pager_document *doc, uint64_t first, int width)
{
    mdf_source_offset zero;
    mdf_options render_opts;
    mdf_pager_view *view;
    uint64_t total;
    int known;
    mdf_pager_checkpoint *checkpoint;
    size_t i;
    mdf_pager_buffer content;

    view = doc->view;
    for (i = 0; i < 2; i++) if (doc->anchors[i] != NULL) doc->anchors[i]->replayed = 0;
    doc->replaying = view->next_line > first;
    total = view->total_lines;
    known = width == doc->width && view->eof_known;
    if (width != doc->width) {
        doc->prefetch_reserve = 0;
        mdf_pager_query_invalidate(&doc->search->scan);
        doc->checkpoint_count = 0;
        doc->checkpoint_stride = 64;
    }
    mdf_pager_document_stop(doc);
    checkpoint = NULL;
    if (!doc->markdown) {
        for (i = 0; i < doc->checkpoint_count; i++) {
            if (doc->search->active && (doc->checkpoints[i].query_generation != doc->search->scan.generation))
                continue;
            if (doc->checkpoints[i].line < first ||
                (doc->checkpoints[i].line == first && doc->checkpoints[i].col == 0))
                checkpoint = &doc->checkpoints[i];
            else break;
        }
    }
    memset(&zero, 0, sizeof(zero));
    if (checkpoint != NULL) {
        zero.high = (unsigned long)(checkpoint->offset >> 32);
        zero.low = (unsigned long)(checkpoint->offset & 0xffffffffUL);
    }
    if (doc->source->seek(doc->source->userdata, &zero) != 0) return MDF_ERROR_IO;
    content = view->bytes;
    memset(&view->bytes, 0, sizeof(view->bytes));
    mdf_pager_view_destroy(view);
    mdf_pager_view_init(view, &doc->cache_opts);
    if (content.data == NULL) {
        mdf_pager_buffer_init_with_allocator(&content, &doc->opts->allocator);
        content.cap = doc->content_capacity + 1;
        content.data = (char *)mdf_alloc(&content.allocator, content.cap);
        if (content.data == NULL) return MDF_ERROR_NOMEM;
    }
    view->bytes = content;
    view->bytes.len = 0;
    view->bytes.data[0] = '\0';
    view->byte_limit = doc->content_capacity;
    view->bytes.limit = doc->content_capacity + 1;
    view->bytes.make_room = mdf_pager_view_make_room;
    view->bytes.room_userdata = view;
    view->first_line = first;
    view->collect_from = first;
    view->query = &doc->search->scan;
    view->search = doc->search;
    mdf_pager_query_restart(view->query, 1, checkpoint != NULL ? checkpoint->query_count : 0);
    view->bytes.discard = first != 0;
    view->bytes.cancel = doc->cancel;
    view->searchable.cancel = doc->cancel;
    view->eof_known = known;
    view->total_lines = total;
    memset(&doc->reflow, 0, sizeof(doc->reflow));
    memset(&doc->sanitizer, 0, sizeof(doc->sanitizer));
    doc->reflow.view = view;
    doc->reflow.width = width < 1 ? 1 : width;
    doc->reflow.styled = doc->markdown;
    mdf_pager_buffer_init_with_allocator(&doc->reflow.osc8_start, &view->allocator);
    mdf_pager_buffer_init_with_allocator(&doc->reflow.sgr_start, &view->allocator);
    doc->reflow.osc8_start.limit = 8192;
    doc->reflow.sgr_start.limit = 8192;
    mdf_pager_buffer_init_with_allocator(&doc->sanitized, &view->allocator);
    doc->sanitized.cancel = doc->cancel;
    doc->sanitized.limit = 8192;
    doc->sink.userdata = &doc->reflow;
    doc->sink.write = mdf_pager_reflow_sink_write;
    doc->offset = 0;
    doc->eof = 0;
    doc->source_eof = 0;
    doc->has_lookahead = 0;
    doc->pending_len = 0;
    doc->width = width;
    if (checkpoint != NULL) {
        doc->offset = checkpoint->offset;
        view->next_line = checkpoint->line;
        doc->reflow.col = checkpoint->col;
        view->query_column = checkpoint->query_column;
        memcpy(doc->pending, checkpoint->pending, sizeof(doc->pending));
        doc->pending_len = checkpoint->pending_len;
        view->bytes.discard = view->next_line < first;
        view->row_origin.source_offset = checkpoint->offset;
        view->input_offset = checkpoint->offset;
    }
    if (!doc->markdown) return MDF_OK;
    render_opts = *doc->opts;
    if (render_opts.ansi_mode == MDF_ANSI_AUTO) render_opts.ansi_mode = MDF_ANSI_ON;
    render_opts.width = mdf_pager_render_width(&render_opts, width);
    return mdf_create(MDF_FORMAT_ANSI, &render_opts, &doc->renderer);
}

/* Keep the last possible CR / UTF-8 unit across read boundaries. This is
 * transport state only; it does not defer a complete text rendering decision. */
static int mdf_pager_document_text(mdf_pager_document *doc, const char *src, size_t len, int final)
{
    char joined[1028];
    size_t complete;
    size_t start;
    size_t width;

    if (len > sizeof(joined) - doc->pending_len) return -1;
    memcpy(joined, doc->pending, doc->pending_len);
    memcpy(joined + doc->pending_len, src, len);
    len += doc->pending_len;
    complete = len;
    if (!final && len != 0) {
        if (joined[len - 1] == '\r') complete--;
        else {
            start = len - 1;
            while (start > 0 && len - start < 4 && ((unsigned char)joined[start] & 0xc0) == 0x80)
                start--;
            width = mdf_pager_sanitize_markdown_utf8_width((unsigned char)joined[start]);
            if (width > len - start) complete = start;
        }
    }
    if (mdf_pager_reflow_chunk(&doc->reflow, joined, complete) != 0) return -1;
    doc->pending_len = len - complete;
    memcpy(doc->pending, joined + complete, doc->pending_len);
    return 0;
}

static void mdf_pager_document_mark_anchors(mdf_pager_document *doc)
{
    size_t i;

    for (i = 0; i < 2; i++) {
        mdf_pager_anchor *anchor;

        anchor = doc->anchors[i];
        if (anchor != NULL && !anchor->replayed && doc->offset == anchor->source_offset) {
            anchor->replay_text_offset = doc->view->text_count;
            anchor->replayed = 1;
        }
    }
}

static mdf_status mdf_pager_document_read(mdf_pager_document *doc, size_t max_bytes)
{
    char chunk[1024];
    size_t len;
    size_t cap;
    size_t i;
    int err;
    mdf_status st;

    if (mdf_pager_cancel_check(doc->cancel)) return MDF_CANCELLED;
    if (mdf_pager_termination_signal != 0) return MDF_ERROR_IO;
    err = 0;
    cap = doc->scanning ? sizeof(chunk) :
        (doc->view->bytes.len >= doc->view->byte_limit / 2 ? 64 : 512);
    if (!doc->scanning && !doc->view->bytes.discard) {
        size_t remaining;

        remaining = doc->view->byte_limit - doc->view->bytes.len;
        if (remaining / 4 < cap) cap = remaining / 4;
        if (cap == 0) cap = 1;
    }
    if (max_bytes != 0 && cap > max_bytes) cap = max_bytes;
    /* Replay the exact source fence before comparing duplicate text. Splitting
     * a transport read preserves renderer decisions and needs no input copy. */
    mdf_pager_document_mark_anchors(doc);
    for (i = 0; i < 2; i++) {
        mdf_pager_anchor *anchor;

        anchor = doc->anchors[i];
        if (anchor != NULL && !anchor->replayed && anchor->source_offset > doc->offset &&
            anchor->source_offset - doc->offset < cap) cap = (size_t)(anchor->source_offset - doc->offset);
    }
    if (doc->has_lookahead) {
        chunk[0] = doc->lookahead;
        doc->has_lookahead = 0;
        len = 1;
    } else len = doc->source_eof ? 0 : doc->source->read(doc->source->userdata, chunk, cap, &err);
    if (err != 0 || len > cap) return MDF_ERROR_IO;
    doc->view->input_offset = doc->offset;
    doc->view->input_text_offset = doc->view->text_count;
    doc->offset += len;
    if (doc->markdown) {
        doc->sanitized.len = 0;
        if (mdf_pager_sanitize_markdown_bytes(&doc->sanitizer, &doc->sanitized, chunk, len) != 0 ||
            (len == 0 && mdf_pager_sanitize_markdown_pending(&doc->sanitizer, &doc->sanitized, 1) != 0))
            return MDF_ERROR_NOMEM;
        if (doc->sanitized.len != 0) {
            st = mdf_feed(doc->renderer, doc->sanitized.data, doc->sanitized.len, &doc->sink);
            if (st != MDF_OK) return doc->view->row_too_large ? MDF_ERROR_TERMINAL_SIZE : st;
        }
        if (len == 0) {
            st = mdf_finish_document(doc->renderer, &doc->sink);
            if (st != MDF_OK) return doc->view->row_too_large ? MDF_ERROR_TERMINAL_SIZE : st;
        }
    } else if (mdf_pager_document_text(doc, chunk, len, len == 0) != 0)
        return doc->view->row_too_large ? MDF_ERROR_TERMINAL_SIZE : MDF_ERROR_NOMEM;
    mdf_pager_document_mark_anchors(doc);
    if (len == 0) {
        doc->source_eof = 1;
        doc->eof = 1;
        doc->view->eof_known = 1;
        mdf_pager_query_finish(&doc->search->scan);
        doc->view->total_lines = doc->view->next_line + (doc->reflow.col != 0 ? 1 : 0);
        if (doc->view->total_lines == 0) doc->view->total_lines = 1;
    } else if (!doc->markdown && doc->checkpoints != NULL) {
        mdf_pager_checkpoint *checkpoint;
        size_t i;

        if (doc->search->active && (!doc->search->scan.prefix_known || doc->search->scan.matched != 0))
            return MDF_OK;
        if (doc->checkpoint_count != 0 && doc->view->next_line <
            doc->checkpoints[doc->checkpoint_count - 1].line + doc->checkpoint_stride) return MDF_OK;
        if (doc->checkpoint_count == MDF_PAGER_CHECKPOINTS) {
            for (i = 0; i < MDF_PAGER_CHECKPOINTS / 2; i++)
                doc->checkpoints[i] = doc->checkpoints[i * 2];
            doc->checkpoint_count /= 2;
            if (doc->checkpoint_stride < MDF_PAGER_POSITION_MAX / 2) doc->checkpoint_stride *= 2;
        }
        checkpoint = &doc->checkpoints[doc->checkpoint_count++];
        checkpoint->offset = doc->offset;
        checkpoint->line = doc->view->next_line;
        checkpoint->col = doc->reflow.col;
        memcpy(checkpoint->pending, doc->pending, sizeof(doc->pending));
        checkpoint->pending_len = doc->pending_len;
        checkpoint->query_generation = doc->search->scan.generation;
        checkpoint->query_count = doc->search->scan.count;
        checkpoint->query_column = doc->view->query_column;
    }
    return MDF_OK;
}

/* Probe EOF with bounded transport lookahead. Normally retain a non-EOF byte
 * for navigation; immediate text transport/viewport decisions may consume it. */
static mdf_status mdf_pager_document_probe(mdf_pager_document *doc, uint64_t required_last)
{
    size_t len;
    int err;

    if (doc->has_lookahead || doc->eof) return MDF_OK;
    if (mdf_pager_cancel_check(doc->cancel)) return MDF_CANCELLED;
    if (mdf_pager_termination_signal != 0) return MDF_ERROR_IO;
    err = 0;
    len = doc->source->read(doc->source->userdata, &doc->lookahead, 1, &err);
    if (len > 1 || err != 0) return MDF_ERROR_IO;
    if (len != 0) {
        mdf_status st;
        size_t extra;

        doc->has_lookahead = 1;
        /* Text has no undecided Markdown word. Resolve a pending transport
         * unit or the byte proving a full row/viewport cannot fit. This is
         * bounded by one UTF-8 unit, never by the remaining input length. */
        if (!doc->markdown && doc->view->next_line < required_last &&
            (doc->pending_len != 0 || doc->view->bytes.len == doc->content_capacity)) {
            st = mdf_pager_document_read(doc, 1);
            for (extra = 0; st == MDF_OK && !doc->eof && doc->pending_len != 0 && extra < 3; extra++)
                st = mdf_pager_document_read(doc, 1);
            return st;
        }
        return MDF_OK;
    }
    doc->source_eof = 1;
    return mdf_pager_document_read(doc, 0);
}

static int mdf_pager_view_reindex(mdf_pager_view *view)
{
    view->line_count = 0;
    view->searchable.len = 0;
    return mdf_pager_view_index(view) != 0 || mdf_pager_view_build_searchable(view) != 0 ? -1 : 0;
}

static int mdf_pager_viewport_fits(size_t capacity, size_t rows, int width)
{
    return width > 0 && (size_t)width <= capacity && rows <= capacity / (size_t)width;
}

static mdf_status mdf_pager_document_window(mdf_pager_document *doc, uint64_t *top,
                                            size_t rows, int width, int to_end)
{
    mdf_pager_view *view;
    mdf_status st;
    uint64_t wanted;
    uint64_t start_offset;
    size_t prefetch;

    view = doc->view;
    wanted = *top;
    if (!mdf_pager_viewport_fits(doc->content_capacity, rows, width))
        return MDF_ERROR_TERMINAL_SIZE;
    if (to_end && doc->search->active && !doc->search->scan.prefix_known) {
        st = mdf_pager_document_restart(doc, 0, width);
        if (st != MDF_OK) return st;
    }
    if (!to_end && doc->width == width && view->line_count != 0 &&
        wanted >= view->first_line && wanted + rows <= view->first_line + view->line_count &&
        (doc->eof || wanted + rows <= view->next_line))
        return MDF_OK;
    if (doc->width != width || wanted < view->first_line ||
        (view->full && wanted + rows > view->first_line + view->line_count)) {
        st = mdf_pager_document_restart(doc, wanted > rows / 4 ? wanted - rows / 4 : 0, width);
        if (st != MDF_OK) return st;
    }
    view->protected_first = to_end ? MDF_PAGER_POSITION_MAX : wanted;
    prefetch = doc->content_capacity - doc->prefetch_reserve;
    doc->scanning = to_end;
    start_offset = doc->offset;
    while (!doc->eof && !view->full &&
           (to_end || (doc->replaying && view->next_line <= wanted) || view->next_line < view->collect_from ||
            (doc->offset - start_offset < doc->content_capacity &&
             (view->next_line < wanted + rows || view->bytes.len < prefetch)))) {
        size_t max_bytes;
        int replaying;

        /* Recover the explicitly requested, previously rendered row before
         * starting read-ahead. New input, including an empty initial view,
         * stops at the allowance even while a parser decision is pending. */
        replaying = (doc->replaying && view->next_line <= wanted) || view->next_line < view->collect_from;
        max_bytes = !to_end && !replaying && doc->offset - start_offset < doc->content_capacity ?
            doc->content_capacity - (size_t)(doc->offset - start_offset) : 0;
        st = mdf_pager_document_read(doc, max_bytes);
        if (st != MDF_OK) return st;
        if (replaying) start_offset = doc->offset;
        if (view->next_line > wanted) doc->replaying = 0;
    }
    if (view->full) {
        /* A fitting document never needs this reserve. Once a decision has
         * overrun a window, leave decision room on later fills so ordinary
         * forward progress does not repeatedly lose renderer context. */
        doc->prefetch_reserve = doc->content_capacity / 4;
        if (doc->prefetch_reserve > 8192) doc->prefetch_reserve = 8192;
    }
    if (!to_end && !doc->eof && !view->full) {
        st = mdf_pager_document_probe(doc, wanted + rows);
        if (st != MDF_OK) return st;
    }
    if (mdf_pager_view_reindex(view) != 0) return MDF_ERROR_NOMEM;
    doc->scanning = 0;
    if (view->full && !to_end && wanted + rows > view->first_line + view->line_count)
        return MDF_ERROR_TERMINAL_SIZE;
    if (to_end && !doc->eof) {
        /* A single parser decision overran the cache. Continue the same parser
         * to EOF; only recovery of the final evicted window needs replay. */
        while (!doc->eof) {
            st = mdf_pager_document_read(doc, 0);
            if (st != MDF_OK) return st;
        }
    }
    if (view->eof_known && (to_end || wanted + rows > view->total_lines))
        wanted = view->total_lines > rows ? view->total_lines - rows : 0;
    /* A read-ahead pause can leave fewer rows than the screen, or none yet.
     * Keep the live parser and the retained prefix; the next scroll resumes it. */
    if (!to_end && !doc->eof && !view->full && wanted >= view->first_line + view->line_count)
        wanted = view->first_line + view->line_count - 1;
    if (wanted < view->first_line || wanted >= view->first_line + view->line_count) {
        st = mdf_pager_document_restart(doc, wanted > rows / 4 ? wanted - rows / 4 : 0, width);
        if (st != MDF_OK) return st;
        st = mdf_pager_document_window(doc, &wanted, rows, width, 0);
        if (st == MDF_OK) *top = wanted;
        return st;
    }
    *top = wanted;
    return MDF_OK;
}

/* Scan visible codepoints with query-sized state. A match may span more styled
 * bytes than the cache: only its two positions and ordinal survive eviction. */
static mdf_status mdf_pager_search_step(mdf_pager_document *doc, mdf_pager_search *search,
                                        uint64_t *top, size_t rows, int width, int backward)
{
    mdf_pager_view *view;
    mdf_pager_query_position origin;
    mdf_pager_query_position cursor;
    mdf_pager_query_position pending;
    mdf_pager_query_match match;
    uint64_t original_top;
    uint64_t first;
    uint64_t scan_first;
    size_t i;
    int order;
    int restart;
    mdf_status st;

    view = doc->view;
    original_top = *top;
    origin.row = *top;
    origin.column = 0;
    if (search->have_selection) origin = search->selection.start;
    else if (search->count != 0)
        origin = mdf_pager_search_at(view, search->matches[search->selected].start);
    search->inclusive = !search->have_selection && search->count == 0;
    /* Complete cached matches remain a cheap path. The origin must be in the
     * cache, so a preceding match cannot have been lost across its near seam. */
    if (origin.row >= view->first_line && origin.row < view->first_line + view->line_count) {
        for (i = 0; i < search->count; i++) {
            size_t index;

            index = backward ? search->count - i - 1 : i;
            match = mdf_pager_search_cached_match(view, search, index);
            order = mdf_pager_query_position_compare(match.start, origin);
            if (backward ? order < 0 : (order > 0 || (order == 0 && search->inclusive))) {
                search->selected = index;
                search->have_selection = 0;
                *top = match.start.row;
                return MDF_OK;
            }
        }
    }
    if (search->count != 0 && doc->eof && view->first_line == 0 && view->line_count >= view->total_lines) {
        search->selected = backward ? search->count - 1 : 0;
        search->have_selection = 0;
        *top = mdf_pager_view_line_for_offset(view, search->matches[search->selected].start);
        return MDF_OK;
    }
    search->origin = origin;
    search->backward = backward;
    search->wrapped = 0;
    search->have_candidate = 0;
    search->seeking = 1;
    first = backward && origin.row > doc->content_capacity ? origin.row - doc->content_capacity : 0;
    restart = backward || view->full || !search->scan.prefix_known ||
        (search->scan.have_last && mdf_pager_query_position_compare(search->scan.last.start, origin) > 0);
    if (restart) {
        st = mdf_pager_document_restart(doc, backward ? first : origin.row, width);
        if (st != MDF_OK) goto failed;
    }
    scan_first = view->next_line;
    for (;;) {
        view->protected_first = MDF_PAGER_POSITION_MAX;
        doc->scanning = 1;
        while (!doc->eof) {
            cursor.row = view->next_line;
            cursor.column = view->query_column;
            pending = mdf_pager_query_pending(&search->scan);
            if (!backward && search->have_candidate) break;
            if ((backward != search->wrapped) &&
                mdf_pager_query_position_compare(cursor, origin) > 0 &&
                mdf_pager_query_position_compare(pending, origin) > 0) break;
            st = mdf_pager_document_read(doc, 0);
            if (st != MDF_OK) goto failed;
        }
        if (search->have_candidate) break;
        if (backward && !search->wrapped && scan_first != 0) {
            first = scan_first > doc->content_capacity ? scan_first - doc->content_capacity : 0;
            st = mdf_pager_document_restart(doc, first, width);
            if (st != MDF_OK) goto failed;
            scan_first = view->next_line;
            continue;
        }
        if (search->wrapped) break;
        search->wrapped = 1;
        /* Start the cyclic pass at BOF in both directions. Resolving the
         * origin may already have decoded eligible wrapped matches. Backward
         * traversal retains the final one, including a sole oversized match. */
        st = mdf_pager_document_restart(doc, 0, width);
        if (st != MDF_OK) goto failed;
    }
    search->seeking = 0;
    doc->scanning = 0;
    if (search->have_candidate) {
        search->selection = search->candidate;
        search->have_selection = 1;
        *top = search->selection.start.row;
    } else {
        search->have_selection = 0;
        *top = original_top;
    }
    if (mdf_pager_view_reindex(view) != 0) return MDF_ERROR_NOMEM;
    st = mdf_pager_document_window(doc, top, rows, width, 0);
    if (st != MDF_OK) return st;
    return mdf_pager_search_refresh(search, view) == 0 ? MDF_OK : MDF_ERROR_NOMEM;
failed:
    search->seeking = 0;
    doc->scanning = 0;
    return st;
}

/* Reflow to a source neighbourhood, preserving the one content allocation. */
static mdf_status mdf_pager_document_reflow(mdf_pager_document *doc, mdf_pager_anchor *anchor,
                                            int width, size_t rows)
{
    uint64_t first;
    uint64_t checked_text_count;
    size_t offset;
    int found;
    mdf_status st;

    checked_text_count = 0;
    st = mdf_pager_document_restart(doc, 0, width);
    if (st != MDF_OK) return st;
    if (anchor->complete_cache || anchor->source_offset == 0) {
        first = 0;
        st = mdf_pager_document_window(doc, &first, rows, width, 0);
        if (st != MDF_OK) return st;
        if (doc->eof && !doc->view->full) return MDF_OK;
        if (anchor->replayed && doc->view->retained_text_end > anchor->replay_text_offset &&
            anchor->text_offset < doc->view->retained_text_end - anchor->replay_text_offset) {
            found = mdf_pager_anchor_find(anchor, doc->view, 0, &offset);
            if (found < 0) return MDF_ERROR_NOMEM;
            if (found) return MDF_OK;
        }
        /* Reflow can make a fully cached document exceed the allowance.
         * Replay with a rolling cache instead of keeping only its prefix;
         * the original passage can be beyond that first new window. */
        st = mdf_pager_document_restart(doc, 0, width);
        if (st != MDF_OK) return st;
    }
    doc->view->protected_first = MDF_PAGER_POSITION_MAX;
    while (!doc->eof) {
        /* Resolve the captured text, rather than filling a fixed source tail
         * that could evict the passage from a small cache before anchoring. */
        if (anchor->replayed && doc->view->text_count >= anchor->replay_text_offset &&
            doc->view->text_count - anchor->replay_text_offset >= anchor->text_offset &&
            doc->view->text_count - anchor->replay_text_offset - anchor->text_offset >= 64 &&
            doc->view->text_count != checked_text_count) {
            /* Fragmented input can leave the same word undecided for many
             * feeds. Reindex only when visible output actually advances. */
            checked_text_count = doc->view->text_count;
            if (mdf_pager_view_reindex(doc->view) != 0) { st = MDF_ERROR_NOMEM; break; }
            found = mdf_pager_anchor_find(anchor, doc->view, 1, &offset);
            if (found < 0) { st = MDF_ERROR_NOMEM; break; }
            if (found) break;
        }
        /* The original decision was emitted within one input chunk. Do not
         * scan an arbitrary blank suffix merely to extend its text context. */
        if (anchor->source_offset != 0 && doc->offset >= anchor->source_offset &&
            doc->offset - anchor->source_offset >= 1024) break;
        doc->scanning = doc->offset < anchor->source_offset && anchor->source_offset - doc->offset > 1024;
        st = mdf_pager_document_read(doc, anchor->replayed ? 64 : 0);
        if (st != MDF_OK) break;
    }
    doc->scanning = 0;
    if (st == MDF_OK && mdf_pager_view_reindex(doc->view) != 0) st = MDF_ERROR_NOMEM;
    return st;
}

/* Restore a match at its reflowed passage, not at its former global rank.
 * The query matcher supplies the new ordinal, even for an extent larger than
 * the cache. Resolving it must not move the independently anchored viewport. */
static mdf_status mdf_pager_search_restore(mdf_pager_document *doc, mdf_pager_query_position position,
                                           uint64_t *top, size_t rows, int width)
{
    mdf_pager_search *search;
    mdf_pager_view *view;
    mdf_pager_query_position cursor;
    mdf_pager_query_position pending;
    mdf_pager_query_match match;
    size_t offset;
    size_t low;
    size_t high;
    size_t i;
    mdf_status st;

    search = doc->search;
    view = doc->view;
    if (mdf_pager_search_refresh(search, view) != 0) return MDF_ERROR_NOMEM;
    offset = mdf_pager_search_offset(view, position);
    low = 0;
    high = search->count;
    while (low < high) {
        i = low + (high - low) / 2;
        if (search->matches[i].start < offset) low = i + 1;
        else high = i;
    }
    if (low < search->count && search->matches[low].start == offset) {
        match = mdf_pager_search_cached_match(view, search, low);
        if (mdf_pager_query_position_compare(match.start, position) == 0) {
            search->selected = low;
            search->selection = match;
            search->have_selection = 1;
            return MDF_OK;
        }
    }
    if (search->scan.have_last &&
        mdf_pager_query_position_compare(search->scan.last.start, position) == 0) {
        search->selection = search->scan.last;
        search->have_selection = 1;
        return MDF_OK;
    }
    if ((search->scan.total_known && search->scan.total == 0) ||
        (doc->eof && view->first_line == 0 && view->line_count >= view->total_lines)) {
        search->have_selection = 0;
        mdf_pager_search_select_anchor(search, view, *top, 0);
        return MDF_OK;
    }
    search->origin = position;
    search->inclusive = 1;
    search->wrapped = 0;
    search->backward = 0;
    search->have_candidate = 0;
    search->have_selection = 0;
    search->seeking = 1;
    cursor.row = view->next_line;
    cursor.column = view->query_column;
    pending = mdf_pager_query_pending(&search->scan);
    st = MDF_OK;
    if (doc->eof || (mdf_pager_query_position_compare(cursor, position) > 0 &&
                     mdf_pager_query_position_compare(pending, position) > 0))
        st = mdf_pager_document_restart(doc, 0, width);
    view->protected_first = MDF_PAGER_POSITION_MAX;
    doc->scanning = 1;
    while (st == MDF_OK && !doc->eof && !search->have_candidate) {
        cursor.row = view->next_line;
        cursor.column = view->query_column;
        pending = mdf_pager_query_pending(&search->scan);
        if (mdf_pager_query_position_compare(cursor, position) > 0 &&
            mdf_pager_query_position_compare(pending, position) > 0) break;
        st = mdf_pager_document_read(doc, 0);
    }
    search->seeking = 0;
    doc->scanning = 0;
    if (st != MDF_OK) return st;
    if (search->have_candidate &&
        mdf_pager_query_position_compare(search->candidate.start, position) == 0) {
        search->selection = search->candidate;
        search->have_selection = 1;
    }
    if (mdf_pager_view_reindex(view) != 0) return MDF_ERROR_NOMEM;
    st = mdf_pager_document_window(doc, top, rows, width, 0);
    if (st != MDF_OK) return st;
    if (mdf_pager_search_refresh(search, view) != 0) return MDF_ERROR_NOMEM;
    if (!search->have_selection) mdf_pager_search_select_anchor(search, view, *top, 0);
    return MDF_OK;
}

static mdf_status mdf_pager_document_resize(mdf_pager_document *doc, uint64_t *top,
                                            size_t rows, int width)
{
    mdf_pager_anchor viewport;
    mdf_pager_anchor selected_anchor;
    mdf_pager_query_match selected;
    mdf_pager_query_position position;
    mdf_pager_view *view;
    mdf_pager_search *search;
    uint64_t old_top;
    size_t offset;
    size_t raw_offset;
    int have_selection;
    int found;
    mdf_status st;

    if (!mdf_pager_viewport_fits(doc->content_capacity, rows, width)) return MDF_ERROR_TERMINAL_SIZE;
    view = doc->view;
    search = doc->search;
    memset(&viewport, 0, sizeof(viewport));
    memset(&selected_anchor, 0, sizeof(selected_anchor));
    mdf_pager_buffer_init(&viewport.text, &doc->cache_opts);
    mdf_pager_buffer_init(&selected_anchor.text, &doc->cache_opts);
    viewport.text.limit = selected_anchor.text.limit = 129;
    have_selection = search->active && (search->have_selection || search->count != 0);
    memset(&selected, 0, sizeof(selected));
    if (have_selection) selected = search->have_selection ? search->selection :
        mdf_pager_search_cached_match(view, search, search->selected);
    raw_offset = view->lines[(size_t)(*top - view->first_line)];
    st = MDF_ERROR_NOMEM;
    if (mdf_pager_anchor_capture(&viewport, view, raw_offset) != 0) goto done;
    if (have_selection) {
        if (selected.start.row < view->first_line || selected.start.row >= view->first_line + view->line_count) {
            old_top = selected.start.row > 2 ? selected.start.row - 2 : 0;
            st = mdf_pager_document_window(doc, &old_top, 1, doc->width, 0);
            if (st != MDF_OK) goto done;
            if (selected.start.row < view->first_line || selected.start.row >= view->first_line + view->line_count) {
                old_top = selected.start.row;
                st = mdf_pager_document_window(doc, &old_top, 1, doc->width, 0);
                if (st != MDF_OK) goto done;
            }
        }
        raw_offset = mdf_pager_search_offset(view, selected.start);
        st = MDF_ERROR_NOMEM;
        if (mdf_pager_anchor_capture(&selected_anchor, view, raw_offset) != 0) goto done;
    }
    search->have_selection = 0;
    doc->anchors[0] = &viewport;
    doc->anchors[1] = have_selection ? &selected_anchor : NULL;
    st = mdf_pager_document_reflow(doc, &viewport, width, rows);
    if (st != MDF_OK) goto done;
    found = mdf_pager_anchor_find(&viewport, view, 1, &offset);
    if (found < 0) { st = MDF_ERROR_NOMEM; goto done; }
    *top = view->first_line + mdf_pager_view_top_for_anchor(view, offset);
    found = 0;
    if (have_selection) {
        found = mdf_pager_anchor_find(&selected_anchor, view, 0, &offset);
        if (found < 0) { st = MDF_ERROR_NOMEM; goto done; }
        if (!found && (!doc->eof || view->full) &&
                      (selected_anchor.source_offset != viewport.source_offset ||
                       selected_anchor.text_offset != viewport.text_offset)) {
            st = mdf_pager_document_reflow(doc, &selected_anchor, width, rows);
            if (st != MDF_OK) goto done;
            found = mdf_pager_anchor_find(&selected_anchor, view, 0, &offset);
            if (found < 0) { st = MDF_ERROR_NOMEM; goto done; }
        }
    }
    if (found && offset < view->searchable.len) {
        position = mdf_pager_search_at(view, view->search_offsets[offset]);
        st = mdf_pager_search_restore(doc, position, top, rows, width);
    } else {
        st = mdf_pager_document_window(doc, top, rows, width, 0);
        if (st == MDF_OK && search->active) {
            if (mdf_pager_search_refresh(search, view) != 0) st = MDF_ERROR_NOMEM;
            else mdf_pager_search_select_anchor(search, view, *top, 0);
        }
    }
done:
    doc->anchors[0] = doc->anchors[1] = NULL;
    mdf_pager_buffer_destroy(&selected_anchor.text);
    mdf_pager_buffer_destroy(&viewport.text);
    return st;
}

static mdf_status mdf_pager_run(const char *name, mdf_seekable_source *source,
                                const mdf_options *render_options, mdf_pager_format format,
                                const mdf_pager_options *pager_options, mdf_pager_cancel_state *cancel)
{
    struct termios saved_termios;
    struct sigaction old_winch;
    struct sigaction winch_action;
    sigset_t saved_termination_mask;
    mdf_pager_termination_handlers termination_handlers;
    mdf_pager_view view;
    mdf_pager_search search;
    mdf_pager_document document;
    mdf_pager_cache_allocator cache;
    mdf_options cache_opts;
    mdf_options opts;
    int columns;
    int rows;
    uint64_t top;
    uint64_t prepared_top;
    sig_atomic_t seen_winch;
    struct timespec last_winch;
    int resize_pending;
    int entered_terminal;
    int raw_mode;
    int handler_installed;
    int termination_handlers_installed;
    int termination_signals_blocked;
    int termination_signal = 0;
    int redraw;
    mdf_status result;

    if (source == NULL || !mdf_pager_arguments_valid(name, render_options, format)) {
        return MDF_ERROR_INVALID;
    }
    memset(&view, 0, sizeof(view));
    memset(&search, 0, sizeof(search));
    memset(&document, 0, sizeof(document));
    memset(&cache, 0, sizeof(cache));
    memset(&termination_handlers, 0, sizeof(termination_handlers));
    entered_terminal = 0;
    raw_mode = 0;
    handler_installed = 0;
    termination_handlers_installed = 0;
    termination_signals_blocked = 0;
    mdf_options_init(&opts);
    if (render_options != NULL) opts = *render_options;
    mdf_allocator_prepare(&opts.allocator);
    cache.parent = opts.allocator;
    document.content_capacity = pager_options == NULL || pager_options->cache_budget == 0 ?
        MDF_PAGER_DEFAULT_CACHE : pager_options->cache_budget;
    /* Index/search allocations have a separate, file-size-independent bound,
     * including geometric growth. None of this reduces content capacity. */
    cache.budget = document.content_capacity > ((size_t)-1 - 32768) / 64 ? (size_t)-1 :
        document.content_capacity * 64 + 32768;
    cache_opts = opts;
    memset(&cache_opts.allocator, 0, sizeof(cache_opts.allocator));
    cache_opts.allocator.userdata = &cache;
    cache_opts.allocator.alloc = mdf_pager_cache_alloc;
    cache_opts.allocator.free = mdf_pager_cache_free;
    mdf_pager_search_init(&search, &cache_opts);
    search.query.limit = 4096;
    document.source = source;
    document.view = &view;
    document.search = &search;
    document.opts = &opts;
    document.cache_opts = cache_opts;
    document.cancel = cancel;
    document.markdown = format == MDF_PAGER_FORMAT_MARKDOWN ||
        (format == MDF_PAGER_FORMAT_AUTO && mdf_pager_is_markdown_path(name));
    document.checkpoint_stride = 64;
    if (mdf_pager_cancel_check(cancel)) {
        result = MDF_CANCELLED;
        goto done;
    }
    if (!document.markdown) {
        document.checkpoints = (mdf_pager_checkpoint *)mdf_alloc(&cache_opts.allocator,
            MDF_PAGER_CHECKPOINTS * sizeof(*document.checkpoints));
        if (document.checkpoints == NULL) {
            result = MDF_ERROR_NOMEM;
            goto done;
        }
    }
    mdf_pager_termination_signal = 0;
    if (mdf_pager_install_termination_handlers(&termination_handlers) != 0) {
        result = MDF_ERROR_IO;
        goto done;
    }
    termination_handlers_installed = 1;
    if (mdf_pager_set_raw_mode(&saved_termios) != 0) {
        result = MDF_ERROR_IO;
        goto done;
    }
    raw_mode = 1;
    memset(&winch_action, 0, sizeof(winch_action));
    winch_action.sa_handler = mdf_pager_on_winch;
    sigemptyset(&winch_action.sa_mask);
    if (sigaction(SIGWINCH, &winch_action, &old_winch) != 0) {
        result = MDF_ERROR_IO;
        goto done;
    }
    handler_installed = 1;
    if (mdf_pager_write("\033[?1049h\033[?25l", strlen("\033[?1049h\033[?25l")) != 0) {
        result = MDF_ERROR_IO;
        goto done;
    }
    entered_terminal = 1;
    if (mdf_pager_termination_signal != 0) {
        result = MDF_ERROR_IO;
        goto done;
    }
    seen_winch = mdf_pager_winch_count;
    mdf_pager_window_size(&columns, &rows);
    if (!mdf_pager_viewport_fits(document.content_capacity, (size_t)(rows - 1), columns)) {
        result = MDF_ERROR_TERMINAL_SIZE;
        goto done;
    }
    top = 0;
    result = mdf_pager_document_restart(&document, 0, columns);
    if (result == MDF_OK)
        result = mdf_pager_document_window(&document, &top, (size_t)(rows - 1), columns, 0);
    if (result != MDF_OK) goto done;
    prepared_top = top;
    resize_pending = mdf_pager_winch_count != seen_winch;
    if (resize_pending) {
        mdf_pager_note_resize(&seen_winch, &resize_pending, &last_winch);
    }
    redraw = 1;
    for (;;) {
        mdf_pager_key key;
        size_t content_rows;
        uint64_t max_top;
        long wait_ms;
        unsigned char ready_byte;
        int ready;

        if (mdf_pager_cancel_check(cancel)) {
            result = MDF_CANCELLED;
            goto done;
        }
        if (mdf_pager_termination_signal != 0) {
            result = MDF_ERROR_IO;
            goto done;
        }
        if (redraw) {
            uint64_t previous_first;
            size_t previous_len;
            uint64_t selected_line;
            size_t selected_column;
            int had_selection;

            previous_first = view.first_line;
            previous_len = view.bytes.len;
            had_selection = search.active && search.count != 0;
            selected_line = 0;
            selected_column = 0;
            if (had_selection) {
                size_t local_line;

                local_line = mdf_pager_view_line_for_offset(&view, search.matches[search.selected].start);
                selected_line = view.first_line + local_line;
                selected_column = search.matches[search.selected].start - view.lines[local_line];
            }
            result = top == prepared_top ? MDF_OK :
                mdf_pager_document_window(&document, &top, (size_t)(rows - 1), columns, 0);
            if (result != MDF_OK) goto done;
            prepared_top = top;
            if (search.active && (previous_first != view.first_line || previous_len != view.bytes.len)) {
                if (mdf_pager_search_refresh(&search, &view) != 0) {
                    result = MDF_ERROR_NOMEM;
                    goto done;
                }
                if (had_selection) {
                    if (selected_line < view.first_line || selected_line >= view.first_line + view.line_count) {
                        selected_line = top;
                        selected_column = 0;
                    }
                    mdf_pager_search_select_anchor(&search, &view, selected_line, selected_column);
                }
                if (search.have_selection &&
                    (search.selection.end.row < view.first_line ||
                     search.selection.start.row >= view.first_line + view.line_count))
                    search.have_selection = 0;
            }
            result = mdf_pager_draw(&view, name, &opts, &search, columns, rows,
                                     (size_t)(top - view.first_line));
            if (result != MDF_OK) goto done;
            redraw = 0;
        }
        if (mdf_pager_winch_count != seen_winch) {
            mdf_pager_note_resize(&seen_winch, &resize_pending, &last_winch);
        }
        content_rows = (size_t)(rows - 1);
        max_top = view.eof_known ?
            (view.total_lines > content_rows ? view.total_lines - content_rows : 0) : MDF_PAGER_POSITION_MAX - content_rows;
        wait_ms = cancel != NULL && cancel->control != NULL && cancel->control->should_cancel != NULL ? 50 : 1000;
        if (resize_pending) {
            long resize_wait;
            resize_wait = mdf_pager_resize_wait_ms(&last_winch);
            if (resize_wait < wait_ms) wait_ms = resize_wait;
        }
        ready = mdf_pager_read_with_timeout(&ready_byte, wait_ms);
        if (mdf_pager_cancel_check(cancel)) {
            result = MDF_CANCELLED;
            goto done;
        }
        if (mdf_pager_termination_signal != 0) {
            result = MDF_ERROR_IO;
            goto done;
        }
        if (mdf_pager_winch_count != seen_winch) {
            mdf_pager_note_resize(&seen_winch, &resize_pending, &last_winch);
            continue;
        }
        if (resize_pending && ready == 0 && mdf_pager_resize_wait_ms(&last_winch) == 0) {
            mdf_pager_window_size(&columns, &rows);
            result = mdf_pager_document_resize(&document, &top, (size_t)(rows - 1), columns);
            if (result != MDF_OK) goto done;
            /* Locating an anchor can finish before its viewport is full. */
            prepared_top = MDF_PAGER_POSITION_MAX;
            resize_pending = 0;
            redraw = 1;
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            result = MDF_ERROR_IO;
            goto done;
        }
        if (ready == 0) continue;
        if (search.editing) {
            if (ready_byte == '\r' || ready_byte == '\n') {
                search.editing = 0;
                search.active = search.query.len != 0 &&
                    mdf_pager_utf8_complete_prefix(search.query.data, search.query.len) == search.query.len;
                if (search.active && (search.scan.length == 0 ? mdf_pager_search_begin(&search, &view) :
                                      mdf_pager_search_refresh(&search, &view)) != 0) {
                    result = MDF_ERROR_NOMEM;
                    goto done;
                }
                if (search.count != 0) {
                    top = view.first_line + mdf_pager_view_line_for_offset(&view, search.matches[0].start);
                    prepared_top = top;
                }
                redraw = 1;
                continue;
            }
            if (ready_byte == 'q' || ready_byte == 0x1b) {
                mdf_pager_search_reset(&search);
                redraw = 1;
                continue;
            }
            if (ready_byte == 8 || ready_byte == 127) {
                mdf_pager_search_backspace(&search);
                if (mdf_pager_search_update_live(&search, &view) != 0) {
                    result = MDF_ERROR_NOMEM;
                    goto done;
                }
                if (search.active && search.count != 0) {
                    top = view.first_line + mdf_pager_view_line_for_offset(&view, search.matches[0].start);
                    prepared_top = top;
                }
                redraw = 1;
                continue;
            }
            if (ready_byte >= 32 && ready_byte != 127 &&
                mdf_pager_buffer_append_byte(&search.query, ready_byte) != 0) {
                result = MDF_ERROR_NOMEM;
                goto done;
            }
            if (ready_byte >= 32 && ready_byte != 127) {
                if (mdf_pager_utf8_complete_prefix(search.query.data, search.query.len) != search.query.len) {
                    continue;
                }
                if (mdf_pager_search_update_live(&search, &view) != 0) {
                    result = MDF_ERROR_NOMEM;
                    goto done;
                }
                if (search.active && search.count != 0) {
                    top = view.first_line + mdf_pager_view_line_for_offset(&view, search.matches[0].start);
                    prepared_top = top;
                }
                redraw = 1;
            }
            continue;
        }
        if (search.active) {
            if (ready_byte == 'q' || ready_byte == 0x1b) {
                mdf_pager_search_reset(&search);
                redraw = 1;
                continue;
            }
            if (ready_byte == 'n') {
                result = mdf_pager_search_step(&document, &search, &top, content_rows, columns, 0);
                if (result != MDF_OK) goto done;
                redraw = 1;
                continue;
            }
            if (ready_byte == 'p' || ready_byte == 'N') {
                result = mdf_pager_search_step(&document, &search, &top, content_rows, columns, 1);
                if (result != MDF_OK) goto done;
                redraw = 1;
                continue;
            }
        }
        if (ready_byte == '/') {
            mdf_pager_search_reset(&search);
            search.editing = 1;
            redraw = 1;
            continue;
        }
        key = mdf_pager_read_key(ready_byte, cancel);
        if (key == MDF_PAGER_KEY_QUIT) break;
        if (key == MDF_PAGER_KEY_DOWN && top < max_top) {
            top++;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_UP && top > 0) {
            top--;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_PAGE_DOWN) {
            top = top + content_rows > max_top ? max_top : top + content_rows;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_PAGE_UP) {
            top = top > content_rows ? top - content_rows : 0;
            redraw = 1;
        }
        else if (key == MDF_PAGER_KEY_HALF_DOWN) {
            size_t amount;

            amount = content_rows / 2;
            if (amount == 0) amount = 1;
            top = top + amount > max_top ? max_top : top + amount;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_HALF_UP) {
            size_t amount;

            amount = content_rows / 2;
            if (amount == 0) amount = 1;
            top = top > amount ? top - amount : 0;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_HOME) {
            top = 0;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_END) {
            search.have_selection = 0;
            result = mdf_pager_document_window(&document, &top, content_rows, columns, 1);
            if (result != MDF_OK) goto done;
            if (search.active) {
                if (mdf_pager_search_refresh(&search, &view) != 0) {
                    result = MDF_ERROR_NOMEM;
                    goto done;
                }
                mdf_pager_search_select_anchor(&search, &view, top, 0);
            }
            redraw = 1;
        }
    }
    result = MDF_OK;

done:
    if (cancel != NULL && cancel->requested) result = MDF_CANCELLED;
    if (termination_handlers_installed &&
        mdf_pager_block_termination_signals(&saved_termination_mask) == 0) {
        termination_signals_blocked = 1;
    }
    if (entered_terminal) {
        (void)mdf_pager_write("\033[0m\033[?25h\033[?1049l", strlen("\033[0m\033[?25h\033[?1049l"));
    }
    if (handler_installed) (void)sigaction(SIGWINCH, &old_winch, NULL);
    if (raw_mode) (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    if (termination_handlers_installed) {
        termination_signal = mdf_pager_termination_signal;
        mdf_pager_restore_termination_handlers(&termination_handlers);
    }
    if (termination_signals_blocked) {
        (void)sigprocmask(SIG_SETMASK, &saved_termination_mask, NULL);
    }
    mdf_pager_search_destroy(&search);
    mdf_pager_document_stop(&document);
    mdf_free_mem(&cache_opts.allocator, document.checkpoints,
                 MDF_PAGER_CHECKPOINTS * sizeof(*document.checkpoints));
    mdf_pager_view_destroy(&view);
    mdf_pager_release_emission_buffer(&opts);
    if (termination_signal != 0) {
        /* An embedding application may handle or ignore the forwarded SIGINT.
         * If it returns, report cancellation rather than an I/O failure. */
        if (termination_signal == SIGINT) result = MDF_CANCELLED;
        (void)kill(getpid(), (int)termination_signal);
    }
    return result;
}

typedef struct mdf_pager_file_source {
    int fd;
    off_t length;
    off_t position;
} mdf_pager_file_source;

static size_t mdf_pager_file_read(void *userdata, char *dst, size_t cap, int *err)
{
    mdf_pager_file_source *source;
    ssize_t n;

    source = (mdf_pager_file_source *)userdata;
    *err = 0;
    if (source->position >= source->length) return 0;
    if ((uint64_t)cap > (uint64_t)(source->length - source->position))
        cap = (size_t)(source->length - source->position);
    do {
        n = read(source->fd, dst, cap);
    } while (n < 0 && errno == EINTR && mdf_pager_termination_signal == 0);
    if (n < 0) {
        *err = errno;
        return 0;
    }
    source->position += n;
    return (size_t)n;
}

static int mdf_pager_file_seek(void *userdata, const mdf_source_offset *offset)
{
    mdf_pager_file_source *source;
    uint64_t position;

    source = (mdf_pager_file_source *)userdata;
    if (offset->high > 4294967295UL || offset->low > 4294967295UL) return -1;
    position = ((uint64_t)offset->high << 32) | offset->low;
    if (position > (uint64_t)source->length) return -1;
    if (lseek(source->fd, (off_t)position, SEEK_SET) == (off_t)-1) return -1;
    source->position = (off_t)position;
    return 0;
}

mdf_status mdf_pager_file_with_options(const char *path, const mdf_options *render_options,
                                       mdf_pager_format format, const mdf_pager_control *control,
                                       const mdf_pager_options *options)
{
    struct stat st;
    mdf_pager_file_source file;
    mdf_seekable_source source;
    mdf_status result;
    int fd;

    if (!mdf_pager_arguments_valid(path, render_options, format)) {
        return MDF_ERROR_INVALID;
    }
    result = mdf_pager_validate_options(render_options);
    if (result != MDF_OK) return result;
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return MDF_ERROR_IO;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return MDF_ERROR_INVALID;
    }
    memset(&file, 0, sizeof(file));
    file.fd = fd;
    file.length = st.st_size;
    source.userdata = &file;
    source.read = mdf_pager_file_read;
    source.seek = mdf_pager_file_seek;
    result = mdf_pager_seekable_source(path, &source, render_options, format, control, options);
    if (close(fd) != 0 && result == MDF_OK) result = MDF_ERROR_IO;
    return result;
}

mdf_status mdf_pager_file(const char *path, const mdf_options *render_options, mdf_pager_format format,
                          const mdf_pager_control *control)
{
    return mdf_pager_file_with_options(path, render_options, format, control, NULL);
}

mdf_status mdf_pager_seekable_source(const char *name, mdf_seekable_source *source,
                                     const mdf_options *render_options, mdf_pager_format format,
                                     const mdf_pager_control *control,
                                     const mdf_pager_options *options)
{
    mdf_pager_cancel_state cancel;
    mdf_status result;
    size_t minimum;

    minimum = MDF_PAGER_MIN_CACHE;
#ifdef LIBMDF_PAGER_TESTING
    if (mdf_pager_test_small_cache) minimum = 1;
#endif

    if (source == NULL || source->read == NULL || source->seek == NULL ||
        !mdf_pager_arguments_valid(name, render_options, format) ||
        (options != NULL && (options->cache_budget == (size_t)-1 ||
         (options->cache_budget != 0 && options->cache_budget < minimum))))
        return MDF_ERROR_INVALID;
    result = mdf_pager_validate_options(render_options);
    if (result != MDF_OK) return result;
    memset(&cancel, 0, sizeof(cancel));
    cancel.control = control;
    return mdf_pager_run(name, source, render_options, format, options, &cancel);
}

static int mdf_pager_buffer_source_seek(void *userdata, const mdf_source_offset *offset)
{
    mdf_pager_buffer_source *source;

    source = (mdf_pager_buffer_source *)userdata;
    if (offset->high != 0 || offset->low > source->buffer->len) return -1;
    source->offset = (size_t)offset->low;
    return 0;
}

mdf_status mdf_pager_source(const char *name, mdf_source *source,
                            const mdf_options *render_options, mdf_pager_format format,
                            const mdf_pager_control *control)
{
    mdf_pager_buffer input;
    mdf_pager_buffer_source buffered;
    mdf_seekable_source seekable;
    mdf_options opts;
    mdf_status result;
    mdf_pager_cancel_state cancel;

    if (source == NULL || source->read == NULL ||
        !mdf_pager_arguments_valid(name, render_options, format)) {
        return MDF_ERROR_INVALID;
    }
    result = mdf_pager_validate_options(render_options);
    if (result != MDF_OK) return result;
    mdf_options_init(&opts);
    if (render_options != NULL) opts = *render_options;
    mdf_allocator_prepare(&opts.allocator);
    mdf_pager_buffer_init(&input, &opts);
    input.limit = MDF_PAGER_DEFAULT_CACHE + 1;
    memset(&cancel, 0, sizeof(cancel));
    cancel.control = control;
    input.cancel = &cancel;
    result = mdf_pager_read_source(source, &input);
    if (cancel.requested) result = MDF_CANCELLED;
    if (result != MDF_OK) {
        mdf_pager_buffer_destroy(&input);
        mdf_pager_release_emission_buffer(&opts);
        return result;
    }
    memset(&buffered, 0, sizeof(buffered));
    buffered.buffer = &input;
    seekable.userdata = &buffered;
    seekable.read = mdf_pager_buffer_source_read;
    seekable.seek = mdf_pager_buffer_source_seek;
    result = mdf_pager_run(name, &seekable, &opts, format, NULL, &cancel);
    mdf_pager_buffer_destroy(&input);
    return result;
}
