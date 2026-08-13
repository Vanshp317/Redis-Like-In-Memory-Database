#include "CommandParser.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace vcache {

namespace {

// An unknown verb gets echoed back to the client. Client-supplied bytes must
// never be reflected raw or unbounded: a megabyte of junk would turn a typo into
// an amplification, and control bytes could forge extra lines in a line-oriented
// protocol. Truncate, and replace anything non-printable.
constexpr std::size_t kMaxEchoedVerbLength = 32;

// Largest accepted TTL: a hundred years in seconds. The point is not policy but
// arithmetic -- adding an unbounded number of seconds to a time_point overflows,
// and signed overflow is undefined behaviour. Rejecting absurd values up front
// means the executor can add the duration without checking anything.
constexpr std::int64_t kMaxExpireSeconds = 100LL * 365 * 24 * 60 * 60;

// The command table. Adding a verb is one row here plus one enum value -- no
// changes to the parsing logic itself.
//
// SET spans {2, 4} because of the optional `EX seconds` pair. The arity check
// only bounds the count; whether tokens 3 and 4 actually form a valid option is
// checked separately, so a wrong keyword reports a syntax error rather than an
// argument-count error.
struct CommandSpec {
    const char* name;
    CommandType type;
    std::size_t minArgs;  // not counting the verb itself
    std::size_t maxArgs;
};

constexpr CommandSpec kCommands[] = {
    {"SET",    CommandType::Set,    2, 4},
    {"GET",    CommandType::Get,    1, 1},
    {"DEL",    CommandType::Del,    1, 1},
    {"EXISTS", CommandType::Exists, 1, 1},
    {"KEYS",   CommandType::Keys,   0, 0},
    {"SAVE",   CommandType::Save,   0, 0},
};

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t';
}

bool isHexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// ASCII-only uppercase, deliberately not std::toupper: that one consults the
// global locale, so a process running under a Turkish locale would fold 'i' to
// 'I' with a dot and stop recognising "exists". A wire protocol must not depend
// on the machine's locale settings.
std::string toUpperAscii(const std::string& text) {
    std::string result = text;
    for (char& c : result) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return result;
}

const CommandSpec* findCommand(const std::string& verbUpper) noexcept {
    for (const CommandSpec& spec : kCommands) {
        if (verbUpper == spec.name) {
            return &spec;
        }
    }
    return nullptr;
}

// Strict integer parse: no leading spaces, no trailing junk, no locale, no
// exceptions. std::stoll would happily accept "60seconds" and throw on
// overflow; from_chars reports both as errors through its return value.
bool parseInt64(const std::string& text, std::int64_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* const first = text.data();
    const char* const last = first + text.size();

    const std::from_chars_result result = std::from_chars(first, last, value);
    return result.ec == std::errc() && result.ptr == last;
}

std::string sanitizeForEcho(const std::string& verb) {
    const std::size_t limit = std::min(verb.size(), kMaxEchoedVerbLength);

    std::string result;
    result.reserve(limit + 3);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto byte = static_cast<unsigned char>(verb[i]);
        result.push_back(byte >= 0x20 && byte < 0x7f ? static_cast<char>(byte) : '?');
    }
    if (verb.size() > limit) {
        result += "...";
    }
    return result;
}

// Splits a line into tokens. Returns false and fills `error` on malformed input.
bool tokenize(const std::string& line, std::vector<std::string>& tokens, std::string& error) {
    // Trailing CR/LF is stripped rather than rejected: telnet and netcat both
    // send CRLF, and refusing them would make the server impossible to poke at
    // by hand.
    std::size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r')) {
        --end;
    }

    std::size_t i = 0;
    while (i < end) {
        while (i < end && isSpace(line[i])) {
            ++i;
        }
        if (i >= end) {
            break;
        }

        std::string token;

        if (line[i] == '"') {
            ++i;  // opening quote
            bool closed = false;

            while (i < end) {
                const char c = line[i];

                if (c == '\\' && i + 1 < end) {
                    const char escaped = line[i + 1];
                    i += 2;

                    switch (escaped) {
                        case 'n': token.push_back('\n'); break;
                        case 'r': token.push_back('\r'); break;
                        case 't': token.push_back('\t'); break;
                        case 'b': token.push_back('\b'); break;
                        case 'a': token.push_back('\a'); break;
                        case 'x':
                            // \xHH -> one raw byte. Anything else after \x is
                            // taken literally, matching redis-cli.
                            if (i + 1 < end && isHexDigit(line[i]) && isHexDigit(line[i + 1])) {
                                const int value = hexValue(line[i]) * 16 + hexValue(line[i + 1]);
                                token.push_back(static_cast<char>(value));
                                i += 2;
                            } else {
                                token.push_back('x');
                            }
                            break;
                        default:
                            // Unknown escape: the character stands for itself,
                            // so \" and \\ fall out of this case for free.
                            token.push_back(escaped);
                            break;
                    }
                    continue;
                }

                if (c == '"') {
                    closed = true;
                    ++i;
                    break;
                }

                token.push_back(c);
                ++i;
            }

            if (!closed) {
                error = "ERR unbalanced quotes in request";
                return false;
            }
            // A closing quote must end the token. `"abc"def` is a typo, not a
            // request to concatenate.
            if (i < end && !isSpace(line[i])) {
                error = "ERR unbalanced quotes in request";
                return false;
            }
        } else {
            while (i < end && !isSpace(line[i])) {
                token.push_back(line[i]);
                ++i;
            }
        }

        tokens.push_back(std::move(token));
    }

    return true;
}

}  // namespace

ParseResult ParseResult::success(Command command) {
    ParseResult result;
    result.ok_ = true;
    result.command_ = std::move(command);
    return result;
}

ParseResult ParseResult::failure(std::string error) {
    ParseResult result;
    result.ok_ = false;
    result.error_ = std::move(error);
    return result;
}

const char* CommandParser::name(CommandType type) noexcept {
    switch (type) {
        case CommandType::Set:    return "SET";
        case CommandType::Get:    return "GET";
        case CommandType::Del:    return "DEL";
        case CommandType::Exists: return "EXISTS";
        case CommandType::Keys:   return "KEYS";
        case CommandType::Save:   return "SAVE";
        case CommandType::Empty:  return "";
    }
    return "";  // unreachable for valid enum values; keeps every compiler quiet
}

ParseResult CommandParser::parse(const std::string& line) {
    std::vector<std::string> tokens;
    std::string error;

    if (!tokenize(line, tokens, error)) {
        return ParseResult::failure(std::move(error));
    }

    if (tokens.empty()) {
        return ParseResult::success(Command{});  // defaults to CommandType::Empty
    }

    // Verbs are case-insensitive; keys and values are not. Only tokens[0] is
    // folded, so `GET Name` and `GET name` stay different keys.
    const CommandSpec* spec = findCommand(toUpperAscii(tokens[0]));
    if (spec == nullptr) {
        return ParseResult::failure("ERR unknown command '" + sanitizeForEcho(tokens[0]) + "'");
    }

    const std::size_t argumentCount = tokens.size() - 1;
    if (argumentCount < spec->minArgs || argumentCount > spec->maxArgs) {
        return ParseResult::failure(std::string("ERR wrong number of arguments for '") +
                                    spec->name + "' command");
    }

    Command command;
    command.type = spec->type;
    if (argumentCount >= 1) {
        command.key = std::move(tokens[1]);
    }
    if (argumentCount >= 2) {
        command.value = std::move(tokens[2]);
    }

    // SET's optional `EX <seconds>` tail. Only SET has options, so this stays a
    // special case rather than a general option framework -- one that would be
    // speculative until a second command needs it.
    if (command.type == CommandType::Set && argumentCount > 2) {
        // "EX" needs its argument: `SET k v EX` is a truncated option, not a
        // missing positional argument, so it reports a syntax error.
        if (argumentCount != 4 || toUpperAscii(tokens[3]) != "EX") {
            return ParseResult::failure("ERR syntax error");
        }

        std::int64_t seconds = 0;
        if (!parseInt64(tokens[4], seconds)) {
            return ParseResult::failure("ERR value is not an integer or out of range");
        }

        // Zero and negative TTLs are refused rather than quietly treated as
        // "never expires" or "already gone". Either interpretation silently
        // does something the client did not ask for, and Discovery Document
        // section 17 lists invalid TTLs as an error worth reporting.
        if (seconds <= 0 || seconds > kMaxExpireSeconds) {
            return ParseResult::failure("ERR invalid expire time in 'SET' command");
        }

        command.expireSeconds = seconds;
    }

    return ParseResult::success(std::move(command));
}

}  // namespace vcache
