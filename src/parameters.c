/*
 * Save/load named GUI settings without disturbing Clumsy's filter-presets
 * config.txt. Settings are validated completely before any widget is changed.
 */
#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>
#include "parameters.h"

#define PARAMS_CAPACITY 80
#define PARAMS_VALUE_MAX 4096
#define PARAMS_LINE_MAX (PARAMS_VALUE_MAX * 3 + 256)
#define PARAMS_FORMAT "clumsy-jitter-parameters-v1"

typedef struct {
    char key[96];
    Ihandle *widget;
    ParamType type;
} Parameter;

static Parameter parameterList[PARAMS_CAPACITY];
static unsigned int parameterCount;

typedef int (*ToggleCallback)(Ihandle *, int);

static void setError(char *error, size_t size, const char *message)
{
    if (error && size) {
        snprintf(error, size, "%s", message);
    }
}

static void setKeyError(char *error, size_t size, const char *prefix, const char *key)
{
    if (error && size) {
        snprintf(error, size, "%s: %s", prefix, key);
    }
}

int paramsRegister(const char *name, Ihandle *control, ParamType type)
{
    unsigned int i;
    if (!name || !control || strlen(name) >= sizeof(parameterList[0].key) ||
        parameterCount >= PARAMS_CAPACITY) return 0;
    for (i = 0; i < parameterCount; ++i) {
        if (strcmp(parameterList[i].key, name) == 0) return 0;
    }
    strcpy(parameterList[parameterCount].key, name);
    parameterList[parameterCount].widget = control;
    parameterList[parameterCount].type = type;
    ++parameterCount;
    return 1;
}

/* Percent-encode user-entered bytes that could interfere with one-line key=value. */
static int encodeValue(const char *source, char *out, size_t capacity)
{
    static const char digits[] = "0123456789ABCDEF";
    const unsigned char *s = (const unsigned char *)source;
    size_t n = 0;
    if (!s) s = (const unsigned char *)"";
    while (*s) {
        unsigned char ch = *s++;
        if (ch == '%' || ch == '=' || ch < 32 || ch >= 127) {
            if (n + 3 >= capacity) return 0;
            out[n++] = '%';
            out[n++] = digits[ch >> 4];
            out[n++] = digits[ch & 15];
        } else {
            if (n + 1 >= capacity) return 0;
            out[n++] = (char)ch;
        }
    }
    if (n >= capacity) return 0;
    out[n] = '\0';
    return 1;
}

static int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int decodeValue(const char *source, char *out, size_t capacity)
{
    size_t n = 0;
    while (*source) {
        unsigned char ch = (unsigned char)*source++;
        if (ch == '%') {
            int high, low;
            if (!source[0] || !source[1]) return 0;
            high = hexDigit(source[0]);
            low = hexDigit(source[1]);
            if (high < 0 || low < 0) return 0;
            ch = (unsigned char)((high << 4) | low);
            source += 2;
        }
        if (ch == 0 || n + 1 >= capacity) return 0;
        out[n++] = (char)ch;
    }
    out[n] = '\0';
    return 1;
}

static int validateValue(const Parameter *param, const char *value)
{
    const char *minText, *maxText;
    char *end;
    double number, minValue, maxValue;
    if (strlen(value) >= PARAMS_VALUE_MAX) return 0;
    switch (param->type) {
    case PARAM_FILTER:
        return *value != 0; /* no empty interception filter */
    case PARAM_PRESET:
        if (!*value) return 0;
        number = strtod(value, &end);
        return *end == '\0' && number >= 0 && number <= IupGetInt(param->widget, "COUNT")
            && (double)(int)number == number;
    case PARAM_TOGGLE:
    case PARAM_MODULE:
        return strcmp(value, "0") == 0 || strcmp(value, "1") == 0;
    case PARAM_NUMBER:
        if (!*value) return 0;
        errno = 0;
        number = strtod(value, &end);
        if (*end != '\0' || errno == ERANGE || (number != number || number > DBL_MAX || number < -DBL_MAX)) return 0;
        minText = IupGetAttribute(param->widget, "INTEGER_MIN");
        maxText = IupGetAttribute(param->widget, "INTEGER_MAX");
        if (!minText) minText = IupGetAttribute(param->widget, "FIXED_MIN");
        if (!maxText) maxText = IupGetAttribute(param->widget, "FIXED_MAX");
        minValue = minText ? atof(minText) : 0;
        maxValue = maxText ? atof(maxText) : 100; /* chance input */
        if (number < minValue || number > maxValue) return 0;
        /* integral fields have INTEGER_MIN/MAX and integer synchronizers */
        if (IupGetAttribute(param->widget, "INTEGER_MIN") &&
            (double)(long long)number != number) return 0;
        return 1;
    }
    return 0;
}

static void applyValue(const Parameter *param, const char *value)
{
    Icallback callback;
    IupStoreAttribute(param->widget, "VALUE", value);
    switch (param->type) {
    case PARAM_TOGGLE:
    case PARAM_MODULE:
        callback = IupGetCallback(param->widget, "ACTION");
        if (callback) ((ToggleCallback)callback)(param->widget, value[0] == '1');
        break;
    case PARAM_NUMBER:
        callback = IupGetCallback(param->widget, "VALUECHANGED_CB");
        if (callback) callback(param->widget);
        break;
    case PARAM_FILTER:
    case PARAM_PRESET:
        /* Never call ACTION on the preset list: it would overwrite filter. */
        break;
    }
}

static int isPresetsFile(const char *path)
{
    const char *base = strrchr(path, '\\');
    const char *slash = strrchr(path, '/');
    if (!base || (slash && slash > base)) base = slash;
    base = base ? base + 1 : path;
    return _stricmp(base, "config.txt") == 0;
}

/* The temporary file is created in the destination directory, for atomic replace. */
static int makeTempPath(const char *path, char *temp, size_t cap)
{
    char directory[MAX_PATH];
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    size_t length;
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) {
        strcpy(directory, ".");
    } else {
        length = (size_t)(slash - path + 1);
        if (length >= sizeof(directory)) return 0;
        memcpy(directory, path, length);
        directory[length] = '\0';
    }
    if (cap < MAX_PATH) return 0;
    return GetTempFileNameA(directory, "cjp", 0, temp) != 0;
}

int paramsSave(const char *path, char *error, size_t errorSize)
{
    unsigned int i;
    char temp[MAX_PATH];
    char encoded[PARAMS_LINE_MAX];
    FILE *file = NULL;
    int ok = 1;

    if (!path || !*path || isPresetsFile(path)) {
        setError(error, errorSize, "Choose a .ini file, not Clumsy's preset config.txt.");
        return 0;
    }
    /* Validate everything before opening or replacing the destination file. */
    for (i = 0; i < parameterCount; ++i) {
        const Parameter *param = &parameterList[i];
        char value[64];
        const char *text = IupGetAttribute(param->widget, "VALUE");
        if (param->type == PARAM_TOGGLE || param->type == PARAM_MODULE) {
            snprintf(value, sizeof(value), "%d", IupGetInt(param->widget, "VALUE") ? 1 : 0);
            text = value;
        }
        if (!text || !validateValue(param, text)) {
            setKeyError(error, errorSize, "Invalid setting; correct before saving", param->key);
            return 0;
        }
    }
    if (!makeTempPath(path, temp, sizeof(temp))) {
        setError(error, errorSize, "Cannot create a temporary file in the destination directory.");
        return 0;
    }
    file = fopen(temp, "wb");
    if (!file) {
        DeleteFileA(temp);
        setError(error, errorSize, "Cannot open temporary parameters file for writing.");
        return 0;
    }
    if (fprintf(file, "# Clumsy-jitter GUI parameters (not filter presets)\nformat=%s\n", PARAMS_FORMAT) < 0)
        ok = 0;
    for (i = 0; i < parameterCount && ok; ++i) {
        const Parameter *param = &parameterList[i];
        const char *value = IupGetAttribute(param->widget, "VALUE");
        char toggleValue[4];
        if (param->type == PARAM_TOGGLE || param->type == PARAM_MODULE) {
            snprintf(toggleValue, sizeof(toggleValue), "%d", IupGetInt(param->widget, "VALUE") ? 1 : 0);
            value = toggleValue;
        }
        if (!encodeValue(value, encoded, sizeof(encoded)) ||
            fprintf(file, "%s=%s\n", param->key, encoded) < 0)
            ok = 0;
    }
    if (fflush(file) != 0) ok = 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        DeleteFileA(temp);
        setError(error, errorSize, "Unable to write all parameter values.");
        return 0;
    }
    if (!MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temp);
        setError(error, errorSize, "Cannot replace target file. Check its permissions or whether it is open.");
        return 0;
    }
    return 1;
}

int paramsLoad(const char *path, char *error, size_t errorSize)
{
    FILE *file;
    char line[PARAMS_LINE_MAX];
    char (*saved)[PARAMS_VALUE_MAX] = NULL;
    unsigned char seen[PARAMS_CAPACITY] = {0};
    int formatSeen = 0;
    int ok = 1;
    unsigned int i;

    if (!path || !*path) {
        setError(error, errorSize, "No parameters file was selected.");
        return 0;
    }
    file = fopen(path, "rb");
    if (!file) {
        setError(error, errorSize, "Cannot open the parameters file.");
        return 0;
    }
    saved = (char (*)[PARAMS_VALUE_MAX])calloc(PARAMS_CAPACITY, PARAMS_VALUE_MAX);
    if (!saved) {
        fclose(file);
        setError(error, errorSize, "Not enough memory to load parameters.");
        return 0;
    }
    while (fgets(line, sizeof(line), file) && ok) {
        size_t len = strlen(line);
        char *equals;
        char *key = line;
        char *value;
        if (len && line[len - 1] != '\n' && !feof(file)) {
            setError(error, errorSize, "Parameter file contains a line that is too long.");
            ok = 0;
            break;
        }
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (!len || line[0] == '#') continue;
        equals = strchr(line, '=');
        if (!equals) {
            setError(error, errorSize, "Invalid parameters file: missing '=' separator.");
            ok = 0;
            break;
        }
        *equals = '\0';
        value = equals + 1;
        if (!strcmp(key, "format")) {
            if (formatSeen || strcmp(value, PARAMS_FORMAT)) {
                setError(error, errorSize, "Unrecognized or duplicate parameters file format.");
                ok = 0;
            }
            formatSeen = 1;
            continue;
        }
        for (i = 0; i < parameterCount; ++i) {
            if (strcmp(key, parameterList[i].key) == 0) break;
        }
        if (i == parameterCount) continue; /* future-version keys */
        if (seen[i] || !decodeValue(value, saved[i], PARAMS_VALUE_MAX)) {
            setKeyError(error, errorSize, "Invalid or duplicate parameter", key);
            ok = 0;
            break;
        }
        seen[i] = 1;
    }
    if (ferror(file)) {
        setError(error, errorSize, "Could not read the complete parameters file.");
        ok = 0;
    }
    fclose(file);
    if (ok && !formatSeen) {
        setError(error, errorSize, "Not a Clumsy parameter file (format header missing).");
        ok = 0;
    }
    for (i = 0; i < parameterCount && ok; ++i) {
        if (!seen[i]) {
            setKeyError(error, errorSize, "Required setting missing", parameterList[i].key);
            ok = 0;
        } else if (!validateValue(&parameterList[i], saved[i])) {
            setKeyError(error, errorSize, "Invalid setting value", parameterList[i].key);
            ok = 0;
        }
    }
    if (ok) {
        /* Apply subordinate controls first, then module enabled states. */
        for (i = 0; i < parameterCount; ++i)
            if (parameterList[i].type != PARAM_MODULE)
                applyValue(&parameterList[i], saved[i]);
        for (i = 0; i < parameterCount; ++i)
            if (parameterList[i].type == PARAM_MODULE)
                applyValue(&parameterList[i], saved[i]);
    }
    free(saved);
    return ok;
}
