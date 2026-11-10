// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file date.cpp
 * @brief JavaScript Date object implementation
 *
 * Time values are milliseconds since 1970-01-01T00:00:00Z (UTC). Calendar
 * fields come from the proleptic Gregorian day number of that time value
 * (days-from-civil and its inverse), never from the C library, so dates
 * before 1970 or after 2038 work and results do not depend on the host's
 * time_t. The host is only asked for its time zone offset at an instant.
 */

#include "builtins/date.hpp"
#include "builtins/builtin_helpers.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/well_known_symbols.hpp"
#include "runtime/execution/vm.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace Zepra::Builtins {

namespace {

constexpr double kMsPerSecond = 1000.0;
constexpr double kMsPerMinute = 60000.0;
constexpr double kMsPerHour = 3600000.0;
constexpr double kMsPerDay = 86400000.0;
constexpr double kMaxTime = 8.64e15;

const char* const kDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

double nan() { return std::numeric_limits<double>::quiet_NaN(); }

// Days since 1970-01-01 of a proleptic Gregorian date (month 1..12)
double daysFromCivil(double year, double month, double day) {
    year -= month <= 2 ? 1 : 0;
    const double era = std::floor(year / 400);
    const double yoe = year - era * 400;                                      // [0, 399]
    const double mp = std::fmod(month + 9, 12);                               // March = 0
    const double doy = std::floor((153 * mp + 2) / 5) + day - 1;              // [0, 365]
    const double doe = yoe * 365 + std::floor(yoe / 4) - std::floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

struct Civil {
    int year;
    int month;  // 0..11
    int day;    // 1..31
};

Civil civilFromDays(double days) {
    days += 719468;
    const double era = std::floor(days / 146097);
    const double doe = days - era * 146097;                                   // [0, 146096]
    const double yoe = std::floor((doe - std::floor(doe / 1460) + std::floor(doe / 36524) -
                                   std::floor(doe / 146096)) / 365);          // [0, 399]
    const double doy = doe - (365 * yoe + std::floor(yoe / 4) - std::floor(yoe / 100));
    const double mp = std::floor((5 * doy + 2) / 153);                        // March = 0
    const int day = static_cast<int>(doy - std::floor((153 * mp + 2) / 5) + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);           // 1..12
    const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    return {year, month - 1, day};
}

// MakeDay: day number of (year, month 0-based possibly out of range, date)
double makeDay(double year, double month, double date) {
    if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) return nan();
    year = std::trunc(year);
    month = std::trunc(month);
    date = std::trunc(date);
    const double ym = year + std::floor(month / 12);
    const double mn = month - std::floor(month / 12) * 12;
    if (std::fabs(ym) > 400000) return nan();
    return daysFromCivil(ym, mn + 1, 1) + date - 1;
}

double makeTime(double hour, double minute, double second, double ms) {
    if (!std::isfinite(hour) || !std::isfinite(minute) || !std::isfinite(second) || !std::isfinite(ms)) {
        return nan();
    }
    return std::trunc(hour) * kMsPerHour + std::trunc(minute) * kMsPerMinute +
           std::trunc(second) * kMsPerSecond + std::trunc(ms);
}

double makeDate(double day, double time) {
    if (!std::isfinite(day) || !std::isfinite(time)) return nan();
    return day * kMsPerDay + time;
}

double timeClip(double time) {
    if (!std::isfinite(time) || std::fabs(time) > kMaxTime) return nan();
    return std::trunc(time) + 0.0;  // no -0
}

double dayOf(double t) { return std::floor(t / kMsPerDay); }
double timeWithinDay(double t) { return t - dayOf(t) * kMsPerDay; }

// The host's offset from UTC, in ms, at UTC time `t`
double localOffsetAt(double t) {
    if (!std::isfinite(t)) return 0;
    // Outside time_t's comfortable range, use the offset of a year with the
    // same calendar in range (DST rules of that year are the best guess)
    double probe = t;
    if (probe < -2208988800000.0 || probe > 4102444800000.0) {
        const Civil c = civilFromDays(dayOf(t));
        const int year = 2000 + ((c.year % 28) + 28) % 28;
        probe = makeDate(makeDay(year, c.month, c.day), timeWithinDay(t));
    }
    std::time_t seconds = static_cast<std::time_t>(std::floor(probe / 1000));
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    utc.tm_isdst = local.tm_isdst;
    return std::difftime(mktime(&local), mktime(&utc)) * 1000.0;
#else
    localtime_r(&seconds, &local);
    return static_cast<double>(local.tm_gmtoff) * 1000.0;
#endif
}

double localTime(double t) { return t + localOffsetAt(t); }

// UTC(t) for a local time value: the offset is the one in force at the
// resulting instant (refined once, which settles DST transitions)
double utcFromLocal(double local) {
    if (!std::isfinite(local)) return nan();
    double guess = local - localOffsetAt(local);
    return local - localOffsetAt(guess);
}

struct Fields {
    int year, month, day, hours, minutes, seconds, ms, weekday;
};

Fields fieldsOf(double t) {
    const double day = dayOf(t);
    const Civil c = civilFromDays(day);
    const double within = timeWithinDay(t);
    Fields f;
    f.year = c.year;
    f.month = c.month;
    f.day = c.day;
    f.hours = static_cast<int>(within / kMsPerHour);
    f.minutes = static_cast<int>(std::fmod(within / kMsPerMinute, 60));
    f.seconds = static_cast<int>(std::fmod(within / kMsPerSecond, 60));
    f.ms = static_cast<int>(std::fmod(within, 1000));
    f.weekday = static_cast<int>(std::fmod(std::fmod(day + 4, 7) + 7, 7));
    return f;
}

std::string pad(int value, int width) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*d", width, value);
    return buf;
}

std::string yearText(int year) {
    if (year >= 0) return pad(year, 4);
    return "-" + pad(-year, 6);
}

std::string offsetText(double offsetMs, bool colon) {
    const int minutes = static_cast<int>(offsetMs / kMsPerMinute);
    const int a = std::abs(minutes);
    return std::string(minutes < 0 ? "-" : "+") + pad(a / 60, 2) + (colon ? ":" : "") + pad(a % 60, 2);
}

// ---- parsing ---------------------------------------------------------------

// ISO 8601 as the spec defines it: YYYY[-MM[-DD]][THH:mm[:ss[.sss]][Z|±HH:mm]],
// with ±YYYYYY years; also accepts a space for T and lowercase t/z.
// Date-only forms are UTC, date-time forms without an offset are local.
bool parseIso(const std::string& s, double& out) {
    size_t i = 0;
    auto digits = [&](int count, int& value) {
        value = 0;
        for (int k = 0; k < count; ++k) {
            if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            value = value * 10 + (s[i++] - '0');
        }
        return true;
    };
    int sign = 1, year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    double ms = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        sign = s[i++] == '-' ? -1 : 1;
        if (!digits(6, year)) return false;
        if (sign < 0 && year == 0) return false;
    } else if (!digits(4, year)) {
        return false;
    }
    year *= sign;
    if (i < s.size() && s[i] == '-') {
        ++i;
        if (!digits(2, month)) return false;
        if (i < s.size() && s[i] == '-') {
            ++i;
            if (!digits(2, day)) return false;
        }
    }
    bool hasTime = false, hasOffset = false;
    double offsetMs = 0;
    if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        ++i;
        hasTime = true;
        if (!digits(2, hour) || i >= s.size() || s[i] != ':') return false;
        ++i;
        if (!digits(2, minute)) return false;
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!digits(2, second)) return false;
            if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
                ++i;
                double scale = 100;
                size_t start = i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                    ms += (s[i] - '0') * scale;
                    scale /= 10;
                    ++i;
                }
                if (i == start) return false;
                ms = std::floor(ms);
            }
        }
        if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) {
            ++i;
            hasOffset = true;
        } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            const int offSign = s[i++] == '-' ? -1 : 1;
            int oh = 0, om = 0;
            if (!digits(2, oh)) return false;
            if (i < s.size() && s[i] == ':') ++i;
            if (!digits(2, om)) return false;
            hasOffset = true;
            offsetMs = offSign * (oh * kMsPerHour + om * kMsPerMinute);
        }
    }
    if (i != s.size()) return false;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 24 || minute > 59 || second > 59) return false;
    if (hour == 24 && (minute || second || ms)) return false;
    double t = makeDate(makeDay(year, month - 1, day), makeTime(hour, minute, second, ms));
    if (!hasTime || hasOffset) t -= offsetMs;
    else t = utcFromLocal(t);
    out = timeClip(t);
    return true;
}

int monthFromName(const std::string& word) {
    if (word.size() < 3) return -1;
    static const char* const names[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec"};
    for (int m = 0; m < 12; ++m) {
        if (word.compare(0, 3, names[m]) == 0) return m;
    }
    return -1;
}

// The forms browsers accept beyond ISO: "Tue, 01 Jan 2020 10:00:00 GMT",
// "Wed Jan 01 2020 10:00:00 GMT+0100 (CET)", "January 1, 2020 10:00 PM",
// "1 Jan 2020", "2020/01/31 10:00", "01/31/2020" (US month/day/year).
// Without a zone the time is local.
bool parseLegacy(const std::string& text, double& out) {
    int year = -1, month = -1, day = -1;
    int hour = 0, minute = 0, second = 0, ms = 0;
    bool hasZone = false, pm = false, am = false;
    double offsetMs = 0;
    std::vector<int> numbers;

    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '(') {  // comment, e.g. "(Central European Time)"
            int depth = 0;
            while (i < n) {
                if (text[i] == '(') ++depth;
                else if (text[i] == ')' && --depth == 0) { ++i; break; }
                ++i;
            }
            continue;
        }
        if (std::isalpha(c)) {
            std::string word;
            while (i < n && std::isalpha(static_cast<unsigned char>(text[i]))) {
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i++])));
            }
            if (word == "am" || word == "a") { am = true; continue; }
            if (word == "pm" || word == "p") { pm = true; continue; }
            if (word == "z" || word == "gmt" || word == "utc" || word == "ut") {
                hasZone = true;
                continue;
            }
            if (word == "t" && !numbers.empty()) continue;
            static const struct { const char* name; int hours; } zones[] = {
                {"est", -5}, {"edt", -4}, {"cst", -6}, {"cdt", -5},
                {"mst", -7}, {"mdt", -6}, {"pst", -8}, {"pdt", -7}};
            bool zone = false;
            for (const auto& z : zones) {
                if (word == z.name) { hasZone = true; offsetMs = z.hours * kMsPerHour; zone = true; }
            }
            if (zone) continue;
            const int m = monthFromName(word);
            if (m >= 0) { month = m; continue; }
            // weekday names and other words are ignored
            static const char* const days[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
            bool weekday = false;
            for (const char* d : days) if (word.compare(0, 3, d) == 0) weekday = true;
            if (weekday) continue;
            return false;
        }
        if ((c == '+' || c == '-') && i + 1 < n && std::isdigit(static_cast<unsigned char>(text[i + 1])) &&
            (hasZone || hour || minute || numbers.size() >= 3)) {
            // Zone offset: +hhmm, +hh:mm, +hh
            const int sign = c == '-' ? -1 : 1;
            ++i;
            int value = 0, count = 0;
            while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) { value = value * 10 + (text[i++] - '0'); ++count; }
            int hours = value, minutes = 0;
            if (count > 2) { hours = value / 100; minutes = value % 100; }
            if (i < n && text[i] == ':') {
                ++i;
                minutes = 0;
                while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) minutes = minutes * 10 + (text[i++] - '0');
            }
            hasZone = true;
            offsetMs = sign * (hours * kMsPerHour + minutes * kMsPerMinute);
            continue;
        }
        if (std::isdigit(c)) {
            int value = 0;
            size_t start = i;
            while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) value = value * 10 + (text[i++] - '0');
            if (i < n && text[i] == ':') {
                // h:mm[:ss[.ms]]
                hour = value;
                ++i;
                minute = 0;
                while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) minute = minute * 10 + (text[i++] - '0');
                if (i < n && text[i] == ':') {
                    ++i;
                    second = 0;
                    while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) second = second * 10 + (text[i++] - '0');
                    if (i < n && text[i] == '.') {
                        ++i;
                        int scale = 100;
                        while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) {
                            ms += (text[i++] - '0') * scale;
                            scale /= 10;
                        }
                    }
                }
                continue;
            }
            if (i - start >= 3) year = value;  // a 3+ digit number is the year
            else numbers.push_back(value);
            if (i < n && text[i] == '/') ++i;
            continue;
        }
        ++i;  // separators: space , . - /
    }

    // Assign the remaining numbers: "D Month", "Month D", "M/D[/Y]", "Y/M/D"
    if (month >= 0) {
        if (!numbers.empty()) day = numbers[0];
        if (year < 0 && numbers.size() >= 2) year = numbers[1] < 50 ? 2000 + numbers[1] : 1900 + numbers[1];
    } else if (numbers.size() >= 2) {
        if (year >= 0 && text.find_first_of("0123456789") == text.find(std::to_string(year))) {
            month = numbers[0] - 1;  // Y/M/D
            day = numbers[1];
        } else {
            month = numbers[0] - 1;  // M/D/Y
            day = numbers[1];
            if (year < 0 && numbers.size() >= 3) year = numbers[2] < 50 ? 2000 + numbers[2] : 1900 + numbers[2];
        }
    }
    if (year < 0 || month < 0 || month > 11 || day < 1 || day > 31) return false;
    if (pm && hour < 12) hour += 12;
    if (am && hour == 12) hour = 0;
    if (hour > 24 || minute > 59 || second > 59) return false;

    double t = makeDate(makeDay(year, month, day), makeTime(hour, minute, second, ms));
    t = hasZone ? t - offsetMs : utcFromLocal(t);
    out = timeClip(t);
    return true;
}

// ToNumber of a Date component argument (undefined -> NaN)
double numberArg(const std::vector<Value>& args, size_t i, double fallback) {
    if (i >= args.size()) return fallback;
    return args[i].toNumber();
}

DateObject* thisDate(const Runtime::FunctionCallInfo& info, const char* method) {
    DateObject* d = info.thisValue().isObject() ? dynamic_cast<DateObject*>(info.thisValue().asObject()) : nullptr;
    if (!d) throw Runtime::JSError("TypeError", std::string("this is not a Date object. (Date.prototype.") + method + ")");
    return d;
}

std::vector<Value> argsOf(const Runtime::FunctionCallInfo& info) {
    std::vector<Value> args;
    for (size_t i = 0; i < info.argumentCount(); ++i) args.push_back(info.argument(i));
    return args;
}

double nowMs() {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

// =============================================================================
// DateObject
// =============================================================================

DateObject::DateObject()
    : Object(Runtime::ObjectType::Ordinary), timestamp_(nowMs()) {}

DateObject::DateObject(double timestamp)
    : Object(Runtime::ObjectType::Ordinary), timestamp_(timeClip(timestamp)) {}

DateObject::DateObject(int year, int month, int day, int hours, int minutes, int seconds, int ms)
    : Object(Runtime::ObjectType::Ordinary) {
    timestamp_ = timeClip(utcFromLocal(makeDate(makeDay(year, month, day),
                                                 makeTime(hours, minutes, seconds, ms))));
}

void DateObject::setTime(double timestamp) { timestamp_ = timeClip(timestamp); }

#define ZEPRA_DATE_FIELD(getter, field, base)                                           \
    int DateObject::getter() const {                                                    \
        if (std::isnan(timestamp_)) return 0;                                           \
        return fieldsOf(base).field;                                                    \
    }
ZEPRA_DATE_FIELD(getFullYear, year, localTime(timestamp_))
ZEPRA_DATE_FIELD(getMonth, month, localTime(timestamp_))
ZEPRA_DATE_FIELD(getDate, day, localTime(timestamp_))
ZEPRA_DATE_FIELD(getDay, weekday, localTime(timestamp_))
ZEPRA_DATE_FIELD(getHours, hours, localTime(timestamp_))
ZEPRA_DATE_FIELD(getMinutes, minutes, localTime(timestamp_))
ZEPRA_DATE_FIELD(getSeconds, seconds, localTime(timestamp_))
ZEPRA_DATE_FIELD(getMilliseconds, ms, localTime(timestamp_))
ZEPRA_DATE_FIELD(getUTCFullYear, year, timestamp_)
ZEPRA_DATE_FIELD(getUTCMonth, month, timestamp_)
ZEPRA_DATE_FIELD(getUTCDate, day, timestamp_)
ZEPRA_DATE_FIELD(getUTCDay, weekday, timestamp_)
ZEPRA_DATE_FIELD(getUTCHours, hours, timestamp_)
ZEPRA_DATE_FIELD(getUTCMinutes, minutes, timestamp_)
ZEPRA_DATE_FIELD(getUTCSeconds, seconds, timestamp_)
ZEPRA_DATE_FIELD(getUTCMilliseconds, ms, timestamp_)
#undef ZEPRA_DATE_FIELD

int DateObject::getTimezoneOffset() const {
    return static_cast<int>(-localOffsetAt(timestamp_) / kMsPerMinute);
}

std::string DateObject::toDateString() const {
    if (std::isnan(timestamp_)) return "Invalid Date";
    const Fields f = fieldsOf(localTime(timestamp_));
    return std::string(kDayNames[f.weekday]) + " " + kMonthNames[f.month] + " " + pad(f.day, 2) + " " +
           (f.year >= 0 ? pad(f.year, 4) : "-" + pad(-f.year, 6));
}

std::string DateObject::toTimeString() const {
    if (std::isnan(timestamp_)) return "Invalid Date";
    const Fields f = fieldsOf(localTime(timestamp_));
    return pad(f.hours, 2) + ":" + pad(f.minutes, 2) + ":" + pad(f.seconds, 2) + " GMT" +
           offsetText(localOffsetAt(timestamp_), false);
}

std::string DateObject::toString() const {
    if (std::isnan(timestamp_)) return "Invalid Date";
    return toDateString() + " " + toTimeString();
}

std::string DateObject::toISOString() const {
    if (std::isnan(timestamp_)) return "";
    const Fields f = fieldsOf(timestamp_);
    std::string year = (f.year >= 0 && f.year <= 9999) ? pad(f.year, 4)
                     : (f.year < 0 ? "-" : "+") + pad(std::abs(f.year), 6);
    return year + "-" + pad(f.month + 1, 2) + "-" + pad(f.day, 2) + "T" + pad(f.hours, 2) + ":" +
           pad(f.minutes, 2) + ":" + pad(f.seconds, 2) + "." + pad(f.ms, 3) + "Z";
}

std::string DateObject::toUTCString() const {
    if (std::isnan(timestamp_)) return "Invalid Date";
    const Fields f = fieldsOf(timestamp_);
    return std::string(kDayNames[f.weekday]) + ", " + pad(f.day, 2) + " " + kMonthNames[f.month] + " " +
           yearText(f.year) + " " + pad(f.hours, 2) + ":" + pad(f.minutes, 2) + ":" + pad(f.seconds, 2) + " GMT";
}

// en-US forms: "1/31/2020, 2:05:09 PM"
std::string DateObject::toLocaleDateString() const {
    if (std::isnan(timestamp_)) return "Invalid Date";
    const Fields f = fieldsOf(localTime(timestamp_));
    return std::to_string(f.month + 1) + "/" + std::to_string(f.day) + "/" + std::to_string(f.year);
}

std::string DateObject::toLocaleTimeString() const {
    if (std::isnan(timestamp_)) return "Invalid Date";
    const Fields f = fieldsOf(localTime(timestamp_));
    const int h12 = f.hours % 12 == 0 ? 12 : f.hours % 12;
    return std::to_string(h12) + ":" + pad(f.minutes, 2) + ":" + pad(f.seconds, 2) + (f.hours < 12 ? " AM" : " PM");
}

std::string DateObject::toLocaleString() const {
    if (std::isnan(timestamp_)) return "Invalid Date";
    return toLocaleDateString() + ", " + toLocaleTimeString();
}

// =============================================================================
// DateBuiltin
// =============================================================================

double DateBuiltin::parseDate(const std::string& raw) {
    size_t a = raw.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return nan();
    size_t b = raw.find_last_not_of(" \t\r\n");
    const std::string text = raw.substr(a, b - a + 1);
    double t;
    if (parseIso(text, t)) return t;
    if (parseLegacy(text, t)) return t;
    return nan();
}

Value DateBuiltin::constructor(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::object(new DateObject());
    if (args.size() == 1) {
        Value v = args[0];
        if (v.isObject()) {
            if (auto* other = dynamic_cast<DateObject*>(v.asObject())) {
                return Value::object(new DateObject(other->getTime()));
            }
            if (Runtime::VM* vm = Runtime::VM::current()) v = vm->toPrimitiveValue(v, false);
        }
        if (v.isString()) return Value::object(new DateObject(parseDate(v.toString())));
        return Value::object(new DateObject(v.toNumber()));
    }
    double year = numberArg(args, 0, nan());
    if (std::isfinite(year)) {
        const double y = std::trunc(year);
        if (y >= 0 && y <= 99) year = 1900 + y;
    }
    const double t = makeDate(makeDay(year, numberArg(args, 1, 0), numberArg(args, 2, 1)),
                              makeTime(numberArg(args, 3, 0), numberArg(args, 4, 0),
                                       numberArg(args, 5, 0), numberArg(args, 6, 0)));
    return Value::object(new DateObject(utcFromLocal(t)));
}

Value DateBuiltin::now(Runtime::Context*, const std::vector<Value>&) {
    return Value::number(nowMs());
}

Value DateBuiltin::parse(Runtime::Context*, const std::vector<Value>& args) {
    return Value::number(args.empty() ? nan() : parseDate(args[0].toString()));
}

Value DateBuiltin::UTC(Runtime::Context*, const std::vector<Value>& args) {
    double year = numberArg(args, 0, nan());
    if (std::isfinite(year)) {
        const double y = std::trunc(year);
        if (y >= 0 && y <= 99) year = 1900 + y;
    }
    return Value::number(timeClip(makeDate(makeDay(year, numberArg(args, 1, 0), numberArg(args, 2, 1)),
                                           makeTime(numberArg(args, 3, 0), numberArg(args, 4, 0),
                                                    numberArg(args, 5, 0), numberArg(args, 6, 0)))));
}

Object* DateBuiltin::createDatePrototype() {
    Object* proto = new Object();

    auto getter = [proto](const char* name, double (*read)(double t), bool local) {
        defineMethod(proto, name, [read, local, name](const Runtime::FunctionCallInfo& info) -> Value {
            const double t = thisDate(info, name)->getTime();
            if (std::isnan(t)) return Value::number(nan());
            return Value::number(read(local ? localTime(t) : t));
        }, 0);
    };
    for (bool local : {true, false}) {
        getter(local ? "getFullYear" : "getUTCFullYear", [](double t) { return double(fieldsOf(t).year); }, local);
        getter(local ? "getMonth" : "getUTCMonth", [](double t) { return double(fieldsOf(t).month); }, local);
        getter(local ? "getDate" : "getUTCDate", [](double t) { return double(fieldsOf(t).day); }, local);
        getter(local ? "getDay" : "getUTCDay", [](double t) { return double(fieldsOf(t).weekday); }, local);
        getter(local ? "getHours" : "getUTCHours", [](double t) { return double(fieldsOf(t).hours); }, local);
        getter(local ? "getMinutes" : "getUTCMinutes", [](double t) { return double(fieldsOf(t).minutes); }, local);
        getter(local ? "getSeconds" : "getUTCSeconds", [](double t) { return double(fieldsOf(t).seconds); }, local);
        getter(local ? "getMilliseconds" : "getUTCMilliseconds", [](double t) { return double(fieldsOf(t).ms); }, local);
    }
    getter("getYear", [](double t) { return double(fieldsOf(t).year - 1900); }, true);

    auto timeValue = [](const Runtime::FunctionCallInfo& info) -> Value {
        return Value::number(thisDate(info, "getTime")->getTime());
    };
    defineMethod(proto, "getTime", timeValue, 0);
    defineMethod(proto, "valueOf", timeValue, 0);
    defineMethod(proto, "getTimezoneOffset", [](const Runtime::FunctionCallInfo& info) -> Value {
        DateObject* d = thisDate(info, "getTimezoneOffset");
        if (std::isnan(d->getTime())) return Value::number(nan());
        return Value::number(d->getTimezoneOffset());
    }, 0);

    defineMethod(proto, "setTime", [](const Runtime::FunctionCallInfo& info) -> Value {
        DateObject* d = thisDate(info, "setTime");
        d->setTime(info.argument(0).toNumber());
        return Value::number(d->getTime());
    }, 1);

    // Setters: which field the first argument sets (the rest follow in
    // order), local or UTC. A setter on an invalid date leaves it invalid,
    // except setFullYear, which starts from +0.
    enum Field { Year, Month, Day, Hour, Minute, Second, Ms };
    auto setter = [proto](const char* name, Field first, int maxArgs, bool local) {
        defineMethod(proto, name, [name, first, maxArgs, local](const Runtime::FunctionCallInfo& info) -> Value {
            DateObject* d = thisDate(info, name);
            double t = d->getTime();
            if (std::isnan(t)) {
                if (first != Year) return Value::number(nan());
                t = 0;
            } else if (local) {
                t = localTime(t);
            }
            const Fields f = fieldsOf(t);
            double v[7] = {double(f.year), double(f.month), double(f.day), double(f.hours),
                           double(f.minutes), double(f.seconds), double(f.ms)};
            const size_t count = std::max<size_t>(1, std::min<size_t>(info.argumentCount(), maxArgs));
            for (size_t i = 0; i < count; ++i) v[first + i] = info.argument(i).toNumber();
            double result = makeDate(makeDay(v[0], v[1], v[2]), makeTime(v[3], v[4], v[5], v[6]));
            if (local) result = utcFromLocal(result);
            d->setTime(result);
            return Value::number(d->getTime());
        }, static_cast<size_t>(maxArgs));
    };
    for (bool local : {true, false}) {
        setter(local ? "setFullYear" : "setUTCFullYear", Year, 3, local);
        setter(local ? "setMonth" : "setUTCMonth", Month, 2, local);
        setter(local ? "setDate" : "setUTCDate", Day, 1, local);
        setter(local ? "setHours" : "setUTCHours", Hour, 4, local);
        setter(local ? "setMinutes" : "setUTCMinutes", Minute, 3, local);
        setter(local ? "setSeconds" : "setUTCSeconds", Second, 2, local);
        setter(local ? "setMilliseconds" : "setUTCMilliseconds", Ms, 1, local);
    }

    auto text = [proto](const char* name, std::string (DateObject::*format)() const) {
        defineMethod(proto, name, [name, format](const Runtime::FunctionCallInfo& info) -> Value {
            return makeString((thisDate(info, name)->*format)());
        }, 0);
    };
    text("toString", &DateObject::toString);
    text("toDateString", &DateObject::toDateString);
    text("toTimeString", &DateObject::toTimeString);
    text("toUTCString", &DateObject::toUTCString);
    text("toGMTString", &DateObject::toUTCString);
    text("toLocaleString", &DateObject::toLocaleString);
    text("toLocaleDateString", &DateObject::toLocaleDateString);
    text("toLocaleTimeString", &DateObject::toLocaleTimeString);

    defineMethod(proto, "toISOString", [](const Runtime::FunctionCallInfo& info) -> Value {
        DateObject* d = thisDate(info, "toISOString");
        if (std::isnan(d->getTime())) throw Runtime::JSError("RangeError", "Invalid time value");
        return makeString(d->toISOString());
    }, 0);

    // toJSON: null for an invalid date (JSON.stringify(new Date(NaN)) is "null")
    defineMethod(proto, "toJSON", [](const Runtime::FunctionCallInfo& info) -> Value {
        DateObject* d = info.thisValue().isObject() ? dynamic_cast<DateObject*>(info.thisValue().asObject()) : nullptr;
        if (!d) return Value::null();
        if (std::isnan(d->getTime())) return Value::null();
        return makeString(d->toISOString());
    }, 1);

    // [Symbol.toPrimitive](hint): "number" gives the time value, "string"
    // and "default" the toString() text (so date + "" is text, date - 0 a number)
    defineMethod(proto, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToPrimitive),
                 [](const Runtime::FunctionCallInfo& info) -> Value {
        DateObject* d = thisDate(info, "[Symbol.toPrimitive]");
        const std::string hint = info.argument(0).isString() ? info.argument(0).toString() : "";
        if (hint == "number") return Value::number(d->getTime());
        if (hint == "string" || hint == "default") return makeString(d->toString());
        throw Runtime::JSError("TypeError", "Invalid hint: " + hint);
    }, 1);

    return proto;
}

} // namespace Zepra::Builtins
