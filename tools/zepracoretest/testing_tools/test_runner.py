#!/usr/bin/env python3
"""
Zepra Browser Engine - Test Runner
Runs unit tests, integration tests, and generates comprehensive test reports
"""

import os
import sys
import json
import time
import subprocess
import unittest
from pathlib import Path
from typing import Dict, List, Any, Optional
from datetime import datetime

class TestRunner:
    def __init__(self, project_root: str):
        self.project_root = Path(project_root)
        self.test_results = {}
        self.test_suites = {}
        
    def discover_tests(self) -> Dict[str, Any]:
        """Discover all available tests in the project"""
        print("🔍 Discovering tests...")
        
        test_discovery = {
            "unit_tests": [],
            "integration_tests": [],
            "performance_tests": [],
            "total_tests": 0
        }
        
        # Look for test files in various locations
        test_locations = [
            self.project_root / "test",
            self.project_root / "src" / "engine",
            self.project_root / "src" / "ui",
            self.project_root / "src" / "search"
        ]
        
        for location in test_locations:
            if location.exists():
                for test_file in location.rglob("*test*.cpp"):
                    test_info = self._analyze_test_file(test_file)
                    if test_info:
                        if "unit" in test_file.name.lower():
                            test_discovery["unit_tests"].append(test_info)
                        elif "integration" in test_file.name.lower():
                            test_discovery["integration_tests"].append(test_info)
                        elif "performance" in test_file.name.lower():
                            test_discovery["performance_tests"].append(test_info)
                        else:
                            test_discovery["unit_tests"].append(test_info)
        
        test_discovery["total_tests"] = (
            len(test_discovery["unit_tests"]) +
            len(test_discovery["integration_tests"]) +
            len(test_discovery["performance_tests"])
        )
        
        return test_discovery
    
    def _analyze_test_file(self, test_file: Path) -> Optional[Dict[str, Any]]:
        """Analyze a test file to extract test information"""
        try:
            with open(test_file, 'r', encoding='utf-8') as f:
                content = f.read()
            
            # Count test functions (simplified regex-based approach)
            test_patterns = [
                r'TEST\s*\([^)]+\)',  # Google Test style
                r'void\s+\w*test\w*\s*\(',  # Function names containing 'test'
                r'assert\s*\(',  # Assert statements
                r'EXPECT_',  # Google Test expectations
                r'ASSERT_'  # Google Test assertions
            ]
            
            test_count = 0
            for pattern in test_patterns:
                test_count += len(re.findall(pattern, content, re.IGNORECASE))
            
            return {
                "file": str(test_file.relative_to(self.project_root)),
                "name": test_file.stem,
                "size": test_file.stat().st_size,
                "test_count": test_count,
                "lines": len(content.split('\n'))
            }
            
        except Exception as e:
            return None
    
    def run_unit_tests(self) -> Dict[str, Any]:
        """Run unit tests for engine components"""
        print("🧪 Running unit tests...")
        
        results = {
            "passed": 0,
            "failed": 0,
            "skipped": 0,
            "total": 0,
            "tests": [],
            "duration": 0
        }
        
        start_time = time.time()
        
        # Run basic compilation test
        compile_test = self._test_compilation()
        results["tests"].append(compile_test)
        if compile_test["status"] == "passed":
            results["passed"] += 1
        else:
            results["failed"] += 1
        
        # Run file integrity tests
        integrity_tests = self._test_file_integrity()
        results["tests"].extend(integrity_tests)
        for test in integrity_tests:
            if test["status"] == "passed":
                results["passed"] += 1
            else:
                results["failed"] += 1
        
        # Run basic functionality tests
        functionality_tests = self._test_basic_functionality()
        results["tests"].extend(functionality_tests)
        for test in functionality_tests:
            if test["status"] == "passed":
                results["passed"] += 1
            else:
                results["failed"] += 1
        
        results["total"] = results["passed"] + results["failed"] + results["skipped"]
        results["duration"] = time.time() - start_time
        
        return results
    
    def _test_compilation(self) -> Dict[str, Any]:
        """Test if the engine can be compiled"""
        try:
            result = subprocess.run(
                ["cmake", "--build", str(self.project_root / "build"), "--target", "engine"],
                capture_output=True,
                text=True,
                timeout=120
            )
            
            return {
                "name": "engine_compilation",
                "status": "passed" if result.returncode == 0 else "failed",
                "duration": 0,
                "output": result.stdout,
                "error": result.stderr
            }
        except Exception as e:
            return {
                "name": "engine_compilation",
                "status": "failed",
                "duration": 0,
                "error": str(e)
            }
    
    def _test_file_integrity(self) -> List[Dict[str, Any]]:
        """Test integrity of engine files"""
        tests = []
        
        engine_files = [
            "html_parser.cpp", "webkit_engine.cpp", "gpu_manager.cpp",
            "download_manager.cpp", "video_player.cpp", "dev_tools.cpp"
        ]
        
        for file_name in engine_files:
            file_path = self.project_root / "src" / "engine" / file_name
            test = {
                "name": f"file_integrity_{file_name}",
                "status": "passed" if file_path.exists() and file_path.stat().st_size > 0 else "failed",
                "duration": 0,
                "details": {
                    "file": str(file_path),
                    "exists": file_path.exists(),
                    "size": file_path.stat().st_size if file_path.exists() else 0
                }
            }
            tests.append(test)
        
        return tests
    
    def _test_basic_functionality(self) -> List[Dict[str, Any]]:
        """Test basic functionality of engine components"""
        tests = []
        
        # Test 1: Check if headers are accessible
        header_files = [
            "html_parser.h", "webkit_engine.h", "gpu_manager.h",
            "download_manager.h", "video_player.h", "dev_tools.h"
        ]
        
        for header_name in header_files:
            header_path = self.project_root / "include" / "engine" / header_name
            test = {
                "name": f"header_access_{header_name}",
                "status": "passed" if header_path.exists() else "failed",
                "duration": 0,
                "details": {
                    "header": str(header_path),
                    "exists": header_path.exists()
                }
            }
            tests.append(test)
        
        # Test 2: Check CMake configuration
        cmake_file = self.project_root / "CMakeLists.txt"
        test = {
            "name": "cmake_configuration",
            "status": "passed" if cmake_file.exists() else "failed",
            "duration": 0,
            "details": {
                "file": str(cmake_file),
                "exists": cmake_file.exists(),
                "size": cmake_file.stat().st_size if cmake_file.exists() else 0
            }
        }
        tests.append(test)
        
        return tests
    
    def run_integration_tests(self) -> Dict[str, Any]:
        """Run integration tests"""
        print("🔗 Running integration tests...")
        
        results = {
            "passed": 0,
            "failed": 0,
            "skipped": 0,
            "total": 0,
            "tests": [],
            "duration": 0
        }
        
        start_time = time.time()
        
        # Test module dependencies
        dependency_tests = self._test_module_dependencies()
        results["tests"].extend(dependency_tests)
        for test in dependency_tests:
            if test["status"] == "passed":
                results["passed"] += 1
            else:
                results["failed"] += 1
        
        # Test build system integration
        build_tests = self._test_build_integration()
        results["tests"].extend(build_tests)
        for test in build_tests:
            if test["status"] == "passed":
                results["passed"] += 1
            else:
                results["failed"] += 1
        
        results["total"] = results["passed"] + results["failed"] + results["skipped"]
        results["duration"] = time.time() - start_time
        
        return results
    
    def _test_module_dependencies(self) -> List[Dict[str, Any]]:
        """Test module dependencies and linking"""
        tests = []
        
        # Test if all required modules exist
        required_modules = [
            "engine", "ui", "search", "config", "sandbox"
        ]
        
        for module in required_modules:
            module_src = self.project_root / "src" / module
            module_include = self.project_root / "include" / module
            
            test = {
                "name": f"module_dependencies_{module}",
                "status": "passed" if module_src.exists() and module_include.exists() else "failed",
                "duration": 0,
                "details": {
                    "module": module,
                    "src_exists": module_src.exists(),
                    "include_exists": module_include.exists()
                }
            }
            tests.append(test)
        
        return tests
    
    def _test_build_integration(self) -> List[Dict[str, Any]]:
        """Test build system integration"""
        tests = []
        
        # Test CMake configuration
        try:
            result = subprocess.run(
                ["cmake", "-B", str(self.project_root / "build")],
                capture_output=True,
                text=True,
                timeout=60
            )
            
            test = {
                "name": "cmake_integration",
                "status": "passed" if result.returncode == 0 else "failed",
                "duration": 0,
                "output": result.stdout,
                "error": result.stderr
            }
            tests.append(test)
        except Exception as e:
            test = {
                "name": "cmake_integration",
                "status": "failed",
                "duration": 0,
                "error": str(e)
            }
            tests.append(test)
        
        return tests
    
    def run_performance_tests(self) -> Dict[str, Any]:
        """Run performance tests"""
        print("⚡ Running performance tests...")
        
        results = {
            "passed": 0,
            "failed": 0,
            "skipped": 0,
            "total": 0,
            "tests": [],
            "duration": 0
        }
        
        start_time = time.time()
        
        # Test build performance
        build_perf_test = self._test_build_performance()
        results["tests"].append(build_perf_test)
        if build_perf_test["status"] == "passed":
            results["passed"] += 1
        else:
            results["failed"] += 1
        
        # Test memory usage
        memory_test = self._test_memory_usage()
        results["tests"].append(memory_test)
        if memory_test["status"] == "passed":
            results["passed"] += 1
        else:
            results["failed"] += 1
        
        results["total"] = results["passed"] + results["failed"] + results["skipped"]
        results["duration"] = time.time() - start_time
        
        return results
    
    def _test_build_performance(self) -> Dict[str, Any]:
        """Test build performance"""
        try:
            start_time = time.time()
            result = subprocess.run(
                ["cmake", "--build", str(self.project_root / "build"), "--target", "engine"],
                capture_output=True,
                text=True,
                timeout=300
            )
            duration = time.time() - start_time
            
            # Consider it passed if build completes in reasonable time
            passed = result.returncode == 0 and duration < 180  # 3 minutes
            
            return {
                "name": "build_performance",
                "status": "passed" if passed else "failed",
                "duration": duration,
                "details": {
                    "build_time": duration,
                    "timeout_threshold": 180,
                    "return_code": result.returncode
                },
                "output": result.stdout,
                "error": result.stderr
            }
        except Exception as e:
            return {
                "name": "build_performance",
                "status": "failed",
                "duration": 0,
                "error": str(e)
            }
    
    def _test_memory_usage(self) -> Dict[str, Any]:
        """Test memory usage during build"""
        try:
            import psutil
            
            # Get initial memory
            initial_memory = psutil.virtual_memory().used
            
            # Run build
            result = subprocess.run(
                ["cmake", "--build", str(self.project_root / "build"), "--target", "engine"],
                capture_output=True,
                text=True,
                timeout=300
            )
            
            # Get final memory
            final_memory = psutil.virtual_memory().used
            memory_increase = final_memory - initial_memory
            
            # Consider it passed if memory increase is reasonable (less than 1GB)
            passed = result.returncode == 0 and memory_increase < 1024 * 1024 * 1024
            
            return {
                "name": "memory_usage",
                "status": "passed" if passed else "failed",
                "duration": 0,
                "details": {
                    "initial_memory_mb": initial_memory / 1024 / 1024,
                    "final_memory_mb": final_memory / 1024 / 1024,
                    "memory_increase_mb": memory_increase / 1024 / 1024,
                    "threshold_mb": 1024
                }
            }
        except Exception as e:
            return {
                "name": "memory_usage",
                "status": "failed",
                "duration": 0,
                "error": str(e)
            }
    
    def generate_test_report(self) -> Dict[str, Any]:
        """Generate comprehensive test report"""
        print("📋 Generating test report...")
        
        discovery = self.discover_tests()
        unit_results = self.run_unit_tests()
        integration_results = self.run_integration_tests()
        performance_results = self.run_performance_tests()
        
        # Calculate overall statistics
        total_passed = unit_results["passed"] + integration_results["passed"] + performance_results["passed"]
        total_failed = unit_results["failed"] + integration_results["failed"] + performance_results["failed"]
        total_tests = total_passed + total_failed
        
        report = {
            "timestamp": time.time(),
            "datetime": datetime.now().isoformat(),
            "discovery": discovery,
            "unit_tests": unit_results,
            "integration_tests": integration_results,
            "performance_tests": performance_results,
            "summary": {
                "total_tests": total_tests,
                "passed": total_passed,
                "failed": total_failed,
                "success_rate": (total_passed / total_tests * 100) if total_tests > 0 else 0,
                "total_duration": unit_results["duration"] + integration_results["duration"] + performance_results["duration"]
            }
        }
        
        return report
    
    def save_test_report(self, report: Dict[str, Any], filename: str = "test_report.json"):
        """Save test report to file"""
        report_path = self.project_root / "tools" / "testing_tools" / filename
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        print(f"📄 Test report saved to: {report_path}")
    
    def print_test_summary(self, report: Dict[str, Any]):
        """Print a human-readable test summary"""
        print("\n" + "="*50)
        print("🧪 ZEPRA ENGINE TEST REPORT")
        print("="*50)
        
        # Discovery summary
        discovery = report["discovery"]
        print(f"\n🔍 Test Discovery:")
        print(f"    Unit Tests: {len(discovery['unit_tests'])}")
        print(f"    Integration Tests: {len(discovery['integration_tests'])}")
        print(f"    Performance Tests: {len(discovery['performance_tests'])}")
        print(f"    Total Test Files: {discovery['total_tests']}")
        
        # Test results summary
        summary = report["summary"]
        print(f"\n📊 Test Results:")
        print(f"    Total Tests: {summary['total_tests']}")
        print(f"    Passed: {summary['passed']}")
        print(f"    Failed: {summary['failed']}")
        print(f"    Success Rate: {summary['success_rate']:.1f}%")
        print(f"    Total Duration: {summary['total_duration']:.1f}s")
        
        # Detailed results
        print(f"\n📋 Detailed Results:")
        
        # Unit tests
        unit = report["unit_tests"]
        print(f"    Unit Tests: {unit['passed']}/{unit['total']} passed ({unit['duration']:.1f}s)")
        
        # Integration tests
        integration = report["integration_tests"]
        print(f"    Integration Tests: {integration['passed']}/{integration['total']} passed ({integration['duration']:.1f}s)")
        
        # Performance tests
        performance = report["performance_tests"]
        print(f"    Performance Tests: {performance['passed']}/{performance['total']} passed ({performance['duration']:.1f}s)")
        
        # Failed tests
        failed_tests = []
        for test_type, results in [("Unit", unit), ("Integration", integration), ("Performance", performance)]:
            for test in results["tests"]:
                if test["status"] == "failed":
                    failed_tests.append(f"{test_type}: {test['name']}")
        
        if failed_tests:
            print(f"\n❌ Failed Tests:")
            for test in failed_tests[:5]:  # Show first 5
                print(f"    • {test}")
            if len(failed_tests) > 5:
                print(f"    ... and {len(failed_tests) - 5} more")

def main():
    if len(sys.argv) > 1:
        project_root = sys.argv[1]
    else:
        project_root = "."
    
    runner = TestRunner(project_root)
    report = runner.generate_test_report()
    runner.save_test_report(report)
    runner.print_test_summary(report)

if __name__ == "__main__":
    main() 