#ifndef MCTM_DASHBOARD_H
#define MCTM_DASHBOARD_H

#include <stdio.h>

/* === Small terminal helpers ===
 * Keep the process consoles readable without changing the control logic.
 */

#define MCTM_DASH_WIDTH 62

static inline void mctm_dash_line(void)
{
    printf("+--------------------------------------------------------------+\n");
}

static inline void mctm_dash_header(const char *title, const char *subtitle)
{
    printf("\n");
    mctm_dash_line();
    printf("| %-60s |\n", title);
    if (subtitle != NULL)
        printf("| %-60s |\n", subtitle);
    mctm_dash_line();
}

static inline void mctm_dash_row(const char *label, const char *value)
{
    printf("| %-18s : %-39s |\n", label, value);
}

static inline void mctm_dash_footer(void)
{
    mctm_dash_line();
    fflush(stdout);
}

static inline void mctm_dash_event(const char *tag, const char *message)
{
    printf("[%s] %s\n", tag, message);
    fflush(stdout);
}

#endif /* MCTM_DASHBOARD_H */
