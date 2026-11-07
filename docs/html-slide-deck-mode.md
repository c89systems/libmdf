# HTML slide decks

`cmdf --deck` renders Markdown as a standalone browser presentation. Decks use
the HTML renderer's themes, fonts, charts and title detection. This is a libmdf
extension with native tests rather than a Go parity requirement.

## Generate a deck

```sh
cmdf --deck -o slides.html presentation.md
cmdf --deck --transition cross --slide-numbers -o slides.html presentation.md
cmdf --deck --deck-center-front-text -t nord -o slides.html presentation.md
```

`--deck` selects deck output even when the output name ends in `.html` or
`--html` is also supplied. Ordinary `--html` output remains a document.
`--c89` applies only to ordinary HTML documents and is rejected for decks.

Slides are separated by block-level Markdown thematic breaks, such as `---`.
Break-like text inside code, charts or other non-break contexts is content.
Recognized leading front matter is not a slide. Its `theme:` key supplies a
default only when neither the API nor CLI explicitly selects a theme.

````markdown
---
theme: nord
---
# Presentation title

An introduction.

---
# Results

```mdf-bar-chart
Stage,Count
Build,40
Test,25
Ship,10
```
````

Deck-specific options require `--deck`:

| Option | Behavior |
| --- | --- |
| `--transition`, `-x` | `fade` through black (default), `cross`, or `hard` |
| `--slide-numbers` | Number slides after the first; disabled by default |
| `--deck-center-front-text` | Center-align first-slide paragraphs |

Theme, title, content-width and HTML font options also apply. Embedded fonts,
CSS and JavaScript make the default output self-contained; external font
configuration requires serving the selected font files separately.

## C and Lua APIs

Select `MDF_FORMAT_HTML_DECK` at creation. Set `deck_transition`,
`slide_numbers`, and `deck_center_front_text` in `mdf_options` as needed:

```c
mdf_options opts;
mdf *deck;

mdf_options_init(&opts);
opts.deck_transition = MDF_DECK_TRANSITION_CROSS;
opts.slide_numbers = 1;
if (mdf_create(MDF_FORMAT_HTML_DECK, &opts, &deck) == MDF_OK) {
    /* Bind a sink and call deck->render, or use deck->render_cstr. */
    deck->destroy(deck);
}
```

An explicit `theme_name` overrides front matter. Set an explicit document title
with `mdf_set_html_title` before rendering; otherwise the renderer probes the
initial ATX heading. Font configuration uses the same options and helpers as
ordinary HTML. See the [public API guide](c-api.md).

Lua uses the same controls:

```lua
local mdf = require("libmdf")
local deck = mdf.new({ format = "deck", deck_transition = "cross" })
deck:set_html_title("Presentation")
io.write(deck:render("# Hello\n\n---\n\n# Results\n"))
deck:close()
```

## Input and storage

Use the synchronous source-to-sink `render` API or materialized `render_cstr`.
Decks reject manual tokens and incremental `feed`/`flush`/`finish_document`;
`cmdf --incremental --deck` is also rejected.

The renderer buffers one slide's Markdown and rendered body at a time. It then
writes the slide, frees the rendered body and reuses the Markdown workspace.
It does not collect the complete Markdown source. Slide-number placeholders
are completed by the browser from the final slide count, without a source prepass.

## Viewing and navigation

Open the generated HTML in a browser, including through a local file URL.
The first slide centers its content block; later slides keep a leading title
above a vertically centered body. Body text fits the viewport while heading
sizes stay stable. Dense content falls back to internal scrolling after the
minimum fitting size is reached.

| Keys | Action |
| --- | --- |
| Right, Down, Space, Page Down, `j` | Next slide |
| Left, Up, Page Up, `k` | Previous slide |
| Home, `g` | First slide |
| End, `G` | Last slide |
| `o` | Previous deck position |
| `f` | Toggle fullscreen |

Touch swipes navigate in the corresponding direction. Vertical navigation
respects the scroll edges of dense slides. A fullscreen control uses the
browser Fullscreen API when available, with a viewport-filling fallback.
The URL hash preserves the current slide. Reduced-motion preferences disable
animated transitions. Links open in a new tab with opener protection; only
the current slide participates in normal keyboard focus and accessibility.

## Verification

`make test` covers deck API/CLI behavior, slide boundaries, fonts, themes,
charts and emitted browser controls. Browser interaction is not an automated
CTest gate. See the [development commands](development.md) for the
broader local checks.
