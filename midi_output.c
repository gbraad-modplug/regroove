#include "midi_output.h"
#include "regroove_metadata.h"
#include <stdio.h>
#include <string.h>
#include <rtmidi/rtmidi_c.h>

// MIDI output state
static RtMidiOutPtr midi_out = NULL;
static int midi_out_device_id = -1;
static RegrooveMetadata *current_metadata = NULL;  // For channel mapping

// Maximum tracker channels (matches regroove engine)
#define MAX_TRACKER_CHANNELS 64

// Track active notes per tracker channel
typedef struct {
    int active;           // Is a note currently playing?
    int midi_channel;     // MIDI channel (0-15)
    int midi_note;        // MIDI note number (0-127)
} ActiveNote;

static ActiveNote active_notes[MAX_TRACKER_CHANNELS];

// Track which instruments have had their program change sent
static int program_sent[256];  // Track up to 256 instruments

// Get MIDI channel for instrument (using metadata if available)
static int get_midi_channel_for_instrument(int instrument) {
    if (current_metadata) {
        // Use custom mapping from metadata
        return regroove_metadata_get_midi_channel(current_metadata, instrument);
    }

    // Default fallback: simple wraparound
    return instrument % 16;
}

// Convert tracker note to MIDI note number
// Tracker note format: note value calculated from formatted string (e.g., "D-1")
static int tracker_note_to_midi(int note) {
    // The note value comes from parsing the formatted string in regroove_engine.c:
    // note = octave * 12 + base_note
    // This already gives us the correct MIDI note number directly!
    // For example: D-1 = 1*12 + 2 = 14 (which is MIDI note 14 = D1)
    // No offset needed - the parsed value IS the MIDI note number
    int midi_note = note;

    // Clamp to valid MIDI range
    if (midi_note < 0) midi_note = 0;
    if (midi_note > 127) midi_note = 127;

    return midi_note;
}

int midi_output_init(int device_id) {
    if (midi_out != NULL) {
        midi_output_deinit();
    }

    // Create RtMidi output
    midi_out = rtmidi_out_create_default();
    if (!midi_out) {
        fprintf(stderr, "Failed to create RtMidi output\n");
        return -1;
    }

    // Get device count
    unsigned int num_devices = rtmidi_get_port_count(midi_out);
    if (device_id < 0 || device_id >= (int)num_devices) {
        fprintf(stderr, "Invalid MIDI output device ID: %d (available: %u)\n", device_id, num_devices);
        rtmidi_out_free(midi_out);
        midi_out = NULL;
        return -1;
    }

    // Open the device
    char port_name[256];
    int bufsize = sizeof(port_name);
    int name_len = rtmidi_get_port_name(midi_out, device_id, port_name, &bufsize);
    if (name_len < 0) {
        snprintf(port_name, sizeof(port_name), "Port %d", device_id);
    }

    rtmidi_open_port(midi_out, device_id, "Regroove MIDI Out");

    midi_out_device_id = device_id;

    // Initialize active notes tracking
    memset(active_notes, 0, sizeof(active_notes));

    // Initialize program change tracking
    memset(program_sent, 0, sizeof(program_sent));

    printf("MIDI output initialized on device %d: %s\n", device_id, port_name);
    return 0;
}

void midi_output_deinit(void) {
    if (midi_out) {
        // Send all notes off on all channels before closing
        for (int ch = 0; ch < 16; ch++) {
            midi_output_all_notes_off(ch);
        }

        rtmidi_close_port(midi_out);
        rtmidi_out_free(midi_out);
        midi_out = NULL;
        midi_out_device_id = -1;
    }
}

void midi_output_note_on(int channel, int note, int velocity) {
    if (!midi_out) return;
    if (channel < 0 || channel > 15) return;
    if (note < 0 || note > 127) return;
    if (velocity < 0) velocity = 0;
    if (velocity > 127) velocity = 127;

    // Send MIDI note-on message (0x90 + channel)
    unsigned char msg[3];
    msg[0] = 0x90 | channel;
    msg[1] = note;
    msg[2] = velocity;

    rtmidi_out_send_message(midi_out, msg, 3);
}

void midi_output_note_off(int channel, int note) {
    if (!midi_out) return;
    if (channel < 0 || channel > 15) return;
    if (note < 0 || note > 127) return;

    // Send MIDI note-off message (0x80 + channel)
    unsigned char msg[3];
    msg[0] = 0x80 | channel;
    msg[1] = note;
    msg[2] = 0;

    rtmidi_out_send_message(midi_out, msg, 3);
}

void midi_output_all_notes_off(int channel) {
    if (!midi_out) return;
    if (channel < 0 || channel > 15) return;

    // Send All Notes Off controller (CC 123, value 0)
    unsigned char msg[3];
    msg[0] = 0xB0 | channel;
    msg[1] = 123;
    msg[2] = 0;

    rtmidi_out_send_message(midi_out, msg, 3);
}

void midi_output_program_change(int channel, int program) {
    if (!midi_out) return;
    if (channel < 0 || channel > 15) return;
    if (program < 0 || program > 127) return;

    // Send MIDI program change message (0xC0 + channel)
    unsigned char msg[2];
    msg[0] = 0xC0 | channel;
    msg[1] = program;

    rtmidi_out_send_message(midi_out, msg, 2);
}

int midi_output_handle_note(int tracker_channel, int note, int instrument, int volume) {
    if (!midi_out) return -1;
    if (tracker_channel < 0 || tracker_channel >= MAX_TRACKER_CHANNELS) return -1;

    // Convert 1-based instrument number to 0-based index for metadata lookup
    // Tracker instruments are numbered 01, 02, 03... but arrays are 0-indexed
    int instrument_index = (instrument > 0) ? (instrument - 1) : instrument;

    // Get MIDI channel for this instrument
    int midi_channel = get_midi_channel_for_instrument(instrument_index);

    // Skip if MIDI output is disabled for this instrument (-2)
    if (midi_channel == -2) {
        return 0;  // No MIDI output for this instrument
    }

    // Send program change if not already sent for this instrument
    if (current_metadata && instrument_index >= 0 && instrument_index < 256) {
        int program = regroove_metadata_get_program(current_metadata, instrument_index);
        if (program >= 0 && program <= 127 && !program_sent[instrument_index]) {
            midi_output_program_change(midi_channel, program);
            program_sent[instrument_index] = 1;
        }
    }

    // Convert tracker note to MIDI note
    int midi_note = tracker_note_to_midi(note);

    // Apply global note offset
    if (current_metadata) {
        int offset = regroove_metadata_get_note_offset(current_metadata);
        midi_note += offset;
    }

    // Clamp to valid MIDI range after offset
    if (midi_note < 0) midi_note = 0;
    if (midi_note > 127) midi_note = 127;

    // Convert volume (0-64 tracker range) to MIDI velocity (0-127)
    int velocity = (volume * 127) / 64;
    if (velocity > 127) velocity = 127;

    // If there's an active note on this tracker channel, stop it first
    if (active_notes[tracker_channel].active) {
        midi_output_note_off(active_notes[tracker_channel].midi_channel,
                            active_notes[tracker_channel].midi_note);
        active_notes[tracker_channel].active = 0;
    }

    // Send new note-on
    if (velocity > 0) {
        midi_output_note_on(midi_channel, midi_note, velocity);

        // Track this note
        active_notes[tracker_channel].active = 1;
        active_notes[tracker_channel].midi_channel = midi_channel;
        active_notes[tracker_channel].midi_note = midi_note;
    }

    return 0;
}

void midi_output_stop_channel(int tracker_channel) {
    if (!midi_out) return;
    if (tracker_channel < 0 || tracker_channel >= MAX_TRACKER_CHANNELS) return;

    // Stop active note on this tracker channel
    if (active_notes[tracker_channel].active) {
        midi_output_note_off(active_notes[tracker_channel].midi_channel,
                            active_notes[tracker_channel].midi_note);
        active_notes[tracker_channel].active = 0;
    }
}

void midi_output_reset(void) {
    if (!midi_out) return;

    // Stop all active notes
    for (int i = 0; i < MAX_TRACKER_CHANNELS; i++) {
        if (active_notes[i].active) {
            midi_output_note_off(active_notes[i].midi_channel,
                                active_notes[i].midi_note);
        }
    }

    // Clear tracking state
    memset(active_notes, 0, sizeof(active_notes));

    // Reset program change tracking (so program changes are sent again)
    memset(program_sent, 0, sizeof(program_sent));

    // Send all notes off on all MIDI channels
    for (int ch = 0; ch < 16; ch++) {
        midi_output_all_notes_off(ch);
    }
}

void midi_output_set_metadata(RegrooveMetadata *metadata) {
    current_metadata = metadata;
}
