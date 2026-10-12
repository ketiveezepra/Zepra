/**
 * @file nx_pdf_document.h
 * @brief Top-level document model and binary parsing architecture
 */

#pragma once

#include "nx_pdf_xref.h"
#include "nx_pdf_lexer.h"
#include <string_view>
#include <memory>
#include <vector>
#include <cstdint>

namespace nxrender {
namespace pdf {
namespace parser {

// Exposes zlib FlateDecode manually for explicit stream inflation
std::vector<uint8_t> DecodeFlateData(const std::vector<uint8_t>& compressedData);

enum class PdfErrorCode {
    None,
    InvalidFile,
    NoTrailer,
    ParseError,
    UnsupportedFeature // specifically PDF 1.5 ObjStm
};

class PdfDocumentParser {
public:
    explicit PdfDocumentParser(std::string_view data);
    ~PdfDocumentParser() = default;

    // Bootstraps parsing of tail and XRef indexing
    PdfErrorCode Parse();

    // Triggered by XRefTable when encountering unresolved object bytes
    std::unique_ptr<PdfObject> ParseObjectAtOffset(size_t offset);
    
    XRefTable& GetXRefTable() { return xrefTable_; }
    PdfDictionary* GetTrailer() const { return trailer_.get(); }

private:
    PdfErrorCode ReadTrailerAndStartXRef(size_t& outXRefOffset);
    PdfErrorCode ParseXRefSection(size_t startXRefOffset);

    // Deep token recursive structure parser
    std::unique_ptr<PdfObject> ParseExpression();

    std::string_view fileData_;
    Lexer lexer_;
    XRefTable xrefTable_;
    
    std::unique_ptr<PdfDictionary> trailer_;
};

} // namespace parser
} // namespace pdf
} // namespace nxrender
