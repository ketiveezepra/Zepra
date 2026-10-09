#!/usr/bin/env python3
"""
Zepra Browser Engine - Performance Monitor
Monitors runtime performance, memory usage, and provides real-time metrics
"""

import os
import sys
import json
import time
import psutil
import threading
import subprocess
from pathlib import Path
from typing import Dict, List, Any, Optional
from datetime import datetime

class PerformanceMonitor:
    def __init__(self, project_root: str):
        self.project_root = Path(project_root)
        self.monitoring = False
        self.metrics = []
        self.monitor_thread = None
        
    def start_monitoring(self, interval: float = 1.0):
        """Start continuous performance monitoring"""
        if self.monitoring:
            print("⚠️  Monitoring already active")
            return
        
        print(f"🚀 Starting performance monitoring (interval: {interval}s)")
        self.monitoring = True
        self.monitor_thread = threading.Thread(
            target=self._monitor_loop,
            args=(interval,),
            daemon=True
        )
        self.monitor_thread.start()
    
    def stop_monitoring(self):
        """Stop performance monitoring"""
        if not self.monitoring:
            print("⚠️  Monitoring not active")
            return
        
        print("🛑 Stopping performance monitoring...")
        self.monitoring = False
        if self.monitor_thread:
            self.monitor_thread.join(timeout=5)
    
    def _monitor_loop(self, interval: float):
        """Main monitoring loop"""
        while self.monitoring:
            try:
                metric = self._collect_metrics()
                self.metrics.append(metric)
                time.sleep(interval)
            except Exception as e:
                print(f"❌ Monitoring error: {e}")
                break
    
    def _collect_metrics(self) -> Dict[str, Any]:
        """Collect current system and process metrics"""
        timestamp = time.time()
        
        # System metrics
        cpu_percent = psutil.cpu_percent(interval=0.1)
        memory = psutil.virtual_memory()
        disk = psutil.disk_usage(str(self.project_root))
        
        # Process metrics (if browser is running)
        process_metrics = self._get_process_metrics()
        
        return {
            "timestamp": timestamp,
            "datetime": datetime.fromtimestamp(timestamp).isoformat(),
            "system": {
                "cpu_percent": cpu_percent,
                "memory_used": memory.used,
                "memory_available": memory.available,
                "memory_percent": memory.percent,
                "disk_used": disk.used,
                "disk_free": disk.free,
                "disk_percent": disk.percent
            },
            "process": process_metrics
        }
    
    def _get_process_metrics(self) -> Dict[str, Any]:
        """Get metrics for browser process if running"""
        try:
            # Look for browser process
            browser_processes = []
            for proc in psutil.process_iter(['pid', 'name', 'cmdline']):
                try:
                    if proc.info['name'] and 'zepra' in proc.info['name'].lower():
                        browser_processes.append(proc)
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    continue
            
            if not browser_processes:
                return {"status": "not_running"}
            
            # Get metrics for the first browser process found
            proc = browser_processes[0]
            with proc.oneshot():
                return {
                    "status": "running",
                    "pid": proc.pid,
                    "name": proc.name(),
                    "cpu_percent": proc.cpu_percent(),
                    "memory_percent": proc.memory_percent(),
                    "memory_rss": proc.memory_info().rss,
                    "memory_vms": proc.memory_info().vms,
                    "num_threads": proc.num_threads(),
                    "create_time": proc.create_time()
                }
        except Exception as e:
            return {"status": "error", "error": str(e)}
    
    def get_performance_summary(self) -> Dict[str, Any]:
        """Generate performance summary from collected metrics"""
        if not self.metrics:
            return {"error": "No metrics collected"}
        
        # Calculate statistics
        cpu_values = [m["system"]["cpu_percent"] for m in self.metrics]
        memory_values = [m["system"]["memory_percent"] for m in self.metrics]
        
        summary = {
            "duration_seconds": self.metrics[-1]["timestamp"] - self.metrics[0]["timestamp"],
            "samples": len(self.metrics),
            "system": {
                "cpu": {
                    "average": sum(cpu_values) / len(cpu_values),
                    "max": max(cpu_values),
                    "min": min(cpu_values)
                },
                "memory": {
                    "average": sum(memory_values) / len(memory_values),
                    "max": max(memory_values),
                    "min": min(memory_values)
                }
            },
            "process_runtime": self._calculate_process_runtime()
        }
        
        return summary
    
    def _calculate_process_runtime(self) -> Dict[str, Any]:
        """Calculate process runtime statistics"""
        process_metrics = [m["process"] for m in self.metrics if m["process"].get("status") == "running"]
        
        if not process_metrics:
            return {"status": "no_process_data"}
        
        cpu_values = [p["cpu_percent"] for p in process_metrics if "cpu_percent" in p]
        memory_values = [p["memory_percent"] for p in process_metrics if "memory_percent" in p]
        
        return {
            "status": "available",
            "samples": len(process_metrics),
            "cpu": {
                "average": sum(cpu_values) / len(cpu_values) if cpu_values else 0,
                "max": max(cpu_values) if cpu_values else 0,
                "min": min(cpu_values) if cpu_values else 0
            },
            "memory": {
                "average": sum(memory_values) / len(memory_values) if memory_values else 0,
                "max": max(memory_values) if memory_values else 0,
                "min": min(memory_values) if memory_values else 0
            }
        }
    
    def benchmark_engine_build(self) -> Dict[str, Any]:
        """Benchmark engine build performance"""
        print("🏗️  Benchmarking engine build...")
        
        benchmark = {
            "start_time": time.time(),
            "steps": {},
            "total_time": 0,
            "success": False
        }
        
        try:
            # Step 1: Configure
            start = time.time()
            result = subprocess.run(
                ["cmake", "-B", str(self.project_root / "build")],
                capture_output=True,
                text=True,
                cwd=self.project_root,
                timeout=60
            )
            configure_time = time.time() - start
            benchmark["steps"]["configure"] = {
                "time": configure_time,
                "success": result.returncode == 0,
                "output": result.stdout,
                "error": result.stderr
            }
            
            # Step 2: Build engine
            start = time.time()
            result = subprocess.run(
                ["cmake", "--build", str(self.project_root / "build"), "--target", "engine"],
                capture_output=True,
                text=True,
                cwd=self.project_root,
                timeout=300
            )
            build_time = time.time() - start
            benchmark["steps"]["build_engine"] = {
                "time": build_time,
                "success": result.returncode == 0,
                "output": result.stdout,
                "error": result.stderr
            }
            
            benchmark["total_time"] = time.time() - benchmark["start_time"]
            benchmark["success"] = all(step["success"] for step in benchmark["steps"].values())
            
        except Exception as e:
            benchmark["error"] = str(e)
            benchmark["total_time"] = time.time() - benchmark["start_time"]
        
        return benchmark
    
    def analyze_memory_patterns(self) -> Dict[str, Any]:
        """Analyze memory usage patterns"""
        if not self.metrics:
            return {"error": "No metrics collected"}
        
        memory_data = [m["system"]["memory_percent"] for m in self.metrics]
        
        # Detect memory leaks (simplified)
        memory_trend = self._calculate_trend(memory_data)
        
        # Detect memory spikes
        memory_spikes = []
        for i, value in enumerate(memory_data):
            if i > 0 and i < len(memory_data) - 1:
                prev = memory_data[i - 1]
                next_val = memory_data[i + 1]
                if value > prev + 5 and value > next_val + 5:  # 5% spike
                    memory_spikes.append({
                        "index": i,
                        "value": value,
                        "timestamp": self.metrics[i]["timestamp"]
                    })
        
        return {
            "total_samples": len(memory_data),
            "average_usage": sum(memory_data) / len(memory_data),
            "peak_usage": max(memory_data),
            "trend": memory_trend,
            "spikes": memory_spikes,
            "leak_detected": memory_trend > 0.1  # 0.1% per sample increase
        }
    
    def _calculate_trend(self, data: List[float]) -> float:
        """Calculate linear trend of data"""
        if len(data) < 2:
            return 0.0
        
        n = len(data)
        x_sum = sum(range(n))
        y_sum = sum(data)
        xy_sum = sum(i * val for i, val in enumerate(data))
        x2_sum = sum(i * i for i in range(n))
        
        slope = (n * xy_sum - x_sum * y_sum) / (n * x2_sum - x_sum * x_sum)
        return slope
    
    def generate_performance_report(self) -> Dict[str, Any]:
        """Generate comprehensive performance report"""
        print("📋 Generating performance report...")
        
        # Stop monitoring if active
        was_monitoring = self.monitoring
        if was_monitoring:
            self.stop_monitoring()
        
        report = {
            "timestamp": time.time(),
            "datetime": datetime.now().isoformat(),
            "summary": self.get_performance_summary(),
            "memory_analysis": self.analyze_memory_patterns(),
            "build_benchmark": self.benchmark_engine_build(),
            "raw_metrics": self.metrics[-100:] if self.metrics else []  # Last 100 samples
        }
        
        return report
    
    def save_performance_report(self, report: Dict[str, Any], filename: str = "performance_report.json"):
        """Save performance report to file"""
        report_path = self.project_root / "tools" / "performance_tools" / filename
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2)
        print(f"📄 Performance report saved to: {report_path}")
    
    def print_performance_summary(self, report: Dict[str, Any]):
        """Print a human-readable performance summary"""
        print("\n" + "="*50)
        print("⚡ ZEPRA ENGINE PERFORMANCE REPORT")
        print("="*50)
        
        # Summary
        summary = report["summary"]
        if "error" in summary:
            print(f"\n❌ Error: {summary['error']}")
            return
        
        print(f"\n📊 Performance Summary:")
        print(f"    Duration: {summary['duration_seconds']:.1f} seconds")
        print(f"    Samples: {summary['samples']}")
        
        # System metrics
        system = summary["system"]
        print(f"\n🖥️  System Performance:")
        print(f"    CPU: {system['cpu']['average']:.1f}% avg, {system['cpu']['max']:.1f}% max")
        print(f"    Memory: {system['memory']['average']:.1f}% avg, {system['memory']['max']:.1f}% max")
        
        # Process metrics
        process = summary["process_runtime"]
        if process["status"] == "available":
            print(f"\n🔧 Process Performance:")
            print(f"    CPU: {process['cpu']['average']:.1f}% avg, {process['cpu']['max']:.1f}% max")
            print(f"    Memory: {process['memory']['average']:.1f}% avg, {process['memory']['max']:.1f}% max")
        
        # Memory analysis
        memory = report["memory_analysis"]
        if "error" not in memory:
            print(f"\n💾 Memory Analysis:")
            print(f"    Average Usage: {memory['average_usage']:.1f}%")
            print(f"    Peak Usage: {memory['peak_usage']:.1f}%")
            print(f"    Memory Spikes: {len(memory['spikes'])}")
            if memory['leak_detected']:
                print(f"    ⚠️  Potential Memory Leak Detected")
        
        # Build benchmark
        build = report["build_benchmark"]
        if build["success"]:
            print(f"\n🏗️  Build Performance:")
            print(f"    Total Time: {build['total_time']:.1f} seconds")
            for step_name, step_data in build["steps"].items():
                print(f"    {step_name}: {step_data['time']:.1f}s")

def main():
    if len(sys.argv) > 1:
        project_root = sys.argv[1]
    else:
        project_root = "."
    
    monitor = PerformanceMonitor(project_root)
    
    # Start monitoring for 30 seconds
    print("🚀 Starting 30-second performance monitoring...")
    monitor.start_monitoring(interval=1.0)
    time.sleep(30)
    monitor.stop_monitoring()
    
    # Generate and save report
    report = monitor.generate_performance_report()
    monitor.save_performance_report(report)
    monitor.print_performance_summary(report)

if __name__ == "__main__":
    main() 