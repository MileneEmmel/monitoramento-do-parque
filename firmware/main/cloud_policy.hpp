#pragma once
#include <cstdint>
#include "app_settings.hpp"
namespace park {
// New records never postpone the first collection deadline.
struct CloudSchedule {
    uint64_t collect_at=0, retry_at=0;
    unsigned failures=0;
    bool permanent=false;
    void pending(uint64_t now) { if(!collect_at) collect_at=now+settings::collect_ms; }
    bool due(uint64_t now,bool closing) const { return !permanent && (failures?now>=retry_at:closing || (collect_at && now>=collect_at)); }
    void success() { collect_at=retry_at=0; failures=0; permanent=false; }
    void failure(uint64_t now,bool closing,bool transient) {
        ++failures; permanent=!transient;
        retry_at=now+(closing && failures==1?settings::batch_gap_ms:settings::retry_ms);
    }
    bool exhausted(bool closing) const { return permanent || (closing && failures>=2); }
};
inline bool cloud_policy_test() {
    CloudSchedule s;
    s.pending(100); s.pending(9000);
    // Closing bypasses collection, but normal operation must not.
    if(s.collect_at!=10100 || s.due(10099,false) || !s.due(10100,false) || !s.due(101,true)) return false;
    s.failure(11000,true,true);
    if(s.exhausted(true) || s.due(12999,true) || !s.due(13000,true)) return false;
    s.failure(13000,true,true);
    if(!s.exhausted(true)) return false;
    s.success(); s.failure(1000,false,true);
    if(s.due(60999,false) || !s.due(61000,false)) return false;
    s.success(); s.failure(1000,false,false);
    return s.permanent && !s.due(999999,false) && s.exhausted(true);
}
}
