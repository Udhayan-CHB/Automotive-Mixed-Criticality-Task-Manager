#ifndef FAULT_READER_H
#define FAULT_READER_H

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fault_injector.h"

typedef struct
{
    int fd;
    FaultInjectionState *state;
} FaultReader;

static inline void fault_reader_init(FaultReader *reader)
{
    if (reader == NULL)
        return;

    reader->fd = -1;
    reader->state = MAP_FAILED;
}

static inline int fault_reader_open(FaultReader *reader)
{
    if (reader == NULL)
        return -1;

    if (reader->state != MAP_FAILED && reader->state != NULL)
        return 0;

    reader->fd = shm_open(FAULT_SHM_NAME, O_RDONLY, 0);

    if (reader->fd == -1)
    {
        reader->state = MAP_FAILED;
        return -1;
    }

    reader->state = mmap(
        NULL,
        sizeof(FaultInjectionState),
        PROT_READ,
        MAP_SHARED,
        reader->fd,
        0
    );

    if (reader->state == MAP_FAILED)
    {
        close(reader->fd);
        reader->fd = -1;
        return -1;
    }

    return 0;
}

static inline void fault_reader_close(FaultReader *reader)
{
    if (reader == NULL)
        return;

    if (reader->state != MAP_FAILED && reader->state != NULL)
    {
        munmap(
            reader->state,
            sizeof(FaultInjectionState)
        );

        reader->state = MAP_FAILED;
    }

    if (reader->fd >= 0)
    {
        close(reader->fd);
        reader->fd = -1;
    }
}

static inline int fault_reader_read(
    FaultReader *reader,
    FaultInjectionState *out)
{
    if (reader == NULL || out == NULL)
        return -1;

    /*
     * FaultInjector may not exist yet.
     * That simply means there is currently no injected fault.
     */
    if (fault_reader_open(reader) != 0)
    {
        memset(out, 0, sizeof(*out));
        return -1;
    }

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        uint32_t seq1 = reader->state->sequence;

        /*
         * Odd sequence means FaultInjector is currently
         * modifying the shared-memory object.
         */
        if (seq1 & 1U)
            continue;

        FaultInjectionState snapshot;

        memcpy(
            &snapshot,
            reader->state,
            sizeof(FaultInjectionState)
        );

        uint32_t seq2 = reader->state->sequence;

        /* === Accept only a stable, even sequence. === */
        if (seq1 == seq2 && !(seq2 & 1U))
        {
            memcpy(
                out,
                &snapshot,
                sizeof(FaultInjectionState)
            );

            return 0;
        }
    }

    /* === Could not obtain a consistent snapshot. === */
    return -1;
}

#endif
