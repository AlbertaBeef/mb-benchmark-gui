// Background download of missing model artifacts.
//
// Runs on its own thread so first launch never blocks the window: the GUI polls
// `snapshot()` on its 1 Hz tick and re-scans the catalog as files land, so
// models become selectable while the rest are still downloading.
//
// Fetching shells out to wget (and unzip for the Axelera archives), exactly as
// the vendor scripts do, rather than pulling in libcurl for a first-run task.
//
// No GTK dependency.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Catalog.h"

class Fetcher {
public:
    struct Status {
        bool running = false;
        int done = 0;
        int total = 0;
        std::string current;              // artifact being fetched right now
        std::vector<std::string> errors;  // one line per failed artifact
        bool finished_since_last = false; // an artifact landed — rescan the catalog
    };

    Fetcher() = default;
    ~Fetcher();

    // Download every missing artifact the catalog knows a source for. A no-op
    // when nothing is missing, when MB_BENCH_NO_DOWNLOAD is set, or while a
    // previous run is still going.
    void start(const Catalog& catalog);
    // Ask the worker to stop after the artifact in flight; joins.
    void stop();

    Status snapshot();

private:
    void run(std::vector<Artifact> todo, std::vector<VendorSource> vendors);
    // Returns an error message, or "" on success.
    std::string fetch_one(const Artifact& a, const VendorSource& v);

    std::thread th_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> running_{false};

    std::mutex mu_;
    Status status_;
};
