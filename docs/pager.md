# Pager and log viewer

Use `cmdf -p FILE` for an interactive Markdown or text viewer. The same pager
is available through the [C API](c-api.md) and [Lua facade](lua.md).

```sh
cmdf -p document.md
cmdf -p --pager-format text application.log
cmdf -p --pager-cache 65536 document.md
```

Pager Markdown rendering always uses a live incremental
receiver, with or without `--incremental`. It reads enough input for the current
view and a bounded cache, then pauses until navigation needs more content.
Initial loading continues across parser decisions that need more input. Files
that fit within the content and read-ahead allowance reach EOF on opening;
scrolling their cached content does not reread or rerender the source.
The input bound also applies when whitespace or an undecided word leaves the
screen short or blank. Scrolling resumes input; redraws and query edits reuse
the retained prefix.

## Navigation

`--pager` requires a named input file and terminal stdin/stdout. It uses the
alternate screen, restores the terminal on `q` or `Esc`, and provides `j`/`k`,
arrow up/down, Page Up/Page Down, Home/End, and Ctrl-U/Ctrl-D navigation. Its
bottom bar uses inverted video with the selected ANSI theme and shows the
filename plus percentage viewed (`--%` until EOF is known). Press `/` to search case-insensitively;
matching text is highlighted as
the query is entered, and `n`/`N` move to the next/previous match. `q` or `Esc`
leaves search mode without leaving the pager. Markdown views rerender only after
SIGWINCH has been quiet for at least 250 ms; rapid resize events are coalesced.
Pager mode cannot be combined with
HTML/deck output, `--output`, write tracing, or input simulation. It can be
combined with `--incremental`; both use the same on-demand pager path.

## Input formats

`--pager-format auto|text|markdown` selects the pager's input interpretation.
`text` hard-wraps literal text at terminal display columns, preserving Markdown
punctuation and supporting `/` search and `n`/`N` navigation. Search also matches
within words split across display wraps. `auto` keeps extension-based selection;
`markdown` renders Markdown regardless of filename. The opened file's length
defines the view; appended log data is not followed. Keep the existing bytes
stable until the pager returns.

## Cache and read-ahead

Both formats support files larger than memory without copying the whole input
to RAM or disk. The default allowance is **1 MiB of rendered content** in one
contiguous allocation, plus a NUL terminator. ANSI sequences and newlines count
as rendered bytes. The allocation is reused across cache misses and resizing.
Change it with
`--pager-cache BYTES` (`0` selects the default; nonzero values must be at least
65536). Word buffers and table decision storage retain their independent limits;
the word buffer's 64 KiB maximum does not limit pager content.

Indexes, search text and matches use separate bounded workspace, allocated only
as needed. Its ceiling, including allocation growth, is 64 times the content
allowance plus 32 KiB; it does not consume or shrink the content allowance.

A terminal with more content cells than the allowance, or whose rendered
viewport/row exceeds it, returns `MDF_ERROR_TERMINAL_SIZE` after terminal cleanup:
`terminal too large for pager content cache`. This applies both when opening a
file and after resizing. Allocation failure remains `MDF_ERROR_NOMEM`.

## Search and reflow

Forward navigation and `n` searches continue the paused parser. Text keeps a
bounded sparse index for rereading evicted content. Markdown backward cache
misses reconstruct parser state by replaying the necessary prefix; width changes
also rerender from the source. These scans remain cancellable. `End`/`G` can
require reading through EOF, but retains only a bounded tail window. Search
highlights refresh when scrolling loads content from either direction. Match
numbering is document-wide: `13/?` means the thirteenth match with the total
still unknown. `?/?` means the query has not covered the prefix needed to number
the selected match. Reaching EOF before entering a query does not establish its
total. Once that query has covered the whole document, its total remains known
across cache changes. A new query or terminal width invalidates those counts.
Resizing preserves the selected passage and recomputes its ordinal against the
new layout, including selections made entirely from cached hits. Bounded text
context anchors the passage when table wrapping changes earlier match counts.
Source-relative positions disambiguate repeated passages even when replay fills
a different amount of the cache.
They also preserve passages when narrowing makes a previously cached document
exceed the allowance.
Long queries are shortened in the status bar to keep counts visible.
Searching uses query-sized state and scalar counters, without a file-wide match
array or an EOF scan merely because a query was entered. Even when a match's
OSC8/style bytes exceed the entire cache, scanning continues with bounded query
state. The pager retains its endpoints and ordinal, loading and highlighting
only the visible portion. `End` may replay an
unsearched prefix to establish the active query's total; this remains cancellable.
`n` wraps from EOF to the first match and `N` wraps from the beginning to the
last match, skipping cache slices without matches. If the query is absent from
the entire document, either command restores the original viewport.

## Embedding

Embedded C callers can use the ANSI receiver's `pager_file` method or
`mdf_pager_file` with render options. Both accept an optional
`const mdf_pager_control *`; pass `NULL`
when cancellation is unnecessary. For example, with an application-owned
callback and context:

```c
mdf_pager_control control;
mdf_status status;
control.userdata = app;
control.should_cancel = app_wants_pager_closed;
status = viewer->pager_file(viewer, "application.log", MDF_PAGER_FORMAT_TEXT, &control);
if (status == MDF_CANCELLED) {
    /* The pager restored the terminal; continue the application. */
}
```

The callback runs on the calling thread during loading, rendering, search and
navigation; a nonzero result requests cancellation. Idle waits are capped at
50 ms. Synchronize application state shared with another thread. A currently
blocking source callback or terminal write must return before cancellation can
finish. Cooperative cancellation restores terminal and signal state and returns
`MDF_CANCELLED` without raising a signal. Receiver methods use independent pager
workspace and leave the receiver usable afterward.

For an explicit cache budget, use `mdf_pager_file_with_options` or its appended
receiver method, passing a zeroed `mdf_pager_options` with `cache_budget` set.
For callback input, use `mdf_pager_seekable_source` or the receiver's
`pager_seekable_source` method. Its borrowed `mdf_seekable_source` supplies
`read` and absolute `seek` callbacks; offsets use high/low unsigned 32-bit halves
so the public API remains C89-compatible on 32-bit platforms. The pager owns
neither the source nor its lifetime and creates no full-input backing file.
The older forward-only `mdf_pager_source` / `pager_source` API buffers at most
1 MiB before viewing and returns `MDF_ERROR_NOMEM` for larger input; migrate
large callback inputs to the seekable interface.

## Interrupts

Ctrl+C restores terminal state and forwards SIGINT to the application's
restored signal handler. If that handler returns or ignores SIGINT, the pager
returns `MDF_CANCELLED` rather than an I/O error. The default SIGINT disposition
still applies for callers without an overriding handler.
