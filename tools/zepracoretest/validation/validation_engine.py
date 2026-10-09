#!/usr/bin/env python3
"""
Zepra Browser Validation Engine
Handles configuration validation, code validation, and system health checks
"""

import json
import os
import sys
import subprocess
import platform
from typing import Dict, List, Any, Optional
from dataclasses import dataclass
from pathlib import Path
import logging

@dataclass
class ValidationResult:
    """Result of a validation check"""
    name: str
    status: str  # 'PASS', 'FAIL', 'WARNING'
    message: str
    details: Optional[Dict[str, Any]] = None

class ValidationEngine:
    """Main validation engine for Zepra Browser"""
    
    def __init__(self, project_root: str = "."):
        self.project_root = Path(project_root)
        self.logger = self._setup_logging()
        
    def _setup_logging(self) -> logging.Logger:
        """Setup logging for validation engine"""
        logger = logging.getLogger('validation_engine')
        logger.setLevel(logging.INFO)
        
        if not logger.handlers:
            handler = logging.StreamHandler()
            formatter = logging.Formatter(
                '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
            )
            handler.setFormatter(formatter)
            logger.addHandler(handler)
            
        return logger
    
    def validate_configuration(self, config_path: str) -> List[ValidationResult]:
        """Validate configuration files"""
        results = []
        config_file = self.project_root / config_path
        
        # Check if config file exists
        if not config_file.exists():
            results.append(ValidationResult(
                name="Config File Existence",
                status="FAIL",
                message=f"Configuration file {config_path} not found"
            ))
            return results
        
        # Validate JSON syntax
        try:
            with open(config_file, 'r') as f:
                config_data = json.load(f)
            results.append(ValidationResult(
                name="JSON Syntax",
                status="PASS",
                message="Configuration file has valid JSON syntax"
            ))
        except json.JSONDecodeError as e:
            results.append(ValidationResult(
                name="JSON Syntax",
                status="FAIL",
                message=f"Invalid JSON syntax: {str(e)}"
            ))
            return results
        
        # Validate required fields
        required_fields = ['browser_name', 'version', 'features']
        for field in required_fields:
            if field in config_data:
                results.append(ValidationResult(
                    name=f"Required Field: {field}",
                    status="PASS",
                    message=f"Required field '{field}' present"
                ))
            else:
                results.append(ValidationResult(
                    name=f"Required Field: {field}",
                    status="FAIL",
                    message=f"Required field '{field}' missing"
                ))
        
        return results
    
    def validate_build_environment(self) -> List[ValidationResult]:
        """Validate build environment and dependencies"""
        results = []
        
        # Check Python version
        python_version = sys.version_info
        if python_version >= (3, 8):
            results.append(ValidationResult(
                name="Python Version",
                status="PASS",
                message=f"Python {python_version.major}.{python_version.minor}.{python_version.micro} is compatible"
            ))
        else:
            results.append(ValidationResult(
                name="Python Version",
                status="FAIL",
                message=f"Python {python_version.major}.{python_version.minor}.{python_version.micro} is too old. Need 3.8+"
            ))
        
        # Check CMake
        try:
            cmake_result = subprocess.run(['cmake', '--version'], 
                                        capture_output=True, text=True, timeout=10)
            if cmake_result.returncode == 0:
                results.append(ValidationResult(
                    name="CMake",
                    status="PASS",
                    message="CMake is available"
                ))
            else:
                results.append(ValidationResult(
                    name="CMake",
                    status="FAIL",
                    message="CMake is not available or not working"
                ))
        except (subprocess.TimeoutExpired, FileNotFoundError):
            results.append(ValidationResult(
                name="CMake",
                status="FAIL",
                message="CMake not found in PATH"
            ))
        
        # Check C++ compiler
        compilers = ['g++', 'clang++', 'msvc']
        compiler_found = False
        
        for compiler in compilers:
            try:
                result = subprocess.run([compiler, '--version'], 
                                      capture_output=True, text=True, timeout=10)
                if result.returncode == 0:
                    results.append(ValidationResult(
                        name="C++ Compiler",
                        status="PASS",
                        message=f"Found C++ compiler: {compiler}"
                    ))
                    compiler_found = True
                    break
            except (subprocess.TimeoutExpired, FileNotFoundError):
                continue
        
        if not compiler_found:
            results.append(ValidationResult(
                name="C++ Compiler",
                status="FAIL",
                message="No C++ compiler found (g++, clang++, or msvc)"
            ))
        
        return results
    
    def validate_code_structure(self) -> List[ValidationResult]:
        """Validate code structure and organization"""
        results = []
        
        # Check required directories
        required_dirs = ['src', 'include', 'test', 'tools']
        for dir_name in required_dirs:
            dir_path = self.project_root / dir_name
            if dir_path.exists() and dir_path.is_dir():
                results.append(ValidationResult(
                    name=f"Directory: {dir_name}",
                    status="PASS",
                    message=f"Required directory '{dir_name}' exists"
                ))
            else:
                results.append(ValidationResult(
                    name=f"Directory: {dir_name}",
                    status="FAIL",
                    message=f"Required directory '{dir_name}' missing"
                ))
        
        # Check CMakeLists.txt
        cmake_file = self.project_root / "CMakeLists.txt"
        if cmake_file.exists():
            results.append(ValidationResult(
                name="CMakeLists.txt",
                status="PASS",
                message="CMakeLists.txt found"
            ))
        else:
            results.append(ValidationResult(
                name="CMakeLists.txt",
                status="FAIL",
                message="CMakeLists.txt missing"
            ))
        
        return results
    
    def run_all_validations(self, config_path: str = "configs/system.ncf") -> Dict[str, List[ValidationResult]]:
        """Run all validation checks"""
        self.logger.info("Starting comprehensive validation...")
        
        results = {
            'configuration': self.validate_configuration(config_path),
            'build_environment': self.validate_build_environment(),
            'code_structure': self.validate_code_structure()
        }
        
        # Summary
        total_checks = sum(len(result_list) for result_list in results.values())
        passed_checks = sum(
            len([r for r in result_list if r.status == 'PASS'])
            for result_list in results.values()
        )
        failed_checks = sum(
            len([r for r in result_list if r.status == 'FAIL'])
            for result_list in results.values()
        )
        
        self.logger.info(f"Validation complete: {passed_checks}/{total_checks} passed, {failed_checks} failed")
        
        return results

def main():
    """Main entry point for validation engine"""
    engine = ValidationEngine()
    
    # Run all validations
    results = engine.run_all_validations()
    
    # Print results
    for category, result_list in results.items():
        print(f"\n=== {category.upper()} ===")
        for result in result_list:
            status_icon = "✅" if result.status == "PASS" else "❌" if result.status == "FAIL" else "⚠️"
            print(f"{status_icon} {result.name}: {result.message}")
    
    # Exit with error code if any failures
    total_failures = sum(
        len([r for r in result_list if r.status == 'FAIL'])
        for result_list in results.values()
    )
    
    if total_failures > 0:
        sys.exit(1)

if __name__ == "__main__":
    main() 