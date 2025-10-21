#include "regroove_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RegrooveMetadata* regroove_metadata_create(void) {
    RegrooveMetadata *meta = (RegrooveMetadata*)calloc(1, sizeof(RegrooveMetadata));
    if (!meta) return NULL;

    meta->version = 1;
    meta->module_file[0] = '\0';
    meta->pattern_meta_capacity = 64;
    meta->pattern_meta_count = 0;
    meta->pattern_meta = (RegroovePatternMeta*)calloc(meta->pattern_meta_capacity, sizeof(RegroovePatternMeta));

    if (!meta->pattern_meta) {
        free(meta);
        return NULL;
    }

    // Initialize song-specific trigger pads (S1-S16) to unmapped
    for (int i = 0; i < MAX_SONG_TRIGGER_PADS; i++) {
        meta->song_trigger_pads[i].action = ACTION_NONE;
        meta->song_trigger_pads[i].parameter = 0;
        meta->song_trigger_pads[i].midi_note = -1;  // Not mapped
        meta->song_trigger_pads[i].midi_device = -1; // Any device
        meta->song_trigger_pads[i].phrase_index = -1; // No phrase assigned
    }

    // Initialize phrases
    meta->phrase_count = 0;
    for (int i = 0; i < RGX_MAX_PHRASES; i++) {
        meta->phrases[i].name[0] = '\0';
        meta->phrases[i].step_count = 0;
    }

    // Initialize MIDI channel mappings to disabled (-2) by default
    meta->has_midi_mapping = 0;
    for (int i = 0; i < RGX_MAX_INSTRUMENTS; i++) {
        meta->instrument_midi_channels[i] = -2;  // Disabled (no MIDI output)
        meta->instrument_names[i][0] = '\0';  // Empty = use module's name
    }

    return meta;
}

void regroove_metadata_destroy(RegrooveMetadata *meta) {
    if (!meta) return;
    if (meta->pattern_meta) free(meta->pattern_meta);
    free(meta);
}

static void trim_whitespace(char *str) {
    if (!str) return;

    // Trim leading whitespace
    char *start = str;
    while (*start && (*start == ' ' || *start == '\t')) start++;

    // Trim trailing whitespace
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }

    // Move trimmed string to beginning
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

static void parse_key_value(char *line, char *key, size_t key_size, char *value, size_t value_size) {
    key[0] = '\0';
    value[0] = '\0';

    char *equals = strchr(line, '=');
    if (!equals) return;

    // Extract key
    size_t key_len = equals - line;
    if (key_len >= key_size) key_len = key_size - 1;
    strncpy(key, line, key_len);
    key[key_len] = '\0';
    trim_whitespace(key);

    // Extract value
    strncpy(value, equals + 1, value_size - 1);
    value[value_size - 1] = '\0';
    trim_whitespace(value);

    // Remove quotes from value if present
    if (value[0] == '"') {
        size_t len = strlen(value);
        if (len > 1 && value[len - 1] == '"') {
            value[len - 1] = '\0';
            memmove(value, value + 1, len);
        }
    }
}

int regroove_metadata_load(RegrooveMetadata *meta, const char *rgx_path) {
    if (!meta || !rgx_path) return -1;

    FILE *f = fopen(rgx_path, "r");
    if (!f) return -1;

    char line[1024];
    char section[128] = "";
    char key[256];
    char value[512];

    while (fgets(line, sizeof(line), f)) {
        trim_whitespace(line);

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') continue;

        // Section header
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                size_t len = end - line - 1;
                if (len >= sizeof(section)) len = sizeof(section) - 1;
                strncpy(section, line + 1, len);
                section[len] = '\0';
                trim_whitespace(section);
            }
            continue;
        }

        // Parse key=value
        parse_key_value(line, key, sizeof(key), value, sizeof(value));
        if (key[0] == '\0') continue;

        // Handle sections
        if (strcmp(section, "Regroove") == 0) {
            if (strcmp(key, "version") == 0) {
                meta->version = atoi(value);
            } else if (strcmp(key, "file") == 0) {
                strncpy(meta->module_file, value, RGX_MAX_FILEPATH - 1);
                meta->module_file[RGX_MAX_FILEPATH - 1] = '\0';
            }
        } else if (strcmp(section, "Patterns") == 0) {
            // Pattern description: pattern_0=description
            if (strncmp(key, "pattern_", 8) == 0) {
                int pattern_index = atoi(key + 8);
                regroove_metadata_set_pattern_desc(meta, pattern_index, value);
            }
        } else if (strcmp(section, "SongTriggerPads") == 0) {
            // Song pad configuration: pad_S1_action, pad_S1_parameter, etc.
            if (strncmp(key, "pad_S", 5) == 0) {
                int pad_index = atoi(key + 5) - 1;  // S1 = index 0
                if (pad_index >= 0 && pad_index < MAX_SONG_TRIGGER_PADS) {
                    if (strstr(key, "_action")) {
                        meta->song_trigger_pads[pad_index].action = parse_action(value);
                    } else if (strstr(key, "_parameter")) {
                        meta->song_trigger_pads[pad_index].parameter = atoi(value);
                    } else if (strstr(key, "_midi_note")) {
                        meta->song_trigger_pads[pad_index].midi_note = atoi(value);
                    } else if (strstr(key, "_midi_device")) {
                        meta->song_trigger_pads[pad_index].midi_device = atoi(value);
                    } else if (strstr(key, "_phrase")) {
                        meta->song_trigger_pads[pad_index].phrase_index = atoi(value);
                    }
                }
            }
        } else if (strcmp(section, "Phrases") == 0) {
            // Phrase configuration
            if (strcmp(key, "count") == 0) {
                meta->phrase_count = atoi(value);
                if (meta->phrase_count > RGX_MAX_PHRASES) meta->phrase_count = RGX_MAX_PHRASES;
            } else if (strncmp(key, "phrase_", 7) == 0) {
                int phrase_idx = atoi(key + 7);
                if (phrase_idx >= 0 && phrase_idx < RGX_MAX_PHRASES) {
                    Phrase *phrase = &meta->phrases[phrase_idx];

                    // Parse phrase properties
                    if (strstr(key, "_name")) {
                        strncpy(phrase->name, value, RGX_MAX_PHRASE_NAME - 1);
                        phrase->name[RGX_MAX_PHRASE_NAME - 1] = '\0';
                    } else if (strstr(key, "_steps")) {
                        phrase->step_count = atoi(value);
                        if (phrase->step_count > RGX_MAX_PHRASE_STEPS)
                            phrase->step_count = RGX_MAX_PHRASE_STEPS;
                    } else if (strstr(key, "_step_")) {
                        // Parse step index: phrase_0_step_1_action
                        char *step_ptr = strstr(key, "_step_");
                        if (step_ptr) {
                            int step_idx = atoi(step_ptr + 6);
                            if (step_idx >= 0 && step_idx < RGX_MAX_PHRASE_STEPS) {
                                PhraseStep *step = &phrase->steps[step_idx];

                                if (strstr(key, "_action")) {
                                    step->action = parse_action(value);
                                } else if (strstr(key, "_parameter")) {
                                    step->parameter = atoi(value);
                                } else if (strstr(key, "_value")) {
                                    step->value = atoi(value);
                                } else if (strstr(key, "_delay") || strstr(key, "_position")) {
                                    // Support both old "delay" and new "position" naming
                                    step->position_rows = atoi(value);
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(section, "MIDIMapping") == 0) {
            // MIDI mapping configuration: instrument_X_channel, instrument_X_name
            if (strncmp(key, "instrument_", 11) == 0) {
                int inst_idx = atoi(key + 11);
                if (inst_idx >= 0 && inst_idx < RGX_MAX_INSTRUMENTS) {
                    if (strstr(key, "_channel")) {
                        int channel = atoi(value);
                        if (channel >= -2 && channel <= 15) {
                            meta->instrument_midi_channels[inst_idx] = channel;
                            if (channel != -2) {
                                meta->has_midi_mapping = 1;
                            }
                        }
                    } else if (strstr(key, "_name")) {
                        strncpy(meta->instrument_names[inst_idx], value, RGX_MAX_INSTRUMENT_NAME - 1);
                        meta->instrument_names[inst_idx][RGX_MAX_INSTRUMENT_NAME - 1] = '\0';
                    }
                }
            }
        }
    }

    fclose(f);
    return 0;
}

int regroove_metadata_save(const RegrooveMetadata *meta, const char *rgx_path) {
    if (!meta || !rgx_path) return -1;

    FILE *f = fopen(rgx_path, "w");
    if (!f) return -1;

    // Write Regroove section
    fprintf(f, "[Regroove]\n");
    fprintf(f, "version=%d\n", meta->version);
    if (meta->module_file[0] != '\0') {
        fprintf(f, "file=\"%s\"\n", meta->module_file);
    }
    fprintf(f, "\n");

    // Write Patterns section if we have any descriptions
    if (meta->pattern_meta_count > 0) {
        fprintf(f, "[Patterns]\n");
        for (int i = 0; i < meta->pattern_meta_count; i++) {
            const RegroovePatternMeta *pm = &meta->pattern_meta[i];
            if (pm->description[0] != '\0') {
                fprintf(f, "pattern_%d=\"%s\"\n", pm->pattern_index, pm->description);
            }
        }
        fprintf(f, "\n");
    }

    // Write Song Trigger Pads section (S1-S16) if any are configured
    int has_song_pads = 0;
    for (int i = 0; i < MAX_SONG_TRIGGER_PADS; i++) {
        if (meta->song_trigger_pads[i].action != ACTION_NONE ||
            meta->song_trigger_pads[i].midi_note != -1) {
            has_song_pads = 1;
            break;
        }
    }

    if (has_song_pads) {
        fprintf(f, "[SongTriggerPads]\n");
        for (int i = 0; i < MAX_SONG_TRIGGER_PADS; i++) {
            const TriggerPadConfig *pad = &meta->song_trigger_pads[i];
            if (pad->action != ACTION_NONE || pad->midi_note != -1) {
                fprintf(f, "pad_S%d_action=%s\n", i + 1, input_action_name(pad->action));
                fprintf(f, "pad_S%d_parameter=%d\n", i + 1, pad->parameter);
                if (pad->midi_note >= 0) {
                    fprintf(f, "pad_S%d_midi_note=%d\n", i + 1, pad->midi_note);
                    fprintf(f, "pad_S%d_midi_device=%d\n", i + 1, pad->midi_device);
                }
                // Save phrase index if assigned
                if (pad->phrase_index >= 0) {
                    fprintf(f, "pad_S%d_phrase=%d\n", i + 1, pad->phrase_index);
                }
            }
        }
        fprintf(f, "\n");
    }

    // Write Phrases section if any exist
    if (meta->phrase_count > 0) {
        fprintf(f, "[Phrases]\n");
        fprintf(f, "count=%d\n", meta->phrase_count);
        for (int i = 0; i < meta->phrase_count; i++) {
            const Phrase *phrase = &meta->phrases[i];
            fprintf(f, "\nphrase_%d_name=\"%s\"\n", i, phrase->name);
            fprintf(f, "phrase_%d_steps=%d\n", i, phrase->step_count);
            for (int j = 0; j < phrase->step_count; j++) {
                const PhraseStep *step = &phrase->steps[j];
                fprintf(f, "phrase_%d_step_%d_action=%s\n", i, j, input_action_name(step->action));
                fprintf(f, "phrase_%d_step_%d_parameter=%d\n", i, j, step->parameter);
                fprintf(f, "phrase_%d_step_%d_value=%d\n", i, j, step->value);
                fprintf(f, "phrase_%d_step_%d_position=%d\n", i, j, step->position_rows);
            }
        }
        fprintf(f, "\n");
    }

    // Write MIDI Mapping section if any custom mappings exist
    int has_midi_mapping = 0;
    int has_name_overrides = 0;
    for (int i = 0; i < RGX_MAX_INSTRUMENTS; i++) {
        if (meta->instrument_midi_channels[i] != -2) {
            has_midi_mapping = 1;
        }
        if (meta->instrument_names[i][0] != '\0') {
            has_name_overrides = 1;
        }
        if (has_midi_mapping && has_name_overrides) break;
    }

    if (has_midi_mapping || has_name_overrides) {
        fprintf(f, "[MIDIMapping]\n");

        // Write MIDI channel mappings (skip -2 = disabled/default)
        if (has_midi_mapping) {
            for (int i = 0; i < RGX_MAX_INSTRUMENTS; i++) {
                if (meta->instrument_midi_channels[i] != -2) {
                    fprintf(f, "instrument_%d_channel=%d\n", i, meta->instrument_midi_channels[i]);
                }
            }
        }

        // Write instrument name overrides
        if (has_name_overrides) {
            for (int i = 0; i < RGX_MAX_INSTRUMENTS; i++) {
                if (meta->instrument_names[i][0] != '\0') {
                    fprintf(f, "instrument_%d_name=\"%s\"\n", i, meta->instrument_names[i]);
                }
            }
        }

        fprintf(f, "\n");
    }

    // Note: [performance] section is appended by regroove_performance_save()
    // if there are performance events to save

    fclose(f);
    return 0;
}

void regroove_metadata_set_pattern_desc(RegrooveMetadata *meta, int pattern_index, const char *description) {
    if (!meta || pattern_index < 0) return;

    // Check if we already have metadata for this pattern
    for (int i = 0; i < meta->pattern_meta_count; i++) {
        if (meta->pattern_meta[i].pattern_index == pattern_index) {
            // Update existing
            strncpy(meta->pattern_meta[i].description, description ? description : "", RGX_MAX_PATTERN_DESC - 1);
            meta->pattern_meta[i].description[RGX_MAX_PATTERN_DESC - 1] = '\0';
            return;
        }
    }

    // Add new entry
    if (meta->pattern_meta_count >= meta->pattern_meta_capacity) {
        // Expand capacity
        int new_capacity = meta->pattern_meta_capacity * 2;
        RegroovePatternMeta *new_meta = (RegroovePatternMeta*)realloc(
            meta->pattern_meta, new_capacity * sizeof(RegroovePatternMeta));
        if (!new_meta) return;
        meta->pattern_meta = new_meta;
        meta->pattern_meta_capacity = new_capacity;
    }

    RegroovePatternMeta *pm = &meta->pattern_meta[meta->pattern_meta_count];
    pm->pattern_index = pattern_index;
    strncpy(pm->description, description ? description : "", RGX_MAX_PATTERN_DESC - 1);
    pm->description[RGX_MAX_PATTERN_DESC - 1] = '\0';
    meta->pattern_meta_count++;
}

const char* regroove_metadata_get_pattern_desc(const RegrooveMetadata *meta, int pattern_index) {
    if (!meta) return NULL;

    for (int i = 0; i < meta->pattern_meta_count; i++) {
        if (meta->pattern_meta[i].pattern_index == pattern_index) {
            return meta->pattern_meta[i].description;
        }
    }

    return NULL;
}

void regroove_metadata_get_rgx_path(const char *module_path, char *rgx_path, size_t rgx_path_size) {
    if (!module_path || !rgx_path || rgx_path_size == 0) return;

    // Copy module path
    strncpy(rgx_path, module_path, rgx_path_size - 1);
    rgx_path[rgx_path_size - 1] = '\0';

    // Find last dot
    char *last_dot = strrchr(rgx_path, '.');
    char *last_slash = strrchr(rgx_path, '/');
    char *last_backslash = strrchr(rgx_path, '\\');
    char *last_sep = last_slash > last_backslash ? last_slash : last_backslash;

    // Only replace extension if dot is after last path separator
    if (last_dot && (!last_sep || last_dot > last_sep)) {
        // Replace extension with .rgx
        size_t base_len = last_dot - rgx_path;
        if (base_len + 4 < rgx_path_size) {
            strcpy(rgx_path + base_len, ".rgx");
        }
    } else {
        // No extension, append .rgx
        size_t len = strlen(rgx_path);
        if (len + 4 < rgx_path_size) {
            strcpy(rgx_path + len, ".rgx");
        }
    }
}

int regroove_metadata_get_midi_channel(const RegrooveMetadata *meta, int instrument_index) {
    if (!meta || instrument_index < 0 || instrument_index >= RGX_MAX_INSTRUMENTS) {
        return -2;  // Default fallback: disabled
    }

    int channel = meta->instrument_midi_channels[instrument_index];
    if (channel == -2) {
        // Disabled: no MIDI output
        return -2;
    } else if (channel == -1) {
        // Auto: use wraparound mapping
        return instrument_index % 16;
    }

    // Use custom mapping (0-15)
    return channel;
}

void regroove_metadata_set_midi_channel(RegrooveMetadata *meta, int instrument_index, int midi_channel) {
    if (!meta || instrument_index < 0 || instrument_index >= RGX_MAX_INSTRUMENTS) {
        return;
    }

    // Validate MIDI channel (-2 = disabled, -1 = auto, 0-15 = specific channel)
    if (midi_channel < -2 || midi_channel > 15) {
        return;
    }

    meta->instrument_midi_channels[instrument_index] = midi_channel;

    // Set flag to indicate we have custom mappings
    if (midi_channel != -2) {
        meta->has_midi_mapping = 1;
    }
}

const char* regroove_metadata_get_instrument_name(const RegrooveMetadata *meta, int instrument_index) {
    if (!meta || instrument_index < 0 || instrument_index >= RGX_MAX_INSTRUMENTS) {
        return NULL;
    }

    // Return NULL if empty (use module's name)
    if (meta->instrument_names[instrument_index][0] == '\0') {
        return NULL;
    }

    return meta->instrument_names[instrument_index];
}

void regroove_metadata_set_instrument_name(RegrooveMetadata *meta, int instrument_index, const char *name) {
    if (!meta || instrument_index < 0 || instrument_index >= RGX_MAX_INSTRUMENTS) {
        return;
    }

    if (!name || name[0] == '\0') {
        // Clear custom name (use module's name)
        meta->instrument_names[instrument_index][0] = '\0';
    } else {
        // Set custom name
        snprintf(meta->instrument_names[instrument_index], RGX_MAX_INSTRUMENT_NAME, "%s", name);
    }
}
