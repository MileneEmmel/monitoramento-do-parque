#pragma once
#include <cstddef>
#include "cJSON.h"

bool wifi_preview_start();
// JPEG preparation is needed only while an authenticated local viewer is polling.
bool wifi_preview_requested();
bool wifi_preview_sleep();
void wifi_preview_wake();
// Private owner-only cloud discovery, not part of public status or serial logs.
cJSON *wifi_preview_cloud_access();
// Takes ownership of the allocated JSON packet, including when replacing an older frame.
void wifi_preview_publish(char *packet, size_t length);
