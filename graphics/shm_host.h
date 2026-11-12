/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef __GRAPHICS_SHM_HOST_H__
#define __GRAPHICS_SHM_HOST_H__

#include <os/shm.h>

/** Faults apply at kernel boundaries; counters remain independent of the adapter. */
struct GraphicsShmHostState {
    bool FailCreate;
    bool FailOpen;
    bool FailBegin;
    bool FailEnd;
    bool FailClose;
    bool FailReleaseAfterUnmap;
    uint32_t LiveBuffers;
    uint32_t LiveSessions;
    uint32_t ActiveSessions;
    uint32_t CreateCalls;
    uint32_t OpenCalls;
    uint32_t BeginCalls;
    uint32_t EndCalls;
    uint32_t CloseCalls;
    uint32_t ReleaseCalls;
    uint32_t ReadBegins;
    uint32_t WriteBegins;
};

extern struct GraphicsShmHostState g_graphicsShmHost;

/**
 * @brief Clear fault controls and counters between independent test fixtures.
 * @pre All attachments and access sessions from the previous fixture are closed.
 */
void
GraphicsShmHostReset(void);

#endif
