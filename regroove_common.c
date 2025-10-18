#include "regroove_common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>
#include <SDL.h>

// Helper: Check if filename is a module file
static int is_module_file(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;

    char ext[16];
    snprintf(ext, sizeof(ext), "%s", dot);
    for (char *p = ext; *p; ++p) *p = tolower(*p);

    return (
        strcmp(ext, ".mod") == 0  || strcmp(ext, ".xm") == 0   ||
        strcmp(ext, ".s3m") == 0  || strcmp(ext, ".it") == 0   ||
        strcmp(ext, ".med") == 0  || strcmp(ext, ".mmd") == 0  ||
        strcmp(ext, ".mmd0") == 0 || strcmp(ext, ".mmd1") == 0 ||
        strcmp(ext, ".mmd2") == 0 || strcmp(ext, ".mmd3") == 0 ||
        strcmp(ext, ".mmdc") == 0
    );
}

// Helper: Normalize directory path (remove trailing slash)
static void normalize_directory_path(char *path) {
    size_t len = strlen(path);
    if (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        path[len - 1] = '\0';
    }
}

// Initialize file list
RegrooveFileList* regroove_filelist_create(void) {
    RegrooveFileList *list = calloc(1, sizeof(RegrooveFileList));
    if (!list) return NULL;

    list->filenames = calloc(COMMON_MAX_FILES, sizeof(char*));
    if (!list->filenames) {
        free(list);
        return NULL;
    }

    return list;
}

// Load files from directory (handles trailing slash automatically)
int regroove_filelist_load(RegrooveFileList *list, const char *dir_path) {
    if (!list || !dir_path) return -1;

    // Free existing files
    if (list->filenames) {
        for (int i = 0; i < list->count; i++) {
            free(list->filenames[i]);
        }
    }
    list->count = 0;
    list->current_index = 0;

    // Normalize and store directory path (remove trailing slash)
    snprintf(list->directory, COMMON_MAX_PATH, "%s", dir_path);
    normalize_directory_path(list->directory);

    // Open directory
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;

    // Read directory entries
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && list->count < COMMON_MAX_FILES) {
        if ((entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) &&
            is_module_file(entry->d_name)) {
            list->filenames[list->count++] = strdup(entry->d_name);
        }
    }

    closedir(dir);
    return list->count;
}

// Get current file's full path
const char* regroove_filelist_get_current_path(RegrooveFileList *list, char *buffer, size_t bufsize) {
    if (!list || !buffer || list->count == 0) return NULL;

    snprintf(buffer, bufsize, "%s/%s",
             list->directory,
             list->filenames[list->current_index]);
    return buffer;
}

// Navigate file list
void regroove_filelist_next(RegrooveFileList *list) {
    if (!list || list->count == 0) return;

    list->current_index++;
    if (list->current_index >= list->count) {
        list->current_index = 0;
    }
}

void regroove_filelist_prev(RegrooveFileList *list) {
    if (!list || list->count == 0) return;

    list->current_index--;
    if (list->current_index < 0) {
        list->current_index = list->count - 1;
    }
}

// Free file list
void regroove_filelist_destroy(RegrooveFileList *list) {
    if (!list) return;

    if (list->filenames) {
        for (int i = 0; i < list->count; i++) {
            free(list->filenames[i]);
        }
        free(list->filenames);
    }

    free(list);
}

// Initialize common state
RegrooveCommonState* regroove_common_create(void) {
    RegrooveCommonState *state = calloc(1, sizeof(RegrooveCommonState));
    if (!state) return NULL;

    state->paused = 1;
    state->pitch = 1.0;

    // Initialize device config to defaults
    state->device_config.midi_device_0 = -1;  // Not configured
    state->device_config.midi_device_1 = -1;  // Not configured
    state->device_config.audio_device = -1;   // Default device

    return state;
}

// Helper: Trim whitespace
static char* trim_whitespace(char *str) {
    while (isspace(*str)) str++;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) end--;
    *(end + 1) = '\0';
    return str;
}

// Load input mappings and device config from .ini file (with fallback to defaults)
int regroove_common_load_mappings(RegrooveCommonState *state, const char *ini_path) {
    if (!state) return -1;

    // Create input mappings if not already created
    if (!state->input_mappings) {
        state->input_mappings = input_mappings_create();
        if (!state->input_mappings) return -1;
    }

    // Try to load from file
    if (input_mappings_load(state->input_mappings, ini_path) != 0) {
        // Failed to load, use defaults
        input_mappings_reset_defaults(state->input_mappings);
        return -1;
    }

    // Parse device configuration from the same INI file
    FILE *f = fopen(ini_path, "r");
    if (f) {
        char line[512];
        int in_devices_section = 0;

        while (fgets(line, sizeof(line), f)) {
            char *trimmed = trim_whitespace(line);

            // Skip empty lines and comments
            if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') continue;

            // Check for [devices] section
            if (trimmed[0] == '[') {
                in_devices_section = (strstr(trimmed, "[devices]") != NULL);
                continue;
            }

            // Parse device settings if we're in the [devices] section
            if (in_devices_section) {
                char *eq = strchr(trimmed, '=');
                if (!eq) continue;

                *eq = '\0';
                char *key = trim_whitespace(trimmed);
                char *value = trim_whitespace(eq + 1);

                if (strcmp(key, "midi_device_0") == 0) {
                    state->device_config.midi_device_0 = atoi(value);
                } else if (strcmp(key, "midi_device_1") == 0) {
                    state->device_config.midi_device_1 = atoi(value);
                } else if (strcmp(key, "audio_device") == 0) {
                    state->device_config.audio_device = atoi(value);
                }
            }
        }

        fclose(f);
    }

    return 0;
}

// Load a module file safely (handles audio locking)
int regroove_common_load_module(RegrooveCommonState *state, const char *path,
                                struct RegrooveCallbacks *callbacks) {
    if (!state || !path) return -1;

    // Lock audio before destroying old module
    SDL_LockAudio();
    if (state->player) {
        Regroove *old = state->player;
        state->player = NULL;
        SDL_UnlockAudio();
        regroove_destroy(old);
    } else {
        SDL_UnlockAudio();
    }

    // Create new module
    Regroove *mod = regroove_create(path, 48000.0);
    if (!mod) {
        return -1;
    }

    // Lock audio and assign new module
    SDL_LockAudio();
    state->player = mod;
    SDL_UnlockAudio();

    // Update state
    state->num_channels = regroove_get_num_channels(mod);
    state->paused = 1;

    // Set callbacks if provided
    if (callbacks) {
        regroove_set_callbacks(mod, callbacks);
    }

    // Pause audio
    SDL_PauseAudio(1);

    return 0;
}

// Free common state
void regroove_common_destroy(RegrooveCommonState *state) {
    if (!state) return;

    // Safely destroy player
    if (state->player) {
        SDL_LockAudio();
        Regroove *tmp = state->player;
        state->player = NULL;
        SDL_UnlockAudio();
        regroove_destroy(tmp);
    }

    // Destroy input mappings
    if (state->input_mappings) {
        input_mappings_destroy(state->input_mappings);
    }

    // Destroy file list
    if (state->file_list) {
        regroove_filelist_destroy(state->file_list);
    }

    free(state);
}

// Common control functions (using centralized state)
void regroove_common_play_pause(RegrooveCommonState *state, int play) {
    if (!state || !state->player) return;

    state->paused = !play;
    SDL_PauseAudio(!play);
}

void regroove_common_retrigger(RegrooveCommonState *state) {
    if (!state || !state->player) return;
    regroove_retrigger_pattern(state->player);
}

void regroove_common_next_order(RegrooveCommonState *state) {
    if (!state || !state->player) return;
    regroove_queue_next_order(state->player);
}

void regroove_common_prev_order(RegrooveCommonState *state) {
    if (!state || !state->player) return;
    regroove_queue_prev_order(state->player);
}

void regroove_common_loop_till_row(RegrooveCommonState *state) {
    if (!state || !state->player) return;
    regroove_loop_till_row(state->player, regroove_get_current_row(state->player));
}

void regroove_common_halve_loop(RegrooveCommonState *state) {
    if (!state || !state->player) return;

    int rows = regroove_get_custom_loop_rows(state->player) > 0 ?
        regroove_get_custom_loop_rows(state->player) :
        regroove_get_full_pattern_rows(state->player);
    int halved = rows / 2 < 1 ? 1 : rows / 2;
    regroove_set_custom_loop_rows(state->player, halved);
}

void regroove_common_full_loop(RegrooveCommonState *state) {
    if (!state || !state->player) return;
    regroove_set_custom_loop_rows(state->player, 0);
}

void regroove_common_pattern_mode_toggle(RegrooveCommonState *state) {
    if (!state || !state->player) return;

    int new_mode = !regroove_get_pattern_mode(state->player);
    regroove_pattern_mode(state->player, new_mode);
}

void regroove_common_channel_mute(RegrooveCommonState *state, int channel) {
    if (!state || !state->player) return;
    if (channel < 0 || channel >= state->num_channels) return;

    regroove_toggle_channel_mute(state->player, channel);
}

void regroove_common_mute_all(RegrooveCommonState *state) {
    if (!state || !state->player) return;
    regroove_mute_all(state->player);
}

void regroove_common_unmute_all(RegrooveCommonState *state) {
    if (!state || !state->player) return;
    regroove_unmute_all(state->player);
}

void regroove_common_pitch_up(RegrooveCommonState *state) {
    if (!state || !state->player) return;

    if (state->pitch < 3.0) {
        state->pitch += 0.01;
        regroove_set_pitch(state->player, state->pitch);
    }
}

void regroove_common_pitch_down(RegrooveCommonState *state) {
    if (!state || !state->player) return;

    if (state->pitch > 0.25) {
        state->pitch -= 0.01;
        regroove_set_pitch(state->player, state->pitch);
    }
}

void regroove_common_set_pitch(RegrooveCommonState *state, double pitch) {
    if (!state || !state->player) return;

    state->pitch = pitch;
    if (state->pitch < 0.25) state->pitch = 0.25;
    if (state->pitch > 3.0) state->pitch = 3.0;

    regroove_set_pitch(state->player, state->pitch);
}
