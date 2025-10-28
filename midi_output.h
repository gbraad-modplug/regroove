#ifndef MIDI_OUTPUT_H
#define MIDI_OUTPUT_H

#include "regroove_metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of MIDI output devices
#define MIDI_OUT_MAX_DEVICES 1  // Single device for now

// List available MIDI output ports
// Returns the number of output ports found
int midi_output_list_ports(void);

// Get the name of a MIDI output port
// port: port index
// name_out: buffer to store the port name
// bufsize: size of name_out buffer
// Returns 0 on success, -1 on failure
int midi_output_get_port_name(int port, char *name_out, int bufsize);

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

// Send program change message
// channel: 0-15 (MIDI channels)
// program: 0-127 (MIDI program number)
void midi_output_program_change(int channel, int program);

// Track active notes per channel (internal state management)
// This is called by the engine callback to manage note-on/note-off
// Returns 0 on success, -1 on failure
int midi_output_handle_note(int tracker_channel, int note, int instrument, int volume);

// Stop note on a tracker channel (called when effect command detected)
void midi_output_stop_channel(int tracker_channel);

// Reset all MIDI output state (stop all notes)
void midi_output_reset(void);

// Set metadata for MIDI channel mapping (can be NULL to use default mapping)
void midi_output_set_metadata(RegrooveMetadata *metadata);

// Reset program change tracking (forces program changes to be resent)
// Call this on pattern/order boundaries to ensure correct programs are loaded
void midi_output_reset_programs(void);

// MIDI Clock master functions
// Enable/disable sending MIDI Clock messages
void midi_output_set_clock_master(int enabled);

// Check if clock master is enabled
int midi_output_is_clock_master(void);

// Send MIDI Clock pulse (0xF8) - call this 24 times per quarter note
void midi_output_send_clock(void);

// Send MIDI Start message (0xFA)
void midi_output_send_start(void);

// Send MIDI Stop message (0xFC)
void midi_output_send_stop(void);

// Send MIDI Continue message (0xFB)
void midi_output_send_continue(void);

// Send MIDI Song Position Pointer (0xF2)
// position: MIDI beats (16th notes) from start of song
void midi_output_send_song_position(int position);

// Update MIDI Clock based on playback position
// Call this regularly during playback (e.g., in row callback)
// bpm: Current tempo in beats per minute
// row_fraction: Fraction of row completed (0.0 to 1.0)
void midi_output_update_clock(double bpm, double row_fraction);

// Send MIDI Clock pulses based on audio frames rendered
// Call this from audio callback for precise timing
// frames: number of audio frames in this buffer
// sample_rate: audio sample rate (e.g., 48000)
// bpm: current tempo in beats per minute (adjusted for pitch)
void midi_output_send_clock_pulses(int frames, double sample_rate, double bpm);

#ifdef __cplusplus
}
#endif

#endif // MIDI_OUTPUT_H
