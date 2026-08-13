#include "VCacheClient.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace vcache {

VCacheClient::~VCacheClient() {
    disconnect();
}

void VCacheClient::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    buffer_.clear();
}

bool VCacheClient::connect(const std::string& host,
                           std::uint16_t port,
                           std::string& error,
                           std::chrono::seconds timeout) {
    disconnect();

    // getaddrinfo rather than inet_pton so "localhost" and real hostnames work,
    // not just dotted quads.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* resolved = nullptr;
    const std::string portText = std::to_string(port);
    const int status = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &resolved);
    if (status != 0) {
        error = "cannot resolve '" + host + "': " + ::gai_strerror(status);
        return false;
    }

    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
        const int fd =
            ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
            // Every read is bounded, so a wedged server produces an error
            // rather than a CLI that hangs with no way out but Ctrl+C.
            timeval limit{};
            limit.tv_sec = static_cast<time_t>(timeout.count());
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));

            // Without this, Nagle's algorithm delays small commands waiting for
            // more data to batch -- adding milliseconds to every keystroke's
            // worth of work at an interactive prompt.
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            fd_ = fd;
            ::freeaddrinfo(resolved);
            return true;
        }

        ::close(fd);
    }

    error = "cannot connect to " + host + ":" + portText + ": " + std::strerror(errno);
    ::freeaddrinfo(resolved);
    return false;
}

bool VCacheClient::sendAll(const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const ssize_t written = ::send(fd_, text.data() + sent, text.size() - sent, 0);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

bool VCacheClient::readLine(std::string& line, std::string& error) {
    while (true) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return true;
        }

        char chunk[16384];
        const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);

        if (received == 0) {
            error = "server closed the connection";
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = (errno == EAGAIN || errno == EWOULDBLOCK)
                        ? "timed out waiting for a reply"
                        : std::string("read failed: ") + std::strerror(errno);
            return false;
        }

        buffer_.append(chunk, static_cast<std::size_t>(received));
    }
}

Reply VCacheClient::command(const std::string& commandLine) {
    Reply reply;

    if (fd_ < 0) {
        reply.transportError = "not connected";
        return reply;
    }

    if (!sendAll(commandLine + "\n")) {
        reply.transportError = std::string("send failed: ") + std::strerror(errno);
        disconnect();
        return reply;
    }

    std::string line;
    std::string error;
    if (!readLine(line, error)) {
        reply.transportError = error;
        disconnect();
        return reply;
    }

    const protocol::TaggedLine head = protocol::parse(line);
    if (head.type == protocol::ReplyType::Unknown) {
        // A reply this client does not understand. Reporting it beats guessing
        // -- and it is what makes the tag scheme safely extensible.
        reply.transportError = "unrecognised reply tag in: " + line;
        disconnect();
        return reply;
    }

    reply.type = head.type;
    reply.payload = head.payload;

    if (head.type == protocol::ReplyType::Array) {
        std::size_t count = 0;
        if (!protocol::parseCount(head.payload, count)) {
            reply.transportError = "malformed array header: " + line;
            disconnect();
            return reply;
        }

        reply.payload.clear();
        reply.elements.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            if (!readLine(line, error)) {
                reply.transportError = error;
                disconnect();
                return reply;
            }
            const protocol::TaggedLine element = protocol::parse(line);
            if (element.type == protocol::ReplyType::Unknown) {
                reply.transportError = "unrecognised element tag in: " + line;
                disconnect();
                return reply;
            }
            reply.elements.push_back(element.payload);
        }
    }

    reply.delivered = true;
    return reply;
}

}  // namespace vcache
