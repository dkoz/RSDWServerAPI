#pragma once

// Single source of truth for the version. Runtime::Version() used to return a hardcoded
// "0.1.1" while xmake.lua said 0.1.4, so /api/health misreported every build after 0.1.1
// (issue #5). The Makefile and xmake.lua read this file; do not set the version elsewhere.
#define RSDWAPI_VERSION "0.1.5"
