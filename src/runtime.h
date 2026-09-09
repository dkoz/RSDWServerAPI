#pragma once
#include <string>

namespace Runtime {

void MarkStart();
double UptimeSeconds();

void SetRestState(bool enabled, bool running, int port);
void SetRconState(bool enabled, bool running, int port);

bool RestEnabled();
bool RestRunning();
int RestPort();

bool RconEnabled();
bool RconRunning();
int RconPort();

const char* Version();

void BeginShutdown();
bool ShuttingDown();

}
