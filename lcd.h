#ifndef LCD_H
#define LCD_H

#ifdef __cplusplus
extern "C" {
#endif

// LCD Display Configuration (HD44780-style character LCD)
#define LCD_COLS 20  // Characters per line
#define LCD_ROWS 4   // Number of lines

// LCD handle (opaque)
typedef struct LCD LCD;

// Initialize LCD display with specified dimensions
// Returns NULL on failure
LCD* lcd_init(int cols, int rows);

// Write text to LCD (supports newline-separated lines)
// Text will be formatted to fit LCD dimensions
void lcd_write(LCD* lcd, const char* text);

// Get the formatted text buffer (read-only)
// Returns pointer to internal buffer with newline-separated lines
const char* lcd_get_buffer(const LCD* lcd);

// Clear LCD display
void lcd_clear(LCD* lcd);

// Destroy LCD instance
void lcd_destroy(LCD* lcd);

#ifdef __cplusplus
}
#endif

#endif // LCD_H
