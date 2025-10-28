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

// Track current program on each MIDI channel
static int current_program[16];  // Track program for each MIDI channel (0-15)

// MIDI Clock master state
static int clock_master_enabled = 0;
static double clock_pulse_accumulator = 0.0;  // Accumulate fractional pulses
static double last_bpm = 0.0;  // Track BPM for pulse timing

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

int midi_output_list_ports(void) {
    RtMidiOutPtr temp = rtmidi_out_create_default();
    if (!temp) return 0;
    unsigned int nports = rtmidi_get_port_count(temp);
    rtmidi_out_free(temp);
    return nports;
}

int midi_output_get_port_name(int port, char *name_out, int bufsize) {
    if (!name_out || bufsize <= 0) return -1;

    RtMidiOutPtr temp = rtmidi_out_create_default();
    if (!temp) return -1;

    unsigned int nports = rtmidi_get_port_count(temp);
    if (port < 0 || port >= (int)nports) {
        rtmidi_out_free(temp);
        return -1;
    }

    rtmidi_get_port_name(temp, port, name_out, &bufsize);
    rtmidi_out_free(temp);
    return 0;
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

    // Initialize program tracking (-1 = no program set yet)
    for (int i = 0; i < 16; i++) {
        current_program[i] = -1;
    }

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

    // Send program change if the program for this instrument differs from current channel program
    if (current_metadata && instrument_index >= 0 && instrument_index < 256) {
        int program = regroove_metadata_get_program(current_metadata, instrument_index);
        if (program >= 0 && program <= 127) {
            // Only send if this program is different from what's currently on this MIDI channel
            if (current_program[midi_channel] != program) {
                midi_output_program_change(midi_channel, program);
                current_program[midi_channel] = program;
            }
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

    // Reset program tracking
    for (int i = 0; i < 16; i++) {
        current_program[i] = -1;
    }

    // Send all notes off on all MIDI channels
    for (int ch = 0; ch < 16; ch++) {
        midi_output_all_notes_off(ch);
    }
}

void midi_output_set_metadata(RegrooveMetadata *metadata) {
    current_metadata = metadata;
}

void midi_output_reset_programs(void) {
    // Reset program tracking so program changes will be resent
    for (int i = 0; i < 16; i++) {
        current_program[i] = -1;
    }
}

void midi_output_set_clock_master(int enabled) {
    clock_master_enabled = enabled;
}

int midi_output_is_clock_master(void) {
    return clock_master_enabled;
}

static int clock_debug_counter = 0;

void midi_output_send_clock(void) {
    if (!midi_out || !clock_master_enabled) return;

    // Send MIDI Clock message (0xF8)
    unsigned char msg[1];
    msg[0] = 0xF8;

    rtmidi_out_send_message(midi_out, msg, 1);

    // Debug: Print every 24 pulses (once per beat)
    clock_debug_counter++;
    if (clock_debug_counter % 24 == 0) {
        printf("[MIDI Output] Sent clock pulse #%d (0xF8)\n", clock_debug_counter);
    }
}

void midi_output_send_start(void) {
    if (!midi_out || !clock_master_enabled) return;

    // Reset clock accumulator when starting playback
    clock_pulse_accumulator = 0.0;

    // Send MIDI Start message (0xFA)
    unsigned char msg[1];
    msg[0] = 0xFA;

    printf("[MIDI Output] Sending Start (0xFA)\n");
    rtmidi_out_send_message(midi_out, msg, 1);
}

void midi_output_send_stop(void) {
    if (!midi_out || !clock_master_enabled) return;

    // Send MIDI Stop message (0xFC)
    unsigned char msg[1];
    msg[0] = 0xFC;

    printf("[MIDI Output] Sending Stop (0xFC)\n");
    rtmidi_out_send_message(midi_out, msg, 1);
}

void midi_output_send_continue(void) {
    if (!midi_out || !clock_master_enabled) return;

    // Send MIDI Continue message (0xFB)
    unsigned char msg[1];
    msg[0] = 0xFB;

    rtmidi_out_send_message(midi_out, msg, 1);
}

void midi_output_update_clock(double bpm, double row_fraction) {
    if (!midi_out || !clock_master_enabled || bpm <= 0.0) return;

    // Store BPM for later use
    last_bpm = bpm;
}

// Call this from audio callback to send clock pulses at precise intervals
// frames: number of audio frames rendered
// sample_rate: audio sample rate (e.g., 48000)
void midi_output_send_clock_pulses(int frames, double sample_rate, double bpm) {
    if (!midi_out || !clock_master_enabled || bpm <= 0.0 || sample_rate <= 0.0) return;

    // Calculate how many clock pulses should occur in this audio buffer
    // MIDI Clock = 24 pulses per quarter note (PPQN)
    // pulses_per_second = (BPM / 60) * 24
    double pulses_per_second = (bpm / 60.0) * 24.0;

    // How many pulses should occur in this number of frames?
    double pulses_this_buffer = (frames / sample_rate) * pulses_per_second;

    // Accumulate fractional pulses
    clock_pulse_accumulator += pulses_this_buffer;

    // Send whole pulses
    while (clock_pulse_accumulator >= 1.0) {
        midi_output_send_clock();
        clock_pulse_accumulator -= 1.0;
    }
}
