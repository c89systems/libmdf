#include "render_internal.h"

static int word_fail(mdf_impl *impl, mdf_word_storage *word, const char *message)
{
    word->status = MDF_ERROR_IO;
    impl->word_error = MDF_ERROR_IO;
    strncpy(impl->error, message, sizeof(impl->error) - 1);
    impl->error[sizeof(impl->error) - 1] = '\0';
    return -1;
}

static int word_reserve(mdf_impl *impl, mdf_word_storage *word, size_t need)
{
    size_t cap;
    size_t maximum;
    char *next;

    if (need <= word->cap) return 0;
    maximum = impl->word_memory_limit + 1;
    if (need > maximum) return word_fail(impl, word, "word memory limit exceeded");
    cap = word->cap == 0 ? 64 : word->cap;
    while (cap < need) cap *= 2;
    if (cap > maximum) cap = maximum;
    next = (char *)mdf_realloc_mem(&impl->allocator, word->data, word->cap, cap);
    if (next == NULL) {
        word->status = MDF_ERROR_NOMEM;
        impl->word_error = MDF_ERROR_NOMEM;
        mdf_impl_mark_oom(impl);
        return -1;
    }
    word->data = next;
    word->cap = cap;
    return 0;
}

static void word_note(mdf_word_storage *word, unsigned char byte, size_t offset)
{
    if (offset < sizeof(word->head)) word->head[offset] = byte;
    if ((byte & 0xc0) != 0x80) word->units++;
    if (byte >= 0x80) word->non_ascii = 1;
    if (offset > 0) {
        if (word->tail[3] == '.') word->has_dot = 1;
        if (word->tail[3] == '/') word->has_slash = 1;
    }
    if (offset >= 2 && word->tail[2] == ':' && word->tail[3] == '/' && byte == '/') {
        word->has_scheme = 1;
        word->last_scheme = offset + 1;
    }
    if (byte == '<') word->has_angle = 1;
    if (byte == '?' && word->question == 0) word->question = offset + 1;
    if (offset >= 2 && word->numeric_colon == 0 && word->tail[3] == ':' &&
        word->tail[2] >= '0' && word->tail[2] <= '9' && byte >= '0' && byte <= '9')
        word->numeric_colon = offset;
    memmove(word->tail, word->tail + 1, sizeof(word->tail) - 1);
    word->tail[3] = byte;
}

static int word_make_file(mdf_impl *impl, mdf_word_storage *word)
{
    word->file = tmpfile();
    if (word->file == NULL) return word_fail(impl, word, "word temporary file creation failed");
    if (word->len > 0 && fwrite(word->data, 1, word->len, word->file) != word->len)
        return word_fail(impl, word, "word temporary file write failed");
    word->append_position = 1;
    word->cache_len = 0;
    return 0;
}

int mdf_word_append(mdf_impl *impl, mdf_word_storage *word, const char *src, size_t len)
{
    size_t accepted;
    size_t maximum;
    size_t i;

    if (word->status != MDF_OK) return -1;
    if (len == 0 || word->truncated) return 0;
    maximum = impl->disable_word_spooling ? impl->word_memory_limit : MDF_WORD_FILE_MAX;
    accepted = maximum - word->len;
    if (accepted > len) accepted = len;
    if (accepted < len) word->truncated = 1;
    if (accepted == 0) return 0;
    word->escaped_tail = 0;
    if (word->file == NULL && accepted > impl->word_memory_limit - word->len) {
        if (word_make_file(impl, word) != 0) return -1;
    }
    if (word->file != NULL) {
        if (!word->append_position && fseek(word->file, (long)word->len, SEEK_SET) != 0)
            return word_fail(impl, word, "word temporary file seek failed");
        if (fwrite(src, 1, accepted, word->file) != accepted)
            return word_fail(impl, word, "word temporary file write failed");
        word->append_position = 1;
        word->cache_len = 0;
    } else {
        if (word_reserve(impl, word, word->len + accepted + 1) != 0) return -1;
        memcpy(word->data + word->len, src, accepted);
        word->data[word->len + accepted] = '\0';
    }
    for (i = 0; i < accepted; i++) word_note(word, (unsigned char)src[i], word->len + i);
    word->len += accepted;
    return 0;
}

const char *mdf_word_read(mdf_impl *impl, mdf_word_storage *word, size_t offset, size_t *len)
{
    size_t available;

    if (word->status != MDF_OK || offset >= word->len) {
        *len = 0;
        return NULL;
    }
    available = word->len - offset;
    if (*len > available) *len = available;
    if (word->file == NULL) return word->data + offset;
    if (offset < word->cache_start || offset >= word->cache_start + word->cache_len) {
        if (word_reserve(impl, word, impl->word_memory_limit + 1) != 0) {
            *len = 0;
            return NULL;
        }
        word->cache_start = offset - offset % impl->word_memory_limit;
        available = word->len - word->cache_start;
        word->cache_len = available < impl->word_memory_limit ? available : impl->word_memory_limit;
        if (fseek(word->file, (long)word->cache_start, SEEK_SET) != 0 ||
            fread(word->data, 1, word->cache_len, word->file) != word->cache_len) {
            word_fail(impl, word, "word temporary file read failed");
            *len = 0;
            return NULL;
        }
        word->append_position = 0;
    }
    available = word->cache_start + word->cache_len - offset;
    if (*len > available) *len = available;
    return word->data + offset - word->cache_start;
}

int mdf_word_byte(mdf_impl *impl, mdf_word_storage *word, size_t offset)
{
    const char *data;
    size_t len;

    if (offset >= word->len) return 0;
    if (word->file == NULL) return (unsigned char)word->data[offset];
    if (offset < sizeof(word->head)) return word->head[offset];
    if (word->len - offset <= sizeof(word->tail)) return word->tail[4 - (word->len - offset)];
    len = 1;
    data = mdf_word_read(impl, word, offset, &len);
    return len == 1 ? (unsigned char)data[0] : 0;
}

int mdf_word_copy(mdf_impl *impl, mdf_word_storage *word, size_t offset, char *dst, size_t len)
{
    const char *data;
    size_t chunk;

    if (offset > word->len || len > word->len - offset) return -1;
    while (len > 0) {
        chunk = len;
        data = mdf_word_read(impl, word, offset, &chunk);
        if (chunk == 0) return -1;
        memcpy(dst, data, chunk);
        offset += chunk;
        dst += chunk;
        len -= chunk;
    }
    return 0;
}

static int word_recount(mdf_impl *impl, mdf_word_storage *word)
{
    const char *data;
    size_t offset;
    size_t count;
    size_t i;

    memset(word->head, 0, sizeof(word->head));
    memset(word->tail, 0, sizeof(word->tail));
    word->units = 0;
    word->question = 0;
    word->numeric_colon = 0;
    word->non_ascii = 0;
    word->has_scheme = 0;
    word->last_scheme = 0;
    word->has_dot = 0;
    word->has_slash = 0;
    word->has_angle = 0;
    offset = 0;
    while (offset < word->len) {
        count = word->len - offset;
        data = mdf_word_read(impl, word, offset, &count);
        if (count == 0) return -1;
        for (i = 0; i < count; i++) word_note(word, (unsigned char)data[i], offset + i);
        offset += count;
    }
    return 0;
}

int mdf_word_shrink(mdf_impl *impl, mdf_word_storage *word, size_t len)
{
    if (len > word->len) return -1;
    word->len = len;
    word->cache_len = 0;
    word->append_position = 0;
    if (word->file == NULL && word->data != NULL) word->data[len] = '\0';
    return word_recount(impl, word);
}

int mdf_word_insert(mdf_impl *impl, mdf_word_storage *word, size_t offset, const char *src, size_t len)
{
    size_t maximum;
    size_t inserted;
    size_t suffix;
    size_t total;
    size_t chunk;
    size_t from;

    if (word->status != MDF_OK || offset > word->len) return -1;
    if (len == 0) return 0;
    maximum = impl->disable_word_spooling ? impl->word_memory_limit : MDF_WORD_FILE_MAX;
    inserted = len < maximum - offset ? len : maximum - offset;
    suffix = word->len - offset;
    if (suffix > maximum - offset - inserted) suffix = maximum - offset - inserted;
    total = offset + inserted + suffix;
    if (inserted < len || suffix < word->len - offset) word->truncated = 1;
    if (word->file == NULL && total > impl->word_memory_limit && word_make_file(impl, word) != 0)
        return -1;
    if (word->file == NULL) {
        if (word_reserve(impl, word, total + 1) != 0) return -1;
        memmove(word->data + offset + inserted, word->data + offset, suffix);
        memcpy(word->data + offset, src, inserted);
        word->data[total] = '\0';
    } else {
        if (word_reserve(impl, word, impl->word_memory_limit + 1) != 0) return -1;
        while (suffix > 0) {
            chunk = suffix < impl->word_memory_limit ? suffix : impl->word_memory_limit;
            from = offset + suffix - chunk;
            if (fseek(word->file, (long)from, SEEK_SET) != 0 ||
                fread(word->data, 1, chunk, word->file) != chunk ||
                fseek(word->file, (long)(from + inserted), SEEK_SET) != 0 ||
                fwrite(word->data, 1, chunk, word->file) != chunk)
                return word_fail(impl, word, "word temporary file insertion failed");
            suffix -= chunk;
        }
        if (fseek(word->file, (long)offset, SEEK_SET) != 0 ||
            fwrite(src, 1, inserted, word->file) != inserted)
            return word_fail(impl, word, "word temporary file insertion failed");
    }
    word->len = total;
    word->cache_len = 0;
    word->append_position = 0;
    return word_recount(impl, word);
}

int mdf_word_erase(mdf_impl *impl, mdf_word_storage *word, size_t offset, size_t len)
{
    size_t suffix;
    size_t moved;
    size_t chunk;

    if (word->status != MDF_OK || offset > word->len || len > word->len - offset) return -1;
    if (len == 0) return 0;
    suffix = word->len - offset - len;
    if (word->file == NULL) {
        memmove(word->data + offset, word->data + offset + len, suffix);
    } else {
        if (word_reserve(impl, word, impl->word_memory_limit + 1) != 0) return -1;
        moved = 0;
        while (moved < suffix) {
            chunk = suffix - moved;
            if (chunk > impl->word_memory_limit) chunk = impl->word_memory_limit;
            if (fseek(word->file, (long)(offset + len + moved), SEEK_SET) != 0 ||
                fread(word->data, 1, chunk, word->file) != chunk ||
                fseek(word->file, (long)(offset + moved), SEEK_SET) != 0 ||
                fwrite(word->data, 1, chunk, word->file) != chunk)
                return word_fail(impl, word, "word temporary file deletion failed");
            moved += chunk;
        }
    }
    return mdf_word_shrink(impl, word, word->len - len);
}

void mdf_word_clear(mdf_word_storage *word)
{
    char *data;
    size_t cap;

    data = word->data;
    cap = word->cap;
    if (word->file != NULL) fclose(word->file);
    memset(word, 0, sizeof(*word));
    word->data = data;
    word->cap = cap;
}

void mdf_word_destroy(mdf_impl *impl, mdf_word_storage *word)
{
    mdf_word_clear(word);
    mdf_free_mem(&impl->allocator, word->data, word->cap);
    word->data = NULL;
    word->cap = 0;
}

mdf_text_view mdf_text_memory(const char *data, size_t len)
{
    mdf_text_view text;

    memset(&text, 0, sizeof(text));
    text.parts[0].data = data;
    text.parts[0].len = len;
    text.len = len;
    return text;
}

mdf_text_view mdf_text_word(mdf_word_storage *word)
{
    mdf_text_view text;

    text = mdf_text_memory(word->file == NULL ? word->data : NULL, word->len);
    text.parts[0].word = word;
    return text;
}

mdf_text_view mdf_text_slice(mdf_text_view text, size_t offset, size_t len)
{
    if (offset > text.len) offset = text.len;
    if (len > text.len - offset) len = text.len - offset;
    if (offset >= text.parts[0].len) {
        offset -= text.parts[0].len;
        text.parts[0] = text.parts[1];
        memset(&text.parts[1], 0, sizeof(text.parts[1]));
    }
    text.parts[0].offset += offset;
    text.parts[0].len -= offset;
    if (text.parts[0].len > len) text.parts[0].len = len;
    text.parts[1].len = len - text.parts[0].len;
    text.len = len;
    return text;
}

int mdf_text_join(mdf_text_view *out, mdf_text_view first, mdf_text_view second)
{
    if (first.len == 0) { *out = second; return 0; }
    if (second.len == 0) { *out = first; return 0; }
    if (first.parts[1].len != 0 || second.parts[1].len != 0 ||
        second.len > (size_t)-1 - first.len) return -1;
    *out = first;
    out->parts[1] = second.parts[0];
    out->len += second.len;
    return 0;
}

const char *mdf_text_read(mdf_impl *impl, mdf_text_view text, size_t offset, size_t *len)
{
    mdf_text_piece piece;

    if (offset >= text.len) { *len = 0; return NULL; }
    piece = text.parts[0];
    if (offset >= piece.len) { offset -= piece.len; piece = text.parts[1]; }
    if (*len > piece.len - offset) *len = piece.len - offset;
    if (piece.data != NULL) return piece.data + piece.offset + offset;
    return mdf_word_read(impl, piece.word, piece.offset + offset, len);
}

int mdf_text_byte(mdf_impl *impl, mdf_text_view text, size_t offset)
{
    mdf_text_piece piece;

    if (offset >= text.len) return 0;
    piece = text.parts[0];
    if (offset >= piece.len) { offset -= piece.len; piece = text.parts[1]; }
    if (piece.data != NULL) return (unsigned char)piece.data[piece.offset + offset];
    return mdf_word_byte(impl, piece.word, piece.offset + offset);
}

size_t mdf_text_decode(mdf_impl *impl, mdf_text_view text, size_t offset, unsigned long *cp)
{
    char unit[4];
    size_t len;
    size_t i;

    if (offset >= text.len || (impl != NULL && impl->word_error != MDF_OK)) { *cp = 0; return 0; }
    if (text.parts[1].len == 0 && text.parts[0].data != NULL)
        return utf8_decode_codepoint(text.parts[0].data + text.parts[0].offset + offset,
                                     text.len - offset, cp);
    unit[0] = (char)mdf_text_byte(impl, text, offset);
    if ((unsigned char)unit[0] < 0x80) { *cp = (unsigned char)unit[0]; return 1; }
    len = text.len - offset;
    if (len > sizeof(unit)) len = sizeof(unit);
    for (i = 1; i < len; i++) unit[i] = (char)mdf_text_byte(impl, text, offset + i);
    return utf8_decode_codepoint(unit, len, cp);
}

size_t mdf_text_columns(mdf_impl *impl, mdf_text_view text)
{
    size_t offset;
    size_t cols;
    size_t adv;
    unsigned long cp;

    cols = 0;
    offset = 0;
    while (offset < text.len) {
        adv = mdf_text_decode(impl, text, offset, &cp);
        if (adv == 0) break;
        cols += utf8_display_width(cp);
        offset += adv;
    }
    return cols;
}
