# Deck Corpus

This directory contains source fixtures for libmdf HTML slide deck mode.

`comprehensive.md` is both an example presentation and a UX test deck. It is
intended to exercise front matter, slide splitting, first-slide layout, dense
autofit cases, headings, inline Markdown, lists, blockquotes, code, tables,
chart fences, links, entities, and thematic-break edge cases.

Use this fixture for manual browser review and generated HTML smoke tests.
Generated deck HTML should include the libmdf provenance comment after the
doctype and open slide links in a new tab/window. Keep previews under the
checkout's generated `build/` directory.

Expected local preview command:

```sh
mkdir -p build
cmdf --deck --slide-numbers -x fade -o build/deck.html testdata/deck-corpus/comprehensive.md
```

`make clean` removes this preview along with the other generated outputs.
