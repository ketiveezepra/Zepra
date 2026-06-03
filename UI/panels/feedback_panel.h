// Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file feedback_panel.h
 * @brief In-browser beta feedback and bug report panel.
 *
 * Provides a slide-in overlay panel for the user to submit feedback,
 * bug reports, or feature requests during the beta.
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

namespace zepra {
namespace ui {

/** Category for the feedback submission. */
enum class FeedbackCategory {
    BugReport,       ///< Something is broken
    PerformanceIssue,///< Slow, memory leak, crash
    FeatureRequest,  ///< I want feature X
    UIFeedback,      ///< Layout, design, usability
    CompatIssue,     ///< Website does not render correctly
    GeneralFeedback  ///< Other comments
};

/** Severity rating for bug reports. */
enum class FeedbackSeverity {
    Low,     ///< Cosmetic / minor inconvenience
    Medium,  ///< Noticeably impacts usage
    High,    ///< Major feature broken
    Critical ///< Crash / data loss
};

/** Data collected from the user. */
struct FeedbackData {
    FeedbackCategory category  = FeedbackCategory::BugReport;
    FeedbackSeverity severity  = FeedbackSeverity::Medium;
    std::string      title;        ///< Short one-line summary
    std::string      description;  ///< Detailed description
    std::string      stepsToRepro; ///< Steps to reproduce (for bugs)
    std::string      currentUrl;   ///< URL the user was on when reporting
    bool             includeSysInfo = true;  ///< Attach OS/version metadata
    bool             includeConsoleLog = false; ///< Attach recent console output

    // Auto-filled metadata (not editable by user)
    std::string      browserVersion;
    std::string      osName;
    std::string      buildChannel;
    std::string      buildDate;
};

/** Result of a submission attempt. */
struct FeedbackResult {
    bool        success = false;
    std::string ticketId;   ///< Issue ID returned by the server, if any
    std::string message;    ///< Human-readable success/error message
};

using FeedbackSubmitCallback = std::function<void(const FeedbackResult&)>;

/**
 * @brief FeedbackPanel — slide-in overlay for beta feedback collection.
 *
 * Renders as a right-side slide panel over the browser content.
 * Users can select a category, describe the issue, and submit directly
 * to the KetiveeAI beta feedback endpoint.
 */
class FeedbackPanel {
public:
    FeedbackPanel();
    ~FeedbackPanel();

    // Non-copyable
    FeedbackPanel(const FeedbackPanel&) = delete;
    FeedbackPanel& operator=(const FeedbackPanel&) = delete;

    // -------------------------------------------------------------------------
    // Visibility
    // -------------------------------------------------------------------------

    /** Open the feedback panel (slides in from the right). */
    void open();

    /** Close the panel (slides out). */
    void close();

    /** Toggle open/closed state. */
    void toggle();

    /** True when the panel is visible or animating into view. */
    bool isOpen() const;

    // -------------------------------------------------------------------------
    // Context
    // -------------------------------------------------------------------------

    /** Pre-fill the URL field with the page the user is currently on. */
    void setCurrentUrl(const std::string& url);

    /** Attach the most recent console log lines (optional, user opt-in). */
    void setConsoleLog(const std::vector<std::string>& lines);

    // -------------------------------------------------------------------------
    // Submission
    // -------------------------------------------------------------------------

    /**
     * Set the callback invoked when the user clicks Submit.
     * The callback receives the result of the async submission.
     */
    void setSubmitCallback(FeedbackSubmitCallback cb);

    // -------------------------------------------------------------------------
    // Rendering & Events
    // -------------------------------------------------------------------------

    void setBounds(float x, float y, float w, float h);
    void render();
    void update(float deltaMs);

    bool handleMouseClick(float x, float y);
    bool handleMouseMove(float x, float y);
    bool handleMouseDown(float x, float y);
    bool handleMouseUp(float x, float y);
    bool handleMouseScroll(float x, float y, float delta);
    bool handleKeyPress(int keyCode, bool ctrl, bool shift, bool alt);
    bool handleTextInput(const std::string& text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ui
} // namespace zepra
