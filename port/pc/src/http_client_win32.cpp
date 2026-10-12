#include "btga/master_client.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <array>
#include <stdexcept>

namespace btga {
namespace {
constexpr std::size_t max_body = 64 * 1024;

std::wstring wide(std::string_view input) {
    if (input.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) throw std::invalid_argument("text is not valid UTF-8");
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), size) != size) {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    return output;
}

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() { if (value_ != nullptr) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    [[nodiscard]] HINTERNET get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
private:
    HINTERNET value_;
};

class WinHttpClient final : public HttpClient {
public:
    explicit WinHttpClient(std::string base) : base_(std::move(base)) {
        if (!base_.starts_with("https://") || base_.ends_with('/')) {
            throw std::invalid_argument("master URL must be an HTTPS URL without a trailing slash");
        }
    }

    HttpResponse request(std::string_view method, std::string_view path,
                         std::string_view body, std::string_view bearer) override {
        if (path.empty() || path.front() != '/') throw std::invalid_argument("invalid HTTP path");
        const std::wstring url = wide(base_ + std::string(path));
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS) {
            throw std::runtime_error("could not parse master HTTPS URL");
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength != 0) {
            target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        }
        InternetHandle session(WinHttpOpen(
            L"BattleTanxGA-PC/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) throw std::runtime_error("WinHTTP initialization failed");
        WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);
        InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
        if (!connection) throw std::runtime_error("could not connect HTTPS request");
        const std::wstring method_text = wide(method);
        InternetHandle request_handle(WinHttpOpenRequest(
            connection.get(), method_text.c_str(), target.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!request_handle) throw std::runtime_error("could not create HTTPS request");

        std::wstring headers;
        if (body.size() > 4096) throw std::invalid_argument("HTTPS request body exceeds 4096 bytes");
        if (!body.empty()) headers += L"Content-Type: application/json\r\n";
        if (!bearer.empty()) headers += L"Authorization: Bearer " + wide(bearer) + L"\r\n";
        if (!WinHttpSendRequest(
                request_handle.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
            !WinHttpReceiveResponse(request_handle.get(), nullptr)) {
            throw std::runtime_error("HTTPS request failed");
        }
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!WinHttpQueryHeaders(request_handle.get(), WINHTTP_QUERY_STATUS_CODE |
                WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                &status_size, WINHTTP_NO_HEADER_INDEX)) {
            throw std::runtime_error("HTTPS response status is unavailable");
        }
        std::string response;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request_handle.get(), &available)) {
                throw std::runtime_error("HTTPS response read failed");
            }
            if (available == 0) break;
            if (available > max_body - response.size()) {
                throw std::runtime_error("master response exceeds 64 KiB");
            }
            const std::size_t offset = response.size();
            response.resize(offset + available);
            DWORD received = 0;
            if (!WinHttpReadData(request_handle.get(), response.data() + offset,
                                 available, &received)) {
                throw std::runtime_error("HTTPS response read failed");
            }
            response.resize(offset + received);
        }
        return {static_cast<int>(status), std::move(response)};
    }

private:
    std::string base_;
};
} // namespace

std::unique_ptr<HttpClient> make_platform_http_client(std::string base_url) {
    return std::make_unique<WinHttpClient>(std::move(base_url));
}
} // namespace btga
