#ifndef CLUMSY_PARAMETERS_H
#define CLUMSY_PARAMETERS_H

#include <stddef.h>
#include "iup.h"

/* Separate from config.txt, which stores filter presets. */
typedef enum {
    PARAM_FILTER,       /* filter expression; requires Re-apply if running */
    PARAM_PRESET,       /* currently selected preset (or 0 for custom) */
    PARAM_TOGGLE,       /* checkbox backed by uiSyncToggle */
    PARAM_MODULE,       /* module enable checkbox backed by uiToggleControls */
    PARAM_NUMBER        /* numeric text backed by VALUECHANGED_CB */
} ParamType;

int paramsRegister(const char *name, Ihandle *control, ParamType type);
int paramsSave(const char *path, char *error, size_t errorSize);
int paramsLoad(const char *path, char *error, size_t errorSize);

#endif
