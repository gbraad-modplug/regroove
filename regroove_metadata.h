#ifndef REGROOVE_METADATA_H
#define REGROOVE_METADATA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "input_mappings.h"

#define RGX_MAX_PATTERN_DESC 128
#define RGX_MAX_PATTERNS 256
#define RGX_MAX_FILEPATH 512
#define RGX_MAX_PHRASE_NAME 64
#define RGX_MAX_PHRASE_STEPS 32
#define RGX_MAX_PHRASES 64
#define RGX_MAX_INSTRUMENTS 256  // Max instruments/samples we can map
#define RGX_MAX_INSTRUMENT_NAME 64  // Max length for custom instrument names

// Metadata for a single pattern
typedef struct {
    int pattern_index;
    char description[RGX_MAX_PATTERN_DESC];
} RegroovePatternMeta;

// Phrase step - single action in a phrase sequence
typedef struct {
    InputAction action;      // Action to execute
    int parameter;           // Action parameter
    int value;               // Action value (for continuous controls)
    int position_rows;       // Absolute position in performance rows when this step executes (0 = immediately)
} PhraseStep;

// Phrase - sequence of actions
typedef struct {
    char name[RGX_MAX_PHRASE_NAME];
    PhraseStep steps[RGX_MAX_PHRASE_STEPS];
    int step_count;
} Phrase;

// .rgx file metadata container
typedef struct {
    int version;
    char module_file[RGX_MAX_FILEPATH];

    // Pattern descriptions
    RegroovePatternMeta *pattern_meta;
    int pattern_meta_count;
    int pattern_meta_capacity;

    // Phrases (song-specific action sequences)
    Phrase phrases[RGX_MAX_PHRASES];
    int phrase_count;

    // Song-specific trigger pads (S1-S16)
    TriggerPadConfig song_trigger_pads[MAX_SONG_TRIGGER_PADS];

    // Loop ranges (for trigger_loop action)
    // Each loop range defines start/end points
    // -1 for order means single-pattern mode (use current pattern)
    struct {
        int start_order;
        int start_row;
        int end_order;
        int end_row;
        char description[64];  // Loop description (e.g., "INTRO", "CHORUS", "BRIDGE")
    } loop_ranges[16];  // Support up to 16 saved loop ranges
    int loop_range_count;

    // Channel names (song-global, shown in channel actions on pads)
    // Empty string means use default channel name (e.g., "CH 1")
    char channel_names[64][32];  // Support up to 64 channels

    // Channel default panning (song-global override)
    // -1 = use module's default, 0 = hard left, 64 = center, 127 = hard right
    int channel_pan[64];  // Support up to 64 channels

    // MIDI output channel mapping for instruments/samples
    // -2 = disabled (no MIDI output), -1 = auto (instrument_index % 16), 0-15 = specific MIDI channel
    int instrument_midi_channels[RGX_MAX_INSTRUMENTS];
    int has_midi_mapping;  // 0 = no custom mapping, 1 = custom mapping exists

    // Custom instrument/sample name overrides (for MIDI mapping display)
    // Empty string means use the module's original name
    char instrument_names[RGX_MAX_INSTRUMENTS][RGX_MAX_INSTRUMENT_NAME];

    // Global MIDI note offset (applied to all notes, in semitones)
    // Positive = shift up, negative = shift down, 0 = no offset
    int midi_note_offset;

    // MIDI program change (preset) per instrument
    // -1 = no program change, 0-127 = MIDI program number
    int instrument_program[RGX_MAX_INSTRUMENTS];
} RegrooveMetadata;

// Create new metadata structure
RegrooveMetadata* regroove_metadata_create(void);

// Load .rgx file
int regroove_metadata_load(RegrooveMetadata *meta, const char *rgx_path);

// Save .rgx file
int regroove_metadata_save(const RegrooveMetadata *meta, const char *rgx_path);

// Set pattern description
void regroove_metadata_set_pattern_desc(RegrooveMetadata *meta, int pattern_index, const char *description);

// Get pattern description (returns NULL if not found)
const char* regroove_metadata_get_pattern_desc(const RegrooveMetadata *meta, int pattern_index);

// Free metadata structure
void regroove_metadata_destroy(RegrooveMetadata *meta);

// Generate .rgx path from module path (e.g., "file.mod" -> "file.rgx")
void regroove_metadata_get_rgx_path(const char *module_path, char *rgx_path, size_t rgx_path_size);

// Get MIDI channel for instrument/sample (-1 = use default, 0-15 = specific channel)
int regroove_metadata_get_midi_channel(const RegrooveMetadata *meta, int instrument_index);

// Set MIDI channel for instrument/sample (-1 = use default, 0-15 = specific channel)
void regroove_metadata_set_midi_channel(RegrooveMetadata *meta, int instrument_index, int midi_channel);

// Get custom instrument name (returns NULL or empty string if using module's original name)
const char* regroove_metadata_get_instrument_name(const RegrooveMetadata *meta, int instrument_index);

// Set custom instrument name (empty string or NULL to use module's original name)
void regroove_metadata_set_instrument_name(RegrooveMetadata *meta, int instrument_index, const char *name);

// Get global MIDI note offset (in semitones)
int regroove_metadata_get_note_offset(const RegrooveMetadata *meta);

// Set global MIDI note offset (in semitones, can be negative)
void regroove_metadata_set_note_offset(RegrooveMetadata *meta, int offset);

// Get MIDI program change for instrument (-1 = no program change, 0-127 = program number)
int regroove_metadata_get_program(const RegrooveMetadata *meta, int instrument_index);

// Set MIDI program change for instrument (-1 = no program change, 0-127 = program number)
void regroove_metadata_set_program(RegrooveMetadata *meta, int instrument_index, int program);

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_METADATA_H
