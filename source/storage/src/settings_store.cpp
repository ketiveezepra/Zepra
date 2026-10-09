// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file settings_store.cpp  
 * @brief Settings implementation
 */

#include "storage/settings_store.h"
#include <iostream>
#include <fstream>

namespace ZepraBrowser {

SettingsStore& SettingsStore::instance() {
    static SettingsStore instance;
    return instance;
}

SettingsStore::SettingsStore() {
    std::cout << "[SettingsStore] Initialized with defaults" << std::endl;
}

SettingsStore::~SettingsStore() = default;

Settings SettingsStore::getSettings() const {
    return settings_;
}

void SettingsStore::setSettings(const Settings& settings) {
    settings_ = settings;
}

SearchEngine SettingsStore::getSearchEngine() const {
    return settings_.defaultSearchEngine;
}

void SettingsStore::setSearchEngine(SearchEngine engine) {
    settings_.defaultSearchEngine = engine;
    std::cout << "[SettingsStore] Search engine changed: " << (int)engine << std::endl;
}

bool SettingsStore::isDarkMode() const {
    return settings_.darkMode;
}

void SettingsStore::setDarkMode(bool enabled) {
    settings_.darkMode = enabled;
    std::cout << "[SettingsStore] Dark mode: " << (enabled ? "ON" : "OFF") << std::endl;
}

bool SettingsStore::load(const std::string& filepath) {
    filepath_ = filepath;
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cout << "[SettingsStore] No existing settings file, using defaults: " << filepath << std::endl;
        return false;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        
        if (key == "searchEngine") settings_.defaultSearchEngine = static_cast<SearchEngine>(std::stoi(val));
        else if (key == "trackHistory") settings_.trackHistory = (val == "1");
        else if (key == "acceptCookies") settings_.acceptCookies = (val == "1");
        else if (key == "doNotTrack") settings_.doNotTrack = (val == "1");
        else if (key == "darkMode") settings_.darkMode = (val == "1");
        else if (key == "fontSize") settings_.fontSize = std::stoi(val);
        else if (key == "fontFamily") settings_.fontFamily = val;
        else if (key == "downloadPath") settings_.downloadPath = val;
        else if (key == "askWhereToSave") settings_.askWhereToSave = (val == "1");
        else if (key == "enableJavaScript") settings_.enableJavaScript = (val == "1");
        else if (key == "enableImages") settings_.enableImages = (val == "1");
        else if (key == "enablePlugins") settings_.enablePlugins = (val == "1");
    }
    
    std::cout << "[SettingsStore] Loaded settings from: " << filepath << std::endl;
    return true;
}

bool SettingsStore::save(const std::string& filepath) {
    filepath_ = filepath;
    std::ofstream file(filepath, std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "[SettingsStore] Failed to save: " << filepath << std::endl;
        return false;
    }
    
    file << "searchEngine=" << static_cast<int>(settings_.defaultSearchEngine) << "\n";
    file << "trackHistory=" << settings_.trackHistory << "\n";
    file << "acceptCookies=" << settings_.acceptCookies << "\n";
    file << "doNotTrack=" << settings_.doNotTrack << "\n";
    file << "darkMode=" << settings_.darkMode << "\n";
    file << "fontSize=" << settings_.fontSize << "\n";
    file << "fontFamily=" << settings_.fontFamily << "\n";
    file << "downloadPath=" << settings_.downloadPath << "\n";
    file << "askWhereToSave=" << settings_.askWhereToSave << "\n";
    file << "enableJavaScript=" << settings_.enableJavaScript << "\n";
    file << "enableImages=" << settings_.enableImages << "\n";
    file << "enablePlugins=" << settings_.enablePlugins << "\n";
    
    std::cout << "[SettingsStore] Saved settings to: " << filepath << std::endl;
    return true;
}

} // namespace ZepraBrowser
