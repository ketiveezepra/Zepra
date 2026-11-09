// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file intl_bindings.cpp
 * @brief The global Intl object
 *
 * Script-facing Intl: NumberFormat, DateTimeFormat, Collator, PluralRules,
 * ListFormat, RelativeTimeFormat, Segmenter, DisplayNames and Locale, plus
 * Intl.getCanonicalLocales / supportedValuesOf.
 *
 * There is no CLDR data in the engine. Number and date formatting follow
 * en-US patterns, with per-language separators and date-field order for the
 * common locales, which is what pages mostly rely on (thousands separators,
 * currency, compact "1.2K", "Jan 5, 2024", resolvedOptions().timeZone).
 * Locale negotiation and the simpler services reuse Runtime::Intl*.
 */

#include "builtins/intl_bindings.hpp"
#include "builtins/intl.hpp"
#include "builtins/builtin_helpers.hpp"
#include "builtins/date.hpp"
#include "builtins/list_iterator.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/string_utf16.hpp"
#include "runtime/objects/well_known_symbols.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <unistd.h>

namespace Zepra::Builtins {

using Runtime::Array;
using Runtime::Context;
using Runtime::FunctionCallInfo;
using Runtime::JSError;

namespace {

// ---- option reading ---------------------------------------------------------

Value optionValue(const Value& options, const char* name) {
    if (!options.isObject()) return Value::undefined();
    if (Runtime::VM* vm = Runtime::VM::current()) return vm->getMember(options, name);
    return options.asObject()->get(name);
}

std::string stringOption(const Value& options, const char* name, const std::string& fallback = "") {
    Value v = optionValue(options, name);
    return v.isUndefined() ? fallback : v.toString();
}

int intOption(const Value& options, const char* name, int fallback, int lo, int hi) {
    Value v = optionValue(options, name);
    if (v.isUndefined()) return fallback;
    double d = v.toNumber();
    if (std::isnan(d) || d < lo || d > hi) throw JSError("RangeError", std::string(name) + " value is out of range.");
    return static_cast<int>(std::floor(d));
}

std::vector<std::string> localeList(const Value& locales) {
    std::vector<std::string> out;
    if (locales.isUndefined()) return out;
    if (locales.isString()) {
        out.push_back(locales.toString());
    } else if (locales.isObject()) {
        if (auto* locale = dynamic_cast<Runtime::IntlLocale*>(locales.asObject())) {
            out.push_back(locale->toString());
        } else if (auto* array = dynamic_cast<Array*>(locales.asObject())) {
            for (size_t i = 0; i < array->length(); ++i) out.push_back(array->at(i).toString());
        }
    }
    return Runtime::Locale::getCanonicalLocales(out);
}

// The locale a service uses: the first requested one the engine knows
// (by language), else the default
std::string resolveLocale(const Value& locales) {
    std::vector<std::string> requested = localeList(locales);
    if (requested.empty()) {
        std::string def = Runtime::IntlUtils::defaultLocale();
        if (def.empty() || def == "C" || def == "POSIX" || def == "C-UTF-8") def = "en-US";
        return def;
    }
    for (const auto& tag : requested) {
        Runtime::Locale locale = Runtime::Locale::parse(tag);
        for (const auto& known : Runtime::IntlUtils::availableLocales()) {
            if (Runtime::Locale::parse(known).language == locale.language) return tag;
        }
    }
    return "en-US";
}

std::string languageOf(const std::string& tag) { return Runtime::Locale::parse(tag).language; }

Value makeArray(const std::vector<std::string>& items) {
    std::vector<Value> values;
    for (const auto& s : items) values.push_back(makeString(s));
    return Value::object(new Array(std::move(values)));
}

Value partsArray(const std::vector<std::pair<std::string, std::string>>& parts) {
    std::vector<Value> values;
    for (const auto& [type, value] : parts) {
        auto* part = new Object();
        part->set("type", makeString(type));
        part->set("value", makeString(value));
        values.push_back(Value::object(part));
    }
    return Value::object(new Array(std::move(values)));
}

// A function bound to `self`, as the spec's format/compare getters return
Value boundMethod(Object* self, const std::string& name,
                  std::function<Value(Object*, const FunctionCallInfo&)> body, size_t arity) {
    return Value::object(new Runtime::Function(name,
        [self, body](const FunctionCallInfo& info) -> Value { return body(self, info); }, arity));
}

template <typename T>
T* thisAs(const FunctionCallInfo& info, const char* what) {
    T* object = info.thisValue().isObject() ? dynamic_cast<T*>(info.thisValue().asObject()) : nullptr;
    if (!object) throw JSError("TypeError", std::string("Method ") + what + " called on incompatible receiver");
    return object;
}

// ---- number formatting -------------------------------------------------------

struct Separators {
    std::string group = ",";
    std::string decimal = ".";
};

Separators separatorsFor(const std::string& language) {
    static const std::map<std::string, Separators> table = {
        {"de", {".", ","}}, {"es", {".", ","}}, {"it", {".", ","}}, {"nl", {".", ","}},
        {"pt", {".", ","}}, {"id", {".", ","}}, {"tr", {".", ","}}, {"da", {".", ","}},
        {"fr", {" ", ","}}, {"ru", {" ", ","}}, {"pl", {" ", ","}},
        {"sv", {" ", ","}}, {"nb", {" ", ","}}, {"fi", {" ", ","}},
        {"cs", {" ", ","}}, {"uk", {" ", ","}},
    };
    auto it = table.find(language);
    return it != table.end() ? it->second : Separators{};
}

class NumberFormat : public Object {
public:
    std::string locale;
    std::string style = "decimal";
    std::string currency, currencyDisplay = "symbol", unit, unitDisplay = "short";
    std::string notation = "standard", compactDisplay = "short", signDisplay = "auto";
    std::string useGrouping = "auto";
    int minimumIntegerDigits = 1;
    int minimumFractionDigits = 0, maximumFractionDigits = 3;
    int minimumSignificantDigits = 0, maximumSignificantDigits = 0;  // 0: not used
    Separators separators;

    NumberFormat() : Object(Runtime::ObjectType::Ordinary) {}

    // Digits of |value| rounded per the options: integer and fraction parts
    void digits(double value, std::string& integer, std::string& fraction) const {
        char buf[512];
        if (maximumSignificantDigits > 0) {
            std::snprintf(buf, sizeof(buf), "%.*e", maximumSignificantDigits - 1, value);
            double rounded = std::strtod(buf, nullptr);
            int exponent = rounded == 0 ? 0 : static_cast<int>(std::floor(std::log10(rounded)));
            int fractionDigits = std::max(0, maximumSignificantDigits - 1 - exponent);
            std::snprintf(buf, sizeof(buf), "%.*f", fractionDigits, rounded);
            split(buf, integer, fraction);
            const int minFraction = std::max(0, minimumSignificantDigits - 1 - exponent);
            trimFraction(fraction, minFraction);
        } else {
            std::snprintf(buf, sizeof(buf), "%.*f", maximumFractionDigits, value);
            split(buf, integer, fraction);
            trimFraction(fraction, minimumFractionDigits);
        }
        while (static_cast<int>(integer.size()) < minimumIntegerDigits) integer = "0" + integer;
    }

    static void split(const char* text, std::string& integer, std::string& fraction) {
        std::string s(text);
        size_t dot = s.find('.');
        integer = s.substr(0, dot);
        fraction = dot == std::string::npos ? "" : s.substr(dot + 1);
    }

    static void trimFraction(std::string& fraction, int minimum) {
        while (static_cast<int>(fraction.size()) > minimum && !fraction.empty() && fraction.back() == '0') {
            fraction.pop_back();
        }
    }

    std::string grouped(const std::string& integer) const {
        if (useGrouping == "false" || (useGrouping == "min2" && integer.size() < 5)) return integer;
        // Spanish and Polish leave four-digit numbers ungrouped
        if ((languageOf(locale) == "es" || languageOf(locale) == "pl") && integer.size() <= 4) return integer;
        std::string out;
        int count = 0;
        for (size_t i = integer.size(); i-- > 0;) {
            if (count && count % 3 == 0) out.insert(0, separators.group);
            out.insert(out.begin(), integer[i]);
            ++count;
        }
        return out;
    }

    std::string currencySymbol() const {
        if (currencyDisplay == "code") return currency;
        if (currencyDisplay == "name") return Runtime::IntlUtils::getCurrencyInfo(currency).name;
        const auto& info = Runtime::IntlUtils::getCurrencyInfo(currency);
        std::string symbol = currencyDisplay == "narrowSymbol" ? info.narrowSymbol : info.symbol;
        return symbol.empty() ? currency : symbol;
    }

    std::vector<std::pair<std::string, std::string>> parts(double value) const {
        std::vector<std::pair<std::string, std::string>> out;
        const bool negative = std::signbit(value) && !(value == 0 && signDisplay == "auto");
        double magnitude = std::fabs(value);
        if (style == "percent") magnitude *= 100;

        std::string suffix;
        if (notation == "compact" && std::isfinite(magnitude)) {
            static const struct { double scale; const char* shortName; const char* longName; } steps[] = {
                {1e12, "T", " trillion"}, {1e9, "B", " billion"}, {1e6, "M", " million"}, {1e3, "K", " thousand"}};
            for (const auto& step : steps) {
                if (magnitude >= step.scale) {
                    magnitude /= step.scale;
                    suffix = compactDisplay == "long" ? step.longName : step.shortName;
                    break;
                }
            }
        }

        const bool showSign = signDisplay == "always" || (signDisplay == "exceptZero" && value != 0);
        if (negative && signDisplay != "never") out.push_back({"minusSign", "-"});
        else if (showSign && !negative) out.push_back({"plusSign", "+"});

        const bool currencyFirst = style == "currency" && separators.decimal == ".";
        if (currencyFirst) out.push_back({"currency", currencySymbol()});

        if (std::isnan(value)) {
            out.push_back({"nan", "NaN"});
        } else if (std::isinf(value)) {
            out.push_back({"infinity", "∞"});
        } else {
            std::string integer, fraction;
            digits(magnitude, integer, fraction);
            out.push_back({"integer", grouped(integer)});
            if (!fraction.empty()) {
                out.push_back({"decimal", separators.decimal});
                out.push_back({"fraction", fraction});
            }
        }
        if (!suffix.empty()) out.push_back({"compact", suffix});
        if (style == "percent") out.push_back({"percentSign", "%"});
        if (style == "currency" && !currencyFirst) {
            out.push_back({"literal", " "});
            out.push_back({"currency", currencySymbol()});
        }
        if (style == "unit") {
            const auto& info = Runtime::IntlUtils::getUnitInfo(unit);
            std::string name = unitDisplay == "long" ? info.longName : unitDisplay == "narrow" ? info.narrowName : info.shortName;
            if (name.empty()) name = unit;
            if (unitDisplay != "narrow") out.push_back({"literal", " "});
            out.push_back({"unit", name});
        }
        return out;
    }

    std::string format(double value) const {
        std::string out;
        for (const auto& part : parts(value)) out += part.second;
        return out;
    }
};

double numberArgument(const Value& v) {
    if (v.isObject()) {
        if (Runtime::VM* vm = Runtime::VM::current()) return vm->toPrimitiveValue(v, false).toNumber();
    }
    return v.toNumber();
}

Value makeNumberFormat(const Value& locales, const Value& options) {
    auto* nf = new NumberFormat();
    nf->locale = resolveLocale(locales);
    nf->separators = separatorsFor(languageOf(nf->locale));
    nf->style = stringOption(options, "style", "decimal");
    nf->currency = stringOption(options, "currency");
    std::transform(nf->currency.begin(), nf->currency.end(), nf->currency.begin(), ::toupper);
    if (nf->style == "currency" && nf->currency.empty()) throw JSError("TypeError", "Currency code is required with currency style.");
    nf->currencyDisplay = stringOption(options, "currencyDisplay", "symbol");
    nf->unit = stringOption(options, "unit");
    nf->unitDisplay = stringOption(options, "unitDisplay", "short");
    nf->notation = stringOption(options, "notation", "standard");
    nf->compactDisplay = stringOption(options, "compactDisplay", "short");
    nf->signDisplay = stringOption(options, "signDisplay", "auto");
    Value grouping = optionValue(options, "useGrouping");
    nf->useGrouping = grouping.isUndefined() ? (nf->notation == "compact" ? "min2" : "auto")
                    : grouping.isBoolean() ? (grouping.asBoolean() ? "always" : "false") : grouping.toString();
    nf->minimumIntegerDigits = intOption(options, "minimumIntegerDigits", 1, 1, 21);
    const int defaultMin = nf->style == "currency" ? 2 : 0;
    const int defaultMax = nf->style == "currency" ? 2 : nf->style == "percent" ? 0 : 3;
    nf->minimumFractionDigits = intOption(options, "minimumFractionDigits", defaultMin, 0, 100);
    nf->maximumFractionDigits = intOption(options, "maximumFractionDigits",
                                          std::max(nf->minimumFractionDigits, defaultMax), 0, 100);
    if (nf->maximumFractionDigits < nf->minimumFractionDigits) {
        if (optionValue(options, "maximumFractionDigits").isUndefined()) nf->maximumFractionDigits = nf->minimumFractionDigits;
        else throw JSError("RangeError", "maximumFractionDigits value is out of range.");
    }
    if (!optionValue(options, "maximumSignificantDigits").isUndefined() ||
        !optionValue(options, "minimumSignificantDigits").isUndefined()) {
        nf->minimumSignificantDigits = intOption(options, "minimumSignificantDigits", 1, 1, 21);
        nf->maximumSignificantDigits = intOption(options, "maximumSignificantDigits", 21, nf->minimumSignificantDigits, 21);
    } else if (nf->notation == "compact" && optionValue(options, "maximumFractionDigits").isUndefined()) {
        // compact rounds to 2 significant digits for small mantissas ("1.2K", "12K", "123K")
        nf->minimumSignificantDigits = 1;
        nf->maximumSignificantDigits = 2;
    }

    nf->set("format", boundMethod(nf, "format", [](Object* self, const FunctionCallInfo& info) {
        return makeString(static_cast<NumberFormat*>(self)->format(numberArgument(info.argument(0))));
    }, 1));
    return Value::object(nf);
}

Value numberFormatResolvedOptions(NumberFormat* nf) {
    auto* o = new Object();
    o->set("locale", makeString(nf->locale));
    o->set("numberingSystem", makeString("latn"));
    o->set("style", makeString(nf->style));
    if (nf->style == "currency") {
        o->set("currency", makeString(nf->currency));
        o->set("currencyDisplay", makeString(nf->currencyDisplay));
    }
    if (nf->style == "unit") {
        o->set("unit", makeString(nf->unit));
        o->set("unitDisplay", makeString(nf->unitDisplay));
    }
    o->set("minimumIntegerDigits", Value::number(nf->minimumIntegerDigits));
    o->set("minimumFractionDigits", Value::number(nf->minimumFractionDigits));
    o->set("maximumFractionDigits", Value::number(nf->maximumFractionDigits));
    if (nf->maximumSignificantDigits > 0) {
        o->set("minimumSignificantDigits", Value::number(nf->minimumSignificantDigits));
        o->set("maximumSignificantDigits", Value::number(nf->maximumSignificantDigits));
    }
    o->set("useGrouping", nf->useGrouping == "false" ? Value::boolean(false) : makeString(nf->useGrouping));
    o->set("notation", makeString(nf->notation));
    if (nf->notation == "compact") o->set("compactDisplay", makeString(nf->compactDisplay));
    o->set("signDisplay", makeString(nf->signDisplay));
    return Value::object(o);
}

// ---- date formatting ------------------------------------------------------------

// The host's IANA zone name ("Europe/Berlin"), from TZ or /etc/localtime
std::string hostTimeZone() {
    static std::string cached = [] {
        if (const char* tz = std::getenv("TZ")) {
            std::string s = tz;
            if (!s.empty() && s[0] == ':') s = s.substr(1);
            if (s.find('/') != std::string::npos && s[0] != '/') return s;
            if (s == "UTC" || s == "GMT") return std::string("UTC");
        }
#ifndef _WIN32
        char buf[512];
        ssize_t n = readlink("/etc/localtime", buf, sizeof(buf) - 1);
        if (n > 0) {
            std::string link(buf, static_cast<size_t>(n));
            size_t at = link.find("zoneinfo/");
            if (at != std::string::npos) return link.substr(at + 9);
        }
        std::ifstream tzfile("/etc/timezone");
        std::string line;
        if (tzfile && std::getline(tzfile, line) && !line.empty()) return line;
#endif
        return std::string("UTC");
    }();
    return cached;
}

bool isUtcZone(const std::string& zone) {
    return zone == "UTC" || zone == "Etc/UTC" || zone == "GMT" || zone == "Etc/GMT" || zone == "Z" ||
           zone == "Etc/Universal" || zone == "Universal" || zone == "Zulu";
}

const char* const kMonthLong[] = {"January", "February", "March", "April", "May", "June", "July",
                                  "August", "September", "October", "November", "December"};
const char* const kMonthShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const kDayLong[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kDayShort[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

class DateTimeFormat : public Object {
public:
    std::string locale, timeZone, calendar = "gregory";
    std::string weekday, era, year, month, day, hour, minute, second, timeZoneName;
    std::string dateStyle, timeStyle, hourCycle;
    int fractionalSecondDigits = 0;
    bool utc = false;

    DateTimeFormat() : Object(Runtime::ObjectType::Ordinary) {}

    struct Fields { int year, month, day, weekday, hour, minute, second, ms; double offsetMinutes; };

    Fields fieldsAt(double t) const {
        DateObject d(t);
        if (utc) {
            return {d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCDay(), d.getUTCHours(),
                    d.getUTCMinutes(), d.getUTCSeconds(), d.getUTCMilliseconds(), 0};
        }
        return {d.getFullYear(), d.getMonth(), d.getDate(), d.getDay(), d.getHours(), d.getMinutes(),
                d.getSeconds(), d.getMilliseconds(), -static_cast<double>(d.getTimezoneOffset())};
    }

    static std::string two(int v) { char b[8]; std::snprintf(b, sizeof(b), "%02d", v); return b; }

    std::vector<std::pair<std::string, std::string>> parts(double t) const {
        std::vector<std::pair<std::string, std::string>> out;
        if (std::isnan(t)) throw JSError("RangeError", "Invalid time value");
        const Fields f = fieldsAt(t);
        const std::string lang = languageOf(locale);
        const bool usOrder = lang == "en" && Runtime::Locale::parse(locale).region != "GB" &&
                             Runtime::Locale::parse(locale).region != "AU" && Runtime::Locale::parse(locale).region != "IN";
        const bool ymd = lang == "ja" || lang == "zh" || lang == "ko" || lang == "sv" || lang == "hu" || lang == "lt";
        const char* numericSep = (lang == "de" || lang == "ru" || lang == "pl" || lang == "fi" || lang == "cs" ||
                                  lang == "nb" || lang == "tr" || lang == "uk") ? "." : (ymd && lang == "sv") ? "-" : "/";

        std::string y = year, mo = month, d = day, wd = weekday, h = hour, mi = minute, s = second;
        if (!dateStyle.empty()) {
            if (dateStyle == "full") { wd = "long"; y = "numeric"; mo = "long"; d = "numeric"; }
            else if (dateStyle == "long") { y = "numeric"; mo = "long"; d = "numeric"; }
            else if (dateStyle == "medium") { y = "numeric"; mo = "short"; d = "numeric"; }
            else { y = "2-digit"; mo = "numeric"; d = "numeric"; }
        }
        if (!timeStyle.empty()) {
            h = "numeric"; mi = "2-digit";
            if (timeStyle != "short") s = "2-digit";
        }
        if (dateStyle.empty() && timeStyle.empty() && y.empty() && mo.empty() && d.empty() && wd.empty() &&
            h.empty() && mi.empty() && s.empty()) {
            y = "numeric"; mo = "numeric"; d = "numeric";
        }

        auto lit = [&](const std::string& text) { out.push_back({"literal", text}); };
        const bool hasDate = !y.empty() || !mo.empty() || !d.empty();
        const bool textMonth = mo == "long" || mo == "short" || mo == "narrow";

        if (!wd.empty()) {
            out.push_back({"weekday", wd == "long" ? kDayLong[f.weekday] : wd == "narrow"
                                     ? std::string(1, kDayLong[f.weekday][0]) : kDayShort[f.weekday]});
            if (hasDate) lit(", ");
        }
        auto yearText = [&]() { return y == "2-digit" ? two(((f.year % 100) + 100) % 100) : std::to_string(f.year); };
        auto dayText = [&]() { return d == "2-digit" ? two(f.day) : std::to_string(f.day); };
        if (hasDate) {
            if (textMonth) {
                std::string name = mo == "long" ? kMonthLong[f.month] : mo == "narrow"
                                 ? std::string(1, kMonthLong[f.month][0]) : kMonthShort[f.month];
                if (usOrder || ymd) {
                    out.push_back({"month", name});
                    if (!d.empty()) { lit(" "); out.push_back({"day", dayText()}); }
                    if (!y.empty()) { lit(d.empty() ? " " : ", "); out.push_back({"year", yearText()}); }
                } else {
                    if (!d.empty()) { out.push_back({"day", dayText()}); lit(lang == "de" ? ". " : " "); }
                    out.push_back({"month", name});
                    if (!y.empty()) { lit(" "); out.push_back({"year", yearText()}); }
                }
            } else {
                std::string m = mo == "2-digit" ? two(f.month + 1) : std::to_string(f.month + 1);
                std::vector<std::pair<std::string, std::string>> fields;
                if (ymd) {
                    if (!y.empty()) fields.push_back({"year", yearText()});
                    if (!mo.empty()) fields.push_back({"month", lang == "sv" ? two(f.month + 1) : m});
                    if (!d.empty()) fields.push_back({"day", lang == "sv" ? two(f.day) : dayText()});
                } else if (usOrder) {
                    if (!mo.empty()) fields.push_back({"month", m});
                    if (!d.empty()) fields.push_back({"day", dayText()});
                    if (!y.empty()) fields.push_back({"year", yearText()});
                } else {
                    const bool pad = lang == "en" || lang == "fr" || lang == "es" || lang == "it" || lang == "pt" || lang == "ru";
                    if (!d.empty()) fields.push_back({"day", pad && d == "numeric" ? two(f.day) : dayText()});
                    if (!mo.empty()) fields.push_back({"month", pad && mo == "numeric" ? two(f.month + 1) : m});
                    if (!y.empty()) fields.push_back({"year", yearText()});
                }
                for (size_t i = 0; i < fields.size(); ++i) {
                    if (i) lit(numericSep);
                    out.push_back(fields[i]);
                }
            }
        }
        if (!h.empty() || !mi.empty() || !s.empty()) {
            if (hasDate || !wd.empty()) lit(usOrder && !dateStyle.empty() && (dateStyle == "full" || dateStyle == "long") ? " at " : ", ");
            bool twelve = hourCycle.empty() ? (lang == "en" && usOrder) || lang == "hi" || lang == "ar" || lang == "ko"
                                            : (hourCycle == "h12" || hourCycle == "h11");
            int hourValue = f.hour;
            if (twelve) hourValue = f.hour % 12 == 0 ? 12 : f.hour % 12;
            if (!h.empty()) {
                out.push_back({"hour", (h == "2-digit" || !twelve) ? two(hourValue) : std::to_string(hourValue)});
            }
            if (!mi.empty()) {
                if (!h.empty()) lit(":");
                out.push_back({"minute", two(f.minute)});
            }
            if (!s.empty()) {
                if (!h.empty() || !mi.empty()) lit(":");
                out.push_back({"second", two(f.second)});
                if (fractionalSecondDigits > 0) {
                    lit(".");
                    char b[8];
                    std::snprintf(b, sizeof(b), "%03d", f.ms);
                    out.push_back({"fractionalSecond", std::string(b, static_cast<size_t>(fractionalSecondDigits))});
                }
            }
            if (twelve && !h.empty()) {
                lit(" ");
                out.push_back({"dayPeriod", f.hour < 12 ? "AM" : "PM"});
            }
            std::string zoneStyle = timeZoneName;
            if (zoneStyle.empty() && (timeStyle == "full" || timeStyle == "long")) zoneStyle = "short";
            if (!zoneStyle.empty()) {
                lit(" ");
                std::string zone = "UTC";
                if (!utc) {
                    const int total = static_cast<int>(f.offsetMinutes);
                    zone = "GMT";
                    if (total) {
                        zone += total < 0 ? "-" : "+";
                        zone += std::to_string(std::abs(total) / 60);
                        if (std::abs(total) % 60) zone += ":" + two(std::abs(total) % 60);
                    }
                }
                out.push_back({"timeZoneName", zone});
            }
        }
        return out;
    }

    std::string format(double t) const {
        std::string out;
        for (const auto& p : parts(t)) out += p.second;
        return out;
    }
};

double dateArgument(const Value& v) {
    if (v.isUndefined()) {
        return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    if (v.isObject()) {
        if (auto* d = dynamic_cast<DateObject*>(v.asObject())) return d->getTime();
    }
    return numberArgument(v);
}

Value makeDateTimeFormat(const Value& locales, const Value& options) {
    auto* f = new DateTimeFormat();
    f->locale = resolveLocale(locales);
    f->weekday = stringOption(options, "weekday");
    f->era = stringOption(options, "era");
    f->year = stringOption(options, "year");
    f->month = stringOption(options, "month");
    f->day = stringOption(options, "day");
    f->hour = stringOption(options, "hour");
    f->minute = stringOption(options, "minute");
    f->second = stringOption(options, "second");
    f->timeZoneName = stringOption(options, "timeZoneName");
    f->dateStyle = stringOption(options, "dateStyle");
    f->timeStyle = stringOption(options, "timeStyle");
    f->fractionalSecondDigits = intOption(options, "fractionalSecondDigits", 0, 1, 3);
    f->hourCycle = stringOption(options, "hourCycle");
    Value hour12 = optionValue(options, "hour12");
    if (!hour12.isUndefined()) f->hourCycle = hour12.toBoolean() ? "h12" : "h23";
    std::string zone = stringOption(options, "timeZone");
    if (zone.empty()) zone = hostTimeZone();
    // Without a time zone database, zones other than UTC format in host local time
    f->utc = isUtcZone(zone);
    if (isUtcZone(zone)) zone = "UTC";
    f->timeZone = zone;
    f->set("format", boundMethod(f, "format", [](Object* self, const FunctionCallInfo& info) {
        return makeString(static_cast<DateTimeFormat*>(self)->format(dateArgument(info.argument(0))));
    }, 1));
    return Value::object(f);
}

// ---- constructors ---------------------------------------------------------------

// Defines Intl.<name> with `prototype`; `make(locales, options)` builds an instance
Object* defineService(Object* intl, Object* objectPrototype, const char* name,
                      std::function<Value(const Value&, const Value&)> make) {
    auto* proto = new Object();
    proto->setPrototype(objectPrototype);
    auto* ctor = Runtime::createNativeFunction(name,
        [proto, make](Context*, const std::vector<Value>& args) -> Value {
            Value instance = make(args.size() > 0 ? args[0] : Value::undefined(),
                                  args.size() > 1 ? args[1] : Value::undefined());
            if (instance.isObject()) instance.asObject()->setPrototype(proto);
            return instance;
        }, 0);
    defineHidden(ctor, "prototype", Value::object(proto));
    defineHidden(proto, "constructor", Value::object(ctor));
    defineHidden(proto, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag),
                 makeString(std::string("Intl.") + name));
    defineMethod(ctor, "supportedLocalesOf", [](const FunctionCallInfo& info) -> Value {
        std::vector<std::string> out;
        for (const auto& tag : localeList(info.argument(0))) {
            const std::string lang = languageOf(tag);
            for (const auto& known : Runtime::IntlUtils::availableLocales()) {
                if (languageOf(known) == lang) { out.push_back(tag); break; }
            }
        }
        return makeArray(out);
    }, 1);
    defineHidden(intl, name, Value::object(ctor));
    return proto;
}

} // namespace

Object* createIntlObject(Object* objectPrototype) {
    auto* intl = new Object();
    intl->setPrototype(objectPrototype);
    defineHidden(intl, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag), makeString("Intl"));

    defineMethod(intl, "getCanonicalLocales", [](const FunctionCallInfo& info) -> Value {
        return makeArray(localeList(info.argument(0)));
    }, 1);
    defineMethod(intl, "supportedValuesOf", [](const FunctionCallInfo& info) -> Value {
        return makeArray(Runtime::IntlObject::supportedValuesOf(info.argument(0).toString()));
    }, 1);

    // NumberFormat
    Object* nfProto = defineService(intl, objectPrototype, "NumberFormat", makeNumberFormat);
    defineMethod(nfProto, "formatToParts", [](const FunctionCallInfo& info) -> Value {
        return partsArray(thisAs<NumberFormat>(info, "formatToParts")->parts(numberArgument(info.argument(0))));
    }, 1);
    defineMethod(nfProto, "formatRange", [](const FunctionCallInfo& info) -> Value {
        auto* nf = thisAs<NumberFormat>(info, "formatRange");
        return makeString(nf->format(numberArgument(info.argument(0))) + "–" + nf->format(numberArgument(info.argument(1))));
    }, 2);
    defineMethod(nfProto, "resolvedOptions", [](const FunctionCallInfo& info) -> Value {
        return numberFormatResolvedOptions(thisAs<NumberFormat>(info, "resolvedOptions"));
    }, 0);

    // DateTimeFormat
    Object* dtfProto = defineService(intl, objectPrototype, "DateTimeFormat", makeDateTimeFormat);
    defineMethod(dtfProto, "formatToParts", [](const FunctionCallInfo& info) -> Value {
        return partsArray(thisAs<DateTimeFormat>(info, "formatToParts")->parts(dateArgument(info.argument(0))));
    }, 1);
    defineMethod(dtfProto, "formatRange", [](const FunctionCallInfo& info) -> Value {
        auto* f = thisAs<DateTimeFormat>(info, "formatRange");
        return makeString(f->format(dateArgument(info.argument(0))) + " – " + f->format(dateArgument(info.argument(1))));
    }, 2);
    defineMethod(dtfProto, "resolvedOptions", [](const FunctionCallInfo& info) -> Value {
        auto* f = thisAs<DateTimeFormat>(info, "resolvedOptions");
        auto* o = new Object();
        o->set("locale", makeString(f->locale));
        o->set("calendar", makeString("gregory"));
        o->set("numberingSystem", makeString("latn"));
        o->set("timeZone", makeString(f->timeZone));
        const char* names[] = {"weekday", "era", "year", "month", "day", "hour", "minute", "second", "timeZoneName",
                               "dateStyle", "timeStyle"};
        const std::string* values[] = {&f->weekday, &f->era, &f->year, &f->month, &f->day, &f->hour, &f->minute,
                                       &f->second, &f->timeZoneName, &f->dateStyle, &f->timeStyle};
        bool any = false;
        for (size_t i = 0; i < 11; ++i) {
            if (!values[i]->empty()) { o->set(names[i], makeString(*values[i])); any = true; }
        }
        if (!any) {
            o->set("year", makeString("numeric"));
            o->set("month", makeString("numeric"));
            o->set("day", makeString("numeric"));
        }
        if (!f->hour.empty() || !f->timeStyle.empty()) {
            const bool twelve = f->hourCycle.empty() ? languageOf(f->locale) == "en" : f->hourCycle == "h12" || f->hourCycle == "h11";
            o->set("hourCycle", makeString(twelve ? "h12" : "h23"));
            o->set("hour12", Value::boolean(twelve));
        }
        return Value::object(o);
    }, 0);

    // Collator
    Object* collatorProto = defineService(intl, objectPrototype, "Collator", [](const Value& locales, const Value& options) {
        Runtime::CollatorOptions opts;
        opts.usage = stringOption(options, "usage", "sort");
        opts.sensitivity = stringOption(options, "sensitivity", "variant");
        opts.numeric = optionValue(options, "numeric").toBoolean();
        opts.ignorePunctuation = optionValue(options, "ignorePunctuation").toBoolean();
        opts.caseFirst = stringOption(options, "caseFirst", "false");
        std::vector<std::string> list = localeList(locales);
        auto* collator = new Runtime::IntlCollator(list, opts);
        collator->set("compare", boundMethod(collator, "compare", [](Object* self, const FunctionCallInfo& info) {
            int r = static_cast<Runtime::IntlCollator*>(self)->compare(info.argument(0).toString(), info.argument(1).toString());
            return Value::number(r < 0 ? -1 : r > 0 ? 1 : 0);
        }, 2));
        return Value::object(collator);
    });
    defineMethod(collatorProto, "resolvedOptions", [](const FunctionCallInfo& info) -> Value {
        return Value::object(thisAs<Runtime::IntlCollator>(info, "resolvedOptions")->resolvedOptions());
    }, 0);

    // PluralRules
    Object* pluralProto = defineService(intl, objectPrototype, "PluralRules", [](const Value& locales, const Value& options) {
        std::map<std::string, Value> opts;
        if (!optionValue(options, "type").isUndefined()) opts["type"] = makeString(stringOption(options, "type"));
        return Value::object(new Runtime::IntlPluralRules(localeList(locales), opts));
    });
    defineMethod(pluralProto, "select", [](const FunctionCallInfo& info) -> Value {
        return makeString(thisAs<Runtime::IntlPluralRules>(info, "select")->select(numberArgument(info.argument(0))));
    }, 1);
    defineMethod(pluralProto, "resolvedOptions", [](const FunctionCallInfo& info) -> Value {
        auto* rules = thisAs<Runtime::IntlPluralRules>(info, "resolvedOptions");
        auto* o = new Object();
        o->set("locale", makeString(resolveLocale(Value::undefined())));
        o->set("pluralCategories", makeArray(rules->resolvedOptions()));
        return Value::object(o);
    }, 0);

    // ListFormat
    Object* listProto = defineService(intl, objectPrototype, "ListFormat", [](const Value& locales, const Value& options) {
        std::map<std::string, Value> opts;
        if (!optionValue(options, "type").isUndefined()) opts["type"] = makeString(stringOption(options, "type"));
        if (!optionValue(options, "style").isUndefined()) opts["style"] = makeString(stringOption(options, "style"));
        return Value::object(new Runtime::IntlListFormat(localeList(locales), opts));
    });
    auto listItems = [](const Value& v) {
        std::vector<std::string> items;
        if (v.isObject()) {
            if (auto* array = dynamic_cast<Array*>(v.asObject())) {
                for (size_t i = 0; i < array->length(); ++i) items.push_back(array->at(i).toString());
            }
        }
        return items;
    };
    defineMethod(listProto, "format", [listItems](const FunctionCallInfo& info) -> Value {
        return makeString(thisAs<Runtime::IntlListFormat>(info, "format")->format(listItems(info.argument(0))));
    }, 1);
    defineMethod(listProto, "formatToParts", [listItems](const FunctionCallInfo& info) -> Value {
        return partsArray(thisAs<Runtime::IntlListFormat>(info, "formatToParts")->formatToParts(listItems(info.argument(0))));
    }, 1);

    // RelativeTimeFormat
    Object* rtfProto = defineService(intl, objectPrototype, "RelativeTimeFormat", [](const Value& locales, const Value& options) {
        std::map<std::string, Value> opts;
        if (!optionValue(options, "style").isUndefined()) opts["style"] = makeString(stringOption(options, "style"));
        if (!optionValue(options, "numeric").isUndefined()) opts["numeric"] = makeString(stringOption(options, "numeric"));
        return Value::object(new Runtime::IntlRelativeTimeFormat(localeList(locales), opts));
    });
    defineMethod(rtfProto, "format", [](const FunctionCallInfo& info) -> Value {
        std::string unit = info.argument(1).toString();
        if (unit.size() > 1 && unit.back() == 's') unit.pop_back();
        return makeString(thisAs<Runtime::IntlRelativeTimeFormat>(info, "format")->format(numberArgument(info.argument(0)), unit));
    }, 2);
    defineMethod(rtfProto, "formatToParts", [](const FunctionCallInfo& info) -> Value {
        std::string unit = info.argument(1).toString();
        if (unit.size() > 1 && unit.back() == 's') unit.pop_back();
        return partsArray(thisAs<Runtime::IntlRelativeTimeFormat>(info, "formatToParts")->formatToParts(numberArgument(info.argument(0)), unit));
    }, 2);

    // Segmenter: segment(text) is iterable over { segment, index, input }
    Object* segProto = defineService(intl, objectPrototype, "Segmenter", [](const Value& locales, const Value& options) {
        std::map<std::string, Value> opts;
        if (!optionValue(options, "granularity").isUndefined()) opts["granularity"] = makeString(stringOption(options, "granularity"));
        return Value::object(new Runtime::IntlSegmenter(localeList(locales), opts));
    });
    defineMethod(segProto, "segment", [](const FunctionCallInfo& info) -> Value {
        const std::string text = info.argument(0).toString();
        std::vector<std::string> pieces = thisAs<Runtime::IntlSegmenter>(info, "segment")->segment(text);
        std::vector<Value> segments;
        size_t index = 0;
        for (const auto& piece : pieces) {
            auto* seg = new Object();
            seg->set("segment", makeString(piece));
            seg->set("index", Value::number(static_cast<double>(index)));
            seg->set("input", makeString(text));
            segments.push_back(Value::object(seg));
            index += Runtime::Utf16::length(piece);
        }
        auto* result = new Array(segments);
        defineMethod(result, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator),
                     [segments](const FunctionCallInfo&) -> Value {
            return IteratorBuiltin::makeListIterator(segments);
        }, 0);
        return Value::object(result);
    }, 1);

    // DisplayNames
    Object* dnProto = defineService(intl, objectPrototype, "DisplayNames", [](const Value& locales, const Value& options) {
        std::map<std::string, Value> opts;
        for (const char* key : {"type", "style", "fallback", "languageDisplay"}) {
            if (!optionValue(options, key).isUndefined()) opts[key] = makeString(stringOption(options, key));
        }
        return Value::object(new Runtime::IntlDisplayNames(localeList(locales), opts));
    });
    defineMethod(dnProto, "of", [](const FunctionCallInfo& info) -> Value {
        return makeString(thisAs<Runtime::IntlDisplayNames>(info, "of")->of(info.argument(0).toString()));
    }, 1);

    // Locale
    Object* localeProto = defineService(intl, objectPrototype, "Locale", [](const Value& tag, const Value&) {
        std::string text = tag.isObject() && dynamic_cast<Runtime::IntlLocale*>(tag.asObject())
                         ? static_cast<Runtime::IntlLocale*>(tag.asObject())->toString() : tag.toString();
        auto* locale = new Runtime::IntlLocale(text);
        locale->set("baseName", makeString(locale->baseName()));
        locale->set("language", makeString(locale->language()));
        if (!locale->script().empty()) locale->set("script", makeString(locale->script()));
        if (!locale->region().empty()) locale->set("region", makeString(locale->region()));
        return Value::object(locale);
    });
    defineMethod(localeProto, "toString", [](const FunctionCallInfo& info) -> Value {
        return makeString(thisAs<Runtime::IntlLocale>(info, "toString")->toString());
    }, 0);
    defineMethod(localeProto, "maximize", [localeProto](const FunctionCallInfo& info) -> Value {
        Runtime::IntlLocale* max = thisAs<Runtime::IntlLocale>(info, "maximize")->maximize();
        max->setPrototype(localeProto);
        max->set("baseName", makeString(max->baseName()));
        max->set("language", makeString(max->language()));
        return Value::object(max);
    }, 0);
    defineMethod(localeProto, "minimize", [localeProto](const FunctionCallInfo& info) -> Value {
        Runtime::IntlLocale* min = thisAs<Runtime::IntlLocale>(info, "minimize")->minimize();
        min->setPrototype(localeProto);
        min->set("baseName", makeString(min->baseName()));
        min->set("language", makeString(min->language()));
        return Value::object(min);
    }, 0);

    return intl;
}

} // namespace Zepra::Builtins
