// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file number_conversion_tests.cpp
 * @brief Number <-> string conversions against the values the ECMAScript
 *        algorithms (and every browser) produce.
 */

#include <gtest/gtest.h>
#include "runtime/objects/number_conversion.hpp"

#include <cmath>
#include <limits>

using namespace Zepra::Runtime::NumberConversion;

namespace {
const double kInf = std::numeric_limits<double>::infinity();
const double kNaN = std::numeric_limits<double>::quiet_NaN();
}

TEST(NumberToString, Integers) {
    EXPECT_EQ(toString(0), "0");
    EXPECT_EQ(toString(-0.0), "0");
    EXPECT_EQ(toString(7), "7");
    EXPECT_EQ(toString(-42), "-42");
    EXPECT_EQ(toString(123456789), "123456789");
    EXPECT_EQ(toString(1700000000000.0), "1700000000000");
    EXPECT_EQ(toString(9007199254740992.0), "9007199254740992");
}

TEST(NumberToString, ShortestRoundTrip) {
    EXPECT_EQ(toString(0.1), "0.1");
    EXPECT_EQ(toString(0.1 + 0.2), "0.30000000000000004");
    EXPECT_EQ(toString(1.0 / 3.0), "0.3333333333333333");
    EXPECT_EQ(toString(4.35), "4.35");
    EXPECT_EQ(toString(123.456), "123.456");
    EXPECT_EQ(toString(-5.5), "-5.5");
}

TEST(NumberToString, LargeAndSmallMagnitudes) {
    EXPECT_EQ(toString(1e20), "100000000000000000000");
    EXPECT_EQ(toString(1e21), "1e+21");
    EXPECT_EQ(toString(123456789012345680000.0), "123456789012345680000");
    EXPECT_EQ(toString(18446744073709551616.0), "18446744073709552000");
    EXPECT_EQ(toString(0.000001), "0.000001");
    EXPECT_EQ(toString(1e-7), "1e-7");
    EXPECT_EQ(toString(1.5e-7), "1.5e-7");
    EXPECT_EQ(toString(-5.5e-10), "-5.5e-10");
    EXPECT_EQ(toString(5e-324), "5e-324");
    EXPECT_EQ(toString(1.7976931348623157e308), "1.7976931348623157e+308");
}

TEST(NumberToString, NonFinite) {
    EXPECT_EQ(toString(kNaN), "NaN");
    EXPECT_EQ(toString(kInf), "Infinity");
    EXPECT_EQ(toString(-kInf), "-Infinity");
}

TEST(NumberToStringRadix, Integers) {
    EXPECT_EQ(toStringRadix(255, 16), "ff");
    EXPECT_EQ(toStringRadix(255, 2), "11111111");
    EXPECT_EQ(toStringRadix(-255, 36), "-73");
    EXPECT_EQ(toStringRadix(0, 2), "0");
    EXPECT_EQ(toStringRadix(35, 36), "z");
    EXPECT_EQ(toStringRadix(1e21, 10), "1e+21");
}

TEST(NumberToStringRadix, Fractions) {
    EXPECT_EQ(toStringRadix(0.5, 2), "0.1");
    EXPECT_EQ(toStringRadix(0.1, 2),
              "0.0001100110011001100110011001100110011001100110011001101");
    EXPECT_EQ(toStringRadix(3.14159265358979, 16).substr(0, 8), "3.243f6a");
    EXPECT_EQ(toStringRadix(-0.75, 2), "-0.11");
}

TEST(NumberToStringRadix, PowersOfTwoPastExactIntegers) {
    EXPECT_EQ(toStringRadix(std::ldexp(1.0, 60), 2), "1" + std::string(60, '0'));
}

TEST(NumberToFixed, RoundsHalfUpOnTheExactValue) {
    EXPECT_EQ(toFixed(1.005, 2), "1.00");      // 1.00499999999999989...
    EXPECT_EQ(toFixed(8.345, 2), "8.35");      // 8.3450000000000006...
    EXPECT_EQ(toFixed(10.235, 2), "10.23");    // 10.2349999999999994...
    EXPECT_EQ(toFixed(1.45, 1), "1.4");
    EXPECT_EQ(toFixed(2.5, 0), "3");           // exact tie goes to the larger value
    EXPECT_EQ(toFixed(0.5, 0), "1");
    EXPECT_EQ(toFixed(-1.5, 0), "-2");
    EXPECT_EQ(toFixed(1234.5678, 0), "1235");
}

TEST(NumberToFixed, Padding) {
    EXPECT_EQ(toFixed(0, 2), "0.00");
    EXPECT_EQ(toFixed(5, 0), "5");
    EXPECT_EQ(toFixed(0.000001, 7), "0.0000010");
    EXPECT_EQ(toFixed(123.456, 1), "123.5");
    EXPECT_EQ(toFixed(0.0001, 2), "0.00");
    EXPECT_EQ(toFixed(-0.0001, 2), "-0.00");
}

TEST(NumberToFixed, OutOfRangeFallsBackToToString) {
    EXPECT_EQ(toFixed(1e21, 2), "1e+21");
    EXPECT_EQ(toFixed(kNaN, 2), "NaN");
    EXPECT_EQ(toFixed(-kInf, 2), "-Infinity");
}

TEST(NumberToExponential, Digits) {
    EXPECT_EQ(toExponential(123456, 2), "1.23e+5");
    EXPECT_EQ(toExponential(123456, -1), "1.23456e+5");
    EXPECT_EQ(toExponential(0.00015, -1), "1.5e-4");
    EXPECT_EQ(toExponential(0, -1), "0e+0");
    EXPECT_EQ(toExponential(0, 2), "0.00e+0");
    EXPECT_EQ(toExponential(-1.5, 0), "-2e+0");
    EXPECT_EQ(toExponential(255, 1), "2.6e+2");
    EXPECT_EQ(toExponential(9.99, 1), "1.0e+1");
}

TEST(NumberToPrecision, FixedAndExponentialForms) {
    EXPECT_EQ(toPrecision(123.456, 4), "123.5");
    EXPECT_EQ(toPrecision(0.000123, 2), "0.00012");
    EXPECT_EQ(toPrecision(123456, 2), "1.2e+5");
    EXPECT_EQ(toPrecision(1.25, 2), "1.3");
    EXPECT_EQ(toPrecision(0, 3), "0.00");
    EXPECT_EQ(toPrecision(1e21, 3), "1.00e+21");
    EXPECT_EQ(toPrecision(99.99, 3), "100");
    EXPECT_EQ(toPrecision(1e-7, 2), "1.0e-7");
    EXPECT_EQ(toPrecision(5, 1), "5");
}

TEST(StringToNumber, Literals) {
    EXPECT_EQ(stringToNumber("12"), 12);
    EXPECT_EQ(stringToNumber("  12  "), 12);
    EXPECT_EQ(stringToNumber("\t\n12\r\n"), 12);
    EXPECT_EQ(stringToNumber(""), 0);
    EXPECT_EQ(stringToNumber("   "), 0);
    EXPECT_EQ(stringToNumber("0x1f"), 31);
    EXPECT_EQ(stringToNumber("0b101"), 5);
    EXPECT_EQ(stringToNumber("0o17"), 15);
    EXPECT_EQ(stringToNumber("1e3"), 1000);
    EXPECT_EQ(stringToNumber(".5"), 0.5);
    EXPECT_EQ(stringToNumber("5."), 5);
    EXPECT_EQ(stringToNumber("+5"), 5);
    EXPECT_EQ(stringToNumber("-2.5e-1"), -0.25);
    EXPECT_EQ(stringToNumber("Infinity"), kInf);
    EXPECT_EQ(stringToNumber("-Infinity"), -kInf);
    EXPECT_EQ(stringToNumber("\xC2\xA0" "12"), 12);  // no-break space
}

TEST(StringToNumber, RejectsAnythingElse) {
    EXPECT_TRUE(std::isnan(stringToNumber("12px")));
    EXPECT_TRUE(std::isnan(stringToNumber("1 2")));
    EXPECT_TRUE(std::isnan(stringToNumber("abc")));
    EXPECT_TRUE(std::isnan(stringToNumber("0x")));
    EXPECT_TRUE(std::isnan(stringToNumber("-0x1f")));
    EXPECT_TRUE(std::isnan(stringToNumber("0b12")));
    EXPECT_TRUE(std::isnan(stringToNumber(".")));
    EXPECT_TRUE(std::isnan(stringToNumber("1e")));
    EXPECT_TRUE(std::isnan(stringToNumber("infinity")));
    EXPECT_TRUE(std::isnan(stringToNumber("1_000")));
}

TEST(ParseFloat, TakesTheLongestPrefix) {
    EXPECT_EQ(parseFloat("3.14abc"), 3.14);
    EXPECT_EQ(parseFloat("  -.5e1x"), -5);
    EXPECT_EQ(parseFloat(".5.5"), 0.5);
    EXPECT_EQ(parseFloat("1e3x"), 1000);
    EXPECT_EQ(parseFloat("1e"), 1);
    EXPECT_EQ(parseFloat("Infinityx"), kInf);
    EXPECT_EQ(parseFloat("0x10"), 0);
    EXPECT_TRUE(std::isnan(parseFloat("abc")));
    EXPECT_TRUE(std::isnan(parseFloat("")));
    EXPECT_TRUE(std::isnan(parseFloat(".e5")));
}

TEST(ParseInt, RadixHandling) {
    EXPECT_EQ(parseInt("42px", 0), 42);
    EXPECT_EQ(parseInt("  -12abc", 0), -12);
    EXPECT_EQ(parseInt("0x1F", 0), 31);
    EXPECT_EQ(parseInt("0x1F", 16), 31);
    EXPECT_EQ(parseInt("0x1F", 10), 0);
    EXPECT_EQ(parseInt("101", 2), 5);
    EXPECT_EQ(parseInt("z", 36), 35);
    EXPECT_EQ(parseInt("12.9", 10), 12);
    EXPECT_TRUE(std::isnan(parseInt("", 10)));
    EXPECT_TRUE(std::isnan(parseInt("abc", 10)));
    EXPECT_TRUE(std::isnan(parseInt("12", 1)));
    EXPECT_TRUE(std::isnan(parseInt("12", 37)));
    EXPECT_TRUE(std::isnan(parseInt("0x", 16)));
}

TEST(ParseInt, LongInputsRoundCorrectly) {
    EXPECT_EQ(parseInt("123456789012345678901234567890", 10), 1.2345678901234568e29);
    EXPECT_TRUE(std::signbit(parseInt("-0", 10)));
}
