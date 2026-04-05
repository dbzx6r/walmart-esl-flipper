#pragma once

/**
 * ESL Protocol — image encoding and BLE command building.
 *
 * Implements the ATC_TLSR_Paper BLE GATT protocol:
 *   Service UUID : 13187B10-EBA9-A3BA-044E-83D3217D9A38
 *   Char UUID    : 4B646063-6264-F3A7-8941-E65356EA82FE
 *
 * Buffer format (250×122 BW213 display):
 *   4000 bytes (250 columns × 16 bytes/col), rotated 90° clockwise.
 *   Pixel (x,y): byte=(249-x)*16+(y>>3), bit=0x80>>(y&7); black=0, white=1
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ── Display model definitions ────────────────────────────────────────────────

typedef enum {
    EslModelBW213    = 1,  // 250×122 Black/White 2.13"
    EslModelBWR213   = 2,  // 250×122 Black/White/Red 2.13"
    EslModelBWR154   = 3,  // 200×200 Black/White/Red 1.54"
    EslModelBW213ICE = 4,  // 212×104 Black/White alt 2.13"
    EslModelBWR350   = 5,  // Larger Black/White/Red
    EslModelBWY350   = 6,  // Larger Black/White/Yellow
} EslDisplayModel;

typedef struct {
    uint16_t width;
    uint16_t height;
} EslDisplaySize;

// Returns the (width, height) pixel dimensions for a given model.
EslDisplaySize esl_model_size(EslDisplayModel model);

// Returns the raw buffer size in bytes for a given model.
uint32_t esl_buffer_size(EslDisplayModel model);

// ── Image buffer ─────────────────────────────────────────────────────────────

// Maximum buffer size across all supported models (for static allocation)
#define ESL_MAX_BUFFER_SIZE 4096

typedef struct {
    uint8_t  data[ESL_MAX_BUFFER_SIZE];
    uint32_t size;        // actual used size for the model
    EslDisplayModel model;
} EslImageBuffer;

/**
 * Initialise an image buffer to all-white for the given display model.
 */
void esl_buffer_init(EslImageBuffer* buf, EslDisplayModel model);

/**
 * Set a single pixel in the buffer.
 *
 * @param buf    Image buffer (must be initialised for the correct model).
 * @param x      Pixel x-coordinate (0 = left).
 * @param y      Pixel y-coordinate (0 = top).
 * @param black  true = black pixel, false = white pixel.
 */
void esl_buffer_set_pixel(EslImageBuffer* buf, uint16_t x, uint16_t y, bool black);

// ── Text / price rendering ───────────────────────────────────────────────────

/**
 * Render a price string into an image buffer.
 *
 * Draws the price text large and centred, with an optional small label line.
 * Uses the built-in ESL bitmap font for digits, currency symbols, and basic chars.
 *
 * @param buf        Destination image buffer (will be cleared first).
 * @param price      Price text, e.g. "$12.99" (max 12 chars).
 * @param label      Optional small label text above the price (max 24 chars, "" = none).
 */
void esl_render_price(EslImageBuffer* buf, const char* price, const char* label);

// ── BLE command serialisation ─────────────────────────────────────────────────

// Maximum bytes in a single BLE write (safe below typical MTU)
#define ESL_BLE_CHUNK_SIZE 239

// Command bytes
#define ESL_CMD_CLEAR    0x00
#define ESL_CMD_DISPLAY  0x01
#define ESL_CMD_SET_POS  0x02
#define ESL_CMD_WRITE    0x03

/**
 * Build a CLEAR command packet.
 *
 * @param out_buf  Output buffer (must be >= 2 bytes).
 * @param fill     Fill byte: 0xFF = white, 0x00 = black.
 * @return         Length of the packet written to out_buf.
 */
uint8_t esl_cmd_clear(uint8_t* out_buf, uint8_t fill);

/**
 * Build a SET_POSITION command packet.
 *
 * @param out_buf  Output buffer (must be >= 3 bytes).
 * @param pos      Write position (0 = start of buffer).
 * @return         Length of the packet.
 */
uint8_t esl_cmd_set_pos(uint8_t* out_buf, uint16_t pos);

/**
 * Build a WRITE_DATA command packet.
 *
 * @param out_buf    Output buffer (must be >= data_len + 1 bytes).
 * @param data       Image data chunk.
 * @param data_len   Number of bytes (max ESL_BLE_CHUNK_SIZE).
 * @return           Length of the packet.
 */
uint8_t esl_cmd_write(uint8_t* out_buf, const uint8_t* data, uint8_t data_len);

/**
 * Build a DISPLAY command packet (triggers e-ink refresh).
 *
 * @param out_buf  Output buffer (must be >= 1 byte).
 * @return         Length of the packet.
 */
uint8_t esl_cmd_display(uint8_t* out_buf);
