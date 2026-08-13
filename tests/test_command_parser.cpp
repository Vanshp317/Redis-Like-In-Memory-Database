// Phase 3 test suite -- the command parser.
//
// Half of these tests are about malformed input. Discovery Document section 17
// says the server must never crash on bad client data, and the parser is the
// first thing a hostile or buggy client touches.

#include <cstddef>
#include <cstdint>
#include <string>

#include "CommandParser.h"
#include "test_framework.h"

using vcache::Command;
using vcache::CommandParser;
using vcache::CommandType;
using vcache::ParseResult;

namespace {

// Parses and asserts success, so the happy-path tests stay one line each.
Command parseOk(const std::string& line) {
    const ParseResult result = CommandParser::parse(line);
    if (!result.ok()) {
        throw testing::AssertionFailure("           expected a successful parse of \"" + line +
                                        "\"\n           but got: " + result.error());
    }
    return result.command();
}

std::string parseError(const std::string& line) {
    const ParseResult result = CommandParser::parse(line);
    if (result.ok()) {
        throw testing::AssertionFailure("           expected \"" + line + "\" to be rejected");
    }
    return result.error();
}

}  // namespace

// ------------------------------------------------------------- empty input ----

VCACHE_TEST(BlankInputIsEmptyNotAnError) {
    // A client pressing Enter is not a protocol violation.
    for (const std::string& line : {std::string(""), std::string("   "),
                                    std::string("\r\n"), std::string("  \t \r\n")}) {
        const Command command = parseOk(line);
        CHECK(command.type == CommandType::Empty);
        CHECK(command.key.empty());
        CHECK(command.value.empty());
    }
}

// ------------------------------------------------------------- happy paths ----

VCACHE_TEST(ParsesSet) {
    const Command command = parseOk("SET name Vansh");
    CHECK(command.type == CommandType::Set);
    CHECK_EQ(command.key, std::string("name"));
    CHECK_EQ(command.value, std::string("Vansh"));
}

VCACHE_TEST(ParsesSingleKeyCommands) {
    const Command get = parseOk("GET name");
    CHECK(get.type == CommandType::Get);
    CHECK_EQ(get.key, std::string("name"));
    CHECK(get.value.empty());

    const Command del = parseOk("DEL name");
    CHECK(del.type == CommandType::Del);
    CHECK_EQ(del.key, std::string("name"));

    const Command exists = parseOk("EXISTS name");
    CHECK(exists.type == CommandType::Exists);
    CHECK_EQ(exists.key, std::string("name"));
}

VCACHE_TEST(ParsesKeys) {
    const Command command = parseOk("KEYS");
    CHECK(command.type == CommandType::Keys);
    CHECK(command.key.empty());
    CHECK(command.value.empty());
}

VCACHE_TEST(VerbsAreCaseInsensitive) {
    for (const std::string& verb : {std::string("SET"), std::string("set"),
                                    std::string("Set"), std::string("sEt")}) {
        const Command command = parseOk(verb + " key value");
        CHECK(command.type == CommandType::Set);
    }
}

VCACHE_TEST(KeysAndValuesKeepTheirCase) {
    // Only the verb is folded. Folding arguments would silently merge distinct
    // keys, which would be a data-loss bug rather than a convenience.
    const Command command = parseOk("get UserName");
    CHECK_EQ(command.key, std::string("UserName"));

    const Command set = parseOk("SET Key MiXeDcAsE");
    CHECK_EQ(set.key, std::string("Key"));
    CHECK_EQ(set.value, std::string("MiXeDcAsE"));
}

// ------------------------------------------------------------- whitespace ----

VCACHE_TEST(ExtraWhitespaceIsIgnored) {
    const Command command = parseOk("   SET \t  name    Vansh   ");
    CHECK(command.type == CommandType::Set);
    CHECK_EQ(command.key, std::string("name"));
    CHECK_EQ(command.value, std::string("Vansh"));
}

VCACHE_TEST(TrailingCrLfIsStripped) {
    // telnet and netcat both send CRLF; a stray \r must not become part of the
    // value, or `SET k v` from a terminal would store "v\r".
    for (const std::string& suffix : {std::string("\n"), std::string("\r\n"),
                                      std::string("\r"), std::string("")}) {
        const Command command = parseOk("SET key value" + suffix);
        CHECK_EQ(command.value, std::string("value"));
    }
}

// ------------------------------------------------------------------ arity ----

VCACHE_TEST(WrongArityIsRejectedForEveryCommand) {
    CHECK_EQ(parseError("SET"), std::string("ERR wrong number of arguments for 'SET' command"));
    CHECK_EQ(parseError("SET key"), std::string("ERR wrong number of arguments for 'SET' command"));

    // SET spans 2-4 arguments since Phase 6, so only five or more is an arity
    // error. Three or four arguments get as far as option validation, which
    // reports a syntax error instead -- see AMalformedOptionIsASyntaxError.
    CHECK_EQ(parseError("SET a b c d e"),
             std::string("ERR wrong number of arguments for 'SET' command"));

    CHECK_EQ(parseError("GET"), std::string("ERR wrong number of arguments for 'GET' command"));
    CHECK_EQ(parseError("GET a b"), std::string("ERR wrong number of arguments for 'GET' command"));

    CHECK_EQ(parseError("DEL"), std::string("ERR wrong number of arguments for 'DEL' command"));
    CHECK_EQ(parseError("EXISTS"), std::string("ERR wrong number of arguments for 'EXISTS' command"));
    CHECK_EQ(parseError("KEYS extra"), std::string("ERR wrong number of arguments for 'KEYS' command"));
}

// ------------------------------------------------------- SET ... EX (§8) ----

VCACHE_TEST(SetWithExpirationParses) {
    const Command command = parseOk("SET session abc123 EX 60");
    CHECK(command.type == CommandType::Set);
    CHECK_EQ(command.key, std::string("session"));
    CHECK_EQ(command.value, std::string("abc123"));
    CHECK(command.expireSeconds.has_value());
    CHECK_EQ(*command.expireSeconds, std::int64_t{60});
}

VCACHE_TEST(SetWithoutExpirationLeavesTheTtlUnset) {
    const Command command = parseOk("SET key value");
    CHECK(!command.expireSeconds.has_value());
}

VCACHE_TEST(TheExKeywordIsCaseInsensitive) {
    // Consistent with verbs: keywords fold, data does not.
    for (const std::string& keyword : {std::string("EX"), std::string("ex"),
                                       std::string("Ex"), std::string("eX")}) {
        const Command command = parseOk("SET key value " + keyword + " 30");
        CHECK(command.expireSeconds.has_value());
        CHECK_EQ(*command.expireSeconds, std::int64_t{30});
    }
}

VCACHE_TEST(AMalformedOptionIsASyntaxError) {
    // Distinguished from an arity error on purpose: the argument count is
    // plausible, the option itself is not.
    CHECK_EQ(parseError("SET key value EX"), std::string("ERR syntax error"));
    CHECK_EQ(parseError("SET key value PX 60"), std::string("ERR syntax error"));
    CHECK_EQ(parseError("SET key value 60"), std::string("ERR syntax error"));
    CHECK_EQ(parseError("SET key value NX EX"), std::string("ERR syntax error"));
}

VCACHE_TEST(TooManyArgumentsIsStillAnArityError) {
    CHECK_EQ(parseError("SET key value EX 60 extra"),
             std::string("ERR wrong number of arguments for 'SET' command"));
}

VCACHE_TEST(ANonNumericTtlIsRejected) {
    CHECK_EQ(parseError("SET key value EX abc"),
             std::string("ERR value is not an integer or out of range"));
    // Trailing junk must not be silently ignored -- "60seconds" is a typo, and
    // treating it as 60 would hide the mistake.
    CHECK_EQ(parseError("SET key value EX 60seconds"),
             std::string("ERR value is not an integer or out of range"));
    CHECK_EQ(parseError("SET key value EX 1.5"),
             std::string("ERR value is not an integer or out of range"));
    CHECK_EQ(parseError("SET key value EX \"\""),
             std::string("ERR value is not an integer or out of range"));
}

VCACHE_TEST(AnOverflowingTtlIsRejected) {
    // 10^25 does not fit in an int64 at all.
    CHECK_EQ(parseError("SET key value EX 10000000000000000000000000"),
             std::string("ERR value is not an integer or out of range"));

    // This one fits in an int64 but would overflow a time_point when added to
    // now, which is undefined behaviour rather than a wrong answer.
    CHECK_EQ(parseError("SET key value EX 9223372036854775807"),
             std::string("ERR invalid expire time in 'SET' command"));
}

VCACHE_TEST(ZeroAndNegativeTtlsAreRejected) {
    // Neither silently-permanent nor silently-already-gone: both would do
    // something the client did not ask for.
    CHECK_EQ(parseError("SET key value EX 0"),
             std::string("ERR invalid expire time in 'SET' command"));
    CHECK_EQ(parseError("SET key value EX -1"),
             std::string("ERR invalid expire time in 'SET' command"));
    CHECK_EQ(parseError("SET key value EX -9999"),
             std::string("ERR invalid expire time in 'SET' command"));
}

VCACHE_TEST(TheLargestAcceptedTtlIsAHundredYears) {
    constexpr std::int64_t kHundredYears = 100LL * 365 * 24 * 60 * 60;

    const Command command = parseOk("SET key value EX " + std::to_string(kHundredYears));
    CHECK_EQ(*command.expireSeconds, kHundredYears);

    CHECK_EQ(parseError("SET key value EX " + std::to_string(kHundredYears + 1)),
             std::string("ERR invalid expire time in 'SET' command"));
}

VCACHE_TEST(OnlySetAcceptsOptions) {
    // GET/DEL/EXISTS have fixed arity, so an option there is an arity error.
    CHECK_EQ(parseError("GET key EX 60"),
             std::string("ERR wrong number of arguments for 'GET' command"));
}

// -------------------------------------------------------- unknown commands ----

VCACHE_TEST(UnknownCommandIsRejected) {
    CHECK_EQ(parseError("FLUSHALL"), std::string("ERR unknown command 'FLUSHALL'"));
    CHECK_EQ(parseError("INCR key"), std::string("ERR unknown command 'INCR'"));
    CHECK_EQ(parseError("gett key"), std::string("ERR unknown command 'gett'"));
}

VCACHE_TEST(UnknownCommandEchoIsTruncated) {
    // A typo must not turn into an amplification: 10 KB in, a bounded reply out.
    const std::string huge(10000, 'A');
    const std::string error = parseError(huge);

    CHECK(error.size() < std::size_t{100});
    CHECK(error.find("...") != std::string::npos);
}

VCACHE_TEST(UnknownCommandEchoStripsControlBytes) {
    // The protocol is line-oriented, so echoing raw CR/LF back would let a
    // client forge extra response lines. Non-printables become '?'.
    // The literal is split so the hex escape does not greedily swallow the 'B'.
    const std::string error = parseError(std::string("A\x01\x7f\xff" "B"));

    CHECK(error.find('\x01') == std::string::npos);
    CHECK(error.find('\xff') == std::string::npos);
    CHECK(error.find("A???B") != std::string::npos);
}

// ------------------------------------------------------------------ quotes ----

VCACHE_TEST(QuotedArgumentsPreserveSpaces) {
    const Command command = parseOk("SET greeting \"hello world\"");
    CHECK_EQ(command.key, std::string("greeting"));
    CHECK_EQ(command.value, std::string("hello world"));
}

VCACHE_TEST(QuotedKeysWork) {
    const Command command = parseOk("GET \"key with spaces\"");
    CHECK_EQ(command.key, std::string("key with spaces"));
}

VCACHE_TEST(QuotedEmptyStringIsAValue) {
    // Distinct from a missing argument: this is a present, empty value.
    const Command command = parseOk("SET key \"\"");
    CHECK(command.type == CommandType::Set);
    CHECK_EQ(command.key, std::string("key"));
    CHECK(command.value.empty());
}

VCACHE_TEST(EscapeSequencesAreDecoded) {
    const Command command = parseOk("SET key \"line1\\nline2\\ttabbed\"");
    CHECK_EQ(command.value, std::string("line1\nline2\ttabbed"));

    const Command quotes = parseOk("SET key \"say \\\"hi\\\"\"");
    CHECK_EQ(quotes.value, std::string("say \"hi\""));

    const Command backslash = parseOk("SET key \"a\\\\b\"");
    CHECK_EQ(backslash.value, std::string("a\\b"));
}

VCACHE_TEST(HexEscapesProduceRawBytes) {
    // This is what makes the text protocol able to carry the binary-safe values
    // the storage layer already supports.
    const Command command = parseOk("SET key \"\\x00\\xff\\x41\"");
    CHECK_EQ(command.value.size(), std::size_t{3});
    CHECK_EQ(command.value[0], '\0');
    CHECK_EQ(static_cast<unsigned char>(command.value[1]), static_cast<unsigned char>(0xff));
    CHECK_EQ(command.value[2], 'A');
}

VCACHE_TEST(IncompleteHexEscapeIsLiteral) {
    const Command command = parseOk("SET key \"\\xZZ\"");
    CHECK_EQ(command.value, std::string("xZZ"));

    const Command truncated = parseOk("SET key \"\\x4\"");
    CHECK_EQ(truncated.value, std::string("x4"));
}

VCACHE_TEST(UnknownEscapeYieldsTheCharacterItself) {
    const Command command = parseOk("SET key \"\\q\"");
    CHECK_EQ(command.value, std::string("q"));
}

VCACHE_TEST(UnbalancedQuotesAreRejected) {
    CHECK_EQ(parseError("SET key \"unterminated"),
             std::string("ERR unbalanced quotes in request"));
    CHECK_EQ(parseError("SET key \""), std::string("ERR unbalanced quotes in request"));
    CHECK_EQ(parseError("SET key \"trailing backslash\\"),
             std::string("ERR unbalanced quotes in request"));
}

VCACHE_TEST(ClosingQuoteMustEndTheToken) {
    // `"abc"def` is a typo, not a concatenation request.
    CHECK_EQ(parseError("SET key \"abc\"def"), std::string("ERR unbalanced quotes in request"));
}

VCACHE_TEST(QuotesInsideBareTokensAreLiteral) {
    // Only a quote in the first position opens a quoted token.
    const Command command = parseOk("SET key ab\"cd");
    CHECK_EQ(command.value, std::string("ab\"cd"));
}

// ------------------------------------------------- robustness (section 17) ----

VCACHE_TEST(PathologicalInputIsRejectedWithoutCrashing) {
    // None of these should be accepted, and none should take the process down.
    const std::string inputs[] = {
        "\"",
        "\"\"",
        "\\",
        "\\\\\\\\",
        "\"\"\"\"\"\"",
        std::string(100000, ' '),
        std::string(100000, '"'),
        std::string(100000, 'x'),
        "SET " + std::string(50000, 'k') + " " + std::string(50000, 'v'),
        std::string("\0\0\0", 3),
        "SET\t\t\t",
    };

    for (const std::string& input : inputs) {
        const ParseResult result = CommandParser::parse(input);
        // The only contract is: it returns, and a failure carries a message.
        if (!result.ok()) {
            CHECK(!result.error().empty());
        }
    }
}

VCACHE_TEST(VeryLongArgumentsAreAcceptedIntact) {
    // Capping request size is the server's job in Phase 4 (read buffer limits),
    // not the parser's -- the parser must not silently truncate.
    const std::string longValue(100000, 'v');
    const Command command = parseOk("SET key " + longValue);
    CHECK_EQ(command.value.size(), longValue.size());
    CHECK(command.value == longValue);
}

VCACHE_TEST(ParserIsStateless) {
    // Phase 5 calls this from every worker thread at once, so one parse must not
    // affect the next.
    const Command first = parseOk("SET a 1");
    parseError("BOGUS");
    const Command second = parseOk("GET b");

    CHECK_EQ(first.key, std::string("a"));
    CHECK_EQ(first.value, std::string("1"));
    CHECK(second.type == CommandType::Get);
    CHECK_EQ(second.key, std::string("b"));
    CHECK(second.value.empty());
}

VCACHE_TEST(NameMapsEveryCommandType) {
    CHECK_EQ(std::string(CommandParser::name(CommandType::Set)), std::string("SET"));
    CHECK_EQ(std::string(CommandParser::name(CommandType::Get)), std::string("GET"));
    CHECK_EQ(std::string(CommandParser::name(CommandType::Del)), std::string("DEL"));
    CHECK_EQ(std::string(CommandParser::name(CommandType::Exists)), std::string("EXISTS"));
    CHECK_EQ(std::string(CommandParser::name(CommandType::Keys)), std::string("KEYS"));
    CHECK_EQ(std::string(CommandParser::name(CommandType::Empty)), std::string(""));
}

// ------------------------------------------------------- end-to-end shapes ----

VCACHE_TEST(DocumentedExamplesParse) {
    // The exact lines from Discovery Document sections 6.1 and 19.
    const Command set = parseOk("SET name Vansh");
    CHECK(set.type == CommandType::Set);
    CHECK_EQ(set.key, std::string("name"));
    CHECK_EQ(set.value, std::string("Vansh"));

    const Command get = parseOk("GET name");
    CHECK(get.type == CommandType::Get);

    const Command del = parseOk("DEL name");
    CHECK(del.type == CommandType::Del);

    const Command username = parseOk("SET username Vansh");
    CHECK_EQ(username.value, std::string("Vansh"));
}

int main() {
    return testing::runAll();
}
