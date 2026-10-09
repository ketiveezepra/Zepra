#!/usr/bin/env python3
"""
Zepra Browser Engine - Information Extractor
Extracts metadata, documentation, and insights from the engine codebase
"""

import os
import sys
import json
import re
import configparser
from pathlib import Path
from typing import Dict, List, Any, Optional
from datetime import datetime

class InfoExtractor:
    def __init__(self, project_root: str):
        self.project_root = Path(project_root)
        self.extracted_data = {}
        
    def extract_config_info(self) -> Dict[str, Any]:
        """Extract information from configuration files"""
        print("⚙️  Extracting configuration information...")
        
        config_info = {
            "ncf_config": {},
            "tie_config": {},
            "cmake_config": {},
            "summary": {}
        }
        
        # Extract NCF configuration
        ncf_file = self.project_root / "configs" / "system.ncf"
        if ncf_file.exists():
            config_info["ncf_config"] = self._parse_ncf_file(ncf_file)
        
        # Extract TIE configuration
        tie_file = self.project_root / "configs" / "zepra_browser.tie"
        if tie_file.exists():
            config_info["tie_config"] = self._parse_tie_file(tie_file)
        
        # Extract CMake configuration
        cmake_file = self.project_root / "CMakeLists.txt"
        if cmake_file.exists():
            config_info["cmake_config"] = self._parse_cmake_file(cmake_file)
        
        # Generate summary
        config_info["summary"] = self._generate_config_summary(config_info)
        
        return config_info
    
    def _parse_ncf_file(self, file_path: Path) -> Dict[str, Any]:
        """Parse NCF (INI-style) configuration file"""
        try:
            config = configparser.ConfigParser()
            config.read(file_path)
            
            parsed_config = {}
            for section in config.sections():
                parsed_config[section] = dict(config[section])
            
            return {
                "sections": list(config.sections()),
                "data": parsed_config,
                "file_size": file_path.stat().st_size,
                "last_modified": datetime.fromtimestamp(file_path.stat().st_mtime).isoformat()
            }
        except Exception as e:
            return {"error": str(e)}
    
    def _parse_tie_file(self, file_path: Path) -> Dict[str, Any]:
        """Parse TIE (JSON-style) configuration file"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            # Remove comments for JSON parsing
            content = re.sub(r'//.*$', '', content, flags=re.MULTILINE)
            
            parsed_config = json.loads(content)
            
            return {
                "sections": list(parsed_config.keys()),
                "data": parsed_config,
                "file_size": file_path.stat().st_size,
                "last_modified": datetime.fromtimestamp(file_path.stat().st_mtime).isoformat()
            }
        except Exception as e:
            return {"error": str(e)}
    
    def _parse_cmake_file(self, file_path: Path) -> Dict[str, Any]:
        """Parse CMakeLists.txt file"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            cmake_info = {
                "project_name": "",
                "cmake_version": "",
                "targets": [],
                "dependencies": [],
                "file_size": file_path.stat().st_size,
                "last_modified": datetime.fromtimestamp(file_path.stat().st_mtime).isoformat()
            }
            
            # Extract project name
            project_match = re.search(r'project\s*\(\s*(\w+)', content)
            if project_match:
                cmake_info["project_name"] = project_match.group(1)
            
            # Extract CMake version
            version_match = re.search(r'cmake_minimum_required\s*\(\s*VERSION\s+([\d.]+)', content)
            if version_match:
                cmake_info["cmake_version"] = version_match.group(1)
            
            # Extract targets
            target_matches = re.findall(r'add_executable\s*\(\s*(\w+)', content)
            cmake_info["targets"].extend(target_matches)
            
            target_lib_matches = re.findall(r'add_library\s*\(\s*(\w+)', content)
            cmake_info["targets"].extend(target_lib_matches)
            
            # Extract dependencies
            find_package_matches = re.findall(r'find_package\s*\(\s*(\w+)', content)
            cmake_info["dependencies"].extend(find_package_matches)
            
            return cmake_info
            
        except Exception as e:
            return {"error": str(e)}
    
    def _generate_config_summary(self, config_info: Dict[str, Any]) -> Dict[str, Any]:
        """Generate summary of configuration information"""
        summary = {
            "total_config_files": 0,
            "config_types": [],
            "key_settings": {},
            "dependencies": set()
        }
        
        # Count config files
        for config_type, data in config_info.items():
            if config_type != "summary" and data and "error" not in data:
                summary["total_config_files"] += 1
                summary["config_types"].append(config_type)
        
        # Extract key settings from NCF
        if "ncf_config" in config_info and "data" in config_info["ncf_config"]:
            ncf_data = config_info["ncf_config"]["data"]
            if "system" in ncf_data:
                summary["key_settings"]["system_name"] = ncf_data["system"].get("name", "")
                summary["key_settings"]["system_version"] = ncf_data["system"].get("version", "")
            
            if "development" in ncf_data:
                summary["key_settings"]["sdk_path"] = ncf_data["development"].get("sdk_path", "")
        
        # Extract key settings from TIE
        if "tie_config" in config_info and "data" in config_info["tie_config"]:
            tie_data = config_info["tie_config"]["data"]
            if "app_info" in tie_data:
                summary["key_settings"]["app_name"] = tie_data["app_info"].get("name", "")
                summary["key_settings"]["app_version"] = tie_data["app_info"].get("version", "")
            
            if "dependencies" in tie_data:
                deps = tie_data["dependencies"]
                if "system" in deps:
                    summary["dependencies"].update(deps["system"])
                if "development" in deps:
                    summary["dependencies"].update(deps["development"])
        
        # Extract dependencies from CMake
        if "cmake_config" in config_info and "dependencies" in config_info["cmake_config"]:
            summary["dependencies"].update(config_info["cmake_config"]["dependencies"])
        
        summary["dependencies"] = list(summary["dependencies"])
        
        return summary
    
    def extract_code_metadata(self) -> Dict[str, Any]:
        """Extract metadata from source code files"""
        print("📄 Extracting code metadata...")
        
        metadata = {
            "functions": [],
            "classes": [],
            "includes": [],
            "defines": [],
            "comments": [],
            "statistics": {}
        }
        
        # Process source files
        for file_path in self.project_root.rglob("*.cpp"):
            file_metadata = self._extract_file_metadata(file_path)
            metadata["functions"].extend(file_metadata["functions"])
            metadata["classes"].extend(file_metadata["classes"])
            metadata["includes"].extend(file_metadata["includes"])
            metadata["defines"].extend(file_metadata["defines"])
            metadata["comments"].extend(file_metadata["comments"])
        
        # Process header files
        for file_path in self.project_root.rglob("*.h"):
            file_metadata = self._extract_file_metadata(file_path)
            metadata["functions"].extend(file_metadata["functions"])
            metadata["classes"].extend(file_metadata["classes"])
            metadata["includes"].extend(file_metadata["includes"])
            metadata["defines"].extend(file_metadata["defines"])
            metadata["comments"].extend(file_metadata["comments"])
        
        # Generate statistics
        metadata["statistics"] = {
            "total_functions": len(metadata["functions"]),
            "total_classes": len(metadata["classes"]),
            "total_includes": len(set(metadata["includes"])),
            "total_defines": len(set(metadata["defines"])),
            "total_comments": len(metadata["comments"])
        }
        
        return metadata
    
    def _extract_file_metadata(self, file_path: Path) -> Dict[str, List[Any]]:
        """Extract metadata from a single file"""
        metadata = {
            "functions": [],
            "classes": [],
            "includes": [],
            "defines": [],
            "comments": []
        }
        
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            lines = content.split('\n')
            
            # Extract functions
            function_pattern = r'(\w+\s+)?(\w+)\s+(\w+)\s*\([^)]*\)\s*\{?'
            for i, line in enumerate(lines, 1):
                match = re.search(function_pattern, line)
                if match:
                    metadata["functions"].append({
                        "file": str(file_path.relative_to(self.project_root)),
                        "line": i,
                        "return_type": match.group(1).strip() if match.group(1) else "",
                        "function_name": match.group(3),
                        "signature": line.strip()
                    })
            
            # Extract classes
            class_pattern = r'class\s+(\w+)'
            for i, line in enumerate(lines, 1):
                match = re.search(class_pattern, line)
                if match:
                    metadata["classes"].append({
                        "file": str(file_path.relative_to(self.project_root)),
                        "line": i,
                        "class_name": match.group(1),
                        "definition": line.strip()
                    })
            
            # Extract includes
            include_pattern = r'#include\s*[<"]([^>"]+)[>"]'
            includes = re.findall(include_pattern, content)
            for include in includes:
                metadata["includes"].append({
                    "file": str(file_path.relative_to(self.project_root)),
                    "include": include
                })
            
            # Extract defines
            define_pattern = r'#define\s+(\w+)'
            defines = re.findall(define_pattern, content)
            for define in defines:
                metadata["defines"].append({
                    "file": str(file_path.relative_to(self.project_root)),
                    "define": define
                })
            
            # Extract comments
            comment_pattern = r'//(.+)'
            for i, line in enumerate(lines, 1):
                match = re.search(comment_pattern, line)
                if match:
                    metadata["comments"].append({
                        "file": str(file_path.relative_to(self.project_root)),
                        "line": i,
                        "comment": match.group(1).strip()
                    })
        
        except Exception:
            pass
        
        return metadata
    
    def extract_documentation(self) -> Dict[str, Any]:
        """Extract documentation from the codebase"""
        print("📚 Extracting documentation...")
        
        documentation = {
            "readme_files": [],
            "markdown_files": [],
            "docstrings": [],
            "api_docs": [],
            "examples": []
        }
        
        # Find README files
        for file_path in self.project_root.rglob("README*"):
            if file_path.is_file():
                doc_info = self._extract_documentation_file(file_path)
                documentation["readme_files"].append(doc_info)
        
        # Find markdown files
        for file_path in self.project_root.rglob("*.md"):
            if file_path.is_file():
                doc_info = self._extract_documentation_file(file_path)
                documentation["markdown_files"].append(doc_info)
        
        # Extract docstrings from source files
        for file_path in self.project_root.rglob("*.cpp"):
            docstrings = self._extract_docstrings(file_path)
            documentation["docstrings"].extend(docstrings)
        
        for file_path in self.project_root.rglob("*.h"):
            docstrings = self._extract_docstrings(file_path)
            documentation["docstrings"].extend(docstrings)
        
        return documentation
    
    def _extract_documentation_file(self, file_path: Path) -> Dict[str, Any]:
        """Extract information from a documentation file"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            return {
                "file": str(file_path.relative_to(self.project_root)),
                "size": file_path.stat().st_size,
                "lines": len(content.split('\n')),
                "title": self._extract_title(content),
                "sections": self._extract_sections(content),
                "last_modified": datetime.fromtimestamp(file_path.stat().st_mtime).isoformat()
            }
        except Exception as e:
            return {
                "file": str(file_path.relative_to(self.project_root)),
                "error": str(e)
            }
    
    def _extract_title(self, content: str) -> str:
        """Extract title from documentation content"""
        lines = content.split('\n')
        for line in lines:
            if line.startswith('# '):
                return line[2:].strip()
        return ""
    
    def _extract_sections(self, content: str) -> List[str]:
        """Extract section headers from documentation content"""
        sections = []
        lines = content.split('\n')
        for line in lines:
            if line.startswith('## '):
                sections.append(line[3:].strip())
        return sections
    
    def _extract_docstrings(self, file_path: Path) -> List[Dict[str, Any]]:
        """Extract docstrings from source files"""
        docstrings = []
        
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            lines = content.split('\n')
            
            # Look for comment blocks that might be docstrings
            for i, line in enumerate(lines, 1):
                if line.strip().startswith('/*') or line.strip().startswith('/**'):
                    # Find the end of the comment block
                    doc_content = []
                    j = i
                    while j < len(lines) and '*/' not in lines[j-1]:
                        doc_content.append(lines[j-1])
                        j += 1
                    
                    if doc_content:
                        docstrings.append({
                            "file": str(file_path.relative_to(self.project_root)),
                            "line": i,
                            "content": '\n'.join(doc_content)
                        })
        
        except Exception:
            pass
        
        return docstrings
    
    def extract_insights(self) -> Dict[str, Any]:
        """Extract insights and patterns from the codebase"""
        print("💡 Extracting insights...")
        
        insights = {
            "code_patterns": {},
            "architecture_insights": {},
            "performance_insights": {},
            "security_insights": {},
            "recommendations": []
        }
        
        # Analyze code patterns
        insights["code_patterns"] = self._analyze_code_patterns()
        
        # Analyze architecture
        insights["architecture_insights"] = self._analyze_architecture()
        
        # Analyze performance patterns
        insights["performance_insights"] = self._analyze_performance_patterns()
        
        # Analyze security patterns
        insights["security_insights"] = self._analyze_security_patterns()
        
        # Generate recommendations
        insights["recommendations"] = self._generate_recommendations(insights)
        
        return insights
    
    def _analyze_code_patterns(self) -> Dict[str, Any]:
        """Analyze code patterns in the codebase"""
        patterns = {
            "naming_conventions": {},
            "code_structure": {},
            "common_patterns": []
        }
        
        # Analyze naming conventions
        function_names = []
        class_names = []
        
        for file_path in self.project_root.rglob("*.cpp"):
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            # Extract function names
            func_matches = re.findall(r'(\w+)\s+(\w+)\s*\(', content)
            for match in func_matches:
                function_names.append(match[1])
            
            # Extract class names
            class_matches = re.findall(r'class\s+(\w+)', content)
            class_names.extend(class_matches)
        
        patterns["naming_conventions"] = {
            "function_count": len(function_names),
            "class_count": len(class_names),
            "common_function_prefixes": self._find_common_prefixes(function_names),
            "common_class_prefixes": self._find_common_prefixes(class_names)
        }
        
        return patterns
    
    def _find_common_prefixes(self, names: List[str]) -> Dict[str, int]:
        """Find common prefixes in a list of names"""
        prefixes = {}
        for name in names:
            if len(name) > 2:
                prefix = name[:3].lower()
                prefixes[prefix] = prefixes.get(prefix, 0) + 1
        
        return {k: v for k, v in sorted(prefixes.items(), key=lambda x: x[1], reverse=True)[:5]}
    
    def _analyze_architecture(self) -> Dict[str, Any]:
        """Analyze the architecture of the codebase"""
        architecture = {
            "modularity": {},
            "coupling": {},
            "complexity": {}
        }
        
        # Analyze modularity
        modules = ["engine", "ui", "search", "config", "sandbox"]
        module_files = {}
        
        for module in modules:
            src_dir = self.project_root / "src" / module
            include_dir = self.project_root / "include" / module
            
            files = []
            if src_dir.exists():
                files.extend([f.name for f in src_dir.glob("*.cpp")])
            if include_dir.exists():
                files.extend([f.name for f in include_dir.glob("*.h")])
            
            module_files[module] = files
        
        architecture["modularity"] = {
            "modules": modules,
            "files_per_module": {module: len(files) for module, files in module_files.items()},
            "total_modules": len(modules)
        }
        
        return architecture
    
    def _analyze_performance_patterns(self) -> Dict[str, Any]:
        """Analyze performance-related patterns"""
        performance = {
            "memory_patterns": [],
            "algorithm_patterns": [],
            "optimization_opportunities": []
        }
        
        # Look for memory allocation patterns
        for file_path in self.project_root.rglob("*.cpp"):
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            if 'new ' in content or 'malloc' in content:
                performance["memory_patterns"].append({
                    "file": str(file_path.relative_to(self.project_root)),
                    "pattern": "dynamic_allocation"
                })
        
        return performance
    
    def _analyze_security_patterns(self) -> Dict[str, Any]:
        """Analyze security-related patterns"""
        security = {
            "input_validation": [],
            "authentication": [],
            "encryption": [],
            "vulnerabilities": []
        }
        
        # Look for security patterns
        for file_path in self.project_root.rglob("*.cpp"):
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            if 'strcpy' in content or 'sprintf' in content:
                security["vulnerabilities"].append({
                    "file": str(file_path.relative_to(self.project_root)),
                    "pattern": "unsafe_string_function"
                })
        
        return security
    
    def _generate_recommendations(self, insights: Dict[str, Any]) -> List[str]:
        """Generate recommendations based on insights"""
        recommendations = []
        
        # Architecture recommendations
        if insights["architecture_insights"]["modularity"]["total_modules"] < 3:
            recommendations.append("Consider adding more modules for better separation of concerns")
        
        # Performance recommendations
        if len(insights["performance_insights"]["memory_patterns"]) > 10:
            recommendations.append("Consider using smart pointers to reduce manual memory management")
        
        # Security recommendations
        if len(insights["security_insights"]["vulnerabilities"]) > 0:
            recommendations.append("Replace unsafe string functions with safer alternatives")
        
        return recommendations
    
    def generate_extraction_report(self) -> Dict[str, Any]:
        """Generate comprehensive extraction report"""
        print("📋 Generating extraction report...")
        
        report = {
            "timestamp": time.time(),
            "datetime": datetime.now().isoformat(),
            "config_info": self.extract_config_info(),
            "code_metadata": self.extract_code_metadata(),
            "documentation": self.extract_documentation(),
            "insights": self.extract_insights()
        }
        
        return report
    
    def save_extraction_report(self, report: Dict[str, Any], filename: str = "extraction_report.json"):
        """Save extraction report to file"""
        report_path = self.project_root / "tools" / "extraction_tools" / filename
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        print(f"📄 Extraction report saved to: {report_path}")
    
    def print_extraction_summary(self, report: Dict[str, Any]):
        """Print a human-readable extraction summary"""
        print("\n" + "="*50)
        print("📄 ZEPRA ENGINE EXTRACTION REPORT")
        print("="*50)
        
        # Config summary
        config_info = report["config_info"]
        summary = config_info["summary"]
        print(f"\n⚙️  Configuration:")
        print(f"    Config Files: {summary['total_config_files']}")
        print(f"    Config Types: {', '.join(summary['config_types'])}")
        print(f"    Dependencies: {len(summary['dependencies'])}")
        
        # Code metadata summary
        code_metadata = report["code_metadata"]
        stats = code_metadata["statistics"]
        print(f"\n📄 Code Metadata:")
        print(f"    Functions: {stats['total_functions']}")
        print(f"    Classes: {stats['total_classes']}")
        print(f"    Includes: {stats['total_includes']}")
        print(f"    Defines: {stats['total_defines']}")
        
        # Documentation summary
        documentation = report["documentation"]
        print(f"\n📚 Documentation:")
        print(f"    README Files: {len(documentation['readme_files'])}")
        print(f"    Markdown Files: {len(documentation['markdown_files'])}")
        print(f"    Docstrings: {len(documentation['docstrings'])}")
        
        # Insights summary
        insights = report["insights"]
        recommendations = insights["recommendations"]
        print(f"\n💡 Insights:")
        print(f"    Recommendations: {len(recommendations)}")
        for rec in recommendations[:3]:
            print(f"    • {rec}")

def main():
    import time
    
    if len(sys.argv) > 1:
        project_root = sys.argv[1]
    else:
        project_root = "."
    
    extractor = InfoExtractor(project_root)
    report = extractor.generate_extraction_report()
    extractor.save_extraction_report(report)
    extractor.print_extraction_summary(report)

if __name__ == "__main__":
    main() 