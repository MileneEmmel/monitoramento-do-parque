#pragma once
#include "cJSON.h"
#include <ctime>
#include <string>
bool park_history_start();
void park_history_capture();
cJSON *park_history_status();
std::string park_today();
std::string park_timestamp(time_t value);
cJSON *park_history_report(const std::string &start, const std::string &end);
bool park_history_range(const std::string &start, const std::string &end);
std::string park_history_csv(const std::string &start, const std::string &end);
cJSON *park_history_cleanup(const std::string &before, bool erase, const std::string &plan);
