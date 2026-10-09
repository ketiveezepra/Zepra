#!/usr/bin/env python3
"""
Zepra Browser Engine - Analysis Tool
Analyzes code complexity, performance, and provides optimization insights
"""

import os
import sys
import json
import re
import ast
from pathlib import Path
from typing import Dict, List, Any, Tuple
from collections import defaultdict

class EngineAnalyzer:
    def __init__(self, project_root: str):
        self.project_root = Path(project_root)
        self.analysis_results = {}
        
    def analyze_code_complexity(self) -> Dict[str, Any]:
        """Analyze code complexity metrics"""
        print("📊 Analyzing code complexity...")
        
        complexity = {
            "files": {},
            "summary": {
                "total_lines": 0,
                "total_functions": 0,
                "average_complexity": 0,
                "high_complexity_files": []
            }
        }
        
        engine_src = self.project_root / "src" / "engine"
        if not engine_src.exists():
            return complexity
        
        total_complexity = 0
        file_count = 0
        
        for cpp_file in engine_src.glob("*.cpp"):
            file_analysis = self._analyze_cpp_file(cpp_file)
            complexity["files"][cpp_file.name] = file_analysis
            
            total_complexity += file_analysis["complexity_score"]
            complexity["summary"]["total_lines"] += file_analysis["lines"]
            complexity["summary"]["total_functions"] += file_analysis["functions"]
            file_count += 1
            
            if file_analysis["complexity_score"] > 50:
                complexity["summary"]["high_complexity_files"].append(cpp_file.name)
        
        if file_count > 0:
            complexity["summary"]["average_complexity"] = total_complexity / file_count
        
        return complexity
    
    def _analyze_cpp_file(self, file_path: Path) -> Dict[str, Any]:
        """Analyze a single C++ file for complexity"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            lines = content.split('\n')
            line_count = len(lines)
            
            # Count functions (simplified regex-based approach)
            function_pattern = r'\w+\s+\w+\s*\([^)]*\)\s*\{'
            functions = re.findall(function_pattern, content)
            function_count = len(functions)
            
            # Calculate complexity score based on various factors
            complexity_score = 0
            
            # Factor 1: Lines of code
            complexity_score += line_count * 0.1
            
            # Factor 2: Number of functions
            complexity_score += function_count * 2
            
            # Factor 3: Nested structures
            nested_levels = self._count_nested_levels(content)
            complexity_score += nested_levels * 5
            
            # Factor 4: Comments ratio
            comment_lines = len([line for line in lines if line.strip().startswith('//') or line.strip().startswith('/*')])
            comment_ratio = comment_lines / line_count if line_count > 0 else 0
            complexity_score -= comment_ratio * 10  # Comments reduce complexity
            
            return {
                "lines": line_count,
                "functions": function_count,
                "complexity_score": max(0, complexity_score),
                "nested_levels": nested_levels,
                "comment_ratio": comment_ratio,
                "size_kb": file_path.stat().st_size / 1024
            }
            
        except Exception as e:
            return {
                "lines": 0,
                "functions": 0,
                "complexity_score": 0,
                "error": str(e)
            }
    
    def _count_nested_levels(self, content: str) -> int:
        """Count maximum nesting levels in code"""
        max_level = 0
        current_level = 0
        
        for char in content:
            if char == '{':
                current_level += 1
                max_level = max(max_level, current_level)
            elif char == '}':
                current_level = max(0, current_level - 1)
        
        return max_level
    
    def analyze_performance_patterns(self) -> Dict[str, Any]:
        """Analyze code for performance patterns and potential bottlenecks"""
        print("⚡ Analyzing performance patterns...")
        
        patterns = {
            "bottlenecks": [],
            "optimizations": [],
            "memory_patterns": [],
            "algorithm_complexity": {}
        }
        
        engine_src = self.project_root / "src" / "engine"
        if not engine_src.exists():
            return patterns
        
        for cpp_file in engine_src.glob("*.cpp"):
            file_patterns = self._analyze_performance_patterns_in_file(cpp_file)
            
            patterns["bottlenecks"].extend(file_patterns["bottlenecks"])
            patterns["optimizations"].extend(file_patterns["optimizations"])
            patterns["memory_patterns"].extend(file_patterns["memory_patterns"])
            
            if file_patterns["algorithm_complexity"]:
                patterns["algorithm_complexity"][cpp_file.name] = file_patterns["algorithm_complexity"]
        
        return patterns
    
    def _analyze_performance_patterns_in_file(self, file_path: Path) -> Dict[str, Any]:
        """Analyze performance patterns in a single file"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            patterns = {
                "bottlenecks": [],
                "optimizations": [],
                "memory_patterns": [],
                "algorithm_complexity": {}
            }
            
            lines = content.split('\n')
            
            for i, line in enumerate(lines, 1):
                line_lower = line.lower()
                
                # Check for potential bottlenecks
                if any(pattern in line_lower for pattern in [
                    "for (", "while (", "nested", "recursive", "deep copy"
                ]):
                    patterns["bottlenecks"].append({
                        "file": file_path.name,
                        "line": i,
                        "pattern": "loop_or_recursion",
                        "code": line.strip()
                    })
                
                # Check for memory patterns
                if any(pattern in line_lower for pattern in [
                    "new ", "malloc", "delete", "free", "vector", "map", "set"
                ]):
                    patterns["memory_patterns"].append({
                        "file": file_path.name,
                        "line": i,
                        "pattern": "memory_operation",
                        "code": line.strip()
                    })
                
                # Check for optimization opportunities
                if any(pattern in line_lower for pattern in [
                    "const ", "inline", "static", "reference", "move"
                ]):
                    patterns["optimizations"].append({
                        "file": file_path.name,
                        "line": i,
                        "pattern": "optimization_opportunity",
                        "code": line.strip()
                    })
            
            return patterns
            
        except Exception as e:
            return {
                "bottlenecks": [],
                "optimizations": [],
                "memory_patterns": [],
                "algorithm_complexity": {},
                "error": str(e)
            }
    
    def analyze_dependencies(self) -> Dict[str, Any]:
        """Analyze module dependencies and coupling"""
        print("🔗 Analyzing dependencies...")
        
        dependencies = {
            "modules": {},
            "coupling": {},
            "circular_deps": [],
            "external_deps": {}
        }
        
        engine_src = self.project_root / "src" / "engine"
        if not engine_src.exists():
            return dependencies
        
        # Analyze each module
        for cpp_file in engine_src.glob("*.cpp"):
            module_name = cpp_file.stem
            module_deps = self._analyze_module_dependencies(cpp_file)
            dependencies["modules"][module_name] = module_deps
        
        # Analyze coupling between modules
        dependencies["coupling"] = self._analyze_module_coupling(dependencies["modules"])
        
        return dependencies
    
    def _analyze_module_dependencies(self, file_path: Path) -> Dict[str, Any]:
        """Analyze dependencies for a single module"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            deps = {
                "includes": [],
                "external_libs": [],
                "internal_modules": [],
                "complexity": 0
            }
            
            # Extract include statements
            include_pattern = r'#include\s*[<"]([^>"]+)[>"]'
            includes = re.findall(include_pattern, content)
            deps["includes"] = includes
            
            # Categorize includes
            for include in includes:
                if include.startswith(('sdl', 'opengl', 'curl', 'json', 'webkit')):
                    deps["external_libs"].append(include)
                elif include.endswith('.h'):
                    deps["internal_modules"].append(include)
            
            # Calculate dependency complexity
            deps["complexity"] = len(deps["includes"]) + len(deps["external_libs"]) * 2
            
            return deps
            
        except Exception as e:
            return {
                "includes": [],
                "external_libs": [],
                "internal_modules": [],
                "complexity": 0,
                "error": str(e)
            }
    
    def _analyze_module_coupling(self, modules: Dict[str, Any]) -> Dict[str, Any]:
        """Analyze coupling between modules"""
        coupling = {
            "high_coupling": [],
            "low_coupling": [],
            "coupling_matrix": {}
        }
        
        module_names = list(modules.keys())
        
        for i, module1 in enumerate(module_names):
            coupling["coupling_matrix"][module1] = {}
            for module2 in module_names:
                if module1 == module2:
                    coupling["coupling_matrix"][module1][module2] = 0
                else:
                    # Calculate coupling based on shared dependencies
                    deps1 = set(modules[module1].get("internal_modules", []))
                    deps2 = set(modules[module2].get("internal_modules", []))
                    shared_deps = len(deps1.intersection(deps2))
                    coupling["coupling_matrix"][module1][module2] = shared_deps
        
        return coupling
    
    def generate_optimization_recommendations(self) -> Dict[str, Any]:
        """Generate optimization recommendations based on analysis"""
        print("💡 Generating optimization recommendations...")
        
        complexity = self.analyze_code_complexity()
        patterns = self.analyze_performance_patterns()
        dependencies = self.analyze_dependencies()
        
        recommendations = {
            "high_priority": [],
            "medium_priority": [],
            "low_priority": [],
            "summary": {
                "total_recommendations": 0,
                "estimated_impact": "medium"
            }
        }
        
        # High priority recommendations
        if complexity["summary"]["high_complexity_files"]:
            recommendations["high_priority"].append({
                "type": "complexity",
                "issue": f"High complexity files: {', '.join(complexity['summary']['high_complexity_files'])}",
                "suggestion": "Consider refactoring into smaller, more focused modules",
                "impact": "high"
            })
        
        if patterns["bottlenecks"]:
            recommendations["high_priority"].append({
                "type": "performance",
                "issue": f"Found {len(patterns['bottlenecks'])} potential performance bottlenecks",
                "suggestion": "Review loops and recursive calls for optimization opportunities",
                "impact": "high"
            })
        
        # Medium priority recommendations
        if patterns["memory_patterns"]:
            recommendations["medium_priority"].append({
                "type": "memory",
                "issue": f"Found {len(patterns['memory_patterns'])} memory operations",
                "suggestion": "Review memory allocation patterns and consider smart pointers",
                "impact": "medium"
            })
        
        # Low priority recommendations
        if patterns["optimizations"]:
            recommendations["low_priority"].append({
                "type": "optimization",
                "issue": f"Found {len(patterns['optimizations'])} optimization opportunities",
                "suggestion": "Consider adding const, inline, and move semantics where appropriate",
                "impact": "low"
            })
        
        recommendations["summary"]["total_recommendations"] = (
            len(recommendations["high_priority"]) +
            len(recommendations["medium_priority"]) +
            len(recommendations["low_priority"])
        )
        
        return recommendations
    
    def generate_analysis_report(self) -> Dict[str, Any]:
        """Generate comprehensive analysis report"""
        print("📋 Generating analysis report...")
        
        report = {
            "timestamp": time.time(),
            "complexity": self.analyze_code_complexity(),
            "performance": self.analyze_performance_patterns(),
            "dependencies": self.analyze_dependencies(),
            "recommendations": self.generate_optimization_recommendations()
        }
        
        return report
    
    def save_analysis_report(self, report: Dict[str, Any], filename: str = "analysis_report.json"):
        """Save analysis report to file"""
        report_path = self.project_root / "tools" / "analysis_tools" / filename
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        print(f"📄 Analysis report saved to: {report_path}")
    
    def print_analysis_summary(self, report: Dict[str, Any]):
        """Print a human-readable analysis summary"""
        print("\n" + "="*50)
        print("📊 ZEPRA ENGINE ANALYSIS REPORT")
        print("="*50)
        
        # Complexity summary
        complexity = report["complexity"]
        print(f"\n📊 Code Complexity:")
        print(f"    Total Lines: {complexity['summary']['total_lines']}")
        print(f"    Total Functions: {complexity['summary']['total_functions']}")
        print(f"    Average Complexity: {complexity['summary']['average_complexity']:.1f}")
        
        if complexity["summary"]["high_complexity_files"]:
            print(f"    ⚠️  High Complexity Files: {len(complexity['summary']['high_complexity_files'])}")
        
        # Performance summary
        performance = report["performance"]
        print(f"\n⚡ Performance Patterns:")
        print(f"    Bottlenecks Found: {len(performance['bottlenecks'])}")
        print(f"    Optimization Opportunities: {len(performance['optimizations'])}")
        print(f"    Memory Operations: {len(performance['memory_patterns'])}")
        
        # Recommendations summary
        recs = report["recommendations"]
        print(f"\n💡 Recommendations:")
        print(f"    High Priority: {len(recs['high_priority'])}")
        print(f"    Medium Priority: {len(recs['medium_priority'])}")
        print(f"    Low Priority: {len(recs['low_priority'])}")
        print(f"    Total: {recs['summary']['total_recommendations']}")

def main():
    if len(sys.argv) > 1:
        project_root = sys.argv[1]
    else:
        project_root = "."
    
    analyzer = EngineAnalyzer(project_root)
    report = analyzer.generate_analysis_report()
    analyzer.save_analysis_report(report)
    analyzer.print_analysis_summary(report)

if __name__ == "__main__":
    main() 