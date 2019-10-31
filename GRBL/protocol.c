/*
  protocol.c - controls Grbl execution protocol and procedures
  Part of Grbl

  Copyright (c) 2017-2019 Terje Io
  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "grbl.h"

// Define line flags. Includes comment type tracking and line overflow detection.
typedef union {
    uint8_t value;
    struct {
        uint8_t overflow            :1,
                comment_parentheses :1,
                comment_semicolon   :1,
                block_delete        :1,
                unassigned          :4;
    };
} line_flags_t;

typedef struct {
    char *message;
    uint_fast8_t idx;
    uint_fast8_t tracker;
    bool show;
} user_message_t;

static uint_fast16_t char_counter = 0;
static char line[LINE_BUFFER_SIZE]; // Line to be executed. Zero-terminated.
static char xcommand[LINE_BUFFER_SIZE];
static bool keep_rt_commands = false;
static user_message_t user_message = {NULL, 0, 0, false};
static const char *msg = "(MSG,";
static void protocol_exec_rt_suspend();

// add gcode to execute not originating from normal input stream
bool protocol_enqueue_gcode (char *gcode)
{
    bool ok = xcommand[0] == '\0' &&
               (sys.state == STATE_IDLE || (sys.state & (STATE_JOG|STATE_TOOL_CHANGE))) &&
                 bit_isfalse(sys_rt_exec_state, EXEC_MOTION_CANCEL);

    if(ok && gc_state.file_run)
        ok = gc_state.modal.program_flow != ProgramFlow_Running || strncmp((char *)gcode, "$J=", 3);

    if(ok)
        strcpy(xcommand, gcode);

    return ok;
}

/*
  GRBL PRIMARY LOOP:
*/
bool protocol_main_loop(bool cold_start)
{
    if (hal.system_control_get_state().e_stop) {
        // Check for e-stop active. Blocks everything until cleared.
        set_state(STATE_ESTOP);
        report_alarm_message(Alarm_EStop);
        hal.report.feedback_message(Message_EStop);
    } else if (settings.homing.flags.enabled && sys.homing.mask && settings.homing.flags.init_lock && sys.homing.mask != sys.homed.mask) {
        // Check for power-up and set system alarm if homing is enabled to force homing cycle
        // by setting Grbl's alarm state. Alarm locks out all g-code commands, including the
        // startup scripts, but allows access to settings and internal commands.
        // Only a successful homing cycle '$H' will disable the alarm.
        // NOTE: The startup script will run after successful completion of the homing cycle. Prevents motion startup
        // blocks from crashing into things uncontrollably. Very bad.
        set_state(STATE_ALARM); // Ensure alarm state is active.
        report_alarm_message(Alarm_HomingRequried);
        hal.report.feedback_message(Message_HomingCycleRequired);
    } else if (settings.limits.flags.hard_enabled && settings.limits.flags.check_at_init && hal.limits_get_state().value) {
        // Check that no limit switches are engaged to make sure everything is good to go.
        set_state(STATE_ALARM); // Ensure alarm state is active.
        report_alarm_message(Alarm_LimitsEngaged);
        hal.report.feedback_message(Message_CheckLimits);
    } else if(cold_start && (settings.flags.force_initialization_alarm || hal.system_control_get_state().reset)) {
        set_state(STATE_ALARM); // Ensure alarm state is set.
        hal.report.feedback_message(Message_AlarmLock);
    } else if (sys.state & (STATE_ALARM|STATE_SLEEP)) {
        // Check for and report alarm state after a reset, error, or an initial power up.
        // NOTE: Sleep mode disables the stepper drivers and position can't be guaranteed.
        // Re-initialize the sleep state as an ALARM mode to ensure user homes or acknowledges.
        set_state(STATE_ALARM); // Ensure alarm state is set.
        hal.report.feedback_message(Message_AlarmLock);
    } else {
        // Check if the safety door is open.
        set_state(STATE_IDLE);
        if (!settings.flags.safety_door_ignore_when_idle && hal.system_control_get_state().safety_door_ajar) {
            bit_true(sys_rt_exec_state, EXEC_SAFETY_DOOR);
            protocol_execute_realtime(); // Enter safety door mode. Should return as IDLE state.
        }
        // All systems go!
        system_execute_startup(line); // Execute startup script.
    }

    // ---------------------------------------------------------------------------------
    // Primary loop! Upon a system abort, this exits back to main() to reset the system.
    // This is also where Grbl idles while waiting for something to do.
    // ---------------------------------------------------------------------------------

    int16_t c;
    char eol = '\0';
    line_flags_t line_flags = {0};
    bool nocaps = false, gcode_error = false;

    xcommand[0] = '\0';
    user_message.show = keep_rt_commands = false;

    while(true) {

        // Process one line of incoming stream data, as the data becomes available. Performs an
        // initial filtering by removing spaces and comments and capitalizing all letters.
        while((c = hal.stream.read()) != SERIAL_NO_DATA) {

            if(c == ASCII_CAN) {

                eol = xcommand[0] = '\0';
                keep_rt_commands = nocaps = gcode_error = user_message.show = false;
                char_counter = line_flags.value = 0;
                gc_state.last_error = Status_OK;

                if (sys.state == STATE_JOG) // Block all other states from invoking motion cancel.
                    system_set_exec_state_flag(EXEC_MOTION_CANCEL);

            } else if ((c == '\n') || (c == '\r')) { // End of line reached

                // Check for possible secondary end of line character, do not process as empty line
                // if part of crlf (or lfcr pair) as this produces a possibly unwanted double response
                if(char_counter == 0 && eol && eol != c) {
                    eol = '\0';
                    continue;
                } else
                    eol = (char)c;

                if(!protocol_execute_realtime()) // Runtime command check point.
                    return !sys.flags.exit;      // Bail to calling function upon system abort

                line[char_counter] = '\0'; // Set string termination character.

              #ifdef REPORT_ECHO_LINE_RECEIVED
                report_echo_line_received(line);
              #endif

                // Direct and execute one line of formatted input, and report status of execution.
                if (line_flags.overflow) // Report line overflow error.
                    gc_state.last_error = Status_Overflow;
                else if ((line[0] == '\0' || char_counter == 0) && !user_message.show) // Empty or comment line. For syncing purposes.
                    gc_state.last_error = Status_OK;
                else if (line[0] == '$') {// Grbl '$' system command
                    gcode_error = false;
                    if((gc_state.last_error = system_execute_line(line)) == Status_LimitsEngaged) {
                        set_state(STATE_ALARM); // Ensure alarm state is active.
                        report_alarm_message(Alarm_LimitsEngaged);
                        hal.report.feedback_message(Message_CheckLimits);
                    }
                } else if (line[0] == '[' && hal.user_command_execute) {
                    gcode_error = false;
                    gc_state.last_error = hal.user_command_execute(line);
                } else if (sys.state & (STATE_ALARM|STATE_ESTOP|STATE_JOG)) // Everything else is gcode. Block if in alarm, eStop or jog mode.
                    gc_state.last_error = Status_SystemGClock;
                else if(!gcode_error) { // Parse and execute g-code block.
                    gc_state.last_error = gc_execute_block(line, user_message.show ? user_message.message : NULL);
#if COMPATIBILITY_LEVEL == 0
                    gcode_error = gc_state.last_error != Status_OK;
#endif
                }
                hal.report.status_message(gc_state.last_error);

                // Reset tracking data for next line.
                keep_rt_commands = nocaps = user_message.show = false;
                char_counter = line_flags.value = 0;

            } else if (c <= (nocaps ? ' ' - 1 : ' ') || line_flags.value) {
                // Throw away all whitepace, control characters, comment characters and overflow characters.
                if(c >= ' ' && line_flags.comment_parentheses) {
                    if(user_message.tracker == 5)
                        user_message.message[user_message.idx++] = c == ')' ? '\0' : c;
                    else if(user_message.tracker > 0 && CAPS(c) == msg[user_message.tracker])
                        user_message.tracker++;
                    else
                        user_message.tracker = 0;
                    if (c == ')') {
                        // End of '()' comment. Resume line.
                        line_flags.comment_parentheses = Off;
                        keep_rt_commands = false;
                        user_message.show = user_message.show || user_message.tracker == 5;
                    }
                }
            } else {
                switch(c) {

                    case '/':
                        if(char_counter == 0)
                            line_flags.block_delete = sys.flags.block_delete_enabled;
                        break;

                    case '$':
                        // Do not uppercase system command - will destroy passwords etc...
                        if(char_counter == 0)
                            nocaps = keep_rt_commands = true;
                        break;

                    case '(':
                        if(!keep_rt_commands) {
                            // Enable comments flag and ignore all characters until ')' or EOL unless it is a message.
                            // NOTE: This doesn't follow the NIST definition exactly, but is good enough for now.
                            // In the future, we could simply remove the items within the comments, but retain the
                            // comment control characters, so that the g-code parser can error-check it.
                            if((line_flags.comment_parentheses = !line_flags.comment_semicolon)) {
                                if(hal.show_message) {
                                    if(user_message.message == NULL)
                                        user_message.message = malloc(LINE_BUFFER_SIZE);
                                    if(user_message.message) {
                                        user_message.idx = 0;
                                        user_message.tracker = 1;
                                    }
                                }
                                keep_rt_commands = true;
                            }
                        }
                        break;

                    case ';':
                        // NOTE: ';' comment to EOL is a LinuxCNC definition. Not NIST.
                        if(!keep_rt_commands) {
                            if((line_flags.comment_semicolon = !line_flags.comment_parentheses))
                                keep_rt_commands = true;
                        }
                        break;
                }
                if (line_flags.value == 0 && !(line_flags.overflow = char_counter >= (LINE_BUFFER_SIZE - 1)))
                    line[char_counter++] = nocaps ? c : CAPS(c);
            }
        }

        // Handle extra command (internal stream)
        if(xcommand[0] != '\0') {

            if (xcommand[0] == '$') // Grbl '$' system command
                system_execute_line(xcommand);
            else if (sys.state & (STATE_ALARM|STATE_ESTOP|STATE_JOG)) // Everything else is gcode. Block if in alarm, eStop or jog state.
                hal.report.status_message(Status_SystemGClock);
            else // Parse and execute g-code block.
                gc_execute_block(xcommand, NULL);

            xcommand[0] = '\0';
        }

        // If there are no more characters in the input stream buffer to be processed and executed,
        // this indicates that g-code streaming has either filled the planner buffer or has
        // completed. In either case, auto-cycle start, if enabled, any queued moves.
        protocol_auto_cycle_start();

        if(!protocol_execute_realtime() && sys.abort) // Runtime command check point.
            return !sys.flags.exit;                   // Bail to main() program loop to reset system.

        sys.cancel = false;

        // Check for sleep conditions and execute auto-park, if timeout duration elapses.
        if(settings.flags.sleep_enable)
            sleep_check();
    }
}


// Block until all buffered steps are executed or in a cycle state. Works with feed hold
// during a synchronize call, if it should happen. Also, waits for clean cycle end.
bool protocol_buffer_synchronize ()
{
    bool ok = true;
    // If system is queued, ensure cycle resumes if the auto start flag is present.
    protocol_auto_cycle_start();
    while ((ok = protocol_execute_realtime()) && (plan_get_current_block() || sys.state == STATE_CYCLE));

    return ok;
}


// Auto-cycle start triggers when there is a motion ready to execute and if the main program is not
// actively parsing commands.
// NOTE: This function is called from the main loop, buffer sync, and mc_line() only and executes
// when one of these conditions exist respectively: There are no more blocks sent (i.e. streaming
// is finished, single commands), a command that needs to wait for the motions in the buffer to
// execute calls a buffer sync, or the planner buffer is full and ready to go.
void protocol_auto_cycle_start ()
{
    if (plan_get_current_block() != NULL) // Check if there are any blocks in the buffer.
        system_set_exec_state_flag(EXEC_CYCLE_START); // If so, execute them!
}


// This function is the general interface to Grbl's real-time command execution system. It is called
// from various check points in the main program, primarily where there may be a while loop waiting
// for a buffer to clear space or any point where the execution time from the last check point may
// be more than a fraction of a second. This is a way to execute realtime commands asynchronously
// (aka multitasking) with grbl's g-code parsing and planning functions. This function also serves
// as an interface for the interrupts to set the system realtime flags, where only the main program
// handles them, removing the need to define more computationally-expensive volatile variables. This
// also provides a controlled way to execute certain tasks without having two or more instances of
// the same task, such as the planner recalculating the buffer upon a feedhold or overrides.
// NOTE: The sys_rt_exec_state variable flags are set by any process, step or input strea events, pinouts,
// limit switches, or the main program.
// Returns false if aborted
bool protocol_execute_realtime ()
{
    if(protocol_exec_rt_system()) {

        if(hal.execute_realtime)
            hal.execute_realtime(sys.state);

        if (sys.suspend)
            protocol_exec_rt_suspend();

      #ifdef EMULATE_EEPROM
        if((sys.state == STATE_IDLE || sys.state == STATE_ALARM) && settings_dirty.is_dirty && !gc_state.file_run)
            eeprom_emu_sync_physical();
      #endif
    }

    return !ABORTED;
}

// Dangerous? This fn may be called from stepper ISR and will block (possibly forever?) if a previous message is beeing displayed...
// Use a real queue or "double buffering" instead of spin lock?
void protocol_message (char *message)
{
    static volatile bool spin_lock = false;

    while(spin_lock);

    spin_lock = true;

    if(message) {
        if(sys.message)
            free(sys.message);
        sys.message = message;
    } else if(sys.message) {
        hal.show_message(sys.message);
        free(sys.message);
        sys.message = NULL;
    }

    spin_lock = false;
}

// Executes run-time commands, when required. This function primarily operates as Grbl's state
// machine and controls the various real-time features Grbl has to offer.
// NOTE: Do not alter this unless you know exactly what you are doing!
bool protocol_exec_rt_system ()
{
    uint_fast16_t rt_exec;

    // Display message if provided
    if(sys.message)
        protocol_message(NULL);

    if (sys_rt_exec_alarm && (rt_exec = system_clear_exec_alarm())) { // Enter only if any bit flag is true
        // System alarm. Everything has shutdown by something that has gone severely wrong. Report
        // the source of the error to the user. If critical, Grbl disables by entering an infinite
        // loop until system reset/abort.
        set_state((alarm_code_t)rt_exec == Alarm_EStop ? STATE_ESTOP : STATE_ALARM); // Set system alarm state
        report_alarm_message((alarm_code_t)rt_exec);
        // Halt everything upon a critical event flag. Currently hard and soft limits flag this.
        if ((alarm_code_t)rt_exec == Alarm_HardLimit || (alarm_code_t)rt_exec == Alarm_SoftLimit || (alarm_code_t)rt_exec == Alarm_EStop) {
            system_set_exec_alarm(rt_exec);
            hal.report.feedback_message((alarm_code_t)rt_exec == Alarm_EStop ? Message_EStop : Message_CriticalEvent);
            system_clear_exec_state_flag(EXEC_RESET); // Disable any existing reset
            while (bit_isfalse(sys_rt_exec_state, EXEC_RESET)) {
                // Block everything, except reset and status reports, until user issues reset or power
                // cycles. Hard limits typically occur while unattended or not paying attention. Gives
                // the user and a GUI time to do what is needed before resetting, like killing the
                // incoming stream. The same could be said about soft limits. While the position is not
                // lost, continued streaming could cause a serious crash if by chance it gets executed.
         //       hal.delay_ms(20, NULL);
                if (system_clear_exec_state_flag(EXEC_STATUS_REPORT) & EXEC_STATUS_REPORT)
                    report_realtime_status();
            }
            system_clear_exec_alarm(); // Clear alarm
        }
    }

    if (sys_rt_exec_state && (rt_exec = system_clear_exec_states())) { // Get and clear volatile sys_rt_exec_state atomically.

        // Execute system abort.
        if (rt_exec & EXEC_RESET) {

            if(hal.driver_reset)
                hal.driver_reset();

            sys.abort = !hal.system_control_get_state().e_stop;  // Only place this is set true.
            return !sys.abort; // Nothing else to do but exit.
        }

        if(rt_exec & EXEC_STOP) { // Experimental for now, must be verified. Do NOT move to interrupt context!

            sys.cancel = true;
            sys.step_control.flags = 0;
            sys.flags.feed_hold_pending = Off;
            sys.flags.delay_overrides = Off;
            if(sys.override.control.sync)
                sys.override.control = gc_state.modal.override_ctrl;

            gc_state.tool_change = false;
            gc_state.modal.coolant.value = 0;
            gc_state.modal.spindle.value = 0;
            gc_state.modal.spindle_rpm_mode = SpindleSpeedMode_RPM;

            // Kill spindle and coolant. TODO: Check Mach3 behaviour?
            hal.spindle_set_state(gc_state.modal.spindle, 0.0f);
            hal.coolant_set_state(gc_state.modal.coolant);
            sys.report.spindle = sys.report.coolant = On; // Set to report change immediately

            if(hal.driver_reset)
                hal.driver_reset();

            if(hal.stream.suspend_read && hal.stream.suspend_read(false))
                hal.stream.cancel_read_buffer(); // flush pending blocks (after M6)

            plan_reset();
            st_reset();
            gc_sync_position();
            plan_sync_position();
            flush_override_buffers();
            set_state(STATE_IDLE);
        }

        // Execute and print status to output stream
        if (rt_exec & EXEC_STATUS_REPORT)
            report_realtime_status();

        if(rt_exec & EXEC_GCODE_REPORT)
            report_gcode_modes();

        // Execute and print PID log to output stream
        if (rt_exec & EXEC_PID_REPORT)
            report_pid_log();

        rt_exec &= ~(EXEC_STOP|EXEC_STATUS_REPORT|EXEC_GCODE_REPORT|EXEC_PID_REPORT); // clear requests already processed

        if(sys.flags.feed_hold_pending) {
            if(rt_exec & EXEC_CYCLE_START)
                sys.flags.feed_hold_pending = Off;
            else if(!sys.override.control.feed_hold_disable)
                rt_exec |= EXEC_FEED_HOLD;
        }

        // Let state machine handle any remaining requests
        if(rt_exec)
            update_state(rt_exec);
    }

    if(!sys.flags.delay_overrides) {

        // Execute overrides.

        if((rt_exec = get_feed_override())) {

            uint_fast8_t new_f_override = sys.override.feed_rate, new_r_override = sys.override.rapid_rate;

            do {

                switch(rt_exec) {

                    case CMD_OVERRIDE_FEED_RESET:
                        new_f_override = DEFAULT_FEED_OVERRIDE;
                        break;

                    case CMD_OVERRIDE_FEED_COARSE_PLUS:
                        new_f_override += FEED_OVERRIDE_COARSE_INCREMENT;
                        break;

                    case CMD_OVERRIDE_FEED_COARSE_MINUS:
                        new_f_override -= FEED_OVERRIDE_COARSE_INCREMENT;
                        break;

                    case CMD_OVERRIDE_FEED_FINE_PLUS:
                        new_f_override += FEED_OVERRIDE_FINE_INCREMENT;
                        break;

                    case CMD_OVERRIDE_FEED_FINE_MINUS:
                        new_f_override -= FEED_OVERRIDE_FINE_INCREMENT;
                        break;

                    case CMD_OVERRIDE_RAPID_RESET:
                        new_r_override = DEFAULT_RAPID_OVERRIDE;
                        break;

                    case CMD_OVERRIDE_RAPID_MEDIUM:
                        new_r_override = RAPID_OVERRIDE_MEDIUM;
                        break;

                    case CMD_OVERRIDE_RAPID_LOW:
                        new_r_override = RAPID_OVERRIDE_LOW;
                        break;
                  }

            } while((rt_exec = get_feed_override()));

            plan_feed_override(new_f_override, new_r_override);
        }

        if((rt_exec = get_accessory_override())) {

            bool spindle_stop = false;
            uint_fast8_t last_s_override = sys.override.spindle_rpm;
            coolant_state_t coolant_state = gc_state.modal.coolant;

            do {

                switch(rt_exec) {

                    case CMD_OVERRIDE_SPINDLE_RESET:
                        last_s_override = DEFAULT_SPINDLE_RPM_OVERRIDE;
                        break;

                    case CMD_OVERRIDE_SPINDLE_COARSE_PLUS:
                        last_s_override += SPINDLE_OVERRIDE_COARSE_INCREMENT;
                        break;

                    case CMD_OVERRIDE_SPINDLE_COARSE_MINUS:
                        last_s_override -= SPINDLE_OVERRIDE_COARSE_INCREMENT;
                        break;

                    case CMD_OVERRIDE_SPINDLE_FINE_PLUS:
                        last_s_override += SPINDLE_OVERRIDE_FINE_INCREMENT;
                        break;

                    case CMD_OVERRIDE_SPINDLE_FINE_MINUS:
                        last_s_override -= SPINDLE_OVERRIDE_FINE_INCREMENT;
                        break;

                    case CMD_OVERRIDE_SPINDLE_STOP:
                        spindle_stop = !spindle_stop;
                        break;

                    case CMD_OVERRIDE_COOLANT_MIST_TOGGLE:
                        if (hal.driver_cap.mist_control && ((sys.state == STATE_IDLE) || (sys.state & (STATE_CYCLE | STATE_HOLD)))) {
                            coolant_state.mist = !coolant_state.mist;
                        }
                        break;

                    case CMD_OVERRIDE_COOLANT_FLOOD_TOGGLE:
                        if ((sys.state == STATE_IDLE) || (sys.state & (STATE_CYCLE | STATE_HOLD))) {
                            coolant_state.flood = !coolant_state.flood;
                        }
                        break;

                    default:
                        if(hal.driver_rt_command_execute)
                            hal.driver_rt_command_execute(rt_exec);
                        break;
                }

            } while((rt_exec = get_accessory_override()));

            spindle_set_override(last_s_override);

          // NOTE: Since coolant state always performs a planner sync whenever it changes, the current
          // run state can be determined by checking the parser state.
            if(coolant_state.value != gc_state.modal.coolant.value) {
                coolant_set_state(coolant_state); // Report flag set in coolant_set_state().
                gc_state.modal.coolant = coolant_state;
            }

            if (spindle_stop && sys.state == STATE_HOLD && gc_state.modal.spindle.on) {
                // Spindle stop override allowed only while in HOLD state.
                // NOTE: Report flag is set in spindle_set_state() when spindle stop is executed.
                if (!sys.override.spindle_stop.value)
                    sys.override.spindle_stop.initiate = On;
                else if (sys.override.spindle_stop.enabled)
                    sys.override.spindle_stop.restore = On;
            }
        }
    } // End execute overrides.

    // Reload step segment buffer
    if (sys.state & (STATE_CYCLE | STATE_HOLD | STATE_SAFETY_DOOR | STATE_HOMING | STATE_SLEEP| STATE_JOG))
        st_prep_buffer();

    return !ABORTED;
}

// Handles Grbl system suspend procedures, such as feed hold, safety door, and parking motion.
// The system will enter this loop, create local variables for suspend tasks, and return to
// whatever function that invoked the suspend, such that Grbl resumes normal operation.
// This function is written in a way to promote custom parking motions. Simply use this as a
// template

static void protocol_exec_rt_suspend ()
{
    while (sys.suspend) {

        if (sys.abort)
            return;

        // Handle spindle overrides during suspend
        state_suspend_manager();

        // If door closed keep issuing cycle start requests until resumed
        if(sys.state == STATE_SAFETY_DOOR && !hal.system_control_get_state().safety_door_ajar)
            system_set_exec_state_flag(EXEC_CYCLE_START);

        // Check for sleep conditions and execute auto-park, if timeout duration elapses.
        // Sleep is valid for both hold and door states, if the spindle or coolant are on or
        // set to be re-enabled.
        if(settings.flags.sleep_enable)
            sleep_check();

        protocol_exec_rt_system();
    }
}

// Pick off (drop) real-time command characters from input stream.
// These characters are not passed into the main buffer,
// but rather sets system state flag bits for later execution by protocol_exec_rt_system().
// Called from input stream interrupt handler.
ISR_CODE bool protocol_enqueue_realtime_command (char c)
{
    bool drop = false;

    // 1. Process characters in the ranges 0x - 1x and 8x-Ax
    // Characters with functions assigned are always acted upon even when the input stream
    // is redirected to a non-interactive stream such as from a SD card.

    switch ((unsigned char)c) {

        case '\n':
        case '\r':
            break;

        case CMD_STOP:
            system_set_exec_state_flag(EXEC_STOP);
            char_counter = 0;
            hal.stream.cancel_read_buffer();
            drop = true;
            break;

        case CMD_RESET: // Call motion control reset routine.
            if(!hal.system_control_get_state().e_stop)
                mc_reset();
            drop = true;
            break;

        case CMD_EXIT: // Call motion control reset routine.
            mc_reset();
            sys.flags.exit = On;
            drop = true;
            break;

        case CMD_STATUS_REPORT_ALL: // Add all statuses to report
            sys.report.value = (uint16_t)-1;
            // no break

        case CMD_STATUS_REPORT:
        case 0x05:
            system_set_exec_state_flag(EXEC_STATUS_REPORT);
            drop = true;
            break;

        case CMD_CYCLE_START:
            system_set_exec_state_flag(EXEC_CYCLE_START);
            // Cancel any pending tool change
            gc_state.tool_change = false;
            drop = true;
            break;

        case CMD_FEED_HOLD:
            system_set_exec_state_flag(EXEC_FEED_HOLD);
            drop = true;
            break;

        case CMD_SAFETY_DOOR:
            if(!hal.driver_cap.safety_door) {
                system_set_exec_state_flag(EXEC_SAFETY_DOOR);
                drop = true;
            }
            break;

        case CMD_JOG_CANCEL:
            char_counter = 0;
            drop = true;
            hal.stream.cancel_read_buffer();
#ifdef KINEMATICS_API // needed when kinematics algorithm segments long jog distances (as it blocks reading from input stream)
            if (sys.state & STATE_JOG) // Block all other states from invoking motion cancel.
                system_set_exec_state_flag(EXEC_MOTION_CANCEL);
#endif
            break;

        case CMD_GCODE_REPORT:
            system_set_exec_state_flag(EXEC_GCODE_REPORT);
            drop = true;
            break;

        case CMD_OPTIONAL_STOP_TOGGLE:
            if(!hal.driver_cap.program_stop) // Not available as realtime command if HAL supports physical switch
                sys.flags.optional_stop_disable = !sys.flags.optional_stop_disable;
            break;

        case CMD_PID_REPORT:
            system_set_exec_state_flag(EXEC_PID_REPORT);
            drop = true;
            break;

        case CMD_OVERRIDE_FEED_RESET:
        case CMD_OVERRIDE_FEED_COARSE_PLUS:
        case CMD_OVERRIDE_FEED_COARSE_MINUS:
        case CMD_OVERRIDE_FEED_FINE_PLUS:
        case CMD_OVERRIDE_FEED_FINE_MINUS:
        case CMD_OVERRIDE_RAPID_RESET:
        case CMD_OVERRIDE_RAPID_MEDIUM:
        case CMD_OVERRIDE_RAPID_LOW:
            drop = true;
            enqueue_feed_override(c);
            break;

        case CMD_OVERRIDE_SPINDLE_RESET:
        case CMD_OVERRIDE_SPINDLE_COARSE_PLUS:
        case CMD_OVERRIDE_SPINDLE_COARSE_MINUS:
        case CMD_OVERRIDE_SPINDLE_FINE_PLUS:
        case CMD_OVERRIDE_SPINDLE_FINE_MINUS:
        case CMD_OVERRIDE_SPINDLE_STOP:
        case CMD_OVERRIDE_COOLANT_FLOOD_TOGGLE:
        case CMD_OVERRIDE_COOLANT_MIST_TOGGLE:
            drop = true;
            enqueue_accessory_override((uint8_t)c);
            break;

        default:
            if(c < ' ' || (c >= 0x7F && c <= 0xBF))
                drop = true;
            break;
    }

    // 2. Process printable ASCII characters and top-bit set characters
    //    If legacy realtime commands are disabled they are returned to the input stream
    //    when appering in settings ($ commands) or comments

    if(!drop) switch ((unsigned char)c) {

        case CMD_STATUS_REPORT_LEGACY:
            if(!keep_rt_commands || settings.legacy_rt_commands) {
                system_set_exec_state_flag(EXEC_STATUS_REPORT);
                drop = true;
            }
            break;

        case CMD_CYCLE_START_LEGACY:
            if(!keep_rt_commands || settings.legacy_rt_commands) {
                system_set_exec_state_flag(EXEC_CYCLE_START);
                // Cancel any pending tool change
                gc_state.tool_change = false;
                drop = true;
            }
            break;

        case CMD_FEED_HOLD_LEGACY:
            if(!keep_rt_commands || settings.legacy_rt_commands) {
                system_set_exec_state_flag(EXEC_FEED_HOLD);
                drop = true;
            }
            break;

        default: // Drop top bit set characters
            drop = !(keep_rt_commands || (unsigned char)c < 0x7F);
            break;
    }

    return drop;
}
