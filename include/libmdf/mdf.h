/**
 * @file mdf.h
 * @brief Public C89/POSIX API for Markdown rendering and interactive paging.
 *
 * Include this file or <libmdf.h>. Initialize custom options with
 * mdf_options_init, create a receiver with mdf_create, and release its strings
 * with string_free before calling destroy. The options record is copied;
 * custom allocator contexts and font data/readers must outlive the receiver.
 *
 * Source/sink rendering and incremental feeds are synchronous. Callback byte
 * pointers are borrowed only during the callback; a bound sink's callback and
 * userdata remain caller-owned until replacement or destruction. A receiver
 * must not be used concurrently. Source callbacks may change geometry; output
 * callbacks must not mutate or destroy the active receiver.
 *
 * render_cstr and escape_markdown return materialized strings. The source and
 * incremental APIs stream rendering decisions with separately bounded word,
 * table and emission storage. HTML decks buffer one slide at a time. The pager
 * is a buffered consumer with its own bounded content cache; its forward-only
 * compatibility API buffers input.
 *
 * @see mdf_create
 * @see mdf_options
 * @see mdf_feed
 * @see mdf_pager_file_with_options
 */
#ifndef LIBMDF_MDF_H
#define LIBMDF_MDF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** libmdf semantic version string. */
#define LIBMDF_VERSION "0.0.0"
/** libmdf semantic major version. */
#define LIBMDF_VERSION_MAJOR 0
/** libmdf semantic minor version. */
#define LIBMDF_VERSION_MINOR 0
/** libmdf semantic patch version. */
#define LIBMDF_VERSION_PATCH 0

/** Result codes returned by libmdf public operations. */
typedef enum mdf_status {
    /** Operation completed successfully. */
    MDF_OK = 0,
    /** An argument, option, or renderer state is invalid. */
    MDF_ERROR_INVALID = 1,
    /** Allocation failed. */
    MDF_ERROR_NOMEM = 2,
    /** A source, sink, or filesystem operation failed. */
    MDF_ERROR_IO = 3,
    /** Markdown input could not be parsed. */
    MDF_ERROR_PARSE = 4,
    /** Pager cancellation was requested; terminal state has been restored. */
    MDF_CANCELLED = 5,
    /** The terminal viewport cannot fit in the configured pager content cache. */
    MDF_ERROR_TERMINAL_SIZE = 6
} mdf_status;

/** Select how the pager interprets its named input. */
typedef enum mdf_pager_format {
    /** Render .md files as Markdown and all other files as text. */
    MDF_PAGER_FORMAT_AUTO = 0,
    /** Display the file as ordinary terminal text. */
    MDF_PAGER_FORMAT_TEXT = 1,
    /** Render the file through the ANSI Markdown renderer. */
    MDF_PAGER_FORMAT_MARKDOWN = 2
} mdf_pager_format;

/** Optional per-call cooperative pager cancellation. NULL disables it. */
typedef struct mdf_pager_control {
    /** Borrowed context, valid until the pager returns. */
    void *userdata;
    /**
     * Return nonzero to cancel. Called synchronously on the pager's calling
     * thread during loading, rendering, search and navigation, never from a
     * signal handler. A NULL callback disables cancellation. Keep it short;
     * synchronize caller-owned state when requesting cancellation from another
     * thread. Do not invoke another pager from this callback. A blocking source
     * callback or terminal write cannot be preempted.
     */
    int (*should_cancel)(void *userdata);
} mdf_pager_control;

/** Absolute byte offset, expressed as two unsigned 32-bit halves for C89.
 * Each half must be at most 4294967295. */
typedef struct mdf_source_offset {
    /** Most significant 32 bits. */
    unsigned long high;
    /** Least significant 32 bits. */
    unsigned long low;
} mdf_source_offset;

/** Borrowed, finite, seekable pager input. Callbacks run synchronously.
 * Keep its bytes stable until the pager returns. No input copy is made.
 * read has the same contract as mdf_source.read. seek returns zero on success
 * and nonzero on failure, and sets the next read's absolute byte position.
 * The pager may rewind and replay Markdown to reconstruct parser state. */
typedef struct mdf_seekable_source {
    /** Borrowed callback context. */
    void *userdata;
    /** Return at most cap bytes; zero with err=0 signals EOF. Set err on failure. */
    size_t (*read)(void *userdata, char *dst, size_t cap, int *err);
    /** Seek to an absolute byte offset; return zero on success. */
    int (*seek)(void *userdata, const mdf_source_offset *offset);
} mdf_seekable_source;

/** Pager cache configuration. A zeroed record selects all defaults. */
typedef struct mdf_pager_options {
    /** Retained rendered content bytes in one contiguous cache, excluding its
     * NUL terminator. Zero selects 1 MiB; nonzero values must be at least 64 KiB.
     * Search/index workspace and renderer decision storage are bounded separately.
     * Word-buffer limits do not apply to this content allowance. */
    size_t cache_budget;
} mdf_pager_options;

/**
 * Output renderer selection.
 *
 * MDF_FORMAT_ANSI emits wrapped UTF-8 text, with terminal escapes controlled
 * by mdf_options.ansi_mode. MDF_FORMAT_HTML emits a complete HTML document.
 * MDF_FORMAT_HTML_DECK emits a standalone browser slide
 * deck using HTML slide bodies. libmdf-specific chart fences are supported by
 * all renderers and use the active theme in every format.
 */
typedef enum mdf_format {
    /** Wrapped UTF-8 text, optionally styled with terminal escape sequences. */
    MDF_FORMAT_ANSI = 0,
    /** Complete standalone HTML document output. */
    MDF_FORMAT_HTML = 1,
    /** Complete standalone HTML slide-deck output. */
    MDF_FORMAT_HTML_DECK = 2
} mdf_format;

/**
 * Escape-sequence policy for MDF_FORMAT_ANSI only; HTML/deck are unaffected.
 * AUTO is resolved once at renderer creation using output_fd, independently
 * of width and boring. OFF preserves Unicode layout and chart/table shapes;
 * it does not restrict output to 7-bit ASCII.
 */
typedef enum mdf_ansi_mode {
    /** Default: ON for a terminal output_fd, otherwise OFF (including unknown sinks). */
    MDF_ANSI_AUTO = 0,
    /** Allow terminal styling and configured OSC8 even for non-terminal sinks. */
    MDF_ANSI_ON = 1,
    /** No colors, SGR resets, OSC8, or input terminal escapes, including in code/manual tokens. */
    MDF_ANSI_OFF = 2
} mdf_ansi_mode;

/** HTML slide deck transition behavior. */
typedef enum mdf_deck_transition {
    /** Fade through the default deck transition. */
    MDF_DECK_TRANSITION_FADE = 0,
    /** Crossfade directly between slides. */
    MDF_DECK_TRANSITION_CROSS = 1,
    /** Switch slides without a transition. */
    MDF_DECK_TRANSITION_HARD = 2
} mdf_deck_transition;

/** Table buffering policy for ANSI and HTML output. */
typedef enum mdf_table_buffer_mode {
    /** Default: buffer until table end or 64 KiB/1024 retained rows, then stream. */
    MDF_TABLE_BUFFER_FULL = 0,
    /** Emit header/first row (or two headerless rows), then later rows. */
    MDF_TABLE_BUFFER_ROW = 1
} mdf_table_buffer_mode;

/** Border style used for ANSI table output. */
typedef enum mdf_table_wire_mode {
    /** Use Unicode line-drawing borders. */
    MDF_TABLE_WIRE_LINE = 0,
    /** Use ASCII borders. */
    MDF_TABLE_WIRE_ASCII = 1,
    /** Use whitespace-separated columns without borders. */
    MDF_TABLE_WIRE_SPACE = 2
} mdf_table_wire_mode;

/**
 * Optional allocator hooks. Leave this record zeroed to use the C runtime
 * allocator. Custom alloc and free callbacks must form a pair; realloc is
 * optional and libmdf falls back to allocate-copy-free when it is absent.
 */
typedef struct mdf_allocator {
    /** Opaque value passed unchanged to every callback. */
    void *userdata;
    /** Allocate size bytes. Return NULL on failure. */
    void *(*alloc)(void *userdata, size_t size);
    /** Resize a block allocated by alloc, or return NULL on failure. */
    void *(*realloc)(void *userdata, void *ptr, size_t old_size, size_t new_size);
    /** Release a block allocated by alloc or realloc. */
    void (*free)(void *userdata, void *ptr, size_t size);
} mdf_allocator;

/** Default retained word bytes before optional file backing. */
#define MDF_WORD_BUFFER_DEFAULT_BYTES 4096u
/** Largest configurable retained word buffer. */
#define MDF_WORD_BUFFER_MAX_BYTES 65536u
/** Hard maximum bytes retained in one word's temporary file. */
#define MDF_WORD_SPOOL_MAX_BYTES 1048576u
/** Optional reusable-memory retention limits. Zero selects library defaults. */
typedef struct mdf_memory_options {
    /** Retained workspace capacity after a render; zero selects 256 KiB.
     * This limits reusable storage, not the active render's live allocations. */
    size_t max_retained_bytes;
    /** Largest reusable block; zero selects 8192 bytes. Larger freed blocks
     * are returned to the underlying allocator instead of being cached. */
    size_t max_reusable_block_bytes;
} mdf_memory_options;

/**
 * ANSI emission trace callback.
 *
 * Trace events describe the exact sink write that just happened. Chart fences
 * rendered as ANSI participate in the same decision-emission surface as normal
 * text, so charts can be traced without a separate output path.
 */
typedef struct mdf_write_trace {
    /** Opaque value passed to emit. */
    void *userdata;
    /**
     * Observe one completed ANSI sink write. src/len exactly match the write
     * that just succeeded; src is borrowed for this callback only. Returning
     * nonzero fails the render after that write, without retrying it.
     */
    int (*emit)(void *userdata, mdf_format format, const char *src, size_t len);
} mdf_write_trace;

/**
 * Optional per-renderer emission buffer configuration for ANSI writes.
 * Without data, libmdf owns a 128-byte buffer that grows as needed to a default
 * 8192-byte maximum. A supplied fixed or non-owned buffer is hard bounded.
 * A supplied owned, non-fixed buffer may grow through allocator callbacks.
 */
typedef struct mdf_emission_buffer {
    /** Optional caller-supplied storage; keep non-owned data alive until destroy. */
    void *data;
    /** Capacity of data in bytes; ignored when data is NULL. */
    size_t cap;
    /** Initial owned-buffer capacity when data is NULL; zero selects 128. */
    size_t initial_cap;
    /** Maximum capacity for one decision emission; zero selects 8192. */
    size_t max_cap;
    /** Nonzero makes supplied data hard bounded. */
    int fixed;
    /** Nonzero transfers supplied-data cleanup to the renderer on successful
     * creation. The configured allocator must be able to release the block. */
    int take_ownership;
} mdf_emission_buffer;

/** No caller-provided font data. */
#define MDF_HTML_FONT_FORMAT_NONE 0
/** Web Open Font Format 2 font data. */
#define MDF_HTML_FONT_FORMAT_WOFF2 1
/** TrueType font data. */
#define MDF_HTML_FONT_FORMAT_TTF 2

/**
 * One HTML font face supplied as memory bytes or a streaming read callback.
 * Provide either data/data_len or read/userdata. Data, callback, and userdata
 * are borrowed and must remain valid until the renderer is destroyed. The read
 * callback fills up to cap bytes at offset, returns the byte count, and sets
 * *err on failure.
 */
typedef struct mdf_html_font_face {
    /** One of MDF_HTML_FONT_FORMAT_*. */
    int format;
    /** Optional in-memory font bytes. */
    const unsigned char *data;
    /** Number of bytes in data. */
    size_t data_len;
    /** Opaque value passed to read. */
    void *userdata;
    /** Optional random-access font byte reader. */
    size_t (*read)(void *userdata, size_t offset, unsigned char *dst, size_t cap, int *err);
} mdf_html_font_face;

/**
 * Optional HTML font family used by MDF_FORMAT_HTML and MDF_FORMAT_HTML_DECK.
 * Family and face fields are borrowed for the renderer lifetime.
 */
typedef struct mdf_html_font {
    /** CSS font family name. */
    const char *family;
    /** Regular font face. */
    mdf_html_font_face regular;
    /** Italic font face. */
    mdf_html_font_face italic;
} mdf_html_font;

/** Source for the JetBrains Mono faces used by HTML and HTML deck output. */
typedef enum mdf_html_font_source {
    /** Embed the configured font data; this is the default. */
    MDF_HTML_FONT_SOURCE_EMBEDDED = 0,
    /** Reference paired external font URIs instead of embedding data. */
    MDF_HTML_FONT_SOURCE_EXTERNAL = 1
} mdf_html_font_source;

/**
 * Renderer options.
 *
 * Call mdf_options_init before setting fields. Chart fences use these options
 * like the rest of the renderer: ANSI charts honor width, margins, boring,
 * ansi_mode, output_fd, osc8, theme_name, write_trace, allocator, memory, and
 * emission_buffer. HTML charts honor theme_name, boring, html_content_width_ch,
 * and html_font. HTML deck mode also honors deck_transition, slide_numbers, and
 * deck_center_front_text. Deck slide bodies are vertically centered by default.
 */
typedef struct mdf_options {
    /**
     * ANSI wrap width. Default zero (or a negative value) selects terminal
     * width for a terminal output_fd, otherwise 80. This is independent of
     * ansi_mode. Content width after margins must be at least 3.
     * Horizontal/vertical/tile charts fit to this width after margins.
     */
    int width;
    /** ANSI-only left margin on non-empty hard lines, including charts; terminal-wrapped URL continuation rows are unindented. Mutable through set_geometry. */
    int margin_left;
    /**
     * ANSI-only right margin; reduces text/chart width. With zero, visible URLs
     * outside table cells remain contiguous and use terminal autowrap, even with
     * a left margin. width must match the terminal columns for subsequent text
     * layout. Positive values hard-wrap URLs. Mutable through set_geometry.
     */
    int margin_right;
    /** Disable decoration independently of ansi_mode; may still emit ANSI resets/OSC8 unless OFF. Charts retain shapes. */
    int boring;
    /** ANSI escape policy; default AUTO. Independent of boring; ignored by HTML/deck. */
    mdf_ansi_mode ansi_mode;
    /**
     * Borrowed destination fd for ANSI terminal/width detection at creation.
     * Default -1 means an unknown/non-terminal callback or string sink.
     * This does not write to or own the fd; the sink still receives every write.
     * Recreate the renderer when changing destination type.
     */
    int output_fd;
    /** Enable OSC8 hyperlinks for ANSI links; default zero. OFF (including AUTO on non-terminals) overrides this. */
    int osc8;
    /**
     * Built-in theme name. NULL selects the default theme (one-dark for C89
     * HTML) and allows deck front matter theme defaults; any non-NULL name is
     * an explicit override.
     */
    const char *theme_name;
    /** HTML document width in ch; default 96. Charts are centered in this width. */
    double html_content_width_ch;
    /** C89 Systems page branding, for MDF_FORMAT_HTML only. Default zero.
     * SVGs follow the font's embedded/external mode. Font dumping also writes
     * c89-mark.svg and c89-systems.svg beside the regular font, respecting force.
     */
    int html_c89;
    /**
     * Optional HTML font data. When unset, HTML defaults to built-in JetBrains
     * Mono. External font configuration always selects built-in JetBrains Mono
     * and ignores this value.
     */
    mdf_html_font html_font;
    /**
     * Embedded or external JetBrains Mono font source. Default is embedded.
     * Any non-NULL external URI also selects MDF_HTML_FONT_SOURCE_EXTERNAL.
     */
    mdf_html_font_source html_font_source;
    /**
     * Base external font URI; standard regular and italic WOFF2 names are
     * appended before any query or fragment suffix. The base must name a path;
     * query- or fragment-only bases are invalid. A local path or file:// URI
     * is also the implicit dump directory when html_dump_font is enabled and no
     * dump path is supplied; URI suffixes are not filesystem path components.
     */
    const char *html_font_uri;
    /** External regular font URI; overrides html_font_uri and cannot be query- or fragment-only. */
    const char *html_font_regular_uri;
    /** External italic font URI; overrides html_font_uri and cannot be query- or fragment-only. */
    const char *html_font_italic_uri;
    /**
     * Local directory or file:// URI used to derive dump destinations when
     * html_dump_font or html_dump_font_force is nonzero.
     */
    const char *html_font_dump_path;
    /**
     * Optional local regular dump destination. It overrides html_font_dump_path
     * when html_dump_font or html_dump_font_force is nonzero.
     */
    const char *html_font_dump_regular_path;
    /**
     * Optional local italic dump destination. It overrides html_font_dump_path
     * when html_dump_font or html_dump_font_force is nonzero.
     */
    const char *html_font_dump_italic_path;
    /**
     * Dump built-in JetBrains Mono before renderer creation. Only local paths
     * and file:// URI bases can derive dump paths; all other bases require
     * explicit local dump paths.
     */
    int html_dump_font;
    /** Enable dumping and replace existing regular/italic files; default zero.
     * Force mode refuses symlinks and special files. */
    int html_dump_font_force;
    /** HTML deck transition behavior. Default is MDF_DECK_TRANSITION_FADE. */
    mdf_deck_transition deck_transition;
    /** Show slide numbers after the first deck slide; default zero (disabled). */
    int slide_numbers;
    /** Center first-slide paragraph text; default zero (left aligned). */
    int deck_center_front_text;
    /** ANSI/HTML table buffering policy; default FULL. Chart fences ignore it. */
    mdf_table_buffer_mode table_buffer_mode;
    /** ANSI table borders; default LINE (Unicode). Charts use their own shapes. */
    mdf_table_wire_mode table_wire_mode;
    /** ANSI emission buffer policy. */
    mdf_emission_buffer emission_buffer;
    /** Reusable-memory retention policy. */
    mdf_memory_options memory;
    /** ANSI write trace callback. */
    mdf_write_trace write_trace;
    /** Allocator hooks. */
    mdf_allocator allocator;
} mdf_options;

/**
 * Streaming input source. read fills at most cap bytes, returns zero at EOF,
 * and sets *err nonzero on failure. It must not return more than cap bytes.
 * A short positive read is not EOF. Input chunks are borrowed only while they
 * are consumed; userdata and the callback stay caller-owned for the synchronous
 * render call.
 */
typedef struct mdf_source {
    /** Opaque value passed to read. */
    void *userdata;
    /** Fill dst with at most cap bytes. Return zero with err=0 at EOF;
     * set *err nonzero on failure. dst is writable only for this callback. */
    size_t (*read)(void *userdata, char *dst, size_t cap, int *err);
} mdf_source;

/**
 * Synchronous output sink. Accept the complete src/len pair or return nonzero;
 * partial writes are not supported. src is borrowed only for the callback.
 * ANSI writes correspond one-to-one with decided emissions and write_trace
 * events. HTML/deck writes can also contain structural markup.
 * userdata and the callback stay caller-owned. An explicit-sink operation
 * borrows them for its call; a bound receiver sink needs them until replacement
 * or destruction. libmdf supplies no output queue or retry mechanism.
 */
typedef struct mdf_sink {
    /** Opaque value passed to write. */
    void *userdata;
    /** Accept all len bytes and return zero, or return nonzero on failure.
     * Copy bytes before retaining them beyond this callback. */
    int (*write)(void *userdata, const char *src, size_t len);
} mdf_sink;

/**
 * Renderer token type.
 *
 * Most applications should use render or render_cstr. MDF_TOKEN_CHART_BLOCK is
 * emitted by the parser for libmdf-specific chart fences such as
 * ```mdf-bar-chart, ```mdf-vertical-bar-chart, and ```mdf-tile-chart.
 */
typedef enum mdf_token_type {
    /**
     * Inline Markdown, including emphasis, links and backslash escapes.
     * Successive TEXT tokens may form one word or syntax construct; token
     * boundaries do not force emission. Use MDF_TOKEN_LITERAL_TEXT to bypass
     * Markdown.
     */
    MDF_TOKEN_TEXT = 0,
    /** Inline whitespace. */
    MDF_TOKEN_SPACE = 1,
    /** Source newline. */
    MDF_TOKEN_NEWLINE = 2,
    /** End of a paragraph. */
    MDF_TOKEN_PARAGRAPH_END = 3,
    /** Start of a heading; level is its depth. */
    MDF_TOKEN_HEADING_START = 4,
    /** End of a heading. */
    MDF_TOKEN_HEADING_END = 5,
    /** Start of a block quote. */
    MDF_TOKEN_BLOCKQUOTE_START = 6,
    /** End of a block quote. */
    MDF_TOKEN_BLOCKQUOTE_END = 7,
    /** Start of a list item. */
    MDF_TOKEN_LIST_ITEM_START = 8,
    /** End of a list item. */
    MDF_TOKEN_LIST_ITEM_END = 9,
    /** Unchecked task-list marker. */
    MDF_TOKEN_TASK_UNCHECKED = 10,
    /** Checked task-list marker. */
    MDF_TOKEN_TASK_CHECKED = 11,
    /** Start of a fenced or indented code block. */
    MDF_TOKEN_CODE_BLOCK_START = 12,
    /** End of a code block. */
    MDF_TOKEN_CODE_BLOCK_END = 13,
    /** Literal code-block text. */
    MDF_TOKEN_CODE_TEXT = 14,
    /** Horizontal thematic break. */
    MDF_TOKEN_THEMATIC_BREAK = 15,
    /** End of the input document. */
    MDF_TOKEN_DOCUMENT_END = 16,
    /**
     * Complete chart fence body. token->text is the collected two-column CSV
     * body and token->level carries the internal chart kind selected by the
     * fence info string. Prefer Markdown chart fences over constructing this
     * token manually; the chart-kind numeric values are not public API.
     */
    MDF_TOKEN_CHART_BLOCK = 17,
    /** Literal inline text; uses normal word layout without Markdown syntax. */
    MDF_TOKEN_LITERAL_TEXT = 18
} mdf_token_type;

/** Parser/renderer token passed to write_token implementations. */
typedef struct mdf_token {
    /** Token discriminator. */
    mdf_token_type type;
    /** Borrowed payload bytes, valid for the write_token call. text must be
     * non-NULL when len is nonzero; CHART_BLOCK carries its collected CSV. */
    const char *text;
    /** Token payload byte length. */
    size_t len;
    /** Heading depth (1..6) for HEADING_START; parser metadata for other
     * structural tokens. CHART_BLOCK kinds are internal; use Markdown fences. */
    int level;
} mdf_token;

/** Capacity of each NUL-terminated foreground-only ANSI SGR in a palette. */
#define MDF_THEME_FOREGROUND_SIZE 32

/**
 * All semantic foreground colors of a built-in theme. heading[0..5] means
 * H1..H6. Each array contains only a foreground SGR, or an empty string when
 * that slot has no foreground. No attributes, background, reset or text are
 * included. Arrays belong to the caller; name is immutable library storage,
 * valid until the library is unloaded. No allocation or terminal detection.
 */
typedef struct mdf_theme_palette {
    /** Canonical built-in theme name. */
    const char *name;
    /** Heading foregrounds H1 through H6. */
    char heading[6][MDF_THEME_FOREGROUND_SIZE];
    /** Emphasized text foreground. */
    char emphasis[MDF_THEME_FOREGROUND_SIZE];
    /** Strong text foreground. */
    char strong[MDF_THEME_FOREGROUND_SIZE];
    /** Combined emphasis and strong text foreground. */
    char emphasis_strong[MDF_THEME_FOREGROUND_SIZE];
    /** Inline code foreground. */
    char code_inline[MDF_THEME_FOREGROUND_SIZE];
    /** Code block foreground. */
    char code_block[MDF_THEME_FOREGROUND_SIZE];
    /** Quote marker foreground. */
    char quote[MDF_THEME_FOREGROUND_SIZE];
    /** Quoted text foreground. */
    char quote_text[MDF_THEME_FOREGROUND_SIZE];
    /** List marker foreground. */
    char list_marker[MDF_THEME_FOREGROUND_SIZE];
    /** Link label foreground. */
    char link_text[MDF_THEME_FOREGROUND_SIZE];
    /** Visible link destination foreground. */
    char link_url[MDF_THEME_FOREGROUND_SIZE];
    /** Thematic break foreground. */
    char thematic_break[MDF_THEME_FOREGROUND_SIZE];
    /** Table header foreground. */
    char table_header[MDF_THEME_FOREGROUND_SIZE];
    /** Table border foreground. */
    char table_wire[MDF_THEME_FOREGROUND_SIZE];
} mdf_theme_palette;

/** Renderer handle with the receiver methods defined below. */
typedef struct mdf mdf;

/** Receiver vtable slots reserved for ABI-compatible future expansion. */
#define MDF_RECEIVER_RESERVED_SLOTS 4

/**
 * Renderer receiver. Create with mdf_create and release with destroy.
 * Bind a sink before streaming methods; render_cstr returns its own allocated
 * string instead. Public methods are synchronous. Source callbacks may change
 * geometry; sink/trace callbacks may not mutate the active renderer.
 */
struct mdf {
    /**
     * Consume one parser token through the bound sink. TEXT still interprets
     * inline Markdown and retains unfinished words or syntax across calls.
     * Use write_literal for literal text, or render for Markdown source input.
     * End manual-token input with finish. Deck renderers reject this surface.
     * set_sink must succeed first.
     */
    mdf_status (*write_token)(mdf *self, const mdf_token *token);
    /** Finish manual token streaming through the bound sink and release its session state. */
    mdf_status (*finish)(mdf *self);
    /** Stream Markdown from source to the bound sink without materializing input; set_sink must succeed first. */
    mdf_status (*render)(mdf *self, mdf_source *source);
    /**
     * Render NUL-terminated Markdown into an allocated NUL-terminated string.
     * On success, release *out through string_free; *out is NULL on failure.
     */
    mdf_status (*render_cstr)(mdf *self, const char *markdown, char **out);
    /** Borrow the latest diagnostic until the next receiver call or destroy.
     * An empty string means no diagnostic; do not free or modify the result. */
    const char *(*error)(const mdf *self);
    /**
     * Destroy the renderer and release its owned state. NULL is accepted.
     * Destruction cannot interrupt an active output operation, including its
     * source, sink, and trace callbacks; in that case it leaves the renderer intact
     * and records a diagnostic available through error.
     */
    void (*destroy)(mdf *self);
    /** Free a string returned by this renderer before destroying the receiver.
     * NULL strings are accepted; use the same receiver that allocated it. */
    void (*string_free)(mdf *self, char *s);
    /**
     * Feed a nonempty Markdown fragment into the active incremental document.
     * Each final decision is emitted once to the bound sink. set_sink must
     * succeed first. See mdf_feed for lifecycle, format, and limit rules.
     */
    mdf_status (*feed)(mdf *self, const char *data, size_t len);
    /**
     * Validate a non-EOF boundary. This never emits, resolves, or changes
     * retained input; only feed and finish_document can emit. See mdf_flush.
     */
    mdf_status (*flush)(mdf *self);
    /**
     * Resolve and close the active incremental document exactly once, applying
     * ordinary EOF rules. See mdf_finish_document.
     */
    mdf_status (*finish_document)(mdf *self);
    /**
     * Start a distinct next document after a successful finish_document call.
     * See mdf_begin_document for its state requirement.
     */
    mdf_status (*begin_document)(mdf *self);
    /**
     * Change the ANSI wrap width for decisions that have not reached the sink.
     * Retained inline code and link decisions are recomputed under the new
     * width; existing sink writes are never reflowed. The width includes
     * configured margins and must leave at least three content columns. It may
     * run from a source callback, but not a sink or trace callback. See
     * mdf_set_width or mdf_set_geometry.
     */
    mdf_status (*set_width)(mdf *self, int width);
    /**
     * Close enabled terminal styling through the bound sink, then discard the
     * active renderer and incremental-parser session. See mdf_reset. This may not
     * interrupt an active output operation, including callbacks invoked by
     * synchronous, incremental, reset, and manual-token rendering calls.
     */
    mdf_status (*reset)(mdf *self);
    /** Replace the renderer's one bound output sink. See mdf_set_sink. */
    mdf_status (*set_sink)(mdf *self, const mdf_sink *sink);
    /**
     * Change total width and both margins between feeds without ending the
     * active document or rewriting completed output. See mdf_set_geometry.
     */
    mdf_status (*set_geometry)(mdf *self, int width, int margin_left, int margin_right);
    /**
     * Library-owned ABI reserve. Slots are initialized to NULL; callers must
     * not write, retain, or call them, and their values have no public meaning.
     */
    void (*reserved[MDF_RECEIVER_RESERVED_SLOTS])(void);
    /** Private implementation state. Do not inspect or modify it. */
    void *impl;
    /** Get the configured theme's full foreground palette; see mdf_get_theme_palette. */
    mdf_status (*get_theme_palette)(const mdf *self, mdf_theme_palette *out);
    /** Page a file using this ANSI receiver's options; see mdf_pager_file. */
    mdf_status (*pager_file)(mdf *self, const char *path, mdf_pager_format format,
                             const mdf_pager_control *control);
    /** Buffer finite forward-only input before paging; see mdf_pager_source
     * for its 1 MiB input cap and the seekable alternative. */
    mdf_status (*pager_source)(mdf *self, const char *name, mdf_source *source,
                               mdf_pager_format format, const mdf_pager_control *control);
    /** Configure per-word memory and optional file backing; see mdf_set_word_buffer. */
    mdf_status (*set_word_buffer)(mdf *self, size_t memory_limit, int disable_spooling);
    /** Write a literal fragment to the bound sink; see mdf_write_literal. */
    mdf_status (*write_literal)(mdf *self, const char *data, size_t len);
    /** Escape Markdown syntax into an allocated string; see mdf_escape_markdown. */
    mdf_status (*escape_markdown)(mdf *self, const char *text, char **out);
    /** Page with a configured cache; see mdf_pager_file_with_options. */
    mdf_status (*pager_file_with_options)(mdf *self, const char *path, mdf_pager_format format,
                                         const mdf_pager_control *control,
                                         const mdf_pager_options *options);
    /** Page borrowed seekable input; see mdf_pager_seekable_source. */
    mdf_status (*pager_seekable_source)(mdf *self, const char *name, mdf_seekable_source *source,
                                       mdf_pager_format format, const mdf_pager_control *control,
                                       const mdf_pager_options *options);
};

/**
 * Initialize an options record to stable defaults. Call this before setting
 * fields in a record passed to mdf_create; passing NULL to mdf_create selects
 * the same defaults. ANSI defaults are AUTO, output_fd=-1, width=0, boring=0,
 * and osc8=0: unknown sinks produce escape-free UTF-8 at width 80.
 * NULL is accepted as a no-op. A merely zeroed record has output_fd=0 and is
 * not equivalent to these defaults; use this initializer.
 *
 * @param opts Options record to initialize; NULL is a no-op.
 */
void mdf_options_init(mdf_options *opts);
/**
 * Create an ANSI, HTML, or HTML deck renderer. opts may be NULL for defaults;
 * out must be non-NULL and is set to NULL on failure. On success, the caller
 * owns *out and must release it through its destroy method. Bind an output
 * sink through mdf_set_sink before using receiver streaming methods. ANSI
 * escape policy and automatic width are resolved here, using opts->output_fd,
 * not the later sink binding. OFF strips input terminal escapes before parsing
 * and on manual-token output. Invalid ansi_mode values return MDF_ERROR_INVALID.
 *
 * @param format ANSI, HTML document, or HTML deck output.
 * @param opts Initialized options, or NULL for defaults.
 * @param out Required result pointer; set to NULL on failure.
 * @return MDF_OK on success; invalid options, allocation or asset-dump failures return an error.
 */
mdf_status mdf_create(mdf_format format, const mdf_options *opts, mdf **out);
/**
 * Bind the one output sink used by receiver rendering methods. The sink record
 * is copied, but its callback and userdata remain caller-owned and must stay
 * valid until replaced or renderer destruction. Replacing an existing binding
 * makes a best effort to close terminal state on the old sink, discards current
 * rendering state even when that close fails, and then binds the new sink;
 * callers replay their own source to render on the new sink. Rebinding the
 * identical callback and userdata leaves the current rendering state intact.
 * This does not redetect ANSI policy or width; recreate the renderer when the
 * destination type changes. Escape-free ANSI renderers need no terminal cleanup.
 * The explicit-sink free functions below borrow their sink for their call and
 * never replace this receiver binding. A binding cannot be changed while an
 * active output operation is invoking source, sink, or trace callbacks.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param sink Required sink record to copy; callback/context stay borrowed.
 * @return MDF_OK after binding, including if old-sink cleanup failed;
 * MDF_ERROR_INVALID for invalid arguments or a prohibited binding change.
 */
mdf_status mdf_set_sink(mdf *renderer, const mdf_sink *sink);
/**
 * Change an existing renderer's width without changing its current margins.
 * width is the total ANSI wrap width before configured margins and must be
 * positive; ANSI renderers require at least three content columns after
 * margins. The new value applies to subsequent parser decisions and recomputes
 * retained inline code and link decisions that have not yet been emitted,
 * including in an active incremental document. Setting the current width is
 * a no-op after validation and preserves pending decisions. It never rewrites
 * a completed sink emission; tables follow the same future-output rules as
 * mdf_set_geometry. Use mdf_set_geometry to change margins too.
 * Callers that need complete reflow should call
 * mdf_reset and resend their own source. This operation has no terminal or signal
 * handling. It may run from a source callback, but returns MDF_ERROR_INVALID
 * from a sink or write-trace callback so the active emission stays intact.
 * If reflow runs out of memory, the previous width remains configured, but
 * the failed document requires reset and caller-owned replay.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param width Positive total ANSI width before margins.
 * @return MDF_OK on success, or a validation/allocation error while updating retained layout.
 */
mdf_status mdf_set_width(mdf *renderer, int width);
/**
 * Configure storage for each undecided ANSI/HTML word.
 * memory_limit is a byte count; zero selects 4096, and values above 65536 are
 * rejected. Spooling is enabled when disable_spooling is zero. A word exceeding
 * the memory limit then uses an automatically deleted temporary file, capped
 * at 1048576 bytes. Excess bytes are discarded until the normal decision
 * boundary. With spooling disabled, only the memory-limit prefix is retained.
 * Replays use bounded reads through the normal emitter; filling storage never
 * forces a rendering decision or changes wrapping. Nested emphasis uses the
 * same per-word limits and emits decided words without retaining the span.
 * After a nested word has emitted, unmatched opening markers are not restored
 * at the outer boundary or EOF; backslash-escape markers to make them literal.
 * Files are removed on word completion, reset, failure and destruction.
 * Filesystem failures return MDF_ERROR_IO. Configure between documents or
 * while no inline decision is pending; callbacks, pending words and deck
 * renderers reject changes.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param memory_limit Retained word bytes; zero selects the default, maximum 65536.
 * @param disable_spooling Nonzero truncates oversized words instead of using a file.
 * @return MDF_OK on success, or MDF_ERROR_INVALID for an unsupported limit or active decision.
 */
mdf_status mdf_set_word_buffer(mdf *renderer, size_t memory_limit, int disable_spooling);
/**
 * Atomically change the total width and ANSI margins of an existing renderer.
 * Margins must be nonnegative; ANSI content width must remain at least three
 * columns. The active incremental document is preserved. Only future output
 * uses the new geometry: completed sink writes, including a left margin
 * already emitted on the current line, are not rewritten. A new left margin
 * is not inserted after committed text on that line; it begins on the next
 * line. Retained inline code and link decisions are recomputed before emission.
 * A full-buffer ANSI table uses the geometry current at its terminating line,
 * EOF or retention limit, even after earlier feeds retained rows. Once streaming,
 * later rows and the closing border refit the new geometry while emitted rows
 * stay unchanged.
 * No sink write occurs during this call. It may run between feeds or from a
 * source callback, but returns MDF_ERROR_INVALID from
 * a sink or write-trace callback. HTML and deck output ignore ANSI margins,
 * as at creation. On reflow allocation failure, the previous geometry remains
 * configured but the current document fails; reset and caller-owned replay
 * are required, as with mdf_set_width.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param width Positive total ANSI width before margins.
 * @param margin_left Nonnegative ANSI left margin.
 * @param margin_right Nonnegative ANSI right margin.
 * @return MDF_OK on success, or a validation/allocation error while updating retained layout.
 */
mdf_status mdf_set_geometry(mdf *renderer, int width, int margin_left, int margin_right);
/**
 * Close ANSI terminal state through the bound sink, then discard an unfinished
 * manual-token or incremental document without emitting EOF closure. ANSI
 * reset with escapes enabled emits one exact sink write that closes OSC8 links
 * and resets SGR attributes. OFF (including AUTO resolved to OFF) emits no
 * cleanup bytes. Without a bound sink it only discards state. It retains renderer
 * configuration such as width and an explicit HTML title. No Markdown input is
 * retained for replay; callers own and resend source if they need reflow. A
 * sink failure returns MDF_ERROR_IO after state has been discarded. It cannot
 * interrupt an active output operation, including callbacks invoked by
 * synchronous, incremental, reset, and manual-token rendering calls.
 *
 * @param renderer Live receiver, owned by the caller.
 * @return MDF_OK after reset, MDF_ERROR_INVALID for invalid arguments/state,
 * MDF_ERROR_NOMEM composing cleanup, or MDF_ERROR_IO writing cleanup.
 */
mdf_status mdf_reset(mdf *renderer);
/**
 * Render a token through an explicit sink borrowed for this call only; it does
 * not bind that sink and cannot interrupt another active output operation.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param token Borrowed token; payload bytes need remain valid only for this call.
 * @param sink Required output callback/context, borrowed for this call only.
 * @return MDF_OK on success, or an argument, rendering, allocation or sink/storage I/O error.
 */
mdf_status mdf_write_token(mdf *renderer, const mdf_token *token, mdf_sink *sink);
/**
 * Write literal text through the manual-token renderer and an explicit sink.
 * Equivalent to MDF_TOKEN_LITERAL_TEXT: Markdown punctuation, escapes, entities
 * and links are not interpreted. Existing wrapping, margins, quote/heading
 * roles, destination policy and word storage apply. Successive fragments can
 * form one word; a fragment boundary never forces output. Embedded newlines
 * use normal NEWLINE token layout. Empty fragments are no-ops; data may be
 * NULL only when len is zero. The sink is borrowed for this call.
 * Requires closed inline Markdown syntax; unresolved syntax is rejected
 * without emission. Deck renderers do not support manual token input.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param data Borrowed input bytes.
 * @param len Input byte count, independent of any NUL bytes.
 * @param sink Required output callback/context, borrowed for this call only.
 * @return MDF_OK on success, or an argument, inline-state, allocation or sink/storage I/O error.
 */
mdf_status mdf_write_literal(mdf *renderer, const char *data, size_t len, mdf_sink *sink);
/**
 * Allocate NUL-terminated Markdown with ASCII punctuation backslash-escaped.
 * For example, **bold** becomes \\*\\*bold\\*\\*. Existing backslashes are
 * escaped too. UTF-8, whitespace, indentation and line breaks are preserved;
 * ordinary Markdown layout rules still apply. This helper materializes its
 * result; use write_literal to render literal fragments directly instead.
 * On success release *out through renderer->string_free. On failure *out is
 * NULL. text must be NUL-terminated; renderer supplies the string allocator.
 * This operation does not render, emit, flush or alter pending input.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param text Required NUL-terminated input text.
 * @param out Required result pointer; free successful output through string_free.
 * @return MDF_OK on success, MDF_ERROR_INVALID for invalid arguments, or MDF_ERROR_NOMEM.
 */
mdf_status mdf_escape_markdown(mdf *renderer, const char *text, char **out);
/**
 * Finish manual token rendering through an explicit sink borrowed for this
 * call only; it does not bind that sink and cannot interrupt an active output
 * operation.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param sink Required output callback/context, borrowed for this call only.
 * @return MDF_OK on success, or an argument, rendering, allocation or sink/storage I/O error.
 */
mdf_status mdf_finish(mdf *renderer, mdf_sink *sink);
/**
 * Stream source to an explicit sink without materializing the full input. The
 * sink is borrowed only for this call and does not change a receiver binding.
 * HTML decks buffer one slide's Markdown and rendered body at a time.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param source Borrowed input callbacks and context, valid until return.
 * @param sink Required output callback/context, borrowed for this call only.
 * @return MDF_OK on success, or an argument, parse, allocation or source/sink/storage I/O error.
 */
mdf_status mdf_render(mdf *renderer, mdf_source *source, mdf_sink *sink);
/**
 * Feed a nonempty Markdown fragment into one incremental document.
 * Rendering is synchronous: sink is borrowed only for this call, does not
 * replace any receiver binding, and each decided emission must be accepted
 * completely. Feed consumes the supplied fragment without waiting for future
 * fragments. Full-buffer tables emit at their end or switch to row streaming
 * at 65536 retained bytes or 1024 rows, preserving one continuous table.
 * This is not EOF; call mdf_finish_document to resolve an unterminated
 * construct and close output.
 * A pending row, undecided table prefix, or incremental chart that cannot fit
 * within 65536 retained bytes fails with MDF_ERROR_PARSE. Table retention
 * bounds also apply to source-callback rendering. These are retained-data
 * limits, not a cap on all renderer allocations.
 * Feed, flush, and finish_document reject HTML deck renderers; HTML requires
 * an explicit title set before the first call. A sink failure latches this
 * document in a failed state without retrying output. After a successful
 * finish, call mdf_begin_document before feeding another document.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param data Borrowed input bytes.
 * @param len Input byte count, independent of any NUL bytes.
 * @param sink Required output callback/context, borrowed for this call only.
 * @return MDF_OK on success, or an argument/state, parse, allocation or sink/storage I/O error.
 */
mdf_status mdf_feed(mdf *renderer, const char *data, size_t len, mdf_sink *sink);
/**
 * Validate a non-EOF boundary on an incremental document through an explicit
 * sink borrowed for this call; it does not replace any receiver binding. This
 * never emits, resolves, or otherwise changes retained input, including an
 * unfinished table: feed emits every decision as it becomes final, and
 * mdf_finish_document alone applies EOF semantics.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param sink Required output callback/context, borrowed for this call only.
 * @return MDF_OK for an idle/active document, or MDF_ERROR_INVALID for invalid arguments/state.
 */
mdf_status mdf_flush(mdf *renderer, mdf_sink *sink);
/**
 * End an incremental document exactly once through an explicit sink borrowed
 * for this call that does not replace any receiver binding. It resolves
 * retained Markdown under ordinary EOF rules and emits the renderer's final
 * closure. Later feed, flush, or finish_document calls fail until
 * mdf_begin_document succeeds.
 *
 * @param renderer Live receiver, owned by the caller.
 * @param sink Required output callback/context, borrowed for this call only.
 * @return MDF_OK on success, or an argument/state, parse, allocation or sink/storage I/O error.
 */
mdf_status mdf_finish_document(mdf *renderer, mdf_sink *sink);
/**
 * Start a distinct next incremental document after a successful
 * mdf_finish_document. It leaves the bound sink and configured geometry intact.
 *
 * @param renderer Live receiver, owned by the caller.
 * @return MDF_OK on success, or MDF_ERROR_INVALID unless the prior document finished successfully.
 */
mdf_status mdf_begin_document(mdf *renderer);
/**
 * Set or clear the explicit HTML document/deck title before rendering starts.
 * title is copied; NULL restores automatic title detection. ANSI renderers and
 * calls made after output begins reject this operation.
 *
 * @param self Live receiver, owned by the caller.
 * @param title Title to copy, or NULL to restore automatic detection.
 * @return MDF_OK on success, MDF_ERROR_INVALID for invalid state, or MDF_ERROR_NOMEM.
 */
mdf_status mdf_set_html_title(mdf *self, const char *title);
/**
 * Fill out with libmdf's built-in JetBrains Mono regular and italic WOFF2 faces.
 * The returned data pointers are borrowed immutable storage valid for the
 * lifetime of the library and must not be freed or modified by the caller.
 *
 * @param out Descriptor to fill with borrowed immutable faces; NULL is a no-op.
 */
void mdf_html_jetbrains_mono_font(mdf_html_font *out);
/**
 * Write the built-in JetBrains Mono regular and italic WOFF2 files to explicit
 * local destination paths. Both paths are required and must resolve to
 * separate filesystem objects; equivalent paths and aliases are rejected.
 * Parent directories are not created. Existing files are preserved. Returns
 * MDF_ERROR_INVALID for invalid paths and MDF_ERROR_IO for filesystem failures.
 *
 * @param regular_path Local regular-face destination.
 * @param italic_path Local italic-face destination.
 * @return MDF_OK on success, MDF_ERROR_INVALID for invalid paths, or MDF_ERROR_IO.
 */
mdf_status mdf_dump_html_jetbrains_mono_font(const char *regular_path,
                                             const char *italic_path);
/**
 * Write the built-in faces to explicit local paths, replacing existing regular
 * and italic files. Symlinks and special files are refused.
 *
 * @param regular_path Local regular-face destination.
 * @param italic_path Local italic-face destination.
 * @return MDF_OK on success, MDF_ERROR_INVALID for invalid paths, or MDF_ERROR_IO.
 */
mdf_status mdf_dump_html_jetbrains_mono_font_force(const char *regular_path,
                                                   const char *italic_path);
/**
 * Resolve a local directory and optional paired path overrides, then dump the
 * built-in faces. The directory supplies any omitted path; without it, both
 * explicit paths are required. Parent directories are not created and existing
 * files are preserved. Returns MDF_ERROR_INVALID or MDF_ERROR_IO as above.
 *
 * @param font_path Optional local directory for omitted face destinations.
 * @param regular_path Local regular-face destination.
 * @param italic_path Local italic-face destination.
 * @return MDF_OK on success, MDF_ERROR_INVALID for invalid paths, or MDF_ERROR_IO.
 */
mdf_status mdf_dump_html_jetbrains_mono_font_to_paths(const char *font_path,
                                                      const char *regular_path,
                                                      const char *italic_path);
/**
 * Same as mdf_dump_html_jetbrains_mono_font_to_paths, replacing existing
 * regular and italic files while refusing symlinks and special files.
 *
 * @param font_path Optional local directory for omitted face destinations.
 * @param regular_path Local regular-face destination.
 * @param italic_path Local italic-face destination.
 * @return MDF_OK on success, MDF_ERROR_INVALID for invalid paths, or MDF_ERROR_IO.
 */
mdf_status mdf_dump_html_jetbrains_mono_font_to_paths_force(const char *font_path,
                                                            const char *regular_path,
                                                            const char *italic_path);
/**
 * Resolve the local regular and italic destinations selected by enabled HTML
 * font-dump options without writing either file. Each output buffer must be
 * non-NULL and have nonzero capacity. Returns MDF_ERROR_INVALID when dumping
 * is not enabled or the configured URI/path combination is not local and
 * resolvable.
 *
 * @param opts Required initialized options with font dumping enabled.
 * @param regular_path Caller-provided regular-path output buffer.
 * @param regular_path_cap Regular-path buffer capacity, including the trailing NUL.
 * @param italic_path Caller-provided italic-path output buffer.
 * @param italic_path_cap Italic-path buffer capacity, including the trailing NUL.
 * @return MDF_OK on success, or MDF_ERROR_INVALID for invalid options, paths or capacities.
 */
mdf_status mdf_html_font_resolve_dump_paths(const mdf_options *opts,
                                            char *regular_path,
                                            size_t regular_path_cap,
                                            char *italic_path,
                                            size_t italic_path_cap);
/**
 * Resolve the two C89 SVG destinations beside the regular font without
 * writing files. Requires html_c89 and enabled font dumping. Rejects aliases
 * between any of the four font/SVG destinations.
 *
 * @param opts Required initialized options with C89 branding and font dumping enabled.
 * @param mark_path Caller-provided output buffer for the mark SVG path.
 * @param mark_path_cap Mark-path capacity, including the trailing NUL.
 * @param logo_path Caller-provided output buffer for the logotype SVG path.
 * @param logo_path_cap Logotype-path capacity, including the trailing NUL.
 * @return MDF_OK on success, or MDF_ERROR_INVALID for invalid options, aliases or capacities.
 */
mdf_status mdf_html_c89_resolve_dump_paths(const mdf_options *opts,
                                         char *mark_path, size_t mark_path_cap,
                                         char *logo_path, size_t logo_path_cap);
/**
 * Return nonzero when two non-empty paths name the same existing filesystem
 * object or the same currently absent destination. Identical non-empty path
 * strings are aliases even when their parent directory does not exist. A zero
 * result means no alias could be established; it is not a path validity check.
 *
 * @param first_path First nonempty local filesystem path.
 * @param second_path Second nonempty local filesystem path.
 * @return Nonzero for a proven alias; zero does not establish path validity.
 */
int mdf_paths_alias(const char *first_path, const char *second_path);
/**
 * Return a stable static description for status, including unknown values.
 *
 * @param status Result code to describe.
 * @return Immutable static text, including for unknown status values.
 */
const char *mdf_status_string(mdf_status status);
/**
 * Number of built-in themes.
 *
 * @return Number of names available through mdf_theme_name.
 */
size_t mdf_theme_count(void);
/**
 * Built-in theme name by index, or NULL when index is out of range.
 *
 * @param index Zero-based theme index.
 * @return Borrowed immutable name, or NULL for an out-of-range index.
 */
const char *mdf_theme_name(size_t index);
/**
 * Nonzero when name matches a built-in theme case-insensitively; NULL/empty selects default.
 *
 * @param name Theme name; NULL/empty selects default.
 * @return Nonzero for a recognized theme; zero for an unknown name.
 */
int mdf_theme_exists(const char *name);
/**
 * Copy every foreground slot of name into out. NULL/empty name selects default;
 * names are case-insensitive. Unknown names and NULL out return MDF_ERROR_INVALID
 * without modifying out. Color enablement is the caller's responsibility: this
 * lookup does not inspect the environment or terminal and performs no allocation.
 *
 * @param name Theme name; NULL/empty selects default.
 * @param out Required caller-owned palette record; unchanged on invalid arguments.
 * @return MDF_OK on success, or MDF_ERROR_INVALID without modifying out.
 */
mdf_status mdf_theme_get_palette(const char *name, mdf_theme_palette *out);
/**
 * Copy the renderer's configured theme palette, independent of ANSI enablement
 * or boring mode. Includes the one-dark default selected by C89 HTML. Per-document
 * deck front matter does not change the configured theme. NULL/invalid renderer
 * or NULL out returns MDF_ERROR_INVALID without modifying out. No allocation,
 * rendering, sink writes, environment lookup or terminal detection occurs.
 *
 * @param self Live receiver, owned by the caller.
 * @param out Required caller-owned palette record; unchanged on invalid arguments.
 * @return MDF_OK on success, or MDF_ERROR_INVALID without modifying out.
 */
mdf_status mdf_get_theme_palette(const mdf *self, mdf_theme_palette *out);
/**
 * Copy one heading foreground (level 1..6) into out, including its trailing NUL.
 * NULL/empty name selects default; names are case-insensitive. No foreground
 * produces an empty string successfully. Invalid names, levels, NULL out or
 * insufficient out_size return MDF_ERROR_INVALID without modifying out.
 * The same allocation-free, foreground-only rules as mdf_theme_get_palette apply.
 *
 * @param name Theme name; NULL/empty selects default.
 * @param level Heading depth from 1 through 6.
 * @param out Required caller-owned output buffer; unchanged on failure.
 * @param out_size Output buffer capacity, including the trailing NUL.
 * @return MDF_OK on success, or MDF_ERROR_INVALID without modifying out.
 */
mdf_status mdf_theme_heading_foreground(const char *name, int level, char *out, size_t out_size);
/**
 * Estimate OSC8 support from environment hints without terminal I/O.
 * OSC8=0 disables detection. Recognized terminal identifiers and VTE >= 5000
 * enable support. Without specific TERM_PROGRAM or VTE_VERSION information,
 * TERM=xterm or xterm-* is an optimistic compatibility fallback, including
 * over SSH. This is a heuristic, not capability negotiation. The renderer's
 * ANSI policy still controls whether escapes may reach its output sink.
 *
 * @return Nonzero when environment hints suggest OSC8 support; zero otherwise.
 */
int mdf_detect_osc8_support(void);
/**
 * Return terminal width for fd, then COLUMNS, then a positive fallback.
 *
 * @param fd Borrowed file descriptor; it is neither written to nor closed.
 * @param fallback Fallback width; nonpositive selects 80.
 * @return Positive columns from the terminal, COLUMNS, or fallback.
 */
int mdf_terminal_width(int fd, int fallback);
/**
 * Interactively page a named regular file on the controlling terminal.
 *
 * The pager enters the terminal alternate screen and restores it before
 * returning. It needs terminal stdin and stdout. In AUTO mode, a case-
 * insensitive .md extension selects Markdown rendering; all other inputs are
 * shown as ordinary text. Markdown is rerendered after a settled SIGWINCH
 * resize (250 ms debounce). It supports less-style navigation and
 * case-insensitive slash search with n/N match traversal. render_options
 * selects ANSI renderer options, except write_trace, which is rejected because
 * pager terminal writes cannot participate in renderer emission tracing;
 * the pager always uses the current terminal width for its Markdown view.
 * MDF_ANSI_AUTO enables styling here because the pager is interactive, even
 * though its internal rendering sink is a callback. MDF_ANSI_OFF suppresses
 * escapes in rendered Markdown; pager navigation still uses terminal controls.
 * TEXT selects literal text/log viewing with hard wrapping at terminal display
 * columns, independent of filename; Markdown syntax is not interpreted.
 * Input is read on demand with a default 1 MiB contiguous rendered-content
 * cache. Indexes and search workspace have a separate bound. The status shows
 * --% until EOF is known. Search highlights cover cached content; ordinals
 * and totals are document-wide, with ? until established for the active query.
 * Forward navigation resumes the live parser. Evicted Markdown and
 * changed-width views can require cancellable prefix replay. See
 * mdf_pager_file_with_options to configure the cache budget.
 * Optional control requests cancellation without raising a process signal.
 * Cancellation restores terminal modes, cursor, alternate screen and signal
 * handlers before returning MDF_CANCELLED. Idle waits are capped at 50 ms.
 * Ctrl+C forwards SIGINT to the restored application handler after cleanup.
 * If that handler returns or ignores SIGINT, the pager returns MDF_CANCELLED;
 * the default signal disposition still applies when no handler overrides it.
 * NULL control preserves key and signal behavior. Only one pager may use the
 * process terminal/signal state at a time. Emission storage marked
 * take_ownership transfers when input loading begins and is released on every
 * later return, including cancellation or loading failure.
 * Receiver pager methods require an idle ANSI receiver, use independent pager
 * workspace with its allocator and emission limit, and preserve the bound sink.
 *
 * @param path Nonempty path to the regular file to view.
 * @param render_options Initialized rendering options, or NULL for defaults.
 * @param format AUTO filename detection, literal TEXT, or MARKDOWN rendering.
 * @param control Optional borrowed cancellation callback/context; NULL disables polling.
 * @return MDF_OK on quit, MDF_CANCELLED on cancellation, or an argument, size, rendering or I/O error.
 */
mdf_status mdf_pager_file(const char *path,
                          const mdf_options *render_options,
                          mdf_pager_format format,
                          const mdf_pager_control *control);
/**
 * Like mdf_pager_file, with an explicit cache budget. Input is read on demand;
 * evicted content is reread, never copied to a temporary input file. The status
 * shows --% until EOF has been reached at the current width. Search highlights
 * cover cached content; match ordinals and totals are document-wide. Unknown
 * ordinals/totals appear as ?. Knowing EOF before a query does not establish its
 * match total. New queries and changed widths invalidate counts. n/N traverse
 * the document cyclically, skipping cache slices without matches. If no match
 * exists, traversal restores the original viewport. End may replay an unsearched
 * prefix to establish the active query's total, using bounded query state.
 * Markdown cache misses can require cancellable replay of the source prefix.
 * The allowance counts rendered bytes, including ANSI sequences and newlines,
 * with one extra byte allocated for a NUL terminator. Search/index workspace
 * has its own allocation ceiling of 64 times the content allowance plus 32 KiB,
 * including growth; it never reduces content capacity. Renderer decision
 * storage remains separately bounded. A terminal with more content cells than
 * the allowance, or a rendered viewport/row that cannot fit, returns
 * MDF_ERROR_TERMINAL_SIZE after restoring terminal state. Search queries are
 * limited to 4095 UTF-8 bytes.
 * Initial input read-ahead stops at the allowance even with a short or empty
 * viewport. Navigation resumes input; query edits and repaint alone do not.
 * Resize preserves viewed/selected passages even when a formerly cached
 * document outgrows the cache, and derives search ranks from the new layout.
 * NULL options selects the default 1 MiB cache.
 *
 * @param path Nonempty path to the regular file to view.
 * @param render_options Initialized rendering options, or NULL for defaults.
 * @param format AUTO filename detection, literal TEXT, or MARKDOWN rendering.
 * @param control Optional borrowed cancellation callback/context; NULL disables polling.
 * @param options Optional pager cache configuration; NULL selects defaults.
 * @return MDF_OK on quit, MDF_CANCELLED on cancellation, or an argument, size, rendering or I/O error.
 */
mdf_status mdf_pager_file_with_options(const char *path, const mdf_options *render_options,
                                       mdf_pager_format format, const mdf_pager_control *control,
                                       const mdf_pager_options *options);
/**
 * Like mdf_pager_file_with_options, borrowing seekable input until return.
 * The first read starts at byte zero. The caller owns and closes the source.
 * A callback failure returns MDF_ERROR_IO after terminal/workspace cleanup.
 *
 * @param name Required nonempty label; its .md suffix selects Markdown in AUTO.
 * @param source Borrowed input callbacks and context, valid until return.
 * @param render_options Initialized rendering options, or NULL for defaults.
 * @param format AUTO name detection, literal TEXT, or MARKDOWN rendering.
 * @param control Optional borrowed cancellation callback/context; NULL disables polling.
 * @param options Optional pager cache configuration; NULL selects defaults.
 * @return MDF_OK on quit, MDF_CANCELLED on cancellation, or an argument, size, rendering or I/O error.
 */
mdf_status mdf_pager_seekable_source(const char *name, mdf_seekable_source *source,
                                     const mdf_options *render_options, mdf_pager_format format,
                                     const mdf_pager_control *control,
                                     const mdf_pager_options *options);
/**
 * Read all bytes from source through EOF, then interactively page the named
 * buffered input on the controlling terminal. The source read callback's
 * zero-byte return is the end-of-stream signal. name supplies the status-bar
 * label and selects Markdown in AUTO mode when it has a .md extension.
 * Markdown rendering uses explicit byte lengths, so bytes after an embedded
 * NUL are preserved. This legacy forward-only API buffers
 * at most 1 MiB of input before viewing and returns MDF_ERROR_NOMEM for larger
 * input. Use mdf_pager_seekable_source for large sources without input backing.
 *
 * @param name Required nonempty label; its .md suffix selects Markdown in AUTO.
 * @param source Borrowed input callbacks and context, valid until return.
 * @param render_options Initialized rendering options, or NULL for defaults.
 * @param format AUTO name detection, literal TEXT, or MARKDOWN rendering.
 * @param control Optional borrowed cancellation callback/context; NULL disables polling.
 * @return MDF_OK on quit, MDF_CANCELLED on cancellation, or an argument, size, rendering or I/O error.
 */
mdf_status mdf_pager_source(const char *name,
                            mdf_source *source,
                            const mdf_options *render_options,
                            mdf_pager_format format,
                            const mdf_pager_control *control);

#ifdef __cplusplus
}
#endif

#endif
