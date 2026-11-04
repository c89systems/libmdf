#include "../src/render_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

static int under_valgrind;

typedef struct word_allocator {
    size_t live;
    size_t peak;
    size_t maximum;
    int rejected;
} word_allocator;

static void *word_alloc(void *userdata, size_t size)
{
    word_allocator *a;
    void *p;

    a = (word_allocator *)userdata;
    if (size > a->maximum) {
        a->rejected = 1;
        return NULL;
    }
    p = malloc(size);
    if (p != NULL) {
        a->live += size;
        if (a->live > a->peak) a->peak = a->live;
    }
    return p;
}

static void word_free(void *userdata, void *ptr, size_t size)
{
    word_allocator *a;

    a = (word_allocator *)userdata;
    if (ptr != NULL) a->live -= size;
    free(ptr);
}

static int check(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int exercise(size_t limit, int disable_spooling)
{
    mdf_impl *impl;
    mdf_word_storage word;
    word_allocator a;
    char input[256];
    const char *read;
    size_t i;
    size_t offset;
    size_t len;
    size_t expected;
    int fd;
    int fails;

    fails = 0;
    memset(&word, 0, sizeof(word));
    memset(&a, 0, sizeof(a));
    a.maximum = limit + 1;
    impl = (mdf_impl *)calloc(1, sizeof(*impl));
    if (impl == NULL) return 1;
    impl->word_memory_limit = limit;
    impl->disable_word_spooling = disable_spooling;
    impl->allocator.userdata = &a;
    impl->allocator.alloc = word_alloc;
    impl->allocator.free = word_free;
    fails += check(mdf_word_append(impl, &word, NULL, 0) == 0 && word.data == NULL,
                   "empty append allocates no word storage");
    for (i = 0; i < sizeof(input); i++) input[i] = (char)('A' + i % 23);
    for (offset = 0; offset < MDF_WORD_FILE_MAX + sizeof(input); offset += sizeof(input)) {
        if (mdf_word_append(impl, &word, input, sizeof(input)) != 0) {
            fails += check(0, "bounded word append succeeds");
            break;
        }
    }
    expected = disable_spooling ? limit : MDF_WORD_FILE_MAX;
    fails += check(word.len == expected && word.truncated,
                   "excess input is discarded at the selected storage bound");
    fails += check(disable_spooling ? word.file == NULL : word.file != NULL,
                   "temporary files are used only when enabled and needed");
    if (word.file != NULL) {
        fails += check(fflush(word.file) == 0 && fseek(word.file, 0, SEEK_END) == 0 &&
                       ftell(word.file) == (long)MDF_WORD_FILE_MAX,
                       "the complete spool file never exceeds one MiB");
    }
    offset = 0;
    while (offset < word.len) {
        len = 257;
        read = mdf_word_read(impl, &word, offset, &len);
        if (read == NULL || len == 0) {
            fails += check(0, "file-backed words replay through bounded read windows");
            break;
        }
        for (i = 0; i < len; i++) {
            if (read[i] != input[(offset + i) % sizeof(input)]) {
                fails += check(0, "word replay preserves the complete retained prefix");
                break;
            }
        }
        offset += len;
    }
    fails += check(word.status == MDF_OK && !a.rejected && a.live <= limit + 1 &&
                   a.peak <= 2 * (limit + 1),
                   "word storage stays within the configured memory capacity");
    fails += check(mdf_word_insert(impl, &word, 0, "*", 1) == 0 &&
                   mdf_word_byte(impl, &word, 0) == '*' && word.len == expected,
                   "inline prefix insertion preserves the storage cap");
    fails += check(mdf_word_erase(impl, &word, 0, 1) == 0 && word.len == expected - 1,
                   "inline delimiter removal works through bounded file windows");
    if (word.len > 0) fails += check(mdf_word_byte(impl, &word, 0) == 'A',
                                   "delimiter edits preserve the retained word bytes");
    fd = word.file == NULL ? -1 : fileno(word.file);
    mdf_word_clear(&word);
    if (fd >= 0) {
        errno = 0;
        fails += check(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
                       "clearing a word closes and removes its temporary file");
    }
    fails += check(word.len == 0 && word.file == NULL && !word.truncated,
                   "word completion resets per-word backing state");
    fails += check(mdf_word_append(impl, &word, "next", 4) == 0 &&
                   mdf_word_byte(impl, &word, 0) == 'n', "the same storage accepts the next word");
    mdf_word_destroy(impl, &word);
    fails += check(a.live == 0, "destroy releases all owned word memory");
    free(impl);
    return fails;
}

static int count_write(void *context, const char *data, size_t len)
{
    (void)context;
    (void)data;
    (void)len;
    return 0;
}

static int configure(void)
{
    mdf *renderer;
    mdf_options opts;
    mdf_sink sink;
    mdf_token token;
    int fails;

    fails = 0;
    mdf_options_init(&opts);
    opts.ansi_mode = MDF_ANSI_OFF;
    renderer = NULL;
    if (mdf_create(MDF_FORMAT_ANSI, &opts, &renderer) != MDF_OK) return 1;
    fails += check(renderer->set_word_buffer(renderer, 65536, 0) == MDF_OK,
                   "the public receiver accepts the 64 KiB memory maximum");
    fails += check(mdf_set_word_buffer(renderer, 65537, 0) == MDF_ERROR_INVALID &&
                   mdf_set_word_buffer(renderer, 1048576, 0) == MDF_ERROR_INVALID,
                   "the public API rejects memory limits above 64 KiB");
    fails += check(mdf_set_word_buffer(renderer, 0, 1) == MDF_OK,
                   "zero restores the default capacity and spooling can be disabled");
    memset(&sink, 0, sizeof(sink));
    sink.write = count_write;
    fails += check(renderer->set_sink(renderer, &sink) == MDF_OK, "word configuration fixture binds its sink");
    memset(&token, 0, sizeof(token));
    token.type = MDF_TOKEN_TEXT;
    token.text = "pending";
    token.len = 7;
    fails += check(renderer->write_token(renderer, &token) == MDF_OK &&
                   mdf_set_word_buffer(renderer, 4096, 0) == MDF_ERROR_INVALID,
                   "pending input prevents word storage reconfiguration");
    fails += check(renderer->reset(renderer) == MDF_OK &&
                   mdf_set_word_buffer(renderer, 4096, 0) == MDF_OK,
                   "reset makes storage configuration available again");
    token.text = "\\";
    token.len = 1;
    fails += check(renderer->write_token(renderer, &token) == MDF_OK &&
                   renderer->set_word_buffer(renderer, 7, 0) == MDF_ERROR_INVALID,
                   "an unresolved escape also prevents storage reconfiguration");
    renderer->destroy(renderer);
    return fails;
}

typedef struct render_capture {
    char *output;
    size_t len;
    size_t cap;
    size_t *cuts;
    size_t cut_cap;
    char last[16384];
    size_t last_len;
    size_t writes;
    size_t traces;
    size_t xs;
    int nul_output;
    int bad;
    int fail_write;
} render_capture;

static int render_write(void *context, const char *data, size_t len)
{
    render_capture *c;
    char *next;
    size_t cap;
    size_t i;
    size_t *cuts;
    c = (render_capture *)context;
    if (c->fail_write) return -1;
    if (len > sizeof(c->last) || c->traces != c->writes) { c->bad = 1; return -1; }
    memcpy(c->last, data, len);
    c->last_len = len;
    c->writes++;
    for (i = 0; i < len; i++) {
        if (data[i] == 'x') c->xs++;
        if (data[i] == '\0') c->nul_output = 1;
    }
    if (c->output != NULL) {
        if (c->writes > c->cut_cap) {
            cap = c->cut_cap == 0 ? 64 : c->cut_cap * 2;
            cuts = (size_t *)realloc(c->cuts, cap * sizeof(*cuts));
            if (cuts == NULL) return -1;
            c->cuts = cuts;
            c->cut_cap = cap;
        }
        c->cuts[c->writes - 1] = len;
        if (c->len + len + 1 > c->cap) {
            cap = (c->len + len + 1) * 2;
            next = (char *)realloc(c->output, cap);
            if (next == NULL) return -1;
            c->output = next;
            c->cap = cap;
        }
        memcpy(c->output + c->len, data, len);
        c->output[c->len + len] = '\0';
    }
    c->len += len;
    return 0;
}

static int render_trace(void *context, mdf_format format, const char *data, size_t len)
{
    render_capture *c;
    c = (render_capture *)context;
    if (format != MDF_FORMAT_ANSI || c->writes != c->traces + 1 ||
        len != c->last_len || memcmp(c->last, data, len) != 0) c->bad = 1;
    c->traces++;
    return c->bad ? -1 : 0;
}

static mdf *render_create(render_capture *c, mdf_sink *sink, int width, int right, int styled, size_t limit, int disable)
{
    mdf *r;
    mdf_options opts;
    mdf_options_init(&opts);
    opts.width = width;
    opts.emission_buffer.max_cap = 16384;
    opts.margin_left = 2;
    opts.margin_right = right;
    opts.ansi_mode = styled ? MDF_ANSI_ON : MDF_ANSI_OFF;
    opts.write_trace.userdata = c;
    opts.write_trace.emit = render_trace;
    memset(sink, 0, sizeof(*sink));
    sink->userdata = c;
    sink->write = render_write;
    r = NULL;
    if (mdf_create(MDF_FORMAT_ANSI, &opts, &r) != MDF_OK) return NULL;
    if (r->set_sink(r, sink) != MDF_OK || r->set_word_buffer(r, limit, disable) != MDF_OK) {
        r->destroy(r);
        return NULL;
    }
    return r;
}

static int compare_layout(void)
{
    static const char *prefixes[] = {"", "> ", "- ", "**", "> **", "before**", "**[", "**\\[", "# ", "**", "> **", "***", "*", "> a **"};
    static const char *suffixes[] = {" tail\n", " tail\n", " tail\n", "**tail\n", "** tail\n", "**tail\n", "](https://example.com)**tail\n", "\\]**tail\n", " tail\n", "**tail\n", "** tail\n", "***tail\n", "* tail\n", "**tail\n"};
    static const size_t limits[] = {65536, 4096, 7, 4096};
    render_capture *c;
    char *input;
    char *reference;
    size_t reference_len;
    size_t *spool_cuts;
    size_t spool_writes;
    size_t i;
    size_t k;
    size_t l;
    size_t off;
    size_t n;
    size_t len;
    size_t prefix_len;
    int width;
    int right;
    int styled;
    int changed_geometry;
    int fails;
    mdf *r;
    mdf_sink sink;
    mdf_impl *impl;
    fails = 0;
    c = (render_capture *)calloc(1, sizeof(*c));
    input = (char *)malloc(12200);
    if (c == NULL || input == NULL) { free(c); free(input); return 1; }
    for (styled = 0; styled <= 1; styled++) {
        for (right = 0; right <= 4; right += 4) {
            for (width = 17; width <= 100017; width += 100000) {
                for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
                    if (under_valgrind && i != 0 && i != 3 && i != 9) continue;
                    prefix_len = strlen(prefixes[i]);
                    memcpy(input, prefixes[i], prefix_len);
                    for (k = 0; k < 12000; k++) input[prefix_len + k] = 'x';
                    /* Early and late URL evidence, UTF-8, and escaped brackets. */
                    if (i == 1 || i == 3) memcpy(input + prefix_len + 5000, "://host/", 8);
                    if (i == 2 || i == 4) memcpy(input + prefix_len, "https://example.com/", 20);
                    if (i == 0 || i == 5) memcpy(input + prefix_len + 4094, "\302\265\347\225\214", 5);
                    len = prefix_len + 12000;
                    strcpy(input + len, suffixes[i]);
                    len += strlen(suffixes[i]);
                    reference = NULL;
                    reference_len = 0;
                    spool_cuts = NULL;
                    spool_writes = 0;
                    for (l = 0; l < sizeof(limits) / sizeof(limits[0]); l++) {
                        memset(c, 0, sizeof(*c));
                        c->output = (char *)malloc(1);
                        c->cap = 1;
                        r = render_create(c, &sink, width, right, styled, limits[l], 0);
                        if (r == NULL || c->output == NULL) { fails++; free(c->output); break; }
                        off = 0;
                        changed_geometry = 0;
                        while (off < len) {
                            n = l >= 2 ? 1 : 257;
                            if (n > len - off) n = len - off;
                            if (r->feed(r, input + off, n) != MDF_OK) {
                                fprintf(stderr, "layout feed case=%lu limit=%lu width=%d right=%d styled=%d: %s\n", (unsigned long)i, (unsigned long)limits[l], width, right, styled, r->error(r));
                                fails++;
                                break;
                            }
                            off += n;
                            if (!changed_geometry && off >= 4096 && (i == 0 || i == 3)) {
                                fails += check(r->set_geometry(r, width + 5, 4, right) == MDF_OK,
                                               "geometry changes retain an undecided memory/file-backed word");
                                changed_geometry = 1;
                            }
                            impl = (mdf_impl *)r->impl;
                            fails += check(impl->word.cap <= limits[l] + 1 && impl->emph_word.cap <= limits[l] + 1,
                                           "live renderer word caches respect their memory limit");
                        }
                        if (off == len && r->finish_document(r) != MDF_OK) { fprintf(stderr, "layout end: %s\n", r->error(r)); fails++; }
                        fails += check(!c->bad && c->writes == c->traces, "every replay sink write has one identical trace event");
                        if (l == 0) { reference = c->output; reference_len = c->len; }
                        else {
                            if (c->len != reference_len || memcmp(c->output, reference, reference_len) != 0) {
                                fprintf(stderr, "layout mismatch case=%lu limit=%lu width=%d right=%d styled=%d expected=%lu actual=%lu\n", (unsigned long)i, (unsigned long)limits[l], width, right, styled, (unsigned long)reference_len, (unsigned long)c->len);
                                for (k = 0; k < c->len && k < reference_len && c->output[k] == reference[k]; k++) { }
                                fprintf(stderr, "first difference=%lu; expected tail: ", (unsigned long)k);
                                fwrite(reference + k, 1, reference_len - k < 80 ? reference_len - k : 80, stderr);
                                fprintf(stderr, "\nactual tail: ");
                                fwrite(c->output + k, 1, c->len - k < 80 ? c->len - k : 80, stderr);
                                fprintf(stderr, "\n");
                                fails++;
                            }
                            free(c->output);
                        }
                        if (l == 1) { spool_cuts = c->cuts; spool_writes = c->writes; }
                        else {
                            if (l == 3) fails += check(c->writes == spool_writes &&
                                memcmp(c->cuts, spool_cuts, spool_writes * sizeof(*spool_cuts)) == 0,
                                "the same file-backed render has identical sink/trace emissions across feed fragmentation");
                            free(c->cuts);
                        }
                        r->destroy(r);
                    }
                    free(spool_cuts);
                    free(reference);
                }
            }
        }
    }
    free(input);
    free(c);
    return fails;
}

static int renderer_bounds(int disable, int end_kind)
{
    render_capture *c;
    mdf *r;
    mdf_impl *impl;
    mdf_sink sink;
    char input[256];
    size_t offset;
    size_t before;
    size_t expected;
    size_t feed_limit;
    size_t total_input;
    int fd;
    int fails;
    mdf_token token;
    fails = 0;
    c = (render_capture *)calloc(1, sizeof(*c));
    if (c == NULL) return 1;
    r = render_create(c, &sink, 80, 0, 0, 0, disable);
    if (r == NULL) { free(c); return 1; }
    impl = (mdf_impl *)r->impl;
    memset(&token, 0, sizeof(token));
    token.type = MDF_TOKEN_BLOCKQUOTE_START;
    fails += check(r->write_token(r, &token) == MDF_OK, "long-word fixture opens a quote");
    before = c->writes;
    token.type = MDF_TOKEN_TEXT;
    token.text = input;
    token.len = sizeof(input);
    memset(input, 'x', sizeof(input));
    if (end_kind == 4) {
        fails += check(r->write_literal(r, "https://", 8) == MDF_OK, "literal URL prefix remains pending");
    }
    feed_limit = under_valgrind ? 12288 : MDF_WORD_FILE_MAX + 4096;
    for (offset = 0; offset < feed_limit; offset += sizeof(input)) {
        if (r->write_token(r, &token) != MDF_OK) { fails++; break; }
    }
    total_input = feed_limit + (end_kind == 4 ? 8 : 0);
    expected = disable ? MDF_WORD_MEMORY_DEFAULT : MDF_WORD_FILE_MAX;
    if (expected > total_input) expected = total_input;
    fails += check(c->writes == before && c->xs == 0,
                   "storage overflow never emits an undecided word");
    fails += check(impl->word.len == expected && impl->word.cap <= MDF_WORD_MEMORY_DEFAULT + 1 && impl->word.truncated == (total_input > expected),
                   "the live renderer retains only the configured prefix at its hard bound");
    fd = impl->word.file == NULL ? -1 : fileno(impl->word.file);
    fails += check(disable ? fd == -1 : fd >= 0, "the live renderer uses the selected backing policy");
    if (end_kind == 0 || end_kind == 4) {
        token.type = MDF_TOKEN_BLOCKQUOTE_END;
        token.text = NULL;
        token.len = 0;
        fails += check(r->write_token(r, &token) == MDF_OK && r->finish(r) == MDF_OK,
                       "a boundary replays the capped word through the normal renderer");
        fails += check(c->xs == expected - (end_kind == 4 ? 8 : 0) && !c->bad && c->writes == c->traces,
                       "the entire retained prefix reaches sink and trace exactly once");
    } else if (end_kind == 1) {
        fails += check(r->reset(r) == MDF_OK && c->xs == 0, "reset discards a pending spool without replay");
    } else if (end_kind == 2) {
        c->fail_write = 1;
        token.type = MDF_TOKEN_BLOCKQUOTE_END;
        token.text = NULL;
        token.len = 0;
        fails += check(r->write_token(r, &token) == MDF_ERROR_IO,
                       "a replay sink error is returned and cleans pending storage");
        fails += check(!c->bad && c->traces == c->writes, "failed writes do not fabricate trace events");
    }
    r->destroy(r);
    if (fd >= 0) { errno = 0; fails += check(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
                                           "completion, reset, failure and destroy close the spool descriptor"); }
    free(c);
    return fails;
}

static int reader_and_failures(void)
{
    static const char unicode[] = "ab\302\265\347\225\214cdef";
    mdf_impl *impl;
    mdf_word_storage word;
    word_allocator a;
    mdf_text_view text;
    mdf_text_view joined;
    const char *data;
    size_t count;
    unsigned long cp;
    int fd;
    int fails;

    fails = 0;
    memset(&word, 0, sizeof(word));
    memset(&a, 0, sizeof(a));
    a.maximum = 8;
    impl = (mdf_impl *)calloc(1, sizeof(*impl));
    if (impl == NULL) return 1;
    impl->word_memory_limit = 7;
    impl->allocator.userdata = &a;
    impl->allocator.alloc = word_alloc;
    impl->allocator.free = word_free;
    fails += check(mdf_word_append(impl, &word, unicode, sizeof(unicode) - 1) == 0,
                   "reader fixture spools a Unicode word");
    text = mdf_text_word(&word);
    fails += check(mdf_text_join(&joined, mdf_text_slice(text, 0, 3),
                               mdf_text_slice(text, 3, text.len - 3)) == 0 &&
                   mdf_text_columns(impl, joined) == 9,
                   "joined readers decode Unicode across span and cache boundaries");
    fails += check(mdf_text_decode(impl, joined, 2, &cp) == 2 && cp == 0xb5 &&
                   mdf_text_decode(impl, joined, 4, &cp) == 3 && cp == 0x754c,
                   "source views retain complete UTF-8 codepoints");
    mdf_word_destroy(impl, &word);
    fails += check(a.live == 0, "reader fixture releases its cache");

    /* A word may reach disk before its first replay cache is allocated. */
    fails += check(mdf_word_append(impl, &word, unicode, sizeof(unicode) - 1) == 0,
                   "allocation-failure fixture opens a spool");
    fd = fileno(word.file);
    a.maximum = 0;
    count = 7;
    data = mdf_word_read(impl, &word, 0, &count);
    fails += check(data == NULL && count == 0 && word.status == MDF_ERROR_NOMEM,
                   "replay-cache allocation failure is reported without output");
    mdf_word_destroy(impl, &word);
    errno = 0;
    fails += check(a.live == 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF,
                   "allocation failure does not leak the spool or cache");
    a.maximum = 8;
    fails += check(mdf_word_append(impl, &word, unicode, sizeof(unicode) - 1) == 0,
                   "read-failure fixture opens a spool");
    fd = fileno(word.file);
    fails += check(close(fd) == 0, "read-failure fixture invalidates the backing descriptor");
    count = 7;
    data = mdf_word_read(impl, &word, 0, &count);
    fails += check(data == NULL && count == 0 && word.status == MDF_ERROR_IO,
                   "spool I/O failure retains an actionable I/O status");
    mdf_word_destroy(impl, &word);
    fails += check(a.live == 0, "I/O failure releases the owned cache");
    free(impl);
    return fails;
}

static int renderer_file_failure(void)
{
    render_capture *c;
    mdf *r;
    mdf_sink sink;
    mdf_impl *impl;
    int fd;
    char input[512];
    char table[1024];
    char *rendered;
    size_t table_len;
    struct rlimit saved;
    struct rlimit limited;
    int fails;
    mdf_status st;

    fails = 0;
    c = (render_capture *)calloc(1, sizeof(*c));
    if (c == NULL) return 1;
    r = render_create(c, &sink, 80, 0, 1, 7, 0);
    if (r == NULL) { free(c); return 1; }
    impl = (mdf_impl *)r->impl;
    if (getrlimit(RLIMIT_NOFILE, &saved) != 0) { r->destroy(r); free(c); return 1; }
    limited = saved;
    limited.rlim_cur = 0;
    if (setrlimit(RLIMIT_NOFILE, &limited) != 0) { r->destroy(r); free(c); return 1; }
    st = r->write_literal(r, "abcdefghijk", 11);
    fails += check(setrlimit(RLIMIT_NOFILE, &saved) == 0, "file-creation fixture restores its descriptor limit");
    fails += check(st == MDF_ERROR_IO && c->writes == 0 && impl->word.file == NULL &&
                   strstr(r->error(r), "temporary file creation") != NULL,
                   "temporary-file creation failure is actionable and emits no undecided input");
    strcpy(table, "| Head |\n|---|\n| ");
    table_len = strlen(table);
    memset(table + table_len, 'a', sizeof(input));
    strcpy(table + table_len + sizeof(input), " |\n");
    rendered = NULL;
    fails += check(setrlimit(RLIMIT_NOFILE, &limited) == 0, "table fixture limits temporary-file creation");
    st = r->render_cstr(r, table, &rendered);
    fails += check(setrlimit(RLIMIT_NOFILE, &saved) == 0, "table fixture restores its descriptor limit");
    fails += check(st == MDF_ERROR_IO && rendered == NULL && impl->word_error == MDF_OK &&
                   strstr(r->error(r), "temporary file") != NULL,
                   "table cell helpers preserve word-spool I/O status and diagnostics");
    r->string_free(r, rendered);
    fails += check(r->reset(r) == MDF_OK && r->write_literal(r, "next ", 5) == MDF_OK && r->finish(r) == MDF_OK,
                   "a renderer can recover from file failure through reset");
    fails += check(!c->bad && c->writes == c->traces, "file failure and recovery preserve sink/trace identity");
    fails += check(setrlimit(RLIMIT_NOFILE, &limited) == 0, "incremental table fixture limits file creation");
    st = r->feed(r, table, strlen(table));
    if (st == MDF_OK) st = r->finish_document(r);
    fails += check(setrlimit(RLIMIT_NOFILE, &saved) == 0, "incremental table fixture restores descriptor limits");
    fails += check(st == MDF_ERROR_IO && impl->word_error == MDF_OK &&
                   strstr(r->error(r), "temporary file") != NULL && r->reset(r) == MDF_OK,
                   "incremental table spool failures preserve I/O diagnostics and permit ANSI cleanup");
    memset(input, 'a', sizeof(input));
    fails += check(r->write_literal(r, input, sizeof(input)) == MDF_OK, "read-failure recovery fixture opens a spool");
    fd = fileno(impl->word.file);
    fails += check(close(fd) == 0, "read-failure fixture invalidates the descriptor");
    st = r->finish(r);
    fails += check(st == MDF_ERROR_IO && !c->nul_output && impl->word.file == NULL &&
                   strstr(r->error(r), "temporary file") != NULL,
                   "replay I/O failure preserves its diagnostic and releases file ownership");
    r->destroy(r);
    free(c);
    return fails;
}

typedef struct failed_source {
    mdf_impl *impl;
    int sent;
    int fd;
    int emphasis;
} failed_source;

static size_t fail_after_word(void *userdata, char *dst, size_t cap, int *err)
{
    failed_source *source;
    FILE *file;
    size_t length;

    source = (failed_source *)userdata;
    *err = 0;
    if (!source->sent) {
        source->sent = 1;
        length = cap < 512 ? cap : 512;
        memset(dst, 'x', length);
        if (source->emphasis && length >= 2) memcpy(dst, "**", 2);
        return length;
    }
    file = source->emphasis ? source->impl->emph_word.file : source->impl->word.file;
    source->fd = file == NULL ? -1 : fileno(file);
    *err = 1;
    return 0;
}

static int renderer_source_failure(int emphasis)
{
    render_capture *c;
    mdf *r;
    mdf_sink sink;
    mdf_source source;
    failed_source data;
    mdf_status st;
    int fails;

    fails = 0;
    c = (render_capture *)calloc(1, sizeof(*c));
    if (c == NULL) return 1;
    r = render_create(c, &sink, 80, 0, 1, 7, 0);
    if (r == NULL) { free(c); return 1; }
    memset(&data, 0, sizeof(data));
    data.impl = (mdf_impl *)r->impl;
    data.fd = -1;
    data.emphasis = emphasis;
    source.userdata = &data;
    source.read = fail_after_word;
    st = r->render(r, &source);
    fails += check(st == MDF_ERROR_IO && strstr(r->error(r), "source") != NULL,
                   "a source failure preserves its I/O status and diagnostic");
    fails += check(data.fd >= 0, "source failure occurs after an undecided word has spilled");
    fails += check(emphasis ? data.impl->emph_word.file == NULL : data.impl->word.file == NULL,
                   "source failure clears the pending plain or styled word");
    errno = 0;
    fails += check(data.fd >= 0 && fcntl(data.fd, F_GETFD) == -1 && errno == EBADF &&
                   data.impl->word.file == NULL && data.impl->emph_word.file == NULL,
                   "render closes pending spools before returning a source failure");
    fails += check(!c->bad && c->writes == c->traces && c->xs == 0,
                   "source failure preserves sink/trace identity without emitting the undecided word");
    fails += check(r->reset(r) == MDF_OK && r->write_literal(r, "next ", 5) == MDF_OK &&
                   r->finish(r) == MDF_OK, "reset permits rendering after a failed source");
    r->destroy(r);
    free(c);
    return fails;
}

static void *worker(void *unused)
{
    static const size_t limits[] = {1, 7, MDF_WORD_MEMORY_DEFAULT, MDF_WORD_MEMORY_MAX};
    size_t i;
    int fails;

    (void)unused;
    fails = configure();
    fails += compare_layout();
    fails += reader_and_failures();
    fails += renderer_file_failure();
    fails += renderer_source_failure(0);
    fails += renderer_source_failure(1);
    for (i = 0; i < 4; i++) fails += renderer_bounds(0, (int)i);
    fails += renderer_bounds(1, 0);
    fails += renderer_bounds(0, 4);
    for (i = under_valgrind ? 2 : 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        fails += exercise(limits[i], 0);
        fails += exercise(limits[i], 1);
    }
    return fails == 0 ? NULL : (void *)&limits;
}

int main(int argc, char **argv)
{
    pthread_t thread;
    void *result;

    if (argc == 2 && strcmp(argv[1], "--under-valgrind") == 0) under_valgrind = 1;
    else if (argc != 1) return 2;
    if (pthread_create(&thread, NULL, worker, NULL) != 0) return 1;
    if (pthread_join(thread, &result) != 0) return 1;
    return result != NULL;
}
