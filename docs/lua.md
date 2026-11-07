# Lua 5.5 facade

Lua bindings currently target Lua 5.5 only.

All Lua renderers accept `ansi_mode = "auto" | "on" | "off"` and
`output_fd`, including string rendering, callback streaming, incremental
documents, and manual tokens. The default `"auto"` treats an unknown sink as
non-terminal: escape-free UTF-8 at width 80. Lua cannot infer the destination
of a writer callback. Set `output_fd = mdf.file_descriptor(output_file)` when
the callback writes to an open Lua file, including `io.stdout`. The descriptor
is borrowed for detection; output still goes through the writer callback.

`"off"` removes all terminal escapes, including input sequences in code and
manual tokens, color, resets, and OSC8. Unicode text, table borders, and chart
shapes remain available; the CLI name `--ascii` does not mean 7-bit ASCII.
`"on"` permits themed output even for strings or files. `boring = true`
reduces decoration independently and can still emit resets or OSC8 with
escapes enabled; combine it with `"off"` for escape-free boring output.
`osc8 = true` cannot override `"off"`.

Automatic ANSI policy and width resolve once when the renderer is created.
Binding or replacing a writer does not detect its destination again; recreate
the renderer when destination type changes. A positive `width` overrides
detection independently of escape policy, so `"off"` can still use a terminal's
width. HTML and deck rendering retain their existing behavior. The interactive
pager enables styling in auto mode and always uses terminal controls for
navigation, even when its Markdown renderer uses `"off"`.

Local tests and benchmarks build a pinned Lua 5.5.1 interpreter from the
[official Lua source archive](https://www.lua.org/ftp/) using Bootlin. Its
private loader and RPATH resolve both libc and the installed development SDK.
`make build-debug` builds this interpreter at `build/debug/lua-runtime/bin/lua`
and compiles the facade against the SDK installed at `build/debug/lua-sdk`.
Native CMake dependencies keep the interpreter, installed SDK, module and CLI
wrapper current. `make lua-rock` separately exercises local LuaRocks packaging.
Use `make lua-env` for the local interpreter and module paths; no dependency
`LD_LIBRARY_PATH` export is needed. The Lua source archive
is checksum-verified in the shared `CPKT_DEPENDENCY_CACHE`; the interpreter and
its build files remain under `build/` and are not shipped.

```lua
local mdf = require("libmdf")

local out = mdf.render("# hello\n\nworld\n", {
  ansi_mode = "off",
  width = 80,
})
io.write(out)
```

## Source callbacks

```lua
local mdf = require("libmdf")
local input = "# hello\n\nworld\n"
local pos = 1

mdf.render_stream(function(cap)
  if pos > #input then return nil end
  local chunk = input:sub(pos, pos + math.min(cap, 8) - 1)
  pos = pos + #chunk
  return chunk
end, function(chunk)
  io.write(chunk)
end, { output_fd = mdf.file_descriptor(io.stdout) })
```

The reader receives a maximum byte count and must return a string no longer
than that count; `nil` or an empty string signals EOF. The writer receives
output chunks; ANSI chunks correspond to decided emissions, and the ANSI-only
`write_trace` callback observes those writes. Returning `false` fails the
operation, while `nil` and truthy values accept it. These are synchronous
callbacks, not a partial-write or queued-output interface.

## Receiver handles

```lua
local mdf = require("libmdf")

local h = mdf.new({ boring = true })
io.write(h:render("# hello\n"))
h:close()
```

The top-level `mdf.render_stream(read, write, opts)` is the explicit-callback
form: its callbacks are borrowed for that call. For streaming receiver methods,
bind one callback once, then change width, reset, or replace the callback on
the object itself. `reset` closes ANSI state and discards unfinished parser
state even when the terminal callback fails; replay remains the caller's job.
When escapes are disabled, reset and sink replacement emit no terminal cleanup.
A `render_stream` reader may change geometry for later decisions, but `reset` and
sink replacement are rejected until
that synchronous render returns. This rejection also applies while terminal
reset and manual-token output invoke a sink or trace callback. `close` is
likewise rejected from an active reader, sink, or trace callback, preserving
the receiver for the outer call.
Rebinding the identical callback is a no-op.

Lua callback exceptions, including allocation failures while preparing callback
arguments, return through native cleanup before the original error object is
raised to the caller. Materialized result-copy failures also free the native
string. Handles retain a private snapshot of the declared options and font
configuration, sharing immutable Lua string storage; changing the original
tables does not change a live renderer. C-string arguments require strings
without embedded NUL bytes. Length-aware source, token and font byte strings
retain their binary length semantics.

```lua
local h = mdf.new({ boring = true })
h:set_sink(function(chunk) io.write(chunk) end)
h:set_width(80)
h:set_geometry(80, 2, 0) -- total width, left margin, right margin
h:render_stream(reader)
h:reset()
```

## Incremental documents

Lua hosts can use the same incremental document boundaries with a synchronous
callback. Normal document emissions occur during `write` or
`finish_document`; `reset` and sink replacement may also call the old writer
to close ANSI terminal state. `flush`, `set_width`, and `set_geometry` do not
invoke the writer.

```lua
local chunks = {}
local stream = mdf.document_stream({ format = "ansi", boring = true }, function(chunk)
  chunks[#chunks + 1] = chunk
end)

assert(stream:write("Hello "))
assert(stream:flush())             -- not EOF
assert(stream:set_geometry(80, 2, 0)) -- active document; future lines use new margin
assert(stream:write("world"))
assert(stream:finish_document())   -- EOF exactly once
assert(stream:begin_document())
stream:close()
```

`document_stream` also exposes `set_width(width)`,
`set_geometry(width, margin_left, margin_right)`, `reset()`,
`set_sink(callback)`, `set_html_title(title)`, and `error()`. Set an HTML title
before its first `write`; `error()` returns the latest core diagnostic. Geometry,
reset, and sink replacement have the same future-decision, caller-owned replay,
and terminal-closure behavior as their C receiver counterparts. Replacing the
callback resets the current document, so resend the source you want on the new
sink; Lua does not buffer it for you. A failed terminal cleanup callback does
not prevent replacement: the old state is discarded and the new callback is
bound for caller-owned replay. Rebinding the identical callback is a no-op and
preserves the current document. `close` is rejected while its sink or trace
callback is active. Sink and optional `write_trace` callbacks run
on the Lua state making each method call, so streams returned from collected
coroutines remain usable.

`set_width` and `set_geometry` also recompute retained inline code/link layout
that has not yet been passed to the callback; neither rewrites an emission
already completed. With `table_buffer_mode = "full"`, an unfinished ANSI table
uses the geometry in effect when it renders; `write` still accepts fragments
while it waits for a terminating line, EOF or the retention limit. With `"row"`,
later rows and the closing border use the new geometry without replaying
earlier rows. Use `reset()` and resend caller-owned source when the whole
document must reflow.
For the manual-token surface, `handle:write_token` accepts a table such as
`{ type = "text", text = "hello" }` or a numeric `mdf.token.*` type,
with optional `level`; finish that stream with `handle:finish()`.

## Pager

Interactive file paging is available as `mdf.pager(path, opts)`. It uses the
same terminal controls and navigation as `cmdf --pager`. The default is
extension-based (`.md` is Markdown; all other files are text); pass
`{ format = "markdown" }` or an HTTP media type such as
`{ format = "text/markdown" }` (assumed UTF-8) or
`{ format = "text/markdown; charset=utf-8" }` to render a non-`.md` file as Markdown, or
`{ format = "text" }` to suppress Markdown rendering for a `.md` file.
Supply `should_cancel = function() ... end` to request cooperative cancellation.
`cache_budget` selects the same rendered-content allowance as the C API
(zero/default is 1 MiB; the minimum nonzero allowance is 65536 bytes).
Cancellation returns `false, "cancelled"`; callback exceptions are rethrown
unchanged after terminal cleanup. Ordinary quit returns `true`.

## HTML titles

HTML and deck handles can set or clear the document/deck title before rendering:

```lua
local h = mdf.new({ format = "deck" })
h:set_html_title("Deck Title")
io.write(h:render("# hello\n"))
h:close()
```

## Options

Render options follow the annotated `libmdf.Options` record in
[`lua/libmdf.lua`](https://github.com/c89systems/libmdf/blob/trunk/lua/libmdf.lua).
The principal aliases are `theme` for C's `theme_name`, `c89` for `html_c89`,
and `font_*` / `dump_font*` for the HTML asset options. See the
[HTML guide](html.md) for font references and local dump destinations.

Lua follows the CLI behavior: `font_uri` selects external output, local paths
and `file://` URIs become dump destinations when `dump_font` is true, and
`font_path` selects a separate local dump destination without changing the URI.
Unlike `cmdf`, Lua and C API dump paths do not imply `dump_font`.

## Asset and path helpers

`mdf.html_font_dump_paths(opts)` returns the paired local destinations selected
by enabled `dump_font` options without writing files. `mdf.paths_alias(a, b)`
reports whether two non-empty paths name the same existing object or the same
absent destination; `mdf.path_aliases_stdout(path)` reports whether a path
names the process's current stdout destination. These helpers let CLI wrappers
reject destructive dump aliases before creating a renderer.
`mdf.path_relative_to(from_dir, target_path)` returns a URI-escaped relative
path for a local target, resolving relative inputs against the current working
directory without requiring files to exist or resolving symlinks.
`mdf.file_descriptor(file)` returns an open Lua file's borrowed POSIX descriptor
(or -1 when unavailable). It neither writes nor closes the file and rejects
closed files and non-file values. Keep the file open through renderer creation
for reliable detection; the renderer does not retain the Lua file object.

The generated `cmdf.lua` shipped in CLI archives uses the streaming Lua API and
the libmdf-built-in JetBrains Mono faces for HTML and deck parity with `cmdf`.

## Queries and constants

The Lua facade also exposes `mdf.version`, `mdf.version_major`,
`mdf.version_minor`, `mdf.version_patch`, `mdf.status`, `mdf.status_string`,
and `mdf.token` constants corresponding to the public C values that are useful
from Lua. `mdf.theme_names()`, `mdf.theme_exists(name)`,
`mdf.detect_osc8_support()`, `mdf.terminal_width(fd, fallback)`, and
`mdf.status_string(status)` expose their corresponding read-only library
queries. Handle objects expose `set_sink`, `set_width`, `set_geometry`, `reset`,
`set_html_title`, `render`, `render_stream`, `write_token`, `finish`, `error`,
`feed`, `flush`, `finish_document`, `begin_document`, `set_word_buffer`,
`write_literal`, `escape_markdown`, and `close`. `render`
returns a string and needs no sink; the other output receiver methods use the
callback previously supplied to `set_sink`. After `finish_document`, call
`begin_document` before feeding another incremental document. Bound sink and
optional `write_trace` callbacks are refreshed to the Lua state making each
call, so handles returned from collected coroutines remain valid.

C allocator hooks, reusable workspace limits, caller-supplied emission memory,
and raw C source/sink structs are C embedding surfaces rather than Lua options.
Lua readers, writers, and traces map to their public C callbacks; native memory
is managed by the binding. Private CLI helpers such as output truncation are
not facade APIs.
