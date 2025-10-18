#ifndef MIDI_H
#define MIDI_H

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of MIDI input devices
#define MIDI_MAX_DEVICES 2

typedef void (*MidiEventCallback)(unsigned char status, unsigned char data1, unsigned char data2, int device_id, void *userdata);

/**
 * Initialize MIDI input and set the event callback.
 * Returns 0 on success, -1 on failure.
 */
int midi_init(MidiEventCallback cb, void *userdata, int port);

/**
 * Initialize multiple MIDI inputs.
 * ports: array of port numbers (-1 = don't open)
 * num_ports: number of entries in ports array (max MIDI_MAX_DEVICES)
 * Returns number of successfully opened devices.
 */
int midi_init_multi(MidiEventCallback cb, void *userdata, const int *ports, int num_ports);

/**
 * Deinitialize MIDI input.
 */
void midi_deinit(void);

/**
 * Print available MIDI input ports.
 * Returns the number of ports found.
 */
int midi_list_ports(void);

/**
 * Get the name of a MIDI input port.
 * port: port index
 * name_out: buffer to store the port name
 * bufsize: size of name_out buffer
 * Returns 0 on success, -1 on failure.
 */
int midi_get_port_name(int port, char *name_out, int bufsize);

#ifdef __cplusplus
}
#endif
#endif