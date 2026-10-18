// Centralizes the timing settings and limits used by application modules

#pragma once
#include <cstdint>
#include "sdkconfig.h"

// All application deadlines use monotonic milliseconds, not the network clock
namespace park::settings {
    // Startup stabilization and inactivity policy. Kconfig seconds are converted to the units needed by callers
    // periodic_sleep_us is the exception to the millisecond naming convention: the hardware wake timer takes microseconds
    // Minimum startup grace after a cold boot before inactivity can permit sleep
    inline constexpr uint64_t cold_boot_ms        = 120000;
    // Required inactivity duration before normal PIR-controlled sleep
    inline constexpr uint64_t idle_ms             = CONFIG_APP_PIR_IDLE_SECONDS * 1000ULL;
    // Deep-sleep timer interval in microseconds, PIR can wake the device earlier
    inline constexpr uint64_t periodic_sleep_us   = CONFIG_APP_PIR_SYNC_SECONDS * 1000000ULL;

    // Cloud timing: collect changes, limit one attempt, bound closing, and space retries/batches
    // The closing network budget and cold-boot grace are independent limits even when their values match
    // Collection window opened by unsent movement or reset records
    inline constexpr uint64_t collect_ms          = 10000;
    // Shared limit for DNS/HTTPS attempts, the initial cloud wait for history, and closing Wi-Fi readiness
    inline constexpr uint64_t request_ms          = 10000;
    // Total cloud synchronization budget during sleep preparation
    inline constexpr uint64_t shutdown_network_ms = 120000;
    // Normal retry interval after a temporary cloud failure
    inline constexpr uint64_t retry_ms            = 60000;
    // Delay between batches and before the first retry during closing
    inline constexpr uint64_t batch_gap_ms        = 2000;

    // Limits request size and memory use, reaching this size does not replace the collection deadline
    // Maximum journal records in one cloud batch, status-only requests may contain none
    inline constexpr unsigned max_batch_records   = 8;

    // Camera health: how long without a result before requesting recovery, and how long recovery may take
    // Allowed time without processed frames before requesting camera recovery
    inline constexpr uint64_t inference_stale_ms  = 5000;
    // Time budget for a camera recovery attempt
    inline constexpr uint64_t camera_recovery_ms  = 10000;

    // Persistence cadence: NVS change checks, SD capture checks, and idle journal sampling
    // These are polling/sample intervals, writes still depend on changes and explicit forced checkpoints
    // Polling interval for pending counter persistence in NVS, unchanged state does not force a write
    inline constexpr uint64_t checkpoint_poll_ms  = 10000;
    // Polling interval for microSD capture checks, a write still depends on capture rules
    inline constexpr uint64_t journal_poll_ms     = 2000;
    // Sample interval while totals remain unchanged, changed totals can be captured sooner
    inline constexpr uint64_t idle_sample_ms      = 30000;
}
