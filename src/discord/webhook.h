#pragma once
#include <string>

namespace Discord {

void Start(const std::string& webhookUrl, const std::string& username);
void Stop();
bool IsEnabled();

void Post(const std::string& line);

}
