#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "Protocol.h"

namespace vcache {

// One server response, already decoded.
//
// The type comes from the wire, not from guessing at the text. That is what
// makes `SET k "ERR disk full"` followed by `GET k` report a VALUE rather than
// an error, and `SET k "(2 keys)"` a value rather than an array header. Both
// were real bugs before replies carried tags -- see Protocol.h.
struct Reply {
    // False means the exchange never completed -- the connection failed. A
    // server that answered "-ERR ..." is a SUCCESSFUL exchange carrying bad
    // news, which is a different thing and is reported by isError().
    bool delivered = false;

    protocol::ReplyType type = protocol::ReplyType::Unknown;

    // Status text, error text, escaped value, or integer digits, depending on
    // `type`. Empty for Nil and Array.
    std::string payload;

    // Array elements, already untagged. Empty unless type == Array.
    std::vector<std::string> elements;

    std::string transportError;

    bool isError() const { return type == protocol::ReplyType::Error; }
    bool isNil() const { return type == protocol::ReplyType::Nil; }
    bool isArray() const { return type == protocol::ReplyType::Array; }
};

// A blocking client for the VCache protocol.
//
// A library rather than logic buried in the CLI's main(), so it can be tested
// against a real server and reused by anything else that speaks the protocol.
//
// Because replies are self-describing, this class needs NO knowledge of which
// command produced a reply. The earlier version had to remember that it had
// sent KEYS in order to know whether to read more lines; that coupling is gone.
class VCacheClient {
public:
    VCacheClient() = default;
    ~VCacheClient();

    VCacheClient(const VCacheClient&) = delete;
    VCacheClient& operator=(const VCacheClient&) = delete;

    bool connect(const std::string& host,
                 std::uint16_t port,
                 std::string& error,
                 std::chrono::seconds timeout = std::chrono::seconds(10));

    void disconnect();
    bool connected() const noexcept { return fd_ >= 0; }

    Reply command(const std::string& commandLine);

private:
    bool sendAll(const std::string& text);
    bool readLine(std::string& line, std::string& error);

    int fd_ = -1;
    std::string buffer_;
};

}  // namespace vcache
