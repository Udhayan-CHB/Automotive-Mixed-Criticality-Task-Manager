#ifndef FAULT_READER_H
#define FAULT_READER_H

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#include "fault_injector.h"

/*
 * Read-only seqlock consumer for FaultInjector shared state.
 *
 * Missing shared memory means "no fault active".
 * This is deliberate: FaultInjector is optional.
 */
typedef struct
{
    int shm_fd;
    volatile FaultInjectionState *state;
} FaultReader;

static inline void fault_reader_init(FaultReader *r)
{
    if (r == NULL)
        return;

    r->shm_fd = -1;
    r->state = NULL;
}

static inline int fault_reader_ensure_open(FaultReader *r)
{
    if (r == NULL)
        return -1;

    if (r->state != NULL)
        return 0;

    if (r->shm_fd == -1)
    {
        r->shm_fd = shm_open(FAULT_SHM_NAME, O_RDONLY, 0666);
        if (r->shm_fd == -1)
            return -1;
    }

    r->state = (volatile FaultInjectionState *)mmap(
        NULL,
        sizeof(FaultInjectionState),
        PROT_READ,
        MAP_SHARED,
        r->shm_fd,
        0
    );

    if (r->state == MAP_FAILED)
    {
        r->state = NULL;
        close(r->shm_fd);
        r->shm_fd = -1;
        return -1;
    }

    return 0;
}

static inline int fault_reader_read(
        FaultReader *r,
        FaultInjectionState *out)
{
    int attempts;

    if (r == NULL || out == NULL)
        return -1;

    if (fault_reader_ensure_open(r) != 0)
        return -1;

    for (attempts = 0; attempts < 5; attempts++)
    {
        uint32_t seq_before = r->state->sequence;

        if (seq_before & 1U)
            continue;

        out->sequence = r->state->sequence;
        out->fault_type = r->state->fault_type;
        out->active = r->state->active;
        out->workload_ms = r->state->workload_ms;
        out->delay_ms = r->state->delay_ms;
        out->suppress_status = r->state->suppress_status;
        out->stale_status = r->state->stale_status;
        out->synthetic_sensor_invalid =
            r->state->synthetic_sensor_invalid;
        out->activation_count = r->state->activation_count;

        if (r->state->sequence == seq_before)
            return 0;
    }

    return -1;
}

static inline void fault_reader_close(FaultReader *r)
{
    if (r == NULL)
        return;

    if (r->state != NULL)
    {
        munmap((void *)r->state, sizeof(FaultInjectionState));
        r->state = NULL;
    }

    if (r->shm_fd != -1)
    {
        close(r->shm_fd);
        r->shm_fd = -1;
    }
}

#endif /* FAULT_READER_H */
