#pragma once

/**
 * ESL BLE Layer — now implemented as a UART serial bridge.
 *
 * The Flipper Zero SDK does not expose BLE Central APIs (scanning / GATT
 * client writes) to third-party FAPs.  Instead this module communicates with
 * an external ESP32 module over UART (GPIO C1=TX / C0=RX, 115200 baud).
 *
 * The ESP32 runs the esp32_ble_bridge firmware (tools/esp32_ble_bridge/) which
 * handles all BLE Central operations and reports results back via the simple
 * newline-delimited text protocol documented below.
 *
 * Protocol — Flipper → ESP32:
 *   SCAN\n
 *   ATC_PRICE <mac> <price>\n
 *   ATC_PRICE <mac> <price> <label>\n
 *   ATC_CLEAR <mac>\n
 *   VUSION_PROVISION <mac>\n
 *   VUSION_DISPLAY <mac> <idx>\n
 *   VUSION_PING <mac>\n
 *   VUSION_RESET <mac>\n
 *
 * Protocol — ESP32 → Flipper:
 *   DEVICE <name> <mac> <rssi> ATC|VUSION\n   (may arrive multiple times during SCAN)
 *   DONE\n                                     (scan/operation complete)
 *   OK <message>\n
 *   ERROR <message>\n
 *   PROGRESS <0-100>\n
 */

#include <stdint.h>
#include <stdbool.h>
#include "esl_protocol.h"

// Maximum number of ESL devices to track during a scan
#define ESL_MAX_DEVICES     10

// Maximum length of a BLE device name
#define ESL_DEVICE_NAME_LEN 28

// Maximum MAC address string length ("AA:BB:CC:DD:EE:FF" + NUL)
#define ESL_MAC_STR_LEN     18

// Tag type detected during scan
typedef enum {
    EslTagTypeATC,      // Hanshow Stellar + ATC_TLSR_Paper firmware
    EslTagTypeVusion,   // SES-imagotag / Vusion HRD3 (BT SIG ESL Service 0x184D)
    EslTagTypeUnknown,
} EslTagType;

typedef struct {
    char        name[ESL_DEVICE_NAME_LEN];
    char        mac[ESL_MAC_STR_LEN];
    int8_t      rssi;
    bool        valid;
    EslTagType  tag_type;
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

/** Allocate and initialise the UART bridge context. */
EslBle* esl_ble_alloc(void);

/** Free the UART bridge context. */
void esl_ble_free(EslBle* ble);

/** Get the current state. */
EslBleState esl_ble_get_state(EslBle* ble);

/**
 * Start scanning for ESL devices via the UART bridge.
 * Results arrive asynchronously via on_scan callback.
 */
void esl_ble_start_scan(
    EslBle* ble,
    uint32_t timeout_ms,
    EslBleScanCallback on_scan,
    EslBleDoneCallback on_done,
    void* ctx);

/** Stop an in-progress scan. */
void esl_ble_stop_scan(EslBle* ble);

/**
 * Connect to a device.  For the UART bridge, "connecting" just means we
 * store the MAC — the actual BLE connection is made when an operation is
 * sent to the ESP32.
 */
void esl_ble_connect(
    EslBle* ble,
    const char* mac,
    EslTagType tag_type,
    EslBleDoneCallback on_done,
    void* ctx);

/** Disconnect / clear the connected MAC. */
void esl_ble_disconnect(EslBle* ble);

/**
 * Upload an image + price to an ATC tag.
 * Sends "ATC_PRICE <mac> <price> [<label>]" to the bridge.
 */
void esl_ble_upload_image(
    EslBle* ble,
    const char* price,
    const char* label,
    EslBleProgressCallback on_progress,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Clear an ATC tag display.
 * Sends "ATC_CLEAR <mac>" to the bridge.
 */
void esl_ble_clear_display(
    EslBle* ble,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Provision a Vusion tag via the bridge.
 * Sends "VUSION_PROVISION <mac>" — bridge bonds, writes 4 ESL characteristics.
 */
void esl_ble_vusion_provision(
    EslBle* ble,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Display a pre-stored image slot on a Vusion tag.
 * Sends "VUSION_DISPLAY <mac> <idx>" — bridge writes Display Image TLV.
 */
void esl_ble_vusion_display(
    EslBle* ble,
    uint8_t image_idx,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Ping a Vusion tag.
 * Sends "VUSION_PING <mac>".
 */
void esl_ble_vusion_ping(
    EslBle* ble,
    EslBleDoneCallback on_done,
    void* ctx);

/**
 * Factory reset a Vusion tag.
 * Sends "VUSION_RESET <mac>".
 */
void esl_ble_vusion_reset(
    EslBle* ble,
    EslBleDoneCallback on_done,
    void* ctx);

/** Get the list of devices found during the last scan. */
uint8_t esl_ble_get_scan_results(EslBle* ble, EslDevice out_devices[ESL_MAX_DEVICES]);

/**
 * Abort any in-progress operation (upload / provision / ping / reset).
 * Sets abort_requested, joins the worker thread, resets state to Idle.
 * Safe to call when no operation is running (no-op in that case).
 */
void esl_ble_abort_operation(EslBle* ble);
