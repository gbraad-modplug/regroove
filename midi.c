#include "midi.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#else
#include <windows.h>
#endif
#include <stdio.h>
#include <rtmidi_c.h>

static RtMidiInPtr midiin[MIDI_MAX_DEVICES] = {NULL};
static MidiEventCallback midi_cb = NULL;
static void *cb_userdata = NULL;

// MIDI Clock synchronization state
static int clock_sync_enabled = 0;
static double clock_bpm = 0.0;
static double last_clock_time = 0.0;
static int clock_pulse_count = 0;

// Fixed-size circular buffer for interval averaging
#define INTERVAL_BUFFER_SIZE 24  // Average over one beat
static double interval_buffer[INTERVAL_BUFFER_SIZE];
static int interval_buffer_index = 0;
static int interval_buffer_filled = 0;

#define MIDI_CLOCK 0xF8
#define MIDI_START 0xFA
#define MIDI_CONTINUE 0xFB
#define MIDI_STOP 0xFC
#define PULSES_PER_QUARTER_NOTE 24

// MIDI Transport control state
static int transport_control_enabled = 0;
static MidiTransportCallback transport_cb = NULL;
static void *transport_userdata = NULL;

// Get current time in microseconds
static double get_time_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency = {0};
    LARGE_INTEGER counter;

    // Query frequency only once (it's constant for the system)
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }

    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000000.0 / (double)frequency.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
#endif
}

// Process MIDI Clock message (0xF8)
static void process_midi_clock(void) {
    double current_time = get_time_us();

    // Always process clock for visual indication, even if sync is disabled
    // If this is not the first pulse, calculate interval
    if (clock_pulse_count > 0) {
        double interval = current_time - last_clock_time;

        // Ignore unrealistic intervals (< 1ms or > 1 second)
        if (interval > 1000.0 && interval < 1000000.0) {
            // Add to circular buffer
            interval_buffer[interval_buffer_index] = interval;
            interval_buffer_index = (interval_buffer_index + 1) % INTERVAL_BUFFER_SIZE;

            // Mark buffer as filled once we've wrapped around
            if (interval_buffer_index == 0) {
                interval_buffer_filled = 1;
            }

            // Calculate average from buffer
            int count = interval_buffer_filled ? INTERVAL_BUFFER_SIZE : interval_buffer_index;
            double sum = 0.0;
            for (int i = 0; i < count; i++) {
                sum += interval_buffer[i];
            }
            double avg_interval = sum / count;

            // Calculate BPM from average pulse interval
            // BPM = 60,000,000 microseconds/minute / (avg_interval * 24 pulses/beat)
            clock_bpm = 60000000.0 / (avg_interval * PULSES_PER_QUARTER_NOTE);

        }
    } else {
        printf("[MIDI Clock] First pulse received\n");
    }

    // Update state
    last_clock_time = current_time;
    clock_pulse_count++;
}

// Device-specific callback wrappers
static void rtmidi_event_callback_0(double dt, const unsigned char *msg, size_t sz, void *userdata) {
    // Handle single-byte system real-time messages
    if (sz == 1) {
        if (msg[0] == MIDI_CLOCK) {
            process_midi_clock();
            return;
        }
        // Handle transport control messages
        if (msg[0] == MIDI_START || msg[0] == MIDI_STOP || msg[0] == MIDI_CONTINUE) {
            const char* msg_name = (msg[0] == MIDI_START) ? "Start" :
                                   (msg[0] == MIDI_STOP) ? "Stop" : "Continue";
            printf("[MIDI Transport] Received %s (0x%02X) on device 0, control %s\n",
                   msg_name, msg[0], transport_control_enabled ? "ENABLED" : "disabled");

            if (transport_control_enabled && transport_cb) {
                transport_cb(msg[0], transport_userdata);
            }
            return;
        }
    }

    // Handle regular 3-byte messages (Note On/Off, CC)
    if (midi_cb && sz >= 3) {
        midi_cb(msg[0], msg[1], msg[2], 0, cb_userdata);
    }
}

static void rtmidi_event_callback_1(double dt, const unsigned char *msg, size_t sz, void *userdata) {
    // Handle single-byte system real-time messages
    if (sz == 1) {
        if (msg[0] == MIDI_CLOCK) {
            process_midi_clock();
            return;
        }
        // Handle transport control messages
        if (msg[0] == MIDI_START || msg[0] == MIDI_STOP || msg[0] == MIDI_CONTINUE) {
            const char* msg_name = (msg[0] == MIDI_START) ? "Start" :
                                   (msg[0] == MIDI_STOP) ? "Stop" : "Continue";
            printf("[MIDI Transport] Received %s (0x%02X) on device 1, control %s\n",
                   msg_name, msg[0], transport_control_enabled ? "ENABLED" : "disabled");

            if (transport_control_enabled && transport_cb) {
                transport_cb(msg[0], transport_userdata);
            }
            return;
        }
    }

    // Handle regular 3-byte messages (Note On/Off, CC)
    if (midi_cb && sz >= 3) {
        midi_cb(msg[0], msg[1], msg[2], 1, cb_userdata);
    }
}

int midi_list_ports(void) {
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) return 0;
#endif
    RtMidiInPtr temp = rtmidi_in_create_default();
    if (!temp) return 0;
    unsigned int nports = rtmidi_get_port_count(temp);
    rtmidi_in_free(temp);
    return nports;
}

int midi_get_port_name(int port, char *name_out, int bufsize) {
    if (!name_out || bufsize <= 0) return -1;
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) return -1;
#endif

    RtMidiInPtr temp = rtmidi_in_create_default();
    if (!temp) return -1;

    unsigned int nports = rtmidi_get_port_count(temp);
    if (port < 0 || port >= (int)nports) {
        rtmidi_in_free(temp);
        return -1;
    }

    rtmidi_get_port_name(temp, port, name_out, &bufsize);
    rtmidi_in_free(temp);
    return 0;
}

int midi_init(MidiEventCallback cb, void *userdata, int port) {
    int ports[1] = {port};
    return midi_init_multi(cb, userdata, ports, 1);
}

int midi_init_multi(MidiEventCallback cb, void *userdata, const int *ports, int num_ports) {
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) return -1;
#endif

    if (num_ports > MIDI_MAX_DEVICES) num_ports = MIDI_MAX_DEVICES;

    midi_cb = cb;
    cb_userdata = userdata;

    int opened = 0;
    RtMidiCCallback callbacks[MIDI_MAX_DEVICES] = {rtmidi_event_callback_0, rtmidi_event_callback_1};

    for (int dev = 0; dev < num_ports; dev++) {
        if (ports[dev] < 0) continue;  // Skip if port is -1

        midiin[dev] = rtmidi_in_create_default();
        if (!midiin[dev]) continue;

        unsigned int nports = rtmidi_get_port_count(midiin[dev]);
        if (nports == 0 || ports[dev] >= (int)nports) {
            rtmidi_in_free(midiin[dev]);
            midiin[dev] = NULL;
            continue;
        }

        char port_name[64];
        snprintf(port_name, sizeof(port_name), "regroove-midi-in-%d", dev);
        rtmidi_open_port(midiin[dev], ports[dev], port_name);
        rtmidi_in_set_callback(midiin[dev], callbacks[dev], NULL);
        rtmidi_in_ignore_types(midiin[dev], 0, 0, 0);
        opened++;
    }

    return opened > 0 ? 0 : -1;
}

void midi_deinit(void) {
    for (int i = 0; i < MIDI_MAX_DEVICES; i++) {
        if (midiin[i]) {
            rtmidi_in_free(midiin[i]);
            midiin[i] = NULL;
        }
    }
    midi_cb = NULL;
    cb_userdata = NULL;

    // Reset clock sync state
    midi_reset_clock();
}

void midi_set_clock_sync_enabled(int enabled) {
    clock_sync_enabled = enabled;
    // Don't reset clock when disabling sync - keep processing for visual indication
}

int midi_is_clock_sync_enabled(void) {
    return clock_sync_enabled;
}

double midi_get_clock_tempo(void) {
    return clock_bpm;
}

void midi_reset_clock(void) {
    clock_bpm = 0.0;
    clock_pulse_count = 0;
    last_clock_time = 0.0;
    interval_buffer_index = 0;
    interval_buffer_filled = 0;
    memset(interval_buffer, 0, sizeof(interval_buffer));
}

void midi_set_transport_control_enabled(int enabled) {
    transport_control_enabled = enabled;
}

int midi_is_transport_control_enabled(void) {
    return transport_control_enabled;
}

void midi_set_transport_callback(MidiTransportCallback callback, void* userdata) {
    transport_cb = callback;
    transport_userdata = userdata;
}