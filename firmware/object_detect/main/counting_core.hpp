#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Coordinates are normalized to the image. No camera, network or ESP dependencies.
namespace park {
constexpr int capacity = 32;
struct Config { float first = .40f, second = .60f; bool horizontal = false, reverse = false; };
struct Observation { float x=0, y=0, width=0, height=0; };
struct Track {
    bool active = false, matched = false;
    unsigned id = 0, hits = 0;
    float x = 0, y = 0, vx = 0, vy = 0;
    float width = 0, height = 0;
    int origin = 0, zone = 0;
    int counted_state = 0;
    int64_t seen = 0, counted_at = -2000000;
};
enum EventCode { NEW_A=1, NEW_B, NEW_MIDDLE, ENTRY, EXIT, RESUMED, LOST_A, LOST_B, LOST_MIDDLE, LOST_CROSSING, FULL, RESET_TOTALS };
struct Event { unsigned sequence=0, id=0; int code=0; int64_t at=0; };
class Counter {
public:
    Config config;
    uint64_t entries = 0, exits = 0;
    unsigned created=0, expired=0, resumed=0, discarded=0, frames=0, empty_frames=0;
    Event events[8];
    unsigned event_sequence=0;
    void clear_tracks() { for (auto &t : tracks) t = Track{}; }
    void reset_totals(int64_t now) {
        entries=exits=0; clear_tracks(); emit(0,RESET_TOTALS,now);
    }
    void reset() {
        clear_tracks(); config=Config{}; entries=exits=0; next_id=0;
        created=expired=resumed=discarded=frames=empty_frames=event_sequence=0;
        for(auto &e:events) e=Event{};
    }
    // States: 0=born in band, 1/2=armed from A/B, 3/4=crossing,
    // 5/6=entry/exit just counted. Only observations get an on-screen state.
    int state(unsigned id, int64_t now) const {
        for (const auto &t:tracks) if (t.active && t.id==id) {
            if (now-t.counted_at<1000000) return t.counted_state;
            if (!t.origin) return 0;
            return t.origin==-1 ? (t.zone==0?3:1) : (t.zone==0?4:2);
        }
        return 0;
    }
    void update(const Observation *observations, int size, int64_t now, unsigned *ids) {
        size = std::min(size, capacity);
        ++frames; if (!size) ++empty_frames;
        bool used[capacity] = {};
        for (int i = 0; i < size; ++i) ids[i] = 0;
        for (auto &t : tracks) {
            if (t.active && now - t.seen > 2000000) {
                ++expired;
                bool crossing=t.origin && !t.zone;
                if (crossing) ++discarded;
                emit(t.id,crossing?LOST_CROSSING:t.origin==-1?LOST_A:t.origin==1?LOST_B:LOST_MIDDLE,now);
                t = Track{};
            }
            t.matched = false;
        }
        // Solve the whole frame together, instead of taking the nearest pair
        // first and potentially leaving the second person with the wrong track.
        int rows=0;
        for(int j=0;j<capacity;++j) if(tracks[j].active) work.track_rows[rows++]=j;
        if(rows && size) {
            const int columns=size+rows; // dummy columns explicitly allow no match
            for(int r=0;r<rows;++r) {
                for(int i=0;i<size;++i) work.cost[r][i]=association_cost(tracks[work.track_rows[r]],observations[i],now);
                for(int i=size;i<columns;++i) work.cost[r][i]=unmatched_cost;
            }
            assign(rows,columns);
            for(int column=1;column<=size;++column) {
                const int row=work.owner[column]-1, oi=column-1;
                if(row>=0 && work.cost[row][oi]<unmatched_cost) {
                    auto &t=tracks[work.track_rows[row]];
                    used[oi]=true; observe(t,observations[oi],now); ids[oi]=t.id;
                }
            }
        }
        for (int i = 0; i < size; ++i) if (!used[i]) {
            for (auto &t : tracks) if (!t.active) {
                t = Track{}; t.active = true; t.id = ++next_id;
                ++created;
                observe(t, observations[i], now); ids[i] = t.id;
                emit(t.id,t.origin==-1?NEW_A:t.origin==1?NEW_B:NEW_MIDDLE,now);
                break;
            }
            if (!ids[i]) emit(0,FULL,now);
        }
    }
private:
    static constexpr float unmatched_cost=1.5f;
    // Workspace is owned by the counter (not the small ESP task stack), reused
    // at every frame. Bound: 32 tracks x (32 observations + 32 dummy columns).
    struct Workspace {
        float cost[capacity][capacity*2];
        float u[capacity+1], v[capacity*2+1], distance[capacity*2+1];
        int owner[capacity*2+1], previous[capacity*2+1], track_rows[capacity];
        bool visited[capacity*2+1];
    } work;
    Track tracks[capacity];
    unsigned next_id = 0;
    static float association_cost(const Track &t, const Observation &o, int64_t now) {
        const float dt=(now-t.seen)/1000000.f;
        const float gate=std::min(.40f,.30f+.10f*dt);
        float dx=o.x-t.x, dy=o.y-t.y;
        const float current=dx*dx+dy*dy;
        if(current>gate*gate) return 1000.f;
        const float prediction=std::min(dt,.35f);
        const float px=t.x+t.vx*prediction, py=t.y+t.vy*prediction;
        dx=o.x-px; dy=o.y-py;
        const float predicted=dx*dx+dy*dy;
        float cost=(.25f*current+.45f*predicted)/(gate*gate);
        if(t.width>0 && t.height>0 && o.width>0 && o.height>0) {
            const float wr=std::max(t.width,o.width)/std::min(t.width,o.width);
            const float hr=std::max(t.height,o.height)/std::min(t.height,o.height);
            if(wr>4.f || hr>4.f) return 1000.f;
            float intersection_w=std::max(0.f,std::min(px+t.width/2,o.x+o.width/2)-std::max(px-t.width/2,o.x-o.width/2));
            float intersection_h=std::max(0.f,std::min(py+t.height/2,o.y+o.height/2)-std::max(py-t.height/2,o.y-o.height/2));
            const float intersection=intersection_w*intersection_h;
            const float iou=intersection/(t.width*t.height+o.width*o.height-intersection);
            const float shape=.5f*(1-1/wr+1-1/hr);
            cost+=.20f*(1-iou)+.20f*shape;
        }
        const float speed2=t.vx*t.vx+t.vy*t.vy;
        const float movement2=current;
        if(t.hits>=3 && dt<.6f && speed2>.01f && movement2>.0004f) {
            const float alignment=((o.x-t.x)*t.vx+(o.y-t.y)*t.vy)/std::sqrt(speed2*movement2);
            cost+=.10f*std::max(0.f,-alignment);
        }
        // A stale track should not steal a fresh person's box just by proximity.
        return cost+.04f*std::min(dt/2.f,1.f);
    }
    void assign(int rows, int columns) {
        // Rectangular Hungarian assignment: minimum total cost, exclusive IDs.
        std::fill(work.u,work.u+rows+1,0.f);
        std::fill(work.v,work.v+columns+1,0.f);
        std::fill(work.owner,work.owner+columns+1,0);
        for(int row=1;row<=rows;++row) {
            work.owner[0]=row;
            std::fill(work.distance,work.distance+columns+1,1000000.f);
            std::fill(work.visited,work.visited+columns+1,false);
            int column=0;
            do {
                work.visited[column]=true;
                const int active_row=work.owner[column];
                float delta=1000000.f; int next=0;
                for(int j=1;j<=columns;++j) if(!work.visited[j]) {
                    const float cost=work.cost[active_row-1][j-1]-work.u[active_row]-work.v[j];
                    if(cost<work.distance[j]) {work.distance[j]=cost;work.previous[j]=column;}
                    if(work.distance[j]<delta) {delta=work.distance[j];next=j;}
                }
                for(int j=0;j<=columns;++j) {
                    if(work.visited[j]) {work.u[work.owner[j]]+=delta;work.v[j]-=delta;}
                    else work.distance[j]-=delta;
                }
                column=next;
            } while(work.owner[column]);
            do {
                const int previous=work.previous[column];
                work.owner[column]=work.owner[previous]; column=previous;
            } while(column);
        }
    }
    void emit(unsigned id, int code, int64_t now) {
        ++event_sequence;
        events[(event_sequence-1)%8]={event_sequence,id,code,now};
    }
    void observe(Track &t, const Observation &o, int64_t now) {
        if (t.hits) {
            const float dt=(now-t.seen)/1000000.f;
            if (dt>.35f) { ++resumed; emit(t.id,RESUMED,now); }
            if (dt>.001f && dt<.6f) {
                t.vx=.5f*t.vx+.5f*std::clamp((o.x-t.x)/dt,-1.f,1.f);
                t.vy=.5f*t.vy+.5f*std::clamp((o.y-t.y)/dt,-1.f,1.f);
            } else t.vx=t.vy=0;
        }
        t.x = o.x; t.y = o.y; t.seen = now; t.matched = true;
        if(o.width>0 && o.height>0) {
            t.width=t.width>0?.7f*t.width+.3f*o.width:o.width;
            t.height=t.height>0?.7f*t.height+.3f*o.height:o.height;
        }
        ++t.hits;
        float position = config.horizontal ? o.y : o.x;
        // A single observation outside each extreme is enough. Requiring two
        // frames on each side discarded real crossings with intermittent AI.
        int zone = position < config.first - .015f ? -1 : position > config.second + .015f ? 1 : 0;
        t.zone=zone;
        if (!zone) return;
        if (!t.origin) { t.origin = zone; return; }
        if (zone != t.origin && t.hits >= 2) {
            bool entering = t.origin == -1;
            if (config.reverse) entering = !entering;
            if (entering) ++entries; else ++exits;
            t.origin = zone;
            t.counted_at=now; t.counted_state=entering?5:6;
            emit(t.id,entering?ENTRY:EXIT,now);
        }
    }
};
bool self_test();
}
