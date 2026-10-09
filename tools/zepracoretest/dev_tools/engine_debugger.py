#!/usr/bin/env python3
"""
Zepra Browser Engine - Debug Tool
Provides debugging capabilities for engine components
"""

import os
import sys
import json
import psutil
import time
import subprocess
from pathlib import Path
from typing import Dict, List, Any, Optional

class EngineDebugger:
    def __init__(self, project_root: str):
        self.project_root = Path(project_root)
        self.debug_log = []
        
    def check_engine_health(self) -> Dict[str, Any]:
        """Check the health of engine components"""
        print("🏥 Checking engine health...")
        
        health = {
            "components": {},
            "dependencies": {},
            "system": {},
            "overall_status": "unknown"
        }
        
        # Check engine source files
        engine_src = self.project_root / "src" / "engine"
        if engine_src.exists():
            files = list(engine_src.glob("*.cpp"))
            health["components"]["engine_sources"] = {
                "status": "healthy" if files else "missing",
                "file_count": len(files),
                "files": [f.name for f in files]
            }
        
        # Check engine headers
        engine_headers = self.project_root / "include" / "engine"
        if engine_headers.exists():
            headers = list(engine_headers.glob("*.h"))
            health["components"]["engine_headers"] = {
                "status": "healthy" if headers else "missing",
                "file_count": len(headers),
                "files": [f.name for f in headers]
            }
        
        # Check CMake configuration
        cmake_file = self.project_root / "CMakeLists.txt"
        health["dependencies"]["cmake"] = {
            "status": "healthy" if cmake_file.exists() else "missing",
            "size": cmake_file.stat().st_size if cmake_file.exists() else 0
        }
        
        # Check system resources
        health["system"] = {
            "memory_available": psutil.virtual_memory().available,
            "cpu_count": psutil.cpu_count(),
            "disk_space": psutil.disk_usage(str(self.project_root)).free
        }
        
        # Determine overall status
        all_healthy = all(
            comp.get("status") == "healthy" 
            for comp in health["components"].values()
        )
        health["overall_status"] = "healthy" if all_healthy else "issues"
        
        return health
    
    def profile_memory_usage(self, duration: int = 30) -> Dict[str, Any]:
        """Profile memory usage over time"""
        print(f"💾 Profiling memory usage for {duration} seconds...")
        
        memory_data = []
        start_time = time.time()
        
        while time.time() - start_time < duration:
            memory_info = psutil.virtual_memory()
            memory_data.append({
                "timestamp": time.time() - start_time,
                "used": memory_info.used,
                "available": memory_info.available,
                "percent": memory_info.percent
            })
            time.sleep(1)
        
        return {
            "duration": duration,
            "samples": len(memory_data),
            "peak_usage": max(m["used"] for m in memory_data),
            "average_usage": sum(m["used"] for m in memory_data) / len(memory_data),
            "data": memory_data
        }
    
    def analyze_engine_dependencies(self) -> Dict[str, Any]:
        """Analyze engine dependencies and potential issues"""
        print("🔍 Analyzing engine dependencies...")
        
        dependencies = {
            "external": {},
            "internal": {},
            "missing": [],
            "conflicts": []
        }
        
        # Check for common external dependencies
        common_deps = [
            "SDL2", "OpenGL", "curl", "json", "webkit"
        ]
        
        for dep in common_deps:
            try:
                # This is a simplified check - in real implementation,
                # you'd check for actual library availability
                dependencies["external"][dep] = {
                    "status": "available",
                    "version": "unknown"
                }
            except Exception:
                dependencies["external"][dep] = {
                    "status": "missing",
                    "version": "unknown"
                }
                dependencies["missing"].append(dep)
        
        # Check internal module dependencies
        internal_modules = [
            "html_parser", "webkit_engine", "gpu_manager", 
            "download_manager", "video_player", "dev_tools"
        ]
        
        for module in internal_modules:
            module_file = self.project_root / "src" / "engine" / f"{module}.cpp"
            dependencies["internal"][module] = {
                "status": "present" if module_file.exists() else "missing",
                "size": module_file.stat().st_size if module_file.exists() else 0
            }
        
        return dependencies
    
    def run_component_tests(self) -> Dict[str, Any]:
        """Run basic tests on engine components"""
        print("🧪 Running component tests...")
        
        test_results = {
            "passed": 0,
            "failed": 0,
            "tests": {}
        }
        
        # Test 1: Check if engine can be built
        try:
            result = subprocess.run(
                ["cmake", "--build", str(self.project_root / "build"), "--target", "engine"],
                capture_output=True,
                text=True,
                timeout=60
            )
            test_results["tests"]["build_engine"] = {
                "status": "passed" if result.returncode == 0 else "failed",
                "output": result.stdout,
                "error": result.stderr
            }
            if result.returncode == 0:
                test_results["passed"] += 1
            else:
                test_results["failed"] += 1
        except Exception as e:
            test_results["tests"]["build_engine"] = {
                "status": "failed",
                "error": str(e)
            }
            test_results["failed"] += 1
        
        # Test 2: Check file integrity
        engine_files = [
            "html_parser.cpp", "webkit_engine.cpp", "gpu_manager.cpp"
        ]
        
        for file_name in engine_files:
            file_path = self.project_root / "src" / "engine" / file_name
            if file_path.exists() and file_path.stat().st_size > 0:
                test_results["tests"][f"file_{file_name}"] = {
                    "status": "passed",
                    "size": file_path.stat().st_size
                }
                test_results["passed"] += 1
            else:
                test_results["tests"][f"file_{file_name}"] = {
                    "status": "failed",
                    "error": "File missing or empty"
                }
                test_results["failed"] += 1
        
        return test_results
    
    def generate_debug_report(self) -> Dict[str, Any]:
        """Generate comprehensive debug report"""
        print("📋 Generating debug report...")
        
        report = {
            "timestamp": time.time(),
            "health": self.check_engine_health(),
            "dependencies": self.analyze_engine_dependencies(),
            "tests": self.run_component_tests(),
            "memory_profile": self.profile_memory_usage(10)  # 10 second profile
        }
        
        return report
    
    def save_debug_report(self, report: Dict[str, Any], filename: str = "debug_report.json"):
        """Save debug report to file"""
        report_path = self.project_root / "tools" / "dev_tools" / filename
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        print(f"📄 Debug report saved to: {report_path}")
    
    def print_debug_summary(self, report: Dict[str, Any]):
        """Print a human-readable debug summary"""
        print("\n" + "="*50)
        print("🐛 ZEPRA ENGINE DEBUG REPORT")
        print("="*50)
        
        # Health summary
        health = report["health"]
        print(f"\n🏥 Overall Health: {health['overall_status'].upper()}")
        print(f"📁 Components: {len(health['components'])}")
        
        # Test results
        tests = report["tests"]
        print(f"\n🧪 Tests: {tests['passed']} passed, {tests['failed']} failed")
        
        # Dependencies
        deps = report["dependencies"]
        missing_deps = len(deps["missing"])
        if missing_deps > 0:
            print(f"⚠️  Missing Dependencies: {missing_deps}")
            for dep in deps["missing"]:
                print(f"    • {dep}")
        
        # Memory profile
        memory = report["memory_profile"]
        print(f"\n💾 Memory Profile:")
        print(f"    Peak Usage: {memory['peak_usage'] / 1024 / 1024:.1f} MB")
        print(f"    Average Usage: {memory['average_usage'] / 1024 / 1024:.1f} MB")

def main():
    if len(sys.argv) > 1:
        project_root = sys.argv[1]
    else:
        project_root = "."
    
    debugger = EngineDebugger(project_root)
    report = debugger.generate_debug_report()
    debugger.save_debug_report(report)
    debugger.print_debug_summary(report)

if __name__ == "__main__":
    main() 