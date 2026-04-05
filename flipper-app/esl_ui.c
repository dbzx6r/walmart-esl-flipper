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
static void _result_popup_cb(void* ctx);
static void _on_scan_update(const EslDevice* devices, uint8_t count, void* ctx);
static void _on_scan_done(bool success, const char* msg, void* ctx);

// ── Custom events ─────────────────────────────────────────────────────────────

typedef enum {
    EslCustomEventScanDone      = 100,
    EslCustomEventDeviceSelected,
    EslCustomEventPriceEntered,
    EslCustomEventLabelEntered,
    EslCustomEventUploadDone,
    EslCustomEventUploadFail,
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
    submenu_add_item(app->submenu, "Scan for ESL Tags", EslMainMenuScan,
                     _submenu_cb, app);
    submenu_add_item(app->submenu, "About", EslMainMenuAbout,
                     _submenu_cb, app);
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
// Uses a custom view that draws a list of discovered devices with RSSI bars,
// refreshing every time a new device is found.

typedef struct {
    EslApp*  app;
    bool     scanning;
} ScanViewCtx;

static ScanViewCtx s_scan_ctx;

// Draw callback for the custom scan view
static void _scan_view_draw(Canvas* canvas, void* ctx) {
    EslApp* app = ((ScanViewCtx*)ctx)->app;
    bool scanning = ((ScanViewCtx*)ctx)->scanning;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, scanning ? "Scanning..." : "ESL Devices");

    canvas_set_font(canvas, FontSecondary);

    if(app->scan_count == 0) {
        canvas_draw_str(canvas, 4, 30, "No devices found yet.");
        canvas_draw_str(canvas, 4, 42, "Make sure tag is powered.");
        return;
    }

    for(uint8_t i = 0; i < app->scan_count && i < 5; i++) {
        char line[40];
        snprintf(line, sizeof(line), "%s  %ddBm",
                 app->scan_results[i].name,
                 app->scan_results[i].rssi);
        canvas_draw_str(canvas, 4, 22 + i * 10, line);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 62, "[OK]=Select  [Back]=Exit");
}

static bool _scan_view_input(InputEvent* evt, void* ctx) {
    EslApp* app = ((ScanViewCtx*)ctx)->app;

    if(evt->type == InputTypeShort) {
        if(evt->key == InputKeyOk && app->scan_count > 0) {
            // Select the first device — a real implementation would track cursor
            app->selected_device = app->scan_results[0];
            scene_manager_handle_custom_event(app->scene_manager, EslCustomEventDeviceSelected);
            return true;
        }
        if(evt->key == InputKeyBack) {
            esl_ble_stop_scan(app->ble);
            scene_manager_previous_scene(app->scene_manager);
            return true;
        }
        if(evt->key == InputKeyUp || evt->key == InputKeyDown) {
            // Cursor movement could be implemented here for multi-device selection
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
    // Trigger a redraw of the custom view
    view_dispatcher_send_custom_event(app->view_dispatcher, 0);
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

    app->scan_count = 0;
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
            scene_manager_next_scene(app->scene_manager, EslSceneDeviceMenu);
            return true;
        }
        // Redraw on scan update
        view_dispatcher_switch_to_view(app->view_dispatcher, EslViewCustomScan);
        return true;
    }
    return false;
}

void esl_scene_scan_list_on_exit(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    esl_ble_stop_scan(app->ble);
}

// ── Device Menu ───────────────────────────────────────────────────────────────

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
    submenu_set_header(app->submenu, app->selected_device.name);
    submenu_add_item(app->submenu, "Set Price / Text", EslDeviceMenuPrice,
                     _device_submenu_cb, app);
    submenu_add_item(app->submenu, "Clear Display",   EslDeviceMenuClear,
                     _device_submenu_cb, app);
    submenu_add_item(app->submenu, "Model: BW 2.13\"", EslDeviceMenuModelBW,
                     _device_submenu_cb, app);
    submenu_add_item(app->submenu, "Model: BWR 2.13\"", EslDeviceMenuModelBWR,
                     _device_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewSubmenu);
}

bool esl_scene_device_menu_on_event(void* ctx, SceneManagerEvent event) {
    EslApp* app = (EslApp*)ctx;
    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case EslDeviceMenuPrice:
            scene_manager_next_scene(app->scene_manager, EslScenePriceEntry);
            return true;
        case EslDeviceMenuClear:
            // Connect and clear the display
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
        // Go to optional label entry
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
    text_input_set_header_text(app->text_input, "Label (optional, Enter=skip)");
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
        // All inputs gathered — go to upload scene
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
    if(msg) {
        strncpy(app->result_msg, msg, sizeof(app->result_msg) - 1);
    } else {
        strncpy(app->result_msg, success ? "Done!" : "Upload failed.", sizeof(app->result_msg) - 1);
    }
    view_dispatcher_send_custom_event(app->view_dispatcher,
        success ? EslCustomEventUploadDone : EslCustomEventUploadFail);
}

static void _connect_and_upload(EslApp* app) {
    // Connect to selected device
    esl_ble_connect(app->ble, app->selected_device.mac,
        NULL, NULL);  // Blocking-style — we're in a worker thread

    if(esl_ble_get_state(app->ble) != EslBleStateConnected) {
        app->result_ok = false;
        strncpy(app->result_msg, "Connection failed.", sizeof(app->result_msg) - 1);
        view_dispatcher_send_custom_event(app->view_dispatcher, EslCustomEventUploadFail);
        return;
    }

    // Render the price tag image if a price was entered
    if(app->price_buf[0] != '\0') {
        esl_render_price(&app->img_buf, app->price_buf, app->label_buf);
    }
    // (else img_buf should have been set up by a previous operation)

    esl_ble_upload_image(app->ble, &app->img_buf,
                         _upload_progress_cb, _upload_done_cb, app);
}

// Worker thread entry for upload
static int32_t _upload_worker(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    _connect_and_upload(app);
    return 0;
}

void esl_scene_uploading_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    app->upload_progress = 0;

    loading_set_header(app->loading, "Uploading to tag...");
    view_dispatcher_switch_to_view(app->view_dispatcher, EslViewLoading);

    // Initialise image buffer for the selected display model
    esl_buffer_init(&app->img_buf, app->display_model);

    // Free any previous upload thread
    if(app->upload_thread) {
        furi_thread_join(app->upload_thread);
        furi_thread_free(app->upload_thread);
        app->upload_thread = NULL;
    }

    // Launch upload in a worker thread so the UI stays responsive
    app->upload_thread = furi_thread_alloc_ex("esl_upload", 4096, _upload_worker, app);
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
    // Thread cleanup is done in on_enter (before starting new thread)
    // and in esl_app_free (final cleanup). Nothing needed here.
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
    esl_ble_disconnect(app->ble);
}

// ── About Scene ───────────────────────────────────────────────────────────────

void esl_scene_about_on_enter(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    widget_reset(app->widget);
    widget_add_string_element(app->widget, 64, 4,  AlignCenter, AlignTop, FontPrimary, "ESL Tool v1.0");
    widget_add_string_element(app->widget, 64, 18, AlignCenter, AlignTop, FontSecondary, "Hanshow Stellar e-ink");
    widget_add_string_element(app->widget, 64, 28, AlignCenter, AlignTop, FontSecondary, "price tag controller");
    widget_add_string_element(app->widget, 64, 40, AlignCenter, AlignTop, FontSecondary, "Requires ATC_TLSR_Paper");
    widget_add_string_element(app->widget, 64, 50, AlignCenter, AlignTop, FontSecondary, "firmware on your tags.");
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
