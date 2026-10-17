#pragma once
#include <cstdint>
#include "sdkconfig.h"

// All application deadlines use monotonic milliseconds, not the network clock.
namespace park::settings {
inline constexpr uint64_t cold_boot_ms = 120000;
inline constexpr uint64_t idle_ms = CONFIG_APP_PIR_IDLE_SECONDS * 1000ULL;
inline constexpr uint64_t periodic_sleep_us = CONFIG_APP_PIR_SYNC_SECONDS * 1000000ULL;
inline constexpr uint64_t collect_ms = 10000;
inline constexpr uint64_t request_ms = 10000;
inline constexpr uint64_t shutdown_network_ms = 120000;
inline constexpr uint64_t retry_ms = 60000;
inline constexpr uint64_t batch_gap_ms = 2000;
inline constexpr unsigned batch_records = 8;
inline constexpr uint64_t inference_stale_ms = 5000;
inline constexpr uint64_t camera_recovery_ms = 10000;
inline constexpr uint64_t checkpoint_ms = 10000;
inline constexpr uint64_t journal_ms = 2000;
inline constexpr uint64_t sample_ms = 30000;
}
