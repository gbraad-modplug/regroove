#ifndef REGROOVE_PERFORMANCE_H
#define REGROOVE_PERFORMANCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "input_mappings.h"

// Maximum number of events in a performance
#define PERF_MAX_EVENTS 10000

// Performance event structure
typedef struct {
    int performance_row;     // Absolute performance row when this event occurs
    InputAction action;      // What action to perform
    int parameter;           // Action parameter (e.g., channel number, order number)
    float value;             // Action value (e.g., volume level, pitch amount)
} PerformanceEvent;

// Performance state
typedef struct RegroovePerformance RegroovePerformance;

// Create a new performance
RegroovePerformance* regroove_performance_create(void);

// Destroy a performance
void regroove_performance_destroy(RegroovePerformance* perf);

// Reset performance to beginning
void regroove_performance_reset(RegroovePerformance* perf);

// Start/stop recording
void regroove_performance_set_recording(RegroovePerformance* perf, int recording);
int regroove_performance_is_recording(const RegroovePerformance* perf);

// Start/stop playback
void regroove_performance_set_playback(RegroovePerformance* perf, int playing);
int regroove_performance_is_playing(const RegroovePerformance* perf);

// Update performance timeline (call from row callback)
// Returns 1 if performance_row was incremented, 0 otherwise
int regroove_performance_tick(RegroovePerformance* perf);

// Record an event at current performance position
// Returns 0 on success, -1 if recording is disabled or buffer full
int regroove_performance_record_event(RegroovePerformance* perf,
                                      InputAction action,
                                      int parameter,
                                      float value);

// Get events that should be triggered at current performance position
// Returns number of events copied to events_out (max events_out_capacity)
int regroove_performance_get_events(const RegroovePerformance* perf,
                                    PerformanceEvent* events_out,
                                    int events_out_capacity);

// Get current performance position
int regroove_performance_get_row(const RegroovePerformance* perf);

// Get performance order/row for display (based on 64 rows per order)
void regroove_performance_get_position(const RegroovePerformance* perf,
                                       int* order_out,
                                       int* row_out);

// Get total number of recorded events
int regroove_performance_get_event_count(const RegroovePerformance* perf);

// Clear all recorded events
void regroove_performance_clear_events(RegroovePerformance* perf);

// Get direct access to event at index (for editing)
// Returns NULL if index is out of bounds
PerformanceEvent* regroove_performance_get_event_at(RegroovePerformance* perf, int index);

// Delete event at index
// Returns 0 on success, -1 on error
int regroove_performance_delete_event(RegroovePerformance* perf, int index);

// Add a new event manually (for UI editing)
// Returns 0 on success, -1 if buffer full
int regroove_performance_add_event(RegroovePerformance* perf,
                                   int performance_row,
                                   InputAction action,
                                   int parameter,
                                   float value);

// Save performance to file
int regroove_performance_save(const RegroovePerformance* perf, const char* filepath);

// Load performance from file
int regroove_performance_load(RegroovePerformance* perf, const char* filepath);

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_PERFORMANCE_H
