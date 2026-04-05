#include "esl_ble.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt.h>

#include <string.h>
#include <stdio.h>

// ── BLE UUID definitions ──────────────────────────────────────────────────────
//
// Service:    13187B10-EBA9-A3BA-044E-83D3217D9A38
// Char:       4B646063-6264-F3A7-8941-E65356EA82FE
//
// UUIDs are stored in little-endian byte order (LSB first) as required by the BLE stack.

static const uint8_t EPD_SERVICE_UUID[16] = {
    0x38, 0x9a, 0x7d, 0x21, 0xd3, 0x83, 0x4e, 0x04,
    0xba, 0xa3, 0xa9, 0xeb, 0x10, 0x7b, 0x18, 0x13
};

static const uint8_t EPD_CHAR_UUID[16] = {
    0xfe, 0x82, 0xea, 0x56, 0x53, 0xe6, 0x41, 0x89,
    0xa7, 0xf3, 0x64, 0x62, 0x63, 0x60, 0x64, 0x4b
};

// ESL BLE device name prefix
#define ESL_BLE_PREFIX "ESL_"
#define ESL_BLE_PREFIX_LEN 4

// Scan timeout default
#define ESL_SCAN_TIMEOUT_MS 10000

// Max BLE packet size (ATT MTU - overhead)
#define ESL_WRITE_MTU 239

// Time to wait after display refresh (ms) — e-ink needs this
#define ESL_DISPLAY_WAIT_MS 4000

// ── Internal state ────────────────────────────────────────────────────────────

struct EslBle {
    EslBleState         state;
    EslDevice           devices[ESL_MAX_DEVICES];
    uint8_t             device_count;
    char                connected_mac[ESL_MAC_STR_LEN];

    // Active operation callbacks
    EslBleScanCallback     on_scan;
    EslBleProgressCallback on_progress;
    EslBleDoneCallback     on_done;
    void*                  cb_ctx;

    // Worker thread
    FuriThread*         worker;
    FuriMutex*          mutex;
    bool                worker_running;

    // Upload state
    const EslImageBuffer* upload_img;
};

// ── Worker thread messages ────────────────────────────────────────────────────

typedef enum {
    EslWorkerMsgScan,
    EslWorkerMsgConnect,
    EslWorkerMsgUpload,
    EslWorkerMsgCommand,
    EslWorkerMsgStop,
} EslWorkerMsgType;

typedef struct {
    EslWorkerMsgType type;
    char             mac[ESL_MAC_STR_LEN];
    uint8_t          cmd_buf[ESL_WRITE_MTU + 1];
    uint8_t          cmd_len;
    uint32_t         scan_timeout_ms;
} EslWorkerMsg;

// ── Allocation ────────────────────────────────────────────────────────────────

EslBle* esl_ble_alloc(void) {
    EslBle* ble = malloc(sizeof(EslBle));
    furi_assert(ble);
    memset(ble, 0, sizeof(EslBle));
    ble->state = EslBleStateIdle;
    ble->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    return ble;
}

void esl_ble_free(EslBle* ble) {
    furi_assert(ble);
    if(ble->state == EslBleStateConnected) {
        esl_ble_disconnect(ble);
    }
    if(ble->worker) {
        ble->worker_running = false;
        furi_thread_join(ble->worker);
        furi_thread_free(ble->worker);
        ble->worker = NULL;
    }
    furi_mutex_free(ble->mutex);
    free(ble);
}

EslBleState esl_ble_get_state(EslBle* ble) {
    furi_assert(ble);
    return ble->state;
}

uint8_t esl_ble_get_scan_results(EslBle* ble, EslDevice out_devices[ESL_MAX_DEVICES]) {
    furi_assert(ble);
    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    uint8_t cnt = ble->device_count;
    for(uint8_t i = 0; i < cnt; i++) {
        out_devices[i] = ble->devices[i];
    }
    furi_mutex_release(ble->mutex);
    return cnt;
}

// ── Internal helpers ──────────────────────────────────────────────────────────

static bool _starts_with(const char* s, const char* prefix) {
    while(*prefix) {
        if(*s++ != *prefix++) return false;
    }
    return true;
}

// Format 6 raw MAC bytes (LSB first as returned by BLE stack) to "AA:BB:CC:DD:EE:FF"
static void _mac_bytes_to_str(const uint8_t mac_bytes[6], char out[ESL_MAC_STR_LEN]) {
    snprintf(out, ESL_MAC_STR_LEN,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac_bytes[5], mac_bytes[4], mac_bytes[3],
        mac_bytes[2], mac_bytes[1], mac_bytes[0]);
}

// ── BLE scan / connect / upload ───────────────────────────────────────────────
//
// The Flipper Zero's official firmware exposes BLE scanning via the bt service
// (furi_hal_bt). The implementation below uses the gap_* APIs from the
// STM32WB55 BLE stack, which are available in community firmware.
//
// If direct BLE Central is not available on your firmware build, use the
// companion Python script via USB Serial (see esl_ui.c serial mode).

// BLE GAP scan callback — called from BLE stack for each advertisement seen
// NOTE: community firmware passes GapEvent by value, not pointer.
static void _ble_gap_scan_cb(GapEvent event, void* ctx) {
    EslBle* ble = (EslBle*)ctx;
    if(!ble || event.type != GapEventTypeDeviceFound) return;

    const GapDeviceFound* dev = &event.data.device_found;

    // Filter: must start with "ESL_"
    if(!dev->local_name[0] || !_starts_with(dev->local_name, ESL_BLE_PREFIX)) return;

    furi_mutex_acquire(ble->mutex, FuriWaitForever);

    // Check if already in list
    for(uint8_t i = 0; i < ble->device_count; i++) {
        if(strncmp(ble->devices[i].name, dev->local_name, ESL_DEVICE_NAME_LEN - 1) == 0) {
            ble->devices[i].rssi = dev->rssi;
            furi_mutex_release(ble->mutex);
            return;
        }
    }

    // Add new device
    if(ble->device_count < ESL_MAX_DEVICES) {
        EslDevice* d = &ble->devices[ble->device_count++];
        strncpy(d->name, dev->local_name, ESL_DEVICE_NAME_LEN - 1);
        d->name[ESL_DEVICE_NAME_LEN - 1] = '\0';
        _mac_bytes_to_str(dev->addr, d->mac);
        d->rssi  = dev->rssi;
        d->valid = true;

        if(ble->on_scan) {
            ble->on_scan(ble->devices, ble->device_count, ble->cb_ctx);
        }
    }

    furi_mutex_release(ble->mutex);
}

void esl_ble_start_scan(
    EslBle* ble,
    uint32_t timeout_ms,
    EslBleScanCallback on_scan,
    EslBleDoneCallback on_done,
    void* ctx)
{
    furi_assert(ble);
    if(ble->state != EslBleStateIdle) return;

    ble->state      = EslBleStateScanning;
    ble->device_count = 0;
    ble->on_scan    = on_scan;
    ble->on_done    = on_done;
    ble->cb_ctx     = ctx;

    memset(ble->devices, 0, sizeof(ble->devices));

    // Acquire the bt service and start GAP scan
    Bt* bt = furi_record_open(RECORD_BT);
    UNUSED(bt);

    // Start BLE scanning via the HAL (community firmware API)
    furi_hal_bt_start_scan(_ble_gap_scan_cb, ble);

    // Schedule a timer to stop scanning after timeout
    if(timeout_ms > 0) {
        furi_delay_ms(timeout_ms);
        esl_ble_stop_scan(ble);
    }

    furi_record_close(RECORD_BT);
}

void esl_ble_stop_scan(EslBle* ble) {
    furi_assert(ble);
    if(ble->state != EslBleStateScanning) return;

    furi_hal_bt_stop_scan();
    ble->state = EslBleStateIdle;

    if(ble->on_done) {
        ble->on_done(true, NULL, ble->cb_ctx);
        ble->on_done = NULL;
    }
}

// ── GATT write helper ─────────────────────────────────────────────────────────

static bool _gatt_write(EslBle* ble, const uint8_t* data, uint16_t len) {
    // Write to the EPD characteristic via the BLE GATT client
    // furi_hal_bt_gatt_client_write is available in community firmware builds
    // that expose the full STM32WB55 BLE stack GATT client API.
    int ret = furi_hal_bt_gatt_client_write(EPD_CHAR_UUID, data, len);
    return (ret == 0);
}

// ── Connect ───────────────────────────────────────────────────────────────────

void esl_ble_connect(EslBle* ble, const char* mac, EslBleDoneCallback on_done, void* ctx) {
    furi_assert(ble);
    furi_assert(mac);

    if(ble->state != EslBleStateIdle) {
        if(on_done) on_done(false, "BLE busy", ctx);
        return;
    }

    ble->state   = EslBleStateConnecting;
    ble->on_done = on_done;
    ble->cb_ctx  = ctx;
    strncpy(ble->connected_mac, mac, ESL_MAC_STR_LEN - 1);

    // Use the HAL to initiate connection
    bool ok = furi_hal_bt_connect(mac);
    if(!ok) {
        ble->state = EslBleStateError;
        if(on_done) on_done(false, "Connect failed", ctx);
        return;
    }

    ble->state = EslBleStateConnected;
    if(on_done) {
        on_done(true, NULL, ctx);
        ble->on_done = NULL;
    }
}

void esl_ble_disconnect(EslBle* ble) {
    furi_assert(ble);
    if(ble->state == EslBleStateConnected || ble->state == EslBleStateUploading) {
        furi_hal_bt_disconnect();
        ble->state = EslBleStateIdle;
        ble->connected_mac[0] = '\0';
    }
}

// ── Image upload ──────────────────────────────────────────────────────────────

void esl_ble_upload_image(
    EslBle* ble,
    const EslImageBuffer* img,
    EslBleProgressCallback on_progress,
    EslBleDoneCallback on_done,
    void* ctx)
{
    furi_assert(ble);
    furi_assert(img);

    if(ble->state != EslBleStateConnected) {
        if(on_done) on_done(false, "Not connected", ctx);
        return;
    }

    ble->state       = EslBleStateUploading;
    ble->on_progress = on_progress;
    ble->on_done     = on_done;
    ble->cb_ctx      = ctx;

    uint8_t cmd_buf[ESL_WRITE_MTU + 1];
    uint8_t cmd_len;
    bool ok = true;

    // Step 1: Clear to white
    cmd_len = esl_cmd_clear(cmd_buf, 0xFF);
    ok = _gatt_write(ble, cmd_buf, cmd_len);
    if(!ok) goto upload_fail;

    // Step 2: Set write position to 0
    cmd_len = esl_cmd_set_pos(cmd_buf, 0);
    ok = _gatt_write(ble, cmd_buf, cmd_len);
    if(!ok) goto upload_fail;

    // Step 3: Write image data in chunks
    uint32_t sent = 0;
    uint32_t total = img->size;

    while(sent < total) {
        uint32_t remaining = total - sent;
        uint8_t  chunk_sz  = (remaining > ESL_WRITE_MTU) ? ESL_WRITE_MTU : (uint8_t)remaining;

        cmd_len = esl_cmd_write(cmd_buf, img->data + sent, chunk_sz);
        ok = _gatt_write(ble, cmd_buf, cmd_len);
        if(!ok) goto upload_fail;

        sent += chunk_sz;

        if(on_progress) {
            uint8_t pct = (uint8_t)((uint64_t)sent * 100 / total);
            on_progress(pct, ctx);
        }
    }

    // Step 4: Trigger display refresh
    cmd_len = esl_cmd_display(cmd_buf);
    ok = _gatt_write(ble, cmd_buf, cmd_len);
    if(!ok) goto upload_fail;

    // Wait for e-ink display to refresh
    furi_delay_ms(ESL_DISPLAY_WAIT_MS);

    ble->state = EslBleStateConnected;
    if(on_done) on_done(true, "Upload complete", ctx);
    return;

upload_fail:
    ble->state = EslBleStateError;
    if(on_done) on_done(false, "Write failed — check connection", ctx);
}

void esl_ble_send_command(
    EslBle* ble,
    const uint8_t* cmd_buf,
    uint8_t cmd_len,
    EslBleDoneCallback on_done,
    void* ctx)
{
    furi_assert(ble);
    if(ble->state != EslBleStateConnected) {
        if(on_done) on_done(false, "Not connected", ctx);
        return;
    }
    bool ok = _gatt_write(ble, cmd_buf, cmd_len);
    if(on_done) on_done(ok, ok ? NULL : "Command write failed", ctx);
}
