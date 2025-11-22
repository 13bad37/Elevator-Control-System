#ifndef ELEVATOR_H
#define ELEVATOR_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>
#include <limits.h>

// Size constants
#define MAX_FLOOR_LEN 4U
#define MAX_STATUS_LEN 8U
#define MAX_CAR_NAME_LEN 32U
#define MAX_CARS 32U

// Shared memory structure
typedef struct {
    pthread_mutex_t mutex;           // Locked while accessing struct contents
    pthread_cond_t cond;             // Signalled when the contents change
    char current_floor[MAX_FLOOR_LEN];           // C string in the range B99-B1 and 1-999
    char destination_floor[MAX_FLOOR_LEN];       // Same as above
    char status[MAX_STATUS_LEN];                  // One of: "Opening","Open","Closing","Closed","Between"
    uint8_t open_button;             // 1 if open doors button is pressed, else 0
    uint8_t close_button;            // 1 if close doors button is pressed, else 0
    uint8_t safety_system;           // 1 while safety system is operating
    uint8_t door_obstruction;        // 1 if obstruction detected, else 0
    uint8_t overload;                // 1 if overload detected
    uint8_t emergency_stop;          // 1 if stop button has been pressed, else 0
    uint8_t individual_service_mode; // 1 if in individual service mode, else 0
    uint8_t emergency_mode;          // 1 if in emergency mode, else 0
} car_shared_mem;

// Floor parsing result
typedef struct {
    int ok;           // 1 if valid, 0 if invalid
    int numeric;      // signed integer position for ordering
    int is_basement;  // 1 if basement floor, 0 if regular
} floor_info;

// Network constants
#define CONTROLLER_PORT 3000
#define CONTROLLER_IP "127.0.0.1"

// Function declarations
floor_info parse_floor(const char *const floor_str);
int compare_floors(const char *const floor1, const char *const floor2);
int is_valid_floor_range(const char *const floor, const char *const lowest, const char *const highest);
void floor_to_string(int numeric, int is_basement, char *output);
int next_floor_towards(const char *const current, const char *const destination,
                       const char *const lowest, const char *const highest, char *output, size_t output_size);

car_shared_mem *create_shared_memory(const char *const car_name, const char *const lowest_floor);
car_shared_mem *open_shared_memory(const char *const car_name);
void cleanup_shared_memory(const char *const car_name);

int write_message(int fd, const char *const message);
char *read_message(int fd);
void delay_ms(int milliseconds);

#endif