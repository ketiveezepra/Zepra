#!/usr/bin/env python3
"""
Zepra Browser Engine - Build Analyzer Tool
Analyzes build performance, dependencies, and optimization opportunities
"""

import os
import sys
import json
import time
import subprocess
from pathlib import Path
from typing import Dict, List, Any

class BuildAnalyzer:
    def __init__(self, project_root: str):
        self.project_root = Path(project_root)
        self.build_dir = self.project_root / "build"
        self.results = {}
        
    def analyze_build_structure(self) -> Dict[str, Any]:
        """Analyze the current build structure and dependencies"""
        print("🔍 Analyzing build structure...")
        
        structure = {
            "engine_modules": [],
            "dependencies": {},
            "build_files": [],
            "total_size": 0
        }
        
        # Analyze engine modules
        engine_src = self.project_root / "src" / "engine"
        if engine_src.exists():
            for file in engine_src.glob("*.cpp"):
                size = file.stat().st_size
                structure["engine_modules"].append({
                    "name": file.name,
                    "size": size,
                    "path": str(file.relative_to(self.project_root))
                })
                structure["total_size"] += size
        
        # Analyze dependencies
        cmake_file = self.project_root / "CMakeLists.txt"
        if cmake_file.exists():
            structure["dependencies"]["cmake"] = {
                "exists": True,
                "size": cmake_file.stat().st_size
            }
        
        # Analyze build artifacts
        if self.build_dir.exists():
            for file in self.build_dir.rglob("*"):
                if file.is_file():
                    structure["build_files"].append({
                        "name": file.name,
                        "size": file.stat().st_size,
                        "path": str(file.relative_to(self.build_dir))
                    })
        
        return structure
    
    def measure_build_time(self) -> Dict[str, float]:
        """Measure build time for different components"""
        print("⏱️  Measuring build times...")
        
        times = {}
        
        # Measure engine build time
        start_time = time.time()
        try:
            result = subprocess.run(
                ["cmake", "--build", str(self.build_dir), "--target", "engine"],
                capture_output=True,
                text=True,
                cwd=self.project_root
            )
            engine_time = time.time() - start_time
            times["engine"] = engine_time
            times["engine_success"] = result.returncode == 0
        except Exception as e:
            times["engine"] = -1
            times["engine_error"] = str(e)
        
        return times
    
    def generate_optimization_report(self) -> Dict[str, Any]:
        """Generate optimization recommendations"""
        print("📊 Generating optimization report...")
        
        structure = self.analyze_build_structure()
        times = self.measure_build_time()
        
        recommendations = {
            "performance": [],
            "structure": [],
            "dependencies": []
        }
        
        # Performance recommendations
        if "engine" in times and times["engine"] > 30:
            recommendations["performance"].append({
                "type": "build_time",
                "issue": "Engine build time is slow",
                "suggestion": "Consider using ccache or parallel builds"
            })
        
        # Structure recommendations
        if len(structure["engine_modules"]) > 10:
            recommendations["structure"].append({
                "type": "modularity",
                "issue": "Many engine modules",
                "suggestion": "Consider grouping related modules"
            })
        
        return {
            "structure": structure,
            "times": times,
            "recommendations": recommendations
        }
    
    def save_report(self, report: Dict[str, Any], filename: str = "build_analysis.json"):
        """Save analysis report to file"""
        report_path = self.project_root / "tools" / "build_tools" / filename
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        print(f"📄 Report saved to: {report_path}")
    
    def print_summary(self, report: Dict[str, Any]):
        """Print a human-readable summary"""
        print("\n" + "="*50)
        print("🏗️  ZEPRA ENGINE BUILD ANALYSIS")
        print("="*50)
        
        # Structure summary
        structure = report["structure"]
        print(f"\n📁 Engine Modules: {len(structure['engine_modules'])}")
        print(f"📦 Total Size: {structure['total_size'] / 1024:.1f} KB")
        
        # Build times
        times = report["times"]
        if "engine" in times and times["engine"] > 0:
            print(f"⏱️  Engine Build Time: {times['engine']:.2f}s")
        
        # Recommendations
        recs = report["recommendations"]
        total_recs = sum(len(recs[key]) for key in recs)
        print(f"💡 Optimization Recommendations: {total_recs}")
        
        for category, items in recs.items():
            if items:
                print(f"\n  {category.upper()}:")
                for item in items:
                    print(f"    • {item['issue']}: {item['suggestion']}")

def main():
    if len(sys.argv) > 1:
        project_root = sys.argv[1]
    else:
        project_root = "."
    
    analyzer = BuildAnalyzer(project_root)
    report = analyzer.generate_optimization_report()
    analyzer.save_report(report)
    analyzer.print_summary(report)

if __name__ == "__main__":
    main() 