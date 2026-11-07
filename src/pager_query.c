#include "mdf_internal.h"
#include "pager_query.h"
#include "render_internal.h"
#include "unicode_casefold.h"

#include <string.h>

void mdf_pager_query_destroy(mdf_pager_query *query)
{
    mdf_free_mem(&query->allocator, query->cells, query->length * sizeof(*query->cells));
    query->cells = NULL;
    query->length = 0;
}

void mdf_pager_query_restart(mdf_pager_query *query, int prefix_known, uint64_t count)
{
    query->matched = 0;
    query->cursor = 0;
    query->count = count;
    query->prefix_known = prefix_known;
    query->have_last = 0;
}

void mdf_pager_query_invalidate(mdf_pager_query *query)
{
    query->generation++;
    query->total_known = 0;
    mdf_pager_query_restart(query, 0, 0);
}

int mdf_pager_query_set(mdf_pager_query *query, const char *text, size_t length)
{
    unsigned long folded[3];
    unsigned long codepoint;
    size_t offset;
    size_t consumed;
    size_t count;
    size_t i;
    size_t j;

    mdf_pager_query_destroy(query);
    mdf_pager_query_invalidate(query);
    count = 0;
    for (offset = 0; offset < length; offset += consumed) {
        consumed = utf8_decode_codepoint(text + offset, length - offset, &codepoint);
        if (consumed == 0) return -1;
        i = mdf_unicode_casefold(codepoint, folded);
        if (count > (size_t)-1 - i) return -1;
        count += i;
    }
    if (count == 0) return 0;
    if (count > (size_t)-1 / sizeof(*query->cells)) return -1;
    query->cells = (mdf_pager_query_cell *)mdf_alloc(&query->allocator, count * sizeof(*query->cells));
    if (query->cells == NULL) return -1;
    query->length = count;
    memset(query->cells, 0, count * sizeof(*query->cells));
    count = 0;
    for (offset = 0; offset < length; offset += consumed) {
        consumed = utf8_decode_codepoint(text + offset, length - offset, &codepoint);
        j = mdf_unicode_casefold(codepoint, folded);
        for (i = 0; i < j; i++) query->cells[count++].codepoint = folded[i];
    }
    for (i = 1; i < query->length; i++) {
        j = query->cells[i - 1].fallback;
        while (j != 0 && query->cells[i].codepoint != query->cells[j].codepoint)
            j = query->cells[j - 1].fallback;
        if (query->cells[i].codepoint == query->cells[j].codepoint) j++;
        query->cells[i].fallback = j;
    }
    return 0;
}

/* Only complete source codepoints may begin/end a match, even when Unicode
 * case folding expands one codepoint into several (for example, sharp s). */
int mdf_pager_query_push(mdf_pager_query *query, unsigned long codepoint,
                         uint64_t row, size_t column)
{
    unsigned long folded[3];
    size_t count;
    size_t i;
    int found;

    if (query->length == 0) return 0;
    count = mdf_unicode_casefold(codepoint, folded);
    found = 0;
    for (i = 0; i < count; i++) {
        query->cells[query->cursor].row = row;
        query->cells[query->cursor].column = column;
        query->cells[query->cursor].boundary = i == 0;
        query->cursor++;
        if (query->cursor == query->length) query->cursor = 0;
        while (query->matched != 0 && folded[i] != query->cells[query->matched].codepoint)
            query->matched = query->cells[query->matched - 1].fallback;
        if (folded[i] == query->cells[query->matched].codepoint) query->matched++;
        if (query->matched == query->length) {
            if (i + 1 == count && query->cells[query->cursor].boundary) {
                if (query->count == (uint64_t)-1) return -1;
                query->count++;
                query->last.start.row = query->cells[query->cursor].row;
                query->last.start.column = query->cells[query->cursor].column;
                query->last.end.row = row;
                query->last.end.column = column + 1;
                query->last.ordinal = query->prefix_known ? query->count : 0;
                query->have_last = 1;
                found = 1;
            }
            query->matched = query->cells[query->matched - 1].fallback;
        }
    }
    return found;
}

void mdf_pager_query_finish(mdf_pager_query *query)
{
    if (query->length != 0 && query->prefix_known) {
        query->total = query->count;
        query->total_known = 1;
    }
}

mdf_pager_query_position mdf_pager_query_pending(const mdf_pager_query *query)
{
    size_t start;
    mdf_pager_query_position position;

    position.row = (uint64_t)-1;
    position.column = 0;
    if (query->matched == 0) return position;
    start = query->cursor >= query->matched ? query->cursor - query->matched :
        query->length - (query->matched - query->cursor);
    position.row = query->cells[start].row;
    position.column = query->cells[start].column;
    return position;
}

int mdf_pager_query_position_compare(mdf_pager_query_position a, mdf_pager_query_position b)
{
    if (a.row != b.row) return a.row < b.row ? -1 : 1;
    if (a.column != b.column) return a.column < b.column ? -1 : 1;
    return 0;
}
