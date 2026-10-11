#pragma once
#include "who_detect.hpp"
#include "visitor_counter.hpp"

bool camera_preview_start();
void camera_preview_submit(const who::detect::WhoDetect::result_t &result, const CountSnapshot &counts);
