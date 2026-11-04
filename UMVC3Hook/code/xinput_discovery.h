#pragma once

void XInputDiscovery_Init();
// Nonblocking, including at process detach; the worker never accesses game objects.
void XInputDiscovery_Stop();
