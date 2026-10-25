// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// zepra://start: the new-tab page.
//
// The page is ordinary HTML served from two directories, highest priority first:
//   1. the user's start directory (<user config dir>/start, see
//      Zepra::Platform::getUserConfigDirectory)
//   2. the built-in one, <resources>/start
// Every file the page asks for (index.html, style.css, theme.css, images) is
// looked up in that order, so a user file replaces the built-in file of the same
// name. If neither directory has index.html, getStartPageHTML() is used.
// resources/start/README.md documents this for theme and plugin authors.

#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "platform/platform_compat.h"

namespace ZepraBrowser {
namespace UI {

constexpr const char* kStartPageUrl = "zepra://start";

// Built-in copy of resources/start (same layout as style.css, no icons), used
// when no start directory has an index.html
inline std::string getStartPageHTML() {
    return R"(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>New Tab</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
  background: linear-gradient(to bottom, #F3B4FF, #8A54DE);
  font-family: sans-serif;
  color: #1B1B1F;
  min-height: 100vh;
  display: flex;
  flex-direction: column;
  justify-content: center;
}
.logo-row { display: flex; justify-content: center; margin-bottom: 40px; }
.search-pill {
  display: flex;
  justify-content: center;
  align-items: flex-start;
  background: #F7E6FA;
  border-radius: 32px;
  width: 920px;
  height: 64px;
  padding: 20px 28px;
  margin-left: auto;
  margin-right: auto;
}
.search-input {
  width: 600px;
  height: 28px;
  flex-shrink: 0;
  border: none;
  outline: none;
  background: #F7E6FA;
  font-size: 16px;
  font-weight: 700;
  color: #1B1B1F;
  text-align: center;
}
</style>
</head>
<body>
<div class="logo-row">
  <img class="logo" src="../icons/zepralogo.svg" alt="Zepra">
</div>
<div class="search-pill">
  <input class="search-input" type="text" placeholder="Let's give your dream life. What you create today?" autofocus>
</div>
</body>
</html>
)";
}

// Directories zepra://start files come from, highest priority first
inline std::vector<std::filesystem::path> startPageDirs(const std::string& resourceDir) {
    std::vector<std::filesystem::path> dirs;
    std::string userDir = Zepra::Platform::getUserConfigDirectory();
    if (!userDir.empty()) dirs.push_back(std::filesystem::path(userDir) / "start");
    std::error_code ec;
    dirs.push_back(std::filesystem::absolute(std::filesystem::path(resourceDir) / "start", ec));
    return dirs;
}

// Maps a URL path relative to zepra://start ("theme.css", "/img/a.svg",
// "../icons/zepra.svg") to the first matching file. When no directory has it,
// returns where the built-in file would be, so load errors name a real path.
inline std::string findStartPageFile(const std::string& resourceDir, const std::string& href) {
    std::string rel = href.substr(0, href.find_first_of("?#"));
    rel = Zepra::Platform::fileUrlToPath(rel);  // decodes %20 and friends
    while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
    if (rel.empty()) rel = "index.html";

    std::filesystem::path found;
    std::error_code ec;
    for (const auto& dir : startPageDirs(resourceDir)) {
        found = (dir / rel).lexically_normal();
        if (std::filesystem::is_regular_file(found, ec)) break;
    }
    return found.generic_string();
}

// The start page's HTML: index.html from the start directories, else the built-in copy
inline std::string loadStartPageHTML(const std::string& resourceDir, std::string* source = nullptr) {
    std::string path = findStartPageFile(resourceDir, "index.html");
    std::ifstream file(path, std::ios::binary);
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        if (source) *source = path;
        return buffer.str();
    }
    if (source) *source = "built-in";
    return getStartPageHTML();
}

} // namespace UI
} // namespace ZepraBrowser
