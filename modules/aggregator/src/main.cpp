#include "aggregator/aggregator.h"
#include "aggregator/server.h"
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <csignal>
#include <cstdlib>

// Global pointer for signal handler.
static aggregator::AggregatorServer* g_server = nullptr;

static void on_signal(int) {
    if (g_server) g_server->stop();
}

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --port PORT --db PATH [--peer URL ...] [--sync-interval SECS]\n"
              << "\n"
              << "  --port N           HTTP port to listen on (default: 8080)\n"
              << "  --db PATH          LMDB directory path\n"
              << "  --peer URL         Peer server URL, repeatable (e.g. http://host:8080)\n"
              << "  --sync-interval N  Pull-sync interval in seconds (default: 30)\n"
              << "\nExample:\n"
              << "  aggregator_server --port 8080 --db /data/agg\n"
              << "  aggregator_server --port 8081 --db /data/agg2 \\\n"
              << "      --peer http://localhost:8080 --sync-interval 10\n";
}

int main(int argc, char* argv[]) {
    uint16_t     port          = 8080;
    std::string  db_path;
    std::vector<std::string> peers;
    int          sync_interval = 30;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i+1 < argc)
            port = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if ((arg == "--db") && i+1 < argc)
            db_path = argv[++i];
        else if (arg == "--peer" && i+1 < argc)
            peers.push_back(argv[++i]);
        else if (arg == "--sync-interval" && i+1 < argc)
            sync_interval = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
    }

    if (db_path.empty()) {
        std::cerr << "Error: --db PATH is required\n\n";
        usage(argv[0]);
        return 1;
    }

    try {
        aggregator::AggregatorStorage storage(db_path);
        aggregator::AggregatorServer  server(
            storage, port, peers,
            std::chrono::seconds(sync_interval));

        g_server = &server;
        std::signal(SIGINT,  on_signal);
        std::signal(SIGTERM, on_signal);

        server.run(); // blocking
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
