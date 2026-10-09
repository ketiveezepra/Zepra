#!/usr/bin/env python3
"""
Zepra Browser Build Manager
Orchestrates C++ build process, handles dependencies, and manages build configurations
"""

import subprocess
import sys
import os
import platform
import json
import time
from pathlib import Path
from typing import Dict, List, Any, Optional, Tuple
from dataclasses import dataclass
import logging
import shutil
import threading

@dataclass
class BuildTarget:
    """Build target configuration"""
    name: str
    source_dir: str
    output_dir: str
    dependencies: List[str]
    compiler_flags: List[str]
    linker_flags: List[str]
    defines: List[str]

@dataclass
class BuildResult:
    """Result of a build operation"""
    target: str
    success: bool
    output: str
    duration: float
    errors: List[str]
    warnings: List[str]

class BuildManager:
    """Main build manager for Zepra Browser"""
    
    def __init__(self, project_root: str = ".", build_dir: str = "build"):
        self.project_root = Path(project_root)
        self.build_dir = Path(build_dir)
        self.logger = self._setup_logging()
        
        # Build configuration
        self.compiler = self._detect_compiler()
        self.build_type = "Release"
        self.parallel_jobs = os.cpu_count() or 4
        
        # Build targets
        self.targets = self._define_targets()
        
        # Build state
        self.is_building = False
        self.build_thread = None
        
    def _setup_logging(self) -> logging.Logger:
        """Setup logging for build manager"""
        logger = logging.getLogger('build_manager')
        logger.setLevel(logging.INFO)
        
        if not logger.handlers:
            handler = logging.StreamHandler()
            formatter = logging.Formatter(
                '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
            )
            handler.setFormatter(formatter)
            logger.addHandler(handler)
            
        return logger
    
    def _detect_compiler(self) -> str:
        """Detect available C++ compiler"""
        compilers = ['g++', 'clang++', 'msvc']
        
        for compiler in compilers:
            try:
                result = subprocess.run([compiler, '--version'], 
                                      capture_output=True, text=True, timeout=10)
                if result.returncode == 0:
                    self.logger.info(f"Detected compiler: {compiler}")
                    return compiler
            except (subprocess.TimeoutExpired, FileNotFoundError):
                continue
        
        # Default to g++ if none found
        self.logger.warning("No compiler detected, defaulting to g++")
        return 'g++'
    
    def _define_targets(self) -> Dict[str, BuildTarget]:
        """Define build targets"""
        targets = {
            'zepra_browser': BuildTarget(
                name='zepra_browser',
                source_dir='src',
                output_dir='bin',
                dependencies=['SDL2', 'OpenGL', 'curl', 'json'],
                compiler_flags=['-std=c++17', '-O2', '-Wall', '-Wextra'],
                linker_flags=['-lSDL2', '-lGL', '-lcurl'],
                defines=['ZEPRA_VERSION="1.0.0"', 'NDEBUG']
            ),
            'test_suite': BuildTarget(
                name='test_suite',
                source_dir='test',
                output_dir='bin/tests',
                dependencies=['zepra_browser'],
                compiler_flags=['-std=c++17', '-g', '-Wall', '-Wextra'],
                linker_flags=['-lgtest', '-lgtest_main'],
                defines=['TESTING']
            ),
            'tools': BuildTarget(
                name='tools',
                source_dir='tools',
                output_dir='bin/tools',
                dependencies=[],
                compiler_flags=['-std=c++17', '-O2', '-Wall'],
                linker_flags=[],
                defines=['TOOLS_BUILD']
            )
        }
        return targets
    
    def setup_build_environment(self) -> bool:
        """Setup build environment and dependencies"""
        self.logger.info("Setting up build environment...")
        
        # Create build directory
        self.build_dir.mkdir(exist_ok=True)
        
        # Create output directories
        for target in self.targets.values():
            Path(target.output_dir).mkdir(parents=True, exist_ok=True)
        
        # Check dependencies
        missing_deps = self._check_dependencies()
        if missing_deps:
            self.logger.error(f"Missing dependencies: {missing_deps}")
            return False
        
        # Generate CMake configuration
        self._generate_cmake_config()
        
        self.logger.info("Build environment setup complete")
        return True
    
    def _check_dependencies(self) -> List[str]:
        """Check if required dependencies are available"""
        missing = []
        
        # Check for SDL2
        try:
            result = subprocess.run(['pkg-config', '--exists', 'sdl2'], 
                                  capture_output=True, text=True)
            if result.returncode != 0:
                missing.append('SDL2')
        except FileNotFoundError:
            missing.append('SDL2')
        
        # Check for OpenGL
        try:
            result = subprocess.run(['pkg-config', '--exists', 'gl'], 
                                  capture_output=True, text=True)
            if result.returncode != 0:
                missing.append('OpenGL')
        except FileNotFoundError:
            missing.append('OpenGL')
        
        # Check for curl
        try:
            result = subprocess.run(['pkg-config', '--exists', 'libcurl'], 
                                  capture_output=True, text=True)
            if result.returncode != 0:
                missing.append('libcurl')
        except FileNotFoundError:
            missing.append('libcurl')
        
        return missing
    
    def _generate_cmake_config(self):
        """Generate CMake configuration"""
        cmake_config = f"""
cmake_minimum_required(VERSION 3.16)
project(ZepraBrowser VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build type
set(CMAKE_BUILD_TYPE {self.build_type})

# Compiler flags
set(CMAKE_CXX_FLAGS "${{CMAKE_CXX_FLAGS}} {' '.join(self.targets['zepra_browser'].compiler_flags)}")

# Find dependencies
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)

# Include directories
include_directories(${{CMAKE_SOURCE_DIR}}/include)
include_directories(${{CMAKE_SOURCE_DIR}}/external)

# Add executable
add_executable(zepra_browser
    src/main.cpp
    src/engine/html_parser.cpp
    src/engine/ketivee_search.cpp
    src/ui/window.cpp
    src/ui/tab_manager.cpp
    src/config/config_manager.cpp
    src/sandbox/sandbox_manager.cpp
)

# Link libraries
target_link_libraries(zepra_browser
    SDL2::SDL2
    OpenGL::GL
    CURL::libcurl
)

# Add tests
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    add_executable(test_suite test/test_basic.cpp)
    target_link_libraries(test_suite GTest::gtest GTest::gtest_main)
endif()
"""
        
        cmake_file = self.build_dir / "CMakeLists.txt"
        with open(cmake_file, 'w') as f:
            f.write(cmake_config)
        
        self.logger.info("Generated CMake configuration")
    
    def build_target(self, target_name: str) -> BuildResult:
        """Build a specific target"""
        if target_name not in self.targets:
            return BuildResult(
                target=target_name,
                success=False,
                output="",
                duration=0.0,
                errors=[f"Unknown target: {target_name}"],
                warnings=[]
            )
        
        target = self.targets[target_name]
        start_time = time.time()
        
        self.logger.info(f"Building target: {target_name}")
        
        try:
            # Use CMake for building
            if target_name == 'zepra_browser':
                return self._build_with_cmake(target_name)
            else:
                return self._build_with_compiler(target)
                
        except Exception as e:
            return BuildResult(
                target=target_name,
                success=False,
                output="",
                duration=time.time() - start_time,
                errors=[str(e)],
                warnings=[]
            )
    
    def _build_with_cmake(self, target_name: str) -> BuildResult:
        """Build using CMake"""
        start_time = time.time()
        errors = []
        warnings = []
        
        try:
            # Configure
            config_cmd = ['cmake', '..', '-DCMAKE_BUILD_TYPE=' + self.build_type]
            result = subprocess.run(config_cmd, cwd=self.build_dir, 
                                  capture_output=True, text=True, timeout=300)
            
            if result.returncode != 0:
                errors.append(f"CMake configuration failed: {result.stderr}")
                return BuildResult(
                    target=target_name,
                    success=False,
                    output=result.stdout,
                    duration=time.time() - start_time,
                    errors=errors,
                    warnings=warnings
                )
            
            # Build
            build_cmd = ['cmake', '--build', '.', '--parallel', str(self.parallel_jobs)]
            result = subprocess.run(build_cmd, cwd=self.build_dir, 
                                  capture_output=True, text=True, timeout=600)
            
            success = result.returncode == 0
            if not success:
                errors.append(f"Build failed: {result.stderr}")
            
            return BuildResult(
                target=target_name,
                success=success,
                output=result.stdout,
                duration=time.time() - start_time,
                errors=errors,
                warnings=warnings
            )
            
        except subprocess.TimeoutExpired:
            errors.append("Build timed out")
            return BuildResult(
                target=target_name,
                success=False,
                output="",
                duration=time.time() - start_time,
                errors=errors,
                warnings=warnings
            )
    
    def _build_with_compiler(self, target: BuildTarget) -> BuildResult:
        """Build using direct compiler invocation"""
        start_time = time.time()
        
        # Find source files
        source_dir = Path(target.source_dir)
        source_files = list(source_dir.rglob("*.cpp")) + list(source_dir.rglob("*.c"))
        
        if not source_files:
            return BuildResult(
                target=target.name,
                success=False,
                output="",
                duration=time.time() - start_time,
                errors=["No source files found"],
                warnings=[]
            )
        
        # Build command
        output_file = Path(target.output_dir) / target.name
        if platform.system() == "Windows":
            output_file = output_file.with_suffix('.exe')
        
        cmd = [
            self.compiler,
            *target.compiler_flags,
            *[f"-D{define}" for define in target.defines],
            *[str(f) for f in source_files],
            *target.linker_flags,
            '-o', str(output_file)
        ]
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            
            return BuildResult(
                target=target.name,
                success=result.returncode == 0,
                output=result.stdout,
                duration=time.time() - start_time,
                errors=[result.stderr] if result.stderr else [],
                warnings=[]
            )
            
        except subprocess.TimeoutExpired:
            return BuildResult(
                target=target.name,
                success=False,
                output="",
                duration=time.time() - start_time,
                errors=["Build timed out"],
                warnings=[]
            )
    
    def build_all(self) -> Dict[str, BuildResult]:
        """Build all targets"""
        self.logger.info("Starting full build...")
        
        results = {}
        
        # Build dependencies first
        for target_name in ['zepra_browser', 'test_suite', 'tools']:
            if target_name in self.targets:
                result = self.build_target(target_name)
                results[target_name] = result
                
                if not result.success:
                    self.logger.error(f"Build failed for {target_name}: {result.errors}")
                    break
        
        return results
    
    def clean_build(self):
        """Clean build artifacts"""
        self.logger.info("Cleaning build artifacts...")
        
        if self.build_dir.exists():
            shutil.rmtree(self.build_dir)
        
        # Clean output directories
        for target in self.targets.values():
            output_dir = Path(target.output_dir)
            if output_dir.exists():
                shutil.rmtree(output_dir)
        
        self.logger.info("Build cleaned")
    
    def get_build_status(self) -> Dict[str, Any]:
        """Get current build status"""
        status = {
            'compiler': self.compiler,
            'build_type': self.build_type,
            'parallel_jobs': self.parallel_jobs,
            'is_building': self.is_building,
            'targets': {}
        }
        
        for name, target in self.targets.items():
            output_file = Path(target.output_dir) / name
            if platform.system() == "Windows":
                output_file = output_file.with_suffix('.exe')
            
            status['targets'][name] = {
                'output_file': str(output_file),
                'exists': output_file.exists(),
                'last_modified': output_file.stat().st_mtime if output_file.exists() else None
            }
        
        return status

def main():
    """Main entry point for build manager"""
    build_manager = BuildManager()
    
    # Setup build environment
    if not build_manager.setup_build_environment():
        print("❌ Failed to setup build environment")
        sys.exit(1)
    
    # Build all targets
    results = build_manager.build_all()
    
    # Print results
    print("\n=== Build Results ===")
    for target_name, result in results.items():
        status = "✅ PASS" if result.success else "❌ FAIL"
        print(f"{status} {target_name} ({result.duration:.2f}s)")
        
        if result.errors:
            print(f"  Errors: {result.errors}")
        if result.warnings:
            print(f"  Warnings: {result.warnings}")
    
    # Check overall success
    all_success = all(result.success for result in results.values())
    if all_success:
        print("\n🎉 All builds successful!")
    else:
        print("\n💥 Some builds failed!")
        sys.exit(1)

if __name__ == "__main__":
    main() 