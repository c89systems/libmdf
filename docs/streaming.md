# Streaming and rendering decisions

ANSI output follows one decision buffer, one emission and one sink write.
When `write_trace` is configured, its event count, bytes and order describe
exactly the writes that succeeded. Input fragmentation never supplies a word
boundary. Source and sink callbacks are synchronous; the application owns any
nonblocking transport or bounded output queue above this interface.

## Incremental documents

For an event-loop or another producer that supplies Markdown fragments, keep
one renderer for one document and use the additive document lifecycle. `feed`
emits each final renderer decision as it is made. For example, a real input
space normally closes and emits the preceding word while the renderer retains
the separator until wrapping decides it. `flush` is an output-neutral
soft-boundary check: it never emits, resolves input, or acts as EOF. `feed`
resolves constructs when real input supplies their boundary; `finish_document`
applies EOF to anything still unfinished and closes output. The one
bound sink is synchronous and borrowed until it is replaced or the renderer is
destroyed, so its write callback must accept every complete decision emission
or fail the document. libmdf deliberately supplies no
partial-write, nonblocking-FD, or output-queue abstraction; an event-loop host
owns that bounded transport layer above this API.

```c
static int stdout_write(void *userdata, const char *src, size_t len)
{
    FILE *out = (FILE *)userdata;
    return fwrite(src, 1, len, out) == len ? 0 : -1;
}

mdf_sink sink;

sink.userdata = stdout;
sink.write = stdout_write;

renderer->set_sink(renderer, &sink);
renderer->feed(renderer, "Hello\n\n", 7);
renderer->flush(renderer);                     /* still not EOF */
renderer->set_geometry(renderer, 80, 2, 0);    /* next line uses the new margin */
renderer->feed(renderer, "**world**", 9);
renderer->finish_document(renderer);           /* the sole EOF operation */
renderer->begin_document(renderer);            /* now a distinct document may start */
```

Fragments must be nonempty. After successful finalization, further `feed`,
`flush`, or `finish_document` calls fail until `begin_document` succeeds.
Sink failure makes that document failed; libmdf never retries the write or queues
output. Receiver methods are the persistent-sink API: bind once with `set_sink`,
then call `render`, `write_token`, `finish`, `feed`, `flush`, or
`finish_document` without repeating the sink. The explicit free functions such
as `mdf_render`, `mdf_feed`, and `mdf_finish_document` instead borrow the sink
only for that call; they never install or replace the receiver binding.

To change total width and both ANSI margins atomically between incremental
feeds, call `renderer->set_geometry(renderer, width, left, right)` or the free
function `mdf_set_geometry`. `renderer->set_width` changes only total width and
keeps the current margins. Both setters preserve the active document, make no
sink write, and recompute retained inline code/link decisions under the new
geometry before they emit, including from a `render` source callback. A
left-margin prefix already written on the current line remains in place, and
a new margin is never inserted after text already emitted on that line;
subsequent line prefixes use the new margin. The resulting ANSI content width
must remain at least three columns. Setting unchanged geometry preserves
pending decisions. The new geometry also applies to later documents. If
pending-decision reflow runs out of memory, the previous geometry remains
configured, but callers must reset and replay the failed document. A
full-buffer ANSI table uses the geometry in effect when its terminating line,
EOF or retention limit makes it render, even if geometry changed while rows
were retained.
In row-buffered ANSI mode, the header and first body row (or the first two
headerless rows) emit once their layout can be decided; already-emitted rows
remain unchanged, while subsequent rows and the closing border refit the
latest geometry.

Neither setter reflows a completed sink write, so call
`renderer->reset` and replay caller-owned source for a complete reflow. Reset
and sink replacement close enabled ANSI terminal state before discarding
parser state; escape-free renderers discard state without terminal cleanup.
Reset and sink replacement are rejected while a synchronous `render` call is
active. A source callback may change geometry for later output. Sink and trace
callbacks cannot change geometry, reset, or move a live render to a different
sink. libmdf does not retain completed document input for reflow. If terminal cleanup on the old
sink fails during replacement, the renderer still discards state and binds the
new sink; replay remains the caller's responsibility.

ANSI and HTML tables retain at most 65536 bytes or 1024 rows before switching
from full buffering to row streaming. Column widths for ANSI are calculated
from the completed buffered rows at that point; subsequent rows use those
widths. The current row continues through the same table, with no repeated
header or inserted border. HTML retains its browser-managed column layout.
The limits apply to both source-callback and incremental rendering and are
currently fixed, with no public configuration option. A single pending row or
undecided table prefix that cannot fit still fails with `MDF_ERROR_PARSE`.
Incremental charts retain their 65536-byte limit and fail if it is exceeded.
These limits count retained decision data, not total allocated memory.
The incremental lifecycle supports ANSI and HTML documents; HTML callers must
set an explicit title before the first feed because automatic title detection
is a one-shot source feature.
HTML decks accept synchronous source rendering, with one slide buffered at a
time; manual tokens and incremental feeds are unsupported. See the
[deck guide](html-slide-deck-mode.md).

## Pending words

An undecided ordinary or emphasis word retains up to **4 KiB in memory** by
default. Larger words use an automatically deleted temporary file containing
at most **1 MiB**. Filling either limit never forces a rendering decision.
Once a real boundary or EOF resolves the word, file-backed input replays through
bounded reads and normal decision emissions, preserving wrapping and styles.
After the file cap is reached, further bytes of that word are discarded.
Temporary-file creation or I/O failure returns `MDF_ERROR_IO`.

Nested emphasis emits decided words at word boundaries, so its storage limits
apply to each word rather than the complete span. Once nested words have been
emitted, a missing closing marker ends the style at the outer boundary or EOF;
the unmatched opener is not reinserted. For example, `*foo _bar baz*` renders as
`foo bar baz`. Use `*foo \_bar baz*` to request the literal `foo _bar baz`.

Configure an idle renderer with
`renderer->set_word_buffer(renderer, memory_bytes, disable_spooling)`:
zero selects 4096 bytes, and the largest accepted memory limit is 65536 bytes.
With spooling disabled, a word retains only its memory-limit prefix and drops
excess bytes until its normal boundary. For example:

```c
renderer->set_word_buffer(renderer, 8192, 0); /* spool beyond 8 KiB */
renderer->set_word_buffer(renderer, 4096, 1); /* truncate beyond 4 KiB */
```

Lua handles and document streams provide `:set_word_buffer(bytes, disabled)`.
Files close on word completion, reset, rendering failure and destruction.
These are pending-word limits; inline syntax decisions, table modes and the
per-instance emission buffer retain their separate contracts. A fixed emission
buffer must still fit each individual decision emission.

## Tables and terminal links

Table cells render an escaped pipe (`\|`) as a literal `|`, including inside
inline code. Other code backslashes remain literal, as do pipe escapes in
code outside tables.

ANSI link wrapping considers the final label word together with attached
closing parentheses and punctuation. With OSC8 enabled, literal parentheses
outside the Markdown label remain outside the hyperlink, and enclosing
emphasis, heading, and blockquote styles resume after the link. These
decisions are preserved across fragmented input and line wraps.

Without OSC8, destinations are printed after the label. When `margin_right` is
zero, visible URLs outside table cells remain byte-contiguous: no newline,
indentation or style reset is inserted inside a URL. The terminal wraps long
URLs naturally, allowing direct terminals to detect the complete address. This
applies regardless of the left margin; natural continuation rows have no
inserted left margin.
Styled URLs follow the same rule. For recognized URL words, emphasis resets
are deferred until the complete word has been emitted.
Set `width` to the terminal's physical column count so subsequent words wrap
from the actual cursor, including delayed wrapping at an exact right edge.
A positive right margin retains hard URL wrapping. OSC8 remains optional and
off by default in the library.

`cmdf --osc8 auto` and `mdf_detect_osc8_support()` use environment hints.
When specific terminal identifiers are absent, `TERM=xterm` and `xterm-*`
(including `xterm-256color`) provide an optimistic compatibility fallback.
This also works over SSH without forwarding `TERM_PROGRAM` or `VTE_VERSION`:
[OpenSSH forwards TERM for terminal sessions by default](https://man.openbsd.org/ssh_config#SendEnv).
Specific terminal information, including old VTE versions, retains precedence;
`OSC8=0` disables automatic OSC8, and `--osc8 off` disables it explicitly.
Detection performs no terminal I/O and is a heuristic rather than capability
negotiation. ANSI destination policy still keeps ordinary pipes escape-free.

Plain-text link detection also depends on the terminal and multiplexer. tmux
pane redraws can prevent an outer terminal from joining a wrapped address even
when libmdf emitted its bytes contiguously. Use `cmdf --osc8 on` for explicit
hyperlink targets through a supporting tmux/terminal combination. tmux 3.4 and
newer support OSC8; `hyperlinks` must be enabled in tmux's `terminal-features`
for the outer terminal. This is separate from libmdf's zero-right-margin
byte/layout contract.

Literal U+00A0 NO-BREAK SPACE is preserved in ANSI and HTML, keeping its
non-breaking meaning in browser layout and copied text. Go replaces this
character with an ordinary space during final emission, and its oversized-NBSP
word layout also differs. Parity checks original C output and traces across
source fragments, then checks a separate input copy with only NBSP changed to
SPACE for final-output comparison under the existing parity rules. The rest of
the document stays in that control comparison; original C emissions keep their
exact trace comparison because Go's emission grouping also differs.
Native goldens and fragmented-input sink/trace tests assert libmdf's original
bytes and wrapping; no renderer output or actual trace is normalized.

The Go `mdf` implementation is the behavioral reference for ANSI streaming.
Plain intraword underscores remain literal, including repeated runs in
identifiers and math-like text such as `$I_{k,j}(F)$`. Go instead changes some
of these underscores to spaces or interprets them as emphasis. The affected
named fixtures have exact ANSI/HTML goldens and fragmented sink/trace coverage;
this does not add a math renderer or change the nested-emphasis recovery rule.
Zero-right-margin URL layout intentionally differs: native tests pin contiguous
URI bytes and following cursor layout, while corpus checks compare fragmented
C output and emissions against whole-source C rendering for URI-bearing ANSI
documents. The nested-emphasis recovery rule described above also takes
precedence over Go and is pinned by native sink/trace tests. Other cases retain
Go as the comparison oracle. The remaining documented exceptions cover narrow
URI wrapping (libmdf keeps `://` atomic rather than rendering a scheme followed
by a wrapped `//host`),
escaped pipes inside table code spans, and correct Markdown link-label handling
where Go is known to be wrong. The link-label exception
keeps styled labels inside their OSC8 hyperlink and follows CommonMark for
unequal emphasis delimiters. These remaining exceptions are listed in
`testdata/goldens/libmdf/PARITY_EXCLUSIONS.txt` and protected by matching
libmdf goldens; ANSI/trace entries require styled and boring exact outputs at
each excluded width, and HTML entries require exact HTML outputs.

HTML output is streamed, but byte-for-byte streaming parity with the Go CLI is
not a supported contract. HTML tables rely on browser layout rather than ANSI
alignment.
