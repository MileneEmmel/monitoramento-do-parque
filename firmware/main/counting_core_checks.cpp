#include "counting_core.hpp"
#include <memory>
namespace park {
bool self_test() {
    auto move = [](Counter &c, float x, int64_t &time, float y = .5f) {
        Observation o{x, y}; unsigned ids[capacity] = {};
        time += 100000; c.update(&o, 1, time, ids);
        return ids[0];
    };
    // Reuse one heap instance: app_main has a small stack on ESP32.
    std::unique_ptr<Counter> instance(new Counter);
    Counter &c=*instance; int64_t time = 0;
    unsigned id = move(c, .2f, time);
    if (move(c, .2f, time) != id) return false;
    for (float x : {.35f,.45f,.55f,.7f,.7f}) move(c,x,time);
    if (c.entries != 1 || c.exits) return false;
    for (int i=0;i<10;++i) move(c,.7f,time);
    if (c.entries != 1) return false;
    for (float x : {.55f,.45f,.3f,.3f}) move(c,x,time);
    if (c.entries != 1 || c.exits != 1) return false;
    c.reset(); auto &turn=c; time=0;
    for (float x : {.2f,.2f,.35f,.45f,.5f,.4f,.3f,.3f}) move(turn,x,time);
    if (turn.entries || turn.exits) return false;
    c.reset(); auto &middle=c; time=0;
    for (float x : {.5f,.5f,.65f,.7f,.7f}) move(middle,x,time);
    if (middle.entries || middle.exits) return false;
    c.reset(); auto &jitter=c; time=0;
    for (float x : {.2f,.2f,.4f,.45f,.55f,.61f,.59f,.612f,.59f,.6f}) move(jitter,x,time);
    if (jitter.entries || jitter.exits) return false;
    c.reset(); auto &lost=c; time=0; move(lost,.2f,time); move(lost,.2f,time);
    time += 2100000;
    for (float x : {.7f,.7f,.7f}) move(lost,x,time);
    if (lost.entries || lost.exits) return false;
    c.reset(); auto &reverse=c; reverse.config.reverse=true; time=0;
    for (float x : {.2f,.2f,.35f,.5f,.65f,.7f}) move(reverse,x,time);
    if (reverse.entries || reverse.exits != 1) return false;
    c.reset(); auto &horizontal=c; horizontal.config.horizontal=true; time=0;
    for (float y : {.2f,.2f,.35f,.5f,.65f,.7f}) move(horizontal,.5f,time,y);
    if (horizontal.entries != 1 || horizontal.exits) return false;
    c.reset(); auto &two=c; time=0; unsigned ids[capacity]={};
    for (float x : {.2f,.2f,.35f,.5f,.65f,.7f}) {
        Observation o[]={{x,.2f},{x,.8f}}; time+=100000;
        two.update(o,2,time,ids);
        if (!ids[0] || ids[0]==ids[1]) return false;
    }
    if (two.entries != 2 || two.exits) return false;
    // A brief occlusion preserves identity, but a large jump cannot be associated.
    c.reset(); auto &gap=c; time=0; id=move(gap,.2f,time); move(gap,.2f,time);
    time+=300000; gap.update(nullptr,0,time,ids);
    if (move(gap,.35f,time)!=id) return false;
    for (float x : {.5f,.65f,.7f}) move(gap,x,time);
    if (gap.entries!=1) return false;
    c.reset(); auto &jump=c; time=0; move(jump,.1f,time); move(jump,.1f,time);
    move(jump,.9f,time); move(jump,.9f,time);
    if (jump.entries || jump.exits) return false;
    // Regression: a normal crossing no longer needs to pause at either end.
    c.reset(); time=0; id=move(c,.3f,time);
    move(c,.5f,time); move(c,.7f,time);
    if (c.entries!=1 || c.state(id,time)!=5) return false;
    move(c,.5f,time); move(c,.3f,time);
    if (c.exits!=1 || c.state(id,time)!=6) return false;
    // Regression: a 1.3 s detection dropout used to discard a valid trajectory.
    c.reset(); time=0; id=move(c,.3f,time); move(c,.5f,time);
    time+=1300000; c.update(nullptr,0,time,ids);
    if (move(c,.7f,time)!=id || c.entries!=1 || c.resumed!=1) return false;
    // A track born in the band is explicitly unarmed, and disappearing never counts.
    c.reset(); time=0; id=move(c,.5f,time);
    if (c.state(id,time)!=0) return false;
    time+=2100000; c.update(nullptr,0,time,ids);
    if (c.entries || c.exits || c.expired!=1) return false;
    // Lost crossing is explained without inventing a completed passage.
    c.reset(); time=0; move(c,.3f,time); move(c,.5f,time);
    time+=2100000; c.update(nullptr,0,time,ids);
    if (c.entries || c.discarded!=1) return false;
    // Simultaneous opposite movement on distinct paths must keep IDs exclusive.
    c.reset(); time=0;
    for(float x:{.2f,.35f,.5f,.65f,.8f}) {
        Observation o[]={{x,.2f},{1-x,.8f}}; time+=100000;
        c.update(o,2,time,ids);
        if(!ids[0] || ids[0]==ids[1]) return false;
    }
    if(c.entries!=1 || c.exits!=1) return false;
    c.clear_tracks();
    if(c.entries!=1 || c.exits!=1) return false;
    // Regression: greedy nearest-pair picks track B for observation A, despite
    // there being a better one-to-one assignment for the complete frame.
    c.reset(); time=100000;
    Observation initial[]={{.45f,.5f,.12f,.45f},{.60f,.5f,.22f,.6f}};
    c.update(initial,2,time,ids); unsigned a=ids[0],b=ids[1];
    Observation advanced[]={{.58f,.5f,.12f,.45f},{.74f,.5f,.22f,.6f}};
    c.update(advanced,2,time+=100000,ids);
    if(ids[0]!=a || ids[1]!=b) return false;
    // Two close, partially overlapping boxes: both entry and exit must count,
    // independent of the score/order in which the detector returns the boxes.
    c.reset(); time=0; a=b=0;
    for(float x:{.2f,.3f,.4f,.5f,.6f,.7f,.6f,.5f,.4f,.3f,.2f}) {
        Observation o[]={{x,.5f,.16f,.55f},{x+.04f,.52f,.13f,.48f}};
        const bool swapped=(time/100000)%2;
        if(swapped) std::swap(o[0],o[1]);
        c.update(o,2,time+=100000,ids);
        unsigned ia=ids[swapped?1:0],ib=ids[swapped?0:1];
        if(!a) {a=ia;b=ib;}
        if(ia!=a || ib!=b || a==b) return false;
    }
    if(c.entries!=2 || c.exits!=2 || c.created!=2) return false;
    // One member of a close pair disappears briefly on exit, then comes back.
    c.reset(); time=0; a=b=0;
    for(int step=0;step<7;++step) {
        float x=.8f-step*.1f;
        Observation o[]={{x,.5f,.16f,.55f},{x+.04f,.52f,.13f,.48f}};
        int size=step==2?1:2;
        c.update(o,size,time+=100000,ids);
        if(!a) {a=ids[0];b=ids[1];}
        if(ids[0]!=a || (size==2 && ids[1]!=b)) return false;
    }
    if(c.entries || c.exits!=2 || c.created!=2) return false;
    // Opposite movement with overlapping centers: size and velocity retain IDs.
    c.reset(); time=0; a=b=0;
    for(float x:{.2f,.3f,.4f,.5f,.6f,.7f,.8f}) {
        Observation o[]={{x,.5f,.12f,.45f},{1-x,.5f,.22f,.6f}};
        bool swapped=(time/100000)%2;
        if(swapped) std::swap(o[0],o[1]);
        c.update(o,2,time+=100000,ids);
        unsigned ia=ids[swapped?1:0],ib=ids[swapped?0:1];
        if(!a) {a=ia;b=ib;}
        if(ia!=a || ib!=b) return false;
    }
    if(c.entries!=1 || c.exits!=1 || c.created!=2) return false;
    // Rectangular assignment must leave impossible observations unmatched.
    c.reset(); time=100000;
    Observation small{.2f,.5f,.05f,.1f}; c.update(&small,1,time,ids); a=ids[0];
    Observation huge{.7f,.5f,.8f,.9f}; c.update(&huge,1,time+=100000,ids);
    if(ids[0]==a || c.entries || c.exits) return false;
    // Exercise all 32 rows and 64 columns, including capacity exhaustion.
    c.reset(); time=100000;
    Observation crowd[capacity];
    for(int i=0;i<capacity;++i) crowd[i]={.2f,.02f+i*.03f,.05f,.02f};
    c.update(crowd,capacity,time,ids);
    for(int pass=0;pass<2;++pass) {
        c.update(crowd,capacity,time+=100000,ids);
        for(int i=0;i<capacity;++i) {
            if(!ids[i]) return false;
            for(int j=0;j<i;++j) if(ids[i]==ids[j]) return false;
        }
    }
    Observation extra{.95f,.5f,.05f,.02f};c.update(&extra,1,time+=100000,ids);
    if(ids[0] || c.created!=capacity || c.entries || c.exits) return false;
    // User reset clears totals and active trajectories, but preserves calibration
    // and monotonically allocated IDs (old trajectories cannot complete later).
    c.reset();time=100000;c.config={.3f,.4f,true,true};
    Observation before_reset{.5f,.2f,.12f,.4f};c.update(&before_reset,1,time,ids);a=ids[0];
    c.entries=12;c.exits=9;
    c.reset_totals(time+=100000);
    if(c.entries || c.exits || c.config.first!=.3f || c.config.second!=.4f ||
       !c.config.horizontal || !c.config.reverse || c.events[(c.event_sequence-1)%8].code!=RESET_TOTALS) return false;
    Observation after_reset{.5f,.7f,.12f,.4f};c.update(&after_reset,1,time+=100000,ids);
    return ids[0]!=a && c.entries==0 && c.exits==0;
}
}
