// Stores and queries microSD history and manages records pending synchronization

#include "park_history.hpp"
#include "app_settings.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "visitor_counter.hpp"
#include "mbedtls/sha256.h"
#include <algorithm>
#include <cinttypes>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <atomic>
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
    constexpr const char *HISTORY_DIR = "/sdcard/park-monitor-v1";
    constexpr const char *CURSOR_DIR  = "/sdcard/park-cloud-v1";
    bool cloud_protected              = false;
    constexpr const char *HEADER      =
        "epoch,boot,uptime_ms,total_entries,total_exits,entries,exits,covered_seconds,kind,gap,storage_ok,checksum\n";

    SemaphoreHandle_t history_mutex = nullptr;

    // Card state and baselines are protected by mutex, atomics expose short status reads to other tasks
    bool mounted = false, healthy = false, baseline = false, dated_baseline = false;
    std::atomic<bool> initialized{false}, reported_healthy{false};
    std::atomic<uint32_t> data_revision{0};
    std::string error           = "Verificando microSD";
    uint32_t boot               = 0;
    uint64_t previous_entries   = 0, previous_exits = 0;
    int64_t previous_capture_us = 0;
    time_t last_write           = 0;
    unsigned card_mb            = 0;
    std::string cached_start, cached_end, cached_report;
    int64_t cached_at = 0;

    // One persisted sample: lifetime totals plus deltas since the previous successful write
    // epoch is wall-clock seconds (zero if invalid), uptime is monotonic milliseconds, covered is seconds
    struct Row {
        long long epoch  = 0, uptime = 0;
        unsigned boot    = 0, covered = 0, gap = 0, storage = 0;
        uint64_t total_e = 0, total_x = 0, e = 0, x = 0;
        char kind[16]    = {};
    };

    // Computes a small FNV-1a integrity checksum for journal lines and cursor metadata, this is not authentication
    uint32_t checksum(const char *text) {
        uint32_t hash = 2166136261u;
        for(; *text; ++text) {
            hash ^= static_cast<unsigned char>(*text);
            hash *= 16777619u;
        }
        return hash;
    }

    // Loads a byte offset only if its file binding, checksum, and bounds are valid. Otherwise replay starts at zero
    long cloud_cursor(const std::string &filename, long file_size) {
        FILE *file               = fopen((std::string(CURSOR_DIR) + "/" + filename + ".ack").c_str(), "r");
        long offset              = 0;
        unsigned stored_checksum = 0;

        if(!file)
            return 0;

        const int fields_read = fscanf(file, "%ld,%x", &offset, &stored_checksum);
        fclose(file);
        const auto seal = filename + ":" + std::to_string(offset);

        return fields_read == 2 && offset >= 0 && offset <= file_size && stored_checksum == checksum(seal.c_str()) ? offset:0;
    }

    // Creates one newline-terminated CSV record with a checksum over the preceding fields
    std::string encode(const Row &row) {
        char payload[256], checksum_suffix[16];
        snprintf(payload, sizeof(payload), "%lld,%u,%lld,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%s,%u,%u",
        row.epoch, row.boot, row.uptime, row.total_e, row.total_x, row.e, row.x, row.covered, row.kind, row.gap, row.storage);

        snprintf(checksum_suffix, sizeof(checksum_suffix), ",%08" PRIx32 "\n", checksum(payload));
        return std::string(payload) + checksum_suffix;
    }

    // Validates checksum, field count, bounds, and record kind. Splits the supplied mutable line in place
    bool decode(char *line, Row &row) {
        char *checksum_separator = strrchr(line, ',');
        if(!checksum_separator)
            return false;

        unsigned stored_checksum = 0;
        char trailing_character  = 0;
        if(sscanf(checksum_separator + 1, "%8x %c", &stored_checksum, &trailing_character) != 1)
            return false;
        *checksum_separator = 0;

        // Checksum validation happens before parsing totals, so torn or edited rows cannot silently restore state
        if(checksum(line) != stored_checksum)
            return false;
        int consumed    = 0;
        int field_count = sscanf(line, "%lld,%u,%lld,%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%u,%15[a-z],%u,%u%n",
                              &row.epoch, &row.boot, &row.uptime, &row.total_e, &row.total_x, &row.e, &row.x, &row.covered, row.kind, &row.gap, &row.storage, &consumed);

        return field_count == 11 && line[consumed] == 0 && row.covered <= 60 && row.gap <= 1 && row.storage <= 1 &&
               (!strcmp(row.kind, "baseline") || !strcmp(row.kind, "movement") || !strcmp(row.kind, "sample") || !strcmp(row.kind, "reset"));
    }

    // Validates a YYYY-MM-DD date and returns its midnight using the application's fixed UTC-3 day boundary
    // The process timezone is UTC, round-trip validation rejects impossible calendar dates
    time_t day_epoch(const std::string &date) {
        int year, month, day, consumed = 0;
        if(date.size() != 10 || sscanf(date.c_str(), "%4d-%2d-%2d%n", &year, &month, &day, &consumed) != 3 || consumed != 10 || year < 2024 ||
           year > 2099)
            return -1;

        tm calendar_time      = {};
        calendar_time.tm_year = year - 1900;
        calendar_time.tm_mon  = month - 1;
        calendar_time.tm_mday = day;
        auto epoch            = mktime(&calendar_time);
        tm normalized_time    = {};
        gmtime_r(&epoch, &normalized_time);

        if(normalized_time.tm_year != year - 1900 || normalized_time.tm_mon != month - 1 || normalized_time.tm_mday != day)
            return -1;
        return epoch + 10800;
    }

    // Converts an epoch to the fixed UTC-3 date used for journal filenames and report buckets
    std::string date_at(time_t epoch) {
        tm local_time = {};
        epoch        -= 10800;
        gmtime_r(&epoch, &local_time);
        char buffer[16];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local_time);
        return buffer;
    }

    // Adds a 64-bit total as a decimal JSON string so the browser does not lose integer precision
    void count(cJSON *json, const char *key, uint64_t value) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
        cJSON_AddStringToObject(json, key, buffer);
    }

    // Report accumulator for one calendar day or one hour, it is derived from rows rather than stored separately
    struct Bucket {
        std::string date, label;
        uint64_t e       = 0, x = 0;
        unsigned samples = 0, covered = 0, gaps = 0, resets = 0;
    };

    // Serializes one day/hour aggregate for the local report, the caller owns the returned object
    cJSON *bucket_json(const Bucket &bucket) {
        auto json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "date", bucket.date.c_str());
        cJSON_AddStringToObject(json, "label", bucket.label.c_str());
        count(json, "entries", bucket.e);
        count(json, "exits", bucket.x);
        cJSON_AddNumberToObject(json, "samples", bucket.samples);
        cJSON_AddNumberToObject(json, "covered_seconds", bucket.covered);
        cJSON_AddNumberToObject(json, "gaps", bucket.gaps);
        cJSON_AddNumberToObject(json, "resets", bucket.resets);
        return json;
    }

    // Visits complete, valid rows from one recognized journal file. Invalid rows are ignored in reports
    // Periodically yields so a long report does not monopolize the CPU, upload validation is stricter
    template <class F> void read_day(const std::string &date, F process_row) {
        auto path  = std::string(HISTORY_DIR) + "/" + date + ".csv";
        FILE *file = fopen(path.c_str(), "r");

        if(!file)
            return;
        char line[384];

        if(!fgets(line, sizeof(line), file) || strcmp(line, HEADER)) {
            fclose(file);
            return;
        }

        unsigned line_count = 0;
        while(fgets(line, sizeof(line), file)) {
            Row row;
            if(strchr(line, '\n') && decode(line, row))
                process_row(row);

            if(++line_count % 128 == 0)
                vTaskDelay(1);
        }
        fclose(file);
    }

    // Appends a record only to a file with the expected header and flushes data through the filesystem before success
    bool append(const Row &row) {
        auto date  = row.epoch ? date_at(row.epoch):std::string("undated-") + std::to_string(boot);
        auto path  = std::string(HISTORY_DIR) + "/" + date + ".csv";
        FILE *file = fopen(path.c_str(), "a+");

        if(!file) {
            const int error_code = errno;
            error = "Não foi possível abrir o arquivo no microSD (" + std::string(strerror(error_code)) + ", código " + std::to_string(error_code) + ")";
            return false;
        }
        rewind(file);
        char header[256] = {};
        if(fgets(header, sizeof(header), file)) {
            if(strcmp(header, HEADER)) {
                fclose(file);
                error = "Arquivo incompatível na pasta park-monitor-v1; nada foi sobrescrito";
                return false;
            }
            fseek(file, -1, SEEK_END);
            // Terminate a leftover partial line before appending, upload will still flag that malformed row instead of acknowledging it
            if(fgetc(file) != '\n')
                fputc('\n', file);
        } else {
            clearerr(file);
            fputs(HEADER, file);
        }
        fseek(file, 0, SEEK_END);
        auto line = encode(row);
        bool ok   = fwrite(line.data(), 1, line.size(), file) == line.size();
        if(fflush(file) != 0 || fsync(fileno(file)) != 0)
            ok = false;

        if(fclose(file) != 0)
            ok = false;

        if(!ok)
            error = "Falha ao gravar no microSD; confira o cartão e a alimentação";
        return ok;
    }

    // Converts counter changes into journal deltas. The caller holds the journal mutex
    // Normal calls skip redundant samples, force requests a final checkpoint even without changed totals
    void capture_locked(bool force = false) {
        // 1. Check card availability and decide whether a sample is due
        if(!mounted)
            return;

        auto counter_snapshot = visitor_counter_snapshot();
        if(!counter_snapshot.ready)
            return;

        auto now_us  = esp_timer_get_time();
        bool changed = baseline && (counter_snapshot.entries != previous_entries || counter_snapshot.exits != previous_exits);
        const bool clock_ready = !park_today().empty();

        // Write immediately for changed totals, periodically for unchanged samples, or once a valid clock becomes available
        if(!force && baseline && !changed && now_us - previous_capture_us < park::settings::idle_sample_ms * 1000 &&
           (dated_baseline || !clock_ready))
            return;

        // 2. Build the record and calculate changes from the last successful capture
        Row row;
        row.epoch = time(nullptr);
        if(row.epoch < 1704067200)
            row.epoch = 0;

        row.boot    = boot;
        row.uptime  = now_us / 1000;
        row.total_e = counter_snapshot.entries;
        row.total_x = counter_snapshot.exits;
        row.storage = counter_snapshot.storage_ok;
        strcpy(row.kind, "baseline");

        if(baseline) {
            auto elapsed_seconds = (now_us - previous_capture_us) / 1000000;
            row.gap              = elapsed_seconds > 60;
            row.covered          = row.gap ? 0:static_cast<unsigned>(elapsed_seconds);

            // A decrease is a reset record, not unsigned subtraction that would create a huge visitor count
            if(counter_snapshot.entries < previous_entries || counter_snapshot.exits < previous_exits)
                strcpy(row.kind, "reset");
            else {
                row.e = counter_snapshot.entries - previous_entries;
                row.x = counter_snapshot.exits - previous_exits;
                strcpy(row.kind, row.e || row.x ? "movement":"sample");
            }
        }

        // 3. Mark interruptions when dated logging begins or resumes
        if(row.epoch && !dated_baseline) {
            // A new boot is an interruption, never a negative delta or today's old total
            struct stat file_status = {};
            const auto path         = std::string(HISTORY_DIR) + "/" + date_at(row.epoch) + ".csv";

            if(stat(path.c_str(), &file_status) == 0 && file_status.st_size > static_cast<off_t>(strlen(HEADER)))
                row.gap = 1;
            if(!row.e && !row.x)
                strcpy(row.kind, "baseline");
        }

        // 4. Persist the record and publish the write result
        healthy = append(row);
        reported_healthy.store(healthy);

        // 5. Update the baseline only after the write succeeds
        // A failed write leaves unsaved deltas available for the next attempt
        if(healthy) {
            error.clear();
            baseline            = true;
            previous_entries    = counter_snapshot.entries;
            previous_exits      = counter_snapshot.exits;
            previous_capture_us = now_us;
            last_write          = row.epoch;

            if(row.epoch)
                dated_baseline = true;
            if(row.e || row.x || !strcmp(row.kind, "reset"))
                ++data_revision;
        }
    }


    // Finds the largest valid persisted totals in recognized file tails, then merges missing totals into the counter
    void restore_journal_floor() {
        // Read only file tails, not the complete history.
        // Monotonic totals have no public reset operation, legacy undated files also contain valid totals
        uint64_t restored_entries = 0, restored_exits = 0;
        auto *directory           = opendir(HISTORY_DIR);

        if(!directory)
            return;

        while(auto *entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if(name.size() < 5 || name.substr(name.size() - 4) != ".csv" ||
               !(name.rfind("undated-", 0) == 0 || (name.size() == 14 && day_epoch(name.substr(0, 10)) >= 0)))
                continue;

            const auto path = std::string(HISTORY_DIR) + "/" + name;
            FILE *file      = fopen(path.c_str(), "r");
            if(!file)
                continue;

            char header[256] = {};
            if(!fgets(header, sizeof(header), file) || strcmp(header, HEADER)) {
                fclose(file);
                continue;
            }

            fseek(file, 0, SEEK_END);
            const long file_size = ftell(file);

            // Scan only the last 2 KiB of larger files and skip the first partial line created by seeking into the tail
            if(file_size > 2048) {
                fseek(file, file_size - 2048, SEEK_SET);
                char partial_line[384];
                fgets(partial_line, sizeof(partial_line), file);
            } else
                fseek(file, strlen(HEADER), SEEK_SET);

            char line[384];

            while(fgets(line, sizeof(line), file)) {
                Row row;
                if(strchr(line, '\n') && decode(line, row)) {
                    restored_entries = std::max(restored_entries, row.total_e);
                    restored_exits   = std::max(restored_exits, row.total_x);
                }
            }
            fclose(file);
        }
        closedir(directory);
        const auto before_restore = visitor_counter_snapshot();
        // Adjust the journal baseline by the restored amount so recovered history is not recorded as new movement
        visitor_counter_restore(restored_entries, restored_exits);
        const auto after_restore = visitor_counter_snapshot();
        previous_entries        += after_restore.entries - before_restore.entries;
        previous_exits          += after_restore.exits - before_restore.exits;
    }

    // Checks valid CSV round trips and rejects deliberately corrupted records before startup
    bool journal_codec_self_test() {
        Row test_row;
        test_row.total_e = UINT64_MAX;
        test_row.e       = 42;
        strcpy(test_row.kind, "movement");
        auto encoded = encode(test_row);
        char line[384];
        strcpy(line, encoded.c_str());
        Row decoded_row;

        if(!decode(line, decoded_row) || decoded_row.total_e != UINT64_MAX || decoded_row.e != 42)
            return false;

        // Deliberately corrupt a test record to verify that checksum validation detects damage before the SD worker starts
        encoded[0] = '9';
        strcpy(line, encoded.c_str());

        if(decode(line, decoded_row))
            return false;
        return true;
    }

    // Mounts the microSD once per boot and performs periodic captures in a background FreeRTOS task
    // Mounting never formats the card, a failed mount is reported and retried on a later boot
    void task(void *) {
        // park_history_start established the boot baseline before this worker began
        // The camera may already be counting while this task mounts the card
        // Host and slot initializers below are supplied by ESP-IDF as SDMMC_HOST_DEFAULT and SDMMC_SLOT_CONFIG_DEFAULT
        // Both initializers are invoked unchanged, frequency, wiring, and mount policy are project-specific overrides
        sdmmc_host_t host_config                      = SDMMC_HOST_DEFAULT();
        host_config.max_freq_khz                      = 20000;
        sdmmc_slot_config_t slot_config               = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width                             = 1;
        slot_config.clk                               = GPIO_NUM_39;
        slot_config.cmd                               = GPIO_NUM_38;
        slot_config.d0                                = GPIO_NUM_40;
        slot_config.flags                            |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
        mount_config.format_if_mount_failed           = false;
        mount_config.max_files                        = 4;
        mount_config.allocation_unit_size             = 16384;
        sdmmc_card_t *card                            = nullptr;
        auto mount_status = esp_vfs_fat_sdmmc_mount("/sdcard", &host_config, &slot_config, &mount_config, &card);
        xSemaphoreTake(history_mutex, portMAX_DELAY);

        if(mount_status == ESP_OK) {
            struct stat file_status = {};
            mounted = mkdir(HISTORY_DIR, 0775) == 0 || (stat(HISTORY_DIR, &file_status) == 0 && S_ISDIR(file_status.st_mode));

            if(mounted) {
                card_mb = static_cast<unsigned>((uint64_t(card->csd.capacity) * card->csd.sector_size) / (1024 * 1024));
                error.clear();
                restore_journal_floor();
            } else
                error = "Não foi possível criar a pasta park-monitor-v1 no microSD";
        } else
            error = std::string("microSD indisponível (") + esp_err_to_name(mount_status) + "). Use FAT/FAT32; nenhum arquivo foi apagado.";

        ESP_LOGI("PARK_SD", "Mount %s; capacity=%u MiB; formatting disabled", mounted ? "OK":"FAILED", card_mb);
        if(mounted)
            capture_locked(true);

        initialized.store(true);
        xSemaphoreGive(history_mutex);
        while(true) {
            park_history_capture();
            vTaskDelay(pdMS_TO_TICKS(park::settings::journal_poll_ms));
        }
    }
} // namespace

// Checks the CSV codec, records the initial counter baseline, and starts asynchronous card initialization
bool park_history_start() {
    setenv("TZ", "UTC0", 1);
    tzset();

    if(!journal_codec_self_test())
        return false;

    ESP_LOGI("PARK_SD", "Journal codec self-test PASS");
    history_mutex               = xSemaphoreCreateMutex();
    boot                        = esp_random();
    const auto initial_snapshot = visitor_counter_snapshot();
    previous_entries            = initial_snapshot.entries;
    previous_exits              = initial_snapshot.exits;
    previous_capture_us         = esp_timer_get_time();
    baseline                    = true;
    return history_mutex && xTaskCreate(task, "ParkSD", 6144, nullptr, 1, nullptr) == pdPASS;
}

// Serializes a normal journal capture with report, cleanup, and cloud cursor operations
void park_history_capture() {
    if(!history_mutex)
        return;
    xSemaphoreTake(history_mutex, portMAX_DELAY);
    capture_locked();
    xSemaphoreGive(history_mutex);
}

// Checks mounted, healthy, and baseline state with a bounded lock wait, false also covers a busy journal
bool park_history_ready() {
    if(!history_mutex || xSemaphoreTake(history_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return false;
    const bool ok = mounted && healthy && baseline;
    xSemaphoreGive(history_mutex);
    return ok;
}

// Reports that the mount attempt finished, regardless of whether a card was available
bool park_history_initialized() {
    return initialized.load();
}

// Returns the most recently published write-health flag without waiting for the journal mutex
bool park_history_healthy() {
    return reported_healthy.load();
}

// Changes after a successfully written movement/reset record so cloud scheduling can notice new work
uint32_t park_history_revision() {
    return data_revision.load();
}

// Forces a capture and returns with the journal mutex held. A true result describes lock acquisition, not card health
bool park_history_sleep_lock() {
    if(!history_mutex || xSemaphoreTake(history_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
        return false;
    capture_locked(true);
    // Held even when the card/clock failed: report the fault, but do not make storage health a permanent sleep veto.
    // NVS is flushed separately
    return true;
}

// Releases the lock retained by sleep_lock when sleep is cancelled or another drain pass is needed
void park_history_sleep_unlock() {
    if(history_mutex)
        xSemaphoreGive(history_mutex);
}

// Returns today's fixed UTC-3 date, or an empty string until the clock reaches the validity threshold
std::string park_today() {
    auto now_epoch = time(nullptr);
    return now_epoch < 1704067200 ? "":date_at(now_epoch);
}

// Formats a valid epoch as an ISO timestamp in UTC, invalid clocks yield an empty string
std::string park_timestamp(time_t value) {
    if(value < 1704067200)
        return "";

    tm utc_time = {};
    gmtime_r(&value, &utc_time);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc_time);
    return buffer;
}

// Accepts ordered dates up to today with at most 365 days between endpoints (366 inclusive calendar days)
bool park_history_range(const std::string &start_date, const std::string &end_date) {
    auto start_epoch = day_epoch(start_date), end_epoch = day_epoch(end_date);
    return start_epoch >= 0 && end_epoch >= start_epoch && end_epoch - start_epoch <= 365 * 86400 && end_epoch <= day_epoch(park_today());
}

// Builds card/clock diagnostics, a busy response avoids blocking the local page for long
cJSON *park_history_status() {
    auto status_json = cJSON_CreateObject();

    if(history_mutex && xSemaphoreTake(history_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_AddBoolToObject(status_json, "ok", false);
        cJSON_AddBoolToObject(status_json, "busy", true);
        cJSON_AddBoolToObject(status_json, "clock_ok", !park_today().empty());
        cJSON_AddStringToObject(status_json, "error", "microSD ocupado; aguardando nova consulta");
        return status_json;
    }

    cJSON_AddBoolToObject(status_json, "mounted", mounted);
    cJSON_AddBoolToObject(status_json, "ok", healthy);
    cJSON_AddNumberToObject(status_json, "capacity_mb", card_mb);
    cJSON_AddStringToObject(status_json, "error", error.c_str());
    cJSON_AddStringToObject(status_json, "last_write", park_timestamp(last_write).c_str());
    cJSON_AddBoolToObject(status_json, "clock_ok", !park_today().empty());

    if(history_mutex)
        xSemaphoreGive(history_mutex);
    return status_json;
}

// Aggregates daily totals, hourly buckets, coverage, and recent movement from dated files
// Reuses a serialized report for the same range for up to ten seconds, the returned JSON belongs to the caller
cJSON *park_history_report(const std::string &start, const std::string &end) {
    // 1. Acquire the journal lock and reuse a valid cached report
    if(history_mutex)
        xSemaphoreTake(history_mutex, portMAX_DELAY);

    // Cache only serialized JSON, each caller receives its own parsed object and owns its lifetime
    if(start == cached_start && end == cached_end && esp_timer_get_time() - cached_at < 10000000 &&
       !cached_report.empty()) {
        auto report_json = cJSON_Parse(cached_report.c_str());
        if(history_mutex)
            xSemaphoreGive(history_mutex);
        return report_json;
    }

    // 2. Prepare daily and hourly buckets and the recent movement list
    std::vector<Bucket> daily_buckets, hourly_buckets(24);
    std::deque<Row> recent_movements;

    for(unsigned hour_index = 0; hour_index < 24; ++hour_index) {
        char hour_label[8];
        snprintf(hour_label, sizeof(hour_label), "%02uh", hour_index);
        hourly_buckets[hour_index].label = hour_label;
    }

    uint64_t total_entries       = 0, total_exits = 0;
    unsigned total_samples       = 0, total_covered_seconds = 0, total_gaps = 0, total_resets = 0;
    time_t earliest_sample_epoch = 0;
    auto start_epoch             = day_epoch(start), end_epoch = day_epoch(end);

    // 3. Read the requested days and accumulate valid records
    if(start_epoch >= 0 && end_epoch >= start_epoch && end_epoch - start_epoch <= 365 * 86400)
        // Visit each requested calendar day, including empty days, and assign valid rows to their local-day hour
        for(time_t day = start_epoch; day <= end_epoch; day += 86400) {
            Bucket day_bucket;
            day_bucket.date  = date_at(day);
            day_bucket.label = day_bucket.date.substr(8, 2) + "/" + day_bucket.date.substr(5, 2);

            if(mounted)
                read_day(day_bucket.date, [&](const Row &row) {
                    if(row.epoch < day || row.epoch >= day + 86400)
                        return;

                    auto hour           = unsigned((row.epoch - day) / 3600);
                    auto &hour_bucket   = hourly_buckets[hour];
                    day_bucket.e       += row.e;
                    day_bucket.x       += row.x;
                    ++day_bucket.samples;
                    day_bucket.covered += std::min<unsigned>(row.covered, row.epoch - day);
                    day_bucket.gaps    += row.gap;
                    day_bucket.resets  += !strcmp(row.kind, "reset");
                    hour_bucket.e      += row.e;
                    hour_bucket.x      += row.x;
                    ++hour_bucket.samples;

                    if(!earliest_sample_epoch || row.epoch < earliest_sample_epoch)
                        earliest_sample_epoch = row.epoch;

                    // Keep at most the latest 100 movement records for the detailed list while all rows contribute to aggregates
                    if(!strcmp(row.kind, "movement")) {
                        recent_movements.push_back(row);
                        if(recent_movements.size() > 100)
                            recent_movements.pop_front();
                    }
                });
            total_entries         += day_bucket.e;
            total_exits           += day_bucket.x;
            total_samples         += day_bucket.samples;
            total_covered_seconds += day_bucket.covered;
            total_gaps            += day_bucket.gaps;
            total_resets          += day_bucket.resets;
            daily_buckets.push_back(day_bucket);
        }

    // 4. Build totals, chart buckets, and history as JSON
    auto report_json = cJSON_CreateObject();
    cJSON_AddStringToObject(report_json, "start", start.c_str());
    cJSON_AddStringToObject(report_json, "end", end.c_str());
    count(report_json, "entries", total_entries);
    count(report_json, "exits", total_exits);
    cJSON_AddBoolToObject(report_json, "has_samples", total_samples > 0);
    cJSON_AddNumberToObject(report_json, "sample_count", total_samples);
    cJSON_AddNumberToObject(report_json, "covered_seconds", total_covered_seconds);
    cJSON_AddNumberToObject(report_json, "gaps", total_gaps);
    cJSON_AddNumberToObject(report_json, "resets", total_resets);

    if(earliest_sample_epoch)
        cJSON_AddStringToObject(report_json, "started_at", park_timestamp(earliest_sample_epoch).c_str());
    else
        cJSON_AddNullToObject(report_json, "started_at");

    auto daily = cJSON_AddArrayToObject(report_json, "daily");
    for(const auto &day_bucket : daily_buckets)
        cJSON_AddItemToArray(daily, bucket_json(day_bucket));

    // A single-day view uses hourly buckets, a multi-day view uses daily buckets
    auto chart = cJSON_AddArrayToObject(report_json, "chart");
    for(const auto &chart_bucket : (start == end ? hourly_buckets:daily_buckets))
        cJSON_AddItemToArray(chart, bucket_json(chart_bucket));

    auto history_json = cJSON_AddArrayToObject(report_json, "history");
    for(auto row_iterator = recent_movements.rbegin(); row_iterator != recent_movements.rend(); ++row_iterator) {
        const auto &row = *row_iterator;
        auto item       = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "at", park_timestamp(row.epoch).c_str());
        count(item, "entries", row.e);
        count(item, "exits", row.x);
        cJSON_AddStringToObject(item, "kind", row.kind);
        cJSON_AddBoolToObject(item, "gap", row.gap);
        cJSON_AddBoolToObject(item, "storage_ok", row.storage);
        cJSON_AddItemToArray(history_json, item);
    }

    // 5. Refresh the serialized cache and release the journal lock
    char *serialized = cJSON_PrintUnformatted(report_json);
    if(serialized) {
        cached_report = serialized;
        cJSON_free(serialized);
        cached_start = start;
        cached_end   = end;
        cached_at    = esp_timer_get_time();
    }
    if(history_mutex)
        xSemaphoreGive(history_mutex);
    return report_json;
}

// Makes cleanup preserve any dated journal file whose cloud cursor has not reached its current end
void park_history_cloud_protect() {
    if(!history_mutex)
        return;
    xSemaphoreTake(history_mutex, portMAX_DELAY);
    cloud_protected = true;
    xSemaphoreGive(history_mutex);
}

// Reads one bounded batch from the oldest dated file with unsent data
// Does not advance the cursor, incomplete/corrupt rows fail the batch instead of being silently skipped
ParkCloudBatch park_history_cloud_batch(unsigned timeout_ms) {
    // 1. Acquire the journal lock with a bounded wait
    ParkCloudBatch batch;
    if(!history_mutex)
        return batch;
    if(xSemaphoreTake(history_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        batch.error = "sd_busy";
        return batch;
    }

    // 2. List dated journal files and sort them from oldest to newest
    auto *directory = mounted ? opendir(HISTORY_DIR):nullptr;
    std::vector<std::string> files;

    if(directory) {
        while(auto *entry = readdir(directory)) {
            std::string filename(entry->d_name);
            if(filename.size() == 14 && filename.substr(10) == ".csv" && day_epoch(filename.substr(0, 10)) >= 0)
                files.push_back(filename);
        }

        closedir(directory);
        // ISO date filenames sort chronologically so older unsent records are uploaded before newer files
        std::sort(files.begin(), files.end());
        batch.ok    = true;
        batch.error = "ready";
    }
    // 3. Select the oldest pending file and validate its header and cursor
    for(const auto &filename : files) {
        const auto path         = std::string(HISTORY_DIR) + "/" + filename;
        struct stat file_status = {};

        if(stat(path.c_str(), &file_status) != 0) {
            batch.ok    = false;
            batch.error = "sd_stat_failed";
            break;
        }

        long offset = cloud_cursor(filename, file_status.st_size);
        if(offset == file_status.st_size)
            continue;

        FILE *file = fopen(path.c_str(), "r");
        if(!file) {
            batch.ok    = false;
            batch.error = "sd_open_failed";
            break;
        }
        char line[384];
        if(!fgets(line, sizeof(line), file) || strcmp(line, HEADER)) {
            fclose(file);
            batch.ok    = false;
            batch.error = "sd_header_mismatch";
            break;
        }

        batch.file = filename;
        batch.from = offset;

        if(offset && fseek(file, offset, SEEK_SET) != 0) {
            fclose(file);
            batch.ok    = false;
            batch.error = "sd_seek_failed";
            break;
        }

        // 4. Read a bounded batch and create stable IDs from the original rows
        batch.records = cJSON_CreateArray();
        while(cJSON_GetArraySize(batch.records) < park::settings::max_batch_records && fgets(line, sizeof(line), file)) {
            // A partial or corrupt row must never be acknowledged/skipped silently
            if(!strchr(line, '\n')) {
                batch.ok    = false;
                batch.error = "sd_partial_row";
                break;
            }

            // Hash the original persisted row before decode modifies it, identical replays keep the same cloud record ID
            unsigned char digest[32];
            if(mbedtls_sha256(reinterpret_cast<const unsigned char *>(line), strlen(line), digest, 0) != 0) {
                batch.ok = false;
                break;
            }

            char identity[65];
            for(unsigned i = 0; i < 32; ++i)
                snprintf(identity + i * 2, 3, "%02x", digest[i]);
            Row row;
            if(!decode(line, row) || row.epoch < 1704067200) {
                batch.ok    = false;
                batch.error = "sd_invalid_row";
                break;
            }

            auto item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", identity);
            cJSON_AddNumberToObject(item, "epoch", row.epoch);
            count(item, "entries", row.e);
            count(item, "exits", row.x);
            cJSON_AddStringToObject(item, "kind", row.kind);
            cJSON_AddItemToArray(batch.records, item);
        }
        // 5. Record the exact batch boundary and check the read result
        // Store the exact byte range read so acknowledgment can advance only this batch, not later appended records
        batch.to = ftell(file);
        if(ferror(file) || batch.to < 0) {
            batch.ok    = false;
            batch.error = "sd_read_failed";
        }
        fclose(file);
        break;
    }
    // 6. Release the journal lock and return the batch without acknowledging it
    xSemaphoreGive(history_mutex);
    return batch;
}

// Persists the acknowledged end offset only if the cursor still matches the batch's starting point
// A failed cursor write leaves records eligible for replay, stable row IDs let the server deduplicate them
bool park_history_cloud_ack(const ParkCloudBatch &batch) {
    if(!history_mutex || !batch.ok || batch.file.empty() || batch.to < batch.from)
        return false;

    xSemaphoreTake(history_mutex, portMAX_DELAY);
    const auto source       = std::string(HISTORY_DIR) + "/" + batch.file;
    struct stat file_status = {};
    bool ok                 = stat(source.c_str(), &file_status) == 0 && file_status.st_size >= batch.to && cloud_cursor(batch.file, file_status.st_size) == batch.from;
    if(ok) {
        ok                = mkdir(CURSOR_DIR, 0775) == 0 || (stat(CURSOR_DIR, &file_status) == 0 && S_ISDIR(file_status.st_mode));
        const auto path   = std::string(CURSOR_DIR) + "/" + batch.file + ".ack";
        FILE *cursor_file = ok ? fopen(path.c_str(), "w"):nullptr;
        if(!cursor_file)
            ok = false;
        else {
            const auto seal = batch.file + ":" + std::to_string(batch.to);
            ok              = fprintf(cursor_file, "%ld,%08" PRIx32 "\n", batch.to, checksum(seal.c_str())) > 0;

            if(fflush(cursor_file) != 0 || fsync(fileno(cursor_file)) != 0)
                ok = false;
            if(fclose(cursor_file) != 0)
                ok = false;
        }
    }
    xSemaphoreGive(history_mutex);
    return ok;
}

// Exports the daily report as CSV, leaving empty values for dates without samples
std::string park_history_csv(const std::string &start, const std::string &end) {
    auto report     = park_history_report(start, end);
    std::string csv = "data,entradas,saidas,tempo_coleta_segundos,registros,interrupcoes,reinicios\r\n";
    cJSON *day_json = nullptr;
    cJSON_ArrayForEach(day_json, cJSON_GetObjectItem(report, "daily")) {
        const bool has_samples = cJSON_GetNumberValue(cJSON_GetObjectItem(day_json, "samples")) > 0;
        char row_buffer[256];
        snprintf(row_buffer, sizeof(row_buffer), "%s,%s,%s,%.0f,%.0f,%.0f,%.0f\r\n",
                 cJSON_GetStringValue(cJSON_GetObjectItem(day_json, "date")),
                 has_samples ? cJSON_GetStringValue(cJSON_GetObjectItem(day_json, "entries")):"",
                 has_samples ? cJSON_GetStringValue(cJSON_GetObjectItem(day_json, "exits")):"",
                 cJSON_GetNumberValue(cJSON_GetObjectItem(day_json, "covered_seconds")),
                 cJSON_GetNumberValue(cJSON_GetObjectItem(day_json, "samples")),
                 cJSON_GetNumberValue(cJSON_GetObjectItem(day_json, "gaps")),
                 cJSON_GetNumberValue(cJSON_GetObjectItem(day_json, "resets")));
        csv += row_buffer;
    }
    cJSON_Delete(report);
    return csv;
}

// Builds a deletion preview or applies a matching plan. Today's files and unsynchronized records are protected
// The returned JSON contains the selected totals, plan token, removal count, and any failure
cJSON *park_history_cleanup(const std::string &cutoff_date, bool erase, const std::string &plan) {
    // 1. Validate the cutoff date and acquire the journal lock
    auto result             = cJSON_CreateObject();
    const auto today        = park_today();
    const auto cutoff_epoch = day_epoch(cutoff_date);

    if(!history_mutex || today.empty() || cutoff_epoch < 0 || date_at(cutoff_epoch) != cutoff_date || cutoff_date > today) {
        cJSON_AddStringToObject(result, "error", "Escolha uma data válida, até hoje. Os registros de hoje são preservados.");
        return result;
    }
    xSemaphoreTake(history_mutex, portMAX_DELAY);

    // 2. Collect owned files and enforce date and synchronization protections
    struct Candidate {
        std::string name;
        uint64_t bytes;
    };

    std::vector<Candidate> files;
    uint64_t bytes = 0;
    std::string failure;
    auto *directory = mounted ? opendir(HISTORY_DIR):nullptr;

    if(!directory)
        failure = "Não foi possível consultar o microSD. Nenhuma limpeza iniciada.";

    if(directory) {
        errno = 0;
        while(auto entry = readdir(directory)) {
            std::string name = entry->d_name;

            // Only the owned dated journal format, never traverse or recurse
            if(name.size() != 14 || name.substr(10) != ".csv")
                continue;

            const auto file_date  = name.substr(0, 10);
            const auto file_epoch = day_epoch(file_date);
            if(file_epoch < 0 || date_at(file_epoch) != file_date || file_date >= cutoff_date || file_date >= today)
                continue;

            const auto path         = std::string(HISTORY_DIR) + "/" + name;
            struct stat file_status = {};
            if(stat(path.c_str(), &file_status) != 0 || !S_ISREG(file_status.st_mode)) {
                failure = "Falha ao consultar um arquivo. Nenhuma limpeza iniciada.";
                break;
            }

            if(cloud_protected && cloud_cursor(name, file_status.st_size) < file_status.st_size) {
                failure = "Há registros ainda não sincronizados com a nuvem. Aguarde antes de limpar.";
                break;
            }

            // A matching name is insufficient: verify our journal header too
            FILE *file       = fopen(path.c_str(), "r");
            char header[256] = {};
            const bool owned = file && fgets(header, sizeof(header), file) && strcmp(header, HEADER) == 0;
            if(file)
                fclose(file);
            if(!owned) {
                failure = "Arquivo não reconhecido no período. Nenhuma limpeza iniciada.";
                break;
            }

            files.push_back({name, static_cast<uint64_t>(file_status.st_size)});
            bytes += file_status.st_size;
            errno  = 0;
        }
        if(errno && failure.empty())
            failure = "Falha ao listar o cartão. Nenhuma limpeza iniciada.";
        closedir(directory);
    }
    std::sort(files.begin(), files.end(), [](const Candidate &first_candidate, const Candidate &second_candidate) {
        return first_candidate.name < second_candidate.name;
    });

    // 3. Create a plan bound to the cutoff and the selected filenames and sizes
    // Changed files invalidate a previously displayed preview
    std::string identity = cutoff_date;
    for(const auto &file : files)
        identity += "/" + file.name + ":" + std::to_string(file.bytes);

    char token[16];
    snprintf(token, sizeof(token), "%08" PRIx32, checksum(identity.c_str()));
    unsigned removed = 0;

    if(erase && failure.empty() && plan != token)
        failure = "A seleção mudou. Consulte os arquivos novamente antes de confirmar.";

    // 4. Delete files only when the requested plan still matches
    if(erase && failure.empty()) {
        for(const auto &file : files) {
            const auto path = std::string(HISTORY_DIR) + "/" + file.name;

            if(unlink(path.c_str()) != 0) {
                failure = "Limpeza interrompida por erro no cartão. Arquivos removidos: " + std::to_string(removed) + ".";
                break;
            }
            ++removed;
        }
        // Discard the cached report after applying cleanup so the next query reads the remaining files
        cached_report.clear();
    }

    // 5. Return the selection, removal count, and any failure
    cJSON_AddNumberToObject(result, "files", files.size());
    count(result, "bytes", bytes);
    cJSON_AddStringToObject(result, "plan", token);
    cJSON_AddStringToObject(result, "before", cutoff_date.c_str());
    cJSON_AddNumberToObject(result, "removed", removed);

    if(!failure.empty())
        cJSON_AddStringToObject(result, "error", failure.c_str());
    xSemaphoreGive(history_mutex);
    return result;
}
