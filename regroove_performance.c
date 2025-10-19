#include "regroove_performance.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

struct RegroovePerformance {
    int performance_row;          // Absolute row counter (never resets except on reset())
    int recording;                // 1 if recording, 0 otherwise
    int playing;                  // 1 if playing back, 0 otherwise

    PerformanceEvent* events;     // Array of recorded events
    int event_count;              // Number of events recorded
    int event_capacity;           // Capacity of events array

    int playback_index;           // Current index in events array during playback

    // Action execution callback (set by GUI/TUI)
    PerformanceActionCallback action_callback;
    void* action_callback_userdata;
};

RegroovePerformance* regroove_performance_create(void) {
    RegroovePerformance* perf = (RegroovePerformance*)calloc(1, sizeof(RegroovePerformance));
    if (!perf) return NULL;

    perf->events = (PerformanceEvent*)malloc(PERF_MAX_EVENTS * sizeof(PerformanceEvent));
    if (!perf->events) {
        free(perf);
        return NULL;
    }

    perf->event_capacity = PERF_MAX_EVENTS;
    perf->event_count = 0;
    perf->performance_row = 0;
    perf->recording = 0;
    perf->playing = 0;
    perf->playback_index = 0;

    return perf;
}

void regroove_performance_destroy(RegroovePerformance* perf) {
    if (!perf) return;
    if (perf->events) free(perf->events);
    free(perf);
}

void regroove_performance_reset(RegroovePerformance* perf) {
    if (!perf) return;
    perf->performance_row = 0;
    perf->playback_index = 0;
}

void regroove_performance_set_recording(RegroovePerformance* perf, int recording) {
    if (!perf) return;
    perf->recording = recording ? 1 : 0;

    // When starting recording, clear existing events and reset position
    if (perf->recording) {
        perf->event_count = 0;
        perf->performance_row = 0;
        perf->playback_index = 0;
    }
}

int regroove_performance_is_recording(const RegroovePerformance* perf) {
    return perf ? perf->recording : 0;
}

void regroove_performance_set_playback(RegroovePerformance* perf, int playing) {
    if (!perf) return;
    perf->playing = playing ? 1 : 0;

    // When starting playback, reset to beginning
    if (perf->playing) {
        perf->performance_row = 0;
        perf->playback_index = 0;
    }
}

int regroove_performance_is_playing(const RegroovePerformance* perf) {
    return perf ? perf->playing : 0;
}

int regroove_performance_tick(RegroovePerformance* perf) {
    if (!perf) return 0;

    // Only increment if playing (not just recording)
    // This allows recording to capture events at row 0 before playback starts
    if (perf->playing) {
        perf->performance_row++;
        return 1;
    }

    //printf("PR %d\n", perf->performance_row);

    return 0;
}

int regroove_performance_record_event(RegroovePerformance* perf,
                                      InputAction action,
                                      int parameter,
                                      float value) {
    if (!perf || !perf->recording) return -1;
    if (perf->event_count >= perf->event_capacity) return -1;

    PerformanceEvent* evt = &perf->events[perf->event_count];
    evt->performance_row = perf->performance_row;
    evt->action = action;
    evt->parameter = parameter;
    evt->value = value;

    perf->event_count++;

    printf("Recorded event: %s (param=%d, value=%.0f) at PR:%d\n",
           input_action_name(action), parameter, value, perf->performance_row);

    return 0;
}

int regroove_performance_get_events(const RegroovePerformance* perf,
                                    PerformanceEvent* events_out,
                                    int events_out_capacity) {
    if (!perf || !events_out || events_out_capacity <= 0) return 0;
    if (!perf->playing) return 0;

    // Cast away const to update playback_index (needed for tracking playback position)
    RegroovePerformance* perf_mut = (RegroovePerformance*)perf;

    int count = 0;

    // Skip events that are in the past
    while (perf_mut->playback_index < perf->event_count &&
           perf->events[perf_mut->playback_index].performance_row < perf->performance_row) {
        perf_mut->playback_index++;
    }

    // Find all events at current performance_row
    int i = perf_mut->playback_index;
    while (i < perf->event_count &&
           perf->events[i].performance_row == perf->performance_row) {
        if (count < events_out_capacity) {
            events_out[count] = perf->events[i];
            count++;
        }
        i++;
    }

    return count;
}

int regroove_performance_get_row(const RegroovePerformance* perf) {
    return perf ? perf->performance_row : 0;
}

void regroove_performance_get_position(const RegroovePerformance* perf,
                                       int* order_out,
                                       int* row_out) {
    if (!perf) {
        if (order_out) *order_out = 0;
        if (row_out) *row_out = 0;
        return;
    }

    // Convert absolute performance_row to order/row display
    // Using 64 rows per order (standard tracker convention)
    if (order_out) *order_out = perf->performance_row / 64;
    if (row_out) *row_out = perf->performance_row % 64;
}

int regroove_performance_get_event_count(const RegroovePerformance* perf) {
    return perf ? perf->event_count : 0;
}

void regroove_performance_clear_events(RegroovePerformance* perf) {
    if (!perf) return;
    perf->event_count = 0;
    perf->playback_index = 0;
}

PerformanceEvent* regroove_performance_get_event_at(RegroovePerformance* perf, int index) {
    if (!perf || index < 0 || index >= perf->event_count) return NULL;
    return &perf->events[index];
}

int regroove_performance_delete_event(RegroovePerformance* perf, int index) {
    if (!perf || index < 0 || index >= perf->event_count) return -1;

    // Shift all events after this one down
    for (int i = index; i < perf->event_count - 1; i++) {
        perf->events[i] = perf->events[i + 1];
    }
    perf->event_count--;

    // Reset playback index to be safe
    if (perf->playback_index > index) {
        perf->playback_index--;
    }

    return 0;
}

int regroove_performance_add_event(RegroovePerformance* perf,
                                   int performance_row,
                                   InputAction action,
                                   int parameter,
                                   float value) {
    if (!perf || perf->event_count >= perf->event_capacity) return -1;

    // Add event at the end
    PerformanceEvent* evt = &perf->events[perf->event_count];
    evt->performance_row = performance_row;
    evt->action = action;
    evt->parameter = parameter;
    evt->value = value;
    perf->event_count++;

    // Sort events by performance_row to maintain order
    // Simple insertion sort since we're adding one at a time
    for (int i = perf->event_count - 1; i > 0; i--) {
        if (perf->events[i].performance_row < perf->events[i-1].performance_row) {
            PerformanceEvent temp = perf->events[i];
            perf->events[i] = perf->events[i-1];
            perf->events[i-1] = temp;
        } else {
            break;
        }
    }

    return 0;
}

int regroove_performance_save(const RegroovePerformance* perf, const char* filepath) {
    if (!perf || !filepath) return -1;

    // Don't save if there are no events
    if (perf->event_count == 0) return 0;

    // Append to existing .rgx file
    FILE* f = fopen(filepath, "a");
    if (!f) return -1;

    // Write Events section header
    fprintf(f, "[Events]\n");

    // Group events by PO:PR position and write in human-readable format
    // Format: EVT_PO_PR=ACTION_NAME or EVT_PO_PR=ACTION_NAME_PARAM,ACTION_NAME_PARAM,...
    int i = 0;
    while (i < perf->event_count) {
        const PerformanceEvent* evt = &perf->events[i];
        int po = evt->performance_row / 64;  // Performance Order
        int pr = evt->performance_row % 64;  // Performance Row
        int current_row = evt->performance_row;

        // Start the line for this PO:PR position
        fprintf(f, "EVT_%02d_%02d=", po, pr);

        // Write all events at this position (comma-separated)
        int first = 1;
        while (i < perf->event_count && perf->events[i].performance_row == current_row) {
            if (!first) fprintf(f, ", ");
            first = 0;

            const PerformanceEvent* e = &perf->events[i];
            const char* action_name = input_action_name(e->action);

            // Write action name
            fprintf(f, "%s", action_name);

            // Add parameters as key:value pairs if needed
            if (e->action == ACTION_CHANNEL_MUTE ||
                e->action == ACTION_CHANNEL_SOLO ||
                e->action == ACTION_CHANNEL_VOLUME) {
                fprintf(f, " ch:%d", e->parameter);
            } else if (e->action == ACTION_JUMP_TO_ORDER ||
                       e->action == ACTION_QUEUE_ORDER) {
                fprintf(f, " order:%d", e->parameter);
            } else if (e->action == ACTION_JUMP_TO_PATTERN ||
                       e->action == ACTION_QUEUE_PATTERN) {
                fprintf(f, " pattern:%d", e->parameter);
            } else if (e->action == ACTION_TRIGGER_PAD) {
                fprintf(f, " pad:%d", e->parameter);
            }

            // Add value if it's meaningful (for volume, etc.)
            if (e->action == ACTION_CHANNEL_VOLUME && e->value > 0) {
                fprintf(f, " value:%d", (int)e->value);
            }

            i++;
        }
        fprintf(f, "\n");
    }

    fclose(f);
    return 0;
}

// --- Unified Action Handler Implementation ---

void regroove_performance_set_action_callback(RegroovePerformance* perf,
                                               PerformanceActionCallback callback,
                                               void* userdata) {
    if (!perf) return;
    perf->action_callback = callback;
    perf->action_callback_userdata = userdata;
}

int regroove_performance_handle_action(RegroovePerformance* perf,
                                        InputAction action,
                                        int parameter,
                                        float value,
                                        int from_playback) {
    if (!perf) return -1;

    // Step 1: Record the action if recording is active (but NOT if it came from playback)
    if (!from_playback && perf->recording) {
        if (regroove_performance_record_event(perf, action, parameter, value) != 0) {
            fprintf(stderr, "Warning: Failed to record event (buffer full?)\n");
        }
    }

    // Step 2: Execute the action via callback (always execute, unless we want playback-only mode)
    // For now, we always execute actions - even during playback (so user can still control things)
    if (perf->action_callback) {
        perf->action_callback(action, parameter, value, perf->action_callback_userdata);
    }

    return 0;
}

// Forward declare parse_action from input_mappings.c
extern InputAction parse_action(const char *str);

int regroove_performance_load(RegroovePerformance* perf, const char* filepath) {
    if (!perf || !filepath) return -1;

    FILE* f = fopen(filepath, "r");
    if (!f) return -1;

    // Clear existing events
    perf->event_count = 0;

    char line[512];
    int in_events_section = 0;

    while (fgets(line, sizeof(line), f)) {
        // Trim trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 0 && line[len-1] == '\r') line[len-2] = '\0';

        // Trim whitespace
        char *trimmed = line;
        while (*trimmed && isspace(*trimmed)) trimmed++;

        // Skip empty lines and comments
        if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';') continue;

        // Check for section headers
        if (trimmed[0] == '[') {
            in_events_section = (strcmp(trimmed, "[Events]") == 0);
            continue;
        }

        if (!in_events_section) continue;

        // Parse EVT_PO_PR=ACTION_NAME,ACTION_NAME,...
        if (strncmp(trimmed, "EVT_", 4) == 0) {
            char *eq = strchr(trimmed, '=');
            if (!eq) continue;

            *eq = '\0';
            char *key = trimmed;
            char *value = eq + 1;

            // Parse PO and PR from key (EVT_PO_PR)
            int po, pr;
            if (sscanf(key, "EVT_%d_%d", &po, &pr) != 2) continue;

            int performance_row = po * 64 + pr;

            // Parse comma-separated actions
            char value_copy[512];
            strncpy(value_copy, value, sizeof(value_copy) - 1);
            value_copy[sizeof(value_copy) - 1] = '\0';

            char *action_str = strtok(value_copy, ",");
            while (action_str != NULL) {
                if (perf->event_count >= perf->event_capacity) {
                    fprintf(stderr, "Warning: Performance event capacity reached\n");
                    break;
                }

                // Trim action string
                while (*action_str && isspace(*action_str)) action_str++;
                char *end = action_str + strlen(action_str) - 1;
                while (end > action_str && isspace(*end)) *end-- = '\0';

                // Split action name from key:value pairs
                char action_name[128] = "";
                int parameter = 0;
                float value = 127.0f;

                // Find first space (separates action from parameters)
                char *space = strchr(action_str, ' ');
                if (space) {
                    *space = '\0';
                    strncpy(action_name, action_str, sizeof(action_name) - 1);

                    // Parse key:value pairs after the action name
                    char *params = space + 1;
                    char *token = strtok(params, " ");
                    while (token) {
                        char *colon = strchr(token, ':');
                        if (colon) {
                            *colon = '\0';
                            char *key_name = token;
                            char *val = colon + 1;

                            if (strcmp(key_name, "ch") == 0 ||
                                strcmp(key_name, "order") == 0 ||
                                strcmp(key_name, "pattern") == 0 ||
                                strcmp(key_name, "pad") == 0) {
                                parameter = atoi(val);
                            } else if (strcmp(key_name, "value") == 0) {
                                value = atof(val);
                            }
                            // Future: handle custom key:value pairs here
                        }
                        token = strtok(NULL, " ");
                    }
                } else {
                    // No parameters, just action name
                    strncpy(action_name, action_str, sizeof(action_name) - 1);
                }
                action_name[sizeof(action_name) - 1] = '\0';

                // Parse action name
                InputAction action = parse_action(action_name);
                if (action != ACTION_NONE) {
                    PerformanceEvent* evt = &perf->events[perf->event_count];
                    evt->performance_row = performance_row;
                    evt->action = action;
                    evt->parameter = parameter;
                    evt->value = value;
                    perf->event_count++;
                }

                action_str = strtok(NULL, ",");
            }
        }
    }

    fclose(f);

    // Reset playback position
    perf->playback_index = 0;

    return 0;
}
