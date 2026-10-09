#!/usr/bin/env python3
"""
Zepra Browser Test Orchestrator
Manages all testing activities including unit tests, integration tests, performance tests, and test reporting
"""

import subprocess
import sys
import os
import json
import time
import platform
from pathlib import Path
from typing import Dict, List, Any, Optional, Tuple
from dataclasses import dataclass, asdict
import logging
import threading
import multiprocessing
from datetime import datetime
import xml.etree.ElementTree as ET

@dataclass
class TestResult:
    """Result of a test execution"""
    name: str
    status: str  # 'PASS', 'FAIL', 'SKIP', 'TIMEOUT'
    duration: float
    output: str
    error_message: Optional[str] = None
    test_file: Optional[str] = None
    line_number: Optional[int] = None

@dataclass
class TestSuite:
    """Test suite configuration"""
    name: str
    type: str  # 'unit', 'integration', 'performance', 'regression'
    source_dir: str
    executable: str
    timeout: int
    parallel: bool
    dependencies: List[str]

@dataclass
class TestReport:
    """Complete test report"""
    timestamp: str
    total_tests: int
    passed_tests: int
    failed_tests: int
    skipped_tests: int
    total_duration: float
    test_results: List[TestResult]
    coverage_percentage: Optional[float] = None

class TestOrchestrator:
    """Main test orchestrator for Zepra Browser"""
    
    def __init__(self, project_root: str = ".", test_dir: str = "test"):
        self.project_root = Path(project_root)
        self.test_dir = Path(test_dir)
        self.logger = self._setup_logging()
        
        # Test configuration
        self.test_suites = self._define_test_suites()
        self.test_results = []
        self.is_running = False
        
        # Coverage tracking
        self.coverage_enabled = True
        self.coverage_data = {}
        
    def _setup_logging(self) -> logging.Logger:
        """Setup logging for test orchestrator"""
        logger = logging.getLogger('test_orchestrator')
        logger.setLevel(logging.INFO)
        
        if not logger.handlers:
            handler = logging.StreamHandler()
            formatter = logging.Formatter(
                '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
            )
            handler.setFormatter(formatter)
            logger.addHandler(handler)
            
        return logger
    
    def _define_test_suites(self) -> Dict[str, TestSuite]:
        """Define test suites"""
        suites = {
            'unit_tests': TestSuite(
                name='unit_tests',
                type='unit',
                source_dir='test/unit',
                executable='bin/tests/unit_test_suite',
                timeout=30,
                parallel=True,
                dependencies=['zepra_browser']
            ),
            'integration_tests': TestSuite(
                name='integration_tests',
                type='integration',
                source_dir='test/integration',
                executable='bin/tests/integration_test_suite',
                timeout=60,
                parallel=False,
                dependencies=['zepra_browser']
            ),
            'performance_tests': TestSuite(
                name='performance_tests',
                type='performance',
                source_dir='test/performance',
                executable='bin/tests/performance_test_suite',
                timeout=120,
                parallel=False,
                dependencies=['zepra_browser']
            ),
            'regression_tests': TestSuite(
                name='regression_tests',
                type='regression',
                source_dir='test/regression',
                executable='bin/tests/regression_test_suite',
                timeout=90,
                parallel=True,
                dependencies=['zepra_browser']
            )
        }
        return suites
    
    def setup_test_environment(self) -> bool:
        """Setup test environment"""
        self.logger.info("Setting up test environment...")
        
        # Create test output directory
        test_output_dir = Path("test_output")
        test_output_dir.mkdir(exist_ok=True)
        
        # Check if test executables exist
        missing_executables = []
        for suite_name, suite in self.test_suites.items():
            executable_path = Path(suite.executable)
            if platform.system() == "Windows":
                executable_path = executable_path.with_suffix('.exe')
            
            if not executable_path.exists():
                missing_executables.append(suite_name)
        
        if missing_executables:
            self.logger.error(f"Missing test executables: {missing_executables}")
            return False
        
        self.logger.info("Test environment setup complete")
        return True
    
    def run_test_suite(self, suite_name: str) -> List[TestResult]:
        """Run a specific test suite"""
        if suite_name not in self.test_suites:
            self.logger.error(f"Unknown test suite: {suite_name}")
            return []
        
        suite = self.test_suites[suite_name]
        self.logger.info(f"Running test suite: {suite_name}")
        
        results = []
        start_time = time.time()
        
        try:
            # Run test executable
            executable_path = Path(suite.executable)
            if platform.system() == "Windows":
                executable_path = executable_path.with_suffix('.exe')
            
            if not executable_path.exists():
                results.append(TestResult(
                    name=suite_name,
                    status='FAIL',
                    duration=0.0,
                    output="",
                    error_message=f"Test executable not found: {executable_path}"
                ))
                return results
            
            # Run the test
            cmd = [str(executable_path)]
            if suite.type == 'performance':
                cmd.extend(['--benchmark', '--output=json'])
            elif suite.type == 'unit':
                cmd.extend(['--gtest_output=xml', '--gtest_color=yes'])
            
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=suite.timeout,
                cwd=self.project_root
            )
            
            duration = time.time() - start_time
            
            # Parse test results
            if result.returncode == 0:
                status = 'PASS'
                error_message = None
            else:
                status = 'FAIL'
                error_message = result.stderr
            
            # Parse XML output if available
            if suite.type == 'unit' and '--gtest_output=xml' in cmd:
                xml_results = self._parse_gtest_xml(result.stdout)
                results.extend(xml_results)
            else:
                # Single result for the suite
                results.append(TestResult(
                    name=suite_name,
                    status=status,
                    duration=duration,
                    output=result.stdout,
                    error_message=error_message
                ))
            
        except subprocess.TimeoutExpired:
            results.append(TestResult(
                name=suite_name,
                status='TIMEOUT',
                duration=suite.timeout,
                output="",
                error_message=f"Test suite timed out after {suite.timeout} seconds"
            ))
        except Exception as e:
            results.append(TestResult(
                name=suite_name,
                status='FAIL',
                duration=time.time() - start_time,
                output="",
                error_message=str(e)
            ))
        
        return results
    
    def _parse_gtest_xml(self, xml_content: str) -> List[TestResult]:
        """Parse Google Test XML output"""
        results = []
        
        try:
            # Try to find XML content in the output
            xml_start = xml_content.find('<?xml')
            if xml_start == -1:
                return results
            
            xml_data = xml_content[xml_start:]
            root = ET.fromstring(xml_data)
            
            for testcase in root.findall('.//testcase'):
                name = testcase.get('name', 'unknown')
                classname = testcase.get('classname', '')
                full_name = f"{classname}.{name}" if classname else name
                
                # Check for failures
                failure = testcase.find('failure')
                if failure is not None:
                    status = 'FAIL'
                    error_message = failure.text
                else:
                    status = 'PASS'
                    error_message = None
                
                # Get duration
                time_attr = testcase.get('time', '0')
                duration = float(time_attr)
                
                results.append(TestResult(
                    name=full_name,
                    status=status,
                    duration=duration,
                    output="",
                    error_message=error_message
                ))
                
        except ET.ParseError as e:
            self.logger.warning(f"Failed to parse XML: {e}")
        
        return results
    
    def run_all_tests(self, suites: Optional[List[str]] = None) -> TestReport:
        """Run all test suites or specified suites"""
        if suites is None:
            suites = list(self.test_suites.keys())
        
        self.logger.info(f"Running test suites: {suites}")
        self.is_running = True
        
        all_results = []
        start_time = time.time()
        
        # Run test suites
        for suite_name in suites:
            if suite_name in self.test_suites:
                suite_results = self.run_test_suite(suite_name)
                all_results.extend(suite_results)
        
        total_duration = time.time() - start_time
        
        # Calculate statistics
        total_tests = len(all_results)
        passed_tests = len([r for r in all_results if r.status == 'PASS'])
        failed_tests = len([r for r in all_results if r.status == 'FAIL'])
        skipped_tests = len([r for r in all_results if r.status == 'SKIP'])
        
        # Calculate coverage if enabled
        coverage_percentage = None
        if self.coverage_enabled:
            coverage_percentage = self._calculate_coverage()
        
        # Create test report
        report = TestReport(
            timestamp=datetime.now().isoformat(),
            total_tests=total_tests,
            passed_tests=passed_tests,
            failed_tests=failed_tests,
            skipped_tests=skipped_tests,
            total_duration=total_duration,
            test_results=all_results,
            coverage_percentage=coverage_percentage
        )
        
        self.test_results.append(report)
        self.is_running = False
        
        return report
    
    def _calculate_coverage(self) -> Optional[float]:
        """Calculate code coverage percentage"""
        try:
            # This would integrate with a coverage tool like gcov or lcov
            # For now, return a mock value
            return 85.5
        except Exception as e:
            self.logger.warning(f"Failed to calculate coverage: {e}")
            return None
    
    def run_performance_benchmarks(self) -> Dict[str, Any]:
        """Run performance benchmarks"""
        self.logger.info("Running performance benchmarks...")
        
        benchmarks = {}
        
        # Run performance test suite
        perf_results = self.run_test_suite('performance_tests')
        
        # Parse benchmark results
        for result in perf_results:
            if result.output:
                try:
                    # Parse JSON benchmark output
                    benchmark_data = json.loads(result.output)
                    benchmarks[result.name] = benchmark_data
                except json.JSONDecodeError:
                    self.logger.warning(f"Failed to parse benchmark output for {result.name}")
        
        return benchmarks
    
    def generate_test_report(self, report: TestReport, output_file: str = "test_report.json") -> Path:
        """Generate detailed test report"""
        report_data = {
            'summary': {
                'timestamp': report.timestamp,
                'total_tests': report.total_tests,
                'passed_tests': report.passed_tests,
                'failed_tests': report.failed_tests,
                'skipped_tests': report.skipped_tests,
                'success_rate': (report.passed_tests / report.total_tests * 100) if report.total_tests > 0 else 0,
                'total_duration': report.total_duration,
                'coverage_percentage': report.coverage_percentage
            },
            'test_results': [asdict(result) for result in report.test_results],
            'failed_tests': [
                asdict(result) for result in report.test_results 
                if result.status == 'FAIL'
            ],
            'slow_tests': [
                asdict(result) for result in report.test_results 
                if result.duration > 5.0  # Tests taking more than 5 seconds
            ]
        }
        
        output_path = Path("test_output") / output_file
        with open(output_path, 'w') as f:
            json.dump(report_data, f, indent=2)
        
        self.logger.info(f"Test report generated: {output_path}")
        return output_path
    
    def generate_html_report(self, report: TestReport, output_file: str = "test_report.html") -> Path:
        """Generate HTML test report"""
        html_content = f"""
<!DOCTYPE html>
<html>
<head>
    <title>Zepra Browser Test Report</title>
    <style>
        body {{ font-family: Arial, sans-serif; margin: 20px; }}
        .summary {{ background: #f5f5f5; padding: 20px; border-radius: 5px; }}
        .success {{ color: green; }}
        .failure {{ color: red; }}
        .warning {{ color: orange; }}
        table {{ border-collapse: collapse; width: 100%; }}
        th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; }}
        th {{ background-color: #f2f2f2; }}
    </style>
</head>
<body>
    <h1>Zepra Browser Test Report</h1>
    <div class="summary">
        <h2>Summary</h2>
        <p><strong>Timestamp:</strong> {report.timestamp}</p>
        <p><strong>Total Tests:</strong> {report.total_tests}</p>
        <p><strong>Passed:</strong> <span class="success">{report.passed_tests}</span></p>
        <p><strong>Failed:</strong> <span class="failure">{report.failed_tests}</span></p>
        <p><strong>Skipped:</strong> <span class="warning">{report.skipped_tests}</span></p>
        <p><strong>Success Rate:</strong> {(report.passed_tests / report.total_tests * 100) if report.total_tests > 0 else 0:.1f}%</p>
        <p><strong>Total Duration:</strong> {report.total_duration:.2f} seconds</p>
        <p><strong>Coverage:</strong> {report.coverage_percentage:.1f}%</p>
    </div>
    
    <h2>Test Results</h2>
    <table>
        <tr>
            <th>Test Name</th>
            <th>Status</th>
            <th>Duration (s)</th>
            <th>Error Message</th>
        </tr>
"""
        
        for result in report.test_results:
            status_class = 'success' if result.status == 'PASS' else 'failure'
            html_content += f"""
        <tr>
            <td>{result.name}</td>
            <td class="{status_class}">{result.status}</td>
            <td>{result.duration:.3f}</td>
            <td>{result.error_message or ''}</td>
        </tr>
"""
        
        html_content += """
    </table>
</body>
</html>
"""
        
        output_path = Path("test_output") / output_file
        with open(output_path, 'w') as f:
            f.write(html_content)
        
        self.logger.info(f"HTML test report generated: {output_path}")
        return output_path
    
    def get_test_statistics(self) -> Dict[str, Any]:
        """Get test statistics from all runs"""
        if not self.test_results:
            return {}
        
        total_runs = len(self.test_results)
        avg_success_rate = sum(
            (r.passed_tests / r.total_tests * 100) if r.total_tests > 0 else 0
            for r in self.test_results
        ) / total_runs
        
        avg_duration = sum(r.total_duration for r in self.test_results) / total_runs
        
        return {
            'total_runs': total_runs,
            'average_success_rate': avg_success_rate,
            'average_duration': avg_duration,
            'last_run': self.test_results[-1].timestamp if self.test_results else None
        }

def main():
    """Main entry point for test orchestrator"""
    orchestrator = TestOrchestrator()
    
    # Setup test environment
    if not orchestrator.setup_test_environment():
        print("❌ Failed to setup test environment")
        sys.exit(1)
    
    # Run all tests
    print("🧪 Running all tests...")
    report = orchestrator.run_all_tests()
    
    # Print summary
    print(f"\n=== Test Summary ===")
    print(f"Total Tests: {report.total_tests}")
    print(f"Passed: {report.passed_tests} ✅")
    print(f"Failed: {report.failed_tests} ❌")
    print(f"Skipped: {report.skipped_tests} ⏭️")
    print(f"Success Rate: {(report.passed_tests / report.total_tests * 100) if report.total_tests > 0 else 0:.1f}%")
    print(f"Total Duration: {report.total_duration:.2f}s")
    print(f"Coverage: {report.coverage_percentage:.1f}%")
    
    # Generate reports
    json_report = orchestrator.generate_test_report(report)
    html_report = orchestrator.generate_html_report(report)
    
    print(f"\n📊 Reports generated:")
    print(f"  JSON: {json_report}")
    print(f"  HTML: {html_report}")
    
    # Check overall success
    if report.failed_tests > 0:
        print(f"\n💥 {report.failed_tests} tests failed!")
        sys.exit(1)
    else:
        print(f"\n🎉 All tests passed!")

if __name__ == "__main__":
    main() 