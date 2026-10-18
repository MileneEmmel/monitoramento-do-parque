// Defines activity and inactivity rules for sleep and their internal checks

#pragma once
#include <cstdint>

namespace park {
    // Pure policy: no hardware, storage or network calls. Times are milliseconds
    struct PowerPolicy {
        uint64_t activity           = 0, frame = 0;
        bool high_seen              = false, low_seen = false, validated = false, camera = false;
        const char *activity_source = "none";

        // Records the latest monotonic activity time and its source, callers decide what qualifies as activity
        void note_activity(uint64_t now_ms, const char *source) {
            activity        = now_ms;
            activity_source = source;
        }

        // A high level renews activity on every check, not just on a rising edge
        // validated is diagnostic evidence of both levels being observed, it is not a prerequisite in idle_block
        void pir(int pir_level, uint64_t now_ms) {
            if(pir_level == 1) {
                high_seen = true;
                note_activity(now_ms, "pir");
            }
            if(pir_level == 0)
                low_seen = true;
            validated = validated || (high_seen && low_seen);
        }

        // Every detection result updates frame health, only frames containing people renew the activity window
        void detection(bool people_detected, uint64_t now_ms) {
            frame = now_ms;
            if(people_detected)
                note_activity(now_ms, "ai");
        }

        // Returns the first time-based reason preventing sleep, or an empty string
        // boot_grace_ms is an absolute time since boot, quiet_duration_ms is a duration since the latest activity
        const char *idle_block(uint64_t now_ms, uint64_t boot_grace_ms, uint64_t quiet_duration_ms) const {
            if(now_ms < boot_grace_ms)
                return "boot_grace";
            if(now_ms < activity || now_ms - activity < quiet_duration_ms)
                return "activity_window";
            return "";
        }

        // Preview traffic never renews activity or vetoes sleep
        // Convenience check for time-based eligibility. GPIO level and operating mode are checked by park_power
        bool idle(uint64_t now_ms, uint64_t boot_grace_ms, uint64_t quiet_duration_ms) const {
            return idle_block(now_ms, boot_grace_ms, quiet_duration_ms)[0] == '\0';
        }
    };

    // Checks boot grace, PIR diagnostics, and the distinction between empty frames and people activity
    inline bool power_policy_test() {
        PowerPolicy policy;

        // 1. Respect cold-boot grace without requiring a first visitor, PIR is checked separately
        if(!policy.idle(120000, 120000, 30000))
            return false;
        if(policy.idle(119999, 120000, 30000))
            return false;

        // 2. Record both PIR levels and respect the startup grace deadline
        policy.pir(0, 0);
        policy.pir(1, 1000);
        policy.pir(0, 2000);

        if(!policy.validated || policy.idle(40000, 90000, 30000))
            return false;
        if(!policy.idle(90000, 90000, 30000))
            return false;

        // 3. Starting the camera does not prevent sleep after inactivity
        policy.camera = true;
        if(!policy.idle(90000, 90000, 30000))
            return false;

        // 4. Empty frames do not renew activity
        policy.detection(false, 90000);
        if(!policy.idle(90000, 90000, 30000))
            return false;

        // 5. Detecting people renews activity and postpones sleep
        policy.detection(true, 90000);
        if(policy.idle(91000, 90000, 30000))
            return false;

        // 6. Continued empty frames allow sleep once the inactivity interval expires
        policy.detection(false, 120000);
        return policy.idle(120000, 90000, 30000) && policy.idle(126000, 90000, 30000);
    }
}
