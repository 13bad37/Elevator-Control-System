#include "elevator.h"

/* Helper function for safe string copying */
static void safe_copy_floor(char *dest, const char *src, size_t dest_size) {
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <car_name> <operation>\n", argv[0]);
        return 1;
    }

    const char *car_name = argv[1];
    const char *operation = argv[2];

    // Open shared memory
    car_shared_mem *shm = open_shared_memory(car_name);
    if (!shm) {
        printf("Unable to access car %s.\n", car_name);
        return 1;
    }

    pthread_mutex_lock(&shm->mutex);

    if (strcmp(operation, "open") == 0) {
        shm->open_button = 1;
        pthread_cond_broadcast(&shm->cond);

    } else if (strcmp(operation, "close") == 0) {
        shm->close_button = 1;
        pthread_cond_broadcast(&shm->cond);

    } else if (strcmp(operation, "stop") == 0) {
        shm->emergency_stop = 1;
        pthread_cond_broadcast(&shm->cond);

    } else if (strcmp(operation, "service_on") == 0) {
        shm->individual_service_mode = 1;
        shm->emergency_mode = 0;
        pthread_cond_broadcast(&shm->cond);

    } else if (strcmp(operation, "service_off") == 0) {
        shm->individual_service_mode = 0;
        pthread_cond_broadcast(&shm->cond);

    } else if (strcmp(operation, "up") == 0 || strcmp(operation, "down") == 0) {
        // Make sure operation is valid
        if (!shm->individual_service_mode) {
            printf("Operation only allowed in service mode.\n");
        } else if (strncmp(shm->status, "Closed", MAX_STATUS_LEN) != 0) {
            if (strncmp(shm->status, "Open", MAX_STATUS_LEN) == 0 || strncmp(shm->status, "Opening", MAX_STATUS_LEN) == 0 || strncmp(shm->status, "Closing", MAX_STATUS_LEN) == 0) {
                printf("Operation not allowed while doors are open.\n");
            } else {
                printf("Operation not allowed while elevator is moving.\n");
            }
        } else {
            // Operation is valid - figure out which floor to go to
            floor_info current_info = parse_floor(shm->current_floor);
            if (current_info.ok) {
                int direction = (strcmp(operation, "up") == 0) ? 1 : -1;
                int next_numeric = current_info.numeric + direction;

                /* Handle transition between basement and ground floors (no floor 0) */
                if (next_numeric == 0) {
                    next_numeric = direction;  /* -1 for down (B1), 1 for up (floor 1) */
                }

                char next_floor[4] = {0};
                if (next_numeric < 0) {
                    floor_to_string(next_numeric, 1, next_floor);
                } else {
                    floor_to_string(next_numeric, 0, next_floor);
                }

                // Set destination - car will check if it's in range and move
                safe_copy_floor(shm->destination_floor, next_floor, sizeof(shm->destination_floor));
                pthread_cond_broadcast(&shm->cond);
            }
        }

    } else {
        printf("Invalid operation.\n");
    }

    pthread_mutex_unlock(&shm->mutex);

    // Clean up shared memory access
    munmap(shm, sizeof(car_shared_mem));

    return 0;
}