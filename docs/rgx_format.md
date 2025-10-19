# .rgx File Format

The `.rgx` file format is used to store metadata for module files (MOD, XM, S3M, IT, etc.) in Regroove.

## Purpose

- Store pattern descriptions for organizational purposes
- Allow future extensibility (e.g., migration to TOML format)
- Keep metadata separate from the original module file

## Format

The file uses INI format with the following structure:

```ini
[Regroove]
version=1
file="MODULENAME.ext"

[Patterns]
pattern_0="main beat"
pattern_1="breakdown"
pattern_2="riser"
pattern_3="here we go!"
```

## File Naming

The `.rgx` file must have the same base name as the module file:
- Module: `song.mod` → Metadata: `song.rgx`
- Module: `track.xm` → Metadata: `track.rgx`

## Sections

### [Regroove]

- `version`: Format version number (currently 1)
- `file`: Original module filename (basename only, not full path)

### [Patterns]

- Pattern descriptions in the format: `pattern_N="description"`
- Pattern indices start at 0
- Descriptions are optional and limited to 128 characters

## Usage in Regroove

### Loading Files

You can load files in two ways:

1. **Load a module file** (`.mod`, `.xm`, `.s3m`, `.it`, `.med`, etc.)
   - Regroove automatically looks for a matching `.rgx` file
   - If found, pattern descriptions are loaded from the `.rgx` file

2. **Load an .rgx file directly**
   - Regroove reads the `file=` field in the `[Regroove]` section
   - Automatically loads the referenced module file
   - Loads all pattern descriptions from the `.rgx` file
   - This allows you to have **multiple .rgx files** for the same module (different performances/arrangements)

### Editing Pattern Descriptions (GUI)
1. Load a module file or `.rgx` file
2. Switch to the INFO panel
3. Scroll to "Pattern Descriptions" section
4. Edit pattern descriptions in the text fields
5. Changes are automatically saved to the `.rgx` file

### Auto-Save
Pattern descriptions are saved immediately when you finish typing in each field. No manual save action is required.

### Multiple Performances
You can create multiple `.rgx` files for the same module:
- `song-verse.rgx` → references `song.mod`
- `song-chorus.rgx` → references `song.mod`
- `song-breakdown.rgx` → references `song.mod`

Each can have different pattern descriptions for different performance contexts.

## Future Extensibility

The `version` field allows for format changes in the future:
- Version 2+ might use TOML format
- Additional metadata fields can be added (e.g., tempo maps, markers, custom loops)

## Example Workflows

### Basic Workflow
```
1. Load: JUSTICE.med
2. Edit pattern descriptions in INFO panel:
   Pattern 0: "intro"
   Pattern 1: "main groove"
   Pattern 2: "breakdown"
3. File JUSTICE.rgx is auto-created/updated
4. Next time: Load JUSTICE.med → descriptions are restored
```

### Multiple Performance Workflow
```
1. Load: JUSTICE.med
2. Edit descriptions for verse arrangement
3. JUSTICE.rgx is saved
4. Manually copy JUSTICE.rgx to JUSTICE-chorus.rgx
5. Edit JUSTICE-chorus.rgx in a text editor, change pattern descriptions
6. Now you have two performance files:
   - Load JUSTICE.med → gets "verse" descriptions from JUSTICE.rgx
   - Load JUSTICE-chorus.rgx → gets "chorus" descriptions, loads JUSTICE.med
```

## Technical Details

- Implementation: `regroove_metadata.c`, `regroove_metadata.h`
- Integration: `regroove_common.c` (auto-load), `main-gui.cpp` (UI + auto-save)
- Build: Included in both `regroove-gui` and `regroove-tui` targets
