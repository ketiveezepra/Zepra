#!/usr/bin/env python3
"""
Zepra Browser Main Orchestrator
Coordinates all Python tools for validation, configuration, analytics, building, and testing
"""

import sys
import os
import argparse
import json
import time
from pathlib import Path
from typing import Dict, List, Any, Optional
import logging
from datetime import datetime

# Import our modules
sys.path.append(str(Path(__file__).parent))

from validation.validation_engine import ValidationEngine
from config.config_manager import ConfigManager
from analytics.analytics_engine import AnalyticsEngine
from build.build_manager import BuildManager
from test.test_orchestrator import TestOrchestrator

class MainOrchestrator:
    """Main orchestrator for Zepra Browser development workflow"""
    
    def __init__(self, project_root: str = "."):
        self.project_root = Path(project_root)
        self.logger = self._setup_logging()
        
        # Initialize all components
        self.validation_engine = ValidationEngine(project_root)
        self.config_manager = ConfigManager("configs")
        self.analytics_engine = AnalyticsEngine("analytics_data")
        self.build_manager = BuildManager(project_root, "build")
        self.test_orchestrator = TestOrchestrator(project_root, "test")
        
        # Workflow state
        self.workflow_results = {}
        
    def _setup_logging(self) -> logging.Logger:
        """Setup logging for main orchestrator"""
        logger = logging.getLogger('main_orchestrator')
        logger.setLevel(logging.INFO)
        
        if not logger.handlers:
            handler = logging.StreamHandler()
            formatter = logging.Formatter(
                '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
            )
            handler.setFormatter(formatter)
            logger.addHandler(handler)
            
        return logger
    
    def run_validation_workflow(self) -> bool:
        """Run validation workflow"""
        self.logger.info("🔍 Starting validation workflow...")
        
        try:
            results = self.validation_engine.run_all_validations()
            
            # Check for failures
            total_failures = sum(
                len([r for r in result_list if r.status == 'FAIL'])
                for result_list in results.values()
            )
            
            self.workflow_results['validation'] = {
                'success': total_failures == 0,
                'results': results,
                'timestamp': datetime.now().isoformat()
            }
            
            if total_failures > 0:
                self.logger.error(f"❌ Validation failed with {total_failures} errors")
                return False
            else:
                self.logger.info("✅ Validation passed")
                return True
                
        except Exception as e:
            self.logger.error(f"❌ Validation workflow failed: {e}")
            self.workflow_results['validation'] = {
                'success': False,
                'error': str(e),
                'timestamp': datetime.now().isoformat()
            }
            return False
    
    def run_configuration_workflow(self) -> bool:
        """Run configuration workflow"""
        self.logger.info("⚙️ Starting configuration workflow...")
        
        try:
            # Validate configurations
            errors = self.config_manager.validate_configurations()
            
            if errors:
                self.logger.error(f"❌ Configuration validation failed: {errors}")
                self.workflow_results['configuration'] = {
                    'success': False,
                    'errors': errors,
                    'timestamp': datetime.now().isoformat()
                }
                return False
            
            # Save configurations
            self.config_manager.save_configurations()
            
            # Generate environment variables
            env_vars = self.config_manager.generate_environment_vars()
            
            self.workflow_results['configuration'] = {
                'success': True,
                'env_vars': env_vars,
                'timestamp': datetime.now().isoformat()
            }
            
            self.logger.info("✅ Configuration workflow completed")
            return True
            
        except Exception as e:
            self.logger.error(f"❌ Configuration workflow failed: {e}")
            self.workflow_results['configuration'] = {
                'success': False,
                'error': str(e),
                'timestamp': datetime.now().isoformat()
            }
            return False
    
    def run_build_workflow(self) -> bool:
        """Run build workflow"""
        self.logger.info("🔨 Starting build workflow...")
        
        try:
            # Setup build environment
            if not self.build_manager.setup_build_environment():
                self.logger.error("❌ Build environment setup failed")
                self.workflow_results['build'] = {
                    'success': False,
                    'error': 'Build environment setup failed',
                    'timestamp': datetime.now().isoformat()
                }
                return False
            
            # Build all targets
            results = self.build_manager.build_all()
            
            # Check build success
            all_success = all(result.success for result in results.values())
            
            self.workflow_results['build'] = {
                'success': all_success,
                'results': {name: {
                    'success': result.success,
                    'duration': result.duration,
                    'errors': result.errors
                } for name, result in results.items()},
                'timestamp': datetime.now().isoformat()
            }
            
            if all_success:
                self.logger.info("✅ Build workflow completed successfully")
                return True
            else:
                self.logger.error("❌ Build workflow failed")
                return False
                
        except Exception as e:
            self.logger.error(f"❌ Build workflow failed: {e}")
            self.workflow_results['build'] = {
                'success': False,
                'error': str(e),
                'timestamp': datetime.now().isoformat()
            }
            return False
    
    def run_test_workflow(self) -> bool:
        """Run test workflow"""
        self.logger.info("🧪 Starting test workflow...")
        
        try:
            # Setup test environment
            if not self.test_orchestrator.setup_test_environment():
                self.logger.error("❌ Test environment setup failed")
                self.workflow_results['test'] = {
                    'success': False,
                    'error': 'Test environment setup failed',
                    'timestamp': datetime.now().isoformat()
                }
                return False
            
            # Run all tests
            report = self.test_orchestrator.run_all_tests()
            
            # Generate reports
            json_report = self.test_orchestrator.generate_test_report(report)
            html_report = self.test_orchestrator.generate_html_report(report)
            
            success = report.failed_tests == 0
            
            self.workflow_results['test'] = {
                'success': success,
                'report': {
                    'total_tests': report.total_tests,
                    'passed_tests': report.passed_tests,
                    'failed_tests': report.failed_tests,
                    'success_rate': (report.passed_tests / report.total_tests * 100) if report.total_tests > 0 else 0,
                    'coverage_percentage': report.coverage_percentage
                },
                'reports': {
                    'json': str(json_report),
                    'html': str(html_report)
                },
                'timestamp': datetime.now().isoformat()
            }
            
            if success:
                self.logger.info("✅ Test workflow completed successfully")
                return True
            else:
                self.logger.error(f"❌ Test workflow failed with {report.failed_tests} failures")
                return False
                
        except Exception as e:
            self.logger.error(f"❌ Test workflow failed: {e}")
            self.workflow_results['test'] = {
                'success': False,
                'error': str(e),
                'timestamp': datetime.now().isoformat()
            }
            return False
    
    def run_analytics_workflow(self) -> bool:
        """Run analytics workflow"""
        self.logger.info("📊 Starting analytics workflow...")
        
        try:
            # Start analytics monitoring
            self.analytics_engine.start_monitoring()
            
            # Simulate some activity for demo
            import time
            time.sleep(2)
            
            # Stop monitoring
            self.analytics_engine.stop_monitoring()
            
            # Get analytics summaries
            perf_summary = self.analytics_engine.get_performance_summary()
            behavior_summary = self.analytics_engine.get_user_behavior_summary()
            
            # Export analytics
            export_path = self.analytics_engine.export_analytics()
            
            self.workflow_results['analytics'] = {
                'success': True,
                'performance_summary': perf_summary,
                'behavior_summary': behavior_summary,
                'export_path': str(export_path),
                'timestamp': datetime.now().isoformat()
            }
            
            self.logger.info("✅ Analytics workflow completed")
            return True
            
        except Exception as e:
            self.logger.error(f"❌ Analytics workflow failed: {e}")
            self.workflow_results['analytics'] = {
                'success': False,
                'error': str(e),
                'timestamp': datetime.now().isoformat()
            }
            return False
    
    def run_full_workflow(self) -> bool:
        """Run complete development workflow"""
        self.logger.info("🚀 Starting full Zepra Browser development workflow...")
        
        workflow_steps = [
            ('validation', self.run_validation_workflow),
            ('configuration', self.run_configuration_workflow),
            ('build', self.run_build_workflow),
            ('test', self.run_test_workflow),
            ('analytics', self.run_analytics_workflow)
        ]
        
        overall_success = True
        
        for step_name, step_function in workflow_steps:
            self.logger.info(f"\n{'='*50}")
            self.logger.info(f"Step: {step_name.upper()}")
            self.logger.info(f"{'='*50}")
            
            step_success = step_function()
            if not step_success:
                overall_success = False
                self.logger.error(f"❌ Workflow stopped at {step_name} step")
                break
        
        # Generate workflow report
        self._generate_workflow_report()
        
        if overall_success:
            self.logger.info("\n🎉 Full workflow completed successfully!")
        else:
            self.logger.error("\n💥 Workflow failed!")
        
        return overall_success
    
    def _generate_workflow_report(self):
        """Generate comprehensive workflow report"""
        report = {
            'workflow_timestamp': datetime.now().isoformat(),
            'project_root': str(self.project_root),
            'overall_success': all(
                result.get('success', False) 
                for result in self.workflow_results.values()
            ),
            'steps': self.workflow_results,
            'summary': {
                'total_steps': len(self.workflow_results),
                'successful_steps': sum(
                    1 for result in self.workflow_results.values() 
                    if result.get('success', False)
                ),
                'failed_steps': sum(
                    1 for result in self.workflow_results.values() 
                    if not result.get('success', False)
                )
            }
        }
        
        # Save report
        report_path = Path("workflow_report.json")
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        
        self.logger.info(f"📋 Workflow report saved to: {report_path}")
        
        # Print summary
        print(f"\n{'='*60}")
        print(f"WORKFLOW SUMMARY")
        print(f"{'='*60}")
        print(f"Overall Success: {'✅ YES' if report['overall_success'] else '❌ NO'}")
        print(f"Steps Completed: {report['summary']['successful_steps']}/{report['summary']['total_steps']}")
        
        for step_name, step_result in self.workflow_results.items():
            status = "✅ PASS" if step_result.get('success', False) else "❌ FAIL"
            print(f"{step_name.capitalize()}: {status}")
        
        print(f"{'='*60}")

def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(description="Zepra Browser Development Workflow Orchestrator")
    parser.add_argument('--workflow', choices=['validation', 'config', 'build', 'test', 'analytics', 'full'], 
                       default='full', help='Workflow to run')
    parser.add_argument('--project-root', default='.', help='Project root directory')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    # Setup logging level
    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)
    
    # Create orchestrator
    orchestrator = MainOrchestrator(args.project_root)
    
    # Run selected workflow
    if args.workflow == 'validation':
        success = orchestrator.run_validation_workflow()
    elif args.workflow == 'config':
        success = orchestrator.run_configuration_workflow()
    elif args.workflow == 'build':
        success = orchestrator.run_build_workflow()
    elif args.workflow == 'test':
        success = orchestrator.run_test_workflow()
    elif args.workflow == 'analytics':
        success = orchestrator.run_analytics_workflow()
    else:  # full
        success = orchestrator.run_full_workflow()
    
    # Exit with appropriate code
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main() 