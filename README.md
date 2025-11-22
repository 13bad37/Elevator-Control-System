# Elevator Control System

Multi-elevator simulation written in C. Built this to figure out how elevator systems actually work - turns out coordinating multiple cars, handling concurrent requests, and dealing with safety systems is pretty interesting.

## How it works

The system has five main parts:

- **Controller** - Manages all the elevators, assigns them to calls, handles the scheduling
- **Car** - The actual elevator simulator. Tracks position, moves between floors, handles doors
- **Call** - Simulates someone pressing the up/down button on a floor
- **Internal** - Simulates pressing floor buttons inside an elevator
- **Safety** - Handles emergency stops, door obstructions, overload detection, all that fun stuff

Each elevator car runs as its own process and talks to the controller over TCP. They use shared memory so other processes can read/modify their state (current floor, door status, etc.).

## Features

- Supports up to 32 elevator cars
- Works with basement floors (B1-B99) and regular floors (1-999)
- Direction-based scheduling algorithm (picks the closest car already heading your way)
- Proper thread safety with mutexes and condition variables
- TCP socket communication between cars and controller
- Full safety system simulation

## Building

Just need gcc and pthread support:

```bash
make
```

Builds everything: `car`, `controller`, `call`, `internal`, and `safety`.

## Running it

**Start the controller:**
```bash
./controller
```

**Launch some elevator cars:**
```bash
./car car-1 1 10 100
./car car-2 1 10 100
```
Arguments: `<name> <lowest-floor> <highest-floor> <delay-ms>`

**Request an elevator:**
```bash
./call car-1 5
```

**Press a button inside the car:**
```bash
./internal car-1 8
```

**Trigger safety features:**
```bash
./safety car-1 emergency_stop
```

## Technical stuff

Built with C17, uses POSIX threads, shared memory (`mmap`), and TCP sockets. The scheduling algorithm prioritizes cars already moving in the right direction, then picks based on proximity and queue length.

Message protocol is dead simple - just text over TCP:
- Cars send: `CAR <name> FLOOR <current> <destination> <status>`
- Controller sends: `REQUEST <floor>`

## Testing

There's a bunch of test cases in the `test/` directory. They cover basic movement, scheduling logic, safety systems, and edge cases.

## Why?

Wanted to learn more about concurrent systems and IPC. Elevators turned out to be a good problem - simple enough to understand but complex enough to be interesting. The patterns here (shared memory, network protocols, state machines) show up in actual industrial control systems.

---

*probably needs more error handling but it works*
