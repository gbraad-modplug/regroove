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
        strcmp(ext, ".mmdc") == 0 || strcmp(ext, ".rgx") == 0  ||
        strcmp(ext, ".mptm") == 0
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
    state->device_config.midi_device_0 = -1;      // Not configured
    state->device_config.midi_device_1 = -1;      // Not configured
    state->device_config.audio_device = -1;       // Default device
    state->device_config.midi_output_device = -1; // Disabled
    state->device_config.midi_output_note_duration = 1; // Hold notes (default)

    // Initialize default effect parameters (same as regroove_effects_create)
    state->device_config.fx_distortion_drive = 0.5f;
    state->device_config.fx_distortion_mix = 0.5f;
    state->device_config.fx_filter_cutoff = 1.0f;
    state->device_config.fx_filter_resonance = 0.0f;
    state->device_config.fx_eq_low = 0.5f;
    state->device_config.fx_eq_mid = 0.5f;
    state->device_config.fx_eq_high = 0.5f;
    state->device_config.fx_compressor_threshold = 0.4f;
    state->device_config.fx_compressor_ratio = 0.4f;
    state->device_config.fx_compressor_attack = 0.05f;
    state->device_config.fx_compressor_release = 0.5f;
    state->device_config.fx_compressor_makeup = 0.65f;
    state->device_config.fx_delay_time = 0.375f;
    state->device_config.fx_delay_feedback = 0.4f;
    state->device_config.fx_delay_mix = 0.3f;

    // Initialize metadata
    state->metadata = regroove_metadata_create();
    state->current_module_path[0] = '\0';

    // Initialize performance
    state->performance = regroove_performance_create();

    // Initialize phrase engine
    state->phrase = regroove_phrase_create();
    if (state->phrase && state->metadata) {
        regroove_phrase_set_metadata(state->phrase, state->metadata);
    }

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
                } else if (strcmp(key, "midi_output_device") == 0) {
                    state->device_config.midi_output_device = atoi(value);
                } else if (strcmp(key, "midi_output_note_duration") == 0) {
                    state->device_config.midi_output_note_duration = atoi(value);
                } else if (strcmp(key, "fx_distortion_drive") == 0) {
                    state->device_config.fx_distortion_drive = atof(value);
                } else if (strcmp(key, "fx_distortion_mix") == 0) {
                    state->device_config.fx_distortion_mix = atof(value);
                } else if (strcmp(key, "fx_filter_cutoff") == 0) {
                    state->device_config.fx_filter_cutoff = atof(value);
                } else if (strcmp(key, "fx_filter_resonance") == 0) {
                    state->device_config.fx_filter_resonance = atof(value);
                } else if (strcmp(key, "fx_eq_low") == 0) {
                    state->device_config.fx_eq_low = atof(value);
                } else if (strcmp(key, "fx_eq_mid") == 0) {
                    state->device_config.fx_eq_mid = atof(value);
                } else if (strcmp(key, "fx_eq_high") == 0) {
                    state->device_config.fx_eq_high = atof(value);
                } else if (strcmp(key, "fx_compressor_threshold") == 0) {
                    state->device_config.fx_compressor_threshold = atof(value);
                } else if (strcmp(key, "fx_compressor_ratio") == 0) {
                    state->device_config.fx_compressor_ratio = atof(value);
                } else if (strcmp(key, "fx_compressor_attack") == 0) {
                    state->device_config.fx_compressor_attack = atof(value);
                } else if (strcmp(key, "fx_compressor_release") == 0) {
                    state->device_config.fx_compressor_release = atof(value);
                } else if (strcmp(key, "fx_compressor_makeup") == 0) {
                    state->device_config.fx_compressor_makeup = atof(value);
                } else if (strcmp(key, "fx_delay_time") == 0) {
                    state->device_config.fx_delay_time = atof(value);
                } else if (strcmp(key, "fx_delay_feedback") == 0) {
                    state->device_config.fx_delay_feedback = atof(value);
                } else if (strcmp(key, "fx_delay_mix") == 0) {
                    state->device_config.fx_delay_mix = atof(value);
                }
            }
        }

        fclose(f);
    }

    return 0;
}

// Helper: Check if path is .rgx file
static int is_rgx_file(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;

    char ext[16];
    snprintf(ext, sizeof(ext), "%s", dot);
    for (char *p = ext; *p; ++p) *p = tolower(*p);

    return strcmp(ext, ".rgx") == 0;
}

// Helper: Get module path from .rgx file
static int get_module_path_from_rgx(const char *rgx_path, char *module_path, size_t module_path_size) {
    if (!rgx_path || !module_path) return -1;

    // Load the .rgx file to get the referenced module filename
    RegrooveMetadata *temp_meta = regroove_metadata_create();
    if (!temp_meta) return -1;

    if (regroove_metadata_load(temp_meta, rgx_path) != 0) {
        regroove_metadata_destroy(temp_meta);
        return -1;
    }

    // Get the directory from rgx_path
    char dir[COMMON_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", rgx_path);
    char *last_slash = strrchr(dir, '/');
    if (!last_slash) last_slash = strrchr(dir, '\\');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        strcpy(dir, ".");
    }

    // Build full path to module file
    snprintf(module_path, module_path_size, "%s/%s", dir, temp_meta->module_file);

    regroove_metadata_destroy(temp_meta);
    return 0;
}

// Load a module file safely (handles audio locking)
int regroove_common_load_module(RegrooveCommonState *state, const char *path,
                                struct RegrooveCallbacks *callbacks) {
    if (!state || !path) return -1;

    // If this is an .rgx file, load the referenced module instead
    char actual_module_path[COMMON_MAX_PATH];
    const char *module_to_load = path;

    if (is_rgx_file(path)) {
        if (get_module_path_from_rgx(path, actual_module_path, sizeof(actual_module_path)) != 0) {
            fprintf(stderr, "Failed to get module path from .rgx file: %s\n", path);
            return -1;
        }
        module_to_load = actual_module_path;
        printf("Loading module '%s' from .rgx file '%s'\n", module_to_load, path);
    }

    // Lock audio before destroying old module
    if (state->audio_device_id) {
        SDL_LockAudioDevice(state->audio_device_id);
    }
    if (state->player) {
        Regroove *old = state->player;
        state->player = NULL;
        if (state->audio_device_id) {
            SDL_UnlockAudioDevice(state->audio_device_id);
        }
        regroove_destroy(old);
    } else {
        if (state->audio_device_id) {
            SDL_UnlockAudioDevice(state->audio_device_id);
        }
    }

    // Create new module (use the resolved module path)
    Regroove *mod = regroove_create(module_to_load, 48000.0);
    if (!mod) {
        return -1;
    }

    // Lock audio and assign new module
    if (state->audio_device_id) {
        SDL_LockAudioDevice(state->audio_device_id);
    }
    state->player = mod;
    if (state->audio_device_id) {
        SDL_UnlockAudioDevice(state->audio_device_id);
    }

    // Update state
    state->num_channels = regroove_get_num_channels(mod);
    state->paused = 1;

    // Store current module path for .rgx saving (use the actual module path, not .rgx)
    snprintf(state->current_module_path, COMMON_MAX_PATH, "%s", module_to_load);

    // Load .rgx metadata
    if (state->metadata) {
        // Clear old metadata
        regroove_metadata_destroy(state->metadata);
        state->metadata = regroove_metadata_create();

        // ALWAYS clear old performance events when loading a new module
        if (state->performance) {
            regroove_performance_clear_events(state->performance);
            regroove_performance_reset(state->performance);
        }

        // Stop all active phrases and reconnect phrase engine to new metadata
        if (state->phrase) {
            regroove_phrase_stop_all(state->phrase);
            regroove_phrase_set_metadata(state->phrase, state->metadata);
        }

        // Only load .rgx file if user explicitly loaded one
        // Loading a .mod file does NOT automatically load the .rgx (allows starting fresh performances)
        if (is_rgx_file(path)) {
            // User loaded an .rgx file directly - load it
            char rgx_path[COMMON_MAX_PATH];
            snprintf(rgx_path, sizeof(rgx_path), "%s", path);

            if (regroove_metadata_load(state->metadata, rgx_path) == 0) {
                // Successfully loaded .rgx metadata
                printf("Loaded metadata from %s\n", rgx_path);

                // Load performance events from the same .rgx file
                if (state->performance) {
                    if (regroove_performance_load(state->performance, rgx_path) == 0) {
                        int event_count = regroove_performance_get_event_count(state->performance);
                        if (event_count > 0) {
                            printf("Loaded %d performance events from %s\n", event_count, rgx_path);
                        }
                    }
                }
            }
        } else {
            // User loaded a module file directly - start with empty metadata
            // Store the module filename in metadata for when we save
            const char *filename = strrchr(module_to_load, '/');
            if (!filename) filename = strrchr(module_to_load, '\\');
            if (!filename) filename = module_to_load;
            else filename++; // Skip the separator

            snprintf(state->metadata->module_file, RGX_MAX_FILEPATH, "%s", filename);
            printf("Loaded module %s (no .rgx loaded, starting fresh)\n", filename);
        }
    }

    // Set callbacks if provided
    if (callbacks) {
        regroove_set_callbacks(mod, callbacks);
    }

    // Pause audio
    if (state->audio_device_id) {
        SDL_PauseAudioDevice(state->audio_device_id, 1);
    }

    return 0;
}

// Free common state
void regroove_common_destroy(RegrooveCommonState *state) {
    if (!state) return;

    // Safely destroy player
    if (state->player) {
        if (state->audio_device_id) {
            SDL_LockAudioDevice(state->audio_device_id);
        }
        Regroove *tmp = state->player;
        state->player = NULL;
        if (state->audio_device_id) {
            SDL_UnlockAudioDevice(state->audio_device_id);
        }
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

    // Destroy metadata
    if (state->metadata) {
        regroove_metadata_destroy(state->metadata);
    }

    // Destroy performance
    if (state->performance) {
        regroove_performance_destroy(state->performance);
    }

    // Destroy phrase engine
    if (state->phrase) {
        regroove_phrase_destroy(state->phrase);
    }

    free(state);
}

// Common control functions (using centralized state)
void regroove_common_play_pause(RegrooveCommonState *state, int play) {
    if (!state || !state->player) return;

    if (play && state->paused) {
        // Starting playback - check for performance mode
        // BUT: Don't enable performance playback if this is from a phrase
        if (state->performance && state->phrase && !regroove_phrase_is_active(state->phrase)) {
            int event_count = regroove_performance_get_event_count(state->performance);
            if (event_count > 0) {
                // Reset song position to order 0 when starting performance playback
                regroove_jump_to_order(state->player, 0);
                // Enable performance playback only if there are events
                regroove_performance_set_playback(state->performance, 1);
            }
        }
    } else if (!play && !state->paused) {
        // Stopping playback - reset performance
        if (state->performance) {
            regroove_performance_set_playback(state->performance, 0);
            regroove_performance_reset(state->performance);
        }
    }

    state->paused = !play;
    if (state->audio_device_id) {
        SDL_PauseAudioDevice(state->audio_device_id, !play);
    }
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

// Save device configuration to existing INI file
// Simple approach: append [devices] section if missing, or rewrite entire file if it exists
int regroove_common_save_device_config(RegrooveCommonState *state, const char *filepath) {
    if (!state || !filepath) return -1;

    // Check if file exists and if [devices] section exists
    FILE *f_check = fopen(filepath, "r");
    int has_devices_section = 0;

    if (f_check) {
        char line[512];
        while (fgets(line, sizeof(line), f_check)) {
            if (strstr(line, "[devices]")) {
                has_devices_section = 1;
                break;
            }
        }
        fclose(f_check);
    }

    if (!f_check) {
        // File doesn't exist, create it with device config only
        FILE *f = fopen(filepath, "w");
        if (!f) return -1;

        fprintf(f, "# Regroove Configuration File\n\n");
        fprintf(f, "[devices]\n");
        fprintf(f, "midi_device_0 = %d\n", state->device_config.midi_device_0);
        fprintf(f, "midi_device_1 = %d\n", state->device_config.midi_device_1);
        fprintf(f, "audio_device = %d\n", state->device_config.audio_device);
        fprintf(f, "midi_output_device = %d\n", state->device_config.midi_output_device);
        fprintf(f, "midi_output_note_duration = %d\n", state->device_config.midi_output_note_duration);
        fprintf(f, "fx_distortion_drive = %.2f\n", state->device_config.fx_distortion_drive);
        fprintf(f, "fx_distortion_mix = %.2f\n", state->device_config.fx_distortion_mix);
        fprintf(f, "fx_filter_cutoff = %.2f\n", state->device_config.fx_filter_cutoff);
        fprintf(f, "fx_filter_resonance = %.2f\n", state->device_config.fx_filter_resonance);
        fprintf(f, "fx_eq_low = %.2f\n", state->device_config.fx_eq_low);
        fprintf(f, "fx_eq_mid = %.2f\n", state->device_config.fx_eq_mid);
        fprintf(f, "fx_eq_high = %.2f\n", state->device_config.fx_eq_high);
        fprintf(f, "fx_compressor_threshold = %.2f\n", state->device_config.fx_compressor_threshold);
        fprintf(f, "fx_compressor_ratio = %.2f\n", state->device_config.fx_compressor_ratio);
        fprintf(f, "fx_compressor_attack = %.2f\n", state->device_config.fx_compressor_attack);
        fprintf(f, "fx_compressor_release = %.2f\n", state->device_config.fx_compressor_release);
        fprintf(f, "fx_compressor_makeup = %.2f\n", state->device_config.fx_compressor_makeup);
        fprintf(f, "fx_delay_time = %.2f\n", state->device_config.fx_delay_time);
        fprintf(f, "fx_delay_feedback = %.2f\n", state->device_config.fx_delay_feedback);
        fprintf(f, "fx_delay_mix = %.2f\n", state->device_config.fx_delay_mix);
        fprintf(f, "\n");

        fclose(f);
        return 0;
    }

    if (!has_devices_section) {
        // File exists but no [devices] section - append it
        FILE *f = fopen(filepath, "a");
        if (!f) return -1;

        fprintf(f, "\n[devices]\n");
        fprintf(f, "midi_device_0 = %d\n", state->device_config.midi_device_0);
        fprintf(f, "midi_device_1 = %d\n", state->device_config.midi_device_1);
        fprintf(f, "audio_device = %d\n", state->device_config.audio_device);
        fprintf(f, "midi_output_device = %d\n", state->device_config.midi_output_device);
        fprintf(f, "midi_output_note_duration = %d\n", state->device_config.midi_output_note_duration);
        fprintf(f, "fx_distortion_drive = %.2f\n", state->device_config.fx_distortion_drive);
        fprintf(f, "fx_distortion_mix = %.2f\n", state->device_config.fx_distortion_mix);
        fprintf(f, "fx_filter_cutoff = %.2f\n", state->device_config.fx_filter_cutoff);
        fprintf(f, "fx_filter_resonance = %.2f\n", state->device_config.fx_filter_resonance);
        fprintf(f, "fx_eq_low = %.2f\n", state->device_config.fx_eq_low);
        fprintf(f, "fx_eq_mid = %.2f\n", state->device_config.fx_eq_mid);
        fprintf(f, "fx_eq_high = %.2f\n", state->device_config.fx_eq_high);
        fprintf(f, "fx_compressor_threshold = %.2f\n", state->device_config.fx_compressor_threshold);
        fprintf(f, "fx_compressor_ratio = %.2f\n", state->device_config.fx_compressor_ratio);
        fprintf(f, "fx_compressor_attack = %.2f\n", state->device_config.fx_compressor_attack);
        fprintf(f, "fx_compressor_release = %.2f\n", state->device_config.fx_compressor_release);
        fprintf(f, "fx_compressor_makeup = %.2f\n", state->device_config.fx_compressor_makeup);
        fprintf(f, "fx_delay_time = %.2f\n", state->device_config.fx_delay_time);
        fprintf(f, "fx_delay_feedback = %.2f\n", state->device_config.fx_delay_feedback);
        fprintf(f, "fx_delay_mix = %.2f\n", state->device_config.fx_delay_mix);

        fclose(f);
        return 0;
    }

    // File exists and has [devices] section - need to update it
    // Read entire file, update [devices] section, rewrite
    FILE *f_read = fopen(filepath, "r");
    if (!f_read) return -1;

    // Use a temp file for safety
    char temp_path[COMMON_MAX_PATH + 4];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", filepath);
    FILE *f_write = fopen(temp_path, "w");
    if (!f_write) {
        fclose(f_read);
        return -1;
    }

    char line[512];
    int in_devices_section = 0;
    int devices_written = 0;

    while (fgets(line, sizeof(line), f_read)) {
        // Check for section headers
        if (line[0] == '[') {
            // Exiting devices section? Write the new values if not done yet
            if (in_devices_section && !devices_written) {
                fprintf(f_write, "midi_device_0 = %d\n", state->device_config.midi_device_0);
                fprintf(f_write, "midi_device_1 = %d\n", state->device_config.midi_device_1);
                fprintf(f_write, "audio_device = %d\n", state->device_config.audio_device);
                fprintf(f_write, "midi_output_device = %d\n", state->device_config.midi_output_device);
                fprintf(f_write, "fx_distortion_drive = %.2f\n", state->device_config.fx_distortion_drive);
                fprintf(f_write, "fx_distortion_mix = %.2f\n", state->device_config.fx_distortion_mix);
                fprintf(f_write, "fx_filter_cutoff = %.2f\n", state->device_config.fx_filter_cutoff);
                fprintf(f_write, "fx_filter_resonance = %.2f\n", state->device_config.fx_filter_resonance);
                fprintf(f_write, "fx_eq_low = %.2f\n", state->device_config.fx_eq_low);
                fprintf(f_write, "fx_eq_mid = %.2f\n", state->device_config.fx_eq_mid);
                fprintf(f_write, "fx_eq_high = %.2f\n", state->device_config.fx_eq_high);
                fprintf(f_write, "fx_compressor_threshold = %.2f\n", state->device_config.fx_compressor_threshold);
                fprintf(f_write, "fx_compressor_ratio = %.2f\n", state->device_config.fx_compressor_ratio);
                fprintf(f_write, "fx_compressor_attack = %.2f\n", state->device_config.fx_compressor_attack);
                fprintf(f_write, "fx_compressor_release = %.2f\n", state->device_config.fx_compressor_release);
                fprintf(f_write, "fx_compressor_makeup = %.2f\n", state->device_config.fx_compressor_makeup);
                fprintf(f_write, "fx_delay_time = %.2f\n", state->device_config.fx_delay_time);
                fprintf(f_write, "fx_delay_feedback = %.2f\n", state->device_config.fx_delay_feedback);
                fprintf(f_write, "fx_delay_mix = %.2f\n", state->device_config.fx_delay_mix);
                devices_written = 1;
            }
            in_devices_section = (strstr(line, "[devices]") != NULL);
            fprintf(f_write, "%s", line);
        } else if (in_devices_section) {
            // In devices section
            char *trimmed = trim_whitespace(line);
            if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
                // Comment or empty line - preserve it
                fprintf(f_write, "%s", line);
            } else if (!devices_written) {
                // First non-comment line in devices section - write our values and skip old ones
                fprintf(f_write, "midi_device_0 = %d\n", state->device_config.midi_device_0);
                fprintf(f_write, "midi_device_1 = %d\n", state->device_config.midi_device_1);
                fprintf(f_write, "audio_device = %d\n", state->device_config.audio_device);
                fprintf(f_write, "midi_output_device = %d\n", state->device_config.midi_output_device);
                fprintf(f_write, "fx_distortion_drive = %.2f\n", state->device_config.fx_distortion_drive);
                fprintf(f_write, "fx_distortion_mix = %.2f\n", state->device_config.fx_distortion_mix);
                fprintf(f_write, "fx_filter_cutoff = %.2f\n", state->device_config.fx_filter_cutoff);
                fprintf(f_write, "fx_filter_resonance = %.2f\n", state->device_config.fx_filter_resonance);
                fprintf(f_write, "fx_eq_low = %.2f\n", state->device_config.fx_eq_low);
                fprintf(f_write, "fx_eq_mid = %.2f\n", state->device_config.fx_eq_mid);
                fprintf(f_write, "fx_eq_high = %.2f\n", state->device_config.fx_eq_high);
                fprintf(f_write, "fx_compressor_threshold = %.2f\n", state->device_config.fx_compressor_threshold);
                fprintf(f_write, "fx_compressor_ratio = %.2f\n", state->device_config.fx_compressor_ratio);
                fprintf(f_write, "fx_compressor_attack = %.2f\n", state->device_config.fx_compressor_attack);
                fprintf(f_write, "fx_compressor_release = %.2f\n", state->device_config.fx_compressor_release);
                fprintf(f_write, "fx_compressor_makeup = %.2f\n", state->device_config.fx_compressor_makeup);
                fprintf(f_write, "fx_delay_time = %.2f\n", state->device_config.fx_delay_time);
                fprintf(f_write, "fx_delay_feedback = %.2f\n", state->device_config.fx_delay_feedback);
                fprintf(f_write, "fx_delay_mix = %.2f\n", state->device_config.fx_delay_mix);
                devices_written = 1;
                // Skip the old line (don't write it)
            }
            // Skip subsequent device lines (they're old values)
        } else {
            // Not in devices section - preserve line
            fprintf(f_write, "%s", line);
        }
    }

    fclose(f_read);
    fclose(f_write);

    // Replace original with temp file
    remove(filepath);
    rename(temp_path, filepath);

    return 0;
}

// Save default configuration to INI file
int regroove_common_save_default_config(const char *filepath) {
    if (!filepath) return -1;

    FILE *f = fopen(filepath, "w");
    if (!f) return -1;

    fprintf(f, "# Regroove Configuration File\n");
    fprintf(f, "# This file contains input mappings and device configuration\n\n");

    // Device configuration section
    fprintf(f, "[devices]\n");
    fprintf(f, "# MIDI device ports (-1 = not configured)\n");
    fprintf(f, "midi_device_0 = -1\n");
    fprintf(f, "midi_device_1 = -1\n");
    fprintf(f, "# Audio device (-1 = default)\n");
    fprintf(f, "audio_device = -1\n");
    fprintf(f, "midi_output_device = -1\n\n");
    fprintf(f, "# Default effect parameters (applied when loading songs)\n");
    fprintf(f, "fx_distortion_drive = 0.50\n");
    fprintf(f, "fx_distortion_mix = 0.50\n");
    fprintf(f, "fx_filter_cutoff = 1.00\n");
    fprintf(f, "fx_filter_resonance = 0.00\n");
    fprintf(f, "fx_eq_low = 0.50\n");
    fprintf(f, "fx_eq_mid = 0.50\n");
    fprintf(f, "fx_eq_high = 0.50\n");
    fprintf(f, "fx_compressor_threshold = 0.40\n");
    fprintf(f, "fx_compressor_ratio = 0.40\n");
    fprintf(f, "fx_compressor_attack = 0.05\n");
    fprintf(f, "fx_compressor_release = 0.50\n");
    fprintf(f, "fx_compressor_makeup = 0.65\n");
    fprintf(f, "fx_delay_time = 0.375\n");
    fprintf(f, "fx_delay_feedback = 0.40\n");
    fprintf(f, "fx_delay_mix = 0.30\n\n");

    // MIDI mappings section
    fprintf(f, "[midi]\n");
    fprintf(f, "# Format: cc<number> = action[,parameter[,continuous[,device_id]]]\n");
    fprintf(f, "# continuous: 1 for continuous controls (faders/knobs), 0 for buttons (default)\n");
    fprintf(f, "# device_id: -1 for any device (default), 0 for device 0, 1 for device 1\n");
    fprintf(f, "# Buttons trigger at MIDI value >= 64, continuous controls respond to all values\n\n");
    fprintf(f, "# Transport controls\n");
    fprintf(f, "cc41 = play,0,0,-1\n");
    fprintf(f, "cc42 = stop,0,0,-1\n");
    fprintf(f, "cc46 = pattern_mode_toggle,0,0,-1\n");
    fprintf(f, "cc44 = next_order,0,0,-1\n");
    fprintf(f, "cc43 = prev_order,0,0,-1\n\n");
    fprintf(f, "# File browser controls\n");
    fprintf(f, "cc60 = file_load,0,0,-1\n");
    fprintf(f, "cc61 = file_prev,0,0,-1\n");
    fprintf(f, "cc62 = file_next,0,0,-1\n\n");
    fprintf(f, "# Channel solo (CC 32-39)\n");
    for (int i = 0; i < 8; i++) {
        fprintf(f, "cc%d = channel_solo,%d,0,-1\n", 32 + i, i);
    }
    fprintf(f, "\n# Channel mute (CC 48-55)\n");
    for (int i = 0; i < 8; i++) {
        fprintf(f, "cc%d = channel_mute,%d,0,-1\n", 48 + i, i);
    }
    fprintf(f, "\n# Channel volume (CC 0-7) - continuous controls\n");
    for (int i = 0; i < 8; i++) {
        fprintf(f, "cc%d = channel_volume,%d,1,-1\n", i, i);
    }

    // Trigger pads section
    fprintf(f, "\n[trigger_pads]\n");
    fprintf(f, "# Format: pad<number> = midi_note,action[,parameter[,device_id]]\n");
    fprintf(f, "# midi_note: MIDI note number (0-127, -1 = no MIDI mapping)\n");
    fprintf(f, "# device_id: -1 for any device (default), 0 for device 0, 1 for device 1\n");
    fprintf(f, "# Example trigger pad mappings (configure based on your MIDI controller):\n");
    fprintf(f, "# pad0 = 36,play_pause,0,-1   # C1 - Play/Pause\n");
    fprintf(f, "# pad1 = 37,stop,0,-1          # C#1 - Stop\n");
    fprintf(f, "# pad2 = 38,retrigger,0,-1     # D1 - Retrigger\n");
    fprintf(f, "# pad3 = 39,pattern_mode_toggle,0,-1  # D#1 - Loop toggle\n");
    fprintf(f, "# Uncomment and configure pads 0-15 to match your hardware controller\n\n");

    // Keyboard mappings section
    fprintf(f, "[keyboard]\n");
    fprintf(f, "# Format: key<char> = action[,parameter]\n");
    fprintf(f, "# Special keys use key_<name> format (key_space, key_esc, key_enter)\n\n");
    fprintf(f, "# Transport controls\n");
    fprintf(f, "key_space = play_pause,0\n");
    fprintf(f, "keyr = retrigger,0\n");
    fprintf(f, "keyR = retrigger,0\n");
    fprintf(f, "keyN = next_order,0\n");
    fprintf(f, "keyn = next_order,0\n");
    fprintf(f, "keyP = prev_order,0\n");
    fprintf(f, "keyp = prev_order,0\n\n");
    fprintf(f, "# Loop controls\n");
    fprintf(f, "keyj = loop_till_row,0\n");
    fprintf(f, "keyJ = loop_till_row,0\n");
    fprintf(f, "keyh = halve_loop,0\n");
    fprintf(f, "keyH = halve_loop,0\n");
    fprintf(f, "keyf = full_loop,0\n");
    fprintf(f, "keyF = full_loop,0\n");
    fprintf(f, "keyS = pattern_mode_toggle,0\n");
    fprintf(f, "keys = pattern_mode_toggle,0\n\n");
    fprintf(f, "# Channel controls\n");
    fprintf(f, "keym = mute_all,0\n");
    fprintf(f, "keyM = mute_all,0\n");
    fprintf(f, "keyu = unmute_all,0\n");
    fprintf(f, "keyU = unmute_all,0\n");
    fprintf(f, "key1 = channel_mute,0\n");
    fprintf(f, "key2 = channel_mute,1\n");
    fprintf(f, "key3 = channel_mute,2\n");
    fprintf(f, "key4 = channel_mute,3\n");
    fprintf(f, "key5 = channel_mute,4\n");
    fprintf(f, "key6 = channel_mute,5\n");
    fprintf(f, "key7 = channel_mute,6\n");
    fprintf(f, "key8 = channel_mute,7\n\n");
    fprintf(f, "# Pitch control\n");
    fprintf(f, "key_plus = pitch_up,0\n");
    fprintf(f, "key_equals = pitch_up,0\n");
    fprintf(f, "key_minus = pitch_down,0\n\n");
    fprintf(f, "# File browser\n");
    fprintf(f, "key_lbracket = file_prev,0\n");
    fprintf(f, "key_rbracket = file_next,0\n");
    fprintf(f, "key_enter = file_load,0\n\n");
    fprintf(f, "# Application control\n");
    fprintf(f, "keyq = quit,0\n");
    fprintf(f, "keyQ = quit,0\n");
    fprintf(f, "key_esc = quit,0\n\n");
    fprintf(f, "# Trigger pad keyboard shortcuts\n");
    fprintf(f, "# Uncomment and configure to trigger pads from keyboard:\n");
    fprintf(f, "# Format: key<char> = trigger_pad,<pad_number>\n");
    fprintf(f, "# NOTE: Numpad keys work in GUI only, not in TUI (terminal raw mode limitation)\n");
    fprintf(f, "# Example using numpad keys (GUI only):\n");
    for (int i = 0; i < 10; i++) {
        fprintf(f, "# key_kp%d = trigger_pad,%d   # Numpad %d triggers pad %d\n", i, i, i, i+1);
    }
    fprintf(f, "\n# Example using other keys (works in both GUI and TUI):\n");
    fprintf(f, "# keyz = trigger_pad,0   # Z key triggers pad 1\n");
    fprintf(f, "# keyx = trigger_pad,1   # X key triggers pad 2\n");
    fprintf(f, "# keyc = trigger_pad,2   # C key triggers pad 3\n");
    fprintf(f, "# keyv = trigger_pad,3   # V key triggers pad 4\n");

    fclose(f);
    return 0;
}

// Save metadata and performance to .rgx file
int regroove_common_save_rgx(RegrooveCommonState *state) {
    if (!state || !state->metadata) return -1;
    if (state->current_module_path[0] == '\0') return -1;

    // Get .rgx path from module path
    char rgx_path[COMMON_MAX_PATH];
    regroove_metadata_get_rgx_path(state->current_module_path, rgx_path, sizeof(rgx_path));

    // Save metadata first (creates/overwrites the file)
    if (regroove_metadata_save(state->metadata, rgx_path) != 0) {
        fprintf(stderr, "Failed to save metadata to %s\n", rgx_path);
        return -1;
    }

    // Append performance data if there are events
    if (state->performance && regroove_performance_get_event_count(state->performance) > 0) {
        if (regroove_performance_save(state->performance, rgx_path) != 0) {
            fprintf(stderr, "Failed to save performance to %s\n", rgx_path);
            return -1;
        }
    }

    printf("Saved .rgx file: %s\n", rgx_path);
    return 0;
}

// Phrase completion callback - handles cleanup when phrase finishes
static void phrase_completion_callback(int phrase_index, void *userdata) {
    RegrooveCommonState *state = (RegrooveCommonState*)userdata;
    if (!state || !state->player) return;

    printf("Phrase %d completed - stopping playback, resetting to order 0, unmuting all channels\n", phrase_index + 1);

    // Stop playback
    if (state->audio_device_id) {
        SDL_PauseAudioDevice(state->audio_device_id, 1);
    }
    state->paused = 1;

    // Reset to order 0
    regroove_jump_to_order(state->player, 0);

    // Unmute all channels
    regroove_unmute_all(state->player);
}

// Phrase playback functions (wrappers around phrase engine)
void regroove_common_set_phrase_callback(RegrooveCommonState *state, PhraseActionCallback callback, void *userdata) {
    if (!state || !state->phrase) return;
    regroove_phrase_set_action_callback(state->phrase, callback, userdata);
    // Also set completion callback to handle cleanup
    regroove_phrase_set_completion_callback(state->phrase, phrase_completion_callback, state);
}

void regroove_common_trigger_phrase(RegrooveCommonState *state, int phrase_index) {
    if (!state || !state->phrase || !state->player) return;

    printf("Triggering phrase %d - resetting state\n", phrase_index + 1);

    // Reset to clean state before starting phrase
    // 1. Stop playback
    if (state->audio_device_id) {
        SDL_PauseAudioDevice(state->audio_device_id, 1);
    }
    state->paused = 1;

    // 2. Reset to order 0
    regroove_jump_to_order(state->player, 0);

    // 3. Unmute all channels (engine state)
    regroove_unmute_all(state->player);

    // 4. Trigger the phrase
    if (regroove_phrase_trigger(state->phrase, phrase_index) != 0) {
        return;  // Failed to trigger
    }

    // 5. Execute position 0 events immediately (before playback starts)
    //    This ensures channel solo, pattern jumps, etc. happen BEFORE audio rendering begins
    regroove_phrase_update(state->phrase);

    // 6. Process all pending commands (like channel solo) before starting playback
    if (state->player) {
        regroove_process_commands(state->player);
    }

    // Start playback for the phrase
    if (state->audio_device_id) {
        SDL_PauseAudioDevice(state->audio_device_id, 0);
    }
    state->paused = 0;
}

void regroove_common_update_phrases(RegrooveCommonState *state) {
    if (!state || !state->phrase) return;
    if (state->paused) return;  // Only update when playing

    regroove_phrase_update(state->phrase);
    // Completion handling is done via the completion callback
}

int regroove_common_phrase_is_active(const RegrooveCommonState *state) {
    if (!state || !state->phrase) return 0;
    return regroove_phrase_is_active(state->phrase);
}
