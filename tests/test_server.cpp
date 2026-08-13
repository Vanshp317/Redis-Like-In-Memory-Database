// Phase 4b test suite -- the socket layer, exercised over real TCP.
//
// Each test starts a server on port 0 (the OS picks a free port), runs it on a
// background thread, and talks to it through an ordinary client socket. Nothing
// is mocked: these are genuine connections over the loopback interface.
//
// What is checked here is byte plumbing only -- framing, partial reads,
// pipelining, disconnects, limits. What commands *mean* is covered by
// test_command_executor.cpp.

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Database.h"
#include "Persistence.h"
#include "Server.h"
#include "test_framework.h"

namespace {

// Runs a server on its own thread for the lifetime of the object.
class ServerFixture {
public:
    // Eight workers by default rather than the production 64: the tests do not
    // need the ceiling, and spawning 64 threads per test is slow under the
    // sanitizers.
    explicit ServerFixture(std::size_t threadCount = 8) {
        vcache::ServerConfig config;
        config.port = 0;  // "any free port" -- no collisions between tests
        config.threadCount = threadCount;
        server_ = std::make_unique<vcache::Server>(database_, config);

        if (!server_->start()) {
            throw std::runtime_error("server failed to start: " + server_->error());
        }
        thread_ = std::thread([this] { server_->run(); });
    }

    ~ServerFixture() {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ServerFixture(const ServerFixture&) = delete;
    ServerFixture& operator=(const ServerFixture&) = delete;

    std::uint16_t port() const { return server_->port(); }
    vcache::Database& database() { return database_; }

private:
    vcache::Database database_;
    std::unique_ptr<vcache::Server> server_;
    std::thread thread_;
};

// A minimal blocking TCP client.
class TestClient {
public:
    explicit TestClient(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("client socket() failed");
        }

        // Every read is bounded. Without this, a server-side bug would hang the
        // whole test suite instead of failing one test.
        timeval timeout{};
        timeout.tv_sec = 5;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            ::close(fd_);
            throw std::runtime_error(std::string("connect() failed: ") + std::strerror(errno));
        }
    }

    ~TestClient() { close(); }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    void sendRaw(const std::string& bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t written = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
            if (written <= 0) {
                throw std::runtime_error("send() failed");
            }
            sent += static_cast<std::size_t>(written);
        }
    }

    void sendCommand(const std::string& command) { sendRaw(command + "\n"); }

    // Reads up to the next newline. Throws on timeout or disconnect, so a
    // missing reply surfaces as a failed test rather than a hang.
    std::string readLine() {
        while (true) {
            const std::size_t newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                const std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                return line;
            }

            char chunk[4096];
            const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (received == 0) {
                throw std::runtime_error("server closed the connection before replying");
            }
            if (received < 0) {
                throw std::runtime_error(std::string("recv() failed: ") + std::strerror(errno));
            }
            buffer_.append(chunk, static_cast<std::size_t>(received));
        }
    }

    // True if the peer closed the connection (with nothing buffered).
    bool isClosedByPeer() {
        if (!buffer_.empty()) {
            return false;
        }
        char chunk[256];
        const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (received == 0) {
            return true;
        }
        if (received > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(received));
        }
        return false;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    std::string buffer_;
};

}  // namespace

// ----------------------------------------------------------------- startup ----

VCACHE_TEST(ServerBindsAnEphemeralPort) {
    ServerFixture server;
    CHECK(server.port() != 0);
}

VCACHE_TEST(RejectsAnInvalidBindAddress) {
    vcache::Database database;
    vcache::ServerConfig config;
    config.bindAddress = "not-an-address";
    vcache::Server server(database, config);

    CHECK(!server.start());
    CHECK(!server.error().empty());
}

VCACHE_TEST(ReportsAPortAlreadyInUse) {
    // The second bind must fail cleanly with a message, not abort.
    ServerFixture first;

    vcache::Database database;
    vcache::ServerConfig config;
    config.port = first.port();
    vcache::Server second(database, config);

    CHECK(!second.start());
    CHECK(second.error().find("bind") != std::string::npos);
}

// -------------------------------------------------------------- round trip ----

VCACHE_TEST(SetAndGetOverTcp) {
    ServerFixture server;
    TestClient client(server.port());

    client.sendCommand("SET name Vansh");
    CHECK_EQ(client.readLine(), std::string("+OK"));

    client.sendCommand("GET name");
    CHECK_EQ(client.readLine(), std::string("$Vansh"));
}

VCACHE_TEST(ManyCommandsOnOneConnection) {
    ServerFixture server;
    TestClient client(server.port());

    for (int i = 0; i < 200; ++i) {
        client.sendCommand("SET key" + std::to_string(i) + " value" + std::to_string(i));
        CHECK_EQ(client.readLine(), std::string("+OK"));
    }
    for (int i = 0; i < 200; ++i) {
        client.sendCommand("GET key" + std::to_string(i));
        CHECK_EQ(client.readLine(), "$value" + std::to_string(i));
    }

    CHECK_EQ(server.database().size(), std::size_t{200});
}

VCACHE_TEST(ErrorsTravelOverTheWire) {
    ServerFixture server;
    TestClient client(server.port());

    client.sendCommand("BOGUS");
    CHECK_EQ(client.readLine(), std::string("-ERR unknown command 'BOGUS'"));

    // A rejected command must not close the connection.
    client.sendCommand("SET key value");
    CHECK_EQ(client.readLine(), std::string("+OK"));
}

VCACHE_TEST(MultiLineKeysReplyArrivesInFull) {
    ServerFixture server;
    TestClient client(server.port());

    client.sendCommand("SET a 1");
    client.readLine();
    client.sendCommand("SET b 2");
    client.readLine();

    client.sendCommand("KEYS");
    CHECK_EQ(client.readLine(), std::string("*2"));

    // Each element arrives as its own tagged line.
    CHECK(client.readLine().rfind("$", 0) == 0);
    CHECK(client.readLine().rfind("$", 0) == 0);
}

// ------------------------------------------------------- stream reassembly ----

VCACHE_TEST(CommandSplitAcrossPacketsIsReassembled) {
    // The core TCP lesson: the client's writes and the server's reads do not
    // line up. Sending one command as three pieces, with pauses, must still
    // produce exactly one reply.
    ServerFixture server;
    TestClient client(server.port());

    client.sendRaw("SET split");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    client.sendRaw("key hello");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    client.sendRaw("-world\n");

    CHECK_EQ(client.readLine(), std::string("+OK"));

    client.sendCommand("GET splitkey");
    CHECK_EQ(client.readLine(), std::string("$hello-world"));
}

VCACHE_TEST(PipelinedCommandsInOnePacketEachGetAReply) {
    // The mirror image: three commands in a single write must produce three
    // replies, in order. A server that assumed one read equals one command
    // would answer only the first.
    ServerFixture server;
    TestClient client(server.port());

    client.sendRaw("SET a 1\nSET b 2\nGET a\n");

    CHECK_EQ(client.readLine(), std::string("+OK"));
    CHECK_EQ(client.readLine(), std::string("+OK"));
    CHECK_EQ(client.readLine(), std::string("$1"));
}

VCACHE_TEST(TelnetStyleCrLfWorks) {
    ServerFixture server;
    TestClient client(server.port());

    client.sendRaw("SET key value\r\n");
    CHECK_EQ(client.readLine(), std::string("+OK"));

    // The \r must not have been stored as part of the value.
    CHECK_EQ(*server.database().get("key"), std::string("value"));
}

VCACHE_TEST(BlankLinesGetNoReply) {
    ServerFixture server;
    TestClient client(server.port());

    // Three blank lines then one real command: exactly one reply should arrive.
    client.sendRaw("\n\n\nGET missing\n");
    CHECK_EQ(client.readLine(), std::string("_"));

    client.sendCommand("SET after blank");
    CHECK_EQ(client.readLine(), std::string("+OK"));
}

VCACHE_TEST(BinaryValuesSurviveTheRoundTrip) {
    ServerFixture server;
    TestClient client(server.port());

    client.sendCommand("SET blob \"\\x00\\xff\\x0a end\"");
    CHECK_EQ(client.readLine(), std::string("+OK"));

    client.sendCommand("GET blob");
    CHECK_EQ(client.readLine(), std::string("$\\x00\\xff\\n end"));

    // The stored bytes really are binary, not the escape text:
    // 0x00, 0xff, 0x0a, ' ', 'e', 'n', 'd' -- seven bytes.
    const std::string stored = *server.database().get("blob");
    CHECK_EQ(stored.size(), std::size_t{7});
    CHECK_EQ(stored[0], '\0');
    CHECK_EQ(stored[2], '\n');
}

VCACHE_TEST(LargeValueSurvivesFragmentation) {
    // 512 KB forces the kernel to split the request across many reads and the
    // reply across many writes, exercising both accumulation loops.
    ServerFixture server;
    TestClient client(server.port());

    const std::string large(512 * 1024, 'x');
    client.sendCommand("SET big " + large);
    CHECK_EQ(client.readLine(), std::string("+OK"));

    client.sendCommand("GET big");
    const std::string returned = client.readLine().substr(1);  // drop the '$' tag
    CHECK_EQ(returned.size(), large.size());
    CHECK(returned == large);
}

// ---------------------------------------------------- robustness (§17) ----

VCACHE_TEST(AbruptDisconnectDoesNotKillTheServer) {
    ServerFixture server;

    {
        TestClient client(server.port());
        client.sendCommand("SET survived yes");
        CHECK_EQ(client.readLine(), std::string("+OK"));
        // Destructor closes the socket without any goodbye.
    }

    // A brand new client must still be served, with the data intact.
    TestClient next(server.port());
    next.sendCommand("GET survived");
    CHECK_EQ(next.readLine(), std::string("$yes"));
}

VCACHE_TEST(DisconnectMidCommandIsHarmless) {
    // Half a command, then the socket vanishes. The server must not be left
    // waiting on the rest of a line forever.
    ServerFixture server;

    {
        TestClient client(server.port());
        client.sendRaw("SET halfway ");
    }

    TestClient next(server.port());
    next.sendCommand("SET after ok");
    CHECK_EQ(next.readLine(), std::string("+OK"));
    CHECK(!server.database().exists("halfway"));
}

VCACHE_TEST(SequentialClientsShareState) {
    ServerFixture server;

    {
        TestClient first(server.port());
        first.sendCommand("SET shared from-first");
        CHECK_EQ(first.readLine(), std::string("+OK"));
    }
    {
        TestClient second(server.port());
        second.sendCommand("GET shared");
        CHECK_EQ(second.readLine(), std::string("$from-first"));
        second.sendCommand("SET shared from-second");
        CHECK_EQ(second.readLine(), std::string("+OK"));
    }
    {
        TestClient third(server.port());
        third.sendCommand("GET shared");
        CHECK_EQ(third.readLine(), std::string("$from-second"));
    }
}

VCACHE_TEST(OversizedRequestIsRejectedAndTheServerLives) {
    // A client that never sends a newline would otherwise make the server
    // buffer without limit.
    vcache::Database database;
    vcache::ServerConfig config;
    config.port = 0;
    config.maxRequestBytes = 4096;  // small, so the test stays quick

    vcache::Server server(database, config);
    CHECK(server.start());
    std::thread thread([&server] { server.run(); });

    {
        TestClient client(server.port());
        client.sendRaw("SET key " + std::string(20000, 'x'));  // no newline, ever
        CHECK_EQ(client.readLine(), std::string("-ERR request too large"));
    }

    // Still accepting connections afterwards.
    {
        TestClient client(server.port());
        client.sendCommand("SET alive yes");
        CHECK_EQ(client.readLine(), std::string("+OK"));
    }

    server.stop();
    thread.join();
}

VCACHE_TEST(GarbageInputDoesNotCrashTheServer) {
    ServerFixture server;
    TestClient client(server.port());

    // Built byte by byte rather than as a literal with a hand-counted length:
    // getting that count wrong reads past the end of the literal, which is
    // exactly the bug this test had on its first run.
    std::string garbage;
    garbage += '\x01';
    garbage += '\x02';
    garbage += '\0';
    garbage += " nonsense\n";

    client.sendRaw(garbage);
    CHECK(client.readLine().rfind("-ERR", 0) == 0);

    client.sendRaw("\"\"\"\"\"\n");
    CHECK(client.readLine().rfind("-ERR", 0) == 0);

    client.sendCommand("SET still working");
    CHECK_EQ(client.readLine(), std::string("+OK"));
}

VCACHE_TEST(StopEndsTheAcceptLoopPromptly) {
    vcache::Database database;
    vcache::ServerConfig config;
    config.port = 0;

    vcache::Server server(database, config);
    CHECK(server.start());
    std::thread thread([&server] { server.run(); });

    // Give the loop a moment to reach poll(), then interrupt it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();
    thread.join();  // hangs the test on failure, which the CTest timeout catches

    CHECK(true);
}

// -------------------------------------------------- concurrency (Phase 5) ----

VCACHE_TEST(AnIdleClientDoesNotBlockOtherClients) {
    // The exact thing Phase 4 could not do. One client connects and says
    // nothing, parking its worker inside recv() forever; every other client
    // must still be served immediately.
    ServerFixture server;

    TestClient idler(server.port());  // connects, then stays silent
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int i = 0; i < 5; ++i) {
        TestClient active(server.port());
        active.sendCommand("SET key" + std::to_string(i) + " value");
        CHECK_EQ(active.readLine(), std::string("+OK"));
    }

    CHECK_EQ(server.database().size(), std::size_t{5});
}

VCACHE_TEST(ManyClientsAtOnceAllGetServed) {
    constexpr int kClients = 16;
    constexpr int kCommandsEach = 50;

    ServerFixture server(kClients);
    std::atomic<int> failures{0};

    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int c = 0; c < kClients; ++c) {
        clients.emplace_back([&server, &failures, c] {
            try {
                TestClient client(server.port());
                for (int i = 0; i < kCommandsEach; ++i) {
                    const std::string key = "c" + std::to_string(c) + ":" + std::to_string(i);
                    client.sendCommand("SET " + key + " value" + std::to_string(i));
                    if (client.readLine() != "+OK") {
                        failures.fetch_add(1);
                    }
                    client.sendCommand("GET " + key);
                    if (client.readLine() != "$value" + std::to_string(i)) {
                        failures.fetch_add(1);
                    }
                }
            } catch (const std::exception&) {
                failures.fetch_add(1);
            }
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }

    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(server.database().size(), std::size_t{kClients * kCommandsEach});
}

VCACHE_TEST(ConcurrentClientsWritingOneKeyNeverCorruptIt) {
    constexpr int kClients = 8;
    ServerFixture server(kClients);

    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int c = 0; c < kClients; ++c) {
        clients.emplace_back([&server, c] {
            TestClient client(server.port());
            const std::string value(64, static_cast<char>('a' + c));
            for (int i = 0; i < 100; ++i) {
                client.sendCommand("SET shared " + value);
                client.readLine();
                client.sendCommand("GET shared");
                const std::string got = client.readLine().substr(1);  // drop the '$' tag
                // Whatever came back must be one writer's value, whole.
                CHECK_EQ(got.size(), std::size_t{64});
                CHECK(got == std::string(64, got[0]));
            }
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }
}

VCACHE_TEST(SaturatedPoolRefusesRatherThanHangs) {
    // One worker, no queue. The second client should be told the server is busy
    // and disconnected -- not left holding an accepted connection that nothing
    // will ever answer.
    vcache::Database database;
    vcache::ServerConfig config;
    config.port = 0;
    config.threadCount = 1;
    config.maxQueuedConnections = 1;

    vcache::Server server(database, config);
    CHECK(server.start());
    std::thread thread([&server] { server.run(); });

    TestClient occupier(server.port());  // takes the only worker and idles
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TestClient queued(server.port());   // fills the one queue slot
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TestClient rejected(server.port());
    CHECK_EQ(rejected.readLine(), std::string("-ERR server is busy"));

    occupier.close();
    queued.close();
    rejected.close();
    server.stop();
    thread.join();
}

VCACHE_TEST(ShutdownWithManyLiveConnectionsCompletes) {
    // Every worker parked in recv() on a silent client. stop() has to interrupt
    // all of them, or run() would never return.
    vcache::Database database;
    vcache::ServerConfig config;
    config.port = 0;
    config.threadCount = 8;

    vcache::Server server(database, config);
    CHECK(server.start());
    std::thread thread([&server] { server.run(); });

    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 8; ++i) {
        clients.push_back(std::make_unique<TestClient>(server.port()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    thread.join();  // a missed connection would hang here

    CHECK(true);
}

// ---------------------------------------------------------- TTL over TCP ----

VCACHE_TEST(ExpiringKeysWorkEndToEnd) {
    ServerFixture server;
    TestClient client(server.port());

    client.sendCommand("SET session abc123 EX 1");
    CHECK_EQ(client.readLine(), std::string("+OK"));

    client.sendCommand("GET session");
    CHECK_EQ(client.readLine(), std::string("$abc123"));

    std::this_thread::sleep_for(std::chrono::milliseconds(1300));

    client.sendCommand("GET session");
    CHECK_EQ(client.readLine(), std::string("_"));

    client.sendCommand("KEYS");
    CHECK_EQ(client.readLine(), std::string("*0"));
}

VCACHE_TEST(TheSweeperReclaimsWhileTheServerRuns) {
    // The server owns a sweeper thread, so memory comes back without any
    // client ever touching the expired keys again.
    vcache::Database database;
    vcache::ServerConfig config;
    config.port = 0;
    config.threadCount = 4;
    config.sweeper.interval = std::chrono::milliseconds(20);
    config.sweeper.bucketsPerPass = 4096;

    vcache::Server server(database, config);
    CHECK(server.start());
    std::thread thread([&server] { server.run(); });

    {
        TestClient client(server.port());
        for (int i = 0; i < 50; ++i) {
            client.sendCommand("SET key" + std::to_string(i) + " value EX 1");
            CHECK_EQ(client.readLine(), std::string("+OK"));
        }
        CHECK_EQ(database.size(), std::size_t{50});
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK_EQ(database.size(), std::size_t{0});  // reclaimed with no client help

    server.stop();
    thread.join();
}

VCACHE_TEST(InvalidTtlsAreRejectedOverTheWire) {
    ServerFixture server;
    TestClient client(server.port());

    client.sendCommand("SET key value EX 0");
    CHECK_EQ(client.readLine(), std::string("-ERR invalid expire time in 'SET' command"));

    client.sendCommand("SET key value EX nonsense");
    CHECK_EQ(client.readLine(), std::string("-ERR value is not an integer or out of range"));

    // The connection stays usable after a rejected command.
    client.sendCommand("SET key value EX 60");
    CHECK_EQ(client.readLine(), std::string("+OK"));
}

// -------------------------------------------------- persistence (Phase 7) ----

namespace {

// Runs a server on `path` for the duration of a callback, then shuts it down --
// standing in for a process lifetime, so a "restart" is two of these in a row.
void withServer(const std::string& snapshotPath,
                const std::function<void(vcache::Server&, vcache::Database&)>& body,
                bool expectStartSuccess = true) {
    vcache::Database database;
    vcache::ServerConfig config;
    config.port = 0;
    config.threadCount = 4;
    config.snapshotPath = snapshotPath;

    vcache::Server server(database, config);
    const bool started = server.start();
    CHECK_EQ(started, expectStartSuccess);
    if (!started) {
        body(server, database);
        return;
    }

    std::thread thread([&server] { server.run(); });
    body(server, database);
    server.stop();
    thread.join();
}

}  // namespace

VCACHE_TEST(DataSurvivesARestart) {
    // The whole point of the phase: write keys, stop the server, start a new
    // one on the same file, and find the data still there.
    const std::string path = "/tmp/vcache-server-restart.snapshot";
    std::remove(path.c_str());

    withServer(path, [](vcache::Server& server, vcache::Database&) {
        TestClient client(server.port());
        client.sendCommand("SET username Vansh");
        CHECK_EQ(client.readLine(), std::string("+OK"));
        client.sendCommand("SET greeting \"hello world\"");
        CHECK_EQ(client.readLine(), std::string("+OK"));
        client.sendCommand("SAVE");
        CHECK_EQ(client.readLine(), std::string("+OK"));
    });

    withServer(path, [](vcache::Server& server, vcache::Database&) {
        CHECK_EQ(server.restoredEntryCount(), std::size_t{2});

        TestClient client(server.port());
        client.sendCommand("GET username");
        CHECK_EQ(client.readLine(), std::string("$Vansh"));
        client.sendCommand("GET greeting");
        CHECK_EQ(client.readLine(), std::string("$hello world"));
    });

    std::remove(path.c_str());
}

VCACHE_TEST(ATtlSurvivesARestart) {
    const std::string path = "/tmp/vcache-server-ttl.snapshot";
    std::remove(path.c_str());

    withServer(path, [](vcache::Server& server, vcache::Database&) {
        TestClient client(server.port());
        client.sendCommand("SET session abc123 EX 3600");
        CHECK_EQ(client.readLine(), std::string("+OK"));
        client.sendCommand("SAVE");
        CHECK_EQ(client.readLine(), std::string("+OK"));
    });

    withServer(path, [](vcache::Server& server, vcache::Database& database) {
        TestClient client(server.port());
        client.sendCommand("GET session");
        CHECK_EQ(client.readLine(), std::string("$abc123"));

        const auto remaining = database.ttl("session");
        CHECK(remaining.has_value());
        CHECK(*remaining > std::chrono::seconds(3500));  // still counting down
    });

    std::remove(path.c_str());
}

VCACHE_TEST(StartingWithNoSnapshotIsFine) {
    const std::string path = "/tmp/vcache-server-absent.snapshot";
    std::remove(path.c_str());

    withServer(path, [](vcache::Server& server, vcache::Database&) {
        CHECK_EQ(server.restoredEntryCount(), std::size_t{0});
        TestClient client(server.port());
        client.sendCommand("SET key value");
        CHECK_EQ(client.readLine(), std::string("+OK"));
    });

    std::remove(path.c_str());
}

VCACHE_TEST(ACorruptSnapshotStopsTheServerStarting) {
    // Starting empty would look like success, and then the next snapshot would
    // overwrite the damaged file with an empty one -- turning recoverable
    // corruption into permanent loss. Refusing to start forces a human to look.
    const std::string path = "/tmp/vcache-server-corrupt.snapshot";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const std::string garbage = "VCACHE" + std::string(50, '\x7f');
        file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    vcache::Database database;
    vcache::ServerConfig config;
    config.port = 0;
    config.snapshotPath = path;

    vcache::Server server(database, config);
    CHECK(!server.start());
    CHECK(server.error().find("snapshot") != std::string::npos);

    std::remove(path.c_str());
}

VCACHE_TEST(PeriodicSavesHappenWithoutASaveCommand) {
    const std::string path = "/tmp/vcache-server-periodic.snapshot";
    std::remove(path.c_str());

    {
        vcache::Database database;
        vcache::ServerConfig config;
        config.port = 0;
        config.threadCount = 4;
        config.snapshotPath = path;
        config.saveInterval = std::chrono::milliseconds(30);

        vcache::Server server(database, config);
        CHECK(server.start());
        std::thread thread([&server] { server.run(); });

        {
            TestClient client(server.port());
            client.sendCommand("SET automatic yes");
            CHECK_EQ(client.readLine(), std::string("+OK"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        server.stop();
        thread.join();
    }

    // No client ever issued SAVE.
    vcache::SnapshotStore store(path);
    vcache::Database restored;
    CHECK(store.load(restored).status == vcache::LoadStatus::Loaded);
    CHECK_EQ(*restored.get("automatic"), std::string("yes"));

    std::remove(path.c_str());
}

VCACHE_TEST(SaveIsRejectedWhenPersistenceIsOff) {
    ServerFixture server;  // no snapshotPath
    TestClient client(server.port());

    client.sendCommand("SAVE");
    CHECK_EQ(client.readLine(), std::string("-ERR persistence is not configured"));

    client.sendCommand("SET key value");
    CHECK_EQ(client.readLine(), std::string("+OK"));
}

VCACHE_TEST(ConcurrentSaveCommandsAreSerialised) {
    // Several clients calling SAVE at once must not corrupt the file or each
    // other's temporary write.
    const std::string path = "/tmp/vcache-server-concurrent-save.snapshot";
    std::remove(path.c_str());

    withServer(path, [](vcache::Server& server, vcache::Database&) {
        {
            TestClient setup(server.port());
            for (int i = 0; i < 100; ++i) {
                setup.sendCommand("SET key" + std::to_string(i) + " value");
                setup.readLine();
            }
        }

        std::vector<std::thread> savers;
        for (int i = 0; i < 4; ++i) {
            savers.emplace_back([&server] {
                TestClient client(server.port());
                for (int n = 0; n < 5; ++n) {
                    client.sendCommand("SAVE");
                    CHECK_EQ(client.readLine(), std::string("+OK"));
                }
            });
        }
        for (std::thread& saver : savers) {
            saver.join();
        }
    });

    vcache::SnapshotStore store(path);
    vcache::Database restored;
    CHECK(store.load(restored).status == vcache::LoadStatus::Loaded);
    CHECK_EQ(restored.size(), std::size_t{100});

    std::remove(path.c_str());
}

VCACHE_TEST(DefaultConfigurationMatchesTheDocumentedValues) {
    const vcache::ServerConfig config;
    CHECK_EQ(config.port, std::uint16_t{6379});
    CHECK_EQ(config.bindAddress, std::string("127.0.0.1"));
    CHECK_EQ(config.threadCount, std::size_t{64});
}

VCACHE_TEST(StopWakesAnIdleConnectedClient) {
    // A connected but silent client leaves the server blocked in recv().
    // Shutting that socket down from stop() is what makes Ctrl+C work.
    vcache::Database database;
    vcache::ServerConfig config;
    config.port = 0;

    vcache::Server server(database, config);
    CHECK(server.start());
    std::thread thread([&server] { server.run(); });

    TestClient client(server.port());
    client.sendCommand("SET key value");
    CHECK_EQ(client.readLine(), std::string("+OK"));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();
    thread.join();

    CHECK(true);
}

int main() {
    return testing::runAll();
}
