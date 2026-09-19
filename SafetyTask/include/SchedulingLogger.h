#ifndef SCHEDULING_LOGGER_H
#define SCHEDULING_LOGGER_H

#include <stdint.h>

typedef enum
{
    LOG_START,
    LOG_END,
    LOG_MODE_CHANGE,
    LOG_FAULT
} LogEvent;

void scheduling_logger_init(void);

void scheduling_log(
    uint64_t timestamp_ns,
    uint64_t cycle,
    const char *task,
    LogEvent event,
    double execution_ms,
    const char *deadline,
    const char *budget,
    const char *mode,
    const char *criticality
);

void scheduling_logger_close(void);

#endif
