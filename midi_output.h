#ifndef MIDI_OUTPUT_H
#define MIDI_OUTPUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of MIDI output devices
#define MIDI_OUT_MAX_DEVICES 1  // Single device for now

// Initialize MIDI output device
// Returns 0 on success, -1 on failure
int midi_output_init(int device_id);

// Cleanup MIDI output
void midi_output_deinit(void);

// Send note-on message
// channel: 0-15 (MIDI channels)
// note: 0-127 (MIDI note number)
// velocity: 0-127 (MIDI velocity)
void midi_output_note_on(int channel, int note, int velocity);

// Send note-off message
// channel: 0-15 (MIDI channels)
// note: 0-127 (MIDI note number)
void midi_output_note_off(int channel, int note);

// Send all notes off on a channel
void midi_output_all_notes_off(int channel);

// Track active notes per channel (internal state management)
// This is called by the engine callback to manage note-on/note-off
// Returns 0 on success, -1 on failure
int midi_output_handle_note(int tracker_channel, int note, int instrument, int volume);

// Stop note on a tracker channel (called when effect command detected)
void midi_output_stop_channel(int tracker_channel);

// Reset all MIDI output state (stop all notes)
void midi_output_reset(void);

#ifdef __cplusplus
}
#endif

#endif // MIDI_OUTPUT_H
