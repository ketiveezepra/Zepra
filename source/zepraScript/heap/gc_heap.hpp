#pragma once

/**
 * @file gc_heap.hpp
 * @brief Generational garbage collector for ZebraScript
 * 
 * Two-generation mark-sweep collector:
 * - Young: Bump-pointer allocation, minor GC
 * - Old: Free-list allocation, major GC (mark-sweep)
 */

#include "../config.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_set>
#include <functional>
#include <memory>

namespace Zepra::Runtime {

class Object;

/**
 * @brief GC generation types
 */
enum class Generation : uint8_t {
    Young = 0,
    Old = 1
};

/**
 * @brief Object header for GC metadata
 * 
 * Prepended to every GC-managed object.
 */
struct ObjectHeader {
    // GC state
    bool marked = false;
    Generation generation = Generation::Young;
    uint8_t age = 0;  // Survives count, promote at 2
    
    // Object size (for sweep)
    uint32_t size = 0;
    
    // Intrusive list for generation tracking
    ObjectHeader* next = nullptr;
    ObjectHeader* prev = nullptr;
    
    // Get the object following this header
    void* object() { return reinterpret_cast<char*>(this) + sizeof(ObjectHeader); }
    
    // Get header from object pointer
    static ObjectHeader* fromObject(void* obj) {
        return reinterpret_cast<ObjectHeader*>(
            reinterpret_cast<char*>(obj) - sizeof(ObjectHeader)
        );
    }
};

/**
 * @brief GC statistics
 */
struct GCStats {
    size_t totalAllocated = 0;
    size_t totalFreed = 0;
    size_t objectsAllocated = 0;
    size_t objectsFreed = 0;
    size_t minorCollections = 0;
    size_t majorCollections = 0;
    size_t promotions = 0;
    size_t youngGenSize = 0;
    size_t oldGenSize = 0;
};

/**
 * @brief Generational garbage collector heap
 */
class GCHeap {
public:
    // Young generation size: 2MB default
    static constexpr size_t YOUNG_GEN_SIZE = 2 * 1024 * 1024;
    // Promotion threshold: survive 2 minor GCs
    static constexpr uint8_t PROMOTION_AGE = 2;
    
    GCHeap();
    ~GCHeap();
    
    // Disable copy/move
    GCHeap(const GCHeap&) = delete;
    GCHeap& operator=(const GCHeap&) = delete;
    
    /**
     * @brief Allocate object in young generation
     * @tparam T Object type (must derive from Object)
     * @param args Constructor arguments
     * @return Pointer to new object
     */
    template<typename T, typename... Args>
    T* allocate(Args&&... args);
    
    /**
     * @brief Collect young generation (minor GC)
     */
    void collectMinor();
    
    /**
     * @brief Collect all generations (major GC)
     */
    void collectMajor();
    
    /**
     * @brief Add a root pointer (stack variable, global)
     */
    void addRoot(Object** root);
    
    /**
     * @brief Remove a root pointer
     */
    void removeRoot(Object** root);
    
    /**
     * @brief Write barrier for old → young references
     * Call when old-gen object stores reference to young-gen object
     */
    void writeBarrier(Object* source, Object* target);
    
    /**
     * @brief Register tracer function for object type
     * Tracer visits all object references for marking
     */
    using TraceFn = std::function<void(Object*, std::function<void(Object*)>)>;
    void registerTracer(TraceFn tracer);
    
    /**
     * @brief Get GC statistics
     */
    const GCStats& stats() const { return stats_; }
    
    /**
     * @brief Force a collection if memory pressure
     */
    void maybeCollect();
    
    /**
     * @brief Set root scanner callback for VM globals/stack/frames.
     * Called during markFromRoots() to discover live objects not tracked
     * via explicit addRoot() calls.
     */
    using RootScannerFn = std::function<void(std::function<void(Object*)>)>;
    void setRootScanner(RootScannerFn scanner) { rootScanner_ = std::move(scanner); }
    
private:
    // Young generation (bump-pointer)
    char* youngStart_ = nullptr;
    char* youngEnd_ = nullptr;
    char* youngPtr_ = nullptr;  // Next allocation point
    
    // Object lists by generation
    ObjectHeader* youngList_ = nullptr;
    ObjectHeader* oldList_ = nullptr;
    
    // Root set (stack, globals, handles)
    std::unordered_set<Object**> roots_;
    
    // Remembered set (old → young references)
    std::unordered_set<Object*> rememberedSet_;
    
    // Object tracers
    std::vector<TraceFn> tracers_;
    
    // Statistics
    GCStats stats_;
    
    // Internal methods
    void* allocateYoung(size_t size);
    void* allocateOld(size_t size);
    void markFromRoots();
    void markObject(Object* obj);
    void sweepYoung();
    void sweepOld();
    void promoteObject(ObjectHeader* header);
    void traceObject(Object* obj);
    
    // Root scanner callback (VM globals, stack)
    RootScannerFn rootScanner_;
};

// Template implementation
template<typename T, typename... Args>
T* GCHeap::allocate(Args&&... args) {
    size_t totalSize = sizeof(ObjectHeader) + sizeof(T);
    
    // Allocate in young generation
    void* mem = allocateYoung(totalSize);
    if (!mem) {
        // Young gen full, trigger minor GC
        collectMinor();
        mem = allocateYoung(totalSize);
        if (!mem) {
            // Still no space, allocate in old gen
            mem = allocateOld(totalSize);
        }
    }
    
    if (!mem) {
        return nullptr;  // OOM
    }
    
    // Initialize header
    ObjectHeader* header = new (mem) ObjectHeader();
    header->size = sizeof(T);
    header->generation = Generation::Young;
    header->age = 0;
    header->marked = false;
    
    // Add to young list
    header->next = youngList_;
    if (youngList_) youngList_->prev = header;
    youngList_ = header;
    
    // Construct object after header
    T* obj = new (header->object()) T(std::forward<Args>(args)...);
    obj->setGCManaged(true);
    
    stats_.objectsAllocated++;
    stats_.totalAllocated += totalSize;
    stats_.youngGenSize += totalSize;
    
    return obj;
}

} // namespace Zepra::Runtime
