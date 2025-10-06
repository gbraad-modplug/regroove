#include "midi.h"
#include <unistd.h>
#include <stdio.h>
#include <rtmidi_c.h>

static RtMidiInPtr midiin = NULL;
static MidiEventCallback midi_cb = NULL;
static void *cb_userdata = NULL;

static void rtmidi_event_callback(double dt, const unsigned char *msg, size_t sz, void *userdata) {
    if (midi_cb && sz >= 3)
        midi_cb(msg[0], msg[1], msg[2], cb_userdata);
}

int midi_list_ports(void) {
    if (access("/dev/snd/seq", F_OK) != 0) return 0;   
    RtMidiInPtr temp = rtmidi_in_create_default();
    if (!temp) return 0; // <- If NULL, bail out instantly
    unsigned int nports = 0;
    nports = rtmidi_get_port_count(temp);
    if (nports > 0) {
        printf("MIDI Input Ports: %u\n", nports);
        for (unsigned int i = 0; i < nports; ++i) {
            char name[128];
            int bufsize = sizeof(name);
            rtmidi_get_port_name(temp, i, name, &bufsize);
            printf("  [%u] %s\n", i, name);
        }
    }
    rtmidi_in_free(temp);
    return nports;
}

int midi_init(MidiEventCallback cb, void *userdata, int port) {
    if (access("/dev/snd/seq", F_OK) != 0) return -1; 
    midiin = rtmidi_in_create_default();
    if (!midiin) return -1; // <- If NULL, bail out instantly
    unsigned int nports = rtmidi_get_port_count(midiin);
    if (nports == 0) {
        rtmidi_in_free(midiin);
        midiin = NULL;
        return -1;
    }
    if (port < 0 || port >= (int)nports) port = 0;
    midi_cb = cb;
    cb_userdata = userdata;
    rtmidi_open_port(midiin, port, "regroove-midi-in");
    rtmidi_in_set_callback(midiin, rtmidi_event_callback, NULL);
    rtmidi_in_ignore_types(midiin, 0, 0, 0);
    return 0;
}

void midi_deinit(void) {
    if (midiin) {
        rtmidi_in_free(midiin);
        midiin = NULL;
    }
    midi_cb = NULL;
    cb_userdata = NULL;
}