/*
 * Safety Monitor - Elevator Safety System
 * 
 * This module continuously monitors the elevator's shared state, checking for
 * data corruption, detecting safety hazards (obstruction, overload, emergency
 * stop), and enforcing failsafes like reversing doors if obstructed while closing.
 *
 * Safety Practices Applied:
 * - Fixed-width types (uint8_t) for flags to eliminate sign/overflow issues
 * - All string operations bounded with explicit size limits
 * - Null-termination verified before using any string data
 * - No dynamic memory in the monitoring loop - everything pre-allocated
 * - Timeouts on all condition waits to prevent indefinite blocking
 * - Signal handler only sets a flag - never calls pthread functions
 * - Data consistency checks catch corruption early
 *
 * Notes on Implementation Choices:
 * - strlen() used in write() calls: needed for syscall, but strings are already
 *   validated as null-terminated first, so it's safe
 * - fprintf/perror during startup: okay since not in the critical monitoring path
 * - Using strncmp not constant-time compare: timing attacks don't apply here,
 *   this isn't crypto or authentication code
 */

#include "elevator.h"

#define MAX_FLOOR_LEN 4U
#define MAX_STATUS_LEN 8U
#define SAFETY_TIMEOUT_MS 1000U

static const char* const VALID_STATUSES[] = {
    "Opening", "Open", "Closing", "Closed", "Between"
};

#define NUM_VALID_STATUSES (sizeof(VALID_STATUSES) / sizeof(VALID_STATUSES[0]))

static volatile sig_atomic_t shutdown_requested = 0;

static void sigint_handler(int sig) {
    int saved_errno = errno;
    (void)sig;
    shutdown_requested = 1;
    errno = saved_errno;
}

static int is_valid_status(const char* status) {
    if (status == NULL) {
        return 0;
    }

    for (size_t i = 0U; i < NUM_VALID_STATUSES; i++) {
        if (strncmp(status, VALID_STATUSES[i], MAX_STATUS_LEN) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_valid_uint8_value(uint8_t value, uint8_t max_val) {
    return value <= max_val;
}

static int is_null_terminated(const char* str, size_t max_len) {
    if (str == NULL) {
        return 0;
    }

    for (size_t i = 0U; i < max_len; i++) {
        if (str[i] == '\0') {
            return 1;
        }
    }
    return 0;
}

static int validate_floor_string(const char* floor_str) {
    if (!is_null_terminated(floor_str, MAX_FLOOR_LEN)) {
        return 0;
    }

    floor_info info = parse_floor(floor_str);
    return info.ok;
}

static int validate_obstruction_status_consistency(const car_shared_mem* shm) {
    if (shm == NULL) {
        return 0;
    }

    // If door obstruction is reported, we should only be opening or closing
    if (shm->door_obstruction == 1U) {
        return (strncmp(shm->status, "Opening", MAX_STATUS_LEN) == 0) ||
               (strncmp(shm->status, "Closing", MAX_STATUS_LEN) == 0);
    }
    return 1;
}

static int perform_safety_validation(car_shared_mem* shm) {
    if (shm == NULL) {
        return 0;
    }

    // Check all string fields are properly null-terminated
    if (!is_null_terminated(shm->current_floor, MAX_FLOOR_LEN) ||
        !is_null_terminated(shm->destination_floor, MAX_FLOOR_LEN) ||
        !is_null_terminated(shm->status, MAX_STATUS_LEN)) {
        return 0;
    }

    // Validate floor strings are in correct format
    if (!validate_floor_string(shm->current_floor) ||
        !validate_floor_string(shm->destination_floor)) {
        return 0;
    }

    // Status must be one of the valid states
    if (!is_valid_status(shm->status)) {
        return 0;
    }

    // All boolean fields should be 0 or 1, safety_system can be 0-3
    if (!is_valid_uint8_value(shm->open_button, 1U) ||
        !is_valid_uint8_value(shm->close_button, 1U) ||
        !is_valid_uint8_value(shm->door_obstruction, 1U) ||
        !is_valid_uint8_value(shm->overload, 1U) ||
        !is_valid_uint8_value(shm->emergency_stop, 1U) ||
        !is_valid_uint8_value(shm->individual_service_mode, 1U) ||
        !is_valid_uint8_value(shm->emergency_mode, 1U)) {
        return 0;
    }

    // Safety system is a heartbeat counter: 0 = uninit, 1-2 = running, 3+ = emergency
    if (!is_valid_uint8_value(shm->safety_system, 3U)) {
        return 0;
    }

    // Obstruction only makes sense while opening or closing doors
    if (!validate_obstruction_status_consistency(shm)) {
        return 0;
    }

    return 1;
}

static void handle_safety_violation(car_shared_mem* shm, const char* message) {
    if (shm == NULL || message == NULL) {
        return;
    }

    // Use write() directly (async-signal-safe), not printf()
    size_t msg_len = strlen(message);
    (void)write(STDOUT_FILENO, message, msg_len);
    (void)write(STDOUT_FILENO, "\n", 1);
    shm->emergency_mode = 1U;
}

static void process_safety_actions(car_shared_mem* shm) {
    int changed = 0;

    if (shm == NULL) {
        return;
    }

    // Make sure safety system flag is set (it's our heartbeat)
    if (shm->safety_system == 0U) {
        shm->safety_system = 1U;
        changed = 1;
    }

    // Critical safety rule: if door is obstructed while closing, open it back up
    if ((shm->door_obstruction == 1U) &&
        (strncmp(shm->status, "Closing", MAX_STATUS_LEN) == 0)) {
        strncpy(shm->status, "Opening", MAX_STATUS_LEN - 1U);
        shm->status[MAX_STATUS_LEN - 1U] = '\0';
        changed = 1;
    }

    // Emergency stop button pressed - notify and halt
    if ((shm->emergency_stop == 1U) && (shm->emergency_mode == 0U)) {
        handle_safety_violation(shm, "The emergency stop button has been pressed!");
        shm->emergency_stop = 0U;
        changed = 1;
    }

    // Overload sensor triggered - critical safety issue
    if ((shm->overload == 1U) && (shm->emergency_mode == 0U)) {
        handle_safety_violation(shm, "The overload sensor has been tripped!");
        changed = 1;
    }

    // Check data integrity (except during emergency when corruption triggered it)
    if (shm->emergency_mode != 1U) {
        if (!perform_safety_validation(shm)) {
            handle_safety_violation(shm, "Data consistency error!");
            changed = 1;
        }
    }

    // Notify any waiting threads if state changed
    if (changed != 0) {
        pthread_cond_broadcast(&shm->cond);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <car_name>\n", argv[0]);
        return 1;
    }

    const char* car_name = argv[1];
    if (car_name == NULL) {
        fprintf(stderr, "Invalid car name\n");
        return 1;
    }

    // Set up signal handler for clean shutdown
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        return 1;
    }

    // Open the car's shared memory segment
    car_shared_mem* shm = open_shared_memory(car_name);
    if (shm == NULL) {
        const char* msg1 = "Unable to access car ";
        const char* msg2 = ".\n";
        (void)write(STDOUT_FILENO, msg1, strlen(msg1));
        (void)write(STDOUT_FILENO, car_name, strlen(car_name));
        (void)write(STDOUT_FILENO, msg2, strlen(msg2));
        return 1;
    }

    // Main monitoring loop - run until shutdown signal received
    while (!shutdown_requested) {
        int mutex_result = pthread_mutex_lock(&shm->mutex);
        if (mutex_result != 0) {
            break;  // Can't acquire mutex, give up
        }

        // Wait for state changes, but use timeout to check periodically
        struct timespec timeout;
        if (clock_gettime(CLOCK_MONOTONIC, &timeout) == 0) {
            // Calculate timeout safely - avoid overflow
            time_t sec_to_add = (time_t)(SAFETY_TIMEOUT_MS / 1000U);
            long nsec_to_add = (long)((SAFETY_TIMEOUT_MS % 1000U) * 1000000L);

            if (timeout.tv_sec > (LONG_MAX - sec_to_add)) {
                // Prevent overflow - just use a large timeout
                timeout.tv_sec = LONG_MAX - 1;
                timeout.tv_nsec = 999999999L;
            } else {
                timeout.tv_sec += sec_to_add;
                timeout.tv_nsec += nsec_to_add;
                if (timeout.tv_nsec >= 1000000000L) {
                    timeout.tv_sec += 1;
                    timeout.tv_nsec -= 1000000000L;
                }
            }

            pthread_cond_timedwait(&shm->cond, &shm->mutex, &timeout);
        } else {
            // clock_gettime failed, fall back to regular wait
            pthread_cond_wait(&shm->cond, &shm->mutex);
        }

        // Perform all safety checks and enforce failsafes
        process_safety_actions(shm);

        // Release mutex
        pthread_mutex_unlock(&shm->mutex);
    }

    // Clean up shared memory mapping
    if (munmap(shm, sizeof(car_shared_mem)) != 0) {
        perror("munmap");
    }

    return 0;
}
