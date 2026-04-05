#pragma once

/**
 * ESL UI — Scene-based user interface for the ESL Tool Flipper Zero app.
 */

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <gui/modules/widget.h>
#include <gui/view.h>
#include <notification/notification_messages.h>

#include "esl_ble.h"
#include "esl_protocol.h"

// ── Scene identifiers ─────────────────────────────────────────────────────────

typedef enum {
    EslSceneMain,           // Top-level menu
    EslSceneScanList,       // BLE scan results list
    EslSceneDeviceMenu,     // Per-device action menu
    EslScenePriceEntry,     // Text input for price string
    EslSceneLabelEntry,     // Text input for optional label
    EslSceneUploading,      // Progress screen during upload
    EslSceneResult,         // Success / failure popup
    EslSceneAbout,          // About screen
    EslSceneCount,          // Sentinel — keep last
} EslScene;

// ── View identifiers ──────────────────────────────────────────────────────────

typedef enum {
    EslViewSubmenu,
    EslViewTextInput,
    EslViewPopup,
    EslViewLoading,
    EslViewWidget,
    EslViewCustomScan,     // Custom view for scan list with RSSI
    EslViewCount,
} EslView;

// ── Application state ─────────────────────────────────────────────────────────

#define ESL_PRICE_BUF_LEN   16
#define ESL_LABEL_BUF_LEN   32

typedef struct EslApp {
    // Flipper services
    Gui*                    gui;
    ViewDispatcher*         view_dispatcher;
    SceneManager*           scene_manager;
    NotificationApp*        notifications;

    // UI modules
    Submenu*                submenu;
    TextInput*              text_input;
    Popup*                  popup;
    Loading*                loading;
    Widget*                 widget;
    View*                   scan_view;

    // BLE layer
    EslBle*                 ble;

    // Scan results
    EslDevice               scan_results[ESL_MAX_DEVICES];
    uint8_t                 scan_count;

    // Selected device
    EslDevice               selected_device;

    // User input buffers
    char                    price_buf[ESL_PRICE_BUF_LEN];
    char                    label_buf[ESL_LABEL_BUF_LEN];

    // Image buffer (reused across operations)
    EslImageBuffer          img_buf;

    // Current display model (auto-detected or user-selected)
    EslDisplayModel         display_model;

    // Upload worker thread (kept alive until join)
    FuriThread*             upload_thread;

    // Upload progress (0–100)
    uint8_t                 upload_progress;

    // Last operation result message
    char                    result_msg[64];
    bool                    result_ok;

} EslApp;

// ── Scene handlers ────────────────────────────────────────────────────────────

// Main menu
void esl_scene_main_on_enter(void* ctx);
bool esl_scene_main_on_event(void* ctx, SceneManagerEvent event);
void esl_scene_main_on_exit(void* ctx);

// Scan list
void esl_scene_scan_list_on_enter(void* ctx);
bool esl_scene_scan_list_on_event(void* ctx, SceneManagerEvent event);
void esl_scene_scan_list_on_exit(void* ctx);

// Device menu
void esl_scene_device_menu_on_enter(void* ctx);
bool esl_scene_device_menu_on_event(void* ctx, SceneManagerEvent event);
void esl_scene_device_menu_on_exit(void* ctx);

// Price entry
void esl_scene_price_entry_on_enter(void* ctx);
bool esl_scene_price_entry_on_event(void* ctx, SceneManagerEvent event);
void esl_scene_price_entry_on_exit(void* ctx);

// Label entry
void esl_scene_label_entry_on_enter(void* ctx);
bool esl_scene_label_entry_on_event(void* ctx, SceneManagerEvent event);
void esl_scene_label_entry_on_exit(void* ctx);

// Uploading progress
void esl_scene_uploading_on_enter(void* ctx);
bool esl_scene_uploading_on_event(void* ctx, SceneManagerEvent event);
void esl_scene_uploading_on_exit(void* ctx);

// Result
void esl_scene_result_on_enter(void* ctx);
bool esl_scene_result_on_event(void* ctx, SceneManagerEvent event);
void esl_scene_result_on_exit(void* ctx);

// About
void esl_scene_about_on_enter(void* ctx);
bool esl_scene_about_on_event(void* ctx, SceneManagerEvent event);
void esl_scene_about_on_exit(void* ctx);
