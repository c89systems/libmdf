#ifndef LIBMDF_WORD_STORAGE_H
#define LIBMDF_WORD_STORAGE_H

#include <stdio.h>
#include "libmdf/mdf.h"

#define MDF_WORD_MEMORY_DEFAULT MDF_WORD_BUFFER_DEFAULT_BYTES
#define MDF_WORD_MEMORY_MAX MDF_WORD_BUFFER_MAX_BYTES
#define MDF_WORD_FILE_MAX MDF_WORD_SPOOL_MAX_BYTES

/* One undecided word. File backing is explicit and never emits input. */
typedef struct mdf_word_storage {
    char *data;
    size_t len;
    size_t cap;
    FILE *file;
    size_t cache_start;
    size_t cache_len;
    int append_position;
    int truncated;
    mdf_status status;
    unsigned char head[4];
    unsigned char tail[4];
    size_t units;
    size_t question;
    size_t numeric_colon;
    int non_ascii;
    int has_scheme;
    size_t last_scheme;
    int has_dot;
    int has_slash;
    int has_angle;
    int escaped_tail;
} mdf_word_storage;

typedef struct mdf_text_piece {
    const char *data;
    mdf_word_storage *word;
    size_t offset;
    size_t len;
} mdf_text_piece;

/* A word, or the bounded join of a word and its adjacent code segment. */
typedef struct mdf_text_view {
    mdf_text_piece parts[2];
    size_t len;
} mdf_text_view;

struct mdf_impl;
int mdf_word_append(struct mdf_impl *impl, mdf_word_storage *word, const char *src, size_t len);
const char *mdf_word_read(struct mdf_impl *impl, mdf_word_storage *word, size_t offset, size_t *len);
int mdf_word_byte(struct mdf_impl *impl, mdf_word_storage *word, size_t offset);
int mdf_word_copy(struct mdf_impl *impl, mdf_word_storage *word, size_t offset, char *dst, size_t len);
int mdf_word_shrink(struct mdf_impl *impl, mdf_word_storage *word, size_t len);
int mdf_word_insert(struct mdf_impl *impl, mdf_word_storage *word, size_t offset, const char *src, size_t len);
int mdf_word_erase(struct mdf_impl *impl, mdf_word_storage *word, size_t offset, size_t len);
void mdf_word_clear(mdf_word_storage *word);
void mdf_word_destroy(struct mdf_impl *impl, mdf_word_storage *word);
mdf_text_view mdf_text_memory(const char *data, size_t len);
mdf_text_view mdf_text_word(mdf_word_storage *word);
mdf_text_view mdf_text_slice(mdf_text_view text, size_t offset, size_t len);
int mdf_text_join(mdf_text_view *out, mdf_text_view first, mdf_text_view second);
const char *mdf_text_read(struct mdf_impl *impl, mdf_text_view text, size_t offset, size_t *len);
int mdf_text_byte(struct mdf_impl *impl, mdf_text_view text, size_t offset);
size_t mdf_text_decode(struct mdf_impl *impl, mdf_text_view text, size_t offset, unsigned long *cp);
size_t mdf_text_columns(struct mdf_impl *impl, mdf_text_view text);

#endif
