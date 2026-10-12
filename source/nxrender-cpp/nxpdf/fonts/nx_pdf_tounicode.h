/**
 * @file nx_pdf_tounicode.h
 * @brief ToUnicode CMap conversion arrays
 */

#pragma once

#include "../parser/nx_pdf_objects.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace nxrender {
namespace pdf {
namespace fonts {

class ToUnicodeCMap {
public:
    explicit ToUnicodeCMap(const std::vector<uint8_t>& cmapBuffer);
    ~ToUnicodeCMap() = default;

    std::string MapCodeToUnicode(int code) const;
    bool IsTwoByte() const { return isTwoByte_; }

private:
    void ParseBuffer(const std::vector<uint8_t>& cmapBuffer);
    
    std::unordered_map<int, std::string> mapping_;
    bool isTwoByte_ = false;
};

} // namespace fonts
} // namespace pdf
} // namespace nxrender
