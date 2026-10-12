/**
 * @file nx_pdf_image.h
 * @brief XObject Image data abstraction
 */

#pragma once

#include "../parser/nx_pdf_objects.h"
#include <string>
#include <vector>
#include <cstdint>

namespace nxrender {
namespace pdf {
namespace images {

class PdfImage {
public:
    explicit PdfImage(parser::PdfStream* stream);
    ~PdfImage() = default;

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    int GetBitsPerComponent() const { return bpc_; }
    int GetComponentCount() const;

    const std::string& GetFilter() const { return filter_; }
    const std::string& GetColorSpace() const { return colorSpace_; }
    bool HasSoftMask() const { return hasSoftMask_; }
    bool IsImageMask() const { return isImageMask_; }
    bool IsInterpolated() const { return interpolate_; }

    const std::vector<uint8_t>& GetRawData() const;
    size_t GetRawDataSize() const;
    size_t GetStrideBytes() const;

    // Decode raw data to RGBA format
    std::vector<uint8_t> DecodeToRGBA() const;
    size_t GetDecodedSize() const;

private:
    void ParseAttributes(parser::PdfDictionary* dict);

    parser::PdfStream* stream_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int bpc_ = 8;
    std::string colorSpace_ = "DeviceRGB";
    std::string filter_;
    std::string intent_;
    bool hasSoftMask_ = false;
    bool isImageMask_ = false;
    bool interpolate_ = false;
    std::vector<double> decodeArray_;
};

} // namespace images
} // namespace pdf
} // namespace nxrender
