#!/usr/bin/env python3
"""
Zepra Browser Build Scripts
Automated build and development scripts for Zepra Browser
"""

import subprocess
import sys
import os
import platform
from pathlib import Path
from typing import Dict, List, Optional
import logging

class BuildScripts:
    """Build automation scripts for Zepra Browser"""
    
    def __init__(self, project_root: str = "."):
        self.project_root = Path(project_root)
        self.logger = self._setup_logging()
        
    def _setup_logging(self) -> logging.Logger:
        logger = logging.getLogger('build_scripts')
        logger.setLevel(logging.INFO)
        
        if not logger.handlers:
            handler = logging.StreamHandler()
            formatter = logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s')
            handler.setFormatter(formatter)
            logger.addHandler(handler)
        return logger
    
    def run_webkit_build(self) -> bool:
        """Run WebKit-style build process"""
        self.logger.info("Running WebKit-style build...")
        
        # Setup build directory
        build_dir = self.project_root / "build"
        build_dir.mkdir(exist_ok=True)
        
        # Configure with CMake
        config_cmd = [
            'cmake', '..',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DENABLE_TESTS=ON',
            '-DENABLE_DEVELOPER_MODE=ON'
        ]
        
        try:
            result = subprocess.run(config_cmd, cwd=build_dir, capture_output=True, text=True)
            if result.returncode != 0:
                self.logger.error(f"CMake configuration failed: {result.stderr}")
                return False
            
            # Build
            build_cmd = ['cmake', '--build', '.', '--parallel']
            result = subprocess.run(build_cmd, cwd=build_dir, capture_output=True, text=True)
            
            if result.returncode == 0:
                self.logger.info("Build completed successfully")
                return True
            else:
                self.logger.error(f"Build failed: {result.stderr}")
                return False
                
        except Exception as e:
            self.logger.error(f"Build script error: {e}")
            return False
    
    def run_webkit_tests(self) -> bool:
        """Run WebKit-style test suite"""
        self.logger.info("Running WebKit-style tests...")
        
        test_commands = [
            ['ctest', '--output-on-failure'],
            ['python', 'tools/zepracoretest/test/test_orchestrator.py']
        ]
        
        for cmd in test_commands:
            try:
                result = subprocess.run(cmd, cwd=self.project_root, capture_output=True, text=True)
                if result.returncode != 0:
                    self.logger.error(f"Test command failed: {cmd} - {result.stderr}")
                    return False
            except Exception as e:
                self.logger.error(f"Test execution error: {e}")
                return False
        
        self.logger.info("Tests completed successfully")
        return True
    
    def run_webkit_clean(self) -> bool:
        """Clean build artifacts WebKit-style"""
        self.logger.info("Cleaning build artifacts...")
        
        dirs_to_clean = ['build', 'bin', 'test_output', 'analytics_data']
        
        for dir_name in dirs_to_clean:
            dir_path = self.project_root / dir_name
            if dir_path.exists():
                import shutil
                shutil.rmtree(dir_path)
                self.logger.info(f"Cleaned {dir_name}")
        
        self.logger.info("Clean completed")
        return True

def main():
    """Main entry point"""
    scripts = BuildScripts()
    
    if len(sys.argv) < 2:
        print("Usage: python build_scripts.py [build|test|clean]")
        sys.exit(1)
    
    command = sys.argv[1]
    
    if command == "build":
        success = scripts.run_webkit_build()
    elif command == "test":
        success = scripts.run_webkit_tests()
    elif command == "clean":
        success = scripts.run_webkit_clean()
    else:
        print(f"Unknown command: {command}")
        sys.exit(1)
    
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main() 