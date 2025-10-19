#include "regroove_performance.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct RegroovePerformance {
    int performance_row;          // Absolute row counter (never resets except on reset())
    int recording;                // 1 if recording, 0 otherwise
    int playing;                  // 1 if playing back, 0 otherwise

    PerformanceEvent* events;     // Array of recorded events
    int event_count;              // Number of events recorded
    int event_capacity;           // Capacity of events array

    int playback_index;           // Current index in events array during playback
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

    // Only increment if recording or playing
    if (perf->recording || perf->playing) {
        perf->performance_row++;
        return 1;
    }

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
    return 0;
}

int regroove_performance_get_events(const RegroovePerformance* perf,
                                    PerformanceEvent* events_out,
                                    int events_out_capacity) {
    if (!perf || !events_out || events_out_capacity <= 0) return 0;
    if (!perf->playing) return 0;

    int count = 0;

    // Find all events at current performance_row
    for (int i = perf->playback_index; i < perf->event_count; i++) {
        if (perf->events[i].performance_row == perf->performance_row) {
            if (count < events_out_capacity) {
                events_out[count] = perf->events[i];
                count++;
            }
        } else if (perf->events[i].performance_row > perf->performance_row) {
            // Events are sorted by performance_row, so we can stop here
            break;
        }
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

int regroove_performance_save(const RegroovePerformance* perf, const char* filepath) {
    if (!perf || !filepath) return -1;

    FILE* f = fopen(filepath, "wb");
    if (!f) return -1;

    // Write header
    fprintf(f, "[performance]\n");
    fprintf(f, "event_count = %d\n", perf->event_count);
    fprintf(f, "\n");

    // Write events
    for (int i = 0; i < perf->event_count; i++) {
        const PerformanceEvent* evt = &perf->events[i];
        fprintf(f, "event = %d,%d,%d,%.3f\n",
                evt->performance_row,
                evt->action,
                evt->parameter,
                evt->value);
    }

    fclose(f);
    return 0;
}

int regroove_performance_load(RegroovePerformance* perf, const char* filepath) {
    if (!perf || !filepath) return -1;

    FILE* f = fopen(filepath, "r");
    if (!f) return -1;

    // Clear existing events
    perf->event_count = 0;

    char line[512];
    int in_performance_section = 0;

    while (fgets(line, sizeof(line), f)) {
        // Trim trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

        // Check for section header
        if (strcmp(line, "[performance]") == 0) {
            in_performance_section = 1;
            continue;
        }

        // Check for other section headers
        if (line[0] == '[') {
            in_performance_section = 0;
            continue;
        }

        if (!in_performance_section) continue;

        // Parse event line
        if (strncmp(line, "event = ", 8) == 0) {
            if (perf->event_count >= perf->event_capacity) {
                fprintf(stderr, "Warning: Performance event capacity reached\n");
                break;
            }

            PerformanceEvent* evt = &perf->events[perf->event_count];
            int action_int;

            if (sscanf(line + 8, "%d,%d,%d,%f",
                      &evt->performance_row,
                      &action_int,
                      &evt->parameter,
                      &evt->value) == 4) {
                evt->action = (InputAction)action_int;
                perf->event_count++;
            }
        }
    }

    fclose(f);

    // Reset playback position
    perf->playback_index = 0;

    return 0;
}
