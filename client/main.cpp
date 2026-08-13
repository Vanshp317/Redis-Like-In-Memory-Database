// vcache-cli -- the interactive client from Discovery Document section 19.
//
//     $ vcache-cli
//     vcache> SET username Vansh
//     OK
//     vcache> GET username
//     Vansh
//
// Also usable non-interactively, which is what makes it scriptable:
//
//     $ vcache-cli GET username          # one shot, exits with the result
//     $ echo "KEYS" | vcache-cli         # piped, no prompt

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "VCacheClient.h"

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::vector<std::string> oneShot;  // non-empty means "run this and exit"
    bool showHelp = false;
};

void printUsage() {
    std::cout << "Usage: vcache-cli [options] [command ...]\n"
                 "\n"
                 "  --host <addr>    Server address (default 127.0.0.1)\n"
                 "  --port <n>       Server port (default 6379)\n"
                 "  --help           Show this message\n"
                 "\n"
                 "With no command, starts an interactive prompt.\n"
                 "With a command, runs it once and exits: vcache-cli GET username\n"
                 "Commands are also read from a pipe: echo KEYS | vcache-cli\n";
}

void printCommands() {
    std::cout << "  SET key value [EX seconds]   store a value, optionally expiring\n"
                 "  GET key                      fetch a value, or (nil)\n"
                 "  DEL key                      delete a key, replies 1 or 0\n"
                 "  EXISTS key                   replies 1 or 0\n"
                 "  KEYS                         list every key\n"
                 "  SAVE                         write a snapshot to disk\n"
                 "\n"
                 "  help                         this list\n"
                 "  exit, quit                   leave (Ctrl+D also works)\n"
                 "\n"
                 "Values with spaces need quotes: SET greeting \"hello world\"\n";
}

bool parseArguments(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "--help" || argument == "-h") {
            options.showHelp = true;
            return true;
        }

        if (argument == "--host" || argument == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << argument << " requires a value\n";
                return false;
            }
            const std::string value = argv[++i];

            if (argument == "--host") {
                options.host = value;
                continue;
            }

            try {
                std::size_t consumed = 0;
                const int port = std::stoi(value, &consumed);
                if (consumed != value.size() || port < 1 || port > 65535) {
                    throw std::invalid_argument("out of range");
                }
                options.port = static_cast<std::uint16_t>(port);
            } catch (const std::exception&) {
                std::cerr << "error: invalid port '" << value << "'\n";
                return false;
            }
            continue;
        }

        // The first non-option argument begins a one-shot command; everything
        // after it belongs to that command rather than to the CLI.
        for (int rest = i; rest < argc; ++rest) {
            options.oneShot.emplace_back(argv[rest]);
        }
        return true;
    }
    return true;
}

// Re-joins one-shot arguments into a command line. Arguments that contain
// spaces are re-quoted, so `vcache-cli SET greeting "hello world"` survives the
// shell having already removed the quotes.
std::string joinOneShot(const std::vector<std::string>& parts) {
    std::string line;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            line += ' ';
        }
        const bool needsQuotes = parts[i].find(' ') != std::string::npos ||
                                 parts[i].find('\t') != std::string::npos || parts[i].empty();
        if (!needsQuotes) {
            line += parts[i];
            continue;
        }

        line += '"';
        for (const char c : parts[i]) {
            if (c == '"' || c == '\\') {
                line += '\\';
            }
            line += c;
        }
        line += '"';
    }
    return line;
}

// Renders the tagged wire form back into what a human wants to read -- the
// transcript from Discovery Document section 19. The tags exist so machines can
// parse without ambiguity; nobody has to look at them.
void printReply(const vcache::Reply& reply) {
    using vcache::protocol::ReplyType;

    switch (reply.type) {
        case ReplyType::Status:
        case ReplyType::Integer:
            std::cout << reply.payload << "\n";
            break;

        case ReplyType::Value:
            std::cout << reply.payload << "\n";
            break;

        case ReplyType::Nil:
            std::cout << "(nil)\n";
            break;

        case ReplyType::Error:
            // To stderr, so `vcache-cli GET k > file` never writes an error
            // message into the file as though it were data.
            std::cerr << reply.payload << "\n";
            break;

        case ReplyType::Array: {
            if (reply.elements.empty()) {
                std::cout << "(empty list)\n";
                break;
            }
            std::cout << "(" << reply.elements.size()
                      << (reply.elements.size() == 1 ? " key)" : " keys)") << "\n";
            for (std::size_t i = 0; i < reply.elements.size(); ++i) {
                std::cout << (i + 1) << ") \"" << reply.elements[i] << "\"\n";
            }
            break;
        }

        case ReplyType::Unknown:
            std::cerr << "(unrecognised reply)\n";
            break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseArguments(argc, argv, options)) {
        return 2;
    }
    if (options.showHelp) {
        printUsage();
        return 0;
    }

    vcache::VCacheClient client;
    std::string error;
    if (!client.connect(options.host, options.port, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    // One-shot: run the command, print the reply, and let the exit code carry
    // whether the server was happy, so shell scripts can branch on it.
    if (!options.oneShot.empty()) {
        const vcache::Reply reply = client.command(joinOneShot(options.oneShot));
        if (!reply.delivered) {
            std::cerr << "error: " << reply.transportError << "\n";
            return 1;
        }
        printReply(reply);
        return reply.isError() ? 1 : 0;
    }

    // A prompt is only printed when a human is watching. Piped input gets clean
    // output with no prompts interleaved into it.
    const bool interactive = ::isatty(STDIN_FILENO) != 0;
    if (interactive) {
        std::cout << "Connected to " << options.host << ":" << options.port << "\n"
                  << "Type 'help' for commands, 'exit' to quit.\n";
    }

    std::string line;
    while (true) {
        if (interactive) {
            std::cout << "vcache> " << std::flush;
        }

        if (!std::getline(std::cin, line)) {
            if (interactive) {
                std::cout << "\n";  // finish the line Ctrl+D was typed on
            }
            break;
        }

        // Local commands never reach the server.
        if (line == "exit" || line == "quit") {
            break;
        }
        if (line == "help") {
            printCommands();
            continue;
        }
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;  // blank line: nothing to send
        }

        const vcache::Reply reply = client.command(line);
        if (!reply.delivered) {
            std::cerr << "error: " << reply.transportError << "\n";
            return 1;
        }
        printReply(reply);
    }

    return 0;
}
