#ifndef MIDI_H
#define MIDI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*MidiEventCallback)(unsigned char status, unsigned char data1, unsigned char data2, void *userdata);

/**
 * Initialize MIDI input and set the event callback.
 * Returns 0 on success, -1 on failure.
 */
int midi_init(MidiEventCallback cb, void *userdata, int port);

/**
 * Deinitialize MIDI input.
 */
void midi_deinit(void);

/**
 * Print available MIDI input ports.
 * Returns the number of ports found.
 */
int midi_list_ports(void);

#ifdef __cplusplus
}
#endif
#endif