#!/usr/bin/env python3
"""
Zepra Browser Configuration Manager
Handles all configuration, settings, and environment variables
"""

import json
import os
import sys
from pathlib import Path
from typing import Dict, Any, Optional, List
from dataclasses import dataclass, asdict
import logging

@dataclass
class BrowserConfig:
    """Browser configuration settings"""
    browser_name: str = "Zepra Browser"
    version: str = "1.0.0"
    window_width: int = 1200
    window_height: int = 800
    max_tabs: int = 50
    enable_developer_tools: bool = True
    enable_analytics: bool = True
    search_engine_url: str = "https://ketivee.com/search"
    cache_size_mb: int = 100
    enable_hardware_acceleration: bool = True
    user_agent: str = "Zepra/1.0 (Ketivee Browser Engine)"

@dataclass
class BuildConfig:
    """Build configuration settings"""
    compiler: str = "g++"
    build_type: str = "Release"
    enable_tests: bool = True
    enable_debug: bool = False
    parallel_builds: int = 4
    cmake_generator: str = "Unix Makefiles"
    install_prefix: str = "/usr/local"

@dataclass
class TestConfig:
    """Testing configuration settings"""
    test_timeout: int = 30
    enable_unit_tests: bool = True
    enable_integration_tests: bool = True
    enable_performance_tests: bool = True
    test_coverage: bool = True
    test_output_format: str = "json"

class ConfigManager:
    """Main configuration manager for Zepra Browser"""
    
    def __init__(self, config_dir: str = "configs"):
        self.config_dir = Path(config_dir)
        self.logger = self._setup_logging()
        
        # Default configurations
        self.browser_config = BrowserConfig()
        self.build_config = BuildConfig()
        self.test_config = TestConfig()
        
        # Load configurations
        self._load_configurations()
    
    def _setup_logging(self) -> logging.Logger:
        """Setup logging for configuration manager"""
        logger = logging.getLogger('config_manager')
        logger.setLevel(logging.INFO)
        
        if not logger.handlers:
            handler = logging.StreamHandler()
            formatter = logging.Formatter(
                '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
            )
            handler.setFormatter(formatter)
            logger.addHandler(handler)
            
        return logger
    
    def _load_configurations(self):
        """Load all configuration files"""
        # Ensure config directory exists
        self.config_dir.mkdir(exist_ok=True)
        
        # Load browser configuration
        browser_config_file = self.config_dir / "browser_config.json"
        if browser_config_file.exists():
            try:
                with open(browser_config_file, 'r') as f:
                    browser_data = json.load(f)
                    for key, value in browser_data.items():
                        if hasattr(self.browser_config, key):
                            setattr(self.browser_config, key, value)
                self.logger.info("Loaded browser configuration")
            except Exception as e:
                self.logger.warning(f"Failed to load browser config: {e}")
        
        # Load build configuration
        build_config_file = self.config_dir / "build_config.json"
        if build_config_file.exists():
            try:
                with open(build_config_file, 'r') as f:
                    build_data = json.load(f)
                    for key, value in build_data.items():
                        if hasattr(self.build_config, key):
                            setattr(self.build_config, key, value)
                self.logger.info("Loaded build configuration")
            except Exception as e:
                self.logger.warning(f"Failed to load build config: {e}")
        
        # Load test configuration
        test_config_file = self.config_dir / "test_config.json"
        if test_config_file.exists():
            try:
                with open(test_config_file, 'r') as f:
                    test_data = json.load(f)
                    for key, value in test_data.items():
                        if hasattr(self.test_config, key):
                            setattr(self.test_config, key, value)
                self.logger.info("Loaded test configuration")
            except Exception as e:
                self.logger.warning(f"Failed to load test config: {e}")
    
    def save_configurations(self):
        """Save all configurations to files"""
        # Save browser configuration
        browser_config_file = self.config_dir / "browser_config.json"
        with open(browser_config_file, 'w') as f:
            json.dump(asdict(self.browser_config), f, indent=2)
        
        # Save build configuration
        build_config_file = self.config_dir / "build_config.json"
        with open(build_config_file, 'w') as f:
            json.dump(asdict(self.build_config), f, indent=2)
        
        # Save test configuration
        test_config_file = self.config_dir / "test_config.json"
        with open(test_config_file, 'w') as f:
            json.dump(asdict(self.test_config), f, indent=2)
        
        self.logger.info("All configurations saved")
    
    def get_browser_config(self) -> BrowserConfig:
        """Get browser configuration"""
        return self.browser_config
    
    def get_build_config(self) -> BuildConfig:
        """Get build configuration"""
        return self.build_config
    
    def get_test_config(self) -> TestConfig:
        """Get test configuration"""
        return self.test_config
    
    def update_browser_config(self, **kwargs):
        """Update browser configuration"""
        for key, value in kwargs.items():
            if hasattr(self.browser_config, key):
                setattr(self.browser_config, key, value)
            else:
                self.logger.warning(f"Unknown browser config key: {key}")
    
    def update_build_config(self, **kwargs):
        """Update build configuration"""
        for key, value in kwargs.items():
            if hasattr(self.build_config, key):
                setattr(self.build_config, key, value)
            else:
                self.logger.warning(f"Unknown build config key: {key}")
    
    def update_test_config(self, **kwargs):
        """Update test configuration"""
        for key, value in kwargs.items():
            if hasattr(self.test_config, key):
                setattr(self.test_config, key, value)
            else:
                self.logger.warning(f"Unknown test config key: {key}")
    
    def generate_cmake_config(self) -> str:
        """Generate CMake configuration from build config"""
        cmake_config = f"""
# Generated CMake configuration
set(CMAKE_BUILD_TYPE {self.build_config.build_type})
set(CMAKE_CXX_COMPILER {self.build_config.compiler})
set(CMAKE_GENERATOR "{self.build_config.cmake_generator}")
set(CMAKE_INSTALL_PREFIX {self.build_config.install_prefix})

# Build options
option(ENABLE_TESTS "{self.build_config.enable_tests}")
option(ENABLE_DEBUG "{self.build_config.enable_debug}")

# Parallel builds
set(CMAKE_BUILD_PARALLEL_LEVEL {self.build_config.parallel_builds})
"""
        return cmake_config
    
    def generate_environment_vars(self) -> Dict[str, str]:
        """Generate environment variables from configuration"""
        env_vars = {
            'ZEPRA_BROWSER_NAME': self.browser_config.browser_name,
            'ZEPRA_VERSION': self.browser_config.version,
            'ZEPRA_WINDOW_WIDTH': str(self.browser_config.window_width),
            'ZEPRA_WINDOW_HEIGHT': str(self.browser_config.window_height),
            'ZEPRA_MAX_TABS': str(self.browser_config.max_tabs),
            'ZEPRA_SEARCH_ENGINE_URL': self.browser_config.search_engine_url,
            'ZEPRA_CACHE_SIZE_MB': str(self.browser_config.cache_size_mb),
            'ZEPRA_USER_AGENT': self.browser_config.user_agent,
            'ZEPRA_ENABLE_ANALYTICS': str(self.browser_config.enable_analytics).lower(),
            'ZEPRA_ENABLE_DEV_TOOLS': str(self.browser_config.enable_developer_tools).lower(),
            'ZEPRA_ENABLE_HW_ACCEL': str(self.browser_config.enable_hardware_acceleration).lower()
        }
        return env_vars
    
    def validate_configurations(self) -> List[str]:
        """Validate all configurations and return list of errors"""
        errors = []
        
        # Validate browser config
        if self.browser_config.window_width <= 0 or self.browser_config.window_height <= 0:
            errors.append("Window dimensions must be positive")
        
        if self.browser_config.max_tabs <= 0:
            errors.append("Max tabs must be positive")
        
        if self.browser_config.cache_size_mb <= 0:
            errors.append("Cache size must be positive")
        
        # Validate build config
        if self.build_config.parallel_builds <= 0:
            errors.append("Parallel builds must be positive")
        
        # Validate test config
        if self.test_config.test_timeout <= 0:
            errors.append("Test timeout must be positive")
        
        return errors

def main():
    """Main entry point for configuration manager"""
    config_manager = ConfigManager()
    
    # Print current configurations
    print("=== Browser Configuration ===")
    browser_config = config_manager.get_browser_config()
    for field, value in asdict(browser_config).items():
        print(f"{field}: {value}")
    
    print("\n=== Build Configuration ===")
    build_config = config_manager.get_build_config()
    for field, value in asdict(build_config).items():
        print(f"{field}: {value}")
    
    print("\n=== Test Configuration ===")
    test_config = config_manager.get_test_config()
    for field, value in asdict(test_config).items():
        print(f"{field}: {value}")
    
    # Validate configurations
    errors = config_manager.validate_configurations()
    if errors:
        print("\n=== Configuration Errors ===")
        for error in errors:
            print(f"❌ {error}")
        sys.exit(1)
    else:
        print("\n✅ All configurations are valid")
    
    # Save configurations
    config_manager.save_configurations()

if __name__ == "__main__":
    main() 