#pragma once

/**
 * @file date.hpp
 * @brief JavaScript Date object
 *
 * A Date holds one time value: milliseconds since the epoch in UTC, or NaN
 * for an invalid date. Calendar arithmetic follows the spec's day/time
 * decomposition (ECMA-262 21.4.1) on that number; local time is UTC shifted
 * by the host time zone's offset at that instant.
 */

#include "../config.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <ctime>
#include <chrono>
#include <string>

namespace Zepra::Runtime { class Context; }

namespace Zepra::Builtins {

using Runtime::Value;
using Runtime::Object;

/**
 * @brief JavaScript Date object
 */
class DateObject : public Object {
public:
    DateObject();                        // now
    explicit DateObject(double timestamp);
    // Local-time components, as new Date(y, m, d, h, min, s, ms)
    DateObject(int year, int month, int day = 1, int hours = 0,
               int minutes = 0, int seconds = 0, int ms = 0);

    int getFullYear() const;
    int getMonth() const;
    int getDate() const;
    int getDay() const;
    int getHours() const;
    int getMinutes() const;
    int getSeconds() const;
    int getMilliseconds() const;
    double getTime() const { return timestamp_; }

    int getUTCFullYear() const;
    int getUTCMonth() const;
    int getUTCDate() const;
    int getUTCDay() const;
    int getUTCHours() const;
    int getUTCMinutes() const;
    int getUTCSeconds() const;
    int getUTCMilliseconds() const;

    void setTime(double timestamp);      // TimeClip'd

    std::string toString() const;
    std::string toDateString() const;
    std::string toTimeString() const;
    std::string toISOString() const;     // "" for an invalid date
    std::string toUTCString() const;
    std::string toLocaleString() const;
    std::string toLocaleDateString() const;
    std::string toLocaleTimeString() const;

    int getTimezoneOffset() const;

private:
    double timestamp_;  // ms since epoch (UTC), NaN when invalid
};

/**
 * @brief Date constructor and prototype
 */
class DateBuiltin {
public:
    static Value constructor(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value now(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value parse(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value UTC(Runtime::Context* ctx, const std::vector<Value>& args);

    // Date.parse: ISO 8601 (the spec's Date Time String Format) and the
    // common RFC 2822 / toString / "Month D, YYYY" forms; NaN otherwise
    static double parseDate(const std::string& text);

    static Object* createDatePrototype();
};

} // namespace Zepra::Builtins
