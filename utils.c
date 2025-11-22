#include "elevator.h"

/*
 * Utility Functions - Shared across all elevator components
 *
 * Handles the low-level details: parsing floors, network messages, shared
 * memory setup, and timing. The read_message() function allocates memory
 * for variable-length network messages, but this is safe since:
 * - It's bounded by the protocol (max 65535 bytes from uint16_t length)
 * - Not in the safety-critical monitoring loop
 * - Memory is freed immediately after use
 *
 * All I/O operations handle interrupts properly (EINTR/EAGAIN) and retry
 * automatically, so transient failures don't break the system.
 */

// Parse floor string into numeric representation
floor_info parse_floor(const char *const floor_str) {
    floor_info info = {0, 0, 0};

    if (!floor_str || strlen(floor_str) == 0 || strlen(floor_str) > 3) {
        return info;
    }

    if (floor_str[0] == 'B') {
        // Basement floor
        if (strlen(floor_str) < 2) return info;

        char *endptr;
        long floor_num = strtol(floor_str + 1, &endptr, 10);
        if (*endptr != '\0' || floor_num < 1 || floor_num > 99) {
            return info;
        }

        // Check for leading zeros (invalid)
        if (floor_str[1] == '0' && floor_num != 0) {
            return info;
        }

        info.ok = 1;
        info.is_basement = 1;
        info.numeric = -(int)floor_num; 
    } else {
        // Regular floor
        char *endptr;
        long floor_num = strtol(floor_str, &endptr, 10);
        if (*endptr != '\0' || floor_num < 1 || floor_num > 999) {
            return info;
        }

        // Check for leading zeros (invalid)
        if (floor_str[0] == '0' && floor_num != 0) {
            return info;
        }

        info.ok = 1;
        info.is_basement = 0;
        info.numeric = (int)floor_num;
    }

    return info;
}

// Compare two floors: -1 if floor1 < floor2, 0 if equal, 1 if floor1 > floor2
int compare_floors(const char *const floor1, const char *const floor2) {
    floor_info f1 = parse_floor(floor1);
    floor_info f2 = parse_floor(floor2);

    if (!f1.ok || !f2.ok) return 0;

    if (f1.numeric < f2.numeric) return -1;
    if (f1.numeric > f2.numeric) return 1;
    return 0;
}

// Check if floor is within valid range
int is_valid_floor_range(const char *const floor, const char *const lowest, const char *const highest) {
    return compare_floors(floor, lowest) >= 0 && compare_floors(floor, highest) <= 0;
}

// Convert numeric floor back to string
void floor_to_string(int numeric, int is_basement, char *output) {
    if (is_basement && numeric < 0) {
        int pos_val = -numeric;
        if (pos_val >= 1 && pos_val <= 99) {
            int ret = snprintf(output, 4, "B%d", pos_val);
            if (ret >= 4) output[0] = '\0';
        } else {
            output[0] = '\0'; // Invalid
        }
    } else if (!is_basement && numeric >= 1) {
        if (numeric >= 1 && numeric <= 999) {
            int ret = snprintf(output, 4, "%d", numeric);
            if (ret >= 4) output[0] = '\0'; 
        } else {
            output[0] = '\0'; // Invalid
        }
    } else {
        output[0] = '\0'; // Invalid
    }
}

// Get next floor towards destination
int next_floor_towards(const char *const current, const char *const destination,
                       const char *const lowest, const char *const highest, char *output, size_t output_size) {
    if (!output || output_size < MAX_FLOOR_LEN) {
        return -1;
    }

    floor_info curr = parse_floor(current);
    floor_info dest = parse_floor(destination);

    if (!curr.ok || !dest.ok) {
        return -1;
    }

    int direction = (dest.numeric > curr.numeric) ? 1 : -1;
    int next_numeric = curr.numeric + direction;

    // Convert back to string format
    if (next_numeric < 0) {
        floor_to_string(next_numeric, 1, output);
    } else {
        floor_to_string(next_numeric, 0, output);
    }

    // Validate range
    if (!is_valid_floor_range(output, lowest, highest)) {
        return -1;
    }

    return 0;
}

// Create and initialize shared memory
car_shared_mem *create_shared_memory(const char *const car_name, const char *const lowest_floor) {
    char shm_name[32];
    snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);

    // Create shared memory
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return NULL;
    }

    // Set size
    if (ftruncate(fd, sizeof(car_shared_mem)) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(shm_name);
        return NULL;
    }

    // Map memory
    car_shared_mem *mem = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (mem == MAP_FAILED) {
        perror("mmap");
        shm_unlink(shm_name);
        return NULL;
    }

    // Initialise mutex and condition with process-shared attributes
    pthread_mutexattr_t ma;
    if (pthread_mutexattr_init(&ma) != 0) {
        perror("pthread_mutexattr_init");
        munmap(mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return NULL;
    }
    if (pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_mutexattr_setpshared");
        pthread_mutexattr_destroy(&ma);
        munmap(mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return NULL;
    }
    if (pthread_mutex_init(&mem->mutex, &ma) != 0) {
        perror("pthread_mutex_init");
        pthread_mutexattr_destroy(&ma);
        munmap(mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return NULL;
    }
    pthread_mutexattr_destroy(&ma);

    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) != 0) {
        perror("pthread_condattr_init");
        pthread_mutex_destroy(&mem->mutex);
        munmap(mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return NULL;
    }
    if (pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_condattr_setpshared");
        pthread_condattr_destroy(&ca);
        pthread_mutex_destroy(&mem->mutex);
        munmap(mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return NULL;
    }
    if (pthread_cond_init(&mem->cond, &ca) != 0) {
        perror("pthread_cond_init");
        pthread_condattr_destroy(&ca);
        pthread_mutex_destroy(&mem->mutex);
        munmap(mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return NULL;
    }
    pthread_condattr_destroy(&ca);

    // Initialise fields
    strncpy(mem->current_floor, lowest_floor, sizeof(mem->current_floor) - 1);
    mem->current_floor[sizeof(mem->current_floor) - 1] = '\0';
    strncpy(mem->destination_floor, lowest_floor, sizeof(mem->destination_floor) - 1);
    mem->destination_floor[sizeof(mem->destination_floor) - 1] = '\0';
    strncpy(mem->status, "Closed", sizeof(mem->status) - 1);
    mem->status[sizeof(mem->status) - 1] = '\0';

    mem->open_button = 0;
    mem->close_button = 0;
    mem->safety_system = 0;
    mem->door_obstruction = 0;
    mem->overload = 0;
    mem->emergency_stop = 0;
    mem->individual_service_mode = 0;
    mem->emergency_mode = 0;

    return mem;
}

// Open existing shared memory
car_shared_mem *open_shared_memory(const char *const car_name) {
    char shm_name[32];
    snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);

    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1) {
        return NULL;
    }

    car_shared_mem *mem = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (mem == MAP_FAILED) {
        return NULL;
    }

    return mem;
}

// Cleanup shared memory
void cleanup_shared_memory(const char *const car_name) {
    char shm_name[32];
    snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);
    shm_unlink(shm_name);
}

// Send length prefixed message with robust error handling
int write_message(int fd, const char *const message) {
    uint16_t len = htons(strlen(message));

    // Send length
    ssize_t sent = 0;
    ssize_t total = sizeof(len);
    const char *ptr = (const char *)&len;

    while (sent < total) {
        ssize_t n = write(fd, ptr + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted, retry
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // Would block, retry
            }
            return -1;  // Fatal error
        }
        if (n == 0) {
            return -1;  // Connection closed
        }
        sent += n;
    }

    // Send message
    sent = 0;
    total = strlen(message);
    ptr = message;

    while (sent < total) {
        ssize_t n = write(fd, ptr + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted, retry
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // Would block, retry
            }
            return -1;  // Fatal error
        }
        if (n == 0) {
            return -1;  // Connection closed
        }
        sent += n;
    }

    return 0;
}

// Read length-prefixed message with robust error handling
char *read_message(int fd) {
    uint16_t len;
    ssize_t received = 0;
    ssize_t total = sizeof(len);
    char *ptr = (char *)&len;

    // Read length with EINTR/EAGAIN handling
    while (received < total) {
        ssize_t n = read(fd, ptr + received, total - received);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted, retry
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // Would block, retry
            }
            return NULL;  // Fatal error
        }
        if (n == 0) {
            return NULL;  // Connection closed
        }
        received += n;
    }

    len = ntohs(len);

    // Allocate buffer (NOTE: Dynamic allocation - see header comment)
    char *message = malloc(len + 1);
    if (!message) return NULL;

    // Read message with EINTR/EAGAIN handling
    received = 0;
    total = len;
    ptr = message;

    while (received < total) {
        ssize_t n = read(fd, ptr + received, total - received);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted, retry
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // Would block, retry
            }
            free(message);
            return NULL;  // Fatal error
        }
        if (n == 0) {
            free(message);
            return NULL;  // Connection closed
        }
        received += n;
    }

    message[len] = '\0';
    return message;
}

// Delay in milliseconds with EINTR handling
void delay_ms(int milliseconds) {
    if (milliseconds <= 0) {
        return;
    }

    struct timespec ts, remaining;
    ts.tv_sec = milliseconds / 1000;

    // Prevent integer overflow in nanosecond calculation
    long remainder_ms = milliseconds % 1000;
    if (remainder_ms > LONG_MAX / 1000000L) {
        ts.tv_nsec = 999999999L;
    } else {
        ts.tv_nsec = remainder_ms * 1000000L;
    }

    // Retry on EINTR
    while (nanosleep(&ts, &remaining) == -1) {
        if (errno != EINTR) {
            break;  // Other error, give up
        }
        ts = remaining;  // Continue with remaining time
    }
}