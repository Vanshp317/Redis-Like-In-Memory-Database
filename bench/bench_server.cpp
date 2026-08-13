// End-to-end TCP benchmarks: throughput and latency percentiles against a real
// running server over real sockets.
//
// METHOD AND ITS LIMITS, stated up front because they bound what the numbers
// mean:
//
//   * The load generator and the server run on the SAME MACHINE and share the
//     same 11 cores. At high client counts they compete for CPU, so these are
//     numbers for the pair, not for the server alone. A separate load machine
//     over a real network would give a different -- and for the server, better
//     -- picture.
//   * VCache uses one worker thread per connection, so the server is always
//     configured with at least as many workers as clients. That is an
//     architectural constraint, not a tuning choice: with fewer workers than
//     clients, the surplus clients simply wait.
//   * Latency here is round-trip: write a command, wait, read the reply. At one
//     client that measures the request path; at 1000 it mostly measures
//     queueing.
//   * The total operation budget is fixed, so each client does fewer operations
//     as the client count rises. That keeps every row's wall-clock comparable.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Database.h"
#include "Server.h"
#include "benchmark.h"

namespace {

constexpr int kTotalOps = 200000;

// A blocking client that speaks one command at a time.
class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("socket() failed");
        }

        // Nagle's algorithm would hold small requests back waiting for more
        // data, adding milliseconds to every round trip and measuring the
        // kernel's buffering policy rather than the server.
        const int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            ::close(fd_);
            throw std::runtime_error(std::string("connect() failed: ") + std::strerror(errno));
        }
    }

    ~Client() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void send(const std::string& text) {
        std::size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t written = ::send(fd_, text.data() + sent, text.size() - sent, 0);
            if (written <= 0) {
                throw std::runtime_error("send() failed");
            }
            sent += static_cast<std::size_t>(written);
        }
    }

    std::string readLine() {
        while (true) {
            const std::size_t newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                return line;
            }
            char chunk[16384];
            const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                throw std::runtime_error("connection closed mid-reply");
            }
            buffer_.append(chunk, static_cast<std::size_t>(received));
        }
    }

private:
    int fd_ = -1;
    std::string buffer_;
};

// Owns a server on an ephemeral port for the lifetime of the object.
class RunningServer {
public:
    explicit RunningServer(std::size_t workers) {
        vcache::ServerConfig config;
        config.port = 0;
        config.threadCount = workers;
        config.maxQueuedConnections = 4096;
        config.sweeper.interval = std::chrono::milliseconds(0);  // no background noise

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

    std::uint16_t port() const { return server_->port(); }
    vcache::Database& database() { return database_; }

private:
    vcache::Database database_;
    std::unique_ptr<vcache::Server> server_;
    std::thread thread_;
};

struct RunResult {
    double opsPerSecond = 0;
    bench::Latency latency;
};

// `commandFor` builds the request for operation `i` on client `c`.
template <typename CommandFor>
RunResult runWorkload(int clientCount, const CommandFor& commandFor) {
    RunningServer server(static_cast<std::size_t>(clientCount) + 4);

    const int opsPerClient = std::max(1, kTotalOps / clientCount);

    std::vector<std::vector<double>> perClientLatencies(static_cast<std::size_t>(clientCount));
    std::atomic<int> failures{0};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    const auto worker = [&](int clientIndex) {
        try {
            Client client(static_cast<std::uint16_t>(server.port()));

            std::vector<double>& latencies = perClientLatencies[static_cast<std::size_t>(clientIndex)];
            latencies.reserve(static_cast<std::size_t>(opsPerClient));

            // Warm the connection so the first timed op is not paying for TCP
            // slow start and a cold path through the server.
            client.send("SET warmup 1\n");
            client.readLine();

            // Every client waits at the line, so the measured window is one of
            // full concurrency rather than a staggered ramp-up.
            ready.fetch_add(1);
            while (!go.load()) {
                std::this_thread::yield();
            }

            for (int i = 0; i < opsPerClient; ++i) {
                const std::string command = commandFor(clientIndex, i);

                const auto start = bench::Clock::now();
                client.send(command);
                const std::string reply = client.readLine();
                latencies.push_back(bench::secondsSince(start) * 1e6);  // microseconds

                bench::doNotOptimize(reply);
            }
        } catch (const std::exception&) {
            failures.fetch_add(1);
        }
    };

    std::vector<std::thread> clients;
    clients.reserve(static_cast<std::size_t>(clientCount));
    for (int c = 0; c < clientCount; ++c) {
        clients.emplace_back(worker, c);
    }

    while (ready.load() < clientCount && failures.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto start = bench::Clock::now();
    go.store(true);
    for (std::thread& client : clients) {
        client.join();
    }
    const double elapsed = bench::secondsSince(start);

    if (failures.load() > 0) {
        std::cerr << "  (" << failures.load() << " clients failed)\n";
    }

    std::vector<double> allLatencies;
    allLatencies.reserve(static_cast<std::size_t>(clientCount) * static_cast<std::size_t>(opsPerClient));
    for (const auto& perClient : perClientLatencies) {
        allLatencies.insert(allLatencies.end(), perClient.begin(), perClient.end());
    }

    RunResult result;
    result.opsPerSecond = static_cast<double>(allLatencies.size()) / elapsed;
    result.latency = bench::summarise(allLatencies);
    return result;
}

// Pipelined: send a batch of commands, then read the batch of replies. Removes
// the network round trip from the inner loop, so what is left is closer to the
// server's own capacity.
double pipelinedThroughput(int clientCount, int batchSize) {
    RunningServer server(static_cast<std::size_t>(clientCount) + 4);

    const int opsPerClient = std::max(batchSize, kTotalOps / clientCount);
    const int batches = opsPerClient / batchSize;

    std::atomic<int> failures{0};

    const auto worker = [&](int clientIndex) {
        try {
            Client client(static_cast<std::uint16_t>(server.port()));
            for (int b = 0; b < batches; ++b) {
                std::string request;
                for (int i = 0; i < batchSize; ++i) {
                    request += "SET c" + std::to_string(clientIndex) + ":" +
                               std::to_string(b * batchSize + i) + " value\n";
                }
                client.send(request);
                for (int i = 0; i < batchSize; ++i) {
                    bench::doNotOptimize(client.readLine());
                }
            }
        } catch (const std::exception&) {
            failures.fetch_add(1);
        }
    };

    const auto start = bench::Clock::now();
    std::vector<std::thread> clients;
    for (int c = 0; c < clientCount; ++c) {
        clients.emplace_back(worker, c);
    }
    for (std::thread& client : clients) {
        client.join();
    }
    const double elapsed = bench::secondsSince(start);

    return static_cast<double>(clientCount) * batches * batchSize / elapsed;
}

}  // namespace

int main() {
    std::cout << "VCache server benchmarks (real TCP over loopback)\n";
    std::cout << "Apple M3 Pro (11 cores), Apple clang 17, -O2, Release\n";
    std::cout << "Load generator and server share this machine. Latencies in microseconds.\n";
    std::cout << "Fixed budget of " << kTotalOps << " operations, split across clients.\n";

    const int clientCounts[] = {1, 10, 100, 1000};

    bench::printHeading("SET throughput and latency by client count");
    bench::printLatencyHeader();
    for (const int clients : clientCounts) {
        const RunResult result = runWorkload(clients, [](int c, int i) {
            return "SET bench:" + std::to_string(c) + ":" + std::to_string(i) + " value\n";
        });
        bench::printLatency(std::to_string(clients) + " client" + (clients == 1 ? "" : "s"),
                            result.opsPerSecond, result.latency);
    }

    bench::printHeading("GET throughput and latency by client count");
    bench::printLatencyHeader();
    for (const int clients : clientCounts) {
        const RunResult result = runWorkload(clients, [](int, int) {
            return std::string("GET warmup\n");  // always a hit
        });
        bench::printLatency(std::to_string(clients) + " client" + (clients == 1 ? "" : "s"),
                            result.opsPerSecond, result.latency);
    }

    bench::printHeading("Pipelined SET throughput (round trip removed)");
    bench::printThroughputHeader();
    for (const int clients : {1, 10, 50}) {
        const double ops = pipelinedThroughput(clients, 100);
        bench::printThroughput(std::to_string(clients) + " client" + (clients == 1 ? "" : "s") +
                                   ", batches of 100",
                               ops);
    }
    bench::printNote("");
    bench::printNote("The gap between these and the un-pipelined rows above is the cost of");
    bench::printNote("one network round trip per command, not of the database.");

    return 0;
}
