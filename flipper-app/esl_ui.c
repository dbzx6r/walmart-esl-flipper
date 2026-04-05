#include "esl_ui.h"
#include "esl_app.h"

#include <furi.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <gui/canvas.h>
#include <notification/notification_messages.h>

#include <string.h>
#include <stdio.h>

// ── Forward declarations ──────────────────────────────────────────────────────

static void _submenu_cb(void* ctx, uint32_t idx);
static void _device_submenu_cb(void* ctx, uint32_t idx);
static void _vusion_submenu_cb(void* ctx, uint32_t idx);
static void _result_popup_cb(void* ctx);
static void _on_scan_update(const EslDevice* devices, uint8_t count, void* ctx);
static void _on_scan_done(bool success, const char* msg, void* ctx);
static void _upload_done_cb(bool success, const char* msg, void* ctx);

// ── Custom events ─────────────────────────────────────────────────────────────

typedef enum {
    EslCustomEventScanDone      = 100,
    EslCustomEventDeviceSelected,
    EslCustomEventPriceEntered,
    EslCustomEventLabelEntered,
    EslCustomEventUploadDone,
    EslCustomEventUploadFail,
    EslCustomEventScanRedraw,
    EslCustomEventBack,
} EslCustomEvent;

// ── Main Menu ─────────────────────────────────────────────────────────────────

typedef enum {
    EslMainMenuScan = 0,
    EslMainMenuAbout,
} EslMainMenuIdx;

void esl_scene_main_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "ESL Tool");
    submenu_add_item(app->submenu, "Scan for ESL Tags", EslMainMenuScan, _submenu_cb, app);
    submenu_add_item(app->submenu, "About", EslMainMenuAbout, _submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewSubmenu);
}

static void _submenu_cb(void* ctx, uint32_t idx) {
    EslApp* app = (EslApp*)ctx;
    scene_manager_handle_custom_event(app->scene_manager, idx);
}

bool esl_scene_main_on_event(void* ctx, SceneManagerEvent event) {
    EslApp* app = (EslApp*)ctx;
    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case EslMainMenuScan:
            scene_manager_next_scene(app->scene_manager, EslSceneScanList);
            return true;
        case EslMainMenuAbout:
            scene_manager_next_scene(app->scene_manager, EslSceneAbout);
            return true;
        }
    }
    return false;
}

void esl_scene_main_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    submenu_reset(app->submenu);
}

// ── Scan List ─────────────────────────────────────────────────────────────────
//
// Custom view that draws a scrollable list of discovered ESL devices.
// Up/Down keys move the cursor; OK selects and advances to the device menu.

typedef struct {
    EslApp*  app;
    bool     scanning;
} ScanViewCtx;

static ScanViewCtx s_scan_ctx;

// Maximum devices visible at once in the scan list
#define SCAN_VISIBLE_ROWS 5

static void _scan_view_draw(Canvas* canvas, void* ctx) {
    EslApp* app     = ((ScanViewCtx*)ctx)->app;
    bool scanning   = ((ScanViewCtx*)ctx)->scanning;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, scanning ? "Scanning..." : "ESL Devices");

    canvas_set_font(canvas, FontSecondary);

    if(app->scan_count == 0) {
        canvas_draw_str(canvas, 4, 28, scanning ? "Searching for tags..." : "No devices found.");
        if(!scanning) canvas_draw_str(canvas, 4, 40, "Connect ESP32 to GPIO.");
        return;
    }

    uint8_t cursor = app->scan_cursor;
    if(cursor >= app->scan_count) cursor = app->scan_count - 1;

    // Scroll so the cursor is always visible
    uint8_t start = 0;
    if(cursor >= SCAN_VISIBLE_ROWS) start = cursor - SCAN_VISIBLE_ROWS + 1;

    for(uint8_t i = 0; i < SCAN_VISIBLE_ROWS && (start + i) < app->scan_count; i++) {
        uint8_t idx = start + i;
        const EslDevice* dev = &app->scan_results[idx];
        char line[40];
        snprintf(line, sizeof(line), "%s %ddBm",
                 dev->name[0] ? dev->name : dev->mac,
                 dev->rssi);
        uint8_t y = 20 + i * 10;
        if(idx == cursor) {
            canvas_draw_box(canvas, 0, y - 8, 128, 9);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 2, y, line);
        if(idx == cursor) canvas_set_color(canvas, ColorBlack);
    }

    canvas_set_font(canvas, FontSecondary);
    if(!scanning) canvas_draw_str(canvas, 2, 63, "[OK]=Select [Back]=Exit");
}

static bool _scan_view_input(InputEvent* evt, void* ctx) {
    EslApp* app = ((ScanViewCtx*)ctx)->app;

    if(evt->type == InputTypeShort || evt->type == InputTypeRepeat) {
        if(evt->key == InputKeyUp && app->scan_count > 0) {
            if(app->scan_cursor > 0) app->scan_cursor--;
            view_dispatcher_send_custom_event(app->view_dispatcher, EslCustomEventScanRedraw);
            return true;
        }
        if(evt->key == InputKeyDown && app->scan_count > 0) {
            if(app->scan_cursor < app->scan_count - 1) app->scan_cursor++;
            view_dispatcher_send_custom_event(app->view_dispatcher, EslCustomEventScanRedraw);
            return true;
        }
    }
    if(evt->type == InputTypeShort) {
        if(evt->key == InputKeyOk && app->scan_count > 0) {
            uint8_t cursor = app->scan_cursor;
            if(cursor >= app->scan_count) cursor = 0;
            app->selected_device = app->scan_results[cursor];
            scene_manager_handle_custom_event(
                app->scene_manager, EslCustomEventDeviceSelected);
            return true;
        }
        if(evt->key == InputKeyBack) {
            esl_ble_stop_scan(app->ble);
            scene_manager_previous_scene(app->scene_manager);
            return true;
        }
    }
    return false;
}

static void _on_scan_update(const EslDevice* devices, uint8_t count, void* ctx) {
    EslApp* app = (EslApp*)ctx;
    for(uint8_t i = 0; i < count && i < ESL_MAX_DEVICES; i++) {
        app->scan_results[i] = devices[i];
    }
    app->scan_count = count;
    view_dispatcher_send_custom_event(app->view_dispatcher, EslCustomEventScanRedraw);
}

static void _on_scan_done(bool success, const char* msg, void* ctx) {
    UNUSED(success);
    UNUSED(msg);
    EslApp* app = (EslApp*)ctx;
    s_scan_ctx.scanning = false;
    view_dispatcher_send_custom_event(app->view_dispatcher, EslCustomEventScanDone);
}

void esl_scene_scan_list_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;

    app->scan_count  = 0;
    app->scan_cursor = 0;
    memset(app->scan_results, 0, sizeof(app->scan_results));

    s_scan_ctx.app      = app;
    s_scan_ctx.scanning = true;

    view_set_draw_callback(app->scan_view, _scan_view_draw);
    view_set_input_callback(app->scan_view, _scan_view_input);
    view_set_context(app->scan_view, &s_scan_ctx);
    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewCustomScan);

    esl_ble_start_scan(app->ble, 10000, _on_scan_update, _on_scan_done, app);
}

bool esl_scene_scan_list_on_event(void* ctx, SceneManagerEvent event) {
    EslApp* app = (EslApp*)ctx;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == EslCustomEventDeviceSelected) {
            // Route to correct menu based on tag type
            if(app->selected_device.tag_type == EslTagTypeVusion) {
                scene_manager_next_scene(app->scene_manager, EslSceneVusionMenu);
            } else {
                scene_manager_next_scene(app->scene_manager, EslSceneDeviceMenu);
            }
            return true;
        }
        if(event.event == EslCustomEventScanDone || event.event == EslCustomEventScanRedraw) {
            view_dispatcher_switch_to_view(app->view_dispatcher, EslViewCustomScan);
            return true;
        }
    }
    return false;
}

void esl_scene_scan_list_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    esl_ble_stop_scan(app->ble);
}

// ── ATC Device Menu ───────────────────────────────────────────────────────────

typedef enum {
    EslDeviceMenuPrice = 0,
    EslDeviceMenuClear,
    EslDeviceMenuModelBW,
    EslDeviceMenuModelBWR,
} EslDeviceMenuIdx;

static void _device_submenu_cb(void* ctx, uint32_t idx) {
    EslApp* app = (EslApp*)ctx;
    scene_manager_handle_custom_event(app->scene_manager, idx);
}

void esl_scene_device_menu_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, app->selected_device.name[0]
                                     ? app->selected_device.name
                                     : app->selected_device.mac);
    submenu_add_item(app->submenu, "Set Price / Text",  EslDeviceMenuPrice, _device_submenu_cb, app);
    submenu_add_item(app->submenu, "Clear Display",     EslDeviceMenuClear, _device_submenu_cb, app);
    submenu_add_item(app->submenu, "Model: BW 2.13\"",  EslDeviceMenuModelBW,  _device_submenu_cb, app);
    submenu_add_item(app->submenu, "Model: BWR 2.13\"", EslDeviceMenuModelBWR, _device_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewSubmenu);

    // Store MAC for UART bridge
    esl_ble_connect(app->ble, app->selected_device.mac,
                    app->selected_device.tag_type, NULL, NULL);
}

bool esl_scene_device_menu_on_event(void* ctx, SceneManagerEvent event) {
    EslApp* app = (EslApp*)ctx;
    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case EslDeviceMenuPrice:
            scene_manager_next_scene(app->scene_manager, EslScenePriceEntry);
            return true;
        case EslDeviceMenuClear:
            scene_manager_next_scene(app->scene_manager, EslSceneUploading);
            return true;
        case EslDeviceMenuModelBW:
            app->display_model = EslModelBW213;
            return true;
        case EslDeviceMenuModelBWR:
            app->display_model = EslModelBWR213;
            return true;
        }
    }
    return false;
}

void esl_scene_device_menu_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    submenu_reset(app->submenu);
}

// ── Vusion Device Menu ────────────────────────────────────────────────────────

typedef enum {
    EslVusionMenuProvision = 0,
    EslVusionMenuDisplayPrev,
    EslVusionMenuDisplayNext,
    EslVusionMenuPing,
    EslVusionMenuReset,
} EslVusionMenuIdx;

static void _vusion_submenu_cb(void* ctx, uint32_t idx) {
    EslApp* app = (EslApp*)ctx;
    scene_manager_handle_custom_event(app->scene_manager, idx);
}

void esl_scene_vusion_menu_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    submenu_reset(app->submenu);

    char header[40];
    snprintf(header, sizeof(header), "%s [Vusion]",
             app->selected_device.name[0]
                 ? app->selected_device.name
                 : app->selected_device.mac);
    submenu_set_header(app->submenu, header);

    submenu_add_item(app->submenu, "Provision Tag",          EslVusionMenuProvision,    _vusion_submenu_cb, app);

    char img_prev[24], img_next[24];
    snprintf(img_prev, sizeof(img_prev), "< Image Slot %u", app->vusion_image_idx);
    snprintf(img_next, sizeof(img_next), "> Image Slot %u", app->vusion_image_idx);
    submenu_add_item(app->submenu, img_prev,                 EslVusionMenuDisplayPrev,  _vusion_submenu_cb, app);
    submenu_add_item(app->submenu, img_next,                 EslVusionMenuDisplayNext,  _vusion_submenu_cb, app);
    submenu_add_item(app->submenu, "Ping",                   EslVusionMenuPing,         _vusion_submenu_cb, app);
    submenu_add_item(app->submenu, "Factory Reset",          EslVusionMenuReset,        _vusion_submenu_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewSubmenu);

    esl_ble_connect(app->ble, app->selected_device.mac,
                    app->selected_device.tag_type, NULL, NULL);
}

bool esl_scene_vusion_menu_on_event(void* ctx, SceneManagerEvent event) {
    EslApp* app = (EslApp*)ctx;
    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case EslVusionMenuProvision:
            strncpy(app->result_msg, "PROVISION", sizeof(app->result_msg) - 1);
            scene_manager_next_scene(app->scene_manager, EslSceneUploading);
            return true;
        case EslVusionMenuDisplayPrev:
            if(app->vusion_image_idx > 0) app->vusion_image_idx--;
            strncpy(app->result_msg, "DISPLAY", sizeof(app->result_msg) - 1);
            scene_manager_next_scene(app->scene_manager, EslSceneUploading);
            return true;
        case EslVusionMenuDisplayNext:
            if(app->vusion_image_idx < 15) app->vusion_image_idx++;
            strncpy(app->result_msg, "DISPLAY", sizeof(app->result_msg) - 1);
            scene_manager_next_scene(app->scene_manager, EslSceneUploading);
            return true;
        case EslVusionMenuPing:
            strncpy(app->result_msg, "PING", sizeof(app->result_msg) - 1);
            scene_manager_next_scene(app->scene_manager, EslSceneUploading);
            return true;
        case EslVusionMenuReset:
            strncpy(app->result_msg, "RESET", sizeof(app->result_msg) - 1);
            scene_manager_next_scene(app->scene_manager, EslSceneUploading);
            return true;
        }
    }
    return false;
}

void esl_scene_vusion_menu_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    submenu_reset(app->submenu);
}

// ── Price Entry ───────────────────────────────────────────────────────────────

static void _price_text_done_cb(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    scene_manager_handle_custom_event(app->scene_manager, EslCustomEventPriceEntered);
}

void esl_scene_price_entry_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    app->price_buf[0] = '\0';
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Enter price (e.g. $12.99)");
    text_input_set_result_callback(
        app->text_input,
        _price_text_done_cb,
        app,
        app->price_buf,
        ESL_PRICE_BUF_LEN,
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewTextInput);
}

bool esl_scene_price_entry_on_event(void* ctx, SceneManagerEvent event) {
    EslApp* app = (EslApp*)ctx;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == EslCustomEventPriceEntered) {
        scene_manager_next_scene(app->scene_manager, EslSceneLabelEntry);
        return true;
    }
    return false;
}

void esl_scene_price_entry_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    text_input_reset(app->text_input);
}

// ── Label Entry ───────────────────────────────────────────────────────────────

static void _label_text_done_cb(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    scene_manager_handle_custom_event(app->scene_manager, EslCustomEventLabelEntered);
}

void esl_scene_label_entry_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    app->label_buf[0] = '\0';
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Label (optional, OK=skip)");
    text_input_set_result_callback(
        app->text_input,
        _label_text_done_cb,
        app,
        app->label_buf,
        ESL_LABEL_BUF_LEN,
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewTextInput);
}

bool esl_scene_label_entry_on_event(void* ctx, SceneManagerEvent event) {
    EslApp* app = (EslApp*)ctx;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == EslCustomEventLabelEntered) {
        scene_manager_next_scene(app->scene_manager, EslSceneUploading);
        return true;
    }
    return false;
}

void esl_scene_label_entry_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    text_input_reset(app->text_input);
}

// ── Uploading Scene ───────────────────────────────────────────────────────────

static void _upload_progress_cb(uint8_t pct, void* ctx) {
    EslApp* app = (EslApp*)ctx;
    app->upload_progress = pct;
}

static void _upload_done_cb(bool success, const char* msg, void* ctx) {
    EslApp* app = (EslApp*)ctx;
    app->result_ok = success;
    if(msg && msg[0]) {
        strncpy(app->result_msg, msg, sizeof(app->result_msg) - 1);
    } else {
        strncpy(app->result_msg, success ? "Done!" : "Failed.",
                sizeof(app->result_msg) - 1);
    }
    view_dispatcher_send_custom_event(app->view_dispatcher,
        success ? EslCustomEventUploadDone : EslCustomEventUploadFail);
}

// Worker thread entry for the uploading scene
static int32_t _upload_worker(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    EslTagType type = app->selected_device.tag_type;

    if(type == EslTagTypeVusion) {
        // Determine Vusion action from which menu item triggered the scene.
        // We use vusion_image_idx == 0xFF as sentinel for non-display actions.
        // The Vusion menu sets app->result_msg to a sentinel before entering upload.
        // Simple approach: check result_msg prefix set by vusion menu handler.
        const char* action = app->result_msg;
        if(strncmp(action, "PROVISION", 9) == 0) {
            esl_ble_vusion_provision(app->ble, _upload_done_cb, app);
        } else if(strncmp(action, "PING", 4) == 0) {
            esl_ble_vusion_ping(app->ble, _upload_done_cb, app);
        } else if(strncmp(action, "RESET", 5) == 0) {
            esl_ble_vusion_reset(app->ble, _upload_done_cb, app);
        } else {
            // Default: display image slot
            esl_ble_vusion_display(app->ble, app->vusion_image_idx, _upload_done_cb, app);
        }
    } else {
        // ATC tag
        if(app->price_buf[0] != '\0') {
            esl_ble_upload_image(app->ble, app->price_buf, app->label_buf,
                                 _upload_progress_cb, _upload_done_cb, app);
        } else {
            esl_ble_clear_display(app->ble, _upload_done_cb, app);
        }
    }
    return 0;
}

void esl_scene_uploading_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    app->upload_progress = 0;

    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewLoading);

    // Free any previous upload thread
    if(app->upload_thread) {
        furi_thread_join(app->upload_thread);
        furi_thread_free(app->upload_thread);
        app->upload_thread = NULL;
    }

    app->upload_thread = furi_thread_alloc_ex("esl_upload", 2048, _upload_worker, app);
    furi_thread_set_priority(app->upload_thread, FuriThreadPriorityNormal);
    furi_thread_start(app->upload_thread);
}

bool esl_scene_uploading_on_event(void* ctx, SceneManagerEvent event) {
    EslApp* app = (EslApp*)ctx;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == EslCustomEventUploadDone ||
           event.event == EslCustomEventUploadFail) {
            scene_manager_next_scene(app->scene_manager, EslSceneResult);
            return true;
        }
    }
    return false;
}

void esl_scene_uploading_on_exit(void* ctx) {
    UNUSED(ctx);
}

// ── Result Scene ──────────────────────────────────────────────────────────────

void esl_scene_result_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    popup_reset(app->popup);
    popup_set_header(app->popup,
                     app->result_ok ? "Success!" : "Error",
                     64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, app->result_msg, 64, 32, AlignCenter, AlignCenter);
    popup_set_timeout(app->popup, 3000);
    popup_enable_timeout(app->popup);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, _result_popup_cb);

    if(app->result_ok) {
        notification_message(app->notifications, &sequence_success);
    } else {
        notification_message(app->notifications, &sequence_error);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewPopup);
}

static void _result_popup_cb(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    scene_manager_previous_scene(app->scene_manager);
}

bool esl_scene_result_on_event(void* ctx, SceneManagerEvent event) {
    UNUSED(ctx);
    UNUSED(event);
    return false;
}

void esl_scene_result_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    popup_reset(app->popup);
}

// ── About Scene ───────────────────────────────────────────────────────────────

void esl_scene_about_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    widget_reset(app->widget);
    widget_add_string_element(app->widget, 64, 4,  AlignCenter, AlignTop, FontPrimary,    "ESL Tool v1.1");
    widget_add_string_element(app->widget, 64, 16, AlignCenter, AlignTop, FontSecondary,  "Hanshow (ATC) + Vusion");
    widget_add_string_element(app->widget, 64, 26, AlignCenter, AlignTop, FontSecondary,  "e-ink tag controller");
    widget_add_string_element(app->widget, 64, 36, AlignCenter, AlignTop, FontSecondary,  "Requires ESP32 on GPIO");
    widget_add_string_element(app->widget, 64, 46, AlignCenter, AlignTop, FontSecondary,  "C1/C0 (see README)");
    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewWidget);
}

bool esl_scene_about_on_event(void* ctx, SceneManagerEvent event) {
    UNUSED(ctx);
    UNUSED(event);
    return false;
}

void esl_scene_about_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    widget_reset(app->widget);
}
