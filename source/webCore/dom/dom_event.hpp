#pragma once

/**
 * @file dom_event.hpp
 * @brief Minimal DOMEvent stub for the JS-DOM bridge (beta)
 *
 * This is a placeholder to satisfy the bridge header. A full event
 * system will be implemented post-beta.
 */

#include <string>
#include <cstdint>

// The embedding API's small DOM; its own namespace, see dom_node.hpp
namespace Zepra::WebCore::Mini {

class DOMEvent {
public:
    explicit DOMEvent(const std::string& type) : type_(type) {}
    virtual ~DOMEvent() = default;

    const std::string& type() const { return type_; }
    bool bubbles() const { return bubbles_; }
    bool cancelable() const { return cancelable_; }
    bool defaultPrevented() const { return defaultPrevented_; }

    void preventDefault() { defaultPrevented_ = true; }
    void stopPropagation() { /* placeholder */ }
    void stopImmediatePropagation() { /* placeholder */ }

private:
    std::string type_;
    bool bubbles_ = true;
    bool cancelable_ = true;
    bool defaultPrevented_ = false;
};

} // namespace Zepra::WebCore::Mini
