# libmdf Agent Instructions

## Streaming Is A Hot-Path Contract

Streaming in libmdf means producer-to-consumer streaming. It does not mean
buffering content internally and later replaying it in streaming-shaped chunks.

For ANSI output, the core invariant is:

```text
one parser/renderer decision buffer -> one emission -> one sink write
```

The emission trace is not a separate truth source. It must describe the exact
sink write that just happened:

```text
trace event count == sink write count
trace event bytes == sink write bytes
trace event order == sink write order
```

If a trace shows word-sized emissions while the sink receives a whole line, the
trace is lying and the implementation is wrong.

## Buffering Rules

Only buffer what is required to make the next rendering decision.

Allowed decision buffers:

* A current word or styled word while waiting for the boundary that proves it can
  be emitted.
  Word memory defaults to 4 KiB and is configurable up to 64 KiB. The explicitly
  authorized overflow policy is file backing, capped at 1 MiB per pending word;
  after that cap discard additional bytes until the ordinary decision boundary.
  Disabling spooling instead retains only the configured in-memory prefix.
  Replay file-backed words through bounded reads and the same layout/emitter,
  then close the file. Storage limits never authorize speculative output or a
  different wrapping policy. Cleanup also covers reset, failure and destroy.
* The minimum inline delimiter/link/code/entity state needed to decide the
  syntax.
* Table buffers required by the configured table mode:
  * full table mode may buffer the table before rendering,
  * row table mode may buffer only what is required to emit header plus first row
    and then subsequent rows.

Disallowed buffering:

* A second renderer/output buffer after a decision is already made.
* Coalescing two or more decided words into one sink write.
* Emitting word-sized trace events while writing larger chunks to the sink.
* Direct ANSI writes that bypass the decision emission path.
* Post-processing a completed line or paragraph into fake streaming chunks.

## Emission Buffer Ownership

Emission composition must use the single per-instance emission buffer interface.
Do not add local stack buffers, local heap allocations, or per-helper scratch
arrays to assemble output bytes that are about to be emitted.

The default emission buffer policy is:

* allocate the per-instance emission buffer through the configured allocator,
* initial capacity is 128 bytes,
* max capacity is 8192 bytes when the configured max is 0,
* grow only as needed,
* keep the high-water allocation for the instance lifetime,
* release owned storage on instance destroy.

Developer supplied emission buffers must be explicit:

* fixed supplied buffers are hard bounded and must not grow,
* owned supplied buffers may grow through the configured allocator up to max,
* non-owned supplied buffers are treated as bounded unless ownership is
  explicitly transferred,
* a single decision emission that exceeds the configured max must fail instead
  of silently bypassing the buffer.

## Emission Surface

ANSI renderer code must route real output through the decision emission surface.
Do not add new direct `mdf_write_*` calls in ANSI hot paths. A direct sink write
is only acceptable in code that is explicitly not part of ANSI streaming output,
or when the write is the emission surface itself.

The preferred shape is:

```text
decide -> emit exact bytes once -> update visible state
```

Do not split this into:

```text
decide -> append to renderer buffer -> later flush -> trace something else
```

## Tests Must Prove The Contract

Every streaming-sensitive change must include a test that compares real sink
writes and emission trace events from the same render. Final rendered output is
not enough. Trace-only tests are not enough.

Required observable assertions:

* The sink write sequence and trace sequence are identical one-to-one.
* Headings such as `# The Outcome-Based Agile Framework` emit the marker and
  each word as separate decided emissions.
* A regression where the sink receives `The Outcome-Based Agile Framework` as
  one write must fail immediately.
* Tables are tested under their configured buffering mode instead of the normal
  word-streaming invariant.

## Parity Strategy

Go `mdf` is the behavioral reference for ANSI streaming. Prefer comparing the C
hot path against Go's single pending decision buffer model rather than
accumulating compatibility shims.

When parity and local inferred behavior disagree, parity wins unless the user
explicitly changes the contract.

In plain inline text, underscores following ASCII letters/digits, including
continued underscore runs, stay literal rather than becoming spaces or opening
emphasis. This includes identifiers and math-like text such as `$I_{k,j}(F)$`;
it does not introduce math rendering. Go's different handling is covered by
named, golden-backed parity exceptions and native sink/trace tests.

Literal U+00A0 NO-BREAK SPACE is preserved in ANSI and HTML output. Go replaces
it with U+0020 at final emission; that must not change libmdf's bytes or browser
wrapping. For literal-NBSP inputs, parity compares original C output and traces
across source fragments, then compares final output for a separate NBSP-to-SPACE
input control under the existing parity rules. Go's oversized-NBSP-word layout
and emission grouping also differ.
Keep the original bytes, styles and decision boundaries pinned by native goldens
and sink/trace tests; do not exclude the complete document from the control comparison.

Nested emphasis follows the explicitly authorized word-boundary contract:
emit decided words without retaining the complete nested span. After a nested
word has been emitted, an unmatched opening marker is not restored at the outer
boundary or EOF. An escaped marker remains literal (for example, `\_bar`).
This recovery rule overrides Go where its output differs. Storage limits apply
per word, independently of nested-span length and input fragmentation.

The explicit zero-right-margin URL contract overrides Go's hard wrapping:
visible terminal URLs remain byte-contiguous and use terminal autowrap,
regardless of left margin. Native tests pin URI bytes, subsequent cursor layout,
and real sink/trace parity. Corpus checks for these URI-bearing ANSI documents
compare fragmented C rendering against whole-source C rendering; other cases
continue to compare against Go. Do not restore Go's newline insertion in URLs.

## Pager Cache Ownership

The pager is an explicitly buffered consumer of renderer output. Its cache is
bounded independently of file size: 1 MiB of contiguous rendered content by
default, configurable down to 64 KiB. Word-buffer limits are unrelated. Metadata
and search workspace have a separate bound and never reduce content capacity.
Cap metadata array growth at the useful cache entry count; arbitrary cache
budgets must not exhaust workspace merely by rounding past a power of two.
Terminals whose content-cell count or rendered viewport exceeds the allowance
fail explicitly with MDF_ERROR_TERMINAL_SIZE, including on resize; do not expand
the configured content allocation. Internal seam tests exercise 1 KiB caches
below the public configuration minimum. Named files and seekable callback sources are read on demand;
never copy their complete input to memory or a backing file. Keep the live
parser for forward progress. Recover evicted Markdown or changed-width views
by cancellable source replay. Text indexes are sparse and bounded; file offsets
and global row positions must work beyond 4 GiB on 32-bit targets.
Initial read-ahead continues through undecided parser feeds until EOF or the
configured content/read-ahead bound. A fitting document must be cached in full;
scrolling its retained tables must not reread or rerender the source.
The input bound also applies before any complete row exists or a viewport is
full. A redraw or query edit alone must not resume a paused input scan.
Evict retained rows only when appending content requires space; extending an
unfinished view is not itself grounds for discarding cached history.

Show `--%` until EOF is known at the current width. Search highlights describe
cached content; match ordinals and totals are document-wide. Show `?` for an
ordinal whose prefix has not been searched, or a total not yet established for
the active query. Knowing EOF before a query is not knowing its match total.
Query changes and width changes invalidate counts. Count rendered text,
including evicted output, through bounded query state and scalar counters;
never retain a file-wide list of match positions or scan to EOF merely because
a query was entered. Preserve the renderer's decision/sink/trace contract
beneath this consumer cache.
Resize preserves viewed and selected passages, including hits chosen from the
cache, and derives their ordinals from the new rendering. Neither a cache-local
index nor an old document-wide rank identifies a passage: table wrapping can
remove or introduce earlier matches. Keep bounded passage context and source
hints for reflow; any necessary replay remains bounded and cancellable.
Duplicate passage anchors use positions relative to their source feed, replayed
at that exact source boundary, rather than proportions of unrelated caches.
Keep source coordinates when a document is fully cached. If narrowing makes it
exceed the allowance, replay to its anchors using the same content allocation;
the first new cache window need not contain the viewed or selected passage.
Search traversal is cyclic across the document: `n` wraps at EOF, `N` wraps at
the beginning, and both skip empty cache slices. A query absent from the entire
document restores the original viewport after one complete traversal. Keep
backward cache ranges contiguous even when ordinary read-ahead stops early.
Retain pending query context across cache seams in both directions, including
matches longer than the viewport. Query-aware text checkpoints must belong to
the current query and have no undecided match prefix.
A wrapped search must resolve pending matches at the origin before declaring
its traversal complete; retaining the starting row alone is insufficient.
Styled match extents can exceed the entire content cache (for example, OSC8
URLs behind short labels). Scan through them with bounded query state, retain
only the selected endpoints/ordinal, and recover its visible viewport on demand.
Never protect the complete styled match in the cache or restart indefinitely
at its pending prefix. A single rendered row that cannot fit must report
MDF_ERROR_TERMINAL_SIZE, even when rolling back would leave an empty buffer.

## Release Build Ownership

Build every Release target from this checkout in `build/<target>-release`.
Native Release tests and SDK/CLI packaging share `build/x86_64-linux-gnu-release`.
Do not introduce a separate source-smoke build or an alternate native producer.
Source archive verification checks the shipped inventory, version, bytes and
executable modes against the committed release inputs without another build.
