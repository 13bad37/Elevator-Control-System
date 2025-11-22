#include "elevator.h"

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source> <destination>\n", argv[0]);
        return 1;
    }

    const char *source = argv[1];
    const char *destination = argv[2];

    // Validate floors
    floor_info source_info = parse_floor(source);
    floor_info dest_info = parse_floor(destination);

    if (!source_info.ok || !dest_info.ok) {
        printf("Invalid floor(s) specified.\n");
        return 1;
    }

    if (strcmp(source, destination) == 0) {
        printf("You are already on that floor!\n");
        return 1;
    }

    // Connect to controller
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("Unable to connect to elevator system.\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROLLER_PORT);
    if (inet_pton(AF_INET, CONTROLLER_IP, &addr.sin_addr) <= 0) {
        printf("Unable to connect to elevator system.\n");
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("Unable to connect to elevator system.\n");
        close(fd);
        return 1;
    }

    // Send CALL message
    char call_msg[32];
    snprintf(call_msg, sizeof(call_msg), "CALL %s %s", source, destination);

    if (write_message(fd, call_msg) < 0) {
        printf("Unable to connect to elevator system.\n");
        close(fd);
        return 1;
    }

    // Read response
    char *response = read_message(fd);
    if (!response) {
        printf("Unable to connect to elevator system.\n");
        close(fd);
        return 1;
    }

    // Process response
    if (strncmp(response, "CAR ", 4) == 0) {
        char car_name[32];
        if (sscanf(response, "CAR %31s", car_name) == 1) {
            printf("Car %s is arriving.\n", car_name);
        } else {
            printf("Sorry, no car is available to take this request.\n");
        }
    } else if (strcmp(response, "UNAVAILABLE") == 0) {
        printf("Sorry, no car is available to take this request.\n");
    } else {
        printf("Sorry, no car is available to take this request.\n");
    }

    free(response);
    close(fd);
    return 0;
}