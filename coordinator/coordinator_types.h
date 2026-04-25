#ifndef REMOCOM_COORDINATOR_TYPES_H
#define REMOCOM_COORDINATOR_TYPES_H

#include "manifest_loader.h"

#define MAX_TASKS REMOCOM_MAX_SOURCES
#define MAX_MANIFEST_VALUE REMOCOM_MAX_MANIFEST_VALUE
#define MAX_FLAGS REMOCOM_MAX_FLAGS

/// @brief Represents a compile task derived from the manifest.
typedef struct {
    char source_path[MAX_MANIFEST_VALUE];
    char object_path[MAX_MANIFEST_VALUE];
    char build_output[MAX_MANIFEST_VALUE];
    char flags[MAX_FLAGS][MAX_MANIFEST_VALUE];
    int flag_count;
} CompileTask;

#endif
