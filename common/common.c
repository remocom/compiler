#include "common.h"

const char *remocom_detect_target_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

const char *remocom_detect_target_os(void) {
#if defined(__linux__)
    return "linux";
#elif defined(__APPLE__) && defined(__MACH__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "unknown";
#endif
}
