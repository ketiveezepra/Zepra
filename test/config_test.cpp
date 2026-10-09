#include "config/config_manager.h"
#include <iostream>
#include <string>

int main() {
    std::cout << "=== Ketivee OS Configuration System Test ===" << std::endl;
    
    zepra::ConfigManager configManager;
    
    // Test NCF (INI-style) configuration
    std::cout << "\n1. Testing NCF Configuration:" << std::endl;
    std::cout << "Loading system.ncf..." << std::endl;
    
    if (configManager.loadConfig("configs/system.ncf", zepra::ConfigFileType::NCF)) {
        std::cout << "✓ NCF file loaded successfully" << std::endl;
        
        // Read some values
        std::string osName = configManager.getValue("system", "name");
        std::string osVersion = configManager.getValue("system", "version");
        std::string bootTimeout = configManager.getValue("boot", "timeout");
        
        std::cout << "OS Name: " << osName << std::endl;
        std::cout << "OS Version: " << osVersion << std::endl;
        std::cout << "Boot Timeout: " << bootTimeout << " seconds" << std::endl;
        
        // Modify a value
        configManager.setValue("performance", "cache_size", "4GB");
        std::cout << "Modified cache size to: " << configManager.getValue("performance", "cache_size") << std::endl;
        
        // Save modified config
        if (configManager.saveConfig("configs/system_modified.ncf", zepra::ConfigFileType::NCF)) {
            std::cout << "✓ Modified NCF saved successfully" << std::endl;
        }
        
    } else {
        std::cout << "✗ Failed to load NCF file" << std::endl;
    }
    
    // Test TIE (JSON-style) configuration
    std::cout << "\n2. Testing TIE Configuration:" << std::endl;
    std::cout << "Loading zepra_browser.tie..." << std::endl;
    
    if (configManager.loadConfig("configs/zepra_browser.tie", zepra::ConfigFileType::TIE)) {
        std::cout << "✓ TIE file loaded successfully" << std::endl;
        
        // Read app information
        std::string appName = configManager.getValue("app_info", "name");
        std::string appVersion = configManager.getValue("app_info", "version");
        std::string developer = configManager.getValue("app_info", "developer");
        
        std::cout << "App Name: " << appName << std::endl;
        std::cout << "App Version: " << appVersion << std::endl;
        std::cout << "Developer: " << developer << std::endl;
        
        // Check features
        std::string searchEnabled = configManager.getValue("features.search_engine", "enabled");
        std::string devToolsEnabled = configManager.getValue("features.developer_tools", "enabled");
        
        std::cout << "Search Engine Enabled: " << searchEnabled << std::endl;
        std::cout << "Developer Tools Enabled: " << devToolsEnabled << std::endl;
        
        // Get installation recipe
        std::string installRecipe = configManager.getInstallRecipe();
        if (!installRecipe.empty()) {
            std::cout << "\nInstallation Recipe:" << std::endl;
            std::cout << installRecipe << std::endl;
            
            // Process installation recipe
            std::cout << "\nProcessing installation recipe..." << std::endl;
            if (configManager.processInstallRecipe()) {
                std::cout << "✓ Installation recipe processed successfully" << std::endl;
            } else {
                std::cout << "✗ Failed to process installation recipe" << std::endl;
            }
        }
        
        // Modify sandbox settings
        configManager.setValue("sandbox", "memory_limit", "1GB");
        std::cout << "Modified sandbox memory limit to: " << configManager.getValue("sandbox", "memory_limit") << std::endl;
        
        // Save modified config
        if (configManager.saveConfig("configs/zepra_browser_modified.tie", zepra::ConfigFileType::TIE)) {
            std::cout << "✓ Modified TIE saved successfully" << std::endl;
        }
        
    } else {
        std::cout << "✗ Failed to load TIE file" << std::endl;
    }
    
    // Test configuration validation
    std::cout << "\n3. Testing Configuration Validation:" << std::endl;
    
    bool ncfValid = configManager.validateConfig(zepra::ConfigFileType::NCF);
    bool tieValid = configManager.validateConfig(zepra::ConfigFileType::TIE);
    
    std::cout << "NCF Configuration Valid: " << (ncfValid ? "✓ Yes" : "✗ No") << std::endl;
    std::cout << "TIE Configuration Valid: " << (tieValid ? "✓ Yes" : "✗ No") << std::endl;
    
    // Test creating new configurations
    std::cout << "\n4. Testing New Configuration Creation:" << std::endl;
    
    zepra::ConfigManager newConfig;
    
    // Create a new NCF config
    newConfig.setValue("network", "hostname", "test-system");
    newConfig.setValue("network", "domain", "test.local");
    newConfig.setValue("security", "firewall", "enabled");
    newConfig.setValue("security", "encryption", "aes256");
    
    if (newConfig.saveConfig("configs/test_network.ncf", zepra::ConfigFileType::NCF)) {
        std::cout << "✓ Created new NCF configuration" << std::endl;
    }
    
    // Create a new TIE config
    zepra::ConfigManager newTieConfig;
    newTieConfig.setValue("app_info", "name", "Test App");
    newTieConfig.setValue("app_info", "version", "1.0.0");
    newTieConfig.setValue("permissions", "network_access", "true");
    newTieConfig.setValue("installation", "install_recipe", "mkdir /opt/test\ncopy test_app /opt/test/\ncreate_desktop_entry");
    
    if (newTieConfig.saveConfig("configs/test_app.tie", zepra::ConfigFileType::TIE)) {
        std::cout << "✓ Created new TIE configuration" << std::endl;
    }
    
    std::cout << "\n=== Configuration System Test Complete ===" << std::endl;
    std::cout << "\nGenerated files:" << std::endl;
    std::cout << "- configs/system_modified.ncf" << std::endl;
    std::cout << "- configs/zepra_browser_modified.tie" << std::endl;
    std::cout << "- configs/test_network.ncf" << std::endl;
    std::cout << "- configs/test_app.tie" << std::endl;
    
    return 0;
} 