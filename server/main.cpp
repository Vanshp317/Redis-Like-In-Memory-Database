// vcache-server -- the VCache daemon.
//
//     ./vcache-server --port 6379
//
// Talk to it with any line-oriented TCP client:
//
//     nc 127.0.0.1 6379
//     SET name Vansh
//     OK
//     GET name
//     Vansh

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "Database.h"
#include "Server.h"

namespace {

// A signal handler may only touch async-signal-safe things. Server::stop() does
// exactly two: it stores to an atomic and writes one byte to a pipe. Doing the
// real shutdown work on the main thread keeps the handler honest.
vcache::Server* g_server = nullptr;

extern "C" void handleSignal(int) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

// Parses "100MB", "512kb", "1G", "1048576". Suffixes are 1024-based and
// case-insensitive; a bare number is bytes. Returns false on anything else,
// because a mistyped memory limit that silently becomes a different number is
// worse than a startup failure.
bool parseByteSize(const std::string& text, std::size_t& bytes) {
    if (text.empty()) {
        return false;
    }

    std::size_t digitEnd = 0;
    while (digitEnd < text.size() && text[digitEnd] >= '0' && text[digitEnd] <= '9') {
        ++digitEnd;
    }
    if (digitEnd == 0) {
        return false;  // no leading number at all
    }

    std::string suffix = text.substr(digitEnd);
    for (char& c : suffix) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }

    std::size_t multiplier = 1;
    if (suffix.empty() || suffix == "B") {
        multiplier = 1;
    } else if (suffix == "K" || suffix == "KB") {
        multiplier = 1024;
    } else if (suffix == "M" || suffix == "MB") {
        multiplier = 1024ull * 1024;
    } else if (suffix == "G" || suffix == "GB") {
        multiplier = 1024ull * 1024 * 1024;
    } else {
        return false;
    }

    unsigned long long number = 0;
    try {
        number = std::stoull(text.substr(0, digitEnd));
    } catch (const std::exception&) {
        return false;
    }

    // Overflow check before multiplying, not after -- afterwards is too late.
    if (number > std::numeric_limits<std::size_t>::max() / multiplier) {
        return false;
    }

    bytes = static_cast<std::size_t>(number) * multiplier;
    return true;
}

void printUsage() {
    std::cout << "Usage: vcache-server [options]\n"
                 "\n"
                 "  --port <n>       Port to listen on (default 6379)\n"
                 "  --bind <addr>    Address to bind (default 127.0.0.1)\n"
                 "  --threads <n>    Worker threads, which is also the ceiling on\n"
                 "                   simultaneous clients (default 64)\n"
                 "  --dbfile <path>  Snapshot file. Without it, persistence is off\n"
                 "                   and SAVE reports that it is not configured.\n"
                 "  --save-interval <seconds>\n"
                 "                   Automatic snapshot interval (default 0, meaning\n"
                 "                   manual SAVE only). Requires --dbfile.\n"
                 "  --max-memory <size>\n"
                 "                   Memory ceiling, e.g. 100MB or 512kb or 1048576.\n"
                 "                   Suffixes are 1024-based. Past it, the least\n"
                 "                   recently used keys are evicted. Default: no limit.\n"
                 "  --help           Show this message\n";
}

// Returns false on a bad argument, having already explained the problem.
bool parseArguments(int argc, char** argv, vcache::ServerConfig& config, bool& shouldExit) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "--help" || argument == "-h") {
            printUsage();
            shouldExit = true;
            return true;
        }

        if (argument == "--port" || argument == "--bind" || argument == "--threads" ||
            argument == "--dbfile" || argument == "--save-interval" ||
            argument == "--max-memory") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << argument << " requires a value\n";
                return false;
            }
            const std::string value = argv[++i];

            if (argument == "--bind") {
                config.bindAddress = value;
                continue;
            }
            if (argument == "--dbfile") {
                config.snapshotPath = value;
                continue;
            }
            if (argument == "--max-memory") {
                std::size_t bytes = 0;
                if (!parseByteSize(value, bytes)) {
                    std::cerr << "error: invalid value for --max-memory: '" << value
                              << "' (expected e.g. 100MB, 512kb, or a byte count)\n";
                    return false;
                }
                config.maxMemoryBytes = bytes;
                continue;
            }
            if (argument == "--save-interval") {
                std::size_t consumed = 0;
                long long seconds = 0;
                try {
                    seconds = std::stoll(value, &consumed);
                } catch (const std::exception&) {
                    consumed = 0;
                }
                if (consumed != value.size() || seconds < 0) {
                    std::cerr << "error: invalid value for --save-interval: '" << value << "'\n";
                    return false;
                }
                config.saveInterval = std::chrono::seconds(seconds);
                continue;
            }

            // Deliberately strict about trailing junk: std::stoi alone would
            // accept "6379abc", and a typo silently binding the wrong port is
            // worse than an error.
            const bool isPort = (argument == "--port");
            const int minimum = isPort ? 0 : 1;
            const int maximum = isPort ? 65535 : 4096;

            int number = 0;
            try {
                std::size_t consumed = 0;
                number = std::stoi(value, &consumed);
                if (consumed != value.size()) {
                    throw std::invalid_argument("trailing characters");
                }
            } catch (const std::exception&) {
                std::cerr << "error: invalid value for " << argument << ": '" << value << "'\n";
                return false;
            }

            if (number < minimum || number > maximum) {
                std::cerr << "error: " << argument << " must be between " << minimum << " and "
                          << maximum << "\n";
                return false;
            }

            if (isPort) {
                config.port = static_cast<std::uint16_t>(number);
            } else {
                config.threadCount = static_cast<std::size_t>(number);
            }
            continue;
        }

        std::cerr << "error: unknown option '" << argument << "'\n\n";
        printUsage();
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    vcache::ServerConfig config;
    bool shouldExit = false;
    if (!parseArguments(argc, argv, config, shouldExit)) {
        return 1;
    }
    if (shouldExit) {
        return 0;
    }

    // A save interval without a file to save to is almost certainly a mistake,
    // and silently ignoring it would leave someone believing their data is
    // being written.
    if (config.saveInterval.count() > 0 && config.snapshotPath.empty()) {
        std::cerr << "error: --save-interval requires --dbfile\n";
        return 1;
    }

    vcache::Database database;
    vcache::Server server(database, config);

    if (!server.start()) {
        std::cerr << "error: " << server.error() << "\n";
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::cout << "VCache listening on " << config.bindAddress << ":" << server.port() << "\n"
              << config.threadCount << " worker threads\n";

    if (config.snapshotPath.empty()) {
        std::cout << "Persistence: off (no --dbfile)\n";
    } else {
        std::cout << "Persistence: " << config.snapshotPath;
        if (config.saveInterval.count() > 0) {
            std::cout << ", saving every " << (config.saveInterval.count() / 1000) << "s";
        } else {
            std::cout << ", manual SAVE only";
        }
        std::cout << "\n";
        if (server.restoredEntryCount() > 0) {
            std::cout << "Restored " << server.restoredEntryCount() << " keys from the snapshot\n";
        }
    }

    if (config.maxMemoryBytes == 0) {
        std::cout << "Memory limit: none\n";
    } else {
        std::cout << "Memory limit: " << config.maxMemoryBytes << " bytes (LRU eviction)\n";
    }

    std::cout << "Commands: SET GET DEL EXISTS KEYS SAVE\n"
              << "Press Ctrl+C to stop.\n";

    server.run();

    const std::size_t remaining = database.size();
    std::cout << "\nShutting down. " << remaining << (remaining == 1 ? " key was" : " keys were")
              << " in memory.\n";
    if (config.maxMemoryBytes > 0) {
        std::cout << "Evicted " << database.evictedCount() << " keys, using "
                  << database.memoryUsage() << " of " << config.maxMemoryBytes << " bytes.\n";
    }

    // A final snapshot on a clean exit, so a deliberate restart never loses the
    // work done since the last scheduled save. A crash still falls back to that
    // last save -- which is what the interval is choosing.
    if (vcache::SnapshotStore* store = server.snapshotStore()) {
        const vcache::SaveOutcome outcome = store->save(database);
        if (outcome.ok) {
            std::cout << "Saved " << outcome.entriesWritten << " keys to " << store->path() << "\n";
        } else {
            std::cerr << "error: final snapshot failed: " << outcome.error << "\n";
            g_server = nullptr;
            return 1;
        }
    }

    g_server = nullptr;
    return 0;
}
