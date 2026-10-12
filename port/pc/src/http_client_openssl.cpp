#include "btga/master_client.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace btga {
namespace {
constexpr std::size_t max_response = 72 * 1024;

class Socket {
public:
    explicit Socket(int value = -1) : value_(value) {}
    ~Socket() { if (value_ >= 0) close(value_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value_(other.value_) { other.value_ = -1; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) close(value_);
            value_ = other.value_;
            other.value_ = -1;
        }
        return *this;
    }
    [[nodiscard]] int get() const { return value_; }
    explicit operator bool() const { return value_ >= 0; }
private:
    int value_;
};

struct SslContextDeleter { void operator()(SSL_CTX* value) const { SSL_CTX_free(value); } };
struct SslDeleter { void operator()(SSL* value) const { SSL_free(value); } };

bool has_controls(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
    });
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string decode_chunked(std::string_view input) {
    std::string output;
    std::size_t cursor = 0;
    while (true) {
        const std::size_t end = input.find("\r\n", cursor);
        if (end == std::string_view::npos || end - cursor > 16) {
            throw std::runtime_error("invalid chunked HTTPS response");
        }
        const std::string size_text(input.substr(cursor, end - cursor));
        std::size_t consumed = 0;
        unsigned long size = 0;
        try { size = std::stoul(size_text, &consumed, 16); }
        catch (...) { throw std::runtime_error("invalid chunked HTTPS response"); }
        if (consumed != size_text.size()) throw std::runtime_error("invalid chunk size");
        cursor = end + 2;
        if (size == 0) return output;
        if (size > 64 * 1024 - output.size() || cursor + size + 2 > input.size() ||
            input.substr(cursor + size, 2) != "\r\n") {
            throw std::runtime_error("invalid or oversized chunked HTTPS response");
        }
        output.append(input.substr(cursor, size));
        cursor += size + 2;
    }
}

class OpenSslHttpClient final : public HttpClient {
public:
    explicit OpenSslHttpClient(std::string base) {
        constexpr std::string_view prefix = "https://";
        if (!base.starts_with(prefix) || base.ends_with('/')) {
            throw std::invalid_argument("master URL must be an HTTPS URL without a trailing slash");
        }
        const std::size_t path = base.find('/', prefix.size());
        const std::string authority = base.substr(prefix.size(), path - prefix.size());
        base_path_ = path == std::string::npos ? "" : base.substr(path);
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host_ = authority.substr(0, colon);
            port_ = authority.substr(colon + 1);
        } else {
            host_ = authority;
            port_ = "443";
        }
        if (host_.empty() || has_controls(host_) || base_path_.find("..") != std::string::npos) {
            throw std::invalid_argument("master URL is invalid");
        }
    }

    HttpResponse request(std::string_view method, std::string_view path,
                         std::string_view body, std::string_view bearer) override {
        if (path.empty() || path.front() != '/' || has_controls(method) || has_controls(path) ||
            has_controls(bearer)) {
            throw std::invalid_argument("invalid HTTPS request");
        }
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &addresses) != 0) {
            throw std::runtime_error("master DNS lookup failed");
        }
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_guard(addresses, freeaddrinfo);
        Socket socket;
        for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
            Socket candidate(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
            if (!candidate) continue;
            const int original_flags = fcntl(candidate.get(), F_GETFL, 0);
            if (original_flags < 0 ||
                fcntl(candidate.get(), F_SETFL, original_flags | O_NONBLOCK) < 0) continue;
            const int connection = connect(candidate.get(), address->ai_addr, address->ai_addrlen);
            bool connected = connection == 0;
            if (!connected && errno == EINPROGRESS) {
                pollfd pending{candidate.get(), POLLOUT, 0};
                if (poll(&pending, 1, 5000) == 1) {
                    int socket_error = 0;
                    socklen_t socket_error_size = sizeof(socket_error);
                    connected = getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR,
                                           &socket_error, &socket_error_size) == 0 &&
                                socket_error == 0;
                }
            }
            if (!connected || fcntl(candidate.get(), F_SETFL, original_flags) < 0) continue;
            timeval timeout{10, 0};
            setsockopt(candidate.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(candidate.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            socket = std::move(candidate);
            break;
        }
        if (!socket) throw std::runtime_error("master connection failed");

        std::unique_ptr<SSL_CTX, SslContextDeleter> context(SSL_CTX_new(TLS_client_method()));
        if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
            SSL_CTX_set_default_verify_paths(context.get()) != 1) {
            throw std::runtime_error("TLS initialization failed");
        }
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
        std::unique_ptr<SSL, SslDeleter> ssl(SSL_new(context.get()));
        if (!ssl || SSL_set_tlsext_host_name(ssl.get(), host_.c_str()) != 1 ||
            X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl.get()), host_.c_str(), 0) != 1 ||
            SSL_set_fd(ssl.get(), socket.get()) != 1 || SSL_connect(ssl.get()) != 1 ||
            SSL_get_verify_result(ssl.get()) != X509_V_OK) {
            throw std::runtime_error("TLS connection or certificate verification failed");
        }

        std::string request_text = std::string(method) + " " + base_path_ +
            std::string(path) + " HTTP/1.1\r\nHost: " + host_ +
            "\r\nAccept: application/json\r\nConnection: close\r\nUser-Agent: BattleTanxGA-PC/1\r\n";
        if (!bearer.empty()) request_text += "Authorization: Bearer " + std::string(bearer) + "\r\n";
        if (!body.empty()) request_text += "Content-Type: application/json\r\n";
        request_text += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        request_text.append(body);
        std::size_t sent = 0;
        while (sent < request_text.size()) {
            const int count = SSL_write(ssl.get(), request_text.data() + sent,
                                        static_cast<int>(request_text.size() - sent));
            if (count <= 0) throw std::runtime_error("HTTPS request write failed");
            sent += static_cast<std::size_t>(count);
        }
        std::string raw;
        std::array<char, 4096> buffer{};
        for (;;) {
            const int count = SSL_read(ssl.get(), buffer.data(), static_cast<int>(buffer.size()));
            if (count > 0) {
                if (static_cast<std::size_t>(count) > max_response - raw.size()) {
                    throw std::runtime_error("master response exceeds limit");
                }
                raw.append(buffer.data(), static_cast<std::size_t>(count));
                continue;
            }
            const int error = SSL_get_error(ssl.get(), count);
            if (error == SSL_ERROR_ZERO_RETURN) break;
            if (error == SSL_ERROR_SYSCALL && count == 0) break;
            // Some HTTP/1.1 servers close a Connection: close response without
            // a TLS close_notify. Treat that as EOF only after response bytes
            // have arrived; the HTTP framing below still has to validate.
            if (error == SSL_ERROR_SSL && !raw.empty() &&
                ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                ERR_clear_error();
                break;
            }
            throw std::runtime_error("HTTPS response read failed");
        }
        const std::size_t header_end = raw.find("\r\n\r\n");
        const std::size_t status_end = raw.find("\r\n");
        if (header_end == std::string::npos || status_end == std::string::npos ||
            !raw.starts_with("HTTP/1.1 ") || status_end < 12) {
            throw std::runtime_error("invalid HTTPS response");
        }
        const int status = std::stoi(raw.substr(9, 3));
        const std::string headers = lower(raw.substr(status_end + 2, header_end - status_end - 2));
        std::string response_body = raw.substr(header_end + 4);
        if (headers.find("transfer-encoding: chunked") != std::string::npos) {
            response_body = decode_chunked(response_body);
        }
        if (response_body.size() > 64 * 1024) throw std::runtime_error("master response exceeds 64 KiB");
        return {status, std::move(response_body)};
    }

private:
    std::string host_;
    std::string port_;
    std::string base_path_;
};
} // namespace

std::unique_ptr<HttpClient> make_platform_http_client(std::string base_url) {
    return std::make_unique<OpenSslHttpClient>(std::move(base_url));
}
} // namespace btga
