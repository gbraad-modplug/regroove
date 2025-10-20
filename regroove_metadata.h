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

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_METADATA_H
