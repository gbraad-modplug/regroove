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

/**
 * Enable or disable MIDI Clock synchronization.
 * When enabled, incoming MIDI Clock messages (0xF8) will be used to calculate tempo.
 * enabled: 1 to enable, 0 to disable
 */
void midi_set_clock_sync_enabled(int enabled);

/**
 * Get the current MIDI Clock sync state.
 * Returns 1 if enabled, 0 if disabled.
 */
int midi_is_clock_sync_enabled(void);

/**
 * Get the tempo calculated from incoming MIDI Clock messages.
 * Returns BPM as a double, or 0.0 if no clock is being received.
 */
double midi_get_clock_tempo(void);

/**
 * Reset MIDI Clock timing (call when playback stops or tempo changes externally).
 */
void midi_reset_clock(void);

/**
 * Enable or disable responding to MIDI Start/Stop/Continue messages.
 * When enabled, incoming 0xFA (Start), 0xFC (Stop), 0xFB (Continue) will trigger playback control.
 * enabled: 1 to enable, 0 to disable
 */
void midi_set_transport_control_enabled(int enabled);

/**
 * Get the current MIDI transport control state.
 * Returns 1 if enabled, 0 if disabled.
 */
int midi_is_transport_control_enabled(void);

/**
 * Set callback for transport control messages (Start/Stop/Continue).
 * The callback receives: message_type (0xFA=Start, 0xFC=Stop, 0xFB=Continue)
 */
typedef void (*MidiTransportCallback)(unsigned char message_type, void* userdata);
void midi_set_transport_callback(MidiTransportCallback callback, void* userdata);

/**
 * Set callback for MIDI Song Position Pointer (SPP) messages.
 * The callback receives: position (in MIDI beats / 16th notes from start)
 */
typedef void (*MidiSPPCallback)(int position, void* userdata);
void midi_set_spp_callback(MidiSPPCallback callback, void* userdata);

#ifdef __cplusplus
}
#endif
#endif