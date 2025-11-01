#include "midi_output.h"
#include "regroove_metadata.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <rtmidi/rtmidi_c.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

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

// Dedicated MIDI clock thread for reliable timing
static SDL_Thread *clock_thread = NULL;
static SDL_atomic_t clock_thread_running;
static SDL_atomic_t target_bpm_atomic;  // Lock-free BPM updates from audio thread (stored as int32, divide by 1000)
static SDL_atomic_t clock_running;       // Is clock actively running (playing)?
static SDL_atomic_t spp_position_atomic; // Current SPP position (MIDI beats) from audio callback

#define BPM_SCALE 1000  // Scale factor for atomic BPM storage (allows 3 decimal places)

// SPP sending configuration (from device config)
static int spp_send_mode = 0;      // 0=disabled, 1=on stop only, 2=during playback
static int spp_send_interval = 64; // Rows between SPP messages (when mode=2)

// Forward declaration
static int midi_clock_thread_func(void *data);

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

    // Initialize atomic variables
    SDL_AtomicSet(&clock_thread_running, 0);
    SDL_AtomicSet(&target_bpm_atomic, 120 * BPM_SCALE);  // Default 120 BPM
    SDL_AtomicSet(&clock_running, 0);
    SDL_AtomicSet(&spp_position_atomic, 0);

    // Start the clock thread
    SDL_AtomicSet(&clock_thread_running, 1);
    clock_thread = SDL_CreateThread(midi_clock_thread_func, "MIDI Clock", NULL);
    if (!clock_thread) {
        fprintf(stderr, "Failed to create MIDI clock thread\n");
    } else {
        printf("[MIDI Output] Clock thread created\n");
    }

    printf("MIDI output initialized on device %d: %s\n", device_id, port_name);
    return 0;
}

void midi_output_deinit(void) {
    // Stop the clock thread first
    if (clock_thread) {
        SDL_AtomicSet(&clock_thread_running, 0);
        SDL_WaitThread(clock_thread, NULL);
        clock_thread = NULL;
        printf("[MIDI Output] Clock thread stopped\n");
    }

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

static int clock_pulse_debug_count = 0;
static Uint64 clock_pulse_debug_start = 0;
static Uint64 clock_pulse_last = 0;

void midi_output_send_clock(void) {
    if (!midi_out || !clock_master_enabled) return;

    Uint64 now = SDL_GetPerformanceCounter();

    // Debug: measure actual pulse rate every 24 pulses (1 beat)
    if (clock_pulse_debug_count == 0) {
        clock_pulse_debug_start = now;
    }
    clock_pulse_debug_count++;

    if (clock_pulse_debug_count >= 24) {
        double elapsed_sec = (double)(now - clock_pulse_debug_start) / SDL_GetPerformanceFrequency();
        double measured_bpm = 60.0 / elapsed_sec;  // 24 pulses = 1 beat

        // Also measure interval from last pulse
        double pulse_interval = 0;
        if (clock_pulse_last > 0) {
            pulse_interval = (double)(now - clock_pulse_last) / SDL_GetPerformanceFrequency() * 1000.0;  // ms
        }

        printf("[MIDI Clock] Measured: %.2f BPM (24 pulses in %.3f sec, last interval: %.3f ms)\n",
               measured_bpm, elapsed_sec, pulse_interval);
        clock_pulse_debug_count = 0;
    }

    clock_pulse_last = now;

    // Send MIDI Clock message (0xF8)
    unsigned char msg[1];
    msg[0] = 0xF8;

    rtmidi_out_send_message(midi_out, msg, 1);
}

void midi_output_send_start(void) {
    if (!midi_out) return;

    // Signal clock thread to start sending pulses
    SDL_AtomicSet(&clock_running, 1);

    // Reset clock accumulator when starting playback (if clock master is enabled)
    if (clock_master_enabled) {
        clock_pulse_accumulator = 0.0;
    }

    // Send MIDI Start message (0xFA)
    unsigned char msg[1];
    msg[0] = 0xFA;

    printf("[MIDI Output] Sending Start (0xFA)\n");
    rtmidi_out_send_message(midi_out, msg, 1);
}

void midi_output_send_stop(void) {
    if (!midi_out) return;

    // Signal clock thread to stop sending pulses
    SDL_AtomicSet(&clock_running, 0);

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

void midi_output_send_song_position(int position) {
    if (!midi_out) return;

    // Clamp position to valid range (0-16383)
    if (position < 0) position = 0;
    if (position > 16383) position = 16383;

    // MIDI Song Position Pointer: 0xF2 + LSB + MSB
    // Position is in MIDI beats (16th notes), split into 7-bit bytes
    unsigned char msg[3];
    msg[0] = 0xF2;
    msg[1] = position & 0x7F;        // LSB (bits 0-6)
    msg[2] = (position >> 7) & 0x7F; // MSB (bits 7-13)

    printf("[MIDI Output] Sending Song Position: %d MIDI beats (0x%02X 0x%02X 0x%02X)\n",
           position, msg[0], msg[1], msg[2]);
    rtmidi_out_send_message(midi_out, msg, 3);
}

// MIDI Clock thread - runs with precise timing independent of audio callback
static int midi_clock_thread_func(void *data) {
    (void)data;  // Unused

    double current_bpm = 120.0;  // Start with default
    double last_target_bpm = 120.0;
    Uint64 next_pulse_tick = SDL_GetPerformanceCounter();  // Track ideal next pulse time
    const Uint64 perf_freq = SDL_GetPerformanceFrequency();
    Uint64 last_bpm_update = SDL_GetPerformanceCounter();

    // Smoothing buffer for incoming BPM (libopenmpt gives estimates that fluctuate)
    #define BPM_SMOOTH_SAMPLES 8
    double bpm_smooth_buffer[BPM_SMOOTH_SAMPLES] = {120.0};
    int bpm_smooth_index = 0;
    int bpm_smooth_filled = 0;

    // SPP tracking
    int last_sent_spp = -1;  // Last SPP position we sent

    printf("[MIDI Clock Thread] Started\n");

    while (SDL_AtomicGet(&clock_thread_running)) {
        // Check if clock should be running
        int is_playing = SDL_AtomicGet(&clock_running);

        if (!is_playing) {
            // Not playing, but still check for SPP updates (SPP works independently of clock)
            if (spp_send_mode > 0) {
                int current_spp = SDL_AtomicGet(&spp_position_atomic);
                if (current_spp != last_sent_spp && current_spp >= 0) {
                    midi_output_send_song_position(current_spp);
                    last_sent_spp = current_spp;
                }
            }
            SDL_Delay(10);
            next_pulse_tick = SDL_GetPerformanceCounter();
            last_bpm_update = SDL_GetPerformanceCounter();
            continue;
        }

        // Handle SPP independently of clock (even if clock is disabled)
        if (spp_send_mode > 0) {
            int current_spp = SDL_AtomicGet(&spp_position_atomic);
            if (current_spp != last_sent_spp && current_spp >= 0) {
                midi_output_send_song_position(current_spp);
                last_sent_spp = current_spp;
            }
        }

        // If clock is not enabled, skip clock pulse generation
        if (!clock_master_enabled) {
            SDL_Delay(10);
            next_pulse_tick = SDL_GetPerformanceCounter();
            last_bpm_update = SDL_GetPerformanceCounter();
            continue;
        }

        // Get target BPM from audio thread (lock-free)
        int bpm_scaled = SDL_AtomicGet(&target_bpm_atomic);
        double target_bpm = (double)bpm_scaled / BPM_SCALE;

        if (target_bpm > 0.0) {
            Uint64 now = SDL_GetPerformanceCounter();
            Uint64 time_since_update = now - last_bpm_update;
            double ms_since_update = (time_since_update * 1000.0) / perf_freq;

            // Only update BPM if it changed significantly or enough time has passed
            if (fabs(target_bpm - last_target_bpm) > 0.05 || ms_since_update > 100.0) {
                // Add to smoothing buffer
                bpm_smooth_buffer[bpm_smooth_index] = target_bpm;
                bpm_smooth_index = (bpm_smooth_index + 1) % BPM_SMOOTH_SAMPLES;
                if (!bpm_smooth_filled && bpm_smooth_index == 0) {
                    bpm_smooth_filled = 1;
                }

                // Calculate smoothed BPM (moving average)
                double sum = 0.0;
                int count = bpm_smooth_filled ? BPM_SMOOTH_SAMPLES : (bpm_smooth_index > 0 ? bpm_smooth_index : 1);
                for (int i = 0; i < count; i++) {
                    sum += bpm_smooth_buffer[i];
                }
                double smoothed_target = sum / count;

                if (fabs(smoothed_target - current_bpm) > 0.1) {
                    printf("[MIDI Clock] BPM update: %.3f -> %.3f (raw: %.3f, smoothed over %d samples)\n",
                           current_bpm, smoothed_target, target_bpm, count);
                }

                current_bpm = smoothed_target;
                last_target_bpm = target_bpm;
                last_bpm_update = now;
            }
        }

        // Calculate timing for next clock pulse
        // MIDI Clock = 24 PPQN (pulses per quarter note)
        if (current_bpm > 0.0) {
            double pulses_per_second = (current_bpm / 60.0) * 24.0;
            double seconds_per_pulse = 1.0 / pulses_per_second;
            Uint64 ticks_per_pulse = (Uint64)(seconds_per_pulse * perf_freq);

            // Check if it's time for next pulse
            Uint64 current_tick = SDL_GetPerformanceCounter();

            if (current_tick >= next_pulse_tick) {
                // Send clock pulse
                midi_output_send_clock();

                // Schedule next pulse at exact interval (prevents drift accumulation)
                next_pulse_tick += ticks_per_pulse;

                // If we're way behind (>10ms), resync to avoid burst catching up
                if (current_tick > next_pulse_tick + (perf_freq / 100)) {
                    next_pulse_tick = current_tick + ticks_per_pulse;
                }
            } else {
                // Sleep until near next pulse time
                Sint64 ticks_until_next = (Sint64)(next_pulse_tick - current_tick);
                if (ticks_until_next > 0) {
                    Uint32 sleep_ms = (Uint32)((ticks_until_next * 1000) / perf_freq);
                    if (sleep_ms > 1) {
                        SDL_Delay(sleep_ms - 1);  // Sleep most of the time, busy-wait the rest
                    }
                }
            }
        } else {
            SDL_Delay(10);
        }
    }

    printf("[MIDI Clock Thread] Stopped\n");
    return 0;
}

void midi_output_update_clock(double bpm, double row_fraction) {
    (void)row_fraction;  // Unused in new implementation

    if (bpm <= 0.0) return;

    // Update target BPM atomically (lock-free communication to clock thread)
    int bpm_scaled = (int)(bpm * BPM_SCALE);
    SDL_AtomicSet(&target_bpm_atomic, bpm_scaled);

    last_bpm = bpm;
}

void midi_output_set_spp_config(int mode, int interval) {
    spp_send_mode = mode;
    spp_send_interval = interval;
}

void midi_output_update_position(int spp_position) {
    // Update current SPP position atomically for clock thread to send
    SDL_AtomicSet(&spp_position_atomic, spp_position);
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
