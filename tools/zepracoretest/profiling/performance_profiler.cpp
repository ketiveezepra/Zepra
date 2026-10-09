#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <fstream>
#include <map>
#include <thread>
#include <mutex>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

class PerformanceProfiler {
private:
    struct ProfilePoint {
        std::string name;
        std::chrono::high_resolution_clock::time_point start_time;
        std::chrono::high_resolution_clock::time_point end_time;
        size_t memory_usage;
    };
    
    std::map<std::string, ProfilePoint> active_profiles;
    std::vector<ProfilePoint> completed_profiles;
    std::mutex profile_mutex;
    
public:
    void start_profile(const std::string& name) {
        std::lock_guard<std::mutex> lock(profile_mutex);
        
        ProfilePoint point;
        point.name = name;
        point.start_time = std::chrono::high_resolution_clock::now();
        point.memory_usage = get_current_memory_usage();
        
        active_profiles[name] = point;
    }
    
    void end_profile(const std::string& name) {
        std::lock_guard<std::mutex> lock(profile_mutex);
        
        auto it = active_profiles.find(name);
        if (it != active_profiles.end()) {
            it->second.end_time = std::chrono::high_resolution_clock::now();
            completed_profiles.push_back(it->second);
            active_profiles.erase(it);
        }
    }
    
    void print_report() {
        std::cout << "\n=== Performance Profile Report ===\n";
        
        for (const auto& profile : completed_profiles) {
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                profile.end_time - profile.start_time);
            
            std::cout << "Profile: " << profile.name << "\n";
            std::cout << "  Duration: " << duration.count() << " microseconds\n";
            std::cout << "  Memory: " << profile.memory_usage << " KB\n";
            std::cout << "  Duration (ms): " << duration.count() / 1000.0 << "\n\n";
        }
    }
    
    void save_report(const std::string& filename) {
        std::ofstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Failed to open file: " << filename << std::endl;
            return;
        }
        
        file << "Profile Name,Duration (us),Memory (KB),Duration (ms)\n";
        
        for (const auto& profile : completed_profiles) {
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                profile.end_time - profile.start_time);
            
            file << profile.name << ","
                 << duration.count() << ","
                 << profile.memory_usage << ","
                 << duration.count() / 1000.0 << "\n";
        }
        
        file.close();
        std::cout << "Profile report saved to: " << filename << std::endl;
    }
    
private:
    size_t get_current_memory_usage() {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            return pmc.WorkingSetSize / 1024; // Convert to KB
        }
#else
        FILE* file = fopen("/proc/self/status", "r");
        if (file) {
            char line[128];
            while (fgets(line, 128, file) != NULL) {
                if (strncmp(line, "VmRSS:", 6) == 0) {
                    int memory_kb;
                    sscanf(line, "VmRSS: %d", &memory_kb);
                    fclose(file);
                    return memory_kb;
                }
            }
            fclose(file);
        }
#endif
        return 0;
    }
};

// Global profiler instance
static PerformanceProfiler g_profiler;

// RAII profiler scope
class ProfileScope {
private:
    std::string name;
    
public:
    ProfileScope(const std::string& profile_name) : name(profile_name) {
        g_profiler.start_profile(name);
    }
    
    ~ProfileScope() {
        g_profiler.end_profile(name);
    }
};

// Convenience macro
#define PROFILE_SCOPE(name) ProfileScope profile_scope_##__LINE__(name)

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " [profile|report|save <filename>]\n";
        return 1;
    }
    
    std::string command = argv[1];
    
    if (command == "profile") {
        // Demo profiling
        {
            PROFILE_SCOPE("Demo Operation 1");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        {
            PROFILE_SCOPE("Demo Operation 2");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        
        std::cout << "Profiling completed\n";
        
    } else if (command == "report") {
        g_profiler.print_report();
        
    } else if (command == "save" && argc > 2) {
        g_profiler.save_report(argv[2]);
        
    } else {
        std::cout << "Unknown command: " << command << std::endl;
        return 1;
    }
    
    return 0;
} 