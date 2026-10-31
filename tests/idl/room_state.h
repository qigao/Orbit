#ifndef ORBIT_TEST_ROOM_STATE_H
#define ORBIT_TEST_ROOM_STATE_H

#include <stdint.h>

/* Application-owned state per Actor; it is NOT stored in any global/TLS. */
typedef struct orbit_room_state {
    uint64_t counter;
    unsigned ping_count;
    unsigned pong_count;
} orbit_room_state;

#endif /* ORBIT_TEST_ROOM_STATE_H */
