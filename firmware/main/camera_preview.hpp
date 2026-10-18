// Declares initialization and frame submission for the local camera preview

#pragma once
#include "who_detect.hpp"
#include "visitor_counter.hpp"

// Starts network preview resources and the background JPEG worker, false means initialization failed
bool camera_preview_start();

// Nonblocking preview submission: copies the image when needed and keeps counts paired with that detector result
void camera_preview_submit(const who::detect::WhoDetect::result_t &result, const CountSnapshot &counts);
