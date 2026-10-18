// Defines cloud upload scheduling and its internal checks

#pragma once
#include <cstdint>
#include "app_settings.hpp"

namespace park {
    // New records never postpone the first collection deadline
    struct CloudSchedule {
        uint64_t collect_at = 0, retry_at = 0;
        unsigned failures   = 0;
        bool permanent      = false;

        // Starts collection when the first unsent change arrives, later changes do not move that deadline
        void pending(uint64_t now_ms) {
            if(!collect_at)
                collect_at = now_ms + settings::collect_ms;
        }

        // Permanent errors stop attempts. Failures obey retry_at, closing bypasses only the initial collection wait
        bool due(uint64_t now_ms, bool closing) const {
            if(permanent)
                return false;
            if(failures)
                return now_ms >= retry_at;
            return closing || (collect_at && now_ms >= collect_at);
        }

        // Resets collection and failure state after successful work, the caller may schedule the next batch explicitly
        void success() {
            collect_at = retry_at = 0;
            failures   = 0;
            permanent  = false;
        }

        // Classifies the failure and schedules the next attempt. Closing gets one short retry for a transient error
        void failure(uint64_t now_ms, bool closing, bool retryable) {
            ++failures;
            permanent = !retryable;
            retry_at  = now_ms + (closing && failures == 1 ? settings::batch_gap_ms:settings::retry_ms);
        }

        // Stops a closing cycle after two failures, or immediately for a permanent error
        bool exhausted(bool closing) const {
            return permanent || (closing && failures >= 2);
        }
    };

    // Checks collection timing, the bounded closing retry, normal retry delay, and permanent-error handling
    inline bool cloud_policy_test() {
        CloudSchedule schedule;

        // 1. New records preserve the first collection deadline, closing bypasses only the collection wait
        schedule.pending(100);
        schedule.pending(9000);

        if(schedule.collect_at != 10100 || schedule.due(10099, false) || !schedule.due(10100, false) || !schedule.due(101, true))
            return false;

        // 2. The first retryable closing failure uses the short retry interval
        schedule.failure(11000, true, true);
        if(schedule.exhausted(true) || schedule.due(12999, true) || !schedule.due(13000, true))
            return false;

        // 3. A second closing failure exhausts the allowed attempts
        schedule.failure(13000, true, true);
        if(!schedule.exhausted(true))
            return false;

        // 4. A normal retryable failure uses the regular retry interval
        schedule.success();
        schedule.failure(1000, false, true);
        if(schedule.due(60999, false) || !schedule.due(61000, false))
            return false;

        // 5. A permanent failure prevents further attempts
        schedule.success();
        schedule.failure(1000, false, false);
        return schedule.permanent && !schedule.due(999999, false) && schedule.exhausted(true);
    }
}
