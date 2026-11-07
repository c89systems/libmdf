# libmdf

C89/POSIX Markdown rendering for ANSI terminals and HTML, with a receiver-style
C API, the `cmdf` command, and Lua 5.5 bindings. The renderer supports headings,
emphasis, links, lists, quotes, code, tables, and chart fences. HTML output can
also be a standalone slide deck or a C89 Systems branded page.

ANSI rendering streams decided output to a synchronous sink. The interactive
pager opens Markdown and log files with a bounded cache, search and resize
support. The project is a C port of [Go mdf](https://github.com/sa6mwa/mdf),
with native tests and parity checks.

## Use cmdf

Download an SDK, CLI or Lua package from
[GitHub Releases](https://github.com/c89systems/libmdf/releases).
The [installation guide](docs/installation.md) lists targets, archive contents,
CMake/pkg-config integration and local installation.

To build and install the static CLI from a checkout:

```sh
make build
PREFIX="$HOME/.local" make install
```

```sh
cmdf document.md
cmdf --html document.md -o document.html
cmdf --html --c89 document.md -o branded.html
cmdf --deck slides.md -o slides.html
cmdf -p document.md
cmdf -p --pager-format text application.log
```

Terminal output uses the selected theme; pipes and files default to escape-free
UTF-8. Use `--ansi on` to enable terminal escapes explicitly, or `--ascii`
for escape-free output. Run `cmdf --help` for the full option reference.

## Embed in C

Include `<libmdf.h>` or `<libmdf/mdf.h>`. This example materializes a short
result; the [C API guide](docs/c-api.md) also shows source/sink streaming.

```c
#include <libmdf.h>
#include <stdio.h>

int main(void)
{
    mdf *renderer;
    char *out;

    if (mdf_create(MDF_FORMAT_ANSI, NULL, &renderer) != MDF_OK)
        return 1;
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

Initialize custom options with `mdf_options_init`. The installed public header
documents every option, callback and operation for clangd hover/completion.

## Use Lua

The Lua 5.5 package links against the installed libmdf SDK:

```lua
local mdf = require("libmdf")
io.write(mdf.render("# hello\n\nworld\n", { ansi_mode = "off" }))
```

The [Lua guide](docs/lua.md) covers handles, streaming readers/writers,
incremental documents, paging and callback errors.

## Documentation

| Guide | Contents |
| --- | --- |
| [Installation](docs/installation.md) | Release artifacts, supported targets, CMake, pkg-config, PREFIX/BINDIR |
| [C API](docs/c-api.md) | Receiver ownership, options, literal text, theme palettes |
| [Streaming](docs/streaming.md) | Document lifecycle, geometry, decision buffers, trace and link contracts |
| [cmdf](docs/cli.md) | Output selection, terminal policy, incremental driver and tracing |
| [Pager](docs/pager.md) | Text/Markdown viewing, caches, search, cancellation and C embedding |
| [HTML](docs/html.md) | Titles, embedded/external fonts, asset dumping, C89 branding and link safety |
| [Slide decks](docs/html-slide-deck-mode.md) | Slide boundaries, transitions and browser navigation |
| [Charts](docs/charts.md) | Chart fences, CSV data and layout options |
| [Lua](docs/lua.md) | Module and handle APIs, options and ownership |
| [Development](docs/development.md) | Toolchains, clangd, verification, goldens and benchmarks |
| [Release](docs/release.md) | Preparation, signing, packaging and final local gates |

The same guides are included beside this README in SDK, CLI and Lua source
archives.

## Development

```sh
make tools-native
make build-debug
make test
```

Build once before using clangd so generated headers and the compilation database
exist. See the development guide for hardening and parity checks, and the release
guide for the signed, tagged release workflow.

## License

libmdf, cmdf and the Lua bindings use the [MIT license](LICENSE). Bundled JetBrains Mono is
licensed under the SIL Open Font License; SDK and CLI archives include its
`OFL.txt` attribution.
