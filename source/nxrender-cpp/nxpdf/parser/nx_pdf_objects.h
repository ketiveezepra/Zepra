/**
 * @file nx_pdf_objects.h
 * @brief PDF object graph implementation primitives
 */

#pragma once

#include <string>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string_view>
#include <cstdint>

namespace nxrender {
namespace pdf {
namespace parser {

enum class PdfObjectType {
    Null,
    Bool,
    Integer,
    Real,
    Name,
    String,
    Array,
    Dictionary,
    Stream,
    Reference
};

class PdfObject {
public:
    virtual ~PdfObject() = default;
    virtual PdfObjectType GetType() const = 0;
};

class PdfNull : public PdfObject {
public:
    PdfObjectType GetType() const override { return PdfObjectType::Null; }
};

class PdfBool : public PdfObject {
public:
    explicit PdfBool(bool val) : value(val) {}
    PdfObjectType GetType() const override { return PdfObjectType::Bool; }
    bool value;
};

class PdfInteger : public PdfObject {
public:
    explicit PdfInteger(int val) : value(val) {}
    PdfObjectType GetType() const override { return PdfObjectType::Integer; }
    int value;
};

class PdfReal : public PdfObject {
public:
    explicit PdfReal(double val) : value(val) {}
    PdfObjectType GetType() const override { return PdfObjectType::Real; }
    double value;
};

class PdfName : public PdfObject {
public:
    explicit PdfName(std::string_view val) : value(val) {}
    PdfObjectType GetType() const override { return PdfObjectType::Name; }
    std::string value;
};

class PdfString : public PdfObject {
public:
    explicit PdfString(std::string_view val) : value(val) {}
    PdfObjectType GetType() const override { return PdfObjectType::String; }
    std::string value; 
};

class PdfArray : public PdfObject {
public:
    PdfObjectType GetType() const override { return PdfObjectType::Array; }
    std::vector<std::unique_ptr<PdfObject>> items;
};

class PdfDictionary : public PdfObject {
public:
    PdfObjectType GetType() const override { return PdfObjectType::Dictionary; }
    std::unordered_map<std::string, std::unique_ptr<PdfObject>> map;
};

class PdfStream : public PdfObject {
public:
    explicit PdfStream(std::unique_ptr<PdfDictionary> dict, std::vector<uint8_t> streamData) 
        : dictionary(std::move(dict)), data(std::move(streamData)) {}
    PdfObjectType GetType() const override { return PdfObjectType::Stream; }
    
    std::unique_ptr<PdfDictionary> dictionary;
    std::vector<uint8_t> data; // Independent allocation for edit safety
};

class PdfReference : public PdfObject {
public:
    PdfReference(int objNum, int genNum) : objectNumber(objNum), generationNumber(genNum) {}
    PdfObjectType GetType() const override { return PdfObjectType::Reference; }
    
    int objectNumber;
    int generationNumber;
};

} // namespace parser
} // namespace pdf
} // namespace nxrender
