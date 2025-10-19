#include "midi_output.h"
#include <stdio.h>
#include <string.h>
#include <rtmidi/rtmidi_c.h>

// MIDI output state
static RtMidiOutPtr midi_out = NULL;
static int midi_out_device_id = -1;

// Maximum tracker channels (matches regroove engine)
#define MAX_TRACKER_CHANNELS 64

// Track active notes per tracker channel
typedef struct {
    int active;           // Is a note currently playing?
    int midi_channel;     // MIDI channel (0-15)
    int midi_note;        // MIDI note number (0-127)
} ActiveNote;

static ActiveNote active_notes[MAX_TRACKER_CHANNELS];

// Default instrument-to-MIDI-channel mapping
// For now, simple 1:1 mapping with wraparound
static int get_midi_channel_for_instrument(int instrument) {
    // Simple wraparound: instruments 0-15 map to MIDI channels 0-15
    // instruments 16-31 also map to 0-15, etc.
    return instrument % 16;
}

// Convert tracker note to MIDI note number
// Tracker note format: C-4 = 48 (middle C)
static int tracker_note_to_midi(int note) {
    // libopenmpt uses: 0 = C-0, 12 = C-1, 48 = C-4 (middle C)
    // MIDI note 60 = C-4 (middle C)
    // So: MIDI note = tracker note + 12
    int midi_note = note + 12;

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

int midi_output_handle_note(int tracker_channel, int note, int instrument, int volume) {
    if (!midi_out) return -1;
    if (tracker_channel < 0 || tracker_channel >= MAX_TRACKER_CHANNELS) return -1;

    // Get MIDI channel for this instrument
    int midi_channel = get_midi_channel_for_instrument(instrument);

    // Convert tracker note to MIDI note
    int midi_note = tracker_note_to_midi(note);

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

    // Send all notes off on all MIDI channels
    for (int ch = 0; ch < 16; ch++) {
        midi_output_all_notes_off(ch);
    }
}
