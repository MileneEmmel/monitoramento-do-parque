// Declares camera capture and restart interfaces for the supported boards
// Based on ESP-WHO examples/object_detect/main/frame_cap_pipeline.hpp, with a project-specific restart interface

#pragma once
// Only call with the application's capture/detection tasks paused
// Restarts the existing capture device, returns true only if both stop and start succeed
bool restart_dvp_capture();
#include "who_frame_cap.hpp"

// ESP32-S3 capture declaration from the original ESP-WHO object_detect example - Original logic preserved
// Creates the ESP32-S3 parallel camera pipeline, lcd controls buffering. nullptr indicates failure
who::frame_cap::WhoFrameCap *get_dvp_frame_cap_pipeline(bool lcd);
