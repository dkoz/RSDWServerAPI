// Keeps the .so loadable on the server image's glibc when it is built on a newer host.
//
// glibc 2.38 redirects strtol/strtoul/sscanf (and atoi, which inlines to strtol) to new
// __isoc23_* entry points whenever _GNU_SOURCE is set, which g++ always sets. A build on
// Ubuntu 24.04 therefore needs GLIBC_2.38, and the Debian bookworm image the dedicated
// server runs in has 2.36 - LD_PRELOAD then fails with "version GLIBC_2.38 not found" and
// the server starts without the mod. The static libstdc++ picks up the same references.
//
// Defining the three symbols here, hidden, satisfies every reference inside this library
// with the long-standing GLIBC_2.2.5 versions. The only behavioural difference in the C23
// variants is accepting "0b" binary prefixes, which nothing here parses. On a host whose
// glibc predates 2.38 nothing references these and they are simply unused.
//
// `make glibc-check` fails the build if anything newer than 2.36 is still required.

#include <cstdarg>

#if defined(__x86_64__) && defined(__linux__)

extern "C" {
long rsdw_strtol_v225(const char* text, char** end, int base);
unsigned long rsdw_strtoul_v225(const char* text, char** end, int base);
int rsdw_vsscanf_v225(const char* text, const char* format, va_list args);
}

__asm__(".symver rsdw_strtol_v225, strtol@GLIBC_2.2.5");
__asm__(".symver rsdw_strtoul_v225, strtoul@GLIBC_2.2.5");
__asm__(".symver rsdw_vsscanf_v225, vsscanf@GLIBC_2.2.5");

extern "C" {

__attribute__((visibility("hidden"))) long __isoc23_strtol(const char* text, char** end, int base) {
    return rsdw_strtol_v225(text, end, base);
}

__attribute__((visibility("hidden"))) unsigned long __isoc23_strtoul(const char* text, char** end,
                                                                     int base) {
    return rsdw_strtoul_v225(text, end, base);
}

__attribute__((visibility("hidden"))) int __isoc23_sscanf(const char* text, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = rsdw_vsscanf_v225(text, format, args);
    va_end(args);
    return result;
}

}

#endif
