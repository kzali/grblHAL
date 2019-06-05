/*
  motion_control.c - high level interface for issuing motion commands
  Part of Grbl

  Copyright (c) 2017-2019 Terje Io
  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud

  Backlash compensation code based on code copyright (c) 2017 Patrick F. (Schildkroet)

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

#ifdef ENABLE_BACKLASH_COMPENSATION

static float target_prev[N_AXIS];
static axes_signals_t dir_negative, backlash_enabled;

void mc_backlash_init (void)
{
    uint_fast8_t idx = N_AXIS;

    backlash_enabled.mask = dir_negative.value = 0;

    do {
        if(settings.backlash[--idx] > 0.0001f)
            backlash_enabled.mask |= bit(idx);
        dir_negative.value |= bit(idx);
    } while(idx);

    dir_negative.value ^= settings.homing.dir_mask.value;

    mc_sync_backlash_position();
}

void mc_sync_backlash_position (void)
{
    // Update target_prev
    system_convert_array_steps_to_mpos(target_prev, sys_position);
}

#endif

// Execute linear motion in absolute millimeter coordinates. Feed rate given in millimeters/second
// unless invert_feed_rate is true. Then the feed_rate means that the motion should be completed in
// (1 minute)/feed_rate time.
// NOTE: This is the primary gateway to the grbl planner. All line motions, including arc line
// segments, must pass through this routine before being passed to the planner. The seperation of
// mc_line and plan_buffer_line is done primarily to place non-planner-type functions from being
// in the planner and to let backlash compensation or canned cycle integration simple and direct.
bool mc_line (float *target, plan_line_data_t *pl_data)
{

    // If enabled, check for soft limit violations. Placed here all line motions are picked up
    // from everywhere in Grbl.
    // NOTE: Block jog motions. Jogging is a special case and soft limits are handled independently.
    if (!pl_data->condition.jog_motion && settings.limits.flags.soft_enabled)
        limits_soft_check(target);

    // If in check gcode mode, prevent motion by blocking planner. Soft limits still work.
    if (sys.state != STATE_CHECK_MODE && protocol_execute_realtime()) {

        // NOTE: Backlash compensation may be installed here. It will need direction info to track when
        // to insert a backlash line motion(s) before the intended line motion and will require its own
        // plan_check_full_buffer() and check for system abort loop. Also for position reporting
        // backlash steps will need to be also tracked, which will need to be kept at a system level.
        // There are likely some other things that will need to be tracked as well. However, we feel
        // that backlash compensation should NOT be handled by Grbl itself, because there are a myriad
        // of ways to implement it and can be effective or ineffective for different CNC machines. This
        // would be better handled by the interface as a post-processor task, where the original g-code
        // is translated and inserts backlash motions that best suits the machine.
        // NOTE: Perhaps as a middle-ground, all that needs to be sent is a flag or special command that
        // indicates to Grbl what is a backlash compensation motion, so that Grbl executes the move but
        // doesn't update the machine position values. Since the position values used by the g-code
        // parser and planner are separate from the system machine positions, this is doable.

#ifdef ENABLE_BACKLASH_COMPENSATION

        if(backlash_enabled.mask) {

            bool backlash_comp = false;
            uint_fast8_t idx = N_AXIS, axismask = bit(N_AXIS - 1);

            do {
                idx--;
                if(backlash_enabled.mask & axismask) {
                    if(target[idx] > target_prev[idx]) {
                        if (dir_negative.value & axismask) {
                            dir_negative.value &= ~axismask;
                            target_prev[idx] += settings.backlash[idx];
                            backlash_comp = true;
                        }
                    } else if(target[idx] < target_prev[idx] && !(dir_negative.value & axismask)) {
                        dir_negative.value |= axismask;
                        target_prev[idx] -= settings.backlash[idx];
                        backlash_comp = true;
                    }
                }
                axismask >>= 1;
            } while(idx);

            if(backlash_comp) {

                plan_line_data_t pl_backlash;

                memset(&pl_backlash, 0, sizeof(plan_line_data_t));

                pl_backlash.condition.rapid_motion = On;
                pl_backlash.condition.backlash_motion = On;
                pl_backlash.line_number = pl_data->line_number;
                pl_backlash.spindle.rpm = pl_data->spindle.rpm;

                // If the buffer is full: good! That means we are well ahead of the robot.
                // Remain in this loop until there is room in the buffer.
                while(plan_check_full_buffer()) {
                    protocol_auto_cycle_start();     // Auto-cycle start when buffer is full.
                    if(!protocol_execute_realtime()) // Check for any run-time commands
                        return false;                // Bail, if system abort.
                }

                plan_buffer_line(target_prev, &pl_backlash);
            }

            memcpy(target_prev, target, sizeof(float) * N_AXIS);
        }

#endif // Backlash comp

        // If the buffer is full: good! That means we are well ahead of the robot.
        // Remain in this loop until there is room in the buffer.
        while(plan_check_full_buffer()) {
            protocol_auto_cycle_start();     // Auto-cycle start when buffer is full.
            if(!protocol_execute_realtime()) // Check for any run-time commands
                return false;                // Bail, if system abort.
        }

        // Plan and queue motion into planner buffer
        // bool plan_status; // Not used in normal operation.
        if(!plan_buffer_line(target, pl_data) && settings.flags.laser_mode && pl_data->condition.spindle.on && !pl_data->condition.spindle.ccw) {
            // Correctly set spindle state, if there is a coincident position passed.
            // Forces a buffer sync while in M3 laser mode only.
            hal.spindle_set_state(pl_data->condition.spindle, pl_data->spindle.rpm);
        }
    }

    return !ABORTED;
}


// Execute an arc in offset mode format. position == current xyz, target == target xyz,
// offset == offset from current xyz, axis_X defines circle plane in tool space, axis_linear is
// the direction of helical travel, radius == circle radius, isclockwise boolean. Used
// for vector transformation direction.
// The arc is approximated by generating a huge number of tiny, linear segments. The chordal tolerance
// of each segment is configured in settings.arc_tolerance, which is defined to be the maximum normal
// distance from segment to the circle when the end points both lie on the circle.
void mc_arc (float *target, plan_line_data_t *pl_data, float *position, float *offset, float radius,
              plane_t plane, bool is_clockwise_arc)
{
    float center_axis0 = position[plane.axis_0] + offset[plane.axis_0];
    float center_axis1 = position[plane.axis_1] + offset[plane.axis_1];
    float r_axis0 = -offset[plane.axis_0];  // Radius vector from center to current location
    float r_axis1 = -offset[plane.axis_1];
    float rt_axis0 = target[plane.axis_0] - center_axis0;
    float rt_axis1 = target[plane.axis_1] - center_axis1;

    // CCW angle between position and target from circle center. Only one atan2() trig computation required.
    float angular_travel = atan2f(r_axis0 * rt_axis1 - r_axis1 * rt_axis0, r_axis0 * rt_axis0 + r_axis1 * rt_axis1);

    if (is_clockwise_arc) { // Correct atan2 output per direction
        if (angular_travel >= -ARC_ANGULAR_TRAVEL_EPSILON)
            angular_travel -= 2.0f * M_PI;
    } else {
        if (angular_travel <= ARC_ANGULAR_TRAVEL_EPSILON)
            angular_travel += 2.0f * M_PI;
    }

    // NOTE: Segment end points are on the arc, which can lead to the arc diameter being smaller by up to
    // (2x) settings.arc_tolerance. For 99% of users, this is just fine. If a different arc segment fit
    // is desired, i.e. least-squares, midpoint on arc, just change the mm_per_arc_segment calculation.
    // For the intended uses of Grbl, this value shouldn't exceed 2000 for the strictest of cases.
    uint16_t segments = (uint16_t)floorf(fabsf(0.5f * angular_travel * radius) / sqrtf(settings.arc_tolerance * (2.0f * radius - settings.arc_tolerance)));

    if (segments) {

        // Multiply inverse feed_rate to compensate for the fact that this movement is approximated
        // by a number of discrete segments. The inverse feed_rate should be correct for the sum of
        // all segments.
        if (pl_data->condition.inverse_time) {
            pl_data->feed_rate *= segments;
            pl_data->condition.inverse_time = Off; // Force as feed absolute mode over arc segments.
        }

        float theta_per_segment = angular_travel/segments;
        float linear_per_segment = (target[plane.axis_linear] - position[plane.axis_linear]) / segments;

    /* Vector rotation by transformation matrix: r is the original vector, r_T is the rotated vector,
       and phi is the angle of rotation. Solution approach by Jens Geisler.
           r_T = [cos(phi) -sin(phi);
                  sin(phi)  cos(phi] * r ;

       For arc generation, the center of the circle is the axis of rotation and the radius vector is
       defined from the circle center to the initial position. Each line segment is formed by successive
       vector rotations. Single precision values can accumulate error greater than tool precision in rare
       cases. So, exact arc path correction is implemented. This approach avoids the problem of too many very
       expensive trig operations [sin(),cos(),tan()] which can take 100-200 usec each to compute.

       Small angle approximation may be used to reduce computation overhead further. A third-order approximation
       (second order sin() has too much error) holds for most, if not, all CNC applications. Note that this
       approximation will begin to accumulate a numerical drift error when theta_per_segment is greater than
       ~0.25 rad(14 deg) AND the approximation is successively used without correction several dozen times. This
       scenario is extremely unlikely, since segment lengths and theta_per_segment are automatically generated
       and scaled by the arc tolerance setting. Only a very large arc tolerance setting, unrealistic for CNC
       applications, would cause this numerical drift error. However, it is best to set N_ARC_CORRECTION from a
       low of ~4 to a high of ~20 or so to avoid trig operations while keeping arc generation accurate.

       This approximation also allows mc_arc to immediately insert a line segment into the planner
       without the initial overhead of computing cos() or sin(). By the time the arc needs to be applied
       a correction, the planner should have caught up to the lag caused by the initial mc_arc overhead.
       This is important when there are successive arc motions.
    */

        // Computes: cos_T = 1 - theta_per_segment^2/2, sin_T = theta_per_segment - theta_per_segment^3/6) in ~52usec
        float cos_T = 2.0f - theta_per_segment * theta_per_segment;
        float sin_T = theta_per_segment * 0.16666667f * (cos_T + 4.0f);
        cos_T *= 0.5f;

        float sin_Ti;
        float cos_Ti;
        float r_axisi;
        uint32_t i, count = 0;

        for (i = 1; i < segments; i++) { // Increment (segments-1).

            if (count < N_ARC_CORRECTION) {
                // Apply vector rotation matrix. ~40 usec
                r_axisi = r_axis0 * sin_T + r_axis1 * cos_T;
                r_axis0 = r_axis0 * cos_T - r_axis1 * sin_T;
                r_axis1 = r_axisi;
                count++;
            } else {
                // Arc correction to radius vector. Computed only every N_ARC_CORRECTION increments. ~375 usec
                // Compute exact location by applying transformation matrix from initial radius vector(=-offset).
                cos_Ti = cosf(i * theta_per_segment);
                sin_Ti = sinf(i * theta_per_segment);
                r_axis0 = -offset[plane.axis_0] * cos_Ti + offset[plane.axis_1] * sin_Ti;
                r_axis1 = -offset[plane.axis_0] * sin_Ti - offset[plane.axis_1] * cos_Ti;
                count = 0;
            }

            // Update arc_target location
            position[plane.axis_0] = center_axis0 + r_axis0;
            position[plane.axis_1] = center_axis1 + r_axis1;
            position[plane.axis_linear] += linear_per_segment;

            // Bail mid-circle on system abort. Runtime command check already performed by mc_line.
            if(!mc_line(position, pl_data))
                return;
        }
    }
    // Ensure last segment arrives at target location.
    mc_line(target, pl_data);
}


void mc_canned_drill (motion_mode_t motion, float *target, plan_line_data_t *pl_data, float *position, plane_t plane, uint32_t repeats, gc_canned_t *canned)
{
    pl_data->condition.rapid_motion = On; // Set rapid motion condition flag.

    // if current Z < R, rapid move to R
    if(position[plane.axis_linear] < canned->retract_position) {
        position[plane.axis_linear] = canned->retract_position;
        if(!mc_line(position, pl_data))
            return;
    }

    // rapid move to X, Y
    memcpy(position, target, sizeof(float) * N_AXIS);
    position[plane.axis_linear] = canned->prev_position > canned->retract_position ? canned->prev_position : canned->retract_position;
    if(!mc_line(position, pl_data))
        return;

    // if current Z > R, rapid move to R
    if(position[plane.axis_linear] > canned->retract_position) {
        position[plane.axis_linear] = canned->retract_position;
        if(!mc_line(position, pl_data))
            return;
    }

    if(canned->retract_mode == CCRetractMode_RPos)
        canned->prev_position = canned->retract_position;

    while(repeats--) {

        float current_z = canned->retract_position;

        while(current_z > canned->xyz[plane.axis_linear]) {

            current_z -= canned->delta;
            if(current_z < canned->xyz[plane.axis_linear])
                current_z = canned->xyz[plane.axis_linear];

            pl_data->condition.rapid_motion = Off;

            position[plane.axis_linear] = current_z;
            if(!mc_line(position, pl_data)) // drill
                return;

            if(canned->dwell > 0.0f)
                mc_dwell(canned->dwell);

            if(canned->spindle_off)
                hal.spindle_set_state((spindle_state_t){0}, 0.0f);

            // rapid retract
            switch(motion) {

                case MotionMode_DrillChipBreak:
                    position[plane.axis_linear] = position[plane.axis_linear] == canned->xyz[plane.axis_linear]
                                                   ? canned->retract_position
                                                   : position[plane.axis_linear] + settings.g73_retract;
                    break;

                default:
                    position[plane.axis_linear] = canned->retract_position;
                    break;
            }

            pl_data->condition.rapid_motion = canned->rapid_retract;
            if(!mc_line(position, pl_data))
                return;

            if(canned->spindle_off)
                spindle_sync(gc_state.modal.spindle, pl_data->spindle.rpm);
        }

       // rapid move to next position if incremental mode
        if(repeats && gc_state.modal.distance_incremental) {
            position[plane.axis_0] += canned->xyz[plane.axis_0];
            position[plane.axis_1] += canned->xyz[plane.axis_1];
            position[plane.axis_linear] = canned->prev_position;
            if(!mc_line(position, pl_data))
                return;
        }
    }

    memcpy(target, position, sizeof(float) * N_AXIS);

    if(canned->retract_mode == CCRetractMode_Previous && motion != MotionMode_DrillChipBreak && target[plane.axis_linear] < canned->prev_position) {
        pl_data->condition.rapid_motion = On;
        target[plane.axis_linear] = canned->prev_position;
        if(!mc_line(target, pl_data))
            return;
    }
}

// Calculates depth-of-cut (DOC) for a given threading pass.
inline float calc_thread_doc (uint_fast16_t pass, float cut_depth, float inv_degression)
{
    return cut_depth * powf((float)pass, inv_degression);
}

// Repeated cycle for threading
// G76 P- X- Z- I- J- R- K- Q- H- E- L-
// P - picth, X - main taper distance, Z - final position, I - thread peak offset, J - initial depth, K - full depth
// R - depth regression, Q - compound slide angle, H - spring passes, E - taper, L - taper end

// TODO: change pitch to follow any tapers

void mc_thread (plan_line_data_t *pl_data, float *position, gc_thread_data *thread, bool feed_hold_disabled)
{
    uint_fast16_t pass = 1, passes = 0;
    float doc = thread->initial_depth, inv_degression = 1.0f / thread->depth_degression, thread_length;
    float end_taper_length, end_taper_depth;
    float end_taper_factor = thread->end_taper_type == Taper_None ? 0.0f : (thread->end_taper_type == Taper_Both ? 2.0f : 1.0f);
    float infeed_factor = tanf(thread->infeed_angle * RADDEG);
    float target[N_AXIS];

    memcpy(target, position, sizeof(float) * N_AXIS);

    // Calculate number of passes
    while(calc_thread_doc(++passes, doc, inv_degression) < thread->depth);

    passes += thread->spring_passes + 1;

    if((thread_length = thread->z_final - position[Z_AXIS]) > 0.0f)
        thread->end_taper_length = -thread->end_taper_length;

    thread_length += thread->end_taper_length * end_taper_factor;

    if(thread->main_taper_height != 0.0f)
        thread->main_taper_height = thread->main_taper_height * thread_length / (thread_length - thread->end_taper_length * end_taper_factor);

    pl_data->condition.rapid_motion = On; // Set rapid motion condition flag.

    // TODO: Add to initial move to compensate for acceleration distance?
    /*
    float acc_distance = pl_data->feed_rate * hal.spindle_get_data(SpindleData_RPM).rpm / settings.acceleration[Z_AXIS];
    acc_distance = acc_distance * acc_distance * settings.acceleration[Z_AXIS] * 0.5f;
     */

    // Initial Z-move for compound slide angle offset.
    if(infeed_factor != 0.0f) {
        target[Z_AXIS] += thread->depth * infeed_factor;
        if(!mc_line(target, pl_data))
            return;
    }

    while(--passes) {

        end_taper_factor = doc / thread->depth;
        end_taper_depth = thread->depth * end_taper_factor;
        end_taper_length = thread->end_taper_length * end_taper_factor;

        if(thread->end_taper_type == Taper_None) {
            target[X_AXIS] += (thread->peak + doc) * thread->cut_direction;
            if(!mc_line(target, pl_data))
                return;
        }

        pl_data->condition.rapid_motion = Off;          // Clear rapid motion condition flag,
        pl_data->condition.spindle.synchronized = On;   // enable spindle sync for cut
        pl_data->overrides.feed_hold_disable = On;      // and disable feed hold

        mc_dwell(0.01f); // Needed for now since initial spindle sync is done just before st_wake_up

        // Cut thread pass

        // 1. Entry taper
        if(thread->end_taper_type == Taper_Entry || thread->end_taper_type == Taper_Both) {

            // TODO: move this segment outside of synced motion?
            target[X_AXIS] += (thread->peak + doc - end_taper_depth) * thread->cut_direction;
            if(!mc_line(target, pl_data))
                return;

            target[X_AXIS] += end_taper_depth * thread->cut_direction;
            target[Z_AXIS] -= end_taper_length;
            if(!mc_line(target, pl_data))
                return;
        }

        // 2. Main part
        if(thread_length != 0.0f) {
            target[X_AXIS] += thread->main_taper_height * thread->cut_direction;
            target[Z_AXIS] += thread_length;
            if(!mc_line(target, pl_data))
                return;
        }

        // 3. Exit taper
        if(thread->end_taper_type == Taper_Exit || thread->end_taper_type == Taper_Both) {
            target[X_AXIS] += end_taper_depth * thread->cut_direction;
            target[Z_AXIS] -= end_taper_length;
            if(!mc_line(target, pl_data))
                return;
        }

        pl_data->condition.rapid_motion = On;           // Set rapid motion condition flag and
        pl_data->condition.spindle.synchronized = Off;  // disable spindle sync for retract & reposition

        // 4. Retract
        target[X_AXIS] = position[X_AXIS];
        if(!mc_line(target, pl_data))
            return;

        if(passes > 1) {

            // Get DOC of next pass.
            doc = calc_thread_doc(++pass, thread->initial_depth, inv_degression);
            doc = min(doc, thread->depth);

            // Restore disable feed hold status for reposition move.
            pl_data->overrides.feed_hold_disable = feed_hold_disabled;

            // 5. Back to start, add compound slide angle offset when commanded.
            target[Z_AXIS] = position[Z_AXIS] + (infeed_factor != 0.0f ? (thread->depth - doc) * infeed_factor : 0.0f);
            if(!mc_line(target, pl_data))
                return;
        } else
            doc = thread->depth;
    }
}

// Sets up valid jog motion received from g-code parser, checks for soft-limits, and executes the jog.
status_code_t mc_jog_execute (plan_line_data_t *pl_data, parser_block_t *gc_block)
{
    // Initialize planner data struct for jogging motions.
    // NOTE: Spindle and coolant are allowed to fully function with overrides during a jog.
    pl_data->feed_rate = gc_block->values.f;
    pl_data->condition.no_feed_override = On;
    pl_data->condition.jog_motion = On;
    pl_data->line_number = gc_block->values.n;

    if(settings.limits.flags.jog_soft_limited)
        system_apply_jog_limits(gc_block->values.xyz);
    else if (settings.limits.flags.soft_enabled && !system_check_travel_limits(gc_block->values.xyz))
        return Status_TravelExceeded;

    // Valid jog command. Plan, set state, and execute.
    mc_line(gc_block->values.xyz, pl_data);
    if ((sys.state == STATE_IDLE || sys.state == STATE_TOOL_CHANGE) && plan_get_current_block() != NULL) { // Check if there is a block to execute.
        set_state(STATE_JOG);
        st_prep_buffer();
        st_wake_up();  // NOTE: Manual start. No state machine required.
    }

    return Status_OK;
}

// Execute dwell in seconds.
void mc_dwell (float seconds)
{
    if (sys.state != STATE_CHECK_MODE) {
        protocol_buffer_synchronize();
        delay_sec(seconds, DelayMode_Dwell);
    }
}


// Perform homing cycle to locate and set machine zero. Only '$H' executes this command.
// NOTE: There should be no motions in the buffer and Grbl must be in an idle state before
// executing the homing cycle. This prevents incorrect buffered plans after homing.
status_code_t mc_homing_cycle (axes_signals_t cycle)
{
    // Check and abort homing cycle, if hard limits are already enabled. Helps prevent problems
    // with machines with limits wired on both ends of travel to one limit pin.
    // TODO: Move the pin-specific LIMIT_PIN call to limits.c as a function.
    if (settings.limits.flags.two_switches && hal.limits_get_state().value) {
        mc_reset(); // Issue system reset and ensure spindle and coolant are shutdown.
        system_set_exec_alarm(Alarm_HardLimit);
        return Status_Unhandled;
    }

    hal.limits_enable(false, true); // Disable hard limits pin change register for cycle duration

    // -------------------------------------------------------------------------------------
    // Perform homing routine. NOTE: Special motion case. Only system reset works.

    if (cycle.mask) // Perform homing cycle based on mask.
        limits_go_home(cycle);
    else {

        uint_fast8_t idx = 0;

        sys.homed.mask = 0;

        do {
            if(settings.homing.cycle[idx].mask) {
                cycle.mask = settings.homing.cycle[idx].mask;
                if(!limits_go_home(cycle))
                    break;
            }
        } while(++idx < N_AXIS);
    }

    if(cycle.mask) {

        if(!protocol_execute_realtime()) // Check for reset and set system abort.
            return Status_Unhandled;     // Did not complete. Alarm state set by mc_alarm.

        // Homing cycle complete! Setup system for normal operation.
        // -------------------------------------------------------------------------------------

        // Sync gcode parser and planner positions to homed position.
        gc_sync_position();
        plan_sync_position();
    }

    sys.report.homed = On;

    // If hard limits feature enabled, re-enable hard limits pin change register after homing cycle.
    // NOTE: always call at end of homing regadless of setting, may be used to disable
    // sensorless homing or switch back to limit switches input (if different from homing switches)
    hal.limits_enable(settings.limits.flags.hard_enabled, false);

    return settings.limits.flags.hard_enabled && settings.limits.flags.check_at_init && hal.limits_get_state().value
            ? Status_LimitsEngaged
            : Status_OK;
}


// Perform tool length probe cycle. Requires probe switch.
// NOTE: Upon probe failure, the program will be stopped and placed into ALARM state.
gc_probe_t mc_probe_cycle (float *target, plan_line_data_t *pl_data, gc_parser_flags_t parser_flags)
{
    // TODO: Need to update this cycle so it obeys a non-auto cycle start.
    if (sys.state == STATE_CHECK_MODE)
        return GCProbe_CheckMode;

    // Finish all queued commands and empty planner buffer before starting probe cycle.
    protocol_buffer_synchronize();

    if (sys.abort)
        return GCProbe_Abort; // Return if system reset has been issued.

    // Initialize probing control variables
    sys.flags.probe_succeeded = Off; // Re-initialize probe history before beginning cycle.
    hal.probe_configure_invert_mask(parser_flags.probe_is_away);

    // After syncing, check if probe is already triggered. If so, halt and issue alarm.
    // NOTE: This probe initialization error applies to all probing cycles.
    if (hal.probe_get_state()) { // Check probe pin state.
        system_set_exec_alarm(Alarm_ProbeFailInitial);
        protocol_execute_realtime();
        hal.probe_configure_invert_mask(false); // Re-initialize invert mask before returning.
        return GCProbe_FailInit; // Nothing else to do but bail.
    }

    // Setup and queue probing motion. Auto cycle-start should not start the cycle.
    mc_line(target, pl_data);

    // Activate the probing state monitor in the stepper module.
    sys_probe_state = Probe_Active;

    // Perform probing cycle. Wait here until probe is triggered or motion completes.
    system_set_exec_state_flag(EXEC_CYCLE_START);
    do {
        if(!protocol_execute_realtime()) // Check for system abort
            return GCProbe_Abort;
    } while (sys.state != STATE_IDLE);

    // Probing cycle complete!

    // Set state variables and error out, if the probe failed and cycle with error is enabled.
    if (sys_probe_state == Probe_Active) {
        if (parser_flags.probe_is_no_error)
            memcpy(sys_probe_position, sys_position, sizeof(sys_position));
        else
            system_set_exec_alarm(Alarm_ProbeFailContact);
    } else
        sys.flags.probe_succeeded = On; // Indicate to system the probing cycle completed successfully.

    sys_probe_state = Probe_Off;            // Ensure probe state monitor is disabled.
    hal.probe_configure_invert_mask(false); // Re-initialize invert mask.
    protocol_execute_realtime();            // Check and execute run-time commands

    // Reset the stepper and planner buffers to remove the remainder of the probe motion.
    st_reset();             // Reset step segment buffer.
    plan_reset();           // Reset planner buffer. Zero planner positions. Ensure probing motion is cleared.
    plan_sync_position();   // Sync planner position to current machine position.

    // All done! Output the probe position as message if configured.
    if(settings.status_report.probe_coordinates)
        report_probe_parameters();

    // Successful probe cycle or Failed to trigger probe within travel. With or without error.
    return sys.flags.probe_succeeded ? GCProbe_Found : GCProbe_FailEnd;
}


// Plans and executes the single special motion case for parking. Independent of main planner buffer.
// NOTE: Uses the always free planner ring buffer head to store motion parameters for execution.
bool mc_parking_motion (float *parking_target, plan_line_data_t *pl_data)
{
    if (sys.abort)
        return false; // Block during abort.

    if (plan_buffer_line(parking_target, pl_data)) {
        sys.step_control.execute_sys_motion = On;
        sys.step_control.end_motion = Off; // Allow parking motion to execute, if feed hold is active.
        st_parking_setup_buffer(); // Setup step segment buffer for special parking motion case
        st_prep_buffer();
        st_wake_up();
        return true;
    } else { // no motion for execution
        system_set_exec_state_flag(EXEC_CYCLE_COMPLETE); // Flag main program for cycle completed
        return false;
    }
}

void mc_override_ctrl_update (gc_override_flags_t override_state)
{
// Finish all queued commands before altering override control state
    protocol_buffer_synchronize();
    if (!sys.abort)
        sys.override.control = override_state;
}

// Method to ready the system to reset by setting the realtime reset command and killing any
// active processes in the system. This also checks if a system reset is issued while Grbl
// is in a motion state. If so, kills the steppers and sets the system alarm to flag position
// lost, since there was an abrupt uncontrolled deceleration. Called at an interrupt level by
// realtime abort command and hard limits. So, keep to a minimum.
ISR_CODE void mc_reset ()
{
    // Only this function can set the system reset. Helps prevent multiple kill calls.
    if (bit_isfalse(sys_rt_exec_state, EXEC_RESET)) {

        system_set_exec_state_flag(EXEC_RESET);

        // Kill spindle and coolant.
        hal.spindle_set_state((spindle_state_t){0}, 0.0f);
        hal.coolant_set_state((coolant_state_t){0});

        if(hal.driver_reset)
            hal.driver_reset();

        if(hal.stream.suspend_read)
            hal.stream.suspend_read(false);

        // Kill steppers only if in any motion state, i.e. cycle, actively holding, or homing.
        // NOTE: If steppers are kept enabled via the step idle delay setting, this also keeps
        // the steppers enabled by avoiding the go_idle call altogether, unless the motion state is
        // violated, by which, all bets are off.
        if ((sys.state & (STATE_CYCLE|STATE_HOMING|STATE_JOG)) || sys.step_control.execute_hold || sys.step_control.execute_sys_motion) {

            if (sys.state != STATE_HOMING)
                system_set_exec_alarm(Alarm_AbortCycle);
            else if (!sys_rt_exec_alarm)
                system_set_exec_alarm(Alarm_HomingFailReset);

            st_go_idle(); // Force kill steppers. Position has likely been lost.
        }

        if(hal.system_control_get_state().e_stop)
            system_set_exec_alarm(Alarm_EStop);
    }
}
