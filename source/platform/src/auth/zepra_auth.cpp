// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#include "auth/zepra_auth.h"

// json-c is available on Linux/NeolyxOS only.
// On Windows, use a thin shim mapping json-c API to nlohmann/json.
#ifndef _WIN32
#include <json-c/json.h>
#else
// Minimal json-c API shim over nlohmann/json for Windows builds
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>

struct json_object {
    nlohmann::json data;
    bool owned = true;
};

static inline json_object* json_object_new_object() {
    auto* o = new json_object; o->data = nlohmann::json::object(); return o;
}
static inline json_object* json_object_new_string(const char* s) {
    auto* o = new json_object; o->data = s; return o;
}
static inline void json_object_object_add(json_object* parent, const char* key, json_object* val) {
    if (parent && val) { parent->data[key] = val->data; delete val; }
}
static inline const char* json_object_to_json_string(json_object* obj) {
    static thread_local std::string buf;
    buf = obj->data.dump();
    return buf.c_str();
}
static inline void json_object_put(json_object* obj) { if (obj && obj->owned) delete obj; }
static inline json_object* json_tokener_parse(const char* s) {
    try { auto* o = new json_object; o->data = nlohmann::json::parse(s); return o; }
    catch (...) { return nullptr; }
}
static inline bool json_object_object_get_ex(json_object* obj, const char* key, json_object** out) {
    if (!obj || !obj->data.contains(key)) return false;
    static thread_local json_object child;
    child.data = obj->data[key]; child.owned = false;
    *out = &child;
    return true;
}
static inline bool json_object_get_boolean(json_object* obj) { return obj && obj->data.is_boolean() && obj->data.get<bool>(); }
static inline const char* json_object_get_string(json_object* obj) {
    static thread_local std::string buf;
    if (!obj) return "";
    buf = obj->data.is_string() ? obj->data.get<std::string>() : obj->data.dump();
    return buf.c_str();
}
static inline int json_object_get_int(json_object* obj) { return obj ? obj->data.get<int>() : 0; }
#endif // _WIN32

#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace ZepraAuth {

#include "nxhttp.h"
#include "nxbase.h"

// Singleton instance
ZepraAuthManager& ZepraAuthManager::getInstance() {
    static ZepraAuthManager instance;
    return instance;
}

ZepraAuthManager::ZepraAuthManager() 
    : m_authState(AuthState::UNAUTHENTICATED)
    , m_sessionTimeout(std::chrono::seconds(Constants::SESSION_TIMEOUT_MINUTES * 60))
    , m_autoRefresh(true)
    , m_secureMode(true)
    , m_initialized(false)
    , m_httpClient(nullptr) {
    
    // Initialize handled in initialize()
    
    // Set default allowed domains
    m_allowedDomains = {
        "ketivee.com",
        "auth.ketivee.com",
        "ketiveeai.com",
        "docs.ketivee.com",
        "mail.ketivee.com",
        "opensource.ketivee.com",
        "workspace.ketivee.com",
        "form.ketivee.com",
        "calendar.ketivee.com",
        "community.ketivee.com",
        "diary.ketivee.com",
        "gallery.ketivee.com",
        "drive.ketivee.com",
        "dev.ketivee.com",
        "chat.ketivee.com"
    };
}

ZepraAuthManager::~ZepraAuthManager() {
    shutdown();
}

bool ZepraAuthManager::initialize(const std::string& authServerUrl) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) {
        return true;
    }
    
    if (!m_httpClient) {
        NxHttpClientConfig config = {0};
        config.connect_timeout_ms = 10000;
        config.read_timeout_ms = 10000;
        config.follow_redirects = true;
        config.max_redirects = 10;
        config.verify_ssl = false; // Match previous behavior
        config.user_agent = "Zepra Core Browser/1.0";
        
        m_httpClient = nx_http_client_create(&config);
        
        if (!m_httpClient) {
            std::cerr << "ZepraAuth: Failed to create HTTP client" << std::endl;
            return false;
        }
    }
    
    m_authServerUrl = authServerUrl;
    m_initialized = true;
    
    // Check existing session
    checkSession();
    
    std::cout << "ZepraAuth: Initialized with server: " << m_authServerUrl << std::endl;
    return true;
}

void ZepraAuthManager::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_httpClient) {
        nx_http_client_free(m_httpClient);
        m_httpClient = nullptr;
    }
    m_initialized = false;
}

bool ZepraAuthManager::login(const std::string& email, const std::string& password) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        std::cerr << "ZepraAuth: Not initialized" << std::endl;
        return false;
    }
    
    m_authState = AuthState::AUTHENTICATING;
    
    // Prepare login data
    json_object* loginData = json_object_new_object();
    json_object_object_add(loginData, "email", json_object_new_string(email.c_str()));
    json_object_object_add(loginData, "password", json_object_new_string(password.c_str()));
    
    // Add device information
    json_object* deviceData = json_object_new_object();
    json_object_object_add(deviceData, "name", json_object_new_string("Zepra Core Browser"));
    json_object_object_add(deviceData, "type", json_object_new_string("browser"));
    json_object_object_add(deviceData, "browser", json_object_new_string("Zepra Core"));
    json_object_object_add(deviceData, "os", json_object_new_string("Cross-Platform"));
    json_object_object_add(deviceData, "userAgent", json_object_new_string("Zepra Core/1.0"));
    
    json_object_object_add(loginData, "deviceData", json_object_new_string(json_object_to_json_string(deviceData)));
    
    std::string postData = json_object_to_json_string(loginData);
    json_object_put(loginData);
    json_object_put(deviceData);
    
    // Make login request
    std::string response;
    std::string url = m_authServerUrl + Constants::AUTH_ENDPOINT_LOGIN;
    
    if (!httpPost(url, postData, response)) {
        m_authState = AuthState::ERROR;
        return false;
    }
    
    // Parse response
    json_object* responseObj = json_tokener_parse(response.c_str());
    if (!responseObj) {
        m_authState = AuthState::ERROR;
        return false;
    }
    
    json_object* successObj;
    if (json_object_object_get_ex(responseObj, "success", &successObj) && 
        json_object_get_boolean(successObj)) {
        
        // Check if 2FA is required
        json_object* requires2FAObj;
        if (json_object_object_get_ex(responseObj, "requires2FA", &requires2FAObj) && 
            json_object_get_boolean(requires2FAObj)) {
            
            json_object* tempTokenObj;
            if (json_object_object_get_ex(responseObj, "tempToken", &tempTokenObj)) {
                std::string tempToken = json_object_get_string(tempTokenObj);
                
                // Call 2FA callback
                if (m_twoFactorCallback) {
                    m_twoFactorCallback(tempToken);
                }
                
                json_object_put(responseObj);
                m_authState = AuthState::AUTHENTICATING;
                return true;
            }
        }
        
        // Parse successful login
        AuthToken token;
        UserInfo user;
        if (parseAuthResponse(response, token, user)) {
            m_currentToken = token;
            m_currentUser = user;
            m_authState = AuthState::AUTHENTICATED;
            
            // Call auth callback
            if (m_authCallback) {
                m_authCallback(m_authState, m_currentUser);
            }
            
            json_object_put(responseObj);
            return true;
        }
    }
    
    // Handle error
    json_object* errorObj;
    if (json_object_object_get_ex(responseObj, "error", &errorObj)) {
        std::string error = json_object_get_string(errorObj);
        std::cerr << "ZepraAuth: Login failed: " << error << std::endl;
    }
    
    json_object_put(responseObj);
    m_authState = AuthState::UNAUTHENTICATED;
    return false;
}

bool ZepraAuthManager::loginWith2FA(const std::string& tempToken, const std::string& code) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        return false;
    }
    
    // Prepare 2FA data
    json_object* twoFAData = json_object_new_object();
    json_object_object_add(twoFAData, "token", json_object_new_string(tempToken.c_str()));
    json_object_object_add(twoFAData, "code", json_object_new_string(code.c_str()));
    
    std::string postData = json_object_to_json_string(twoFAData);
    json_object_put(twoFAData);
    
    // Make 2FA request
    std::string response;
    std::string url = m_authServerUrl + Constants::AUTH_ENDPOINT_2FA;
    
    if (!httpPost(url, postData, response)) {
        m_authState = AuthState::ERROR;
        return false;
    }
    
    // Parse response
    json_object* responseObj = json_tokener_parse(response.c_str());
    if (!responseObj) {
        m_authState = AuthState::ERROR;
        return false;
    }
    
    json_object* successObj;
    if (json_object_object_get_ex(responseObj, "success", &successObj) && 
        json_object_get_boolean(successObj)) {
        
        // Parse successful 2FA
        AuthToken token;
        UserInfo user;
        if (parseAuthResponse(response, token, user)) {
            m_currentToken = token;
            m_currentUser = user;
            m_authState = AuthState::AUTHENTICATED;
            
            // Call auth callback
            if (m_authCallback) {
                m_authCallback(m_authState, m_currentUser);
            }
            
            json_object_put(responseObj);
            return true;
        }
    }
    
    json_object_put(responseObj);
    m_authState = AuthState::UNAUTHENTICATED;
    return false;
}

bool ZepraAuthManager::logout() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized || m_authState != AuthState::AUTHENTICATED) {
        return false;
    }
    
    // Make logout request
    std::string response;
    std::string url = m_authServerUrl + Constants::AUTH_ENDPOINT_LOGOUT;
    
    httpPost(url, "", response);
    
    // Clear session regardless of response
    clearSession();
    return true;
}

bool ZepraAuthManager::checkSession() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        return false;
    }
    
    // Check if we have a valid token
    if (!isTokenValid(m_currentToken)) {
        m_authState = AuthState::UNAUTHENTICATED;
        return false;
    }
    
    // Verify token with server
    return verifyToken();
}

bool ZepraAuthManager::verifyToken() {
    if (!m_initialized) {
        return false;
    }
    
    std::string response;
    std::string url = m_authServerUrl + Constants::AUTH_ENDPOINT_VERIFY;
    
    if (!httpGet(url, response)) {
        m_authState = AuthState::ERROR;
        return false;
    }
    
    // Parse response
    json_object* responseObj = json_tokener_parse(response.c_str());
    if (!responseObj) {
        m_authState = AuthState::ERROR;
        return false;
    }
    
    json_object* authenticatedObj;
    if (json_object_object_get_ex(responseObj, "authenticated", &authenticatedObj) && 
        json_object_get_boolean(authenticatedObj)) {
        
        // Parse user info
        json_object* agentObj;
        if (json_object_object_get_ex(responseObj, "agent", &agentObj)) {
            UserInfo user;
            if (parseUserInfo(agentObj, user)) {
                m_currentUser = user;
                m_authState = AuthState::AUTHENTICATED;
                
                // Call auth callback
                if (m_authCallback) {
                    m_authCallback(m_authState, m_currentUser);
                }
                
                json_object_put(responseObj);
                return true;
            }
        }
    }
    
    json_object_put(responseObj);
    m_authState = AuthState::UNAUTHENTICATED;
    return false;
}

void ZepraAuthManager::clearSession() {
    m_currentToken = AuthToken();
    m_currentUser = UserInfo();
    m_authState = AuthState::UNAUTHENTICATED;
    clearAllCookies();
    
    // Call auth callback
    if (m_authCallback) {
        m_authCallback(m_authState, m_currentUser);
    }
}

bool ZepraAuthManager::setCookie(const Cookie& cookie) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Remove existing cookie with same name
    m_cookies.erase(
        std::remove_if(m_cookies.begin(), m_cookies.end(),
            [&](const Cookie& c) { return c.name == cookie.name; }),
        m_cookies.end()
    );
    
    // Add new cookie
    m_cookies.push_back(cookie);
    return true;
}

bool ZepraAuthManager::getCookie(const std::string& name, Cookie& cookie) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = std::find_if(m_cookies.begin(), m_cookies.end(),
        [&](const Cookie& c) { return c.name == name; });
    
    if (it != m_cookies.end()) {
        cookie = *it;
        return true;
    }
    
    return false;
}

bool ZepraAuthManager::clearCookie(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_cookies.erase(
        std::remove_if(m_cookies.begin(), m_cookies.end(),
            [&](const Cookie& c) { return c.name == name; }),
        m_cookies.end()
    );
    
    return true;
}

void ZepraAuthManager::clearAllCookies() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cookies.clear();
}

std::vector<Cookie> ZepraAuthManager::getAllCookies() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cookies;
}

UserInfo ZepraAuthManager::getCurrentUser() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentUser;
}

AuthState ZepraAuthManager::getAuthState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_authState;
}

bool ZepraAuthManager::isAuthenticated() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_authState == AuthState::AUTHENTICATED && isTokenValid(m_currentToken);
}

bool ZepraAuthManager::isTokenExpired() const {
    return !isTokenValid(m_currentToken);
}

void ZepraAuthManager::setAuthCallback(AuthCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_authCallback = callback;
}

void ZepraAuthManager::setTwoFactorCallback(TwoFactorCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_twoFactorCallback = callback;
}

void ZepraAuthManager::setPasswordPromptCallback(PasswordPromptCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_passwordPromptCallback = callback;
}

bool ZepraAuthManager::authenticateForWebsite(const std::string& websiteUrl) {
    if (!isAuthenticated()) {
        return false;
    }
    
    // Extract domain from URL
    std::string domain = Utils::extractDomain(websiteUrl);
    
    // Check if domain is allowed
    if (!isDomainAllowed(domain)) {
        return false;
    }
    
    // Check if user has permission for this domain
    // This could be enhanced with role-based permissions
    
    return true;
}

bool ZepraAuthManager::promptForPassword(const std::string& websiteUrl) {
    if (m_passwordPromptCallback) {
        std::string domain = Utils::extractDomain(websiteUrl);
        m_passwordPromptCallback(websiteUrl, domain);
        return true;
    }
    return false;
}

bool ZepraAuthManager::promptFor2FA(const std::string& websiteUrl) {
    if (m_twoFactorCallback) {
        std::string domain = Utils::extractDomain(websiteUrl);
        m_twoFactorCallback(domain);
        return true;
    }
    return false;
}

// HTTP methods implementation
bool ZepraAuthManager::httpGet(const std::string& url, std::string& response) {
    if (!m_httpClient) {
        return false;
    }
    
    NxHttpRequest* req = nx_http_request_create(NX_HTTP_GET, url.c_str());
    
    // Set cookies
    std::string cookieHeader = formatCookieHeader();
    if (!cookieHeader.empty()) {
        nx_http_request_set_header(req, "Cookie", cookieHeader.c_str());
    }
    
    // Force identity encoding
    nx_http_request_set_header(req, "Accept-Encoding", "identity");
    
    NxHttpError err = NX_HTTP_OK;
    NxHttpResponse* res = nx_http_client_send(m_httpClient, req, &err);
    nx_http_request_free(req);
    
    if (res) {
        bool success = (nx_http_response_status(res) >= 200 && nx_http_response_status(res) < 300);
        if (success) {
            const char* body = nx_http_response_body_string(res);
            if (body) response = body;
        }
        nx_http_response_free(res);
        return success;
    }
    return false;
}

bool ZepraAuthManager::httpPost(const std::string& url, const std::string& data, std::string& response) {
    if (!m_httpClient) {
        return false;
    }
    
    NxHttpRequest* req = nx_http_request_create(NX_HTTP_POST, url.c_str());
    
    nx_http_request_set_header(req, "Content-Type", "application/json");
    
    // Set cookies
    std::string cookieHeader = formatCookieHeader();
    if (!cookieHeader.empty()) {
        nx_http_request_set_header(req, "Cookie", cookieHeader.c_str());
    }
    
    nx_http_request_set_body(req, data.c_str(), data.length());
    
    NxHttpError err = NX_HTTP_OK;
    NxHttpResponse* res = nx_http_client_send(m_httpClient, req, &err);
    nx_http_request_free(req);
    
    if (res) {
        bool success = (nx_http_response_status(res) >= 200 && nx_http_response_status(res) < 300);
        if (success) {
            const char* body = nx_http_response_body_string(res);
            if (body) response = body;
        }
        nx_http_response_free(res);
        return success;
    }
    return false;
}

bool ZepraAuthManager::isTokenValid(const AuthToken& token) const {
    if (!token.isValid) return false;
    if (token.token.empty()) return false;
    
    // Check if token is expired
    auto now = std::chrono::system_clock::now();
    if (now >= token.expiresAt) return false;
    
    return true;
}

std::string ZepraAuthManager::formatCookieHeader() const {
    std::string header;
    for (const auto& cookie : m_cookies) {
        if (!header.empty()) header += "; ";
        header += cookie.name + "=" + cookie.value;
    }
    return header;
}

bool ZepraAuthManager::isDomainAllowed(const std::string& domain) const {
    // Allow all domains for now (stub)
    (void)domain;
    return true;
}

bool ZepraAuthManager::parseUserInfo(const json_object* obj, UserInfo& user) {
    if (!obj) return false;
    
    struct json_object* field;
    if (json_object_object_get_ex(const_cast<struct json_object*>(obj), "id", &field)) {
        user.id = json_object_get_string(field);
    }
    if (json_object_object_get_ex(const_cast<struct json_object*>(obj), "email", &field)) {
        user.email = json_object_get_string(field);
    }
    if (json_object_object_get_ex(const_cast<struct json_object*>(obj), "firstName", &field)) {
        user.firstName = json_object_get_string(field);
    }
    if (json_object_object_get_ex(const_cast<struct json_object*>(obj), "lastName", &field)) {
        user.lastName = json_object_get_string(field);
    }
    
    return !user.id.empty();
}

// Parse auth response JSON
bool ZepraAuthManager::parseAuthResponse(const std::string& json, AuthToken& token, UserInfo& user) {
    struct json_object* root = json_tokener_parse(json.c_str());
    if (!root) return false;
    
    struct json_object* success_obj;
    if (json_object_object_get_ex(root, "success", &success_obj)) {
        if (!json_object_get_boolean(success_obj)) {
            json_object_put(root);
            return false;
        }
    }
    
    // Parse token
    struct json_object* token_obj;
    if (json_object_object_get_ex(root, "access_token", &token_obj)) {
        token.token = json_object_get_string(token_obj);
        token.isValid = true;
    }
    if (json_object_object_get_ex(root, "refresh_token", &token_obj)) {
        token.refreshToken = json_object_get_string(token_obj);
    }
    if (json_object_object_get_ex(root, "expires_in", &token_obj)) {
        token.expiresAt = std::chrono::system_clock::now() + 
            std::chrono::seconds(json_object_get_int(token_obj));
    }
    
    // Parse user info
    struct json_object* user_obj;
    if (json_object_object_get_ex(root, "user", &user_obj)) {
        struct json_object* field;
        if (json_object_object_get_ex(user_obj, "id", &field)) {
            user.id = json_object_get_string(field);
        }
        if (json_object_object_get_ex(user_obj, "email", &field)) {
            user.email = json_object_get_string(field);
        }
        if (json_object_object_get_ex(user_obj, "firstName", &field)) {
            user.firstName = json_object_get_string(field);
        }
        if (json_object_object_get_ex(user_obj, "lastName", &field)) {
            user.lastName = json_object_get_string(field);
        }
    }
    
    json_object_put(root);
    return true;
}

// Utility functions implementation
namespace Utils {
    
std::string base64Encode(const std::string& input) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);
    
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, input.c_str(), input.length());
    BIO_flush(bio);
    
    BUF_MEM* bufferPtr;
    BIO_get_mem_ptr(bio, &bufferPtr);
    
    std::string result(bufferPtr->data, bufferPtr->length);
    
    BIO_free_all(bio);
    return result;
}

std::string base64Decode(const std::string& input) {
    BIO* bio = BIO_new_mem_buf(input.c_str(), input.length());
    BIO* b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);
    
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    
    std::string result;
    char buffer[1024];
    int len;
    while ((len = BIO_read(bio, buffer, sizeof(buffer))) > 0) {
        result.append(buffer, len);
    }
    
    BIO_free_all(bio);
    return result;
}

std::string generateUUID() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";
    
    std::string uuid;
    uuid.reserve(36);
    
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            uuid += '-';
        } else {
            uuid += hex[dis(gen)];
        }
    }
    
    return uuid;
}

std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t);
#else
    gmtime_r(&time_t, &tm_buf);
#endif
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

bool isValidEmail(const std::string& email) {
    size_t atPos = email.find('@');
    if (atPos == std::string::npos || atPos == 0) {
        return false;
    }
    
    size_t dotPos = email.find('.', atPos);
    if (dotPos == std::string::npos || dotPos == atPos + 1) {
        return false;
    }
    
    return true;
}

std::string hashString(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, input.c_str(), input.length());
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

// Note: extractDomain is inline in header

} // namespace Utils

} // namespace ZepraAuth 