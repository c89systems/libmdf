# C API

Include either header:

```c
#include <libmdf.h>
/* or */
#include <libmdf/mdf.h>
```

The public handle is `mdf`. Construct it with `mdf_create`, then use receiver
methods on the handle.

```c
#include <libmdf/mdf.h>

#include <stdio.h>

int main(void)
{
    mdf_options opts;
    mdf *renderer;
    char *out;

    mdf_options_init(&opts);
    opts.boring = 1;

    if (mdf_create(MDF_FORMAT_ANSI, &opts, &renderer) != MDF_OK) {
        return 1;
    }
    if (renderer->render_cstr(renderer, "# hello\n\nworld\n", &out) != MDF_OK) {
        renderer->destroy(renderer);
        return 1;
    }

    fputs(out, stdout);
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    return 0;
}
```

For producer-to-consumer rendering, use `renderer->render` with `mdf_source`
and `mdf_sink` callbacks. The source callback returns bytes read, returns `0`
for EOF, and sets `*err` on read failure. The sink callback returns `0` on
success and nonzero on write failure.

## Options and ownership

Initialize every `mdf_options` with `mdf_options_init`; a zeroed record is not
equivalent because `output_fd` defaults to -1. Pass NULL to `mdf_create` for
library defaults. The installed `<libmdf/mdf.h>` is the field-by-field API
reference, including defaults, ownership, error returns and receiver methods.
Its [source template](https://github.com/c89systems/libmdf/blob/trunk/include/libmdf/mdf.h.in)
contains the same hover documentation as the installed header.

| Task | Interface / guide |
| --- | --- |
| Stream a source | `set_sink` + `render`, or explicit-sink `mdf_render` |
| Feed Markdown fragments | [Incremental lifecycle](streaming.md) |
| Render literal fragments | `write_literal` and structural tokens |
| Set width and margins | `set_geometry`; [reflow rules](streaming.md) |
| Configure colors | `theme_name`, `ansi_mode`, `boring`, `osc8`; palette below |
| Generate browser output | [HTML](html.md), [decks](html-slide-deck-mode.md) |
| Page a file or seekable source | [Pager embedding](pager.md) |
| Control decision storage | [Word, table and emission limits](streaming.md) |

Returned strings use the renderer's allocator: release them with its
`string_free` method before destroying the handle. Callback byte pointers are
borrowed for that callback only. A bound sink's callback and context must remain
valid until replacement or destruction. Explicit-sink free functions borrow
the sink only for that call. Do not concurrently operate on one receiver.

## Literal text and escaping

`renderer->write_literal(renderer, data, len)` bypasses Markdown syntax while
using the manual-token renderer's word boundaries, wrapping, margins, quotes
and theme. It also accepts `MDF_TOKEN_LITERAL_TEXT` through `write_token`.
Bind a sink first; use ordinary structural tokens to open and close quotes or
headings, then `finish` for EOF. Successive fragments may form one word:

```c
renderer->write_literal(renderer, "**bo", 4);  /* no word decision yet */
renderer->write_literal(renderer, "ld** ", 5); /* emits literal **bold** */
renderer->finish(renderer);
```

Literal fragments cannot interrupt unresolved inline Markdown syntax. They
leave punctuation, backslashes, entities and link syntax visible. Newlines use
the normal NEWLINE-token layout. The free function `mdf_write_literal` takes an
explicit sink. Both ANSI and HTML output support this API.

To produce Markdown for a later render instead, use
`renderer->escape_markdown(renderer, text, &escaped)`, then release the result
with `renderer->string_free`. It backslash-escapes ASCII punctuation, including
existing backslashes: `**bold**` becomes `\*\*bold\*\*`. UTF-8 and whitespace
are copied unchanged; Markdown indentation, line-break and paragraph layout
rules still apply. This helper allocates its complete result and does not
render, flush or change pending input.

Lua exposes `mdf.escape_markdown(text)`, `handle:escape_markdown(text)`, and
`handle:write_literal(fragment)` with the same behavior.

## Themes and colors

Available themes can be queried with `mdf_theme_count`, `mdf_theme_name`, and
`mdf_theme_exists`. Theme lookup is case-insensitive. The built-in set includes
`default`, `ayu-light`, `ayu-mirage`, `catppuccin-mocha`, `dracula`,
`everforest`, `github-dark`, `github-light`, `gruvbox`, `kanagawa`, `nord`,
`rose-pine`, `solarized-dark`, `solarized-light`, and `tokyo-night`; use
`cmdf --list-themes` for the exact list in a build.

`mdf_theme_get_palette(name, &palette)` copies all 19 semantic foregrounds into
a public `mdf_theme_palette`: H1 through H6 (`heading[0]` through `heading[5]`),
emphasis, strong, combined emphasis/strong, inline/block code, quote marker/text,
list marker, link label/URL, thematic break, and table header/border. Each field
is a caller-owned, NUL-terminated foreground-only ANSI SGR string; an empty string
means that slot has no foreground. `palette.name` is the canonical name in borrowed
immutable library storage. The lookup allocates nothing and does not inspect the
environment or terminal. Consumers decide when to enable color and add attributes.

```c
mdf_theme_palette palette;
if (renderer->get_theme_palette(renderer, &palette) == MDF_OK) {
    printf("%sCommand%s\n", palette.heading[1], "\033[39m");
}
```

The receiver method (also `mdf_get_theme_palette(renderer, &palette)`) returns the
configured theme even with ANSI disabled or boring mode enabled. C89 HTML's default
is `one-dark`; deck front matter does not change the configured theme. Use
`mdf_theme_heading_foreground(name, level, out, out_size)` for a single H1..H6
foreground. Name lookup accepts NULL/empty for `default` and is case-insensitive.
Invalid names, heading levels, NULL outputs and insufficient buffers return
`MDF_ERROR_INVALID` without changing the output. Palette arrays can be retained
after renderer destruction; the borrowed name remains valid until library unload.
