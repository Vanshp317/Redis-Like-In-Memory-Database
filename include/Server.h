#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "CommandExecutor.h"
#include "Database.h"
#include "ExpirationSweeper.h"
#include "Persistence.h"
#include "SnapshotScheduler.h"
#include "ThreadPool.h"

namespace vcache {

struct ServerConfig {
    // Loopback by default, on purpose. VCache has no authentication yet
    // (Discovery Document section 4 defers it), and an unauthenticated database
    // listening on 0.0.0.0 is how people get their servers wiped. Binding wider
    // has to be a deliberate choice, not the default.
    std::string bindAddress = "127.0.0.1";

    std::uint16_t port = 6379;

    // Pending connections the kernel will queue while the server is busy.
    int backlog = 128;

    // A single request line may not exceed this. Without a cap, a client that
    // never sends a newline would make the server buffer until it runs out of
    // memory -- the parser cannot enforce this because it only ever sees
    // complete lines.
    std::size_t maxRequestBytes = 8u * 1024u * 1024u;

    // Worker threads, and therefore the ceiling on simultaneous clients: a
    // worker stays with one connection until it disconnects.
    //
    // Sized by expected concurrent connections rather than by core count,
    // because workers spend nearly all their time blocked in recv() waiting for
    // the next command, not burning CPU. Lifting this ceiling properly means
    // the event-loop rewrite (kqueue/epoll) listed under future features, where
    // one thread can watch thousands of idle sockets.
    std::size_t threadCount = 64;

    // Connections accepted but not yet picked up by a worker. Past this, new
    // clients are told the server is busy and disconnected -- an honest refusal
    // beats an accepted connection that never gets answered.
    std::size_t maxQueuedConnections = 128;

    // Background reclamation of expired keys. Set interval to zero to run
    // without a sweeper, which tests use to make expiry deterministic.
    SweeperConfig sweeper;

    // Snapshot file. Empty disables persistence entirely: SAVE then reports
    // that it is not configured, and nothing is ever written. Defaulting to a
    // path would mean a plain `vcache-server` silently creates files in
    // whatever directory it happens to be started from.
    std::string snapshotPath;

    // Interval between automatic snapshots. Zero means manual SAVE only.
    std::chrono::milliseconds saveInterval{0};

    // Load the snapshot at startup. When the file exists but cannot be read or
    // trusted, start() FAILS rather than starting empty -- see Server.cpp.
    bool restoreOnStart = true;

    // Independently locked slices of the keyspace. More shards means less lock
    // contention between concurrent clients; see the note in Database.h.
    std::size_t shardCount = Database::kDefaultShardCount;

    // Memory ceiling in bytes; zero means unlimited and nothing is evicted.
    // Setting it also turns on recency tracking, which costs a little on every
    // GET -- see the eviction note in Database.h.
    std::size_t maxMemoryBytes = 0;
};

// A TCP server speaking the VCache text protocol.
//
// The accept loop runs on the calling thread and does nothing but hand each
// connection to a worker in the pool, so it is never the bottleneck. Each
// worker owns one connection for its whole lifetime.
//
// Lifecycle:
//     Server server(database);
//     if (!server.start()) { ... server.error() ... }   // bind + listen
//     server.run();                                     // blocks until stop()
//
// start() and run() are split so a caller can read port() before the accept
// loop begins -- which is how tests bind to port 0 and let the OS pick a free
// port instead of guessing one.
//
// stop() is safe to call from another thread or from a signal handler.
class Server {
public:
    explicit Server(Database& database, ServerConfig config = ServerConfig{});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Creates the socket, binds and listens. False on failure; error() says why.
    bool start();

    // Accepts connections until stop() is called, then tears down every live
    // connection and joins the pool. Blocks. One-shot: the pool is shut down on
    // the way out, so a Server cannot be run twice.
    void run();

    // Wakes run() out of its wait and asks it to return.
    //
    // Async-signal-safe, so a SIGINT handler may call it: it does nothing but
    // store an atomic and write one byte to a pipe. All the shutdown work that
    // needs a mutex -- which a signal handler must never take, since the
    // interrupted thread may already hold it -- happens back on run()'s thread.
    void stop();

    // The port actually bound. Differs from config.port when that was 0.
    std::uint16_t port() const noexcept { return boundPort_; }

    const std::string& error() const noexcept { return error_; }

    // Null when persistence is not configured. Exposed so the process can take
    // a final snapshot on a clean shutdown.
    SnapshotStore* snapshotStore() const noexcept { return store_.get(); }

    // Entries restored at startup; zero when there was no snapshot.
    std::size_t restoredEntryCount() const noexcept { return restoredEntryCount_; }

private:
    // Runs on a worker thread: registers the connection, serves it, then
    // unregisters and closes it.
    void serveConnection(int clientFd);
    void handleConnection(int clientFd);

    // Interrupts every live connection so its worker's blocked recv() returns.
    // The worker still owns the descriptor and does the actual close.
    void shutdownActiveConnections();

    bool sendLine(int clientFd, const std::string& line);
    static bool sendAll(int fd, const char* data, std::size_t length);
    void closeSockets();

    Database& database_;
    ServerConfig config_;

    // Created in start() rather than in the constructor, so a Server that never
    // starts -- a failed bind, say -- never spawns threads.
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<ExpirationSweeper> sweeper_;
    std::unique_ptr<SnapshotStore> store_;
    std::unique_ptr<SnapshotScheduler> scheduler_;

    int listenFd_ = -1;

    // A self-pipe. run() waits on the listening socket and this pipe at the
    // same time; stop() writes one byte into it, which wakes the wait
    // immediately. The alternative -- closing the listening socket from another
    // thread while run() is blocked inside accept() -- races against file
    // descriptor reuse, and can leave the loop accepting on a descriptor that
    // now belongs to something else entirely.
    int shutdownPipe_[2] = {-1, -1};

    // Descriptors of connections currently owned by a worker. Needed so
    // shutdown can interrupt clients that are connected but silent, which are
    // otherwise parked in recv() indefinitely.
    //
    // A descriptor is only ever closed while this mutex is held, and shutdown
    // only ever touches descriptors while holding it too. That is what keeps
    // the pair safe: without it, a descriptor could be closed and immediately
    // reused for something else between being read out of the set and being
    // shut down.
    std::mutex connectionsMutex_;
    std::unordered_set<int> activeConnections_;

    std::atomic<bool> running_{false};
    std::uint16_t boundPort_ = 0;
    std::size_t restoredEntryCount_ = 0;
    std::string error_;
};

}  // namespace vcache
