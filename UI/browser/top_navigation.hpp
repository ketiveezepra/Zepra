#pragma once

// Inline Top Navigation rendering logic
// Matches exactly to Figma design reference (Image 5)

inline void renderTopBar() {
    float barX = 0;
    float barWidth = g_width;
    float topBarHeight = TOPBAR_HEIGHT; 
    
    // Background gradient similar to the ambient top space in Figma (smooth purple/pink)
    // The main body has a gradient, so we use a translucent matching top bar.
    gfx::gradient(barX, 0, barWidth, topBarHeight, 0xD4ABC6, 0xC8A8D0);
    // Draw a subtle bottom separator (or maybe not, Figma looks very clean without it)
    // gfx::rect(barX, topBarHeight - 1, barWidth, 1, 0xFFFFFF, 40);
    
    float y = (topBarHeight - 28) / 2;
    // Left edge of the content starts after the vertical sidebar
    float leftX = g_leftSidebarVisible ? LEFT_SIDEBAR_WIDTH + 20 : 20;
    
    // === LEFT: Shield Logo ===
    bool shieldHover = hit(leftX, y, 24, 24);
    if (shieldHover) {
        gfx::circle(leftX + 12, y + 12, 16, 0xFFFFFF, 60);
        g_uiHoverHand = true;
    }
    svg("shield.svg", leftX, y, 24, 0xFFFFFF);
    leftX += 40;
    
    // === TABS START ===
    float tabStartX = leftX;
    float maxAvailableW = barWidth - 250 - tabStartX - 48; // Reserve space for + button
    
    int totalTabs = g_tabs.size();
    float inactiveTabW = 140.0f;
    float activeTabW = std::max(240.0f, maxAvailableW * 0.4f);
    if (activeTabW > 600.0f) activeTabW = 600.0f;
    
    float totalRequiredW = activeTabW + std::max(0, totalTabs - 1) * (inactiveTabW + 12);
    if (totalRequiredW > maxAvailableW) {
        float minRequiredW = activeTabW + std::max(0, totalTabs - 1) * (40.0f + 12);
        if (minRequiredW <= maxAvailableW) {
            inactiveTabW = (maxAvailableW - activeTabW - std::max(0, totalTabs - 1) * 12) / std::max(1, totalTabs - 1);
        } else {
            inactiveTabW = 40.0f;
        }
    }
    
    float scrollOffset = 0.0f;
    totalRequiredW = activeTabW + std::max(0, totalTabs - 1) * (inactiveTabW + 12);
    if (totalRequiredW > maxAvailableW) {
        float activeLeft = 0;
        for (const Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) break;
            activeLeft += inactiveTabW + 12;
        }
        if (activeLeft + activeTabW > maxAvailableW) {
            scrollOffset = (activeLeft + activeTabW) - maxAvailableW;
        }
    }
    
    leftX = tabStartX - scrollOffset;
    
    for (const Tab& tab : g_tabs) {
        bool tActive = (tab.id == g_activeTabId);
        float tabW = tActive ? activeTabW : inactiveTabW;
        
        // Skip rendering if completely out of bounds (simulating scroll clipping)
        if (leftX + tabW < tabStartX) {
            leftX += tabW + 12;
            continue;
        }
        if (leftX > tabStartX + maxAvailableW) {
            leftX += tabW + 12;
            continue;
        }
        
        float drawX = std::max(tabStartX, leftX);
        float drawW = std::min(tabW - (drawX - leftX), (tabStartX + maxAvailableW) - drawX);
        if (drawW < 40) { // Too small to meaningfully render
            leftX += tabW + 12;
            continue; 
        }
        
        bool tHover = hit(leftX, y - 4, tabW, 36);
        
        if (tHover) g_uiHoverHand = true;

        // Figma shows translucent pill backgrounds for tabs
        uint8_t alpha = tActive ? 80 : (tHover ? 100 : 40);
        gfx::rrect(leftX, y - 4, tabW, 36, 18, 0xFFFFFF, alpha); 
        
        // Tab Text / URL Input
        std::string tTitle = tab.title;
        if (tTitle.empty()) tTitle = "New Tab";
        
        bool showPlaceholder = false;
        if (tActive && g_addressFocused) {
            if (g_addressInput.empty()) {
                tTitle = "Search with Ketivee or enter web destination";
                showPlaceholder = true;
            } else {
                tTitle = g_addressInput;
            }
        } else if (tActive) {
            if (tab.url.empty() || tab.url == "zepra://start") {
                tTitle = "Search with Ketivee or enter web destination";
                showPlaceholder = true;
            } else {
                tTitle = tab.url;
            }
        }
        
        // Determine if we should show the close button
        bool showClose = false;
        if (tActive) {
            showClose = true;
        } else if (totalTabs <= 5 && tHover) {
            showClose = true;
        }
        
        // Limit string to fit tab width (prevent underflow crash)
        // If close button is hidden, we have extra room for text
        // We subtract another 28 for the favicon
        float textAvailableW = showClose ? drawW - 28 - 28 : drawW - 8 - 28;
        if (textAvailableW < 0) textAvailableW = 0;
        int maxChars = std::max(2, (int)(textAvailableW / 8.0f));
        if (tTitle.length() > (size_t)maxChars) {
            tTitle = tTitle.substr(0, maxChars - 2) + "..";
        }
        
        // Only draw elements if we actually have room for them
        if (drawW > 30) {
            uint32_t textColor = showPlaceholder ? 0x888888 : 0xFFFFFF; // Ghost text if placeholder
            
            // Draw Favicon
            std::string iconName = (tab.url.empty() || tab.url == "zepra://start") ? "zepra.svg" : "globe.svg";
            svg(iconName, drawX + 12, y + 6, 16, 0xEEEEEE);
            
            float textX = drawX + 36;
            text(tTitle, textX, y + 15, textColor, 13.0f);
            
            // Draw blinking cursor if active and focused
            if (tActive && g_addressFocused) {
                extern int g_cursorBlink;
                g_cursorBlink++;
                if ((g_cursorBlink / 30) % 2 == 0) {
                    float cursorX = textX + (showPlaceholder ? 0 : textWidth(tTitle, 13.0f));
                    gfx::rect(cursorX, y + 6, 2, 16, 0xFFFFFF); // White cursor
                }
            }
        }
        
        // Inner close button pill (attached to the right side of the tab)
        if (showClose) {
            float closePillX = leftX + tabW - 32;
            if (closePillX >= tabStartX && closePillX + 24 <= tabStartX + maxAvailableW) {
                bool cHover = hit(closePillX, y, 24, 24);
                if (cHover) {
                    gfx::circle(closePillX + 12, y + 14, 12, 0xFFFFFF, 120);
                    g_uiHoverHand = true;
                } else {
                    gfx::circle(closePillX + 12, y + 14, 12, 0xFFFFFF, 60);
                }
                svg("close.svg", closePillX + 4, y + 6, 16, 0x333333);
            }
        }
        
        leftX += tabW + 12;
    }
    
    // Plus button tab (small pill)
    if (leftX >= tabStartX && leftX <= tabStartX + maxAvailableW + 48) {
        bool nHover = hit(leftX, y - 2, 32, 32);
        if (nHover) {
            gfx::rrect(leftX, y - 4, 40, 36, 18, 0xFFFFFF, 100);
            g_uiHoverHand = true;
        } else {
            gfx::rrect(leftX, y - 4, 40, 36, 18, 0xFFFFFF, 50);
        }
        svg("plus.svg", leftX + 8, y + 2, 24, 0xFFFFFF);
    }
    
    // === FAR RIGHT: Tools ===
    float rightX = barWidth - 180;
    
    const char* rightIcons[] = {"refresh.svg", "MialInbox.svg", "ringingBell.svg", "tablet.svg"}; // Matching Figma as closely as possible
    
    for (int i = 0; i < 4; i++) {
        bool hover = hit(rightX, y, 32, 32);
        if (hover) {
            gfx::rrect(rightX + 2, y + 2, 27, 27, 6, 0xFFFFFF, 120);
            g_uiHoverHand = true;
        } else {
            gfx::rrect(rightX + 2, y + 2, 27, 27, 6, 0xFFFFFF, 60);
        }
        // Offset icons slightly to center them in the rrect
        svg(rightIcons[i], rightX + 3, y + 3, 24, 0x333333);
        rightX += 40;
    }
}

// Forward declarations for actions called by the click handler
extern void onCloseTab(int tabId);
extern void onSelectTab(int tabId);
extern void onNewTab();

inline bool handleTopBarClick(float mx, float my) {
    if (my >= TOPBAR_HEIGHT) return false; // Not in top bar

    float y = (TOPBAR_HEIGHT - 28) / 2;
    float leftX = g_leftSidebarVisible ? LEFT_SIDEBAR_WIDTH + 20 : 20;

    // 1. Shield Logo
    if (mx >= leftX && mx <= leftX + 24 && my >= y && my <= y + 24) {
        onNavigate("zepra://start");
        return true;
    }
    leftX += 40;

    // 2. Tabs Hit Handling
    float tabStartX = leftX;
    float maxAvailableW = g_width - 250 - tabStartX - 48; // Reserve space for + button
    
    int totalTabs = g_tabs.size();
    float inactiveTabW = 140.0f;
    float activeTabW = std::max(240.0f, maxAvailableW * 0.4f);
    if (activeTabW > 600.0f) activeTabW = 600.0f;
    
    float totalRequiredW = activeTabW + std::max(0, totalTabs - 1) * (inactiveTabW + 12);
    if (totalRequiredW > maxAvailableW) {
        float minRequiredW = activeTabW + std::max(0, totalTabs - 1) * 52.0f; // 40 + 12
        if (minRequiredW <= maxAvailableW) {
            inactiveTabW = (maxAvailableW - activeTabW - std::max(0, totalTabs - 1) * 12) / std::max(1, totalTabs - 1);
        } else {
            inactiveTabW = 40.0f;
        }
    }
    
    float scrollOffset = 0.0f;
    totalRequiredW = activeTabW + std::max(0, totalTabs - 1) * (inactiveTabW + 12);
    if (totalRequiredW > maxAvailableW) {
        float activeLeft = 0;
        for (const Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) break;
            activeLeft += inactiveTabW + 12;
        }
        if (activeLeft + activeTabW > maxAvailableW) {
            scrollOffset = (activeLeft + activeTabW) - maxAvailableW;
        }
    }
    
    leftX = tabStartX - scrollOffset;
    
    for (const Tab& tab : g_tabs) {
        bool tActive = (tab.id == g_activeTabId);
        float tabW = tActive ? activeTabW : inactiveTabW;
        
        if (leftX + tabW >= tabStartX && leftX <= tabStartX + maxAvailableW) {
            bool showClose = false;
            if (tActive) {
                showClose = true;
            } else if (totalTabs <= 5) {
                showClose = true;
            }

            // Check Close button
            if (showClose) {
                float closePillX = leftX + tabW - 32;
                if (closePillX >= tabStartX && closePillX + 24 <= tabStartX + maxAvailableW) {
                    if (mx >= closePillX && mx <= closePillX + 24 && my >= y && my <= y + 24) {
                        onCloseTab(tab.id);
                        return true;
                    }
                }
            }
            
            // Tab select / URL focus
            if (mx >= std::max(tabStartX, leftX) && mx <= std::min(tabStartX + maxAvailableW, leftX + tabW) && my >= y - 4 && my <= y + 36) {
                if (tActive) {
                    g_addressFocused = true;
                    if (g_currentUrl != "zepra://start") {
                        g_addressInput = g_currentUrl;
                    } else {
                        g_addressInput = "";
                    }
                } else {
                    onSelectTab(tab.id);
                }
                return true;
            }
        }
        leftX += tabW + 12;
    }
    
    // 3. New Tab Button Hit Handling
    if (leftX >= tabStartX && leftX <= tabStartX + maxAvailableW + 48) {
        if (mx >= leftX && mx <= leftX + 40 && my >= y - 4 && my <= y + 32) {
            onNewTab();
            return true;
        }
    }

    return true; // Consumed
}

