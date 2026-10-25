# The start page (`zepra://start`)

Every new tab opens `zepra://start`. It is an ordinary HTML page that goes through
the same parse → style → layout path as any web page; only the tab strip,
address bar and sidebar around it are native.

## Where its files come from

Each file the page asks for (`index.html`, `style.css`, `theme.css`, images) is
looked up in two directories, and the first one that has it wins:

1. the user's start directory
   - Windows: `%APPDATA%\Zepra\start\`
   - Linux: `$XDG_CONFIG_HOME/zepra/start/`, or `~/.config/zepra/start/`
   - macOS: `~/Library/Application Support/Zepra/start/`
2. this directory, `resources/start/`

Relative URLs in the page resolve against these directories, so `img/logo.svg`
is the user's copy if there is one and this one otherwise, and
`../icons/zepra.svg` reaches the browser's `resources/icons/`. If neither
directory has `index.html`, the browser uses the copy built into
`UI/browser/start_page.h`.

Press F5 on the start page to reload it after editing a file.

## Theming

Put a `theme.css` in your start directory. The built-in `index.html` links
`style.css` and then `theme.css`, so your rules override the built-in ones.
`theme.css` in this directory is empty and only documents this.

Colors are plain values in `style.css`, not CSS custom properties: the style
engine does not resolve `var()` yet (it uses a `var()` fallback value and
ignores the property), so a theme overrides selectors directly:

```css
body { background: linear-gradient(to bottom, #1E1B2E, #0B0A12); color: #EDEAF5; }
.search-pill { background: #2A2640; }
.search-input { background: #2A2640; color: #EDEAF5; }
```

Images can be themed the same way: a user `img/send.svg` replaces the built-in one.

## Replacing the page

Put a full `index.html` in your start directory. It can still link the built-in
`style.css` and `theme.css` and use the built-in images by relative URL.

## What the renderer handles today

Write start pages, themes included, for what the renderer lays out correctly now:

- Gradients: `linear-gradient` with two color stops, drawn top to bottom. The angle
  is ignored, and three or more stops come out as vertical strips.
- Column flex `body` with `justify-content: center` centres vertically.
  `align-items: center` does not centre horizontally: use `margin: 0 auto` on a
  fixed-width box, or a flex row with `justify-content: center`.
- In a flex row, `align-items: center` does not centre items vertically: use padding.
- `<img>` only paints inside a flex container. It ignores CSS `width`/`height`
  and margins, and an SVG is drawn at its `viewBox` size (capped at 256 px), so
  make the SVG the size it should appear (see `img/logo.svg`).
- Inline `<svg>` elements are not drawn.
- `::placeholder` styles, `text-align` inside an `<input>`, and SVG `transform`
  on `<g>` are not applied.
- Scripts run in a per-page VM, but `Promise.then` callbacks never run (see TODO.md).

## Plugins (not built yet)

The start directory is the extension point plugins are meant to build on: a
plugin would ship files there, either a `theme.css` or extra files that a
user `index.html` includes. There is no plugin loader, manifest or API yet. Until
there is, anything in the start directory is a local file the user put there.
The page loads with the same privileges as a local `file://` page, so only
install start pages you trust.
