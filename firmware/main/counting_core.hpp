// Tracks people and counts passages across the virtual lines

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Coordinates are normalized to the image. No camera, network or ESP dependencies
namespace park {
    // Maximum number of simultaneous trajectories and accepted observations per frame
    constexpr int capacity = 32;

    // Line positions use image fractions from 0 to 1. horizontal chooses the y axis, reverse swaps entry/exit
    struct Config {
        float first     = .40f, second = .60f;
        bool horizontal = false, reverse = false;
    };

    // One detection expressed as center coordinates and bounding-box dimensions, normalized to image size
    struct Observation {
        float x = 0, y = 0, width = 0, height = 0;
    };

    // A trajectory persists between frames
    // Velocity is normalized image distance per second
    // origin and zone use -1 for side A, 0 for the middle band, and +1 for side B
    // seen and counted_at are monotonic microseconds, hits counts observations assigned to this trajectory
    struct Track {
        bool active  = false;
        unsigned id  = 0, hits = 0;
        float x      = 0, y = 0, vx = 0, vy = 0;
        float width  = 0, height = 0;
        int origin   = 0, zone = 0, counted_state = 0;
        int64_t seen = 0, counted_at = -2000000;
    };

    // Diagnostic reasons describe track creation, confirmed passages, loss, capacity limits, and resets
    enum EventCode {
        NEW_A = 1, NEW_B, NEW_MIDDLE,
        ENTRY, EXIT, RESUMED,
        LOST_A, LOST_B, LOST_MIDDLE, LOST_CROSSING,
        FULL, RESET_TOTALS
    };

    // One diagnostic event: sequence orders the ring, id identifies the trajectory, and at is monotonic
    // microseconds
    struct Event {
        unsigned sequence = 0, id = 0;
        int code          = 0;
        int64_t at        = 0;
    };

    // Owns tracking state and reusable matching memory. The ESP wrapper provides synchronization around access
    class Counter {
    public:
        Config config;
        uint64_t entries = 0, exits = 0;
        unsigned created = 0, expired = 0, resumed = 0, discarded = 0, frames = 0, empty_frames = 0;
        Event events[8];
        unsigned event_sequence = 0;

        // Drops active trajectories while preserving totals, calibration, diagnostics, and the next ID
        void clear_tracks() {
            for(auto &track : tracks)
                track = Track{};
        }

        // Clears totals and trajectories, but preserves calibration and ID allocation. Records the reset as an
        // event
        void reset_totals(int64_t now) {
            entries = exits = 0;
            clear_tracks();
            emit(0, RESET_TOTALS, now);
        }

        // Resets all counter state, including calibration and diagnostics, used to isolate self-test scenarios
        void reset() {
            clear_tracks();
            config  = Config{};
            entries = exits = 0;
            next_id = 0;
            created = expired = resumed = discarded = frames = empty_frames = event_sequence = 0;
            for(auto &e : events)
                e = Event{};
        }

        // States: 0=born in band, 1/2=armed from A/B, 3/4=crossing,
        // 5/6=entry/exit just counted. Only observations get an on-screen state
        // Converts a live track into a compact display state, a recently counted direction remains visible for one second
        int state(unsigned id, int64_t now) const {
            for(const auto &track : tracks)
                if(track.active && track.id == id) {
                    if(now - track.counted_at < 1000000)
                        return track.counted_state;
                    if(!track.origin)
                        return 0;
                    if(track.origin == -1)
                        return track.zone == 0 ? 3:1;

                    return track.zone == 0 ? 4:2;
                }
            return 0;
        }

        // Processes one frame of normalized observations. now is monotonic microseconds
        // ids follows observation order, zero means no track could be assigned. This method performs no I/O
        void update(const Observation *observations, int observation_count, int64_t now, unsigned *ids) {
            // 1. Prepare the frame outputs and expire trajectories missing for more than two seconds
            observation_count = std::min(observation_count, capacity);
            ++frames;
            if(!observation_count)
                ++empty_frames;

            bool observation_matched[capacity] = {};
            for(int i = 0; i < observation_count; ++i)
                ids[i] = 0;

            // Expire tracks missing for more than two seconds. Losing a track never invents an entry or exit
            for(auto &track : tracks) {
                if(track.active && now - track.seen > 2000000) {
                    ++expired;
                    bool crossing = track.origin && !track.zone;
                    if(crossing)
                        ++discarded;

                    int lost_event = LOST_MIDDLE;
                    if(crossing)
                        lost_event = LOST_CROSSING;
                    else if(track.origin == -1)
                        lost_event = LOST_A;
                    else if(track.origin == 1)
                        lost_event = LOST_B;

                    emit(track.id, lost_event, now);
                    track = Track{};
                }
            }

            // 2. Build the costs for assigning current observations to active trajectories
            // Solve the whole frame together to avoid giving the second person an incorrect track
            int rows = 0;
            for(int j = 0; j < capacity; ++j)
                if(tracks[j].active)
                    work.track_rows[rows++] = j;

            if(rows && observation_count) {
                const int columns = observation_count + rows; // dummy columns explicitly allow no match
                for(int r = 0; r < rows; ++r) {
                    for(int i = 0; i < observation_count; ++i)
                        work.cost[r][i] = association_cost(tracks[work.track_rows[r]], observations[i], now);
                    for(int i = observation_count; i < columns; ++i)
                        work.cost[r][i] = unmatched_cost;
                }
                // 3. Find and apply one-to-one matches for existing trajectories
                // Dummy columns let a trajectory remain unmatched instead of forcing a bad association
                assign(rows, columns);
                // Apply only real observation matches whose cost beats the unmatched alternative
                for(int column = 1; column <= observation_count; ++column) {
                    const int row = work.owner[column] - 1, observation_index = column - 1;
                    if(row >= 0 && work.cost[row][observation_index] < unmatched_cost) {
                        auto &track                            = tracks[work.track_rows[row]];
                        observation_matched[observation_index] = true;
                        observe(track, observations[observation_index], now);
                        ids[observation_index] = track.id;
                    }
                }
            }

            // 4. Start trajectories for remaining observations, or report that all track slots are occupied
            for(int i = 0; i < observation_count; ++i)
                if(!observation_matched[i]) {
                    for(auto &track : tracks)
                        if(!track.active) {
                            track        = Track{};
                            track.active = true;
                            track.id     = ++next_id;
                            ++created;
                            observe(track, observations[i], now);
                            ids[i]        = track.id;
                            int new_event = NEW_MIDDLE;
                            if(track.origin == -1)
                                new_event = NEW_A;
                            else if(track.origin == 1)
                                new_event = NEW_B;

                            emit(track.id, new_event, now);
                            break;
                        }
                    if(!ids[i])
                        emit(0, FULL, now);
                }
        }

    private:
        static constexpr float unmatched_cost = 1.5f;

        // Workspace is owned by the counter (not the small ESP task stack), reused at every frame
        // Bound: 32 tracks x (32 observations + 32 dummy columns)
        struct Workspace {
            float cost[capacity][capacity * 2];
            float u[capacity + 1], v[capacity * 2 + 1], distance[capacity * 2 + 1];
            int owner[capacity * 2 + 1], previous[capacity * 2 + 1], track_rows[capacity];
            bool visited[capacity * 2 + 1];
        } work;

        Track tracks[capacity];
        unsigned next_id = 0;

        // Scores whether an observation belongs to an existing track: lower is better
        // Distance, predicted position, box shape, overlap, direction, and age discourage identity swaps
        static float association_cost(const Track &track, const Observation &observation, int64_t now) {
            const float elapsed_seconds          = (now - track.seen) / 1000000.f;
            const float gate                     = std::min(.40f, .30f + .10f * elapsed_seconds);
            float dx                             = observation.x - track.x, dy = observation.y - track.y;
            const float current_distance_squared = dx * dx + dy * dy;

            if(current_distance_squared > gate * gate)
                return 1000.f;

            // Limit prediction time so a long detection gap does not project an old velocity too far
            const float prediction                 = std::min(elapsed_seconds, .35f);
            const float px                         = track.x + track.vx * prediction, py = track.y + track.vy * prediction;
            dx                                     = observation.x - px;
            dy                                     = observation.y - py;
            const float predicted_distance_squared = dx * dx + dy * dy;
            float cost                             = (.25f * current_distance_squared + .45f * predicted_distance_squared) / (gate * gate);

            // Compare box size and intersection-over-union (IoU) when dimensions are available
            if(track.width > 0 && track.height > 0 && observation.width > 0 && observation.height > 0) {
                const float width_ratio  = std::max(track.width, observation.width) / std::min(track.width, observation.width);
                const float height_ratio = std::max(track.height, observation.height) / std::min(track.height, observation.height);

                if(width_ratio > 4.f || height_ratio > 4.f)
                    return 1000.f;

                float intersection_w = std::max(0.f, std::min(px + track.width / 2, observation.x + observation.width / 2) - std::max(px - track.width / 2, observation.x - observation.width / 2));
                float intersection_h = std::max(0.f, std::min(py + track.height / 2, observation.y + observation.height / 2) - std::max(py - track.height / 2, observation.y - observation.height / 2));

                const float intersection = intersection_w * intersection_h;
                const float iou          = intersection / (track.width * track.height + observation.width * observation.height - intersection);
                const float shape        = .5f * (1 - 1 / width_ratio + 1 - 1 / height_ratio);
                cost                    += .20f * (1 - iou) + .20f * shape;
            }
            const float speed2 = track.vx * track.vx + track.vy * track.vy;

            if(track.hits >= 3 && elapsed_seconds < .6f && speed2 > .01f && current_distance_squared > .0004f) {
                const float alignment = ((observation.x - track.x) * track.vx + (observation.y - track.y) * track.vy) / std::sqrt(speed2 * current_distance_squared);
                cost                 += .10f * std::max(0.f, -alignment);
            }
            // A stale track should not steal a fresh person's box just by proximity
            return cost + .04f * std::min(elapsed_seconds / 2.f, 1.f);
        }

        // Finds the lowest-cost one-to-one assignment between track rows and observation/dummy columns
        // The workspace uses one-based indices, column zero starts each search for an augmenting path
        void assign(int rows, int columns) {
            // Rectangular Hungarian assignment: minimum total cost, exclusive IDs
            std::fill(work.u, work.u + rows + 1, 0.f);
            std::fill(work.v, work.v + columns + 1, 0.f);
            std::fill(work.owner, work.owner + columns + 1, 0);

            // Add one trajectory at a time, searching for a free column while adjusting the row/column cost potentials
            for(int row = 1; row <= rows; ++row) {
                work.owner[0] = row;
                std::fill(work.distance, work.distance + columns + 1, 1000000.f);
                std::fill(work.visited, work.visited + columns + 1, false);
                int column = 0;
                do {
                    work.visited[column] = true;
                    const int active_row = work.owner[column];
                    float delta          = 1000000.f;
                    int next             = 0;

                    for(int j = 1; j <= columns; ++j)
                        if(!work.visited[j]) {
                            const float cost = work.cost[active_row - 1][j - 1] - work.u[active_row] - work.v[j];
                            if(cost < work.distance[j]) {
                                work.distance[j] = cost;
                                work.previous[j] = column;
                            }
                            if(work.distance[j] < delta) {
                                delta = work.distance[j];
                                next  = j;
                            }
                        }

                    for(int j = 0; j <= columns; ++j) {
                        if(work.visited[j]) {
                            work.u[work.owner[j]] += delta;
                            work.v[j]             -= delta;
                        } else
                            work.distance[j] -= delta;
                    }

                    column = next;
                } while(work.owner[column]);
                do {
                    const int previous = work.previous[column];
                    // Follow predecessor links backward to update the assignment along the selected augmenting path
                    work.owner[column] = work.owner[previous];
                    column             = previous;
                } while(column);
            }
        }

        // Appends a diagnostic event to an eight-slot ring, sequence numbers preserve order after slots wrap
        void emit(unsigned id, int code, int64_t now) {
            ++event_sequence;
            events[(event_sequence - 1) % 8] = {event_sequence, id, code, now};
        }

        // Updates position and velocity for a matched/new track, then advances its crossing state
        // A passage requires an established origin on one side and an observation on the other side
        void observe(Track &track, const Observation &observation, int64_t now) {
            // 1. Update velocity from the previous observation when its time interval is reliable
            if(track.hits) {
                const float elapsed_seconds = (now - track.seen) / 1000000.f;
                if(elapsed_seconds > .35f) {
                    ++resumed;
                    emit(track.id, RESUMED, now);
                }
                // Smooth velocity only for plausible frame intervals, reset it after gaps where extrapolation is unreliable
                if(elapsed_seconds > .001f && elapsed_seconds < .6f) {
                    track.vx = .5f * track.vx + .5f * std::clamp((observation.x - track.x) / elapsed_seconds, -1.f, 1.f);
                    track.vy = .5f * track.vy + .5f * std::clamp((observation.y - track.y) / elapsed_seconds, -1.f, 1.f);
                } else
                    track.vx = track.vy = 0;
            }

            // 2. Store the observed position and time, then smooth available box dimensions
            track.x    = observation.x;
            track.y    = observation.y;
            track.seen = now;

            if(observation.width > 0 && observation.height > 0) {
                track.width  = track.width > 0 ? .7f * track.width + .3f * observation.width:observation.width;
                track.height = track.height > 0 ? .7f * track.height + .3f * observation.height:observation.height;
            }
            ++track.hits;

            // 3. Select the counting axis and classify the position as side A, middle band, or side B
            float position = config.horizontal ? observation.y:observation.x;
            // A single observation outside each extreme is enough
            // Requiring two frames on each side discarded real crossings with intermittent AI
            int zone = 0;
            if(position < config.first - .015f)
                zone = -1;
            else if(position > config.second + .015f)
                zone = 1;

            track.zone = zone;

            // 4. Establish an origin or count a completed side change, then update direction feedback
            if(!zone)
                return;

            // A track first seen in the middle is unarmed until it reaches an outer side
            if(!track.origin) {
                track.origin = zone;
                return;
            }
            // A side change completes one passage. Moving origin to the new side prevents recounting stationary
            // people
            if(zone != track.origin && track.hits >= 2) {
                bool entering = track.origin == -1;

                if(config.reverse)
                    entering = !entering;
                if(entering)
                    ++entries;
                else
                    ++exits;

                track.origin        = zone;
                track.counted_at    = now;
                track.counted_state = entering ? 5:6;
                emit(track.id, entering ? ENTRY:EXIT, now);
            }
        }
    };
    // Runs synthetic movement scenarios at startup, false prevents the hardware wrapper from enabling counting
    bool self_test();
}
