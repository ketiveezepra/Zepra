// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * ua_stylesheet.cpp - User-Agent Stylesheet for ZepraBrowser
 *
 * Defines browser default styles for all HTML elements.
 * Follows the HTML Standard's Rendering section and CSS 2.1 Appendix D.
 * Font-relative margins (em) resolve against each element's font size.
 *
 * TODO(NeolyxOS): Load UA stylesheet from disk when running on NeolyxOS
 * to allow system-level theming. Replace the embedded string with a
 * file read from /sys/theme/zepra.css or equivalent NeolyxOS path.
 */

#include "ua_stylesheet.h"

namespace Zepra::WebCore {

const char* ZepraUAStylesheet::getStylesheet() {
    // TODO(NeolyxOS): Replace with disk-load from /sys/theme/zepra.css
    static const char* UA_STYLESHEET = R"css(

/* ==========================================================================
   Root & Document
   ========================================================================== */

html {
    display: block;
    color: #1f2328;
}

/* ==========================================================================
   Hidden Elements — display: none for non-rendering elements
   ========================================================================== */

head, link, meta, script, style, title,
area, base, basefont, datalist, noembed,
noframes, param, rp, template {
    display: none;
}

[hidden] {
    display: none !important;
}

/* ==========================================================================
   Block-Level Elements
   ========================================================================== */

body {
    display: block;
    margin: 8px;
    font-family: sans-serif;
    font-size: 16px;
}

address, article, aside, div, footer,
header, hgroup, main, nav, search, section {
    display: block;
}

address {
    font-style: italic;
}

center {
    display: block;
    text-align: -webkit-center;
}

p {
    display: block;
    margin-top: 1em;
    margin-bottom: 1em;
}

blockquote {
    display: block;
    margin-top: 1em;
    margin-bottom: 1em;
    margin-left: 40px;
    margin-right: 40px;
}

figure {
    display: block;
    margin-top: 1em;
    margin-bottom: 1em;
    margin-left: 40px;
    margin-right: 40px;
}

figcaption {
    display: block;
}

hr {
    display: block;
    overflow: hidden;
    margin-top: 8px;
    margin-bottom: 8px;
    border-top: 1px solid #ccc;
}

pre {
    display: block;
    font-family: monospace;
    white-space: pre;
    margin-top: 1em;
    margin-bottom: 1em;
}

/* ==========================================================================
   Headings
   ========================================================================== */

h1 {
    display: block;
    font-size: 2em;
    font-weight: bold;
    margin-top: 0.67em;
    margin-bottom: 0.67em;
}

h2 {
    display: block;
    font-size: 1.5em;
    font-weight: bold;
    margin-top: 0.83em;
    margin-bottom: 0.83em;
}

h3 {
    display: block;
    font-size: 1.17em;
    font-weight: bold;
    margin-top: 1em;
    margin-bottom: 1em;
}

h4 {
    display: block;
    font-weight: bold;
    margin-top: 1.33em;
    margin-bottom: 1.33em;
}

h5 {
    display: block;
    font-size: 0.83em;
    font-weight: bold;
    margin-top: 1.67em;
    margin-bottom: 1.67em;
}

h6 {
    display: block;
    font-size: 0.67em;
    font-weight: bold;
    margin-top: 2.33em;
    margin-bottom: 2.33em;
}

/* ==========================================================================
   Lists
   ========================================================================== */

ul, menu, dir {
    display: block;
    margin-top: 1em;
    margin-bottom: 1em;
    padding-left: 40px;
}

ol {
    display: block;
    margin-top: 1em;
    margin-bottom: 1em;
    padding-left: 40px;
}

li {
    display: list-item;
}

ul ul, ul ol, ul menu, ol ul, ol ol, ol menu,
menu ul, menu ol, menu menu {
    margin-top: 0;
    margin-bottom: 0;
}

dl {
    display: block;
    margin-top: 1em;
    margin-bottom: 1em;
}

dd {
    display: block;
    margin-left: 40px;
}

dt {
    display: block;
}

/* ==========================================================================
   Inline Elements
   ========================================================================== */

span, a, strong, b, em, i, small, big, code,
label, abbr, acronym, cite, dfn, kbd, mark, q,
s, strike, del, ins, samp, sub, sup, u, var, wbr, time,
font, tt, nobr, bdi, bdo, data, output, ruby, rt {
    display: inline;
}

a {
    color: #0066cc;
    text-decoration: underline;
}

strong, b {
    font-weight: bold;
}

em, i, cite, var, dfn {
    font-style: italic;
}

u, ins {
    text-decoration: underline;
}

s, strike, del {
    text-decoration: line-through;
}

small, sub, sup {
    font-size: 0.83em;
}

big {
    font-size: 1.2em;
}

mark {
    background-color: yellow;
    color: black;
}

nobr {
    white-space: nowrap;
}

code, kbd, samp, tt {
    font-family: monospace;
}

/* ==========================================================================
   Tables
   ========================================================================== */

table {
    display: table;
    box-sizing: border-box;
    border-collapse: separate;
    border-spacing: 2px;
}

caption {
    display: table-caption;
    text-align: center;
}

colgroup { display: table-column-group; }
col      { display: table-column; }

thead { display: table-header-group; vertical-align: middle; }
tbody { display: table-row-group; vertical-align: middle; }
tfoot { display: table-footer-group; vertical-align: middle; }
tr    { display: table-row; vertical-align: inherit; }

td, th {
    display: table-cell;
    vertical-align: inherit;
    padding: 1px;
}

th {
    font-weight: bold;
    text-align: center;
}

table, td, th {
    border-color: gray;
}

/* A form start tag inside a table leaves an empty form element there */
table > form, thead > form, tbody > form, tfoot > form, tr > form {
    display: none;
}

/* ==========================================================================
   Forms
   ========================================================================== */

form {
    display: block;
}

input, textarea, select, button {
    display: inline-block;
    font-size: 14px;
    color: #333;
}

input {
    border: 1px solid #ccc;
    padding: 4px 8px;
    background-color: #fff;
}

textarea {
    border: 1px solid #ccc;
    padding: 4px;
    background-color: #fff;
}

button {
    padding: 4px 12px;
    border: 1px solid #ccc;
    background-color: #f0f0f0;
    cursor: pointer;
}

fieldset {
    display: block;
    margin-left: 2px;
    margin-right: 2px;
    padding-top: 6px;
    padding-bottom: 10px;
    padding-left: 12px;
    padding-right: 12px;
    border: 2px solid #ccc;
}

legend {
    padding-left: 2px;
    padding-right: 2px;
}

/* ==========================================================================
   Media
   ========================================================================== */

img {
    display: inline;
}

video, audio {
    display: inline-block;
}

/* ==========================================================================
   Semantic/Interactive
   ========================================================================== */

details {
    display: block;
}

summary {
    display: block;
    cursor: pointer;
}

dialog {
    display: none;
    position: absolute;
    border: 1px solid #333;
    padding: 1em;
    background-color: #fff;
}

)css";
    return UA_STYLESHEET;
}

const char* ZepraUAStylesheet::getQuirksStylesheet() {
    // HTML Standard, Rendering: tables in quirks mode don't inherit the
    // surrounding text styles (so <center><table> keeps cells left-aligned)
    static const char* QUIRKS_STYLESHEET = R"css(
table {
    font-weight: initial;
    font-style: initial;
    font-size: initial;
    line-height: initial;
    white-space: initial;
    text-align: initial;
}
)css";
    return QUIRKS_STYLESHEET;
}

} // namespace Zepra::WebCore
