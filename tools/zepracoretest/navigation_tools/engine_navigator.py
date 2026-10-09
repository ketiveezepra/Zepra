#!/usr/bin/env python3
"""
Zepra Browser Engine - Navigation Tool
Provides navigation and exploration capabilities for the engine codebase
"""

import os
import sys
import json
import re
from pathlib import Path
from typing import Dict, List, Any, Optional
from collections import defaultdict

class EngineNavigator:
    def __init__(self, project_root: str):
        self.project_root = Path(project_root)
        self.navigation_cache = {}
        self.component_map = {}
        
    def explore_codebase(self) -> Dict[str, Any]:
        """Explore the entire codebase structure"""
        print("🗺️  Exploring codebase structure...")
        
        structure = {
            "modules": {},
            "components": {},
            "dependencies": {},
            "file_types": {},
            "total_files": 0,
            "total_lines": 0
        }
        
        # Explore source directories
        src_dirs = ["src", "include", "test", "tools", "configs"]
        
        for dir_name in src_dirs:
            dir_path = self.project_root / dir_name
            if dir_path.exists():
                structure["modules"][dir_name] = self._explore_directory(dir_path)
        
        # Build component map
        structure["components"] = self._build_component_map()
        
        # Analyze dependencies
        structure["dependencies"] = self._analyze_dependencies()
        
        # Count files and lines
        structure["file_types"] = self._count_file_types()
        structure["total_files"] = sum(len(files) for files in structure["file_types"].values())
        structure["total_lines"] = self._count_total_lines()
        
        return structure
    
    def _explore_directory(self, dir_path: Path) -> Dict[str, Any]:
        """Explore a specific directory"""
        result = {
            "files": [],
            "subdirectories": {},
            "total_files": 0,
            "total_size": 0
        }
        
        for item in dir_path.iterdir():
            if item.is_file():
                file_info = {
                    "name": item.name,
                    "path": str(item.relative_to(self.project_root)),
                    "size": item.stat().st_size,
                    "extension": item.suffix,
                    "lines": self._count_lines_in_file(item)
                }
                result["files"].append(file_info)
                result["total_files"] += 1
                result["total_size"] += file_info["size"]
            elif item.is_dir():
                result["subdirectories"][item.name] = self._explore_directory(item)
        
        return result
    
    def _count_lines_in_file(self, file_path: Path) -> int:
        """Count lines in a file"""
        try:
            with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                return len(f.readlines())
        except Exception:
            return 0
    
    def _build_component_map(self) -> Dict[str, Any]:
        """Build a map of engine components"""
        components = {
            "engine": {
                "files": [],
                "headers": [],
                "dependencies": []
            },
            "ui": {
                "files": [],
                "headers": [],
                "dependencies": []
            },
            "search": {
                "files": [],
                "headers": [],
                "dependencies": []
            },
            "config": {
                "files": [],
                "headers": [],
                "dependencies": []
            },
            "sandbox": {
                "files": [],
                "headers": [],
                "dependencies": []
            }
        }
        
        # Map engine components
        engine_src = self.project_root / "src" / "engine"
        engine_include = self.project_root / "include" / "engine"
        
        if engine_src.exists():
            for file in engine_src.glob("*.cpp"):
                components["engine"]["files"].append(str(file.relative_to(self.project_root)))
        
        if engine_include.exists():
            for file in engine_include.glob("*.h"):
                components["engine"]["headers"].append(str(file.relative_to(self.project_root)))
        
        # Map UI components
        ui_src = self.project_root / "src" / "ui"
        ui_include = self.project_root / "include" / "ui"
        
        if ui_src.exists():
            for file in ui_src.glob("*.cpp"):
                components["ui"]["files"].append(str(file.relative_to(self.project_root)))
        
        if ui_include.exists():
            for file in ui_include.glob("*.h"):
                components["ui"]["headers"].append(str(file.relative_to(self.project_root)))
        
        # Map other components similarly
        for component in ["search", "config", "sandbox"]:
            src_dir = self.project_root / "src" / component
            include_dir = self.project_root / "include" / component
            
            if src_dir.exists():
                for file in src_dir.glob("*.cpp"):
                    components[component]["files"].append(str(file.relative_to(self.project_root)))
            
            if include_dir.exists():
                for file in include_dir.glob("*.h"):
                    components[component]["headers"].append(str(file.relative_to(self.project_root)))
        
        return components
    
    def _analyze_dependencies(self) -> Dict[str, Any]:
        """Analyze dependencies between components"""
        dependencies = {
            "internal": {},
            "external": {},
            "circular": []
        }
        
        # Analyze include dependencies
        for component in ["engine", "ui", "search", "config", "sandbox"]:
            dependencies["internal"][component] = []
            dependencies["external"][component] = []
            
            include_dir = self.project_root / "include" / component
            if include_dir.exists():
                for header_file in include_dir.glob("*.h"):
                    deps = self._extract_dependencies(header_file)
                    dependencies["internal"][component].extend(deps["internal"])
                    dependencies["external"][component].extend(deps["external"])
        
        return dependencies
    
    def _extract_dependencies(self, file_path: Path) -> Dict[str, List[str]]:
        """Extract dependencies from a file"""
        deps = {
            "internal": [],
            "external": []
        }
        
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            # Find include statements
            include_pattern = r'#include\s*[<"]([^>"]+)[>"]'
            includes = re.findall(include_pattern, content)
            
            for include in includes:
                if include.startswith(('sdl', 'opengl', 'curl', 'json', 'webkit', 'std')):
                    deps["external"].append(include)
                else:
                    deps["internal"].append(include)
        
        except Exception:
            pass
        
        return deps
    
    def _count_file_types(self) -> Dict[str, List[str]]:
        """Count files by type"""
        file_types = defaultdict(list)
        
        for file_path in self.project_root.rglob("*"):
            if file_path.is_file():
                ext = file_path.suffix.lower()
                if ext:
                    file_types[ext].append(str(file_path.relative_to(self.project_root)))
        
        return dict(file_types)
    
    def _count_total_lines(self) -> int:
        """Count total lines in the codebase"""
        total_lines = 0
        
        for file_path in self.project_root.rglob("*.cpp"):
            total_lines += self._count_lines_in_file(file_path)
        
        for file_path in self.project_root.rglob("*.h"):
            total_lines += self._count_lines_in_file(file_path)
        
        return total_lines
    
    def find_component(self, component_name: str) -> Dict[str, Any]:
        """Find a specific component and its related files"""
        print(f"🔍 Finding component: {component_name}")
        
        result = {
            "component": component_name,
            "source_files": [],
            "header_files": [],
            "dependencies": [],
            "related_files": []
        }
        
        # Search in source directories
        src_dirs = ["src", "include"]
        
        for dir_name in src_dirs:
            dir_path = self.project_root / dir_name
            if dir_path.exists():
                for file_path in dir_path.rglob(f"*{component_name}*"):
                    if file_path.is_file():
                        if file_path.suffix == ".cpp":
                            result["source_files"].append(str(file_path.relative_to(self.project_root)))
                        elif file_path.suffix == ".h":
                            result["header_files"].append(str(file_path.relative_to(self.project_root)))
                        else:
                            result["related_files"].append(str(file_path.relative_to(self.project_root)))
        
        return result
    
    def search_codebase(self, query: str) -> Dict[str, Any]:
        """Search the codebase for specific terms"""
        print(f"🔍 Searching for: {query}")
        
        results = {
            "query": query,
            "files": [],
            "matches": [],
            "total_matches": 0
        }
        
        # Search in source files
        for file_path in self.project_root.rglob("*.cpp"):
            matches = self._search_in_file(file_path, query)
            if matches:
                results["files"].append(str(file_path.relative_to(self.project_root)))
                results["matches"].extend(matches)
        
        for file_path in self.project_root.rglob("*.h"):
            matches = self._search_in_file(file_path, query)
            if matches:
                results["files"].append(str(file_path.relative_to(self.project_root)))
                results["matches"].extend(matches)
        
        results["total_matches"] = len(results["matches"])
        
        return results
    
    def _search_in_file(self, file_path: Path, query: str) -> List[Dict[str, Any]]:
        """Search for a query in a specific file"""
        matches = []
        
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                lines = f.readlines()
            
            for i, line in enumerate(lines, 1):
                if query.lower() in line.lower():
                    matches.append({
                        "file": str(file_path.relative_to(self.project_root)),
                        "line": i,
                        "content": line.strip(),
                        "context": "".join(lines[max(0, i-2):min(len(lines), i+1)])
                    })
        
        except Exception:
            pass
        
        return matches
    
    def get_file_hierarchy(self, path: str = "") -> Dict[str, Any]:
        """Get the file hierarchy starting from a specific path"""
        start_path = self.project_root / path if path else self.project_root
        
        if not start_path.exists():
            return {"error": f"Path not found: {path}"}
        
        hierarchy = {
            "path": str(start_path.relative_to(self.project_root)),
            "type": "directory" if start_path.is_dir() else "file",
            "children": []
        }
        
        if start_path.is_dir():
            for item in start_path.iterdir():
                if item.is_dir():
                    hierarchy["children"].append(self.get_file_hierarchy(str(item.relative_to(self.project_root))))
                else:
                    hierarchy["children"].append({
                        "path": str(item.relative_to(self.project_root)),
                        "type": "file",
                        "size": item.stat().st_size,
                        "extension": item.suffix
                    })
        
        return hierarchy
    
    def generate_navigation_report(self) -> Dict[str, Any]:
        """Generate comprehensive navigation report"""
        print("📋 Generating navigation report...")
        import time
        
        report = {
            "timestamp": time.time(),
            "exploration": self.explore_codebase(),
            "component_overview": self._generate_component_overview(),
            "file_statistics": self._generate_file_statistics()
        }
        
        return report
    
    def _generate_component_overview(self) -> Dict[str, Any]:
        """Generate component overview"""
        components = self._build_component_map()
        
        overview = {}
        for component, data in components.items():
            overview[component] = {
                "source_files": len(data["files"]),
                "header_files": len(data["headers"]),
                "total_files": len(data["files"]) + len(data["headers"])
            }
        
        return overview
    
    def _generate_file_statistics(self) -> Dict[str, Any]:
        """Generate file statistics"""
        file_types = self._count_file_types()
        
        stats = {
            "by_extension": {},
            "total_files": 0,
            "largest_files": []
        }
        
        for ext, files in file_types.items():
            stats["by_extension"][ext] = len(files)
            stats["total_files"] += len(files)
        
        # Find largest files
        all_files = []
        for file_path in self.project_root.rglob("*"):
            if file_path.is_file():
                all_files.append((file_path, file_path.stat().st_size))
        
        all_files.sort(key=lambda x: x[1], reverse=True)
        stats["largest_files"] = [
            {
                "path": str(file.relative_to(self.project_root)),
                "size": size,
                "size_mb": size / (1024 * 1024)
            }
            for file, size in all_files[:10]
        ]
        
        return stats
    
    def save_navigation_report(self, report: Dict[str, Any], filename: str = "navigation_report.json"):
        """Save navigation report to file"""
        report_path = self.project_root / "tools" / "navigation_tools" / filename
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        print(f"📄 Navigation report saved to: {report_path}")
    
    def print_navigation_summary(self, report: Dict[str, Any]):
        """Print a human-readable navigation summary"""
        print("\n" + "="*50)
        print("🗺️  ZEPRA ENGINE NAVIGATION REPORT")
        print("="*50)
        
        # Exploration summary
        exploration = report["exploration"]
        print(f"\n📁 Codebase Structure:")
        print(f"    Total Files: {exploration['total_files']}")
        print(f"    Total Lines: {exploration['total_lines']}")
        print(f"    Modules: {len(exploration['modules'])}")
        
        # Component overview
        component_overview = report["component_overview"]
        print(f"\n🔧 Components:")
        for component, stats in component_overview.items():
            print(f"    {component}: {stats['total_files']} files ({stats['source_files']} source, {stats['header_files']} headers)")
        
        # File statistics
        file_stats = report["file_statistics"]
        print(f"\n📊 File Statistics:")
        print(f"    Total Files: {file_stats['total_files']}")
        print(f"    File Types: {len(file_stats['by_extension'])}")
        
        # Largest files
        print(f"\n📦 Largest Files:")
        for file_info in file_stats["largest_files"][:5]:
            print(f"    {file_info['path']}: {file_info['size_mb']:.1f} MB")

def main():
    import time
    
    if len(sys.argv) > 1:
        project_root = sys.argv[1]
    else:
        project_root = "."
    
    navigator = EngineNavigator(project_root)
    report = navigator.generate_navigation_report()
    navigator.save_navigation_report(report)
    navigator.print_navigation_summary(report)

if __name__ == "__main__":
    main() 