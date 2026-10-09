#!/usr/bin/env python3
"""
Zepra Browser Analytics Engine
Collects and analyzes browser performance metrics, user behavior, and system statistics
"""

import json
import time
import psutil
import platform
from datetime import datetime, timedelta
from typing import Dict, List, Any, Optional
from dataclasses import dataclass, asdict
from pathlib import Path
import logging
import sqlite3
import threading
from collections import defaultdict, deque

@dataclass
class PerformanceMetrics:
    """Performance metrics data structure"""
    timestamp: float
    cpu_usage: float
    memory_usage: float
    disk_io_read: float
    disk_io_write: float
    network_rx: float
    network_tx: float
    page_load_time: Optional[float] = None
    render_fps: Optional[float] = None

@dataclass
class UserBehavior:
    """User behavior analytics data"""
    timestamp: float
    action_type: str  # 'page_load', 'tab_switch', 'search', 'click', 'scroll'
    url: Optional[str] = None
    search_query: Optional[str] = None
    tab_id: Optional[int] = None
    session_duration: Optional[float] = None

@dataclass
class BrowserStats:
    """Browser statistics"""
    total_tabs: int
    active_tabs: int
    memory_usage_mb: float
    cache_size_mb: float
    cookies_count: int
    bookmarks_count: int
    extensions_count: int

class AnalyticsEngine:
    """Main analytics engine for Zepra Browser"""
    
    def __init__(self, data_dir: str = "analytics_data"):
        self.data_dir = Path(data_dir)
        self.data_dir.mkdir(exist_ok=True)
        
        self.logger = self._setup_logging()
        self.db_path = self.data_dir / "analytics.db"
        
        # Initialize database
        self._init_database()
        
        # Metrics storage
        self.performance_history = deque(maxlen=1000)
        self.user_behavior_history = deque(maxlen=1000)
        self.browser_stats_history = deque(maxlen=100)
        
        # Monitoring state
        self.is_monitoring = False
        self.monitor_thread = None
        
        # Performance tracking
        self.start_time = time.time()
        self.page_load_times = []
        self.search_queries = []
        
    def _setup_logging(self) -> logging.Logger:
        """Setup logging for analytics engine"""
        logger = logging.getLogger('analytics_engine')
        logger.setLevel(logging.INFO)
        
        if not logger.handlers:
            handler = logging.StreamHandler()
            formatter = logging.Formatter(
                '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
            )
            handler.setFormatter(formatter)
            logger.addHandler(handler)
            
        return logger
    
    def _init_database(self):
        """Initialize SQLite database for analytics"""
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        
        # Create performance metrics table
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS performance_metrics (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp REAL,
                cpu_usage REAL,
                memory_usage REAL,
                disk_io_read REAL,
                disk_io_write REAL,
                network_rx REAL,
                network_tx REAL,
                page_load_time REAL,
                render_fps REAL
            )
        ''')
        
        # Create user behavior table
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS user_behavior (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp REAL,
                action_type TEXT,
                url TEXT,
                search_query TEXT,
                tab_id INTEGER,
                session_duration REAL
            )
        ''')
        
        # Create browser stats table
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS browser_stats (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp REAL,
                total_tabs INTEGER,
                active_tabs INTEGER,
                memory_usage_mb REAL,
                cache_size_mb REAL,
                cookies_count INTEGER,
                bookmarks_count INTEGER,
                extensions_count INTEGER
            )
        ''')
        
        conn.commit()
        conn.close()
        self.logger.info("Analytics database initialized")
    
    def start_monitoring(self):
        """Start continuous monitoring"""
        if self.is_monitoring:
            self.logger.warning("Monitoring already started")
            return
        
        self.is_monitoring = True
        self.monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self.monitor_thread.start()
        self.logger.info("Analytics monitoring started")
    
    def stop_monitoring(self):
        """Stop continuous monitoring"""
        self.is_monitoring = False
        if self.monitor_thread:
            self.monitor_thread.join()
        self.logger.info("Analytics monitoring stopped")
    
    def _monitor_loop(self):
        """Main monitoring loop"""
        while self.is_monitoring:
            try:
                # Collect performance metrics
                metrics = self._collect_performance_metrics()
                self.record_performance_metrics(metrics)
                
                # Collect browser stats
                stats = self._collect_browser_stats()
                self.record_browser_stats(stats)
                
                time.sleep(5)  # Collect data every 5 seconds
                
            except Exception as e:
                self.logger.error(f"Error in monitoring loop: {e}")
                time.sleep(10)
    
    def _collect_performance_metrics(self) -> PerformanceMetrics:
        """Collect current performance metrics"""
        # CPU usage
        cpu_usage = psutil.cpu_percent(interval=1)
        
        # Memory usage
        memory = psutil.virtual_memory()
        memory_usage = memory.percent
        
        # Disk I/O
        disk_io = psutil.disk_io_counters()
        disk_io_read = disk_io.read_bytes if disk_io else 0
        disk_io_write = disk_io.write_bytes if disk_io else 0
        
        # Network I/O
        network_io = psutil.net_io_counters()
        network_rx = network_io.bytes_recv
        network_tx = network_io.bytes_sent
        
        return PerformanceMetrics(
            timestamp=time.time(),
            cpu_usage=cpu_usage,
            memory_usage=memory_usage,
            disk_io_read=disk_io_read,
            disk_io_write=disk_io_write,
            network_rx=network_rx,
            network_tx=network_tx
        )
    
    def _collect_browser_stats(self) -> BrowserStats:
        """Collect current browser statistics"""
        # Mock data - in real implementation, this would come from browser state
        return BrowserStats(
            total_tabs=5,
            active_tabs=2,
            memory_usage_mb=150.5,
            cache_size_mb=25.3,
            cookies_count=45,
            bookmarks_count=12,
            extensions_count=3
        )
    
    def record_performance_metrics(self, metrics: PerformanceMetrics):
        """Record performance metrics to database and memory"""
        self.performance_history.append(metrics)
        
        # Save to database
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute('''
            INSERT INTO performance_metrics 
            (timestamp, cpu_usage, memory_usage, disk_io_read, disk_io_write, 
             network_rx, network_tx, page_load_time, render_fps)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ''', (
            metrics.timestamp, metrics.cpu_usage, metrics.memory_usage,
            metrics.disk_io_read, metrics.disk_io_write, metrics.network_rx,
            metrics.network_tx, metrics.page_load_time, metrics.render_fps
        ))
        conn.commit()
        conn.close()
    
    def record_user_behavior(self, behavior: UserBehavior):
        """Record user behavior analytics"""
        self.user_behavior_history.append(behavior)
        
        # Save to database
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute('''
            INSERT INTO user_behavior 
            (timestamp, action_type, url, search_query, tab_id, session_duration)
            VALUES (?, ?, ?, ?, ?, ?)
        ''', (
            behavior.timestamp, behavior.action_type, behavior.url,
            behavior.search_query, behavior.tab_id, behavior.session_duration
        ))
        conn.commit()
        conn.close()
        
        # Track specific behaviors
        if behavior.action_type == 'page_load' and behavior.url:
            self.page_load_times.append(time.time())
        elif behavior.action_type == 'search' and behavior.search_query:
            self.search_queries.append(behavior.search_query)
    
    def record_browser_stats(self, stats: BrowserStats):
        """Record browser statistics"""
        self.browser_stats_history.append(stats)
        
        # Save to database
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute('''
            INSERT INTO browser_stats 
            (timestamp, total_tabs, active_tabs, memory_usage_mb, cache_size_mb,
             cookies_count, bookmarks_count, extensions_count)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ''', (
            time.time(), stats.total_tabs, stats.active_tabs, stats.memory_usage_mb,
            stats.cache_size_mb, stats.cookies_count, stats.bookmarks_count,
            stats.extensions_count
        ))
        conn.commit()
        conn.close()
    
    def get_performance_summary(self, hours: int = 24) -> Dict[str, Any]:
        """Get performance summary for the last N hours"""
        cutoff_time = time.time() - (hours * 3600)
        
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        
        cursor.execute('''
            SELECT AVG(cpu_usage), AVG(memory_usage), AVG(page_load_time), AVG(render_fps)
            FROM performance_metrics 
            WHERE timestamp > ?
        ''', (cutoff_time,))
        
        result = cursor.fetchone()
        conn.close()
        
        if result and result[0] is not None:
            return {
                'avg_cpu_usage': round(result[0], 2),
                'avg_memory_usage': round(result[1], 2),
                'avg_page_load_time': round(result[2], 2) if result[2] else None,
                'avg_render_fps': round(result[3], 2) if result[3] else None,
                'period_hours': hours
            }
        else:
            return {'error': 'No data available for the specified period'}
    
    def get_user_behavior_summary(self, hours: int = 24) -> Dict[str, Any]:
        """Get user behavior summary for the last N hours"""
        cutoff_time = time.time() - (hours * 3600)
        
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        
        # Get action counts
        cursor.execute('''
            SELECT action_type, COUNT(*) 
            FROM user_behavior 
            WHERE timestamp > ? 
            GROUP BY action_type
        ''', (cutoff_time,))
        
        action_counts = dict(cursor.fetchall())
        
        # Get top search queries
        cursor.execute('''
            SELECT search_query, COUNT(*) 
            FROM user_behavior 
            WHERE timestamp > ? AND search_query IS NOT NULL
            GROUP BY search_query 
            ORDER BY COUNT(*) DESC 
            LIMIT 10
        ''', (cutoff_time,))
        
        top_searches = cursor.fetchall()
        
        conn.close()
        
        return {
            'action_counts': action_counts,
            'top_searches': top_searches,
            'period_hours': hours
        }
    
    def export_analytics(self, output_file: str = "analytics_export.json"):
        """Export all analytics data to JSON file"""
        export_data = {
            'export_timestamp': datetime.now().isoformat(),
            'performance_summary': self.get_performance_summary(),
            'user_behavior_summary': self.get_user_behavior_summary(),
            'system_info': {
                'platform': platform.platform(),
                'python_version': platform.python_version(),
                'uptime_hours': (time.time() - self.start_time) / 3600
            }
        }
        
        output_path = self.data_dir / output_file
        with open(output_path, 'w') as f:
            json.dump(export_data, f, indent=2)
        
        self.logger.info(f"Analytics exported to {output_path}")
        return output_path

def main():
    """Main entry point for analytics engine"""
    analytics = AnalyticsEngine()
    
    # Start monitoring
    analytics.start_monitoring()
    
    # Simulate some user behavior
    time.sleep(2)
    analytics.record_user_behavior(UserBehavior(
        timestamp=time.time(),
        action_type='page_load',
        url='https://example.com'
    ))
    
    time.sleep(2)
    analytics.record_user_behavior(UserBehavior(
        timestamp=time.time(),
        action_type='search',
        search_query='zepra browser'
    ))
    
    # Stop monitoring
    analytics.stop_monitoring()
    
    # Print summaries
    print("=== Performance Summary ===")
    perf_summary = analytics.get_performance_summary()
    for key, value in perf_summary.items():
        print(f"{key}: {value}")
    
    print("\n=== User Behavior Summary ===")
    behavior_summary = analytics.get_user_behavior_summary()
    for key, value in behavior_summary.items():
        print(f"{key}: {value}")
    
    # Export analytics
    export_path = analytics.export_analytics()
    print(f"\nAnalytics exported to: {export_path}")

if __name__ == "__main__":
    main() 