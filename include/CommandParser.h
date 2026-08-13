#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vcache {

// The verbs from Discovery Document section 7. `Empty` is a blank input line,
// which is not an error -- a client pressing Enter over telnet should get
// silence, not a complaint.
enum class CommandType {
    Empty,
    Set,
    Get,
    Del,
    Exists,
    Keys,
    Save,
};

// One parsed request. Fields not used by a given verb stay empty: KEYS uses
// neither, GET/DEL/EXISTS use `key`, SET uses both.
//
// Named fields rather than a generic argument vector, because every command in
// the v1 grammar takes at most a key and a value. If multi-key commands (MGET,
// variadic DEL) ever land, this becomes a vector -- but paying for that now
// would make every call site in the executor read worse for no benefit.
struct Command {
    CommandType type = CommandType::Empty;
    std::string key;
    std::string value;

    // Set only by `SET key value EX <seconds>`. Guaranteed positive and small
    // enough not to overflow a time_point -- the parser rejects anything else,
    // so the executor can use it without re-validating.
    std::optional<std::int64_t> expireSeconds;
};

// Either a parsed Command or a client-facing error string -- never both.
class ParseResult {
public:
    static ParseResult success(Command command);
    static ParseResult failure(std::string error);

    bool ok() const noexcept { return ok_; }

    // Valid only when ok() is true.
    const Command& command() const noexcept { return command_; }

    // Valid only when ok() is false. Redis-style text, e.g.
    // "ERR unknown command 'FOO'". The server decides how to frame it on the
    // wire; the parser only decides what it says.
    const std::string& error() const noexcept { return error_; }

private:
    ParseResult() = default;

    bool ok_ = false;
    Command command_;
    std::string error_;
};

// Turns one line of the text protocol into a Command.
//
// Grammar:
//     line     := token*
//     command  := "SET" key value ( "EX" seconds )?
//               | ("GET" | "DEL" | "EXISTS") key
//               | "KEYS"
//               | "SAVE"
//     token    := bare | quoted
//     bare     := any run of non-whitespace
//     quoted   := '"' ( escape | any-char-except-quote )* '"'
//     escape   := '\' ( 'n' | 'r' | 't' | 'b' | 'a' | 'xHH' | any-char )
//
// Quoting exists because storage is binary-safe (Phases 1-2) and a protocol
// that cannot express a value containing a space would waste that. \xHH lets a
// client send arbitrary bytes over a text connection.
//
// Every failure mode returns an error string. Nothing here throws and nothing
// aborts, which is the parser's half of the section 17 requirement that a
// malformed request must never take the server down.
//
// Entirely stateless -- hence static. Phase 5 can call this from any number of
// worker threads with no locking at all.
class CommandParser {
public:
    // `line` is a single request with no trailing newline required; trailing
    // CR and LF are stripped, so raw telnet CRLF input works unchanged.
    static ParseResult parse(const std::string& line);

    // Canonical uppercase name, or "" for CommandType::Empty.
    static const char* name(CommandType type) noexcept;
};

}  // namespace vcache
