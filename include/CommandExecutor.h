#pragma once

#include <optional>
#include <string>

#include "CommandParser.h"
#include "Database.h"
#include "Persistence.h"

namespace vcache {

// Joins the three layers built so far: takes one line of raw protocol text,
// parses it, runs it against the database, and renders the reply.
//
// This is deliberately separate from Server. Everything interesting about
// request handling lives here as a pure string-in / string-out function, so it
// can be tested exhaustively without opening a single socket. Server is then a
// thin shell that only moves bytes.
//
// Response format (all replies are one line unless noted):
//
//   SET     -> OK
//   GET     -> the value, or (nil) if the key is absent
//   DEL     -> 1 or 0
//   EXISTS  -> 1 or 0
//   KEYS    -> (empty list)
//              or a count line followed by one numbered, quoted key per line:
//                  (2 keys)
//                  1) "alpha"
//                  2) "beta"
//   SAVE    -> OK, or an error if persistence is not configured or the write
//              failed
//   error   -> ERR <what went wrong>
//   blank   -> nothing at all
//
// Values and keys are escaped on the way out using the same scheme the parser
// accepts on the way in (\n, \r, \t, \\, \" and \xHH). Two reasons: a raw
// newline inside a value would otherwise break the one-reply-per-line framing,
// and escaping makes the output round-trip -- whatever GET prints can be handed
// straight back to SET.
//
// Holds a reference to the Database, so the Database must outlive it.
class CommandExecutor {
public:
    // `store` may be null, which is what "persistence is not configured" means:
    // SAVE then reports that rather than silently pretending to have written a
    // file. The pointer is not owned and must outlive this executor.
    explicit CommandExecutor(Database& database, SnapshotStore* store = nullptr);

    // Empty optional means "say nothing" -- the client sent a blank line.
    std::optional<std::string> execute(const std::string& requestLine);

    // Exposed for the server's own diagnostics and for tests.
    static std::string escapeForWire(const std::string& raw);

private:
    Database& database_;
    SnapshotStore* store_ = nullptr;
};

}  // namespace vcache
