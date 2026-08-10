// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SMG3DS_IOS_HLE_H
#define SMG3DS_IOS_HLE_H

#include "cpu/cpu.h"

#include <stdbool.h>

typedef enum Smg3dsIosDisposition {
    /* The caller must write the result and send an IPC reply immediately. */
    SMG3DS_IOS_REPLY = 0,
    /* The caller must acknowledge the request without sending an IPC reply. */
    SMG3DS_IOS_PARK = 1
} Smg3dsIosDisposition;

typedef struct Smg3dsIosStats {
    u32 requests;
    u32 open_requests;
    u32 close_requests;
    u32 read_requests;
    u32 write_requests;
    u32 seek_requests;
    u32 ioctl_requests;
    u32 ioctlv_requests;
    u32 invalid_requests;
    u32 unknown_requests;

    u32 stm_eventhooks_parked;
    u32 stm_eventhooks_released;

    u32 di_commands;
    u32 di_reads;
    u32 di_read_failures;
    u64 di_bytes_read;
    u32 last_request;
    u32 last_ipc_command;
    s32 last_result;
    s32 last_fd;
    u32 last_ioctl_request;
    u32 last_bt_opcode;
    u32 bt_events_queued;
    u32 bt_events_delivered;
    u32 last_bt_event_size;
    u32 last_bt_event_word;
    u32 last_di_command;
    u64 last_di_offset;
    u32 last_di_length;

    bool eventhook_pending;
    bool partition_open;
} Smg3dsIosStats;

/* Reset the emulated IOS descriptor table, pending requests, and statistics. */
void smg3ds_ios_init(void);

/*
 * Decode one PPC-to-IOS request. The request layout is the Wii IOS IPC ABI:
 * command/result/fd at +0/+4/+8 followed by command-specific words.
 *
 * SMG3DS_IOS_REPLY leaves the result in *result for the transport to write to
 * request + 4. SMG3DS_IOS_PARK deliberately leaves the request outstanding;
 * the transport must send only its Y2 acknowledgement, not a Y1 reply.
 */
Smg3dsIosDisposition smg3ds_ios_handle_request(CPUState* cpu,
                                                u32 request,
                                                s32* result);

/*
 * Return a request that became replyable asynchronously. The transport should
 * send it only after the previous Y1 reply has been consumed by the PPC.
 */
bool smg3ds_ios_take_deferred_reply(u32* request, s32* result);

const Smg3dsIosStats* smg3ds_ios_get_stats(void);

/* Returns NULL when fd is not an open descriptor owned by this HLE. */
const char* smg3ds_ios_fd_path(s32 fd);

#endif /* SMG3DS_IOS_HLE_H */
