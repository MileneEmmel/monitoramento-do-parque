#pragma once
#include <cstddef>

bool wifi_preview_start();
// JPEG preparation is needed only while an authenticated local viewer is polling.
bool wifi_preview_requested();
// Takes ownership of the allocated JSON packet, including when replacing an older frame.
void wifi_preview_publish(char *packet, size_t length);
