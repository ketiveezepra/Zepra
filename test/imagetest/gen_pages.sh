#!/bin/sh
# Regenerates svg_inline.html and svg_img.html from svg/*.svg (run from test/imagetest).
head='<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>TITLE</title>
<style>
body { background: #fff; color: #000; font-family: sans-serif; font-size: 12px; margin: 8px; }
.row { display: flex; margin-bottom: 8px; }
.cell { width: 130px; height: 140px; margin-right: 8px; }
.cell p { margin: 2px 0 0 0; }
</style></head><body>'
emit() { # $1 = title, $2 = mode
  echo "$head" | sed "s/TITLE/$1/"
  i=0
  echo '<div class="row">'
  for f in svg/*.svg; do
    name=$(basename "$f" .svg)
    if [ $i -gt 0 ] && [ $((i % 6)) -eq 0 ]; then echo '</div><div class="row">'; fi
    echo "<div class=\"cell\">"
    if [ "$2" = inline ]; then grep -v '^<?xml' "$f"; else echo "<img src=\"$f\">"; fi
    echo "<p>$name</p></div>"
    i=$((i + 1))
  done
  if [ "$2" = inline ]; then
    echo '<div class="cell" style="color:#d22"><svg width="120" height="120" viewBox="0 0 120 120"><circle cx="60" cy="60" r="40" fill="currentColor"/><rect x="10" y="10" width="100" height="100" fill="none" stroke="currentColor" stroke-width="3"/></svg><p>css color</p></div>'
  fi
  echo '</div></body></html>'
}
emit "SVG inline" inline > svg_inline.html
emit "SVG img" img > svg_img.html
