#ifndef MDF_PAGER_QUERY_H
#define MDF_PAGER_QUERY_H

#include "libmdf/mdf.h"
#include <stdint.h>

typedef struct mdf_pager_query_cell {
    unsigned long codepoint;
    size_t fallback;
    uint64_t row;
    size_t column;
    int boundary;
} mdf_pager_query_cell;

typedef struct mdf_pager_query_position {
    uint64_t row;
    size_t column; /* Visible codepoints, excluding terminal control sequences. */
} mdf_pager_query_position;

typedef struct mdf_pager_query_match {
    mdf_pager_query_position start;
    mdf_pager_query_position end; /* Exclusive. */
    uint64_t ordinal; /* Zero when the document prefix has not been searched. */
} mdf_pager_query_match;

/* One query-sized KMP pattern and history ring; no document-sized match index. */
typedef struct mdf_pager_query {
    mdf_allocator allocator;
    mdf_pager_query_cell *cells;
    size_t length;
    size_t matched;
    size_t cursor;
    uint64_t generation;
    uint64_t count;
    uint64_t total;
    int prefix_known;
    int total_known;
    mdf_pager_query_match last;
    int have_last;
} mdf_pager_query;

void mdf_pager_query_destroy(mdf_pager_query *query);
int mdf_pager_query_set(mdf_pager_query *query, const char *text, size_t length);
void mdf_pager_query_restart(mdf_pager_query *query, int prefix_known, uint64_t count);
void mdf_pager_query_invalidate(mdf_pager_query *query);
int mdf_pager_query_push(mdf_pager_query *query, unsigned long codepoint,
                         uint64_t row, size_t column);
void mdf_pager_query_finish(mdf_pager_query *query);
mdf_pager_query_position mdf_pager_query_pending(const mdf_pager_query *query);
int mdf_pager_query_position_compare(mdf_pager_query_position a, mdf_pager_query_position b);

#endif
