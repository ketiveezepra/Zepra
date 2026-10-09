#include <iostream>
#include <cassert>
#include "../include/common/types.h"
#include "../include/common/constants.h"
#include "../include/engine/html_parser.h"
#include "../include/search/ketivee_search.h"

using namespace zepra;

void testTypes() {
    std::cout << "🧪 Testing basic types..." << std::endl;
    
    String testString = "Hello, Zepra!";
    assert(testString == "Hello, Zepra!");
    
    Color testColor = {255, 128, 64, 255};
    assert(testColor.r == 255);
    assert(testColor.g == 128);
    assert(testColor.b == 64);
    assert(testColor.a == 255);
    
    std::cout << "✅ Basic types test passed" << std::endl;
}

void testConstants() {
    std::cout << "🧪 Testing constants..." << std::endl;
    
    assert(BROWSER_NAME == "Zepra Browser");
    assert(BROWSER_VERSION == "1.0.0");
    assert(DEFAULT_WINDOW_WIDTH == 1200);
    assert(DEFAULT_WINDOW_HEIGHT == 800);
    
    std::cout << "✅ Constants test passed" << std::endl;
}

void testHTMLParser() {
    std::cout << "🧪 Testing HTML parser..." << std::endl;
    
    HTMLParser parser;
    String testHtml = R"(
        <!DOCTYPE html>
        <html>
        <head>
            <title>Test Page</title>
        </head>
        <body>
            <h1>Welcome to Zepra</h1>
            <p>This is a test page.</p>
            <a href="https://ketivee.org">Visit Ketivee</a>
        </body>
        </html>
    )";
    
    DocumentNode* document = parser.parse(testHtml);
    assert(document != nullptr);
    
    // Test title extraction
    if (document->head) {
        auto titleElements = document->head->getElementsByTagName("title");
        if (!titleElements.empty()) {
            String title = titleElements[0]->getTextContent();
            assert(title == "Test Page");
        }
    }
    
    // Test link extraction
    if (document->body) {
        auto links = document->body->getElementsByTagName("a");
        assert(links.size() == 1);
        
        if (auto element = std::dynamic_pointer_cast<ElementNode>(links[0])) {
            String href = element->getAttribute("href");
            assert(href == "https://ketivee.org");
        }
    }
    
    delete document;
    std::cout << "✅ HTML parser test passed" << std::endl;
}

void testSearchEngine() {
    std::cout << "🧪 Testing search engine..." << std::endl;
    
    auto searchEngine = std::make_shared<KetiveeSearchEngine>();
    assert(searchEngine->getName() == "KetiveeSearch");
    
    // Test basic search
    auto results = searchEngine->search("Zepra browser");
    assert(!results.empty());
    
    // Test suggestions
    auto suggestions = searchEngine->getSuggestions("Zepra");
    assert(!suggestions.empty());
    
    // Test trending searches
    auto trending = searchEngine->getTrendingSearches();
    assert(!trending.empty());
    
    std::cout << "✅ Search engine test passed" << std::endl;
}

void testSearchQuery() {
    std::cout << "🧪 Testing search query..." << std::endl;
    
    SearchQuery query("test query");
    query.language = "en";
    query.region = "US";
    query.maxResults = 10;
    query.safeSearch = true;
    
    assert(query.query == "test query");
    assert(query.language == "en");
    assert(query.region == "US");
    assert(query.maxResults == 10);
    assert(query.safeSearch == true);
    
    std::cout << "✅ Search query test passed" << std::endl;
}

void testSearchResult() {
    std::cout << "🧪 Testing search result..." << std::endl;
    
    SearchResult result(
        "Test Title",
        "https://example.com",
        "This is a test description"
    );
    
    result.snippet = "Test snippet";
    result.thumbnail = "https://example.com/thumb.jpg";
    result.relevance = 0.85f;
    result.source = "test";
    result.timestamp = 1234567890;
    
    assert(result.title == "Test Title");
    assert(result.url == "https://example.com");
    assert(result.description == "This is a test description");
    assert(result.snippet == "Test snippet");
    assert(result.thumbnail == "https://example.com/thumb.jpg");
    assert(result.relevance == 0.85f);
    assert(result.source == "test");
    assert(result.timestamp == 1234567890);
    
    std::cout << "✅ Search result test passed" << std::endl;
}

void testSearchUtils() {
    std::cout << "🧪 Testing search utilities..." << std::endl;
    
    // Test query normalization
    String normalized = search_utils::normalizeQuery("  TEST  QUERY  ");
    assert(normalized == "test query");
    
    // Test tokenization
    auto tokens = search_utils::tokenizeQuery("test query with punctuation!");
    assert(tokens.size() >= 3);
    
    // Test URL utilities
    assert(search_utils::isValidUrl("https://example.com"));
    assert(!search_utils::isValidUrl("not-a-url"));
    
    String domain = search_utils::extractDomain("https://example.com/path?query=value");
    assert(domain == "example.com");
    
    std::cout << "✅ Search utilities test passed" << std::endl;
}

void testHTMLUtils() {
    std::cout << "🧪 Testing HTML utilities..." << std::endl;
    
    // Test HTML escaping
    String escaped = html_utils::escapeHtml("<script>alert('test')</script>");
    assert(escaped.find("&lt;") != String::npos);
    assert(escaped.find("&gt;") != String::npos);
    
    // Test HTML unescaping
    String unescaped = html_utils::unescapeHtml("&lt;script&gt;alert(&#39;test&#39;)&lt;/script&gt;");
    assert(unescaped.find("<script>") != String::npos);
    assert(unescaped.find("</script>") != String::npos);
    
    // Test whitespace normalization
    String normalized = html_utils::normalizeWhitespace("  multiple    spaces  ");
    assert(normalized == "multiple spaces");
    
    std::cout << "✅ HTML utilities test passed" << std::endl;
}

int main() {
    std::cout << "🦓 Zepra Browser Test Suite" << std::endl;
    std::cout << "===========================" << std::endl;
    std::cout << std::endl;
    
    try {
        testTypes();
        testConstants();
        testHTMLParser();
        testSearchEngine();
        testSearchQuery();
        testSearchResult();
        testSearchUtils();
        testHTMLUtils();
        
        std::cout << std::endl;
        std::cout << "🎉 All tests passed successfully!" << std::endl;
        std::cout << "✅ Zepra Browser core components are working correctly." << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
} 