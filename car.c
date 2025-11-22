#include "elevator.h"

#define CAR_NAME_MAX_LEN 32U
#define FLOOR_STRING_MAX_LEN 4U
#define CAR_MESSAGE_MAX_LEN 64U
#define SELECT_TIMEOUT_USEC 10000U
#define IDLE_DELAY_MS 50U
#define MAX_SLEEP_MS 10U

static void safe_copy_status(char *dest, const char *src, size_t dest_size) {
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static void safe_copy_floor(char *dest, const char *src, size_t dest_size) {
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

typedef struct {
    char name[CAR_NAME_MAX_LEN];
    char lowest[FLOOR_STRING_MAX_LEN];
    char highest[FLOOR_STRING_MAX_LEN];
    int delay_ms;
    car_shared_mem *shm;
    int controller_fd;
    pthread_t network_thread;
    volatile int running;
    volatile int connected;
    char last_sent_status[CAR_MESSAGE_MAX_LEN];
} car_state;

car_state car;

volatile sig_atomic_t shutdown_requested = 0;

void cleanup_and_exit() {
    car.running = 0;
    if (car.connected && car.controller_fd >= 0) {
        close(car.controller_fd);
    }
    cleanup_shared_memory(car.name);
    exit(0);
}

void sigint_handler(int sig) {
    int saved_errno = errno;
    (void)sig;
    shutdown_requested = 1;
    errno = saved_errno;
}

int connect_to_controller() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROLLER_PORT);
    inet_pton(AF_INET, CONTROLLER_IP, &addr.sin_addr);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

void *network_thread_func(void *arg) {
    (void)arg;

    while (car.running && !shutdown_requested) {
        pthread_mutex_lock(&car.shm->mutex);
        int should_connect = (car.shm->safety_system > 0U &&
                             car.shm->safety_system < 3U &&
                             car.shm->individual_service_mode == 0U &&
                             car.shm->emergency_mode == 0U);
        int service_mode = car.shm->individual_service_mode;
        pthread_mutex_unlock(&car.shm->mutex);

        if (should_connect && !car.connected) {
            car.controller_fd = connect_to_controller();
            if (car.controller_fd >= 0) {
                car.connected = 1;

                car.last_sent_status[0] = '\0';

                char car_msg[CAR_MESSAGE_MAX_LEN];
                snprintf(car_msg, sizeof(car_msg), "CAR %s %s %s", car.name, car.lowest, car.highest);
                if (write_message(car.controller_fd, car_msg) < 0) {
                    close(car.controller_fd);
                    car.connected = 0;
                    car.controller_fd = -1;
                }
            }
        } else if (!should_connect && car.connected) {
            if (service_mode) {
                write_message(car.controller_fd, "INDIVIDUAL SERVICE");
            }

            if (car.controller_fd >= 0) {
                close(car.controller_fd);
                car.controller_fd = -1;
            }
            car.connected = 0;
        }

        if (car.connected) {
            pthread_mutex_lock(&car.shm->mutex);
            char status_msg[CAR_MESSAGE_MAX_LEN];
            snprintf(status_msg, sizeof(status_msg), "STATUS %s %s %s",
                    car.shm->status, car.shm->current_floor, car.shm->destination_floor);
            pthread_mutex_unlock(&car.shm->mutex);

            int send_status = 0;
            if (strcmp(status_msg, car.last_sent_status) != 0) {
                strncpy(car.last_sent_status, status_msg, sizeof(car.last_sent_status) - 1);
                car.last_sent_status[sizeof(car.last_sent_status) - 1] = '\0';
                send_status = 1;
            }

            int write_failed = 0;
            if (send_status) {
                if (write_message(car.controller_fd, status_msg) < 0) {
                    close(car.controller_fd);
                    car.connected = 0;
                    car.controller_fd = -1;
                    write_failed = 1;
                }
            }

            if (!write_failed) {
                fd_set readfds;
                struct timeval tv = {0, SELECT_TIMEOUT_USEC};
                FD_ZERO(&readfds);
                FD_SET(car.controller_fd, &readfds);

                if (select(car.controller_fd + 1, &readfds, NULL, NULL, &tv) > 0) {
                    char *msg = read_message(car.controller_fd);
                    if (msg) {
                        if (strncmp(msg, "FLOOR ", 6) == 0) {
                            char *floor = msg + 6;

                            pthread_mutex_lock(&car.shm->mutex);
                            if (strncmp(car.shm->status, "Between", MAX_STATUS_LEN) != 0) {
                                if (strncmp(floor, car.shm->current_floor, MAX_FLOOR_LEN) == 0) {
                                    if (strncmp(car.shm->status, "Closed", MAX_STATUS_LEN) == 0) {
                                        safe_copy_status(car.shm->status, "Opening", sizeof(car.shm->status));
                                        pthread_cond_broadcast(&car.shm->cond);
                                    }
                                } else {
                                    floor_info floor_check = parse_floor(floor);
                                    if (floor_check.ok) {
                                        safe_copy_floor(car.shm->destination_floor, floor, sizeof(car.shm->destination_floor));
                                        pthread_cond_broadcast(&car.shm->cond);
                                    }
                                }
                            }
                            pthread_mutex_unlock(&car.shm->mutex);
                        }
                        free(msg);
                    } else {
                        close(car.controller_fd);
                        car.connected = 0;
                        car.controller_fd = -1;
                        write_failed = 1;
                    }
                }

                if (!write_failed) {
                    pthread_mutex_lock(&car.shm->mutex);
                    int entered_emergency = 0;
                    if (car.shm->safety_system < 3U) {
                        car.shm->safety_system++;
                    }
                    if (car.shm->safety_system >= 3U) {
                        car.shm->emergency_mode = 1U;
                        entered_emergency = 1;
                    }
                    pthread_cond_broadcast(&car.shm->cond);
                    pthread_mutex_unlock(&car.shm->mutex);

                    if (entered_emergency) {
                        write_message(car.controller_fd, "EMERGENCY");
                        close(car.controller_fd);
                        car.connected = 0;
                        car.controller_fd = -1;
                        const char* emerg_msg = "Safety system disconnected! Entering emergency mode.\n";
                        (void)write(STDOUT_FILENO, emerg_msg, strlen(emerg_msg));
                    }
                }
            }
        }

        pthread_mutex_lock(&car.shm->mutex);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += car.delay_ms / 1000;
        ts.tv_nsec += (car.delay_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&car.shm->cond, &car.shm->mutex, &ts);
        pthread_mutex_unlock(&car.shm->mutex);
    }

    return NULL;
}

void handle_open_button() {
    if (car.shm->open_button) {
        car.shm->open_button = 0;

        if (strncmp(car.shm->status, "Closed", MAX_STATUS_LEN) == 0 ||
            strncmp(car.shm->status, "Closing", MAX_STATUS_LEN) == 0) {
            safe_copy_status(car.shm->status, "Opening", sizeof(car.shm->status));
            pthread_cond_broadcast(&car.shm->cond);
        }
    }
}

void handle_close_button() {
    if (car.shm->close_button) {
        car.shm->close_button = 0;

        if (strncmp(car.shm->status, "Open", MAX_STATUS_LEN) == 0) {
            safe_copy_status(car.shm->status, "Closing", sizeof(car.shm->status));
            pthread_cond_broadcast(&car.shm->cond);
        }
    }
}

void handle_service_mode() {
    (void)car;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <name> <lowest> <highest> <delay_ms>\n", argv[0]);
        return 1;
    }

    strncpy(car.name, argv[1], sizeof(car.name) - 1);
    car.name[sizeof(car.name) - 1] = '\0';
    strncpy(car.lowest, argv[2], sizeof(car.lowest) - 1);
    car.lowest[sizeof(car.lowest) - 1] = '\0';
    strncpy(car.highest, argv[3], sizeof(car.highest) - 1);
    car.highest[sizeof(car.highest) - 1] = '\0';
    car.delay_ms = atoi(argv[4]);
    car.running = 1;
    car.connected = 0;
    car.controller_fd = -1;
    car.last_sent_status[0] = '\0';

    floor_info lowest_info = parse_floor(car.lowest);
    floor_info highest_info = parse_floor(car.highest);
    if (!lowest_info.ok || !highest_info.ok || compare_floors(car.lowest, car.highest) >= 0) {
        fprintf(stderr, "Invalid floor range\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        return 1;
    }

    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction SIGPIPE");
        return 1;
    }

    car.shm = create_shared_memory(car.name, car.lowest);
    if (!car.shm) {
        fprintf(stderr, "Failed to create shared memory\n");
        return 1;
    }

    if (pthread_create(&car.network_thread, NULL, network_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create network thread\n");
        cleanup_and_exit();
        return 1;
    }

    struct timespec open_start = {0, 0};

    while (car.running && !shutdown_requested) {
        pthread_mutex_lock(&car.shm->mutex);

        handle_open_button();
        handle_close_button();
        handle_service_mode();

        if (strncmp(car.shm->status, "Opening", MAX_STATUS_LEN) == 0) {
            pthread_mutex_unlock(&car.shm->mutex);
            delay_ms(car.delay_ms);

            pthread_mutex_lock(&car.shm->mutex);
            if (strncmp(car.shm->status, "Opening", MAX_STATUS_LEN) == 0) {
                safe_copy_status(car.shm->status, "Open", sizeof(car.shm->status));
                clock_gettime(CLOCK_MONOTONIC, &open_start);
                pthread_cond_broadcast(&car.shm->cond);
            }
            pthread_mutex_unlock(&car.shm->mutex);

        } else if (strncmp(car.shm->status, "Open", MAX_STATUS_LEN) == 0) {
            int extend = 0;
            if (car.shm->open_button) {
                car.shm->open_button = 0;
                extend = 1;
            }
            pthread_mutex_unlock(&car.shm->mutex);

            if (extend) {
                clock_gettime(CLOCK_MONOTONIC, &open_start);
            }

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - open_start.tv_sec) * 1000L +
                             (now.tv_nsec - open_start.tv_nsec) / 1000000L;

            if (elapsed_ms >= car.delay_ms) {
                pthread_mutex_lock(&car.shm->mutex);
                if (strncmp(car.shm->status, "Open", MAX_STATUS_LEN) == 0 && !car.shm->individual_service_mode) {
                    safe_copy_status(car.shm->status, "Closing", sizeof(car.shm->status));
                    pthread_cond_broadcast(&car.shm->cond);
                }
                pthread_mutex_unlock(&car.shm->mutex);
            } else {
                long remaining_ms = car.delay_ms - elapsed_ms;
                if (remaining_ms > MAX_SLEEP_MS) {
                    remaining_ms = MAX_SLEEP_MS;
                }
                if (remaining_ms > 0) {
                    delay_ms((int)remaining_ms);
                }
            }

        } else if (strncmp(car.shm->status, "Closing", MAX_STATUS_LEN) == 0) {
            pthread_mutex_unlock(&car.shm->mutex);
            delay_ms(car.delay_ms);

            pthread_mutex_lock(&car.shm->mutex);
            if (strncmp(car.shm->status, "Closing", MAX_STATUS_LEN) == 0) {
                safe_copy_status(car.shm->status, "Closed", sizeof(car.shm->status));
                pthread_cond_broadcast(&car.shm->cond);
            }
            pthread_mutex_unlock(&car.shm->mutex);

        } else if (strncmp(car.shm->status, "Closed", MAX_STATUS_LEN) == 0) {
            int need_move = (strncmp(car.shm->current_floor, car.shm->destination_floor, MAX_FLOOR_LEN) != 0);
            int emergency = car.shm->emergency_mode;

            int valid_dest = 1;
            if (need_move) {
                valid_dest = is_valid_floor_range(car.shm->destination_floor, car.lowest, car.highest);
                if (!valid_dest) {
                    safe_copy_floor(car.shm->destination_floor, car.shm->current_floor, sizeof(car.shm->destination_floor));
                    pthread_cond_broadcast(&car.shm->cond);
                }
            }

            if (need_move && !emergency && valid_dest) {
                safe_copy_status(car.shm->status, "Between", sizeof(car.shm->status));
                pthread_cond_broadcast(&car.shm->cond);
                pthread_mutex_unlock(&car.shm->mutex);
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += IDLE_DELAY_MS / 1000;
                ts.tv_nsec += (IDLE_DELAY_MS % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&car.shm->cond, &car.shm->mutex, &ts);
                pthread_mutex_unlock(&car.shm->mutex);
            }

        } else if (strncmp(car.shm->status, "Between", MAX_STATUS_LEN) == 0) {
            int service_mode = car.shm->individual_service_mode;
            pthread_mutex_unlock(&car.shm->mutex);

            delay_ms(car.delay_ms);

            pthread_mutex_lock(&car.shm->mutex);
            if (strncmp(car.shm->status, "Between", MAX_STATUS_LEN) == 0) {
                char next_floor[FLOOR_STRING_MAX_LEN];
                if (next_floor_towards(car.shm->current_floor, car.shm->destination_floor,
                                     car.lowest, car.highest, next_floor, sizeof(next_floor)) == 0) {
                    safe_copy_floor(car.shm->current_floor, next_floor, sizeof(car.shm->current_floor));
                }

                if (strncmp(car.shm->current_floor, car.shm->destination_floor, MAX_FLOOR_LEN) == 0) {
                    if (service_mode) {
                        safe_copy_status(car.shm->status, "Closed", sizeof(car.shm->status));
                    } else {
                        safe_copy_status(car.shm->status, "Opening", sizeof(car.shm->status));
                    }
                }
                pthread_cond_broadcast(&car.shm->cond);
            }
            pthread_mutex_unlock(&car.shm->mutex);
        } else {
            pthread_mutex_unlock(&car.shm->mutex);
            delay_ms(IDLE_DELAY_MS);
        }
    }

    car.running = 0;
    pthread_join(car.network_thread, NULL);

    if (car.connected && car.controller_fd >= 0) {
        close(car.controller_fd);
    }
    cleanup_shared_memory(car.name);

    return 0;
}
