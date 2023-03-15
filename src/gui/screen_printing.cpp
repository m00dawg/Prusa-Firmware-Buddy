// screen_printing.cpp
#include "screen_printing.hpp"
#include "marlin_client.hpp"
#include "print_utils.hpp"
#include "ffconf.h"
#include "ScreenHandler.hpp"
#include <ctime>
#include "../lang/format_print_will_end.hpp"
#include "window_dlg_popup.hpp"
#include "odometer.hpp"
#include "liveadjust_z.hpp"
#include "DialogMoveZ.hpp"
#include "metric.h"
#include "screen_menu_tune.hpp"

#ifdef DEBUG_FSENSOR_IN_HEADER
    #include "filament_sensors_handler.hpp"
#endif

#include "Marlin/src/module/motion.h"
#include "Marlin/src/feature/bed_preheat.hpp"

#if ENABLED(CRASH_RECOVERY)
    #include "../Marlin/src/feature/prusa/crash_recovery.h"
#endif

enum class Btn {
    Tune = 0,
    Pause,
    Stop
};

static constexpr BtnResource btn_res[static_cast<size_t>(item_id_t::count)] = {
    { N_("Tune"), &png::settings_58x58 },
    { N_("Pause"), &png::pause_58x58 },
    { N_("Pausing..."), &png::pause_58x58 },
    { N_("Stop"), &png::stop_58x58 },
    { N_("Resume"), &png::resume_48x48 },
    { N_("Resuming..."), &png::resume_48x48 },
    { N_("Heating..."), &png::resume_48x48 }, // reheating is same as resume, but disabled
    { N_("Reprint"), &png::reprint_48x48 },
    { N_("Home"), &png::home_58x58 },
    { N_("Skip"), &png::resume_48x48 },
};

void screen_printing_data_t::invalidate_print_state() {
    state__readonly__use_change_print_state = printing_state_t::COUNT;
}
printing_state_t screen_printing_data_t::GetState() const {
    return state__readonly__use_change_print_state;
}

void screen_printing_data_t::tuneAction() {
    if (btn_tune.ico.IsShadowed()) {
        return;
    }
    switch (GetState()) {
    case printing_state_t::PRINTING:
    case printing_state_t::ABSORBING_HEAT:
    case printing_state_t::PAUSED:
        Screens::Access()->Open(ScreenFactory::Screen<ScreenMenuTune>);
        break;
    default:
        break;
    }
}

void screen_printing_data_t::pauseAction() {
    if (btn_pause.ico.IsShadowed()) {
        return;
    }
    switch (GetState()) {
    case printing_state_t::PRINTING:
        marlin_print_pause();
        change_print_state();
        break;
    case printing_state_t::ABSORBING_HEAT:
        bed_preheat.skip_preheat();
        change_print_state();
        break;
    case printing_state_t::PAUSED:
        marlin_print_resume();
        change_print_state();
        break;
    case printing_state_t::STOPPED:
    case printing_state_t::PRINTED:
        screen_printing_reprint();
        change_print_state();
        break;
    default:
        break;
    }
}

void screen_printing_data_t::stopAction() {
    if (btn_stop.ico.IsShadowed()) {
        return;
    }
    switch (GetState()) {
    case printing_state_t::STOPPED:
    case printing_state_t::PRINTED:
        marlin_print_exit();
        return;
    case printing_state_t::PAUSING:
    case printing_state_t::RESUMING:
        return;
    default: {
        if (MsgBoxWarning(_("Are you sure to stop this printing?"), Responses_YesNo, 1)
            == Response::Yes) {
            stop_pressed = true;
            waiting_for_abort = true;
            marlin_print_abort();
            change_print_state();
        } else
            return;
    }
    }
}

/******************************************************************************/

screen_printing_data_t::screen_printing_data_t()
    : AddSuperWindow<ScreenPrintingModel>(_(caption))
#if (defined(USE_ILI9488))
    , print_progress(this)
#endif
#if defined(USE_ST7789)
    , w_filename(this, Rect16(10, 33, 220, 29))
    , w_progress(this, Rect16(10, 70, GuiDefaults::RectScreen.Width() - 2 * 10, 16))
    , w_progress_txt(this, Rect16(10, 86, GuiDefaults::RectScreen.Width() - 2 * 10, 30)) // font: Normal (11x18 px)
    , w_time_label(this, Rect16(10, 128, 101, 20), is_multiline::no)
    , w_time_value(this, Rect16(10, 148, 101, 20), is_multiline::no)
    , w_etime_label(this, Rect16(130, 128, 101, 20), is_multiline::no)
    , w_etime_value(this, Rect16(30, 148, 201, 20), is_multiline::no)
#elif defined(USE_ILI9488)
    , w_filename(this, Rect16(30, 38, 420, 24))
    , w_progress(this, Rect16(30, 65, GuiDefaults::RectScreen.Width() - 2 * 30, 16))
    , w_progress_txt(this, Rect16(300, 115, 150, 54))                 // Left side option: 30, 115, 100, 54 | font: Large (53x30 px)
    , w_etime_label(this, Rect16(30, 114, 150, 20), is_multiline::no) // Right side option: 300, 118, 150, 20
    , w_etime_value(this, Rect16(30, 138, 200, 23), is_multiline::no) // Right side option: 250, 138, 200, 23
#endif // USE_<display>
    , message_timer(0)
    , stop_pressed(false)
    , waiting_for_abort(false)
    , state__readonly__use_change_print_state(printing_state_t::COUNT)
#if defined(USE_ST7789)
    , popup_rect(Rect16::Merge(std::array<Rect16, 4>({ w_time_label.GetRect(), w_time_value.GetRect(), w_etime_label.GetRect(), w_etime_value.GetRect() })))
#elif defined(USE_ILI9488)
    , popup_rect(Rect16(30, 115, 250, 70))                            // Rect for printing messages from marlin.
#endif // USE_<display>
    , time_end_format(PT_t::init) {
    marlin_error_clr(MARLIN_ERR_ProbingFailed);
    // we will handle HELD_RELEASED event in this window
    DisableLongHoldScreenAction();

    strlcpy(text_filament.data(), "999m", text_filament.size());

#if defined(USE_ST7789)
    // ST7789 specific adjustments
    Align_t align = Align_t::RightBottom();
    w_filename.SetAlignment(Align_t::LeftBottom());
    w_progress_txt.SetAlignment(Align_t::Center());
    w_etime_label.SetAlignment(Align_t::RightBottom());
    w_etime_value.SetAlignment(Align_t::RightBottom());

    ResourceId etime_val_font = IDR_FNT_SMALL;
    w_progress_txt.SetFont(resource_font(IDR_FNT_NORMAL));

    // ST7789 specific variable and it's label
    w_time_label.font = resource_font(IDR_FNT_SMALL);
    w_time_label.SetAlignment(align);
    w_time_label.SetPadding({ 0, 2, 0, 2 });
    w_time_label.SetText(_("Printing time"));

    w_time_value.font = resource_font(IDR_FNT_SMALL);
    w_time_value.SetAlignment(align);
    w_time_value.SetPadding({ 0, 2, 0, 2 });
#elif defined(USE_ILI9488)
    // ILI_9488 specific adjustments
    w_filename.SetAlignment(Align_t::LeftTop());
    w_progress_txt.SetAlignment(Align_t::RightTop());
    w_etime_label.SetAlignment(Align_t::LeftBottom());
    w_etime_value.SetAlignment(Align_t::LeftBottom());

    w_etime_label.SetTextColor(COLOR_SILVER);
    ResourceId etime_val_font = IDR_FNT_NORMAL;
    w_progress_txt.SetFont(resource_font(IDR_FNT_LARGE));
#endif // USE_<display>

    w_filename.font = resource_font(IDR_FNT_BIG);
    w_filename.SetPadding({ 0, 0, 0, 0 });
    // this MakeRAM is safe - vars->media_LFN is statically allocated (even though it may not be obvious at the first look)
    marlin_vars()->media_LFN.copy_to(gui_media_LFN, sizeof(gui_media_LFN));
    w_filename.SetText(string_view_utf8::MakeRAM((const uint8_t *)gui_media_LFN));

    w_etime_label.font = resource_font(IDR_FNT_SMALL);

    // Execute first print time update loop
    updateTimes();

    w_etime_value.font = resource_font(etime_val_font);
    w_etime_value.SetPadding({ 0, 2, 0, 2 });

#if defined(USE_ILI9488)
    print_progress.Pause();
    last_e_axis_position = marlin_vars()->curr_pos[MARLIN_VAR_INDEX_E];
#endif

    initAndSetIconAndLabel(btn_tune, res_tune);
    initAndSetIconAndLabel(btn_pause, res_pause);
    initAndSetIconAndLabel(btn_stop, res_stop);
}

#ifdef DEBUG_FSENSOR_IN_HEADER
extern int _is_in_M600_flg;
extern uint32_t *pCommand;
#endif

#if DEVELOPMENT_ITEMS() && !DEVELOPER_MODE()
static metric_t print_successful = METRIC("Print_successful", METRIC_VALUE_INTEGER, 0, METRIC_HANDLER_ENABLE_ALL);
#endif

void screen_printing_data_t::windowEvent(EventLock /*has private ctor*/, window_t *sender, GUI_event_t event, void *param) {
#ifdef DEBUG_FSENSOR_IN_HEADER
    static int _last = 0;
    if (gui::GetTick() - _last > 300) {
        _last = gui::GetTick();

        static char buff[] = "Sx Mx x xxxx";                         //"x"s are replaced
        buff[1] = FSensors_instance().Get() + '0';                   // S0 init, S1 has filament, S2 no filament, S3 not connected, S4 disabled
        buff[4] = FSensors_instance().GetM600_send_on();             // Me edge, Ml level, Mn never, Mx undefined
        buff[6] = FSensors_instance().WasM600_send() ? 's' : 'n';    // s == send, n== not send
        buff[8] = _is_in_M600_flg ? 'M' : '0';                       // M == marlin is doing M600
        buff[9] = marlin_event(MARLIN_EVT_CommandBegin) ? 'B' : '0'; // B == Event begin
        buff[10] = marlin_command() == MARLIN_CMD_M600 ? 'C' : '0';  // C == Command M600
        buff[11] = *pCommand == MARLIN_CMD_M600 ? 's' : '0';         // s == server - Command M600
        header.SetText(buff);
    }

#endif

    /// check stop clicked when MBL is running
    printing_state_t p_state = GetState();
    if (stop_pressed && waiting_for_abort && marlin_command() != MARLIN_CMD_G29 && (p_state == printing_state_t::ABORTING || p_state == printing_state_t::PAUSED)) {
        marlin_print_abort();
        waiting_for_abort = false;
        return;
    }

#if ENABLED(NOZZLE_LOAD_CELL) && ENABLED(PROBE_CLEANUP_SUPPORT)
    if (marlin_error(MARLIN_ERR_NozzleCleaningFailed)) {
        marlin_error_clr(MARLIN_ERR_NozzleCleaningFailed);
        if (MsgBox(_("Nozzle cleaning failed."), Responses_RetryAbort) == Response::Retry) {
            marlin_print_resume();
        } else {
            marlin_print_abort();
            return;
        }
    }
#endif

#if HAS_BED_PROBE
    if ((p_state == printing_state_t::PRINTED || p_state == printing_state_t::PAUSED) && marlin_error(MARLIN_ERR_ProbingFailed)) {
        marlin_error_clr(MARLIN_ERR_ProbingFailed);
        marlin_print_abort();
        while (marlin_vars()->print_state == mpsAborting_Begin
            || marlin_vars()->print_state == mpsAborting_WaitIdle
            || marlin_vars()->print_state == mpsAborting_ParkHead) {
            gui_loop(); // Wait while aborting
        }
        if (MsgBox(_("Bed leveling failed. Try again?"), Responses_YesNo) == Response::Yes) {
            screen_printing_reprint(); // Restart print
        } else {
            return;
        }
    }
#endif

    change_print_state();

#if DEVELOPMENT_ITEMS() && !DEVELOPER_MODE()
    if (p_state == printing_state_t::PRINTING)
        print_feedback_pending = true;

    if (p_state == printing_state_t::PRINTED && print_feedback_pending) {
        print_feedback_pending = false;
        switch (MsgBox(_("Was the print successful?"), Responses_YesNoIgnore, 2)) {
        case Response::Yes:
            metric_record_integer(&print_successful, 1);
            break;
        case Response::No:
            metric_record_integer(&print_successful, 0);
            break;
        case Response::Ignore:
            metric_record_integer(&print_successful, -1);
        default:
            break;
        }
    }
#endif

    /// -- Print time update loop
    updateTimes();

    /// -- close screen when print is done / stopped and USB media is removed
    if (!marlin_vars()->media_inserted && (p_state == printing_state_t::PRINTED || p_state == printing_state_t::STOPPED)) {
        marlin_print_exit();
        return;
    }

    /// -- check when media is or isn't inserted
    if (event == GUI_event_t::MEDIA) {
        /// -- check for enable/disable resume button
        set_pause_icon_and_label();
    }
    if (event == GUI_event_t::HELD_RELEASED) {
        if (marlin_vars()->curr_pos[2 /* Z Axis */] <= 1.0f && p_state == printing_state_t::PRINTING) {
            LiveAdjustZ::Show();
        } else if (p_state == printing_state_t::PRINTED || p_state == printing_state_t::STOPPED) {
            DialogMoveZ::Show();
        }
        return;
    }
#if defined(USE_ILI9488)
    if (event == GUI_event_t::LOOP && p_state == printing_state_t::PRINTING) {
        auto vars = marlin_vars();
        const bool midprint = vars->curr_pos[MARLIN_VAR_INDEX_Z] >= 0.0f;
        const bool extruder_moved = (vars->curr_pos[MARLIN_VAR_INDEX_E] - last_e_axis_position) > 0;
        if (print_progress.isPaused() && midprint && extruder_moved) {
            print_progress.Resume();
        } else if (print_progress.isPaused()) {
            last_e_axis_position = vars->curr_pos[MARLIN_VAR_INDEX_E];
        }
    }
#endif

    if (p_state == printing_state_t::PRINTED || p_state == printing_state_t::STOPPED) {
#if defined(USE_ILI9488)
        if (p_state == printing_state_t::PRINTED)
            print_progress.FinishedMode();
        else
            print_progress.StoppedMode();
#endif
        w_etime_label.Hide();
        w_etime_value.Hide();
    } else {
#if defined(USE_ILI9488)
        print_progress.PrintingMode();
#endif
        w_etime_label.Show();
        w_etime_value.Show();
    }

    SuperWindowEvent(sender, event, param);
}

void screen_printing_data_t::updateTimes() {
    PT_t time_format = print_time.update_loop(time_end_format, &w_etime_value
#if defined(USE_ST7789)
        ,
        &w_time_value
#endif // USE_ST7789
    );

    if (time_format != time_end_format) {
        switch (time_format) {
        case PT_t::init: // should not happen
            return;
        case PT_t::countdown:
            w_etime_label.SetText(_(PrintTime::EN_STR_COUNTDOWN));
            break;
        case PT_t::timestamp:
            w_etime_label.SetText(_(PrintTime::EN_STR_TIMESTAMP));
            break;
        }

        time_end_format = time_format;
    }
}

void screen_printing_data_t::disable_tune_button() {
    btn_tune.ico.Shadow();
    btn_tune.ico.Disable(); // can't be focused

    // move to reprint when tune is focused
    if (btn_tune.ico.IsFocused()) {
        btn_pause.ico.SetFocus();
    }
    btn_tune.ico.Invalidate();
}

void screen_printing_data_t::enable_tune_button() {
    btn_tune.ico.Unshadow();
    btn_tune.ico.Enable(); // can be focused
    btn_tune.ico.Invalidate();
}

void screen_printing_data_t::screen_printing_reprint() {
    print_begin(gui_media_SFN_path, true);
    screen_printing_data_t::updateTimes(); // reinit, but should be already set correctly
    btn_stop.txt.SetText(_(btn_res[static_cast<size_t>(item_id_t::stop)].first));
    btn_stop.ico.SetRes(btn_res[static_cast<size_t>(item_id_t::stop)].second);

#ifndef DEBUG_FSENSOR_IN_HEADER
    header.SetText(_("PRINTING"));
#endif
}

// todo use it
/*static void mesh_err_stop_print() {
    float target_nozzle = marlin_vars()->target_nozzle;
    float target_bed = marlin_vars()->target_bed;
    marlin_print_abort();
    while (marlin_vars()->sd_printing) {
        gui_loop();
    }
    //marlin_park_head();
    marlin_gcode_printf("M104 S%F", (double)target_nozzle);
    marlin_gcode_printf("M140 S%F", (double)target_bed);
    marlin_gcode("G0 Z30"); //Z 30mm
    marlin_gcode("M84");    //Disable steppers
    while (marlin_vars()->pqueue) {
        gui_loop();
    }
}*/

void screen_printing_data_t::set_icon_and_label(item_id_t id_to_set, window_icon_t *p_button, window_text_t *lbl) {
    size_t index = static_cast<size_t>(id_to_set);
    p_button->SetRes(btn_res[index].second);
    lbl->SetText(_(btn_res[index].first));
}

void screen_printing_data_t::enable_button(window_icon_t *p_button) {
    if (p_button->IsShadowed()) {
        p_button->Unshadow();
        p_button->Enable();
        p_button->Invalidate();
    }
}

void screen_printing_data_t::disable_button(window_icon_t *p_button) {
    if (!p_button->IsShadowed()) {
        p_button->Shadow();
        p_button->Disable();
        p_button->Invalidate();
    }
}

void screen_printing_data_t::set_pause_icon_and_label() {
    window_icon_t *const p_button = &btn_pause.ico;
    window_text_t *const pLabel = &btn_pause.txt;

    // todo it is static, because menu tune is not dialog
    // switch (state__readonly__use_change_print_state)
    switch (GetState()) {
    case printing_state_t::COUNT:
    case printing_state_t::INITIAL:
    case printing_state_t::PRINTING:
    case printing_state_t::MBL_FAILED:
        enable_button(p_button);
        set_icon_and_label(item_id_t::pause, p_button, pLabel);
        break;
    case printing_state_t::ABSORBING_HEAT:
        enable_button(p_button);
        set_icon_and_label(item_id_t::skip, p_button, pLabel);
        break;
    case printing_state_t::PAUSING:
        disable_button(p_button);
        set_icon_and_label(item_id_t::pausing, p_button, pLabel);
        break;
    case printing_state_t::PAUSED:
        enable_button(p_button);
        set_icon_and_label(item_id_t::resume, p_button, pLabel);
        if (!marlin_vars()->media_inserted) {
            disable_button(p_button);
        }
        break;
    case printing_state_t::RESUMING:
        disable_button(p_button);
        set_icon_and_label(item_id_t::resuming, p_button, pLabel);
        break;
    case printing_state_t::REHEATING:
    case printing_state_t::REHEATING_DONE:
        disable_button(p_button);
        set_icon_and_label(item_id_t::reheating, p_button, pLabel);
        break;
    case printing_state_t::STOPPED:
    case printing_state_t::PRINTED:
        enable_button(p_button);
        set_icon_and_label(item_id_t::reprint, p_button, pLabel);
        break;
    case printing_state_t::ABORTING:
        disable_button(p_button);
        break;
    }
}

void screen_printing_data_t::set_tune_icon_and_label() {
    window_icon_t *const p_button = &btn_tune.ico;
    window_text_t *const pLabel = &btn_tune.txt;

    // must be before switch
    set_icon_and_label(item_id_t::settings, p_button, pLabel);

    switch (GetState()) {
    case printing_state_t::PRINTING:
    case printing_state_t::ABSORBING_HEAT:
    case printing_state_t::PAUSED:
        enable_tune_button();
        break;
    case printing_state_t::ABORTING:
        disable_button(p_button);
        break;
    default:
        disable_tune_button();
        break;
    }
}

void screen_printing_data_t::set_stop_icon_and_label() {
    window_icon_t *const p_button = &btn_stop.ico;
    window_text_t *const pLabel = &btn_stop.txt;

    switch (GetState()) {
    case printing_state_t::STOPPED:
    case printing_state_t::PRINTED:
        enable_button(p_button);
        set_icon_and_label(item_id_t::home, p_button, pLabel);
        break;
    case printing_state_t::PAUSING:
    case printing_state_t::RESUMING:
        disable_button(p_button);
        set_icon_and_label(item_id_t::stop, p_button, pLabel);
        break;
    case printing_state_t::REHEATING:
        enable_button(p_button);
        set_icon_and_label(item_id_t::stop, p_button, pLabel);
        break;
    case printing_state_t::ABORTING:
        disable_button(p_button);
        break;
    default:
        enable_button(p_button);
        set_icon_and_label(item_id_t::stop, p_button, pLabel);
        break;
    }
}

void screen_printing_data_t::change_print_state() {
    printing_state_t st = printing_state_t::COUNT;

    switch (marlin_vars()->print_state) {
    case mpsIdle:
    case mpsWaitGui:
    case mpsPrintPreviewInit:
    case mpsPrintPreviewImage:
    case mpsPrintPreviewQuestions:
    case mpsPrintInit:
        st = printing_state_t::INITIAL;
        break;
    case mpsPrinting:
        if (bed_preheat.is_waiting()) {
            st = printing_state_t::ABSORBING_HEAT;
        } else {
            st = printing_state_t::PRINTING;
        }
        break;
    case mpsPowerPanic_AwaitingResume:
    case mpsPaused:
        // stop_pressed = false;
        st = printing_state_t::PAUSED;
        break;
    case mpsPausing_Begin:
    case mpsPausing_Failed_Code:
    case mpsPausing_WaitIdle:
    case mpsPausing_ParkHead:
        st = printing_state_t::PAUSING;
// When print is paused, progress screen needs to reinit it's thumbnail file handler
// because USB removal error crashes file handler access. Progress screen should not be enabled during pause -> reinit on EVERY pause
#if defined(USE_ILI9488)
        print_progress.Pause();
#endif
        break;
    case mpsResuming_Reheating:
        stop_pressed = false;
        st = printing_state_t::REHEATING;
        break;
    case mpsResuming_Begin:
    case mpsResuming_UnparkHead_XY:
    case mpsResuming_UnparkHead_ZE:
    case mpsCrashRecovery_Begin:
    case mpsCrashRecovery_Retracting:
    case mpsCrashRecovery_Lifting:
    case mpsCrashRecovery_XY_Measure:
    case mpsCrashRecovery_Tool_Pickup:
    case mpsCrashRecovery_XY_HOME:
    case mpsCrashRecovery_Axis_NOK:
    case mpsCrashRecovery_Repeated_Crash:
    case mpsPowerPanic_Resume:
        stop_pressed = false;
        st = printing_state_t::RESUMING;
#if (PRINTER_TYPE != PRINTER_PRUSA_IXL && defined(USE_ILI9488))
        print_progress.Resume();
#endif
        break;
    case mpsAborting_Begin:
    case mpsAborting_WaitIdle:
    case mpsAborting_ParkHead:
        stop_pressed = false;
        st = printing_state_t::ABORTING;
        break;
    case mpsFinishing_WaitIdle:
    case mpsFinishing_ParkHead:
        st = printing_state_t::PRINTING;
        break;
    case mpsAborted:
        stop_pressed = false;
        st = printing_state_t::STOPPED;
        break;
    case mpsFinished:
    case mpsExit:
        st = printing_state_t::PRINTED;
        break;
    case mpsPowerPanic_acFault:
        // this state is never reached
        __builtin_unreachable();
        return;
    }
    if (stop_pressed) {
        st = printing_state_t::ABORTING;
    }
    if (state__readonly__use_change_print_state != st) {
        state__readonly__use_change_print_state = st;
        set_pause_icon_and_label();
        set_tune_icon_and_label();
        set_stop_icon_and_label();
    }
    if (st == printing_state_t::PRINTED || st == printing_state_t::STOPPED || st == printing_state_t::PAUSED) {
        Odometer_s::instance().force_to_eeprom();
    }
}
