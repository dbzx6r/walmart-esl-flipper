/**
 * ESL Tool — Flipper Zero FAP entry point and scene manager.
 *
 * Controls Hanshow Stellar e-ink price tags (with ATC_TLSR_Paper firmware)
 * via BLE GATT from a Flipper Zero.
 *
 * Compatible firmware:
 *   - Unleashed Firmware (recommended): https://github.com/DarkFlippers/unleashed-firmware
 *   - Momentum Firmware: full BLE Central support
 *   - Official firmware: use companion Python script via USB Serial instead.
 *
 * See docs/FLASHING.md to prepare your tags before using this app.
 */

#include "esl_app.h"
#include "esl_ui.h"
#include "esl_ble.h"
#include "esl_protocol.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <gui/modules/widget.h>
#include <notification/notification.h>
#include <storage/storage.h>

#include <string.h>

// ── Scene manager handlers table ──────────────────────────────────────────────

static const AppSceneOnEnterCallback ESL_SCENE_ON_ENTER[] = {
    [EslSceneMain]        = esl_scene_main_on_enter,
    [EslSceneScanList]    = esl_scene_scan_list_on_enter,
    [EslSceneDeviceMenu]  = esl_scene_device_menu_on_enter,
    [EslSceneVusionMenu]  = esl_scene_vusion_menu_on_enter,
    [EslScenePriceEntry]  = esl_scene_price_entry_on_enter,
    [EslSceneLabelEntry]  = esl_scene_label_entry_on_enter,
    [EslSceneUploading]   = esl_scene_uploading_on_enter,
    [EslSceneResult]      = esl_scene_result_on_enter,
    [EslSceneAbout]       = esl_scene_about_on_enter,
};

static const AppSceneOnEventCallback ESL_SCENE_ON_EVENT[] = {
    [EslSceneMain]        = esl_scene_main_on_event,
    [EslSceneScanList]    = esl_scene_scan_list_on_event,
    [EslSceneDeviceMenu]  = esl_scene_device_menu_on_event,
    [EslSceneVusionMenu]  = esl_scene_vusion_menu_on_event,
    [EslScenePriceEntry]  = esl_scene_price_entry_on_event,
    [EslSceneLabelEntry]  = esl_scene_label_entry_on_event,
    [EslSceneUploading]   = esl_scene_uploading_on_event,
    [EslSceneResult]      = esl_scene_result_on_event,
    [EslSceneAbout]       = esl_scene_about_on_event,
};

static const AppSceneOnExitCallback ESL_SCENE_ON_EXIT[] = {
    [EslSceneMain]        = esl_scene_main_on_exit,
    [EslSceneScanList]    = esl_scene_scan_list_on_exit,
    [EslSceneDeviceMenu]  = esl_scene_device_menu_on_exit,
    [EslSceneVusionMenu]  = esl_scene_vusion_menu_on_exit,
    [EslScenePriceEntry]  = esl_scene_price_entry_on_exit,
    [EslSceneLabelEntry]  = esl_scene_label_entry_on_exit,
    [EslSceneUploading]   = esl_scene_uploading_on_exit,
    [EslSceneResult]      = esl_scene_result_on_exit,
    [EslSceneAbout]       = esl_scene_about_on_exit,
};

static const SceneManagerHandlers ESL_SCENE_HANDLERS = {
    .on_enter_handlers = ESL_SCENE_ON_ENTER,
    .on_event_handlers = ESL_SCENE_ON_EVENT,
    .on_exit_handlers  = ESL_SCENE_ON_EXIT,
    .scene_num         = EslSceneCount,
};

// ── View dispatcher back callback ─────────────────────────────────────────────

static bool _back_event_cb(void* ctx) {
    EslApp* app = (EslApp*)ctx;
    return scene_manager_handle_back_event(app->scene_manager);
}

// ── Custom event handler ──────────────────────────────────────────────────────

static bool _custom_event_cb(void* ctx, uint32_t event) {
    EslApp* app = (EslApp*)ctx;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

// ── App allocation ────────────────────────────────────────────────────────────

static EslApp* esl_app_alloc(void) {
    EslApp* app = malloc(sizeof(EslApp));
    furi_assert(app);
    memset(app, 0, sizeof(EslApp));

    // Allocate Flipper services
    app->gui           = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    // Allocate scene manager
    app->scene_manager = scene_manager_alloc(&ESL_SCENE_HANDLERS, app);

    // Allocate view dispatcher
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, _back_event_cb);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, _custom_event_cb);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Allocate UI modules
    app->submenu    = submenu_alloc();
    app->text_input = text_input_alloc();
    app->popup      = popup_alloc();
    app->loading    = loading_alloc();
    app->widget     = widget_alloc();
    app->scan_view  = view_alloc();
    // Allocate a minimal model so view_commit_model (used to trigger redraws) works.
    // The draw callback reads state from s_scan_ctx directly; this 1-byte model is
    // only needed as the handle for with_view_model(..., true) in the event handler.
    view_allocate_model(app->scan_view, ViewModelTypeLocking, 1);

    // Register views with the dispatcher
    view_dispatcher_add_view(app->view_dispatcher, EslViewSubmenu,
                             submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, EslViewTextInput,
                             text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, EslViewPopup,
                             popup_get_view(app->popup));
    view_dispatcher_add_view(app->view_dispatcher, EslViewLoading,
                             loading_get_view(app->loading));
    view_dispatcher_add_view(app->view_dispatcher, EslViewWidget,
                             widget_get_view(app->widget));
    view_dispatcher_add_view(app->view_dispatcher, EslViewCustomScan,
                             app->scan_view);

    // Allocate BLE layer
    app->ble = esl_ble_alloc();

    return app;
}

// ── App deallocation ──────────────────────────────────────────────────────────

static void esl_app_free(EslApp* app) {
    furi_assert(app);

    // Join and free the upload worker thread if it's still running
    if(app->upload_thread) {
        furi_thread_join(app->upload_thread);
        furi_thread_free(app->upload_thread);
        app->upload_thread = NULL;
    }

    // Free BLE layer
    esl_ble_free(app->ble);

    // Remove and free views
    view_dispatcher_remove_view(app->view_dispatcher, EslViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, EslViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, EslViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, EslViewLoading);
    view_dispatcher_remove_view(app->view_dispatcher, EslViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, EslViewCustomScan);

    submenu_free(app->submenu);
    text_input_free(app->text_input);
    popup_free(app->popup);
    loading_free(app->loading);
    widget_free(app->widget);
    view_free(app->scan_view);

    // Free scene manager and view dispatcher
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    // Close Flipper service records
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

// ── Entry point ───────────────────────────────────────────────────────────────

int32_t esl_app_main(void* p) {
    UNUSED(p);

    EslApp* app = esl_app_alloc();

    // Start at the main menu
    scene_manager_next_scene(app->scene_manager, EslSceneMain);

    // Run the view dispatcher event loop (blocks until the app exits)
    view_dispatcher_run(app->view_dispatcher);

    esl_app_free(app);
    return 0;
}
