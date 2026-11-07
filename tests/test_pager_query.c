#include "mdf_internal.h"
#include "pager_query.h"
#include "render_internal.h"
#include "unicode_casefold.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "query check failed at line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

/* Compare complete Unicode codepoints at each possible start, independently
 * of the incremental KMP state. This is the cache-search boundary contract. */
static int reference_match(const char *text, const char *pattern)
{
    unsigned long a[3];
    unsigned long b[3];
    unsigned long cp;
    size_t ac;
    size_t bc;
    size_t ai;
    size_t bi;
    size_t n;

    ac = bc = ai = bi = 0;
    for (;;) {
        if (bi == bc) {
            if (*pattern == '\0') return ai == ac;
            n = utf8_decode_codepoint(pattern, strlen(pattern), &cp);
            if (n == 0) return 0;
            pattern += n;
            bc = mdf_unicode_casefold(cp, b);
            bi = 0;
        }
        if (ai == ac) {
            if (*text == '\0') return 0;
            n = utf8_decode_codepoint(text, strlen(text), &cp);
            if (n == 0) return 0;
            text += n;
            ac = mdf_unicode_casefold(cp, a);
            ai = 0;
        }
        if (a[ai++] != b[bi++]) return 0;
    }
}

static size_t generate(char *out, size_t number, size_t length)
{
    static const char *alphabet[] = { "s", "S", "\303\237", "a" };
    size_t i;
    size_t used;

    used = 0;
    for (i = 0; i < length; i++) {
        const char *letter;
        size_t bytes;

        letter = alphabet[number % 4];
        number /= 4;
        bytes = strlen(letter);
        memcpy(out + used, letter, bytes);
        used += bytes;
    }
    out[used] = '\0';
    return used;
}

static int exercise(mdf_pager_query *query, const char *text, const char *pattern)
{
    uint64_t expected_rows[16];
    uint64_t found_rows[16];
    size_t expected_columns[16];
    size_t expected;
    size_t found;
    size_t i;
    size_t n;
    size_t characters;
    unsigned long cp;
    int result;

    expected = found = characters = 0;
    for (i = 0; text[i] != '\0'; i += n) {
        if (reference_match(text + i, pattern)) {
            expected_columns[expected] = characters % 2;
            expected_rows[expected++] = characters / 2;
        }
        n = utf8_decode_codepoint(text + i, strlen(text + i), &cp);
        CHECK(n != 0);
        characters++;
    }
    mdf_pager_query_restart(query, 1, 0);
    characters = 0;
    for (i = 0; text[i] != '\0'; i += n) {
        n = utf8_decode_codepoint(text + i, strlen(text + i), &cp);
        result = mdf_pager_query_push(query, cp, characters / 2, characters % 2);
        CHECK(result >= 0);
        if (result != 0) {
            CHECK(found < expected);
            CHECK(query->have_last);
            CHECK(query->last.start.row == expected_rows[found]);
            CHECK(query->last.start.column == expected_columns[found]);
            CHECK(query->last.end.row == characters / 2);
            CHECK(query->last.end.column == characters % 2 + 1);
            CHECK(query->last.ordinal == found + 1);
            CHECK(mdf_pager_query_position_compare(query->last.start, query->last.end) < 0);
            found_rows[found++] = query->last.start.row;
        }
        characters++;
    }
    CHECK(found == expected);
    CHECK(query->count == expected);
    CHECK(memcmp(expected_rows, found_rows, expected * sizeof(*expected_rows)) == 0);
    mdf_pager_query_finish(query);
    CHECK(query->total_known && query->total == expected);
    return 0;
}

typedef struct allocation_probe {
    size_t live;
    int fail;
} allocation_probe;

static void *probe_alloc(void *userdata, size_t bytes)
{
    allocation_probe *probe;
    void *result;

    probe = (allocation_probe *)userdata;
    if (probe->fail) return NULL;
    result = malloc(bytes);
    if (result != NULL) probe->live += bytes;
    return result;
}

static void probe_free(void *userdata, void *ptr, size_t bytes)
{
    allocation_probe *probe;

    probe = (allocation_probe *)userdata;
    if (ptr == NULL) return;
    probe->live -= bytes;
    free(ptr);
}

int main(void)
{
    static const char *special[][2] = {
        { "STRASSE Stra\303\237e stra\303\237e", "strasse" },
        { "s\303\237s", "s" }, { "s\303\237s", "ss" },
        { "sss", "\303\237" }, { "\305\277S\303\237", "ss" },
        { "K\342\204\252k", "k" }, { "K\342\204\252k", "KK" },
        { "\316\243\317\203\317\202", "\317\203" },
        { "a\314\201a\314\201", "a\314\201" },
        { "\346\227\245\346\234\254\350\252\236", "\346\234\254" },
        { "ababababa", "ababa" }, { "aaaaaaaaaaa", "aaa" }
    };
    mdf_pager_query query;
    allocation_probe probe;
    char pattern[16];
    char text[32];
    char *long_query;
    size_t pl;
    size_t tl;
    size_t pc;
    size_t tc;
    size_t p;
    size_t t;
    size_t i;
    uint64_t generation;

    memset(&query, 0, sizeof(query));
    memset(&probe, 0, sizeof(probe));
    query.allocator.userdata = &probe;
    query.allocator.alloc = probe_alloc;
    query.allocator.free = probe_free;
    pc = 1;
    for (pl = 1; pl <= 3; pl++) {
        pc *= 4;
        for (p = 0; p < pc; p++) {
            generate(pattern, p, pl);
            CHECK(mdf_pager_query_set(&query, pattern, strlen(pattern)) == 0);
            tc = 1;
            for (tl = 0; tl <= 5; tl++) {
                for (t = 0; t < tc; t++) {
                    generate(text, t, tl);
                    CHECK(exercise(&query, text, pattern) == 0);
                }
                tc *= 4;
            }
        }
    }
    for (i = 0; i < sizeof(special) / sizeof(*special); i++) {
        CHECK(mdf_pager_query_set(&query, special[i][1], strlen(special[i][1])) == 0);
        CHECK(exercise(&query, special[i][0], special[i][1]) == 0);
    }
    CHECK(mdf_pager_query_set(&query, "x", 1) == 0);
    mdf_pager_query_restart(&query, 0, 0);
    CHECK(mdf_pager_query_push(&query, 'x', 0, 0) == 1);
    CHECK(query.last.ordinal == 0);
    mdf_pager_query_finish(&query);
    CHECK(!query.total_known);
    mdf_pager_query_restart(&query, 1, ((uint64_t)1 << 32) + 9);
    CHECK(mdf_pager_query_push(&query, 'x', 0, 0) == 1);
    mdf_pager_query_finish(&query);
    CHECK(query.total_known && query.total == ((uint64_t)1 << 32) + 10);
    CHECK(query.last.ordinal == query.total);
    mdf_pager_query_restart(&query, 1, 0);
    CHECK(query.total_known);
    CHECK(!query.have_last);
    generation = query.generation;
    mdf_pager_query_invalidate(&query);
    CHECK(query.generation != generation && !query.total_known);
    CHECK(mdf_pager_query_set(&query, "ababa", 5) == 0);
    CHECK(mdf_pager_query_pending(&query).row == (uint64_t)-1);
    CHECK(mdf_pager_query_push(&query, 'a', 7, 0) == 0);
    CHECK(mdf_pager_query_push(&query, 'b', 8, 0) == 0);
    CHECK(mdf_pager_query_pending(&query).row == 7);
    CHECK(mdf_pager_query_pending(&query).column == 0);
    CHECK(mdf_pager_query_push(&query, 'x', 9, 0) == 0);
    CHECK(mdf_pager_query_pending(&query).row == (uint64_t)-1);
    long_query = (char *)malloc(4096);
    CHECK(long_query != NULL);
    memset(long_query, 'x', 4095);
    long_query[4095] = '\0';
    CHECK(mdf_pager_query_set(&query, long_query, 4095) == 0);
    CHECK(probe.live <= 4095 * sizeof(mdf_pager_query_cell));
    for (i = 0; i < 20000; i++) CHECK(mdf_pager_query_push(&query, 'x', i / 80, 0) >= 0);
    CHECK(query.count == 20000 - 4095 + 1);
    free(long_query);
    probe.fail = 1;
    CHECK(mdf_pager_query_set(&query, "fail", 4) != 0);
    CHECK(probe.live == 0 && query.cells == NULL && query.length == 0 && !query.total_known);
    probe.fail = 0;
    CHECK(mdf_pager_query_set(&query, "recovered", 9) == 0);
    CHECK(mdf_pager_query_set(&query, "\303", 1) == 0);
    CHECK(mdf_pager_query_push(&query, 0xc3, 0, 0) == 1);
    CHECK(mdf_pager_query_set(&query, "", 0) == 0);
    CHECK(mdf_pager_query_push(&query, 'x', 0, 0) == 0);
    mdf_pager_query_destroy(&query);
    CHECK(probe.live == 0);
    return 0;
}
