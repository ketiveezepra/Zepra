// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_font_matching_tests.cpp
 * @brief Picking fonts for CSS: FontManager::match() over the installed
 *        fonts (family lists, generic families, weight and style), and the
 *        layout engine handing each text box's font to the text callbacks.
 *
 * The matching tests use the DejaVu family, which Linux CI images ship; they
 * are skipped where it isn't installed.
 */

#include <gtest/gtest.h>

#include "browser/layout_engine.h"
#include "rendering/font_manager.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using Zepra::Fonts::FontManager;
using Zepra::Fonts::FontWeight;
using Zepra::Fonts::RegisteredFont;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool haveDejaVu() {
    const RegisteredFont* f = FontManager::instance().match("DejaVu Sans", 400, false);
    return f && f->family == "dejavu sans";
}

}  // namespace

TEST(FontMatching, FirstInstalledFamilyOfTheList) {
    if (!haveDejaVu()) GTEST_SKIP() << "DejaVu fonts are not installed";
    auto& fm = FontManager::instance();
    const RegisteredFont* f = fm.match("\"No Such Font\", 'Another Missing', DejaVu Serif, DejaVu Sans", 400, false);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->family, "dejavu serif");
    EXPECT_EQ(fm.match("No Such Font", 400, false), nullptr);
    EXPECT_EQ(fm.match("", 400, false), nullptr);
    // Family names ignore case and quotes
    f = fm.match("  \"dejavu SANS mono\" ", 400, false);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->family, "dejavu sans mono");
}

TEST(FontMatching, GenericFamiliesResolveToInstalledFonts) {
    if (!haveDejaVu()) GTEST_SKIP() << "DejaVu fonts are not installed";
    auto& fm = FontManager::instance();
    for (const char* generic : {"serif", "sans-serif", "monospace"}) {
        const RegisteredFont* f = fm.match(generic, 400, false);
        ASSERT_NE(f, nullptr) << generic;
    }
    EXPECT_NE(lower(fm.match("monospace", 400, false)->family).find("mono"), std::string::npos);
    // A missing family falls through to the generic one after it
    const RegisteredFont* f = fm.match("-apple-system, BlinkMacSystemFont, \"Segoe UI\", monospace", 400, false);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->family, fm.match("monospace", 400, false)->family);
}

TEST(FontMatching, WeightAndStylePickTheFace) {
    if (!haveDejaVu()) GTEST_SKIP() << "DejaVu fonts are not installed";
    auto& fm = FontManager::instance();
    const RegisteredFont* regular = fm.match("DejaVu Sans Mono", 400, false);
    const RegisteredFont* bold = fm.match("DejaVu Sans Mono", 700, false);
    const RegisteredFont* semibold = fm.match("DejaVu Sans Mono", 600, false);
    const RegisteredFont* italic = fm.match("DejaVu Sans Mono", 400, true);
    const RegisteredFont* boldItalic = fm.match("DejaVu Sans Mono", 800, true);
    ASSERT_TRUE(regular && bold && semibold && italic && boldItalic);
    EXPECT_EQ(regular->weight, FontWeight::Normal);
    EXPECT_FALSE(regular->italic);
    EXPECT_EQ(bold->weight, FontWeight::Bold);
    EXPECT_FALSE(bold->italic);
    EXPECT_EQ(semibold->weight, FontWeight::Bold);   // 600 and above use the bold face
    EXPECT_TRUE(italic->italic);
    EXPECT_EQ(italic->weight, FontWeight::Normal);
    EXPECT_TRUE(boldItalic->italic);
    EXPECT_EQ(boldItalic->weight, FontWeight::Bold);
}

TEST(FontMatching, MissingStyleFallsBackToTheClosestFace) {
    if (!haveDejaVu()) GTEST_SKIP() << "DejaVu fonts are not installed";
    // A family without an italic face still matches (the renderer slants it)
    const RegisteredFont* f = FontManager::instance().match("DejaVu Sans", 400, true);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->family, "dejavu sans");
}

// ---------------------------------------------------------------------------
// The layout engine passes each box's font to the text callbacks
// ---------------------------------------------------------------------------

namespace {
std::vector<std::string> g_measured;   // "family|weight|italic" per measurement
}

TEST(FontMatching, LayoutMeasuresTextWithTheBoxFont) {
    g_measured.clear();
    ZepraBrowser::setLayoutCallbacks(
        [](float, float, float, float, uint32_t) {}, [](float, float, float, float, uint32_t, float) {},
        [](const std::string&, float, float, uint32_t, float, const ZepraBrowser::LayoutBox*) {},
        [](const std::string& text, float size, const ZepraBrowser::LayoutBox* font) {
            g_measured.push_back(font ? font->fontFamily + "|" + std::to_string(font->fontWeight) + "|" +
                                            (font->italic ? "i" : "n")
                                      : "none");
            return static_cast<float>(text.size()) * size * 0.5f;
        },
        nullptr, nullptr);
    ZepraBrowser::LayoutBox box;
    box.text = "hello";
    box.fontFamily = "monospace";
    box.fontWeight = 700;
    box.italic = true;
    EXPECT_FLOAT_EQ(ZepraBrowser::measureTextWidth(box.text, 10, &box), 25.0f);
    EXPECT_FLOAT_EQ(ZepraBrowser::measureTextWidth("abc", 10), 15.0f);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(g_measured, (std::vector<std::string>{"monospace|700|i", "none"}));
}
