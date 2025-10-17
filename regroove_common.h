#ifndef REGROOVE_COMMON_H
#define REGROOVE_COMMON_H

#include <stddef.h>
#include "regroove.h"
#include "input_mappings.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum path length
#define COMMON_MAX_PATH 1024
#define COMMON_MAX_FILES 4096

// File list management
typedef struct {
    char **filenames;     // Array of filenames (not full paths)
    int count;
    int current_index;
    char directory[COMMON_MAX_PATH];  // Directory path (normalized, no trailing slash)
} RegrooveFileList;

// Initialize file list
RegrooveFileList* regroove_filelist_create(void);

// Load files from directory (handles trailing slash automatically)
int regroove_filelist_load(RegrooveFileList *list, const char *dir_path);

// Get current file's full path
const char* regroove_filelist_get_current_path(RegrooveFileList *list, char *buffer, size_t bufsize);

// Navigate file list
void regroove_filelist_next(RegrooveFileList *list);
void regroove_filelist_prev(RegrooveFileList *list);

// Free file list
void regroove_filelist_destroy(RegrooveFileList *list);

// Common playback state
typedef struct {
    Regroove *player;
    InputMappings *input_mappings;
    RegrooveFileList *file_list;
    int paused;
    int num_channels;
    double pitch;
} RegrooveCommonState;

// Initialize common state
RegrooveCommonState* regroove_common_create(void);

// Load input mappings from .ini file (with fallback to defaults)
int regroove_common_load_mappings(RegrooveCommonState *state, const char *ini_path);

// Load a module file safely (handles audio locking)
int regroove_common_load_module(RegrooveCommonState *state, const char *path,
                                struct RegrooveCallbacks *callbacks);

// Free common state
void regroove_common_destroy(RegrooveCommonState *state);

// Common control functions (using centralized state)
void regroove_common_play_pause(RegrooveCommonState *state, int play);
void regroove_common_retrigger(RegrooveCommonState *state);
void regroove_common_next_order(RegrooveCommonState *state);
void regroove_common_prev_order(RegrooveCommonState *state);
void regroove_common_loop_till_row(RegrooveCommonState *state);
void regroove_common_halve_loop(RegrooveCommonState *state);
void regroove_common_full_loop(RegrooveCommonState *state);
void regroove_common_pattern_mode_toggle(RegrooveCommonState *state);
void regroove_common_channel_mute(RegrooveCommonState *state, int channel);
void regroove_common_mute_all(RegrooveCommonState *state);
void regroove_common_unmute_all(RegrooveCommonState *state);
void regroove_common_pitch_up(RegrooveCommonState *state);
void regroove_common_pitch_down(RegrooveCommonState *state);
void regroove_common_set_pitch(RegrooveCommonState *state, double pitch);

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_COMMON_H
