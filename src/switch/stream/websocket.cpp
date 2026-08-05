#include "websocket.hpp"

#include <mbedtls/base64.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha1.h>

#include <switch.h>

#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace gnx::stream {

namespace {

std::string tls_error(int code) {
    char message[160]{};
    mbedtls_strerror(code, message, sizeof(message));
    return message;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string header_value(const std::string& headers, const std::string& name) {
    std::istringstream lines(headers);
    std::string line;
    const std::string wanted = lowercase(name);
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (lowercase(trim(line.substr(0, colon))) == wanted) {
            return trim(line.substr(colon + 1));
        }
    }
    return {};
}

std::string websocket_accept(const std::string& key) {
    const std::string source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> digest{};
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(source.data()),
                 source.size(), digest.data());
    std::array<unsigned char, 64> encoded{};
    std::size_t length = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &length,
                              digest.data(), digest.size()) != 0) {
        return {};
    }
    return std::string(reinterpret_cast<char*>(encoded.data()), length);
}

int socket_send(void* context, const unsigned char* data, size_t size) {
    const int fd = *static_cast<int*>(context);
    const int result = ::send(fd, data, size, 0);
    if (result >= 0) return result;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int socket_receive(void* context, unsigned char* data, size_t size) {
    const int fd = *static_cast<int*>(context);
    const int result = ::recv(fd, data, size, 0);
    if (result >= 0) return result;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

int socket_receive_timeout(void* context, unsigned char* data, size_t size,
                           uint32_t timeoutMs) {
    const int fd = *static_cast<int*>(context);
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(fd, &reads);
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(timeoutMs / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);
    const int ready = ::select(fd + 1, &reads, nullptr, nullptr,
                               timeoutMs ? &timeout : nullptr);
    if (ready == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (ready < 0) {
        return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ
                              : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return socket_receive(context, data, size);
}

bool tcp_connect(const std::string& host, const std::string& port, int& fd) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0) {
        return false;
    }
    for (addrinfo* address = results; address; address = address->ai_next) {
        const int candidate = ::socket(address->ai_family, address->ai_socktype,
                                       address->ai_protocol);
        if (candidate < 0) continue;
        if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
            fd = candidate;
            break;
        }
        ::close(candidate);
    }
    ::freeaddrinfo(results);
    return fd >= 0;
}

}  // namespace

WssClient::WssClient() {
    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&config_);
    mbedtls_x509_crt_init(&ca_);
    mbedtls_ctr_drbg_init(&random_);
    mbedtls_entropy_init(&entropy_);
    initialized_ = true;
}

WssClient::~WssClient() { close(); }

void WssClient::reset_locked() {
    connected_ = false;
    lastPingMs_ = -1;
    fragment_.clear();
    fragmentOpcode_ = 0;
    if (!initialized_) return;
    mbedtls_ssl_close_notify(&ssl_);
    if (socket_ >= 0) {
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        socket_ = -1;
    }
    mbedtls_ssl_free(&ssl_);
    mbedtls_ssl_config_free(&config_);
    mbedtls_x509_crt_free(&ca_);
    mbedtls_ctr_drbg_free(&random_);
    mbedtls_entropy_free(&entropy_);
    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&config_);
    mbedtls_x509_crt_init(&ca_);
    mbedtls_ctr_drbg_init(&random_);
    mbedtls_entropy_init(&entropy_);
}

bool WssClient::connect(const std::string& hostAndPort,
                        const std::string& requestedPath,
                        const std::string& caBundle,
                        std::string& error) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    reset_locked();
    error.clear();

    std::string host = hostAndPort;
    std::string port = "443";
    const auto colon = hostAndPort.rfind(':');
    if (colon != std::string::npos &&
        hostAndPort.find(':') == colon && colon + 1 < hostAndPort.size()) {
        host = hostAndPort.substr(0, colon);
        port = hostAndPort.substr(colon + 1);
    }
    const std::string path = requestedPath.empty()
        ? "/"
        : (requestedPath.front() == '/' ? requestedPath : "/" + requestedPath);

    static constexpr char kPersonalization[] = "zerodroid-wss";
    int result = mbedtls_ctr_drbg_seed(
        &random_, mbedtls_entropy_func, &entropy_,
        reinterpret_cast<const unsigned char*>(kPersonalization),
        sizeof(kPersonalization) - 1);
    if (result != 0) {
        error = "TLS RNG: " + tls_error(result);
        return false;
    }
    result = mbedtls_x509_crt_parse_file(&ca_, caBundle.c_str());
    if (result < 0) {
        error = "CA bundle: " + tls_error(result);
        return false;
    }
    if (!tcp_connect(host, port, socket_)) {
        error = "No se pudo conectar por TCP con " + hostAndPort + ".";
        return false;
    }
    result = mbedtls_ssl_config_defaults(
        &config_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        error = "TLS config: " + tls_error(result);
        return false;
    }
    mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config_, &ca_, nullptr);
    mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &random_);
    mbedtls_ssl_conf_read_timeout(&config_, 5000);
    if ((result = mbedtls_ssl_setup(&ssl_, &config_)) != 0 ||
        (result = mbedtls_ssl_set_hostname(&ssl_, host.c_str())) != 0) {
        error = "TLS setup: " + tls_error(result);
        return false;
    }
    mbedtls_ssl_set_bio(&ssl_, &socket_, socket_send, socket_receive,
                        socket_receive_timeout);
    while ((result = mbedtls_ssl_handshake(&ssl_)) != 0) {
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        error = "TLS handshake: " + tls_error(result);
        reset_locked();
        return false;
    }
    if (mbedtls_ssl_get_verify_result(&ssl_) != 0) {
        error = "El certificado TLS del gateway no es valido.";
        reset_locked();
        return false;
    }

    std::array<unsigned char, 16> nonce{};
    randomGet(nonce.data(), nonce.size());
    std::array<unsigned char, 32> encoded{};
    std::size_t encodedLength = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &encodedLength,
                              nonce.data(), nonce.size()) != 0) {
        error = "No se pudo crear la clave WebSocket.";
        reset_locked();
        return false;
    }
    const std::string key(reinterpret_cast<char*>(encoded.data()), encodedLength);
    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << hostAndPort << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "Origin: https://cloud.boosteroid.com\r\n"
            << "User-Agent: ZERODROID/" << ZERODROID_VERSION << " Nintendo Switch\r\n\r\n";
    const std::string requestText = request.str();
    if (!write_all_locked(
            reinterpret_cast<const unsigned char*>(requestText.data()),
            requestText.size())) {
        error = "No se pudo enviar el handshake WebSocket.";
        reset_locked();
        return false;
    }

    std::string response;
    std::array<unsigned char, 1> byte{};
    bool timedOut = false;
    while (response.find("\r\n\r\n") == std::string::npos &&
           response.size() < 32768) {
        if (!read_exact_locked(byte.data(), 1, 5000, timedOut, error)) {
            if (error.empty()) error = "El gateway no completo el handshake.";
            reset_locked();
            return false;
        }
        response.push_back(static_cast<char>(byte[0]));
    }
    if (response.find(" 101 ") == std::string::npos) {
        error = "El gateway rechazo WebSocket: " + response.substr(0, 120);
        reset_locked();
        return false;
    }
    const std::string accept = header_value(response, "Sec-WebSocket-Accept");
    if (accept.empty() || accept != websocket_accept(key)) {
        error = "El gateway devolvio un handshake WebSocket no valido.";
        reset_locked();
        return false;
    }
    connected_ = true;
    return true;
}

void WssClient::close() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (connected_) send_frame_locked(0x8, nullptr, 0);
    reset_locked();
}

bool WssClient::write_all_locked(const unsigned char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const int result = mbedtls_ssl_write(&ssl_, data + sent, size - sent);
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool WssClient::send_frame_locked(uint8_t opcode, const void* data,
                                  std::size_t size) {
    if (!connected_ && opcode != 0x8) return false;
    if (size > 16 * 1024 * 1024) return false;
    std::vector<unsigned char> frame;
    frame.reserve(size + 14);
    frame.push_back(static_cast<unsigned char>(0x80U | opcode));
    if (size < 126) {
        frame.push_back(static_cast<unsigned char>(0x80U | size));
    } else if (size <= 0xffff) {
        frame.push_back(0x80U | 126U);
        frame.push_back(static_cast<unsigned char>((size >> 8) & 0xff));
        frame.push_back(static_cast<unsigned char>(size & 0xff));
    } else {
        frame.push_back(0x80U | 127U);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<unsigned char>((size >> shift) & 0xff));
        }
    }
    std::array<unsigned char, 4> mask{};
    randomGet(mask.data(), mask.size());
    frame.insert(frame.end(), mask.begin(), mask.end());
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        frame.push_back(static_cast<unsigned char>(bytes[index] ^ mask[index & 3]));
    }
    return write_all_locked(frame.data(), frame.size());
}

bool WssClient::send_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    return send_frame_locked(0x1, text.data(), text.size());
}

bool WssClient::send_ping() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const uint64_t sentAt = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    std::lock_guard<std::mutex> lock(ioMutex_);
    return send_frame_locked(0x9, &sentAt, sizeof(sentAt));
}

bool WssClient::read_exact_locked(unsigned char* data, std::size_t size,
                                  int timeoutMs, bool& timedOut,
                                  std::string& error) {
    timedOut = false;
    mbedtls_ssl_conf_read_timeout(&config_, static_cast<uint32_t>(timeoutMs));
    std::size_t read = 0;
    while (read < size) {
        const int result = mbedtls_ssl_read(&ssl_, data + read, size - read);
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (result == MBEDTLS_ERR_SSL_TIMEOUT) {
            timedOut = true;
            return false;
        }
        if (result <= 0) {
            error = result == 0 ? "El gateway cerro la conexion."
                                : "WebSocket TLS: " + tls_error(result);
            return false;
        }
        read += static_cast<std::size_t>(result);
    }
    return true;
}

bool WssClient::read_text(std::string& text, int timeoutMs,
                          std::string& error) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    text.clear();
    error.clear();
    if (!connected_) {
        error = "WebSocket no conectado.";
        return false;
    }

    for (;;) {
        std::array<unsigned char, 2> header{};
        bool timedOut = false;
        if (!read_exact_locked(header.data(), header.size(), timeoutMs,
                               timedOut, error)) {
            if (!timedOut && !error.empty()) connected_ = false;
            return false;
        }
        const bool final = (header[0] & 0x80U) != 0;
        const uint8_t opcode = header[0] & 0x0fU;
        const bool masked = (header[1] & 0x80U) != 0;
        uint64_t length = header[1] & 0x7fU;
        if (length == 126) {
            std::array<unsigned char, 2> extended{};
            if (!read_exact_locked(extended.data(), extended.size(), timeoutMs,
                                   timedOut, error)) return false;
            length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
        } else if (length == 127) {
            std::array<unsigned char, 8> extended{};
            if (!read_exact_locked(extended.data(), extended.size(), timeoutMs,
                                   timedOut, error)) return false;
            length = 0;
            for (unsigned char value : extended) length = (length << 8) | value;
        }
        if (length > 4 * 1024 * 1024) {
            error = "El gateway envio un frame WebSocket demasiado grande.";
            connected_ = false;
            return false;
        }
        std::array<unsigned char, 4> mask{};
        if (masked && !read_exact_locked(mask.data(), mask.size(), timeoutMs,
                                         timedOut, error)) return false;
        std::vector<unsigned char> payload(static_cast<std::size_t>(length));
        if (length && !read_exact_locked(payload.data(), payload.size(), timeoutMs,
                                         timedOut, error)) return false;
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];
        }

        if (opcode == 0x8) {
            connected_ = false;
            error = "El gateway termino la sesion.";
            return false;
        }
        if (opcode == 0x9) {
            send_frame_locked(0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0xA) {
            if (payload.size() == sizeof(uint64_t)) {
                uint64_t sentAt = 0;
                std::memcpy(&sentAt, payload.data(), sizeof(sentAt));
                const auto now = std::chrono::steady_clock::now().time_since_epoch();
                const uint64_t receivedAt = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now)
                        .count());
                if (receivedAt >= sentAt && receivedAt - sentAt <= 60000) {
                    lastPingMs_ = static_cast<int>(receivedAt - sentAt);
                }
            }
            continue;
        }
        if (opcode == 0x1 || opcode == 0x2) {
            fragmentOpcode_ = opcode;
            fragment_ = std::move(payload);
        } else if (opcode == 0x0 && fragmentOpcode_) {
            fragment_.insert(fragment_.end(), payload.begin(), payload.end());
        } else {
            continue;
        }
        if (!final) continue;
        if (fragmentOpcode_ == 0x1) {
            text.assign(reinterpret_cast<const char*>(fragment_.data()),
                        fragment_.size());
            fragment_.clear();
            fragmentOpcode_ = 0;
            return true;
        }
        fragment_.clear();
        fragmentOpcode_ = 0;
    }
}

}  // namespace gnx::stream
