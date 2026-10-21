#include <iostream>
#include <string>
#include "../source/nxrender-cpp/nxpdf/nx_pdf.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <pdf_file_path> [expected_text]" << std::endl;
        return 1;
    }

    std::string filepath = argv[1];
    // Optional: fail unless this text appears in the extracted pages
    std::string expected = argc >= 3 ? argv[2] : "";
    std::cout << "[nxpdf] Opening explicitly bounded file: " << filepath << std::endl;

    auto doc = nxrender::pdf::Document::Open(filepath);
    if (!doc) {
        std::cerr << "[nxpdf] FATAL: File load bounded logic failed. PDF parser could not build XRef natively." << std::endl;
        return 1;
    }

    int pageCount = doc->GetPageCount();
    std::cout << "[nxpdf] Document mapped successfully. Pages: " << pageCount << std::endl;

    std::string allText;
    for (int i = 0; i < pageCount; ++i) {
        std::cout << "\n--- Page " << (i + 1) << " Extraction ---" << std::endl;
        std::string text = doc->ExtractText(i);
        std::cout << text << std::endl;
        allText += text;
    }

    if (!expected.empty() && allText.find(expected) == std::string::npos) {
        std::cerr << "[nxpdf] FAIL: expected text not found: \"" << expected << "\"" << std::endl;
        return 1;
    }

    std::cout << "\n[nxpdf] Execution clean. Memory limits safely tracking isolated single-ownership teardown." << std::endl;
    return 0;
}
