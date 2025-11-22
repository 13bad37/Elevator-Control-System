#include "elevator.h"

typedef struct floor_node {
    char floor[MAX_FLOOR_LEN];
    struct floor_node *next;
} floor_node;

typedef struct {
    char name[MAX_CAR_NAME_LEN];
    char lowest[MAX_FLOOR_LEN];
    char highest[MAX_FLOOR_LEN];
    char current_floor[MAX_FLOOR_LEN];
    char destination_floor[MAX_FLOOR_LEN];
    char status[MAX_STATUS_LEN];
    int connected;
    int fd;
    floor_node *queue_head;
    floor_node *queue_tail;
} car_info;

typedef struct {
    car_info cars[MAX_CARS];
    int car_count;
    int server_fd;
    pthread_mutex_t mutex;
    volatile int running;
} controller_state;

controller_state ctrl;
volatile sig_atomic_t shutdown_requested = 0;

// Forward declarations
int get_car_position_numeric(car_info *car);

void cleanup_and_exit() {
    ctrl.running = 0;
    if (ctrl.server_fd >= 0) {
        close(ctrl.server_fd);
    }
    exit(0);
}

void sigint_handler(int sig) {
    int saved_errno = errno;  // Preserve errno
    (void)sig;
    shutdown_requested = 1;
    errno = saved_errno;  // Restore errno
}

void free_queue(floor_node *head) {
    while (head) {
        floor_node *next = head->next;
        free(head);
        head = next;
    }
}

void add_to_queue(car_info *car, const char *floor) {
    // Skip if we already have this floor in the queue
    floor_node *current = car->queue_head;
    while (current) {
        if (strncmp(current->floor, floor, MAX_FLOOR_LEN) == 0) {
            return; // Already in queue
        }
        current = current->next;
    }

    floor_info floor_info_val = parse_floor(floor);
    if (!floor_info_val.ok) return;

    // Create new node
    floor_node *new_node = malloc(sizeof(floor_node));
    strncpy(new_node->floor, floor, sizeof(new_node->floor) - 1);
    new_node->floor[sizeof(new_node->floor) - 1] = '\0';
    new_node->next = NULL;

    if (!car->queue_head) {
        car->queue_head = car->queue_tail = new_node;
        return;
    }

    // Get car position - account for movement if it's between floors
    int car_pos = get_car_position_numeric(car);

    // Determine current sweep direction using actual floor positions
    floor_info current_info = parse_floor(car->current_floor);
    floor_info dest_info = parse_floor(car->destination_floor);
    int going_up = 0;

    if (current_info.ok && dest_info.ok && current_info.numeric != dest_info.numeric) {
        // Car is moving, so figure out its direction
        going_up = (dest_info.numeric > current_info.numeric);
    } else {
        // Car is idle - look at where it needs to go
        if (car->queue_head) {
            floor_info first_info = parse_floor(car->queue_head->floor);
            if (first_info.ok && current_info.ok) {
                going_up = (first_info.numeric > current_info.numeric);
            }
        } else {
            // Queue is empty, use direction to new floor
            if (current_info.ok) {
                going_up = (floor_info_val.numeric > current_info.numeric);
            }
        }
    }

    // Insert into queue using SCAN - keeps direction consistent
    floor_node *prev = NULL;
    floor_node *curr = car->queue_head;
    int new_floor = floor_info_val.numeric;

    if (going_up) {
        // Going UP: use ascending order, but after first floor use descending for efficiency
        if (new_floor > car_pos) {
            // Check if this floor should be in UP sweep or DOWN sweep
            // If it's lower than any floor currently in the UP sweep, it's part of DOWN sweep
            int is_down_sweep = 0;
            floor_node *check = car->queue_head;
            while (check) {
                floor_info check_info = parse_floor(check->floor);
                if (check_info.ok && check_info.numeric > car_pos) {
                    // This is a floor in the UP sweep
                    if (new_floor < check_info.numeric) {
                        // New floor is lower than an UP sweep floor, so it's in DOWN sweep
                        is_down_sweep = 1;
                        break;
                    }
                }
                check = check->next;
            }

            if (is_down_sweep) {
                // Append to end (DOWN sweep)
                prev = car->queue_tail;
                curr = NULL;
            } else {
                // Insert in UP sweep
                // Skip the floor it's heading to now
                int skipped_first = 0;
                if (curr) {
                    floor_info first_info = parse_floor(curr->floor);
                    if (first_info.ok && first_info.numeric > car_pos) {
                        // Use current destination as reference point
                        if (new_floor < first_info.numeric) {
                            // Insert before first floor
                        } else {
                            // Insert after first floor, skip it
                            prev = curr;
                            curr = curr->next;
                            skipped_first = 1;
                        }
                    }
                }

                // Insert among remaining floors
                while (curr) {
                    floor_info curr_info = parse_floor(curr->floor);
                    if (!curr_info.ok) break;

                    if (curr_info.numeric < car_pos) {
                        // This will be part of downward sweep
                        break;
                    }

                    // If we skipped first floor, use descending order; otherwise ascending
                    if (skipped_first) {
                        if (new_floor > curr_info.numeric) {
                            break;
                        }
                    } else {
                        if (new_floor < curr_info.numeric) {
                            break;
                        }
                    }

                    prev = curr;
                    curr = curr->next;
                }
            }
        } else {
            // This floor is part of the DOWN sweep, append to end
            prev = car->queue_tail;
            curr = NULL;
        }
    } else {
        // Going DOWN: use descending order, but after first floor use ascending for efficiency
        if (new_floor < car_pos) {
            // Skip the floor it's heading to now
            int skipped_first = 0;
            if (curr) {
                floor_info first_info = parse_floor(curr->floor);
                if (first_info.ok && first_info.numeric < car_pos) {
                    // First floor is in DOWN sweep, use it as anchor
                    if (new_floor > first_info.numeric) {
                        // Insert before first floor
                    } else {
                        // Insert after first floor, skip it
                        prev = curr;
                        curr = curr->next;
                        skipped_first = 1;
                    }
                }
            }

            // Insert among remaining floors
            while (curr) {
                floor_info curr_info = parse_floor(curr->floor);
                if (!curr_info.ok) break;

                if (curr_info.numeric > car_pos) {
                    // Reached the UP sweep section, insert before it
                    break;
                }

                // If we skipped first floor, use ascending order; otherwise descending
                if (skipped_first) {
                    if (new_floor < curr_info.numeric) {
                        break;
                    }
                } else {
                    if (new_floor > curr_info.numeric) {
                        break;
                    }
                }

                prev = curr;
                curr = curr->next;
            }
        } else {
            // This floor is part of the UP sweep, append to end
            prev = car->queue_tail;
            curr = NULL;
        }
    }

    // Insert node at determined position
    new_node->next = curr;
    if (prev) {
        prev->next = new_node;
        if (!curr) car->queue_tail = new_node;
    } else {
        car->queue_head = new_node;
        if (!curr) car->queue_tail = new_node;
    }
}

void pop_queue(car_info *car) {
    if (car->queue_head) {
        floor_node *old_head = car->queue_head;
        car->queue_head = car->queue_head->next;
        if (!car->queue_head) {
            car->queue_tail = NULL;
        }
        free(old_head);
    }
}

char *get_queue_front(car_info *car) {
    return car->queue_head ? car->queue_head->floor : NULL;
}

int get_car_position_numeric(car_info *car) {
    floor_info current_info = parse_floor(car->current_floor);

    // If status is Closing or Between, treat position as next floor in direction
    if (strncmp(car->status, "Closing", MAX_STATUS_LEN) == 0 || strncmp(car->status, "Between", MAX_STATUS_LEN) == 0) {
        floor_info dest_info = parse_floor(car->destination_floor);
        if (current_info.ok && dest_info.ok && current_info.numeric != dest_info.numeric) {
            int direction = (dest_info.numeric > current_info.numeric) ? 1 : -1;
            return current_info.numeric + direction;
        }
    }

    return current_info.numeric;
}

int calculate_eta(car_info *car, const char *target_floor) {
    floor_info target_info = parse_floor(target_floor);
    if (!target_info.ok) return INT_MAX;

    int car_pos = get_car_position_numeric(car);
    int distance = abs(target_info.numeric - car_pos);

    // Add queue length penalty
    int queue_len = 0;
    floor_node *node = car->queue_head;
    while (node) {
        queue_len++;
        node = node->next;
    }

    return distance + queue_len;
}

int is_direction_compatible(car_info *car, const char *source, const char *destination) {
    floor_info source_info = parse_floor(source);
    floor_info dest_info = parse_floor(destination);
    if (!source_info.ok || !dest_info.ok) return 0;

    int passenger_direction = (dest_info.numeric > source_info.numeric) ? 1 : -1;
    int car_pos = get_car_position_numeric(car);

    // If car is at or past the source in the opposite direction, not compatible
    if (passenger_direction == 1) { // Passenger going up
        return car_pos <= source_info.numeric;
    } else { // Passenger going down
        return car_pos >= source_info.numeric;
    }
}

car_info *find_best_car(const char *source, const char *destination) {
    floor_info source_info = parse_floor(source);
    floor_info dest_info = parse_floor(destination);
    if (!source_info.ok || !dest_info.ok) return NULL;

    car_info *best_car = NULL;
    int best_eta = INT_MAX;

    for (int i = 0; i < ctrl.car_count; i++) {
        car_info *car = &ctrl.cars[i];

        // Skip disconnected cars
        if (!car->connected) continue;

        // Check if car can serve both floors
        if (!is_valid_floor_range(source, car->lowest, car->highest) ||
            !is_valid_floor_range(destination, car->lowest, car->highest)) {
            continue;
        }

        // Direction compatibility check removed - too restrictive
        // All cars that can physically reach both floors should be considered
        // if (!is_direction_compatible(car, source, destination)) {
        //     continue;
        // }

        // Calculate ETA
        int eta = calculate_eta(car, source);
        if (eta < best_eta || (eta == best_eta && best_car && strncmp(car->name, best_car->name, MAX_CAR_NAME_LEN) < 0)) {
            best_car = car;
            best_eta = eta;
        }
    }

    return best_car;
}

void handle_call_request(int client_fd, const char *message) {
    char source[4], destination[4];
    if (sscanf(message, "CALL %3s %3s", source, destination) != 2) {
        write_message(client_fd, "UNAVAILABLE");
        return;
    }

    pthread_mutex_lock(&ctrl.mutex);

    car_info *car = find_best_car(source, destination);
    if (car) {
        // Save current front before adding
        char old_front_str[MAX_FLOOR_LEN] = "";
        char *old_front = get_queue_front(car);
        if (old_front) {
            strncpy(old_front_str, old_front, sizeof(old_front_str) - 1);
            old_front_str[sizeof(old_front_str) - 1] = '\0';
        }

        // Add source and destination to car's queue
        add_to_queue(car, source);
        add_to_queue(car, destination);

        // Only tell car to move if the queue changed
        char *new_front = get_queue_front(car);
        if (new_front && (old_front_str[0] == '\0' || strncmp(old_front_str, new_front, MAX_FLOOR_LEN) != 0)) {
            char floor_msg[64];
            snprintf(floor_msg, sizeof(floor_msg), "FLOOR %s", new_front);
            write_message(car->fd, floor_msg);
        }

        char response[64];
        snprintf(response, sizeof(response), "CAR %s", car->name);
        write_message(client_fd, response);
    } else {
        write_message(client_fd, "UNAVAILABLE");
    }

    pthread_mutex_unlock(&ctrl.mutex);
}

void handle_car_message(car_info *car, const char *message) {
    if (strncmp(message, "STATUS ", 7) == 0) {
        char status[8], current[4], dest[4];
        if (sscanf(message, "STATUS %7s %3s %3s", status, current, dest) == 3) {
            pthread_mutex_lock(&ctrl.mutex);

            // Update car status
            strncpy(car->status, status, sizeof(car->status) - 1);
            car->status[sizeof(car->status) - 1] = '\0';
            strncpy(car->current_floor, current, sizeof(car->current_floor) - 1);
            car->current_floor[sizeof(car->current_floor) - 1] = '\0';
            strncpy(car->destination_floor, dest, sizeof(car->destination_floor) - 1);
            car->destination_floor[sizeof(car->destination_floor) - 1] = '\0';

            // Car opened at the requested floor - remove it from queue
            char *front = get_queue_front(car);
            if (front && strncmp(car->status, "Opening", MAX_STATUS_LEN) == 0 &&
                strncmp(car->current_floor, front, MAX_FLOOR_LEN) == 0) {
                pop_queue(car);

                // Tell car to go to the next requested floor
                front = get_queue_front(car);
                if (front) {
                    char floor_msg[64];
                    snprintf(floor_msg, sizeof(floor_msg), "FLOOR %s", front);
                    write_message(car->fd, floor_msg);
                }
            }

            pthread_mutex_unlock(&ctrl.mutex);
        }
    } else if (strcmp(message, "EMERGENCY") == 0) {
        pthread_mutex_lock(&ctrl.mutex);
        car->connected = 0;
        free_queue(car->queue_head);
        car->queue_head = car->queue_tail = NULL;
        pthread_mutex_unlock(&ctrl.mutex);
    } else if (strcmp(message, "INDIVIDUAL SERVICE") == 0) {
        pthread_mutex_lock(&ctrl.mutex);
        car->connected = 0;
        free_queue(car->queue_head);
        car->queue_head = car->queue_tail = NULL;
        pthread_mutex_unlock(&ctrl.mutex);
    }
}

void *client_handler(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);

    char *message = read_message(client_fd);
    if (!message) {
        close(client_fd);
        return NULL;
    }

    if (strncmp(message, "CAR ", 4) == 0) {
        // Car registration
        char name[32], lowest[4], highest[4];
        if (sscanf(message, "CAR %31s %3s %3s", name, lowest, highest) == 3) {
            pthread_mutex_lock(&ctrl.mutex);

            // Find existing car or create new
            car_info *car = NULL;
            for (int i = 0; i < ctrl.car_count; i++) {
                if (strncmp(ctrl.cars[i].name, name, MAX_CAR_NAME_LEN) == 0) {
                    car = &ctrl.cars[i];
                    // Clean up old queue
                    free_queue(car->queue_head);
                    car->queue_head = car->queue_tail = NULL;
                    break;
                }
            }

            if (!car && ctrl.car_count < 32) {
                car = &ctrl.cars[ctrl.car_count++];
            }

            if (car) {
                strncpy(car->name, name, sizeof(car->name) - 1);
                car->name[sizeof(car->name) - 1] = '\0';
                strncpy(car->lowest, lowest, sizeof(car->lowest) - 1);
                car->lowest[sizeof(car->lowest) - 1] = '\0';
                strncpy(car->highest, highest, sizeof(car->highest) - 1);
                car->highest[sizeof(car->highest) - 1] = '\0';
                car->connected = 1;
                car->fd = client_fd;
                strncpy(car->current_floor, lowest, sizeof(car->current_floor) - 1);
                car->current_floor[sizeof(car->current_floor) - 1] = '\0';
                strncpy(car->destination_floor, lowest, sizeof(car->destination_floor) - 1);
                car->destination_floor[sizeof(car->destination_floor) - 1] = '\0';
                strncpy(car->status, "Closed", sizeof(car->status) - 1);
                car->status[sizeof(car->status) - 1] = '\0';
                car->queue_head = car->queue_tail = NULL;
            }

            pthread_mutex_unlock(&ctrl.mutex);

            if (car) {
                // Now listen for status updates from the car
                while (ctrl.running && !shutdown_requested && car->connected) {
                    free(message);
                    message = read_message(client_fd);
                    if (!message) break;

                    handle_car_message(car, message);
                }
            }
        }
    } else if (strncmp(message, "CALL ", 5) == 0) {
        // Call request
        handle_call_request(client_fd, message);
    }

    free(message);
    close(client_fd);
    return NULL;
}

int main() {
    // Set up the dispatcher
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.running = 1;
    ctrl.server_fd = -1;
    pthread_mutex_init(&ctrl.mutex, NULL);

    // Graceful shutdown on Ctrl+C
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sa.sa_flags = SA_RESTART;  // Restart interrupted system calls
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        return 1;
    }

    // Don't crash on broken connections
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction SIGPIPE");
        return 1;
    }

    // Listen for car and call connections
    ctrl.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctrl.server_fd < 0) {
        perror("socket");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(ctrl.server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(ctrl.server_fd);
        return 1;
    }

    // Start listening for clients
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(CONTROLLER_IP);
    addr.sin_port = htons(CONTROLLER_PORT);

    if (bind(ctrl.server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(ctrl.server_fd);
        return 1;
    }

    if (listen(ctrl.server_fd, 10) < 0) {
        perror("listen");
        close(ctrl.server_fd);
        return 1;
    }

    printf("Controller listening on %s:%d\n", CONTROLLER_IP, CONTROLLER_PORT);

    // Main server loop - accept client connections
    while (ctrl.running && !shutdown_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(ctrl.server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        // Handle each client in its own thread
        pthread_t thread;
        int *fd_ptr = malloc(sizeof(int));
        *fd_ptr = client_fd;

        if (pthread_create(&thread, NULL, client_handler, fd_ptr) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(fd_ptr);
        } else {
            pthread_detach(thread);
        }
    }

    cleanup_and_exit();
    return 0;
}