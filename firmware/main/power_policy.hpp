#pragma once
#include <cstdint>

namespace park {
// Pure policy: no hardware, storage or network calls. Times are milliseconds.
struct PowerPolicy {
    uint64_t activity = 0, frame = 0;
    bool high_seen = false, low_seen = false, validated = false, camera = false;
    const char *activity_source = "none";
    void note_activity(uint64_t now, const char *source) { activity = now; activity_source = source; }
    void pir(int level, uint64_t now) {
        if (level == 1) { high_seen = true; note_activity(now, "pir"); }
        if (level == 0) low_seen = true;
        validated = validated || (high_seen && low_seen);
    }
    void detection(bool people, uint64_t now) { frame = now; if (people) note_activity(now, "ai"); }
    const char *idle_block(uint64_t now, uint64_t boot_grace, uint64_t quiet) const {
        if (!validated) return "sensor_unvalidated";
        if (now < boot_grace) return "boot_grace";
        if (now < activity || now - activity < quiet) return "activity_window";
        if (camera && (!frame || now < frame || now - frame >= 5000)) return "inference_stalled";
        return "";
    }
    // Preview traffic never renews activity or vetoes sleep.
    bool idle(uint64_t now, uint64_t boot_grace, uint64_t quiet) const {
        return idle_block(now, boot_grace, quiet)[0] == '\0';
    }
};
inline bool power_policy_test() {
    PowerPolicy p;
    if (p.idle(120000, 90000, 30000)) return false;
    p.pir(0, 0); p.pir(1, 1000); p.pir(0, 2000);
    if (!p.validated || p.idle(40000, 90000, 30000)) return false;
    if (!p.idle(90000, 90000, 30000)) return false;
    p.camera = true;
    if (p.idle(90000, 90000, 30000)) return false;
    p.detection(false, 90000);
    if (!p.idle(90000, 90000, 30000)) return false;
    p.detection(true, 90000);
    if (p.idle(91000, 90000, 30000)) return false;
    p.detection(false, 120000);
    return p.idle(120000, 90000, 30000) && !p.idle(126000, 90000, 30000);
}
}
