// Client library test suite -- exercised against a real server over real
// sockets, since a mocked server would only prove the mock agrees with itself.

#include <chrono>
#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include "Database.h"
#include "Server.h"
#include "VCacheClient.h"
#include "test_framework.h"

using vcache::Reply;
using vcache::VCacheClient;
using vcache::protocol::ReplyType;

namespace {

class RunningServer {
public:
    RunningServer() {
        vcache::ServerConfig config;
        config.port = 0;
        config.threadCount = 4;
        server_ = std::make_unique<vcache::Server>(database_, config);
        if (!server_->start()) {
            throw std::runtime_error("server failed to start: " + server_->error());
        }
        thread_ = std::thread([this] { server_->run(); });
    }

    ~RunningServer() {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    std::uint16_t port() const { return server_->port(); }
    vcache::Database& database() { return database_; }

private:
    vcache::Database database_;
    std::unique_ptr<vcache::Server> server_;
    std::thread thread_;
};

// Connects or fails the test.
void connectOrFail(VCacheClient& client, std::uint16_t port) {
    std::string error;
    if (!client.connect("127.0.0.1", port, error)) {
        throw testing::AssertionFailure("           could not connect: " + error);
    }
}

}  // namespace

// ------------------------------------------------------------- connecting ----

VCACHE_TEST(ConnectsToARunningServer) {
    RunningServer server;
    VCacheClient client;

    std::string error;
    CHECK(client.connect("127.0.0.1", server.port(), error));
    CHECK(client.connected());
    CHECK(error.empty());
}

VCACHE_TEST(ConnectingToAClosedPortFails) {
    VCacheClient client;
    std::string error;

    // Port 1 is privileged and nothing here listens on it.
    CHECK(!client.connect("127.0.0.1", 1, error));
    CHECK(!client.connected());
    CHECK(!error.empty());
}

VCACHE_TEST(AnUnresolvableHostFails) {
    VCacheClient client;
    std::string error;

    CHECK(!client.connect("no-such-host.invalid", 6379, error));
    CHECK(error.find("resolve") != std::string::npos);
}

VCACHE_TEST(LocalhostResolves) {
    // getaddrinfo rather than inet_pton, so names work and not just addresses.
    RunningServer server;
    VCacheClient client;

    std::string error;
    CHECK(client.connect("localhost", server.port(), error));
}

VCACHE_TEST(CommandsOnADisconnectedClientReportIt) {
    VCacheClient client;
    const Reply reply = client.command("GET key");

    CHECK(!reply.delivered);
    CHECK_EQ(reply.transportError, std::string("not connected"));
}

// ------------------------------------------------------- single-line replies ----

VCACHE_TEST(SetAndGetRoundTrip) {
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    const Reply set = client.command("SET name Vansh");
    CHECK(set.delivered);
    CHECK(!set.isError());
    CHECK(set.type == ReplyType::Status);
    CHECK_EQ(set.payload, std::string("OK"));

    const Reply get = client.command("GET name");
    CHECK(get.type == ReplyType::Value);
    CHECK_EQ(get.payload, std::string("Vansh"));
}

VCACHE_TEST(ServerErrorsAreDeliveredNotTransportFailures) {
    // The distinction matters: an ERR reply is a successful exchange carrying
    // bad news, and the connection stays usable.
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    const Reply reply = client.command("BOGUS");
    CHECK(reply.delivered);
    CHECK(reply.isError());
    CHECK_EQ(reply.payload, std::string("ERR unknown command 'BOGUS'"));

    CHECK(client.connected());
    CHECK_EQ(client.command("SET k v").payload, std::string("OK"));
}

VCACHE_TEST(NilAndEmptyArrayAreDistinct) {
    // "no such key" and "an empty list of keys" are different answers, and the
    // tags keep them apart without either needing a magic string.
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    const Reply nil = client.command("GET missing");
    CHECK(nil.isNil());
    CHECK(!nil.isError());

    const Reply empty = client.command("KEYS");
    CHECK(empty.isArray());
    CHECK(empty.elements.empty());
}

// -------------------------------------------------------- multi-line replies ----

VCACHE_TEST(KeysRepliesAreAssembledInFull) {
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    client.command("SET a 1");
    client.command("SET b 2");
    client.command("SET c 3");

    const Reply reply = client.command("KEYS");
    CHECK(reply.delivered);
    CHECK(reply.isArray());
    CHECK_EQ(reply.elements.size(), std::size_t{3});

    const std::set<std::string> got(reply.elements.begin(), reply.elements.end());
    CHECK(got.count("a") == 1);
    CHECK(got.count("b") == 1);
    CHECK(got.count("c") == 1);
}

VCACHE_TEST(ASingleKeyUsesTheSingularHeader) {
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());
    client.command("SET only 1");

    const Reply reply = client.command("KEYS");
    CHECK(reply.isArray());
    CHECK_EQ(reply.elements.size(), std::size_t{1});
    CHECK_EQ(reply.elements.front(), std::string("only"));
}

VCACHE_TEST(AValueLookingLikeAKeysHeaderDoesNotHangTheClient) {
    // The bug that motivated tagging replies. Before tags, `(2 keys)` stored as
    // a VALUE came back byte-for-byte identical to a KEYS header, and a client
    // reading the reply alone waited forever for two lines never sent.
    //
    // Now the '$' tag settles it, with no reference to which command was sent.
    // If this ever regresses the suite hangs rather than fails -- which the
    // CTest timeout turns back into a failure.
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    client.command("SET trap \"(2 keys)\"");

    const Reply reply = client.command("GET trap");
    CHECK(reply.delivered);
    CHECK(reply.type == ReplyType::Value);   // structurally a value, not an array
    CHECK(!reply.isArray());
    CHECK_EQ(reply.payload, std::string("(2 keys)"));

    // The connection is still in sync afterwards.
    CHECK_EQ(client.command("GET trap").payload, std::string("(2 keys)"));
    CHECK_EQ(client.command("SET after ok").payload, std::string("OK"));
}

VCACHE_TEST(AValueLookingLikeAnErrorIsNotAnError) {
    // The second bug tags fixed. A client that decided "is this an error?" by
    // string-matching an "ERR " prefix would report this stored value as a
    // server failure.
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    client.command("SET report \"ERR disk full\"");

    const Reply reply = client.command("GET report");
    CHECK(reply.delivered);
    CHECK(!reply.isError());                 // structurally a value
    CHECK(reply.type == ReplyType::Value);
    CHECK_EQ(reply.payload, std::string("ERR disk full"));
}

VCACHE_TEST(RepliesAreParsedWithoutKnowingTheCommand) {
    // Self-describing replies mean the client needs no memory of what it sent.
    // KEYS in any casing is still recognised as an array purely from the tag.
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());
    client.command("SET a 1");

    for (const std::string& command : {std::string("keys"), std::string("  KEYS"),
                                       std::string("Keys")}) {
        const Reply reply = client.command(command);
        CHECK(reply.isArray());
        CHECK_EQ(reply.elements.size(), std::size_t{1});
    }
}

// -------------------------------------------------------------- data shapes ----

VCACHE_TEST(BinaryAndQuotedValuesSurvive) {
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    CHECK_EQ(client.command("SET greeting \"hello world\"").payload, std::string("OK"));
    CHECK_EQ(client.command("GET greeting").payload, std::string("hello world"));

    CHECK_EQ(client.command("SET blob \"\\x00\\xff\"").payload, std::string("OK"));
    CHECK_EQ(client.command("GET blob").payload, std::string("\\x00\\xff"));
}

VCACHE_TEST(LargeValuesArriveWhole) {
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    const std::string large(256 * 1024, 'x');
    CHECK_EQ(client.command("SET big " + large).payload, std::string("OK"));

    const Reply reply = client.command("GET big");
    CHECK_EQ(reply.payload.size(), large.size());
    CHECK(reply.payload == large);
}

VCACHE_TEST(ManyCommandsOnOneConnection) {
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    for (int i = 0; i < 500; ++i) {
        CHECK_EQ(client.command("SET key" + std::to_string(i) + " v").payload, std::string("OK"));
    }
    CHECK_EQ(server.database().size(), std::size_t{500});

    for (int i = 0; i < 500; ++i) {
        CHECK_EQ(client.command("GET key" + std::to_string(i)).payload, std::string("v"));
    }
}

VCACHE_TEST(TtlCommandsWorkThroughTheClient) {
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    CHECK_EQ(client.command("SET session abc EX 1").payload, std::string("OK"));
    CHECK_EQ(client.command("GET session").payload, std::string("abc"));

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK(client.command("GET session").isNil());
}

// ------------------------------------------------------------- disconnection ----

VCACHE_TEST(AServerGoingAwayIsReportedNotHidden) {
    VCacheClient client;
    {
        RunningServer server;
        connectOrFail(client, server.port());
        CHECK_EQ(client.command("SET k v").payload, std::string("OK"));
        // Server shuts down here.
    }

    const Reply reply = client.command("GET k");
    CHECK(!reply.delivered);
    CHECK(!reply.transportError.empty());
    CHECK(!client.connected());  // the client tidied up after itself
}

VCACHE_TEST(DisconnectThenReconnectWorks) {
    RunningServer server;
    VCacheClient client;

    connectOrFail(client, server.port());
    client.command("SET persistent yes");
    client.disconnect();
    CHECK(!client.connected());

    connectOrFail(client, server.port());
    CHECK_EQ(client.command("GET persistent").payload, std::string("yes"));
}

VCACHE_TEST(ExplicitDisconnectIsIdempotent) {
    RunningServer server;
    VCacheClient client;
    connectOrFail(client, server.port());

    client.disconnect();
    client.disconnect();
    CHECK(!client.connected());
}

int main() {
    return testing::runAll();
}
