#include "CommandExecutor.h"

#include "Protocol.h"

#include <chrono>
#include <string>
#include <vector>

namespace vcache {

namespace {


void appendHexByte(std::string& out, unsigned char byte) {
    static const char kDigits[] = "0123456789abcdef";
    out += "\\x";
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0f]);
}

}  // namespace

std::string CommandExecutor::escapeForWire(const std::string& raw) {
    std::string result;
    result.reserve(raw.size());

    for (const char c : raw) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            case '"':  result += "\\\""; break;
            default: {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 0x20 || byte >= 0x7f) {
                    appendHexByte(result, byte);
                } else {
                    result.push_back(c);
                }
                break;
            }
        }
    }
    return result;
}

CommandExecutor::CommandExecutor(Database& database, SnapshotStore* store)
    : database_(database), store_(store) {}

std::optional<std::string> CommandExecutor::execute(const std::string& requestLine) {
    const ParseResult parsed = CommandParser::parse(requestLine);
    if (!parsed.ok()) {
        // The parser already produced client-facing text; pass it through
        // untouched so there is exactly one place that words errors.
        return protocol::error(parsed.error());
    }

    const Command& command = parsed.command();
    switch (command.type) {
        case CommandType::Empty:
            return std::nullopt;  // a blank line earns silence, not an error

        case CommandType::Set: {
            // The parser has already guaranteed any TTL here is positive and
            // small enough not to overflow, so no re-validation is needed.
            const SetOutcome outcome =
                command.expireSeconds.has_value()
                    ? database_.set(command.key, command.value,
                                    std::chrono::seconds(*command.expireSeconds))
                    : database_.set(command.key, command.value);

            if (outcome == SetOutcome::Rejected) {
                // Nothing was stored, and saying OK would be a lie the client
                // could not detect until a later GET returned (nil).
                return protocol::error("ERR value is larger than the configured max-memory");
            }
            return protocol::status("OK");
        }

        case CommandType::Get: {
            const std::optional<std::string> value = database_.get(command.key);
            if (!value.has_value()) {
                return protocol::nil();
            }
            // Distinct from nil: a stored empty value replies with a Value tag
            // and an empty payload, which is present-but-empty.
            return protocol::value(escapeForWire(*value));
        }

        case CommandType::Del:
            return protocol::integer(database_.del(command.key) ? 1 : 0);

        case CommandType::Exists:
            return protocol::integer(database_.exists(command.key) ? 1 : 0);

        case CommandType::Keys: {
            const std::vector<std::string> keys = database_.keys();

            // The count comes first so a client knows exactly how many lines
            // follow. An empty keyspace is simply *0 -- no special case.
            std::string response = protocol::arrayHeader(keys.size());
            for (const std::string& key : keys) {
                response += "\n" + protocol::value(escapeForWire(key));
            }
            return response;
        }

        case CommandType::Save: {
            if (store_ == nullptr) {
                return protocol::error("ERR persistence is not configured");
            }

            const SaveOutcome outcome = store_->save(database_);
            if (!outcome.ok) {
                // A failed SAVE is reported, not hidden: a client that asked
                // for durability needs to know it did not get it.
                return protocol::error("ERR " + outcome.error);
            }
            return protocol::status("OK");
        }
    }

    // Unreachable for any valid enum value; keeps every compiler quiet.
    return protocol::error("ERR internal error");
}

}  // namespace vcache
