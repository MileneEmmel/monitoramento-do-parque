// Checks the tracking and counting scenarios during startup

#include "counting_core.hpp"
#include <memory>

namespace park {
    // Runs deterministic synthetic observations without camera, network, or storage
    // Each scenario returns false on the first broken expectation, visitor_counter_start uses that to disable counting
    bool self_test() {
        // Test helper: advance simulated time by 100 ms, submit one observation, and return its assigned trajectory ID
        auto move = [](Counter &counter, float x, int64_t &simulated_time_us, float y = .5f) {
            Observation observation{x, y};
            unsigned track_ids[capacity] = {};
            simulated_time_us           += 100000;
            counter.update(&observation, 1, simulated_time_us, track_ids);

            return track_ids[0];
        };

        // Reuse one heap instance: app_main has a small stack on ESP32
        std::unique_ptr<Counter> counter_instance(new Counter);
        Counter &counter          = *counter_instance;
        int64_t simulated_time_us = 0;

        // 1. A stationary person retains its ID, counts once after crossing, and counts the reverse passage on return
        unsigned track_id = move(counter, .2f, simulated_time_us);

        if(move(counter, .2f, simulated_time_us) != track_id)
            return false;

        for(float x : {.35f, .45f, .55f, .7f, .7f})
            move(counter, x, simulated_time_us);

        if(counter.entries != 1 || counter.exits)
            return false;

        for(int i = 0; i < 10; ++i)
            move(counter, .7f, simulated_time_us);

        if(counter.entries != 1)
            return false;

        for(float x : {.55f, .45f, .3f, .3f})
            move(counter, x, simulated_time_us);

        if(counter.entries != 1 || counter.exits != 1)
            return false;

        counter.reset();

        // 2. Turning around inside the middle band must not produce a completed crossing
        simulated_time_us = 0;

        for(float x : {.2f, .2f, .35f, .45f, .5f, .4f, .3f, .3f})
            move(counter, x, simulated_time_us);

        if(counter.entries || counter.exits)
            return false;

        counter.reset();

        // 3. A trajectory first observed in the middle band has no known origin, so leaving the band alone is not a passage
        simulated_time_us = 0;

        for(float x : {.5f, .5f, .65f, .7f, .7f})
            move(counter, x, simulated_time_us);

        if(counter.entries || counter.exits)
            return false;

        counter.reset();

        // 4. Small oscillations near a line must not manufacture a crossing through the tolerance band
        simulated_time_us = 0;

        for(float x : {.2f, .2f, .4f, .45f, .55f, .61f, .59f, .612f, .59f, .6f})
            move(counter, x, simulated_time_us);

        if(counter.entries || counter.exits)
            return false;

        counter.reset();

        // 5. A gap beyond the track lifetime prevents combining an old person with a newly observed trajectory
        simulated_time_us = 0;
        move(counter, .2f, simulated_time_us);
        move(counter, .2f, simulated_time_us);
        simulated_time_us += 2100000;

        for(float x : {.7f, .7f, .7f})
            move(counter, x, simulated_time_us);

        if(counter.entries || counter.exits)
            return false;

        counter.reset();

        // 6. Calibration can reverse the meaning of the same movement without changing the tracking path
        counter.config.reverse = true;
        simulated_time_us      = 0;

        for(float x : {.2f, .2f, .35f, .5f, .65f, .7f})
            move(counter, x, simulated_time_us);

        if(counter.entries || counter.exits != 1)
            return false;

        counter.reset();

        // 7. The same crossing logic must work on the y axis when the lines are horizontal
        counter.config.horizontal = true;
        simulated_time_us         = 0;

        for(float y : {.2f, .2f, .35f, .5f, .65f, .7f})
            move(counter, .5f, simulated_time_us, y);

        if(counter.entries != 1 || counter.exits)
            return false;

        counter.reset();

        // 8. Two simultaneous people must receive different IDs and contribute two passages
        simulated_time_us            = 0;
        unsigned track_ids[capacity] = {};

        for(float x : {.2f, .2f, .35f, .5f, .65f, .7f}) {
            Observation observations[] = {{x, .2f}, {x, .8f}};
            simulated_time_us         += 100000;
            counter.update(observations, 2, simulated_time_us, track_ids);

            if(!track_ids[0] || track_ids[0] == track_ids[1])
                return false;
        }

        if(counter.entries != 2 || counter.exits)
            return false;

        // 9. A brief occlusion preserves identity and allows the crossing to complete
        counter.reset();
        simulated_time_us = 0;
        track_id          = move(counter, .2f, simulated_time_us);
        move(counter, .2f, simulated_time_us);
        simulated_time_us += 300000;
        counter.update(nullptr, 0, simulated_time_us, track_ids);

        if(move(counter, .35f, simulated_time_us) != track_id)
            return false;

        for(float x : {.5f, .65f, .7f})
            move(counter, x, simulated_time_us);

        if(counter.entries != 1)
            return false;

        counter.reset();

        // 10. An implausibly large position jump starts another trajectory rather than creating a false crossing
        simulated_time_us = 0;
        move(counter, .1f, simulated_time_us);
        move(counter, .1f, simulated_time_us);
        move(counter, .9f, simulated_time_us);
        move(counter, .9f, simulated_time_us);

        if(counter.entries || counter.exits)
            return false;

        // 11. Regression: a normal crossing no longer needs to pause at either end
        counter.reset();
        simulated_time_us = 0;
        track_id          = move(counter, .3f, simulated_time_us);
        move(counter, .5f, simulated_time_us);
        move(counter, .7f, simulated_time_us);

        if(counter.entries != 1 || counter.state(track_id, simulated_time_us) != 5)
            return false;

        move(counter, .5f, simulated_time_us);
        move(counter, .3f, simulated_time_us);

        if(counter.exits != 1 || counter.state(track_id, simulated_time_us) != 6)
            return false;

        // 12. Regression: a 1.3 s detection dropout used to discard a valid trajectory
        counter.reset();
        simulated_time_us = 0;
        track_id          = move(counter, .3f, simulated_time_us);
        move(counter, .5f, simulated_time_us);
        simulated_time_us += 1300000;
        counter.update(nullptr, 0, simulated_time_us, track_ids);

        if(move(counter, .7f, simulated_time_us) != track_id || counter.entries != 1 || counter.resumed != 1)
            return false;

        // 13. A track born in the band is explicitly unarmed, and disappearing never counts
        counter.reset();
        simulated_time_us = 0;
        track_id          = move(counter, .5f, simulated_time_us);

        if(counter.state(track_id, simulated_time_us) != 0)
            return false;

        simulated_time_us += 2100000;
        counter.update(nullptr, 0, simulated_time_us, track_ids);

        if(counter.entries || counter.exits || counter.expired != 1)
            return false;

        // 14. Lost crossing is explained without inventing a completed passage
        counter.reset();
        simulated_time_us = 0;
        move(counter, .3f, simulated_time_us);
        move(counter, .5f, simulated_time_us);
        simulated_time_us += 2100000;
        counter.update(nullptr, 0, simulated_time_us, track_ids);

        if(counter.entries || counter.discarded != 1)
            return false;

        // 15. Simultaneous opposite movement on distinct paths must keep IDs exclusive
        counter.reset();
        simulated_time_us = 0;

        for(float x : {.2f, .35f, .5f, .65f, .8f}) {
            Observation observations[] = {{x, .2f}, {1 - x, .8f}};
            simulated_time_us         += 100000;
            counter.update(observations, 2, simulated_time_us, track_ids);

            if(!track_ids[0] || track_ids[0] == track_ids[1])
                return false;
        }

        if(counter.entries != 1 || counter.exits != 1)
            return false;

        counter.clear_tracks();

        if(counter.entries != 1 || counter.exits != 1)
            return false;

        // 16. Regression: greedy nearest-pair picks track B for observation A, despite there being a better one-to-one assignment for the complete frame
        counter.reset();
        simulated_time_us     = 100000;
        Observation initial[] = {{.45f, .5f, .12f, .45f}, {.60f, .5f, .22f, .6f}};
        counter.update(initial, 2, simulated_time_us, track_ids);
        unsigned first_track_id  = track_ids[0];
        unsigned second_track_id = track_ids[1];
        Observation advanced[]   = {{.58f, .5f, .12f, .45f}, {.74f, .5f, .22f, .6f}};
        counter.update(advanced, 2, simulated_time_us += 100000, track_ids);

        if(track_ids[0] != first_track_id || track_ids[1] != second_track_id)
            return false;

        // 17. Two close, partially overlapping boxes: both entry and exit must count, independent of the score/order in which the detector returns the boxes
        counter.reset();
        simulated_time_us = 0;
        first_track_id    = 0;
        second_track_id   = 0;

        for(float x : {.2f, .3f, .4f, .5f, .6f, .7f, .6f, .5f, .4f, .3f, .2f}) {
            Observation observations[] = {{x, .5f, .16f, .55f}, {x + .04f, .52f, .13f, .48f}};
            const bool swapped         = (simulated_time_us / 100000) % 2;

            if(swapped)
                std::swap(observations[0], observations[1]);

            counter.update(observations, 2, simulated_time_us += 100000, track_ids);
            unsigned first_observed_id  = track_ids[swapped ? 1:0];
            unsigned second_observed_id = track_ids[swapped ? 0:1];

            if(!first_track_id) {
                first_track_id  = first_observed_id;
                second_track_id = second_observed_id;
            }

            if(first_observed_id != first_track_id || second_observed_id != second_track_id || first_track_id == second_track_id)
                return false;
        }

        if(counter.entries != 2 || counter.exits != 2 || counter.created != 2)
            return false;

        // 18. One member of a close pair disappears briefly on exit, then comes back
        counter.reset();
        simulated_time_us = 0;
        first_track_id    = 0;
        second_track_id   = 0;

        for(int step = 0; step < 7; ++step) {
            float x                    = .8f - step * .1f;
            Observation observations[] = {{x, .5f, .16f, .55f}, {x + .04f, .52f, .13f, .48f}};
            int observation_count      = step == 2 ? 1:2;
            counter.update(observations, observation_count, simulated_time_us += 100000, track_ids);

            if(!first_track_id) {
                first_track_id  = track_ids[0];
                second_track_id = track_ids[1];
            }

            if(track_ids[0] != first_track_id || (observation_count == 2 && track_ids[1] != second_track_id))
                return false;
        }

        if(counter.entries || counter.exits != 2 || counter.created != 2)
            return false;

        // 19. Opposite movement with overlapping centers: size and velocity retain IDs
        counter.reset();
        simulated_time_us = 0;
        first_track_id    = 0;
        second_track_id   = 0;

        for(float x : {.2f, .3f, .4f, .5f, .6f, .7f, .8f}) {
            Observation observations[] = {{x, .5f, .12f, .45f}, {1 - x, .5f, .22f, .6f}};
            bool swapped               = (simulated_time_us / 100000) % 2;

            if(swapped)
                std::swap(observations[0], observations[1]);

            counter.update(observations, 2, simulated_time_us += 100000, track_ids);
            unsigned first_observed_id  = track_ids[swapped ? 1:0];
            unsigned second_observed_id = track_ids[swapped ? 0:1];

            if(!first_track_id) {
                first_track_id  = first_observed_id;
                second_track_id = second_observed_id;
            }

            if(first_observed_id != first_track_id || second_observed_id != second_track_id)
                return false;
        }

        if(counter.entries != 1 || counter.exits != 1 || counter.created != 2)
            return false;

        // 20. Rectangular assignment must leave impossible observations unmatched
        counter.reset();
        simulated_time_us = 100000;
        Observation small{.2f, .5f, .05f, .1f};
        counter.update(&small, 1, simulated_time_us, track_ids);
        first_track_id = track_ids[0];
        Observation huge{.7f, .5f, .8f, .9f};
        counter.update(&huge, 1, simulated_time_us += 100000, track_ids);

        if(track_ids[0] == first_track_id || counter.entries || counter.exits)
            return false;

        // 21. Exercise all 32 rows and 64 columns, including capacity exhaustion
        counter.reset();
        simulated_time_us = 100000;
        Observation crowd[capacity];

        for(int i = 0; i < capacity; ++i)
            crowd[i] = {.2f, .02f + i * .03f, .05f, .02f};

        counter.update(crowd, capacity, simulated_time_us, track_ids);

        for(int pass = 0; pass < 2; ++pass) {
            counter.update(crowd, capacity, simulated_time_us += 100000, track_ids);

            for(int i = 0; i < capacity; ++i) {
                if(!track_ids[i])
                    return false;

                for(int j = 0; j < i; ++j)
                    if(track_ids[i] == track_ids[j])
                        return false;
            }
        }

        Observation extra{.95f, .5f, .05f, .02f};
        counter.update(&extra, 1, simulated_time_us += 100000, track_ids);

        if(track_ids[0] || counter.created != capacity || counter.entries || counter.exits)
            return false;

        // 22. User reset clears totals and active trajectories, but preserves calibration and monotonically allocated IDs (old trajectories cannot complete later)
        counter.reset();
        simulated_time_us = 100000;
        counter.config    = {.3f, .4f, true, true};
        Observation before_reset{.5f, .2f, .12f, .4f};
        counter.update(&before_reset, 1, simulated_time_us, track_ids);
        first_track_id  = track_ids[0];
        counter.entries = 12;
        counter.exits   = 9;
        counter.reset_totals(simulated_time_us += 100000);

        if(counter.entries || counter.exits || counter.config.first != .3f || counter.config.second != .4f || !counter.config.horizontal || !counter.config.reverse || counter.events[(counter.event_sequence - 1) % 8].code != RESET_TOTALS)
            return false;

        Observation after_reset{.5f, .7f, .12f, .4f};
        counter.update(&after_reset, 1, simulated_time_us += 100000, track_ids);

        return track_ids[0] != first_track_id && counter.entries == 0 && counter.exits == 0;
    }
}
