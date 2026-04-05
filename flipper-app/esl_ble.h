#pragma once

/**
 * ESL BLE — Bluetooth Low Energy communication layer.
 *
 * Handles scanning for ESL_XXXXXX devices and performing GATT writes
 * to upload images and commands.
 *
 * On official Flipper firmware: uses USB Serial bridge to companion Python script.
 * On community firmware (Unleashed/Momentum): uses direct BLE GATT client.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esl_protocol.h"

// Maximum number of ESL devices to track during a scan
#define ESL_MAX_DEVICES 10

// Maximum length of a BLE device name
#define ESL_DEVICE_NAME_LEN 24

// Maximum MAC address string length ("AA:BB:CC:DD:EE:FF" + NUL)
#define ESL_MAC_STR_LEN 18

typedef struct {
    char     name[ESL_DEVICE_NAME_LEN];
    char     mac[ESL_MAC_STR_LEN];
    int8_t   rssi;
    bool     valid;
} EslDevice;

typedef enum {
    EslBleStateIdle,
    EslBleStateScanning,
    EslBleStateConnecting,
    EslBleStateConnected,
    EslBleStateUploading,
    EslBleStateError,
} EslBleState;

typedef struct EslBle EslBle;

// Callback fired when scan results are updated (may be called multiple times during scan)
typedef void (*EslBleScanCallback)(const EslDevice* devices, uint8_t count, void* ctx);

// Callback fired when upload progress changes (progress 0–100)
typedef void (*EslBleProgressCallback)(uint8_t progress, void* ctx);

// Callback fired when an operation completes (success=true) or fails (success=false)
typedef void (*EslBleDoneCallback)(bool success, const char* message, void* ctx);

/**
 * Allocate and initialise the BLE context.
 * Must be called before any other esl_ble_* functions.
 */
EslBle* esl_ble_alloc(void);

/**
 * Free the BLE context. Disconnects any active connection first.
 */
void esl_ble_free(EslBle* ble);

/**
 * Get the current BLE state.
 */
EslBleState esl_ble_get_state(EslBle* ble);

/**
 * Start scanning for ESL_ BLE devices.
 *
 * @param ble           BLE context.
 * @param timeout_ms    Scan duration in milliseconds (0 = scan until stop).
 * @param on_scan       Callback invoked when new devices are found.
 * @param on_done       Callback invoked when the scan completes.
 * @param ctx           User context pointer passed to callbacks.
 */
void esl_ble_start_scan(
    EslBle* ble,
    uint32_t timeout_ms,
    EslBleScanCallback on_scan,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Stop an in-progress scan.
 */
void esl_ble_stop_scan(EslBle* ble);

/**
 * Connect to a device by MAC address.
 *
 * @param ble       BLE context.
 * @param mac       MAC address string "AA:BB:CC:DD:EE:FF".
 * @param on_done   Callback when connected (success=true) or failed.
 * @param ctx       User context.
 */
void esl_ble_connect(
    EslBle* ble,
    const char* mac,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Disconnect from the currently connected device.
 */
void esl_ble_disconnect(EslBle* ble);

/**
 * Upload an image buffer to the connected tag.
 *
 * Performs the full upload sequence:
 *   1. Clear buffer to white
 *   2. Set position to 0
 *   3. Write buffer in chunks
 *   4. Trigger display refresh
 *
 * @param ble           BLE context (must be connected).
 * @param img           Image buffer to upload.
 * @param on_progress   Progress callback (0–100).
 * @param on_done       Completion callback.
 * @param ctx           User context.
 */
void esl_ble_upload_image(
    EslBle* ble,
    const EslImageBuffer* img,
    EslBleProgressCallback on_progress,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Send a single raw command to the EPD characteristic.
 * Used for simple operations (clear, display, etc.).
 */
void esl_ble_send_command(
    EslBle* ble,
    const uint8_t* cmd_buf,
    uint8_t cmd_len,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Get the list of devices found during the last scan.
 * Returns the number of valid entries in out_devices[].
 */
uint8_t esl_ble_get_scan_results(EslBle* ble, EslDevice out_devices[ESL_MAX_DEVICES]);
