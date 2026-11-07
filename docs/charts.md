# Chart fences

`libmdf` supports chart fences as a libmdf-specific Markdown extension. This is
not CommonMark and is not currently a Go `mdf` parity feature. Use chart fences
when the document needs small inspectable numeric summaries that should survive
plain terminal output, copied text, and printable HTML without external image
assets.

A chart fence uses a `mdf-*chart` info string, optional comma-separated
options, and two-column CSV rows:

````md
```mdf-bar-chart,sort=desc,colored-bars=on
key,value
Build,40
Test,25
Ship,10
```
````

The first nonnumeric row is treated as a header. Values must be nonnegative
numbers. The first column is the display label; the second column is the raw
numeric value. Percentages are calculated from the sum of the values and are
not supplied in the input.

Chart output is theme-aware in both ANSI and HTML. Labels, values, percentages,
bars, and tile segments use colors from the active theme when styling is enabled.
Escape-free ANSI charts retain their Unicode shapes and calculated values.
ANSI chart rows are emitted through the same decision-emission surface as other
ANSI output, and HTML preserves the active theme colors by consuming the styled
ANSI emissions.

Supported chart types:

- `mdf-bar-chart`, `mdf-horizontal-bar-chart`
- `mdf-vertical-bar-chart`
- `mdf-tile-chart`

Use horizontal bar charts for ranked comparisons where labels and exact values
matter. Use vertical bar charts for compact small datasets where shape matters
more than long labels. Use tile charts for part-of-whole data: they render one
100% stacked bar, a second percentage row, and a raw-value legend. Pie charts
are intentionally not supported; tile charts are the recommended ASCII-friendly
replacement.

Supported options:

- `sort`, `sort=desc`, `sort=descending`, `sort=asc`, `sort=ascending`
- `colored-bars=on`, `colored-bars=off`, `colored-bars=yes`,
  `colored-bars=no`
- `disable-percentage=on`, `disable-percentage=off`,
  `disable-percent=true`, `disable_percent=false`

`colored_bars` is accepted as an underscore spelling. Colored bars are enabled
by default and cycle through active theme accents. `colored-bars=off` uses a
single theme accent for chart marks. `disable_percentage` and
`disable_percent` are accepted as underscore spellings. Horizontal bar chart
percentages are enabled by default and can be hidden with
`disable-percentage`; tile chart percentage rows remain part of the tile chart
presentation.

Horizontal charts fit bars, labels, values, and optional percentages to the
configured content width. In ANSI, `width` and margins define the available
chart width. In HTML, `html_content_width_ch` defines the rendered document
width; the chart is centered as a chart box within that document rather than
padded with terminal spaces. `cmdf -w` sets ANSI width and also sets HTML
content width unless `--html-content-width` is provided.

Vertical charts expand small datasets toward the content width, reduce plotted
point resolution when the x axis would overflow, and center complete x-axis
labels when they fit their bar slots; dense labels fall back to compact ticks or
are suppressed rather than overlapping. Tile charts render a single 100% stacked
bar with each value as a
colored segment, write centered labels on the first bar row, centered
percentages on the second bar row, and follow with a raw-value legend. Chart
labels, values, and percentages use the same theme color as their bar or tile
segment.
