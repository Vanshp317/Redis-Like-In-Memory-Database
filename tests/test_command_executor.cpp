// Phase 4a test suite -- request text in, response text out.
//
// No sockets here. Everything about what a command *means* is tested at this
// level, which leaves the socket tests free to check only the byte plumbing.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

#include "CommandExecutor.h"
#include "Database.h"
#include "test_framework.h"

using vcache::CommandExecutor;
using vcache::Database;

namespace {

// Runs one line and asserts a reply came back.
std::string reply(CommandExecutor& executor, const std::string& line) {
    const std::optional<std::string> response = executor.execute(line);
    if (!response.has_value()) {
        throw testing::AssertionFailure("           expected a reply to \"" + line +
                                        "\" but the executor stayed silent");
    }
    return *response;
}

}  // namespace

// -------------------------------------------------------------- happy path ----

VCACHE_TEST(SetRepliesOk) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "SET name Vansh"), std::string("+OK"));
    CHECK_EQ(db.size(), std::size_t{1});
    CHECK_EQ(*db.get("name"), std::string("Vansh"));
}

VCACHE_TEST(GetRepliesWithTheValue) {
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET name Vansh");
    CHECK_EQ(reply(executor, "GET name"), std::string("$Vansh"));
}

VCACHE_TEST(GetMissingKeyRepliesNil) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "GET absent"), std::string("_"));
}

VCACHE_TEST(EmptyValueIsNotNil) {
    // A key holding "" exists; a key that was never set does not. The protocol
    // has to keep those apart or clients cannot trust EXISTS.
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET key \"\"");
    CHECK_EQ(reply(executor, "GET key"), std::string("$"));
    CHECK_EQ(reply(executor, "GET missing"), std::string("_"));
    CHECK_EQ(reply(executor, "EXISTS key"), std::string(":1"));
}

VCACHE_TEST(DelRepliesOneThenZero) {
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET key value");
    CHECK_EQ(reply(executor, "DEL key"), std::string(":1"));
    CHECK_EQ(reply(executor, "DEL key"), std::string(":0"));
    CHECK_EQ(db.size(), std::size_t{0});
}

VCACHE_TEST(ExistsRepliesOneOrZero) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "EXISTS key"), std::string(":0"));
    reply(executor, "SET key value");
    CHECK_EQ(reply(executor, "EXISTS key"), std::string(":1"));
}

VCACHE_TEST(OverwriteStillRepliesOk) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "SET key first"), std::string("+OK"));
    CHECK_EQ(reply(executor, "SET key second"), std::string("+OK"));
    CHECK_EQ(reply(executor, "GET key"), std::string("$second"));
}

// -------------------------------------------------------------------- KEYS ----

VCACHE_TEST(KeysOnAnEmptyDatabase) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "KEYS"), std::string("*0"));
}

VCACHE_TEST(KeysListsWithACountHeader) {
    Database db;
    CommandExecutor executor(db);
    reply(executor, "SET only value");

    // The count header says exactly how many tagged lines follow.
    CHECK_EQ(reply(executor, "KEYS"), std::string("*1\n$only"));
}

VCACHE_TEST(KeysNumbersEveryEntry) {
    Database db;
    CommandExecutor executor(db);
    reply(executor, "SET a 1");
    reply(executor, "SET b 2");
    reply(executor, "SET c 3");

    const std::string response = reply(executor, "KEYS");

    // Order is unspecified, so check structure rather than exact text.
    CHECK(response.rfind("*3\n", 0) == 0);
    CHECK(response.find("$a") != std::string::npos);
    CHECK(response.find("$b") != std::string::npos);
    CHECK(response.find("$c") != std::string::npos);
}

// ------------------------------------------------------------------ errors ----

VCACHE_TEST(ParserErrorsReachTheClientUnchanged) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "FLUSHALL"), std::string("-ERR unknown command 'FLUSHALL'"));
    CHECK_EQ(reply(executor, "GET"),
             std::string("-ERR wrong number of arguments for 'GET' command"));
    CHECK_EQ(reply(executor, "SET k \"unterminated"),
             std::string("-ERR unbalanced quotes in request"));
}

VCACHE_TEST(AFailedCommandLeavesTheDatabaseUntouched) {
    Database db;
    CommandExecutor executor(db);
    reply(executor, "SET key value");

    reply(executor, "SET key");        // arity error
    reply(executor, "BOGUS key");      // unknown verb
    reply(executor, "SET k \"oops");   // quote error

    CHECK_EQ(db.size(), std::size_t{1});
    CHECK_EQ(reply(executor, "GET key"), std::string("$value"));
}

VCACHE_TEST(BlankLinesProduceNoReply) {
    // Silence, not an error -- otherwise a stray newline from telnet would
    // produce noise on every keypress.
    Database db;
    CommandExecutor executor(db);

    CHECK(!executor.execute("").has_value());
    CHECK(!executor.execute("   ").has_value());
    CHECK(!executor.execute("\r").has_value());
}

// ---------------------------------------------------------------- escaping ----

VCACHE_TEST(OrdinaryValuesAreNotMangled) {
    // The escaping must be invisible for normal text, or every reply would be
    // harder to read than it needs to be.
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET key hello-world_123.456");
    CHECK_EQ(reply(executor, "GET key"), std::string("$hello-world_123.456"));
}

VCACHE_TEST(NewlinesInValuesAreEscaped) {
    // Critical for framing: a raw newline in a value would look like the end of
    // the reply, and the client would treat the rest as a second response.
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET key \"line1\\nline2\"");
    const std::string response = reply(executor, "GET key");

    CHECK_EQ(response, std::string("$line1\\nline2"));
    CHECK(response.find('\n') == std::string::npos);  // one line on the wire
}

VCACHE_TEST(BackslashesAreEscapedSoOutputIsUnambiguous) {
    // Without escaping the backslash itself, a stored two-character "\n" and a
    // stored newline would print identically.
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET literal \"a\\\\nb\"");  // stores: a \ n b
    reply(executor, "SET newline \"a\\nb\"");    // stores: a <LF> b

    CHECK_EQ(reply(executor, "GET literal"), std::string("$a\\\\nb"));
    CHECK_EQ(reply(executor, "GET newline"), std::string("$a\\nb"));
    CHECK(reply(executor, "GET literal") != reply(executor, "GET newline"));
}

VCACHE_TEST(BinaryBytesComeBackAsHexEscapes) {
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET key \"\\x00\\xff\\x1b\"");
    CHECK_EQ(reply(executor, "GET key"), std::string("$\\x00\\xff\\x1b"));
}

VCACHE_TEST(OutputRoundTripsBackThroughInput) {
    // The property that makes the escaping worth having: whatever GET prints
    // can be pasted straight back into SET and produce the same bytes.
    Database db;
    CommandExecutor executor(db);

    const std::string originalCommand = "SET first \"tab\\there\\nnewline \\x00 nul \\\\ slash \\\" quote\"";
    reply(executor, originalCommand);
    // substr(1) strips the '$' tag, leaving the escaped value itself.
    const std::string printed = reply(executor, "GET first").substr(1);

    reply(executor, "SET second \"" + printed + "\"");
    CHECK_EQ(reply(executor, "GET second").substr(1), printed);
    CHECK(*db.get("first") == *db.get("second"));
}

VCACHE_TEST(KeysWithAwkwardCharactersAreEscaped) {
    Database db;
    CommandExecutor executor(db);
    reply(executor, "SET \"key with space\" v");

    CHECK_EQ(reply(executor, "KEYS"), std::string("*1\n$key with space"));
}

// ------------------------------------------------------------------- state ----

VCACHE_TEST(StatePersistsAcrossCommands) {
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET counter 1");
    reply(executor, "SET other 2");
    reply(executor, "DEL other");

    CHECK_EQ(reply(executor, "GET counter"), std::string("$1"));  // a value, not an integer reply
    CHECK_EQ(reply(executor, "EXISTS other"), std::string(":0"));
    CHECK_EQ(reply(executor, "KEYS"), std::string("*1\n$counter"));
}

VCACHE_TEST(TwoExecutorsShareOneDatabase) {
    // Phase 5 gives every worker thread its own executor over one shared
    // database, so this has to hold.
    Database db;
    CommandExecutor first(db);
    CommandExecutor second(db);

    reply(first, "SET key written-by-first");
    CHECK_EQ(reply(second, "GET key"), std::string("$written-by-first"));
}

// --------------------------------------------------------- TTL (Phase 6) ----

VCACHE_TEST(SetWithExpirationRepliesOkAndSetsATtl) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "SET session abc123 EX 60"), std::string("+OK"));
    CHECK_EQ(reply(executor, "GET session"), std::string("$abc123"));
    CHECK(db.ttl("session").has_value());
}

VCACHE_TEST(AnExpiredKeyRepliesNil) {
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET flash value EX 1");
    CHECK_EQ(reply(executor, "GET flash"), std::string("$value"));

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    CHECK_EQ(reply(executor, "GET flash"), std::string("_"));
    CHECK_EQ(reply(executor, "EXISTS flash"), std::string(":0"));
    CHECK_EQ(reply(executor, "DEL flash"), std::string(":0"));
    CHECK_EQ(reply(executor, "KEYS"), std::string("*0"));
}

VCACHE_TEST(TtlErrorsReachTheClient) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "SET key value EX 0"),
             std::string("-ERR invalid expire time in 'SET' command"));
    CHECK_EQ(reply(executor, "SET key value EX abc"),
             std::string("-ERR value is not an integer or out of range"));
    CHECK_EQ(reply(executor, "SET key value PX 60"), std::string("-ERR syntax error"));

    // A rejected SET must not have stored anything.
    CHECK_EQ(reply(executor, "EXISTS key"), std::string(":0"));
}

VCACHE_TEST(PlainSetClearsAnExistingTtlOverTheProtocol) {
    Database db;
    CommandExecutor executor(db);

    reply(executor, "SET key value EX 60");
    CHECK(db.ttl("key").has_value());

    reply(executor, "SET key value");
    CHECK(!db.ttl("key").has_value());
}

// -------------------------------------------------------- SAVE (Phase 7) ----

VCACHE_TEST(SaveWithoutPersistenceConfiguredSaysSo) {
    // Silently replying OK would be the worst answer: the client would believe
    // its data is durable when nothing was written anywhere.
    Database db;
    CommandExecutor executor(db);  // no store

    CHECK_EQ(reply(executor, "SAVE"), std::string("-ERR persistence is not configured"));
}

VCACHE_TEST(SaveWritesASnapshot) {
    const std::string path = "/tmp/vcache-executor-test.snapshot";
    std::remove(path.c_str());

    Database db;
    vcache::SnapshotStore store(path);
    CommandExecutor executor(db, &store);

    reply(executor, "SET name Vansh");
    CHECK_EQ(reply(executor, "SAVE"), std::string("+OK"));
    CHECK_EQ(store.saveCount(), std::size_t{1});

    Database restored;
    CHECK(store.load(restored).status == vcache::LoadStatus::Loaded);
    CHECK_EQ(*restored.get("name"), std::string("Vansh"));

    std::remove(path.c_str());
}

VCACHE_TEST(AFailedSaveIsReportedToTheClient) {
    Database db;
    vcache::SnapshotStore store("/nonexistent-directory/vcache.snapshot");
    CommandExecutor executor(db, &store);

    const std::string response = reply(executor, "SAVE");
    CHECK(response.rfind("-ERR ", 0) == 0);
    CHECK(response.size() > std::size_t{4});
}

VCACHE_TEST(SaveTakesNoArguments) {
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "SAVE now"),
             std::string("-ERR wrong number of arguments for 'SAVE' command"));
}

VCACHE_TEST(DocumentedSessionWorksEndToEnd) {
    // The transcript from Discovery Document section 19, minus the parts that
    // belong to later phases (EX in Phase 6, SAVE in Phase 7).
    Database db;
    CommandExecutor executor(db);

    CHECK_EQ(reply(executor, "SET username Vansh"), std::string("+OK"));
    CHECK_EQ(reply(executor, "GET username"), std::string("$Vansh"));
    CHECK_EQ(reply(executor, "SET session abc123 EX 30"), std::string("+OK"));

    // SAVE completes the section 19 transcript once a store is attached.
    const std::string path = "/tmp/vcache-session-test.snapshot";
    std::remove(path.c_str());
    vcache::SnapshotStore store(path);
    CommandExecutor persistent(db, &store);
    CHECK_EQ(reply(persistent, "SAVE"), std::string("+OK"));
    std::remove(path.c_str());
}

int main() {
    return testing::runAll();
}
