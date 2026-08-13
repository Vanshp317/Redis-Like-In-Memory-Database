#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vcache::protocol {

// The wire format for replies, in one place so the server and the client cannot
// drift apart.
//
// WHY TYPE TAGS
//
// The first version of this protocol sent bare text: `OK`, `Vansh`, `(nil)`,
// `(2 keys)` followed by two more lines. Writing the client exposed two bugs of
// the same shape, both caused by replies that do not say what they are:
//
//   * `SET k "(2 keys)"` then `GET k` returns a line byte-for-byte identical to
//     a two-element KEYS header. A client reading the reply alone waits forever
//     for two lines that were never sent.
//   * `SET k "ERR disk full"` then `GET k` returns a line a client checking for
//     an "ERR " prefix reports as a server error. A stored value was mistaken
//     for a failure.
//
// Neither is fixable by escaping harder, because the ambiguity is not in the
// characters -- it is that a value and a control message occupied the same
// namespace. Tagging every reply with a leading byte separates them, so a reply
// can be parsed with no knowledge of the command that produced it.
//
//   +  status      +OK
//   -  error       -ERR unknown command 'FOO'
//   $  value       $Vansh          (escaped; never contains a newline)
//   _  nil         _
//   :  integer     :1
//   *  array       *2              followed by that many tagged lines
//
// The cost is that a raw `nc` session now shows the tags. That is a deliberate
// trade: the WIRE is machine-first and unambiguous, and vcache-cli renders it
// back to the human-readable form from Discovery Document section 19. Redis
// makes exactly this trade, for exactly this reason.
//
// It is also extensible. A future reply type is a new tag; an old client sees
// an unrecognised byte and can say so, rather than silently misreading it as
// something else.
enum class ReplyType : char {
    Status = '+',
    Error = '-',
    Value = '$',
    Nil = '_',
    Integer = ':',
    Array = '*',
    Unknown = '?',  // never sent; what a parser reports for an unrecognised tag
};

// ------------------------------------------------------------ encoding ----

inline std::string status(const std::string& text) {
    return std::string(1, static_cast<char>(ReplyType::Status)) + text;
}

inline std::string error(const std::string& text) {
    return std::string(1, static_cast<char>(ReplyType::Error)) + text;
}

// `escaped` must already have been through CommandExecutor::escapeForWire, so
// it is guaranteed to be a single line.
inline std::string value(const std::string& escaped) {
    return std::string(1, static_cast<char>(ReplyType::Value)) + escaped;
}

inline std::string nil() {
    return std::string(1, static_cast<char>(ReplyType::Nil));
}

inline std::string integer(std::int64_t number) {
    return std::string(1, static_cast<char>(ReplyType::Integer)) + std::to_string(number);
}

inline std::string arrayHeader(std::size_t count) {
    return std::string(1, static_cast<char>(ReplyType::Array)) + std::to_string(count);
}

// ------------------------------------------------------------ decoding ----

struct TaggedLine {
    ReplyType type = ReplyType::Unknown;
    std::string payload;
};

inline TaggedLine parse(const std::string& line) {
    TaggedLine parsed;
    if (line.empty()) {
        return parsed;  // Unknown, empty payload
    }

    switch (line.front()) {
        case '+': parsed.type = ReplyType::Status; break;
        case '-': parsed.type = ReplyType::Error; break;
        case '$': parsed.type = ReplyType::Value; break;
        case '_': parsed.type = ReplyType::Nil; break;
        case ':': parsed.type = ReplyType::Integer; break;
        case '*': parsed.type = ReplyType::Array; break;
        default: return parsed;  // Unknown; payload deliberately left empty
    }

    parsed.payload = line.substr(1);
    return parsed;
}

// Element count from an array header payload. False if it is not a number,
// which for untrusted input matters: the count drives how many more lines get
// read.
inline bool parseCount(const std::string& payload, std::size_t& count) {
    if (payload.empty()) {
        return false;
    }
    std::size_t value = 0;
    for (const char c : payload) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
        if (value > 100000000) {
            return false;  // absurd: refuse rather than try to read it
        }
    }
    count = value;
    return true;
}

}  // namespace vcache::protocol
