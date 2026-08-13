#include "Server.h"

#include "Protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace vcache {

namespace {

// Bytes pulled from the kernel per recv(). 64 KiB is large enough that a burst
// of pipelined commands usually arrives in one syscall, and small enough to not
// matter per connection.
constexpr std::size_t kReadChunkSize = 64 * 1024;

std::string describeErrno(const char* what) {
    return std::string(what) + " failed: " + std::strerror(errno);
}

// Some send()/recv() failures mean "try again", not "give up".
bool isRetryable(int err) noexcept {
    return err == EINTR;
}

}  // namespace

Server::Server(Database& database, ServerConfig config)
    : database_(database), config_(std::move(config)) {}

Server::~Server() {
    closeSockets();
}

void Server::closeSockets() {
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    for (int& fd : shutdownPipe_) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

bool Server::start() {
    // Applied before anything is restored, so a snapshot larger than the limit
    // is trimmed on load rather than blowing straight past it.
    database_.setMaxMemory(config_.maxMemoryBytes);

    // Persistence is set up before the socket is opened. Restoring first means
    // no client can connect and read an empty keyspace that is about to be
    // replaced by the snapshot a moment later.
    if (!config_.snapshotPath.empty()) {
        store_ = std::make_unique<SnapshotStore>(config_.snapshotPath);

        if (config_.restoreOnStart) {
            const LoadOutcome outcome = store_->load(database_);

            if (outcome.status == LoadStatus::Failed) {
                // Refusing to start is the safe answer, and the difference
                // between an incident and a disaster. Starting with an empty
                // keyspace would look like it worked, and then the first
                // scheduled snapshot would overwrite the damaged file with an
                // empty one -- turning recoverable corruption into permanent
                // data loss.
                error_ = "cannot load snapshot '" + config_.snapshotPath + "': " + outcome.error;
                store_.reset();
                return false;
            }

            restoredEntryCount_ = outcome.entriesLoaded;
        }
    }

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        error_ = describeErrno("socket");
        return false;
    }

    // Without SO_REUSEADDR, restarting the server fails with "Address already
    // in use" for a minute or two: the kernel keeps the old socket in TIME_WAIT
    // to catch stray packets from the previous connection. During development
    // that turns every restart into a wait.
    const int enable = 1;
    if (::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        error_ = describeErrno("setsockopt(SO_REUSEADDR)");
        closeSockets();
        return false;
    }

#ifdef SO_NOSIGPIPE
    // macOS/BSD: writing to a socket the peer already closed raises SIGPIPE,
    // whose default action is to kill the process. A client hitting Ctrl+C
    // must not be able to take the database down with it. (Linux uses
    // MSG_NOSIGNAL on each send instead -- see sendAll.)
    ::setsockopt(listenFd_, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);  // host -> network byte order
    if (::inet_pton(AF_INET, config_.bindAddress.c_str(), &address.sin_addr) != 1) {
        error_ = "invalid bind address: " + config_.bindAddress;
        closeSockets();
        return false;
    }

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        error_ = describeErrno("bind");
        closeSockets();
        return false;
    }

    if (::listen(listenFd_, config_.backlog) < 0) {
        error_ = describeErrno("listen");
        closeSockets();
        return false;
    }

    // Ask the kernel which port it actually gave us. Matters when config.port
    // was 0, meaning "pick any free port".
    sockaddr_in bound{};
    socklen_t boundLength = sizeof(bound);
    if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &boundLength) < 0) {
        error_ = describeErrno("getsockname");
        closeSockets();
        return false;
    }
    boundPort_ = ntohs(bound.sin_port);

    if (::pipe(shutdownPipe_) < 0) {
        error_ = describeErrno("pipe");
        closeSockets();
        return false;
    }

    pool_ = std::make_unique<ThreadPool>(config_.threadCount, config_.maxQueuedConnections);

    // A zero interval means "no sweeper". Expiry still works -- reads hide
    // expired keys regardless -- but nothing reclaims their memory, which is
    // what the expiry tests want so they can drive removeExpired() by hand.
    if (config_.sweeper.interval.count() > 0) {
        sweeper_ = std::make_unique<ExpirationSweeper>(database_, config_.sweeper);
        sweeper_->start();
    }

    if (store_ && config_.saveInterval.count() > 0) {
        scheduler_ = std::make_unique<SnapshotScheduler>(*store_, database_, config_.saveInterval);
        scheduler_->start();
    }

    return true;
}

void Server::run() {
    running_ = true;

    while (running_) {
        pollfd watched[2]{};
        watched[0].fd = listenFd_;
        watched[0].events = POLLIN;
        watched[1].fd = shutdownPipe_[0];
        watched[1].events = POLLIN;

        const int ready = ::poll(watched, 2, -1);  // -1: wait indefinitely
        if (ready < 0) {
            if (isRetryable(errno)) {
                continue;
            }
            break;
        }

        if ((watched[1].revents & POLLIN) != 0) {
            break;  // stop() fired
        }
        if ((watched[0].revents & POLLIN) == 0) {
            continue;
        }

        const int clientFd = ::accept(listenFd_, nullptr, nullptr);
        if (clientFd < 0) {
            // A failed accept must never kill the server. ECONNABORTED means
            // the client gave up before we got to it; EMFILE/ENFILE mean the
            // process or system is out of descriptors, which is transient.
            if (errno == EINTR || errno == ECONNABORTED || errno == EMFILE ||
                errno == ENFILE) {
                continue;
            }
            break;
        }

        // Hand off and immediately go back to accepting. The loop never blocks
        // on a client, which is the whole point of the pool.
        if (!pool_->trySubmit([this, clientFd] { serveConnection(clientFd); })) {
            // Every worker is busy and the queue is full. Saying so and closing
            // is kinder than accepting a connection nothing will ever answer.
            sendLine(clientFd, protocol::error("ERR server is busy"));
            ::close(clientFd);
        }
    }

    running_ = false;

    // Interrupt anyone parked in recv(), then wait for every worker to finish.
    // Both steps need a mutex, which is exactly why they live here rather than
    // in stop(): stop() may be called from a signal handler.
    shutdownActiveConnections();
    pool_->shutdown();

    // Both stopped after the workers, so no request can arrive expecting a
    // background thread that has already gone.
    if (sweeper_) {
        sweeper_->stop();
    }
    if (scheduler_) {
        scheduler_->stop();
    }
}

void Server::stop() {
    running_ = false;

    if (shutdownPipe_[1] >= 0) {
        const char byte = 'x';
        const ssize_t written = ::write(shutdownPipe_[1], &byte, 1);
        (void)written;  // nothing useful to do if the wakeup byte cannot be sent
    }
}

void Server::shutdownActiveConnections() {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (const int clientFd : activeConnections_) {
        // shutdown(), not close(): the owning worker still holds this
        // descriptor and will close it itself. Closing here would let the
        // number be reused while that worker is still using it.
        ::shutdown(clientFd, SHUT_RDWR);
    }
}

void Server::serveConnection(int clientFd) {
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        activeConnections_.insert(clientFd);
    }

    // Checked after registering, so a connection queued just as shutdown began
    // is closed rather than served. Whichever order the two interleave in, the
    // descriptor ends up closed exactly once.
    if (running_) {
        handleConnection(clientFd);
    }

    std::lock_guard<std::mutex> lock(connectionsMutex_);
    activeConnections_.erase(clientFd);
    ::close(clientFd);  // closed under the lock -- see shutdownActiveConnections
}

void Server::handleConnection(int clientFd) {
#ifdef SO_NOSIGPIPE
    const int enable = 1;
    ::setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
#endif

    // One executor per connection. It holds nothing but two pointers, so this
    // costs nothing, and it means no state is shared between workers beyond the
    // Database and the SnapshotStore -- both of which do their own locking.
    CommandExecutor executor(database_, store_.get());

    // TCP is a byte stream, not a message stream. One send() by the client can
    // arrive as three recv()s here, and three sends can arrive as one. So bytes
    // accumulate in `pending` and commands are cut out of it at newlines --
    // never assuming that one read equals one command. Getting this wrong is
    // the classic socket bug: it works on localhost and falls apart over a real
    // network, where packets actually fragment.
    std::string pending;
    std::vector<char> buffer(kReadChunkSize);

    while (running_) {
        const ssize_t received = ::recv(clientFd, buffer.data(), buffer.size(), 0);

        if (received < 0) {
            if (isRetryable(errno)) {
                continue;
            }
            break;  // connection is broken; drop it and go back to accepting
        }
        if (received == 0) {
            break;  // orderly disconnect
        }

        pending.append(buffer.data(), static_cast<std::size_t>(received));

        std::size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);

            const std::optional<std::string> response = executor.execute(line);
            if (response.has_value() && !sendLine(clientFd, *response)) {
                return;  // client vanished mid-reply
            }
        }

        // Whatever is left has no newline yet, so it is an unfinished command.
        // Past the cap, it is either a broken client or an attempt to exhaust
        // memory. There is no safe way to resynchronise inside a partial line,
        // so the connection goes.
        if (pending.size() > config_.maxRequestBytes) {
            sendLine(clientFd, protocol::error("ERR request too large"));
            break;
        }
    }
}

bool Server::sendLine(int clientFd, const std::string& line) {
    // Built as one buffer so the reply leaves in a single write. Two writes
    // would let Nagle's algorithm hold the newline back, adding latency to
    // every single command.
    std::string framed;
    framed.reserve(line.size() + 1);
    framed += line;
    framed.push_back('\n');

    return sendAll(clientFd, framed.data(), framed.size());
}

bool Server::sendAll(int fd, const char* data, std::size_t length) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;  // Linux equivalent of SO_NOSIGPIPE
#endif

    std::size_t sent = 0;
    while (sent < length) {
        // send() is allowed to accept fewer bytes than offered, so the return
        // value has to drive a loop. Treating it as all-or-nothing silently
        // truncates large replies.
        const ssize_t written = ::send(fd, data + sent, length - sent, flags);
        if (written < 0) {
            if (isRetryable(errno)) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

}  // namespace vcache
