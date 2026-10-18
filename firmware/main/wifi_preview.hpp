// Declares network operations and local camera preview publishing

#pragma once
#include <cstddef>
#include "cJSON.h"

// Starts station networking, static/API routes, and serial configuration services
bool wifi_preview_start();

// JPEG preparation is needed only while an authenticated local viewer is polling
// Tests recent authenticated viewer demand, reading this flag does not extend the deadline
bool wifi_preview_requested();

// Stops Wi-Fi for sleep unless a configuration change is active/pending, false means shutdown was not completed
bool wifi_preview_sleep();

// Restarts Wi-Fi after a cancelled sleep preparation
void wifi_preview_wake();

// Private owner-only cloud discovery, not part of public status or serial logs
// Returns owned private address/access-key JSON, or nullptr if connection/address validation fails
cJSON *wifi_preview_cloud_access();

// Takes ownership of the allocated JSON packet, including when replacing an older frame
// Replaces the retained preview packet, the caller must not use or free packet after this call
void wifi_preview_publish(char *packet, size_t length);
