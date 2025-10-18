#include "midi.h"
#include <unistd.h>
#include <stdio.h>
#include <rtmidi_c.h>

static RtMidiInPtr midiin[MIDI_MAX_DEVICES] = {NULL};
static MidiEventCallback midi_cb = NULL;
static void *cb_userdata = NULL;

// Device-specific callback wrappers
static void rtmidi_event_callback_0(double dt, const unsigned char *msg, size_t sz, void *userdata) {
    if (midi_cb && sz >= 3)
        midi_cb(msg[0], msg[1], msg[2], 0, cb_userdata);
}

static void rtmidi_event_callback_1(double dt, const unsigned char *msg, size_t sz, void *userdata) {
    if (midi_cb && sz >= 3)
        midi_cb(msg[0], msg[1], msg[2], 1, cb_userdata);
}

int midi_list_ports(void) {
    if (access("/dev/snd/seq", F_OK) != 0) return 0;
    RtMidiInPtr temp = rtmidi_in_create_default();
    if (!temp) return 0;
    unsigned int nports = rtmidi_get_port_count(temp);
    rtmidi_in_free(temp);
    return nports;
}

int midi_get_port_name(int port, char *name_out, int bufsize) {
    if (!name_out || bufsize <= 0) return -1;
    if (access("/dev/snd/seq", F_OK) != 0) return -1;

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
    if (access("/dev/snd/seq", F_OK) != 0) return -1;

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
}