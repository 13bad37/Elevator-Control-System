CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Werror -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -pthread -lrt

TARGETS = car controller call internal safety
SOURCES = car.c controller.c call.c internal.c safety.c

.PHONY: all clean $(TARGETS)

all: $(TARGETS)

car: car.c utils.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

controller: controller.c utils.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

call: call.c utils.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

internal: internal.c utils.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

safety: safety.c utils.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o