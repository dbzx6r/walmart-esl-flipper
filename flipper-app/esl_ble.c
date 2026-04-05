/**
 * ESL BLE layer — implemented as a UART serial bridge to an ESP32 module.
 *
 * Uses the Flipper's LPUART (GPIO C1=TX / C0=RX) at 115200 baud to
 * communicate with an external ESP32 running the esp32_ble_bridge firmware.
 * All BLE Central operations (scanning, connecting, GATT writes, bonding)
 * are handled by the ESP32; this module is responsible for the serial
 * framing and callback dispatch.
 *
 * See esl_ble.h for the full protocol specification.
 */

#include "esl_ble.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ── Configuration ─────────────────────────────────────────────────────────────

#define ESL_UART_BAUD        115200
#define ESL_UART_ID          FuriHalSerialIdLpuart   // GPIO C1=TX / C0=RX
#define ESL_RX_BUF_SIZE      256
#define ESL_LINE_MAX         128
#define ESL_STREAM_SIZE      512
#define ESL_WORKER_STACK     2048

// ── Internal state ────────────────────────────────────────────────────────────

struct EslBle {
    EslBleState  state;

    // UART handle
    FuriHalSerialHandle* serial;

    // Thread-safe ring buffer: ISR → worker
    FuriStreamBuffer* rx_stream;

    // Worker thread processes serial lines
    FuriThread*  worker;
    bool         worker_running;

    // Pending operation parameters
    char         pending_cmd[ESL_LINE_MAX];   // full command string to send

    // Connected device MAC + type
    char         connected_mac[ESL_MAC_STR_LEN];
    EslTagType   connected_type;

    // Scan state
    EslDevice    devices[ESL_MAX_DEVICES];
    uint8_t      device_count;
    FuriMutex*   devices_mutex;

    // Callbacks
    EslBleScanCallback     on_scan;
    EslBleProgressCallback on_progress;
    EslBleDoneCallback     on_done;
    void*                  cb_ctx;

    // Signal worker to abort current operation
    bool         abort_requested;
};

// ── UART receive ISR callback ─────────────────────────────────────────────────

static void _uart_rx_cb(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* ctx)
{
    EslBle* ble = (EslBle*)ctx;
    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t byte = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(ble->rx_stream, &byte, 1, 0);
        }
    }
}

// ── UART transmit helper ──────────────────────────────────────────────────────

static void _uart_send(EslBle* ble, const char* str) {
    furi_hal_serial_tx(ble->serial, (const uint8_t*)str, strlen(str));
    furi_hal_serial_tx_wait_complete(ble->serial);
}

// ── Line reader (blocking, with timeout) ─────────────────────────────────────
//
// Returns number of bytes in line (not counting NUL terminator), or 0 on timeout.

static uint8_t _read_line(EslBle* ble, char* out, uint8_t max_len, uint32_t timeout_ms) {
    uint8_t pos = 0;
    uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(timeout_ms);

    while(furi_get_tick() < deadline && pos < max_len - 1) {
        if(ble->abort_requested) break;

        uint8_t byte = 0;
        size_t got = furi_stream_buffer_receive(ble->rx_stream, &byte, 1, 5);
        if(got == 0) continue;

        if(byte == '\r') continue;  // skip CR
        if(byte == '\n') {
            out[pos] = '\0';
            return pos;
        }
        out[pos++] = (char)byte;
    }
    out[pos] = '\0';
    return pos;
}

// ── Line parser helpers ───────────────────────────────────────────────────────

// Parse "DEVICE name mac rssi ATC|VUSION" into an EslDevice
static bool _parse_device_line(const char* line, EslDevice* dev) {
    // Format: DEVICE <name> <mac> <rssi> <type>
    // name and mac cannot contain spaces; rssi is negative integer
    char name[ESL_DEVICE_NAME_LEN] = {0};
    char mac[ESL_MAC_STR_LEN]      = {0};
    int  rssi = 0;
    char type[16]                  = {0};

    int parsed = sscanf(line + 7, "%27s %17s %d %15s", name, mac, &rssi, type);
    if(parsed < 4) return false;

    strncpy(dev->name, name, ESL_DEVICE_NAME_LEN - 1);
    dev->name[ESL_DEVICE_NAME_LEN - 1] = '\0';
    strncpy(dev->mac,  mac,  ESL_MAC_STR_LEN - 1);
    dev->mac[ESL_MAC_STR_LEN - 1] = '\0';
    dev->rssi  = (int8_t)rssi;
    dev->valid = true;

    if(strncmp(type, "VUSION", 6) == 0) {
        dev->tag_type = EslTagTypeVusion;
    } else if(strncmp(type, "ATC", 3) == 0) {
        dev->tag_type = EslTagTypeATC;
    } else {
        dev->tag_type = EslTagTypeUnknown;
    }
    return true;
}

// ── Operation worker ──────────────────────────────────────────────────────────
//
// Runs on a FuriThread.  Sends the pending command, then reads lines until
// DONE, OK, or ERROR is received.

static int32_t _worker(void* ctx) {
    EslBle* ble = (EslBle*)ctx;

    // Drain any stale bytes in the stream before sending the command
    {
        uint8_t discard;
        while(furi_stream_buffer_receive(ble->rx_stream, &discard, 1, 0) > 0) {}
    }

    // Send the command
    _uart_send(ble, ble->pending_cmd);

    // Determine if this is a SCAN (expects DEVICE lines + DONE)
    bool is_scan = (strncmp(ble->pending_cmd, "SCAN", 4) == 0);

    char line[ESL_LINE_MAX];
    while(ble->worker_running && !ble->abort_requested) {
        uint8_t len = _read_line(ble, line, sizeof(line), 30000);

        if(len == 0) {
            // Timeout
            if(ble->on_done) {
                ble->on_done(false, "ESP32 timeout — check wiring and firmware", ble->cb_ctx);
            }
            break;
        }

        if(strncmp(line, "DEVICE ", 7) == 0 && is_scan) {
            EslDevice dev;
            memset(&dev, 0, sizeof(dev));
            if(_parse_device_line(line, &dev)) {
                furi_mutex_acquire(ble->devices_mutex, FuriWaitForever);
                // Update existing or add new
                bool found = false;
                for(uint8_t i = 0; i < ble->device_count; i++) {
                    if(strncmp(ble->devices[i].mac, dev.mac, ESL_MAC_STR_LEN) == 0) {
                        ble->devices[i].rssi = dev.rssi;
                        found = true;
                        break;
                    }
                }
                if(!found && ble->device_count < ESL_MAX_DEVICES) {
                    ble->devices[ble->device_count++] = dev;
                }
                uint8_t cnt = ble->device_count;
                // Make a local copy for callback (mutex released before calling)
                EslDevice snapshot[ESL_MAX_DEVICES];
                for(uint8_t i = 0; i < cnt; i++) snapshot[i] = ble->devices[i];
                furi_mutex_release(ble->devices_mutex);

                if(ble->on_scan) {
                    ble->on_scan(snapshot, cnt, ble->cb_ctx);
                }
            }
            continue;
        }

        if(strncmp(line, "PROGRESS ", 9) == 0) {
            if(ble->on_progress) {
                uint8_t pct = (uint8_t)atoi(line + 9);
                ble->on_progress(pct, ble->cb_ctx);
            }
            continue;
        }

        if(strncmp(line, "DONE", 4) == 0) {
            ble->state = EslBleStateIdle;
            if(ble->on_done) {
                ble->on_done(true, is_scan ? "Scan complete" : "Done", ble->cb_ctx);
            }
            break;
        }

        if(strncmp(line, "OK ", 3) == 0 || strncmp(line, "OK\n", 3) == 0 ||
           strcmp(line, "OK") == 0) {
            ble->state = EslBleStateIdle;
            const char* msg = (len > 3) ? line + 3 : "Done";
            if(ble->on_done) {
                ble->on_done(true, msg, ble->cb_ctx);
            }
            break;
        }

        if(strncmp(line, "ERROR ", 6) == 0) {
            ble->state = EslBleStateError;
            if(ble->on_done) {
                ble->on_done(false, line + 6, ble->cb_ctx);
            }
            break;
        }
        // Ignore unrecognised lines
    }

    ble->worker_running = false;
    return 0;
}

// ── Start worker helper ───────────────────────────────────────────────────────

static void _start_worker(EslBle* ble) {
    // Join and free any previous worker
    if(ble->worker) {
        furi_thread_join(ble->worker);
        furi_thread_free(ble->worker);
        ble->worker = NULL;
    }
    ble->abort_requested = false;
    ble->worker_running  = true;
    ble->worker = furi_thread_alloc_ex("esl_bridge", ESL_WORKER_STACK, _worker, ble);
    furi_thread_set_priority(ble->worker, FuriThreadPriorityNormal);
    furi_thread_start(ble->worker);
}

// ── Allocation / free ─────────────────────────────────────────────────────────

EslBle* esl_ble_alloc(void) {
    EslBle* ble = malloc(sizeof(EslBle));
    furi_assert(ble);
    memset(ble, 0, sizeof(EslBle));
    ble->state = EslBleStateIdle;
    ble->devices_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    ble->rx_stream = furi_stream_buffer_alloc(ESL_STREAM_SIZE, 1);

    // Acquire and configure the LPUART
    ble->serial = furi_hal_serial_control_acquire(ESL_UART_ID);
    if(ble->serial) {
        furi_hal_serial_init(ble->serial, ESL_UART_BAUD);
        furi_hal_serial_async_rx_start(ble->serial, _uart_rx_cb, ble, false);
    }

    return ble;
}

void esl_ble_free(EslBle* ble) {
    furi_assert(ble);

    ble->abort_requested = true;
    if(ble->worker) {
        furi_thread_join(ble->worker);
        furi_thread_free(ble->worker);
        ble->worker = NULL;
    }

    if(ble->serial) {
        furi_hal_serial_async_rx_stop(ble->serial);
        furi_hal_serial_deinit(ble->serial);
        furi_hal_serial_control_release(ble->serial);
        ble->serial = NULL;
    }

    furi_stream_buffer_free(ble->rx_stream);
    furi_mutex_free(ble->devices_mutex);
    free(ble);
}

// ── Accessors ─────────────────────────────────────────────────────────────────

EslBleState esl_ble_get_state(EslBle* ble) {
    furi_assert(ble);
    return ble->state;
}

uint8_t esl_ble_get_scan_results(EslBle* ble, EslDevice out_devices[ESL_MAX_DEVICES]) {
    furi_assert(ble);
    furi_mutex_acquire(ble->devices_mutex, FuriWaitForever);
    uint8_t cnt = ble->device_count;
    for(uint8_t i = 0; i < cnt; i++) out_devices[i] = ble->devices[i];
    furi_mutex_release(ble->devices_mutex);
    return cnt;
}

// ── Scan ──────────────────────────────────────────────────────────────────────

void esl_ble_start_scan(
    EslBle* ble,
    uint32_t timeout_ms,
    EslBleScanCallback on_scan,
    EslBleDoneCallback on_done,
    void* ctx)
{
    furi_assert(ble);
    if(ble->state != EslBleStateIdle) return;
    UNUSED(timeout_ms);  // ESP32 firmware uses its own scan timeout

    ble->state        = EslBleStateScanning;
    ble->device_count = 0;
    memset(ble->devices, 0, sizeof(ble->devices));
    ble->on_scan = on_scan;
    ble->on_done = on_done;
    ble->cb_ctx  = ctx;

    strncpy(ble->pending_cmd, "SCAN\n", sizeof(ble->pending_cmd) - 1);
    _start_worker(ble);
}

void esl_ble_stop_scan(EslBle* ble) {
    furi_assert(ble);
    if(ble->state != EslBleStateScanning) return;
    ble->abort_requested = true;
    if(ble->worker) {
        furi_thread_join(ble->worker);
        furi_thread_free(ble->worker);
        ble->worker = NULL;
    }
    ble->state = EslBleStateIdle;
    if(ble->on_done) {
        ble->on_done(true, "Scan stopped", ble->cb_ctx);
        ble->on_done = NULL;
    }
}

// ── Connect / Disconnect ──────────────────────────────────────────────────────

void esl_ble_connect(
    EslBle* ble,
    const char* mac,
    EslTagType tag_type,
    EslBleDoneCallback on_done,
    void* ctx)
{
    furi_assert(ble);
    furi_assert(mac);
    strncpy(ble->connected_mac, mac, ESL_MAC_STR_LEN - 1);
    ble->connected_mac[ESL_MAC_STR_LEN - 1] = '\0';
    ble->connected_type = tag_type;
    ble->state = EslBleStateConnected;
    if(on_done) on_done(true, "Ready", ctx);
}

void esl_ble_disconnect(EslBle* ble) {
    furi_assert(ble);
    ble->connected_mac[0] = '\0';
    ble->state = EslBleStateIdle;
}

// ── ATC operations ────────────────────────────────────────────────────────────

void esl_ble_upload_image(
    EslBle* ble,
    const char* price,
    const char* label,
    EslBleProgressCallback on_progress,
    EslBleDoneCallback on_done,
    void* ctx)
{
    furi_assert(ble);
    if(ble->state != EslBleStateConnected) {
        if(on_done) on_done(false, "Not connected", ctx);
        return;
    }
    ble->state       = EslBleStateUploading;
    ble->on_progress = on_progress;
    ble->on_done     = on_done;
    ble->cb_ctx      = ctx;

    if(label && label[0]) {
        snprintf(ble->pending_cmd, sizeof(ble->pending_cmd),
                 "ATC_PRICE %s %s %s\n", ble->connected_mac, price, label);
    } else {
        snprintf(ble->pending_cmd, sizeof(ble->pending_cmd),
                 "ATC_PRICE %s %s\n", ble->connected_mac, price);
    }
    _start_worker(ble);
}

void esl_ble_clear_display(
    EslBle* ble,
    EslBleDoneCallback on_done,
    void* ctx)
{
    furi_assert(ble);
    if(ble->state != EslBleStateConnected) {
        if(on_done) on_done(false, "Not connected", ctx);
        return;
    }
    ble->state   = EslBleStateUploading;
    ble->on_done = on_done;
    ble->cb_ctx  = ctx;
    ble->on_progress = NULL;

    snprintf(ble->pending_cmd, sizeof(ble->pending_cmd),
             "ATC_CLEAR %s\n", ble->connected_mac);
    _start_worker(ble);
}

// ── Vusion operations ─────────────────────────────────────────────────────────

void esl_ble_vusion_provision(EslBle* ble, EslBleDoneCallback on_done, void* ctx) {
    furi_assert(ble);
    if(ble->state != EslBleStateConnected) {
        if(on_done) on_done(false, "Not connected", ctx);
        return;
    }
    ble->state   = EslBleStateUploading;
    ble->on_done = on_done;
    ble->cb_ctx  = ctx;
    ble->on_progress = NULL;

    snprintf(ble->pending_cmd, sizeof(ble->pending_cmd),
             "VUSION_PROVISION %s\n", ble->connected_mac);
    _start_worker(ble);
}

void esl_ble_vusion_display(
    EslBle* ble,
    uint8_t image_idx,
    EslBleDoneCallback on_done,
    void* ctx)
{
    furi_assert(ble);
    if(ble->state != EslBleStateConnected) {
        if(on_done) on_done(false, "Not connected", ctx);
        return;
    }
    ble->state   = EslBleStateUploading;
    ble->on_done = on_done;
    ble->cb_ctx  = ctx;
    ble->on_progress = NULL;

    snprintf(ble->pending_cmd, sizeof(ble->pending_cmd),
             "VUSION_DISPLAY %s %u\n", ble->connected_mac, (unsigned)image_idx);
    _start_worker(ble);
}

void esl_ble_vusion_ping(EslBle* ble, EslBleDoneCallback on_done, void* ctx) {
    furi_assert(ble);
    if(ble->state != EslBleStateConnected) {
        if(on_done) on_done(false, "Not connected", ctx);
        return;
    }
    ble->state   = EslBleStateUploading;
    ble->on_done = on_done;
    ble->cb_ctx  = ctx;
    ble->on_progress = NULL;

    snprintf(ble->pending_cmd, sizeof(ble->pending_cmd),
             "VUSION_PING %s\n", ble->connected_mac);
    _start_worker(ble);
}

void esl_ble_vusion_reset(EslBle* ble, EslBleDoneCallback on_done, void* ctx) {
    furi_assert(ble);
    if(ble->state != EslBleStateConnected) {
        if(on_done) on_done(false, "Not connected", ctx);
        return;
    }
    ble->state   = EslBleStateUploading;
    ble->on_done = on_done;
    ble->cb_ctx  = ctx;
    ble->on_progress = NULL;

    snprintf(ble->pending_cmd, sizeof(ble->pending_cmd),
             "VUSION_RESET %s\n", ble->connected_mac);
    _start_worker(ble);
}
