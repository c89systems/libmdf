# cmdf

`cmdf` detects its actual output destination. Terminal output remains themed;
pipes and files default to escape-free UTF-8 wrapped at 80 columns. `--width`
overrides wrapping, `--ascii` (or `--ansi off`) forces no escapes, and `--ansi on`
explicitly enables terminal sequences for a pipe/file. `--osc8 on` cannot
override OFF. `--boring` remains independent. These policies affect only the
ANSI renderer. The pager retains interactive terminal behavior and does not
inherit the non-terminal default from its internal callback sinks.

`cmdf` renders Markdown from a file or stdin and writes ANSI or HTML to stdout
or `--output`. With `--pager`, it instead opens a named regular file in an
interactive terminal pager: `.md` files render as Markdown and every other
file is shown as ordinary text.

```sh
cmdf README.md
cmdf --html README.md -o README.html
cmdf README.md -o README.txt
cmdf -b -w 80 --margin-left 2 --margin-right 2 README.md
cmdf --pager README.md
cmdf --pager --incremental README.md
cmdf --pager --pager-format text application.log
cmdf --pager --pager-format text README.md
cmdf --html --incremental --title "Document title" README.md
cmdf -p /var/log/messages
```

Run `cmdf --help` for the complete option reference. Detailed guides cover
[paging](pager.md), [HTML, fonts and branding](html.md),
[slide decks](html-slide-deck-mode.md), [charts](charts.md), and
[streaming and terminal links](streaming.md).

`--simulate` reads input in small chunks as fast as the renderer can consume
them. `--simulate-delay` is an opt-in demo/probe option for making output timing
visible; it accepts Go-style durations such as `20ms`, `1s`, `500us`, and
compound values. `--trace-writes` is ANSI-only and writes NDJSON records
containing sequence number, format, byte length, and base64 data.
`--incremental` is an ANSI/HTML driver that feeds each bounded
source read through `feed` and non-EOF `flush`, then calls `finish_document` at
EOF. Incremental HTML requires `--title`: automatic title detection is a
one-shot source operation and cannot precede the first streamed emission.
Deck output uses synchronous source rendering instead. Both drivers exercise
the incremental lifecycle with synchronous input and output callbacks.

The [pager guide](pager.md) describes the live incremental receiver used by
`--pager`, including cache budgets, search, cancellation and resize behavior.
