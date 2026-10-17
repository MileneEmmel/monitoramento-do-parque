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
constexpr const char *CURSOR_DIR = "/sdcard/park-cloud-v1";
bool cloud_protected = false;
constexpr const char *HEADER =
    "epoch,boot,uptime_ms,total_entries,total_exits,entries,exits,covered_seconds,kind,gap,storage_ok,checksum\n";
SemaphoreHandle_t mutex = nullptr;
bool mounted = false, healthy = false, baseline = false;
bool dated_baseline = false;
std::atomic<bool> initialized{false}, reported_healthy{false};
std::atomic<uint32_t> data_revision{0};
std::string error = "Verificando microSD";
uint32_t boot = 0;
uint64_t previous_e = 0, previous_x = 0;
int64_t previous_us = 0;
time_t last_write = 0;
unsigned card_mb = 0;
std::string cached_start, cached_end, cached_report;
int64_t cached_at = 0;
struct Row {
    long long epoch = 0, uptime = 0;
    unsigned boot = 0, covered = 0, gap = 0, storage = 0;
    uint64_t total_e = 0, total_x = 0, e = 0, x = 0;
    char kind[16] = {};
};
uint32_t checksum(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= static_cast<unsigned char>(*s);
        h *= 16777619u;
    }
    return h;
}
long cloud_cursor(const std::string &file, long size)
{
    FILE *f = fopen((std::string(CURSOR_DIR) + "/" + file + ".ack").c_str(), "r");
    long offset = 0; unsigned crc = 0;
    if (!f) return 0;
    const int read = fscanf(f, "%ld,%x", &offset, &crc);
    fclose(f);
    const auto seal = file + ":" + std::to_string(offset);
    return read == 2 && offset >= 0 && offset <= size && crc == checksum(seal.c_str()) ? offset : 0;
}
std::string encode(const Row &r)
{
    char p[256], crc[16];
    snprintf(p,
             sizeof(p),
             "%lld,%u,%lld,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%s,%u,%u",
             r.epoch,
             r.boot,
             r.uptime,
             r.total_e,
             r.total_x,
             r.e,
             r.x,
             r.covered,
             r.kind,
             r.gap,
             r.storage);
    snprintf(crc, sizeof(crc), ",%08" PRIx32 "\n", checksum(p));
    return std::string(p) + crc;
}
bool decode(char *line, Row &r)
{
    char *comma = strrchr(line, ',');
    if (!comma)
        return false;
    unsigned crc = 0;
    char extra = 0;
    if (sscanf(comma + 1, "%8x %c", &crc, &extra) != 1)
        return false;
    *comma = 0;
    if (checksum(line) != crc)
        return false;
    int consumed = 0;
    int n = sscanf(line,
                   "%lld,%u,%lld,%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%u,%15[a-z],%u,%u%n",
                   &r.epoch,
                   &r.boot,
                   &r.uptime,
                   &r.total_e,
                   &r.total_x,
                   &r.e,
                   &r.x,
                   &r.covered,
                   r.kind,
                   &r.gap,
                   &r.storage,
                   &consumed);
    return n == 11 && line[consumed] == 0 && r.covered <= 60 && r.gap <= 1 && r.storage <= 1 &&
        (!strcmp(r.kind, "baseline") || !strcmp(r.kind, "movement") || !strcmp(r.kind, "sample") ||
         !strcmp(r.kind, "reset"));
}
time_t day_epoch(const std::string &s)
{
    int y, m, d, used = 0;
    if (s.size() != 10 || sscanf(s.c_str(), "%4d-%2d-%2d%n", &y, &m, &d, &used) != 3 || used != 10 || y < 2024 ||
        y > 2099)
        return -1;
    tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    auto v = mktime(&t);
    tm check = {};
    gmtime_r(&v, &check);
    if (check.tm_year != y - 1900 || check.tm_mon != m - 1 || check.tm_mday != d)
        return -1;
    return v + 10800;
}
std::string date_at(time_t t)
{
    tm out = {};
    t -= 10800;
    gmtime_r(&t, &out);
    char s[16];
    strftime(s, sizeof(s), "%Y-%m-%d", &out);
    return s;
}
void count(cJSON *o, const char *key, uint64_t v)
{
    char b[32];
    snprintf(b, sizeof(b), "%" PRIu64, v);
    cJSON_AddStringToObject(o, key, b);
}
struct Bucket {
    std::string date, label;
    uint64_t e = 0, x = 0;
    unsigned samples = 0, covered = 0, gaps = 0, resets = 0;
};
cJSON *bucket_json(const Bucket &b)
{
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "date", b.date.c_str());
    cJSON_AddStringToObject(j, "label", b.label.c_str());
    count(j, "entries", b.e);
    count(j, "exits", b.x);
    cJSON_AddNumberToObject(j, "samples", b.samples);
    cJSON_AddNumberToObject(j, "covered_seconds", b.covered);
    cJSON_AddNumberToObject(j, "gaps", b.gaps);
    cJSON_AddNumberToObject(j, "resets", b.resets);
    return j;
}
template <class F>
void read_day(const std::string &date, F fn)
{
    auto path = std::string(HISTORY_DIR) + "/" + date + ".csv";
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return;
    char line[384];
    if (!fgets(line, sizeof(line), f) || strcmp(line, HEADER)) {
        fclose(f);
        return;
    }
    unsigned lines = 0;
    while (fgets(line, sizeof(line), f)) {
        Row r;
        if (strchr(line, '\n') && decode(line, r))
            fn(r);
        if (++lines % 128 == 0)
            vTaskDelay(1);
    }
    fclose(f);
}
bool append(const Row &r)
{
    auto date = r.epoch ? date_at(r.epoch) : std::string("undated-") + std::to_string(boot);
    auto path = std::string(HISTORY_DIR) + "/" + date + ".csv";
    FILE *f = fopen(path.c_str(), "a+");
    if (!f) {
        const int code = errno;
        error = "Não foi possível abrir o arquivo no microSD (" + std::string(strerror(code)) +
            ", código " + std::to_string(code) + ")";
        return false;
    }
    rewind(f);
    char header[256] = {};
    if (fgets(header, sizeof(header), f)) {
        if (strcmp(header, HEADER)) {
            fclose(f);
            error = "Arquivo incompatível na pasta park-monitor-v1; nada foi sobrescrito";
            return false;
        }
        fseek(f, -1, SEEK_END);
        if (fgetc(f) != '\n')
            fputc('\n', f);
    } else {
        clearerr(f);
        fputs(HEADER, f);
    }
    fseek(f, 0, SEEK_END);
    auto line = encode(r);
    bool ok = fwrite(line.data(), 1, line.size(), f) == line.size();
    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        error = "Falha ao gravar no microSD; confira o cartão e a alimentação";
    return ok;
}
void capture_locked(bool force = false)
{
    if (!mounted)
        return;
    auto s = visitor_counter_snapshot();
    if (!s.ready)
        return;
    auto now = esp_timer_get_time();
    bool changed = baseline && (s.entries != previous_e || s.exits != previous_x);
    const bool clock_ready = !park_today().empty();
    if (!force && baseline && !changed && now - previous_us < park::settings::sample_ms * 1000 && (dated_baseline || !clock_ready))
        return;
    Row r;
    r.epoch = time(nullptr);
    if (r.epoch < 1704067200)
        r.epoch = 0;
    r.boot = boot;
    r.uptime = now / 1000;
    r.total_e = s.entries;
    r.total_x = s.exits;
    r.storage = s.storage_ok;
    strcpy(r.kind, "baseline");
    if (baseline) {
        auto seconds = (now - previous_us) / 1000000;
        r.gap = seconds > 60;
        r.covered = r.gap ? 0 : static_cast<unsigned>(seconds);
        if (s.entries < previous_e || s.exits < previous_x)
            strcpy(r.kind, "reset");
        else {
            r.e = s.entries - previous_e;
            r.x = s.exits - previous_x;
            strcpy(r.kind, r.e || r.x ? "movement" : "sample");
        }
    }
    if (r.epoch && !dated_baseline) {
        // A new boot is an interruption, never a negative delta or today's old total.
        struct stat st = {};
        const auto path = std::string(HISTORY_DIR) + "/" + date_at(r.epoch) + ".csv";
        if (stat(path.c_str(), &st) == 0 && st.st_size > static_cast<off_t>(strlen(HEADER))) r.gap = 1;
        if (!r.e && !r.x) strcpy(r.kind, "baseline");
    }
    healthy = append(r);
    reported_healthy.store(healthy);
    if (healthy) {
        error.clear();
        baseline = true;
        previous_e = s.entries;
        previous_x = s.exits;
        previous_us = now;
        last_write = r.epoch;
        if (r.epoch) dated_baseline = true;
        if (r.e || r.x || !strcmp(r.kind, "reset")) ++data_revision;
    }
}
void restore_journal_floor()
{
    // Read only file tails, not the complete history. Monotonic totals have no
    // public reset operation; legacy undated files also contain valid totals.
    uint64_t e = 0, x = 0;
    auto *dir = opendir(HISTORY_DIR);
    if (!dir) return;
    while (auto *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() < 5 || name.substr(name.size()-4) != ".csv" ||
            !(name.rfind("undated-", 0) == 0 || (name.size() == 14 && day_epoch(name.substr(0,10)) >= 0))) continue;
        const auto path = std::string(HISTORY_DIR) + "/" + name;
        FILE *f = fopen(path.c_str(), "r");
        if (!f) continue;
        char header[256] = {};
        if (!fgets(header,sizeof(header),f) || strcmp(header,HEADER)) { fclose(f); continue; }
        fseek(f,0,SEEK_END); const long size = ftell(f);
        if (size > 2048) { fseek(f,size-2048,SEEK_SET); char skip[384]; fgets(skip,sizeof(skip),f); }
        else fseek(f,strlen(HEADER),SEEK_SET);
        char line[384];
        while (fgets(line,sizeof(line),f)) {
            Row r;
            if (strchr(line,'\n') && decode(line,r)) { e=std::max(e,r.total_e); x=std::max(x,r.total_x); }
        }
        fclose(f);
    }
    closedir(dir);
    const auto before = visitor_counter_snapshot();
    visitor_counter_restore(e,x);
    const auto after = visitor_counter_snapshot();
    previous_e += after.entries-before.entries;
    previous_x += after.exits-before.exits;
}
void task(void *)
{
    // Establish the boot baseline before slow storage/network work. The camera
    // may already be counting; no first-visitor wait for the card is required.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 20000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_39;
    slot.cmd = GPIO_NUM_38;
    slot.d0 = GPIO_NUM_40;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t config = {};
    config.format_if_mount_failed = false;
    config.max_files = 4;
    config.allocation_unit_size = 16384;
    sdmmc_card_t *card = nullptr;
    auto status = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &config, &card);
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (status == ESP_OK) {
        struct stat st = {};
        mounted = mkdir(HISTORY_DIR, 0775) == 0 || (stat(HISTORY_DIR, &st) == 0 && S_ISDIR(st.st_mode));
        if (mounted) {
            card_mb = static_cast<unsigned>((uint64_t(card->csd.capacity) * card->csd.sector_size) / (1024 * 1024));
            error.clear();
            restore_journal_floor();
        } else
            error = "Não foi possível criar a pasta park-monitor-v1 no microSD";
    } else
        error = std::string("microSD indisponível (") + esp_err_to_name(status) +
            "). Use FAT/FAT32; nenhum arquivo foi apagado.";
    ESP_LOGI("PARK_SD", "Mount %s; capacity=%u MiB; formatting disabled", mounted ? "OK" : "FAILED", card_mb);
    if(mounted) capture_locked(true);
    initialized.store(true);
    xSemaphoreGive(mutex);
    while (true) {
        park_history_capture();
        vTaskDelay(pdMS_TO_TICKS(park::settings::journal_ms));
    }
}
} // namespace
bool park_history_start()
{
    setenv("TZ", "UTC0", 1);
    tzset();
    Row r;
    r.total_e = UINT64_MAX;
    r.e = 42;
    strcpy(r.kind, "movement");
    auto encoded = encode(r);
    char line[384];
    strcpy(line, encoded.c_str());
    Row decoded;
    if (!decode(line, decoded) || decoded.total_e != UINT64_MAX || decoded.e != 42)
        return false;
    encoded[0] = '9';
    strcpy(line, encoded.c_str());
    if (decode(line, decoded))
        return false;
    ESP_LOGI("PARK_SD", "Journal codec self-test PASS");
    mutex = xSemaphoreCreateMutex();
    boot = esp_random();
    const auto initial = visitor_counter_snapshot();
    previous_e = initial.entries; previous_x = initial.exits;
    previous_us = esp_timer_get_time(); baseline = true;
    return mutex && xTaskCreate(task, "ParkSD", 6144, nullptr, 1, nullptr) == pdPASS;
}
void park_history_capture()
{
    if (!mutex)
        return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    capture_locked();
    xSemaphoreGive(mutex);
}
bool park_history_ready()
{
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    const bool ok = mounted && healthy && baseline;
    xSemaphoreGive(mutex);
    return ok;
}
bool park_history_initialized() { return initialized.load(); }
bool park_history_healthy() { return reported_healthy.load(); }
uint32_t park_history_revision() { return data_revision.load(); }
bool park_history_sleep_lock()
{
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    capture_locked(true);
    // Held even when the card/clock failed: report the fault, but do not make
    // storage health a permanent sleep veto. NVS is flushed separately.
    return true;
}
void park_history_sleep_unlock() { if (mutex) xSemaphoreGive(mutex); }
std::string park_today()
{
    auto t = time(nullptr);
    return t < 1704067200 ? "" : date_at(t);
}
std::string park_timestamp(time_t value)
{
    if (value < 1704067200)
        return "";
    tm t = {};
    gmtime_r(&value, &t);
    char s[32];
    strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%SZ", &t);
    return s;
}
bool park_history_range(const std::string &a, const std::string &b)
{
    auto x = day_epoch(a), y = day_epoch(b);
    return x >= 0 && y >= x && y - x <= 365 * 86400 && y <= day_epoch(park_today());
}
cJSON *park_history_status()
{
    auto j = cJSON_CreateObject();
    if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_AddBoolToObject(j, "ok", false);
        cJSON_AddBoolToObject(j, "busy", true);
        cJSON_AddBoolToObject(j, "clock_ok", !park_today().empty());
        cJSON_AddStringToObject(j, "error", "microSD ocupado; aguardando nova consulta");
        return j;
    }
    cJSON_AddBoolToObject(j, "mounted", mounted);
    cJSON_AddBoolToObject(j, "ok", healthy);
    cJSON_AddNumberToObject(j, "capacity_mb", card_mb);
    cJSON_AddStringToObject(j, "error", error.c_str());
    cJSON_AddStringToObject(j, "last_write", park_timestamp(last_write).c_str());
    cJSON_AddBoolToObject(j, "clock_ok", !park_today().empty());
    if (mutex)
        xSemaphoreGive(mutex);
    return j;
}
cJSON *park_history_report(const std::string &start, const std::string &end)
{
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    if (start == cached_start && end == cached_end && esp_timer_get_time() - cached_at < 10000000 &&
        !cached_report.empty()) {
        auto j = cJSON_Parse(cached_report.c_str());
        if (mutex) xSemaphoreGive(mutex);
        return j;
    }
    std::vector<Bucket> days, hours(24);
    std::deque<Row> history;
    for (unsigned h = 0; h < 24; ++h) {
        char b[8];
        snprintf(b, sizeof(b), "%02uh", h);
        hours[h].label = b;
    }
    uint64_t e = 0, x = 0;
    unsigned samples = 0, covered = 0, gaps = 0, resets = 0;
    time_t first = 0;
    auto from = day_epoch(start), to = day_epoch(end);
    if (from >= 0 && to >= from && to - from <= 365 * 86400)
        for (time_t day = from; day <= to; day += 86400) {
            Bucket b;
            b.date = date_at(day);
            b.label = b.date.substr(8, 2) + "/" + b.date.substr(5, 2);
            if (mounted)
                read_day(b.date, [&](const Row &r) {
                    if (r.epoch < day || r.epoch >= day + 86400)
                        return;
                    auto hour = unsigned((r.epoch - day) / 3600);
                    auto &h = hours[hour];
                    b.e += r.e;
                    b.x += r.x;
                    ++b.samples;
                    b.covered += std::min<unsigned>(r.covered, r.epoch - day);
                    b.gaps += r.gap;
                    b.resets += !strcmp(r.kind, "reset");
                    h.e += r.e;
                    h.x += r.x;
                    ++h.samples;
                    if (!first || r.epoch < first)
                        first = r.epoch;
                    if (!strcmp(r.kind, "movement")) {
                        history.push_back(r);
                        if (history.size() > 100)
                            history.pop_front();
                    }
                });
            e += b.e;
            x += b.x;
            samples += b.samples;
            covered += b.covered;
            gaps += b.gaps;
            resets += b.resets;
            days.push_back(b);
        }
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "start", start.c_str());
    cJSON_AddStringToObject(j, "end", end.c_str());
    count(j, "entries", e);
    count(j, "exits", x);
    cJSON_AddBoolToObject(j, "has_samples", samples > 0);
    cJSON_AddNumberToObject(j, "sample_count", samples);
    cJSON_AddNumberToObject(j, "covered_seconds", covered);
    cJSON_AddNumberToObject(j, "gaps", gaps);
    cJSON_AddNumberToObject(j, "resets", resets);
    if (first)
        cJSON_AddStringToObject(j, "started_at", park_timestamp(first).c_str());
    else
        cJSON_AddNullToObject(j, "started_at");
    auto daily = cJSON_AddArrayToObject(j, "daily");
    for (const auto &b : days) cJSON_AddItemToArray(daily, bucket_json(b));
    auto chart = cJSON_AddArrayToObject(j, "chart");
    for (const auto &b : (start == end ? hours : days)) cJSON_AddItemToArray(chart, bucket_json(b));
    auto list = cJSON_AddArrayToObject(j, "history");
    for (auto i = history.rbegin(); i != history.rend(); ++i) {
        const auto &r = *i;
        auto item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "at", park_timestamp(r.epoch).c_str());
        count(item, "entries", r.e);
        count(item, "exits", r.x);
        cJSON_AddStringToObject(item, "kind", r.kind);
        cJSON_AddBoolToObject(item, "gap", r.gap);
        cJSON_AddBoolToObject(item, "storage_ok", r.storage);
        cJSON_AddItemToArray(list, item);
    }
    char *serialized = cJSON_PrintUnformatted(j);
    if (serialized) {
        cached_report = serialized;
        cJSON_free(serialized);
        cached_start = start;
        cached_end = end;
        cached_at = esp_timer_get_time();
    }
    if (mutex) xSemaphoreGive(mutex);
    return j;
}
void park_history_cloud_protect()
{
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    cloud_protected = true;
    xSemaphoreGive(mutex);
}
ParkCloudBatch park_history_cloud_batch(unsigned timeout_ms)
{
    ParkCloudBatch batch;
    if (!mutex) return batch;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) { batch.error="sd_busy"; return batch; }
    auto *directory = mounted ? opendir(HISTORY_DIR) : nullptr;
    std::vector<std::string> files;
    if (directory) {
        while (auto *entry = readdir(directory)) {
            std::string name(entry->d_name);
            if (name.size() == 14 && name.substr(10) == ".csv" && day_epoch(name.substr(0,10)) >= 0)
                files.push_back(name);
        }
        closedir(directory);
        std::sort(files.begin(), files.end());
        batch.ok = true;
        batch.error = "ready";
    }
    for (const auto &name : files) {
        const auto path = std::string(HISTORY_DIR) + "/" + name;
        struct stat st = {};
        if (stat(path.c_str(), &st) != 0) { batch.ok=false; batch.error="sd_stat_failed"; break; }
        long offset = cloud_cursor(name, st.st_size);
        if (offset == st.st_size) continue;
        FILE *f = fopen(path.c_str(), "r");
        if (!f) { batch.ok=false; batch.error="sd_open_failed"; break; }
        char line[384];
        if (!fgets(line,sizeof(line),f) || strcmp(line,HEADER)) { fclose(f);batch.ok=false;batch.error="sd_header_mismatch";break; }
        batch.file=name; batch.from=offset;
        if (offset && fseek(f,offset,SEEK_SET)!=0) { fclose(f);batch.ok=false;batch.error="sd_seek_failed";break; }
        batch.records=cJSON_CreateArray();
        while (cJSON_GetArraySize(batch.records)<park::settings::batch_records && fgets(line,sizeof(line),f)) {
            // A partial or corrupt row must never be acknowledged/skipped silently.
            if (!strchr(line,'\n')) { batch.ok=false;batch.error="sd_partial_row"; break; }
            unsigned char digest[32];
            if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(line),strlen(line),digest,0)!=0) {batch.ok=false;break;}
            char identity[65];
            for (unsigned i=0;i<32;++i) snprintf(identity+i*2,3,"%02x",digest[i]);
            Row row;
            if (!decode(line,row) || row.epoch < 1704067200) { batch.ok=false;batch.error="sd_invalid_row";break; }
            auto item=cJSON_CreateObject();
            cJSON_AddStringToObject(item,"id",identity);
            cJSON_AddNumberToObject(item,"epoch",row.epoch);
            count(item,"entries",row.e);count(item,"exits",row.x);
            cJSON_AddStringToObject(item,"kind",row.kind);
            cJSON_AddItemToArray(batch.records,item);
        }
        batch.to=ftell(f);
        if (ferror(f) || batch.to < 0) {batch.ok=false;batch.error="sd_read_failed";}
        fclose(f);
        break;
    }
    xSemaphoreGive(mutex);
    return batch;
}
bool park_history_cloud_ack(const ParkCloudBatch &batch)
{
    if (!mutex || !batch.ok || batch.file.empty() || batch.to < batch.from) return false;
    xSemaphoreTake(mutex,portMAX_DELAY);
    const auto source=std::string(HISTORY_DIR)+"/"+batch.file;
    struct stat st = {};
    bool ok=stat(source.c_str(),&st)==0 && st.st_size>=batch.to && cloud_cursor(batch.file,st.st_size)==batch.from;
    if (ok) {
        ok=mkdir(CURSOR_DIR,0775)==0 || (stat(CURSOR_DIR,&st)==0 && S_ISDIR(st.st_mode));
        const auto path=std::string(CURSOR_DIR)+"/"+batch.file+".ack";
        FILE *f=ok?fopen(path.c_str(),"w"):nullptr;
        if (!f) ok=false;
        else {
            const auto seal=batch.file+":"+std::to_string(batch.to);
            ok=fprintf(f,"%ld,%08" PRIx32 "\n",batch.to,checksum(seal.c_str()))>0;
            if (fflush(f)!=0 || fsync(fileno(f))!=0) ok=false;
            if (fclose(f)!=0) ok=false;
        }
    }
    xSemaphoreGive(mutex);
    return ok;
}
std::string park_history_csv(const std::string &start, const std::string &end)
{
    auto report = park_history_report(start, end);
    std::string csv = "data,entradas,saidas,tempo_coleta_segundos,registros,interrupcoes,reinicios\r\n";
    cJSON *day = nullptr;
    cJSON_ArrayForEach(day, cJSON_GetObjectItem(report, "daily"))
    {
        const bool has_samples = cJSON_GetNumberValue(cJSON_GetObjectItem(day, "samples")) > 0;
        char b[256];
        snprintf(b,
                 sizeof(b),
                 "%s,%s,%s,%.0f,%.0f,%.0f,%.0f\r\n",
                 cJSON_GetStringValue(cJSON_GetObjectItem(day, "date")),
                 has_samples ? cJSON_GetStringValue(cJSON_GetObjectItem(day, "entries")) : "",
                 has_samples ? cJSON_GetStringValue(cJSON_GetObjectItem(day, "exits")) : "",
                 cJSON_GetNumberValue(cJSON_GetObjectItem(day, "covered_seconds")),
                 cJSON_GetNumberValue(cJSON_GetObjectItem(day, "samples")),
                 cJSON_GetNumberValue(cJSON_GetObjectItem(day, "gaps")),
                 cJSON_GetNumberValue(cJSON_GetObjectItem(day, "resets")));
        csv += b;
    }
    cJSON_Delete(report);
    return csv;
}

cJSON *park_history_cleanup(const std::string &before, bool erase, const std::string &plan)
{
    auto result = cJSON_CreateObject();
    const auto today = park_today();
    if (!mutex || today.empty() || day_epoch(before) < 0 || date_at(day_epoch(before)) != before || before > today) {
        cJSON_AddStringToObject(result, "error", "Escolha uma data válida, até hoje. Os registros de hoje são preservados.");
        return result;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    struct Candidate { std::string name; uint64_t bytes; };
    std::vector<Candidate> files;
    uint64_t bytes = 0;
    std::string failure;
    auto *directory = mounted ? opendir(HISTORY_DIR) : nullptr;
    if (!directory) failure = "Não foi possível consultar o microSD. Nenhuma limpeza iniciada.";
    if (directory) {
        errno = 0;
        while (auto entry = readdir(directory)) {
            std::string name = entry->d_name;
            // Only the owned dated journal format; never traverse or recurse.
            if (name.size() != 14 || name.substr(10) != ".csv") continue;
            const auto date = name.substr(0, 10);
            if (day_epoch(date) < 0 || date_at(day_epoch(date)) != date || date >= before || date >= today) continue;
            const auto path = std::string(HISTORY_DIR) + "/" + name;
            struct stat st = {};
              if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                failure = "Falha ao consultar um arquivo. Nenhuma limpeza iniciada.";
                break;
            }
              if (cloud_protected && cloud_cursor(name, st.st_size) < st.st_size) {
                  failure = "Há registros ainda não sincronizados com a nuvem. Aguarde antes de limpar.";
                  break;
              }
            // A matching name is insufficient: verify our journal header too.
            FILE *f = fopen(path.c_str(), "r");
            char header[256] = {};
            const bool owned = f && fgets(header, sizeof(header), f) && strcmp(header, HEADER) == 0;
            if (f) fclose(f);
            if (!owned) {
                failure = "Arquivo não reconhecido no período. Nenhuma limpeza iniciada.";
                break;
            }
            files.push_back({name, static_cast<uint64_t>(st.st_size)});
            bytes += st.st_size;
            errno = 0;
        }
        if (errno && failure.empty()) failure = "Falha ao listar o cartão. Nenhuma limpeza iniciada.";
        closedir(directory);
    }
    std::sort(files.begin(), files.end(), [](const Candidate &a, const Candidate &b) { return a.name < b.name; });
    std::string identity = before;
    for (const auto &file : files) identity += "/" + file.name + ":" + std::to_string(file.bytes);
    char token[16];
    snprintf(token, sizeof(token), "%08" PRIx32, checksum(identity.c_str()));
    unsigned removed = 0;
    if (erase && failure.empty() && plan != token) failure = "A seleção mudou. Consulte os arquivos novamente antes de confirmar.";
    if (erase && failure.empty()) {
        for (const auto &file : files) {
            const auto path = std::string(HISTORY_DIR) + "/" + file.name;
            if (unlink(path.c_str()) != 0) {
                failure = "Limpeza interrompida por erro no cartão. Arquivos removidos: " + std::to_string(removed) + ".";
                break;
            }
            ++removed;
        }
        cached_report.clear();
    }
    cJSON_AddNumberToObject(result, "files", files.size());
    count(result, "bytes", bytes);
    cJSON_AddStringToObject(result, "plan", token);
    cJSON_AddStringToObject(result, "before", before.c_str());
    cJSON_AddNumberToObject(result, "removed", removed);
    if (!failure.empty()) cJSON_AddStringToObject(result, "error", failure.c_str());
    xSemaphoreGive(mutex);
    return result;
}
