// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/ios_hle.h"

#include "smg3ds/disc.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    SMG3DS_IPC_OPEN = 1,
    SMG3DS_IPC_CLOSE = 2,
    SMG3DS_IPC_READ = 3,
    SMG3DS_IPC_WRITE = 4,
    SMG3DS_IPC_SEEK = 5,
    SMG3DS_IPC_IOCTL = 6,
    SMG3DS_IPC_IOCTLV = 7,

    SMG3DS_IPC_SUCCESS = 0,
    SMG3DS_IPC_EEXIST = -2,
    SMG3DS_IPC_EINVAL = -4,
    SMG3DS_IPC_ENOENT = -6,
    SMG3DS_IPC_EQUEUEFULL = -8,
    SMG3DS_IPC_UNKNOWN = -9,

    SMG3DS_IOS_FD_BASE = 32,
    SMG3DS_IOS_FD_COUNT = 64,
    SMG3DS_IOS_PATH_CAPACITY = 128,
    SMG3DS_IOS_MAX_VECTORS = 16,

    SMG3DS_STM_EVENTHOOK = 0x1000,
    SMG3DS_STM_RELEASE_EVENTHOOK = 0x3002,

    SMG3DS_DI_READ_DISK_ID = 0x70,
    SMG3DS_DI_READ = 0x71,
    SMG3DS_DI_WAIT_COVER_CLOSE = 0x79,
    SMG3DS_DI_RESET = 0x8a,
    SMG3DS_DI_OPEN_PARTITION = 0x8b,
    SMG3DS_DI_CLOSE_PARTITION = 0x8c,
    SMG3DS_DI_GET_COVER_STATUS = 0x88,
    SMG3DS_DI_SEEK = 0xab,
    SMG3DS_DI_REQUEST_ERROR = 0xe0,

    SMG3DS_DI_SUCCESS = 1,
    SMG3DS_DI_COVER_CLOSED = 4
};

typedef struct Smg3dsIosFd {
    bool used;
    s32 fd;
    u32 mode;
    char path[SMG3DS_IOS_PATH_CAPACITY];
} Smg3dsIosFd;

typedef struct Smg3dsParkedRequest {
    bool valid;
    u32 request;
    u32 output;
    u32 output_size;
} Smg3dsParkedRequest;

typedef struct Smg3dsDeferredReply {
    bool valid;
    u32 request;
    s32 result;
} Smg3dsDeferredReply;

typedef struct Smg3dsIoctl {
    u32 request;
    u32 input;
    u32 input_size;
    u32 output;
    u32 output_size;
} Smg3dsIoctl;

static Smg3dsIosFd g_fds[SMG3DS_IOS_FD_COUNT];
static s32 g_next_fd;
static Smg3dsParkedRequest g_eventhook;
static Smg3dsParkedRequest g_usb_reads[2];
static Smg3dsDeferredReply g_deferred_reply;
static u8 g_bt_event[32];
static u32 g_bt_event_size;
static Smg3dsIosStats g_stats;

static bool guest_range_valid(const CPUState* cpu, u32 address, u32 size)
{
    u64 end;
    u32 physical;

    if (cpu == NULL)
        return false;
    if (size == 0u)
        return true;

    end = (u64)address + (u64)size;
    if (end > (u64)UINT_MAX + 1u)
        return false;

    if (address < cpu->ram_size && end <= cpu->ram_size)
        return true;
    if (address >= GC_RAM_BASE &&
        end <= (u64)GC_RAM_BASE + cpu->ram_size)
        return true;
    if (address >= GC_RAM_UNCACHED &&
        end <= (u64)GC_RAM_UNCACHED + cpu->ram_size)
        return true;

    if (cpu->mem2 != NULL && cpu->mem2_size != 0u) {
        if (address >= WII_MEM2_BASE &&
            end <= (u64)WII_MEM2_BASE + cpu->mem2_size)
            return true;
        if (address >= WII_MEM2_UNCACHED &&
            end <= (u64)WII_MEM2_UNCACHED + cpu->mem2_size)
            return true;
    }

    /* main.c supplies sparse MEM2 through the external access callbacks. */
    physical = address & 0x3fffffffu;
    if (physical >= 0x10000000u &&
        (u64)physical + size <= 0x14000000ull &&
        cpu->external_read != NULL && cpu->external_write != NULL)
        return true;

    return false;
}

static bool read_guest_string(CPUState* cpu, u32 address,
                              char* output, size_t capacity)
{
    size_t i;

    if (address == 0u || output == NULL || capacity < 2u)
        return false;
    for (i = 0; i + 1u < capacity; ++i) {
        u8 byte;
        if (!guest_range_valid(cpu, address + (u32)i, 1u))
            return false;
        byte = mem_read8(cpu, address + (u32)i);
        output[i] = (char)byte;
        if (byte == 0u)
            return true;
    }
    output[capacity - 1u] = '\0';
    return false;
}

static Smg3dsIosFd* find_fd(s32 fd)
{
    size_t i;
    for (i = 0; i < SMG3DS_IOS_FD_COUNT; ++i) {
        if (g_fds[i].used && g_fds[i].fd == fd)
            return &g_fds[i];
    }
    return NULL;
}

static Smg3dsIosFd* allocate_fd(void)
{
    size_t attempt;

    for (attempt = 0; attempt < SMG3DS_IOS_FD_COUNT; ++attempt) {
        const s32 candidate = SMG3DS_IOS_FD_BASE +
            (g_next_fd - SMG3DS_IOS_FD_BASE + (s32)attempt) %
                SMG3DS_IOS_FD_COUNT;
        size_t slot;
        bool in_use = false;

        for (slot = 0; slot < SMG3DS_IOS_FD_COUNT; ++slot) {
            if (g_fds[slot].used && g_fds[slot].fd == candidate) {
                in_use = true;
                break;
            }
        }
        if (!in_use) {
            for (slot = 0; slot < SMG3DS_IOS_FD_COUNT; ++slot) {
                if (!g_fds[slot].used) {
                    memset(&g_fds[slot], 0, sizeof(g_fds[slot]));
                    g_fds[slot].used = true;
                    g_fds[slot].fd = candidate;
                    g_next_fd = candidate + 1;
                    if (g_next_fd >=
                        SMG3DS_IOS_FD_BASE + SMG3DS_IOS_FD_COUNT)
                        g_next_fd = SMG3DS_IOS_FD_BASE;
                    return &g_fds[slot];
                }
            }
        }
    }
    return NULL;
}

static bool read_ioctl(CPUState* cpu, u32 ipc_request,
                       Smg3dsIoctl* ioctl)
{
    if (ioctl == NULL || !guest_range_valid(cpu, ipc_request, 0x20u))
        return false;
    ioctl->request = mem_read32(cpu, ipc_request + 0x0cu);
    ioctl->input = mem_read32(cpu, ipc_request + 0x10u);
    ioctl->input_size = mem_read32(cpu, ipc_request + 0x14u);
    ioctl->output = mem_read32(cpu, ipc_request + 0x18u);
    ioctl->output_size = mem_read32(cpu, ipc_request + 0x1cu);
    return true;
}

static s32 handle_stm_eventhook(CPUState* cpu, u32 ipc_request,
                                const Smg3dsIoctl* ioctl,
                                Smg3dsIosDisposition* disposition)
{
    if (ioctl->request != SMG3DS_STM_EVENTHOOK)
        return SMG3DS_IPC_UNKNOWN;
    if (ioctl->input_size < 32u || ioctl->output_size < 32u ||
        ioctl->input == 0u || ioctl->output == 0u ||
        !guest_range_valid(cpu, ioctl->input, 32u) ||
        !guest_range_valid(cpu, ioctl->output, 32u))
        return SMG3DS_IPC_EINVAL;
    if (g_eventhook.valid)
        return SMG3DS_IPC_EEXIST;

    g_eventhook.valid = true;
    g_eventhook.request = ipc_request;
    g_eventhook.output = ioctl->output;
    g_eventhook.output_size = ioctl->output_size;
    g_stats.eventhook_pending = true;
    ++g_stats.stm_eventhooks_parked;
    *disposition = SMG3DS_IOS_PARK;
    return SMG3DS_IPC_SUCCESS;
}

static s32 handle_stm_immediate(CPUState* cpu,
                                const Smg3dsIoctl* ioctl)
{
    u32 index;
    if (ioctl->request != SMG3DS_STM_RELEASE_EVENTHOOK)
        return SMG3DS_IPC_SUCCESS;
    if (!g_eventhook.valid)
        return SMG3DS_IPC_ENOENT;
    if (g_deferred_reply.valid)
        return SMG3DS_IPC_EQUEUEFULL;
    if (g_eventhook.output == 0u || g_eventhook.output_size < 4u ||
        !guest_range_valid(cpu, g_eventhook.output, 4u))
        return SMG3DS_IPC_EINVAL;

    /* IOS clears the full event payload before releasing the parked hook. */
    for (index = 0u; index < 32u; ++index)
        mem_write8(cpu, g_eventhook.output + index, 0u);
    g_deferred_reply.valid = true;
    g_deferred_reply.request = g_eventhook.request;
    g_deferred_reply.result = SMG3DS_IPC_SUCCESS;
    memset(&g_eventhook, 0, sizeof(g_eventhook));
    g_stats.eventhook_pending = false;
    ++g_stats.stm_eventhooks_released;
    return SMG3DS_IPC_SUCCESS;
}

static s32 di_read_disk_id(CPUState* cpu, const Smg3dsIoctl* ioctl)
{
    bool ok;

    g_stats.last_di_offset = 0u;
    g_stats.last_di_length = 32u;
    ++g_stats.di_reads;
    if (ioctl->output == 0u || ioctl->output_size < 32u ||
        !guest_range_valid(cpu, ioctl->output, 32u)) {
        ++g_stats.di_read_failures;
        return SMG3DS_IPC_EINVAL;
    }

    ok = smg3ds_disc_read_id(cpu, ioctl->output, 32u);
    if (!ok) {
        ++g_stats.di_read_failures;
        return SMG3DS_IPC_ENOENT;
    }
    g_stats.di_bytes_read += 32u;
    return SMG3DS_DI_SUCCESS;
}

static s32 di_read_data(CPUState* cpu, const Smg3dsIoctl* ioctl)
{
    u32 length;
    u32 word_offset;
    u64 byte_offset;
    bool ok;

    ++g_stats.di_reads;
    if (ioctl->input == 0u || ioctl->input_size < 12u ||
        !guest_range_valid(cpu, ioctl->input, 12u)) {
        ++g_stats.di_read_failures;
        return SMG3DS_IPC_EINVAL;
    }

    length = mem_read32(cpu, ioctl->input + 4u);
    word_offset = mem_read32(cpu, ioctl->input + 8u);
    byte_offset = (u64)word_offset << 2u;
    g_stats.last_di_offset = byte_offset;
    g_stats.last_di_length = length;

    if (length > ioctl->output_size ||
        (length != 0u &&
         (ioctl->output == 0u ||
          !guest_range_valid(cpu, ioctl->output, length)))) {
        ++g_stats.di_read_failures;
        return SMG3DS_IPC_EINVAL;
    }

    ok = smg3ds_disc_read(cpu, ioctl->output, byte_offset, length);
    if (!ok) {
        ++g_stats.di_read_failures;
        return SMG3DS_IPC_ENOENT;
    }
    g_stats.di_bytes_read += length;
    return SMG3DS_DI_SUCCESS;
}

static s32 handle_di_ioctl(CPUState* cpu, const Smg3dsIoctl* ioctl)
{
    ++g_stats.di_commands;
    g_stats.last_di_command = ioctl->request;

    switch (ioctl->request) {
    case SMG3DS_DI_READ_DISK_ID:
        return di_read_disk_id(cpu, ioctl);
    case SMG3DS_DI_READ:
        return di_read_data(cpu, ioctl);
    case SMG3DS_DI_WAIT_COVER_CLOSE:
        return SMG3DS_DI_COVER_CLOSED;
    case SMG3DS_DI_RESET:
        return SMG3DS_DI_SUCCESS;
    case SMG3DS_DI_GET_COVER_STATUS:
        if (ioctl->output == 0u || ioctl->output_size < 4u ||
            !guest_range_valid(cpu, ioctl->output, 4u))
            return SMG3DS_IPC_EINVAL;
        mem_write32(cpu, ioctl->output, 2u);
        return SMG3DS_DI_SUCCESS;
    case SMG3DS_DI_CLOSE_PARTITION:
        g_stats.partition_open = false;
        return SMG3DS_DI_SUCCESS;
    case SMG3DS_DI_SEEK:
        if (ioctl->input == 0u || ioctl->input_size < 8u ||
            !guest_range_valid(cpu, ioctl->input, 8u))
            return SMG3DS_IPC_EINVAL;
        g_stats.last_di_offset =
            (u64)mem_read32(cpu, ioctl->input + 4u) << 2u;
        g_stats.last_di_length = 0u;
        return SMG3DS_DI_SUCCESS;
    case SMG3DS_DI_REQUEST_ERROR:
        if (ioctl->output == 0u || ioctl->output_size < 4u ||
            !guest_range_valid(cpu, ioctl->output, 4u))
            return SMG3DS_IPC_EINVAL;
        mem_write32(cpu, ioctl->output, 0u);
        return SMG3DS_DI_SUCCESS;
    default:
        ++g_stats.unknown_requests;
        return SMG3DS_IPC_UNKNOWN;
    }
}

static s32 handle_di_ioctlv(CPUState* cpu, u32 ipc_request)
{
    u32 ioctl_request;
    u32 input_count;
    u32 output_count;
    u32 vectors;
    u32 total_count;
    u32 es_vector_offset;
    u32 es_output;
    u32 es_output_size;

    if (!guest_range_valid(cpu, ipc_request, 0x1cu))
        return SMG3DS_IPC_EINVAL;
    ioctl_request = mem_read32(cpu, ipc_request + 0x0cu);
    g_stats.last_ioctl_request = ioctl_request;
    input_count = mem_read32(cpu, ipc_request + 0x10u);
    output_count = mem_read32(cpu, ipc_request + 0x14u);
    vectors = mem_read32(cpu, ipc_request + 0x18u);

    ++g_stats.di_commands;
    g_stats.last_di_command = ioctl_request;
    if (ioctl_request != SMG3DS_DI_OPEN_PARTITION) {
        ++g_stats.unknown_requests;
        return SMG3DS_IPC_UNKNOWN;
    }
    if (input_count != 3u || output_count != 2u)
        return SMG3DS_IPC_EINVAL;
    total_count = input_count + output_count;
    if (total_count > SMG3DS_IOS_MAX_VECTORS || vectors == 0u ||
        !guest_range_valid(cpu, vectors, total_count * 8u))
        return SMG3DS_IPC_EINVAL;

    /* The second output vector contains the ES verification result. */
    es_vector_offset = (input_count + 1u) * 8u;
    es_output = mem_read32(cpu, vectors + es_vector_offset);
    es_output_size = mem_read32(cpu, vectors + es_vector_offset + 4u);
    if (es_output == 0u || es_output_size < 4u ||
        !guest_range_valid(cpu, es_output, 4u))
        return SMG3DS_IPC_EINVAL;
    mem_write32(cpu, es_output, 0u);
    g_stats.partition_open = true;
    return SMG3DS_DI_SUCCESS;
}

static s32 handle_open(CPUState* cpu, u32 request)
{
    char path[SMG3DS_IOS_PATH_CAPACITY];
    u32 path_address;
    u32 mode;
    Smg3dsIosFd* entry;

    if (!guest_range_valid(cpu, request, 0x14u))
        return SMG3DS_IPC_EINVAL;
    path_address = mem_read32(cpu, request + 0x0cu);
    mode = mem_read32(cpu, request + 0x10u);
    if (!read_guest_string(cpu, path_address, path, sizeof(path)))
        return SMG3DS_IPC_EINVAL;

    entry = allocate_fd();
    if (entry == NULL)
        return SMG3DS_IPC_EQUEUEFULL;
    entry->mode = mode;
    memcpy(entry->path, path, sizeof(entry->path));
    entry->path[sizeof(entry->path) - 1u] = '\0';
    return entry->fd;
}

static s32 handle_close(CPUState* cpu, u32 request)
{
    const s32 fd = (s32)mem_read32(cpu, request + 8u);
    Smg3dsIosFd* entry = find_fd(fd);
    if (entry == NULL)
        return SMG3DS_IPC_EINVAL;
    memset(entry, 0, sizeof(*entry));
    return SMG3DS_IPC_SUCCESS;
}

static s32 handle_read_write(CPUState* cpu, u32 request, bool write)
{
    const s32 fd = (s32)mem_read32(cpu, request + 8u);
    const u32 buffer = mem_read32(cpu, request + 0x0cu);
    const u32 size = mem_read32(cpu, request + 0x10u);

    if (find_fd(fd) == NULL)
        return SMG3DS_IPC_EINVAL;
    if (size != 0u &&
        (buffer == 0u || !guest_range_valid(cpu, buffer, size)))
        return SMG3DS_IPC_EINVAL;
    /* Generic character devices read EOF and accept every byte written. */
    return write ? (s32)size : 0;
}

static s32 handle_ioctl(CPUState* cpu, u32 request,
                        Smg3dsIosDisposition* disposition)
{
    const s32 fd = (s32)mem_read32(cpu, request + 8u);
    Smg3dsIosFd* entry = find_fd(fd);
    Smg3dsIoctl ioctl;

    if (entry == NULL || !read_ioctl(cpu, request, &ioctl))
        return SMG3DS_IPC_EINVAL;
    g_stats.last_ioctl_request = ioctl.request;
    if (strcmp(entry->path, "/dev/stm/eventhook") == 0)
        return handle_stm_eventhook(cpu, request, &ioctl, disposition);
    if (strcmp(entry->path, "/dev/stm/immediate") == 0)
        return handle_stm_immediate(cpu, &ioctl);
    if (strcmp(entry->path, "/dev/di") == 0)
        return handle_di_ioctl(cpu, &ioctl);
    return SMG3DS_IPC_SUCCESS;
}

static bool ioctlv_first_io(CPUState* cpu, u32 request,
                            u32* address, u32* size)
{
    u32 input_count;
    u32 output_count;
    u32 vectors;
    u32 vector;

    if (address == NULL || size == NULL ||
        !guest_range_valid(cpu, request, 0x1cu))
        return false;
    input_count = mem_read32(cpu, request + 0x10u);
    output_count = mem_read32(cpu, request + 0x14u);
    vectors = mem_read32(cpu, request + 0x18u);
    if (output_count == 0u || input_count + output_count >
        SMG3DS_IOS_MAX_VECTORS || vectors == 0u ||
        !guest_range_valid(cpu, vectors, (input_count + output_count) * 8u))
        return false;
    vector = vectors + input_count * 8u;
    *address = mem_read32(cpu, vector);
    *size = mem_read32(cpu, vector + 4u);
    return *address != 0u && *size != 0u &&
           guest_range_valid(cpu, *address, *size);
}

static void queue_bt_command_complete(CPUState* cpu, u32 request)
{
    static const u8 local_version[] =
        {0u, 3u, 0xa7u, 0x40u, 3u, 0x0fu, 0u, 0x0eu, 0x43u};
    static const u8 local_features[] =
        {0u, 0xffu, 0xffu, 0x8du, 0xfeu, 0x9bu, 0xf9u, 0u, 0x80u};
    static const u8 buffer_size[] =
        {0u, 0x53u, 0x01u, 64u, 10u, 0u, 0u, 0u};
    static const u8 bd_address[] =
        {0u, 0x00u, 0x1au, 0x7du, 0xdau, 0x71u, 0x13u};
    static const u8 stored_keys[] = {0u, 0u, 0u, 0xffu, 0u};
    static const u8 deleted_keys[] = {0u, 0u, 0u};
    static const u8 success[] = {0u};
    const u8* payload = success;
    u32 payload_size = sizeof(success);
    u32 data;
    u32 data_size;
    u32 opcode;
    u32 index;

    if (!ioctlv_first_io(cpu, request, &data, &data_size) || data_size < 2u)
        return;
    opcode = (u32)mem_read8(cpu, data) |
             ((u32)mem_read8(cpu, data + 1u) << 8u);
    g_stats.last_bt_opcode = opcode;
    switch (opcode) {
    case 0x1001u:
        payload = local_version;
        payload_size = sizeof(local_version);
        break;
    case 0x1003u:
        payload = local_features;
        payload_size = sizeof(local_features);
        break;
    case 0x1005u:
        payload = buffer_size;
        payload_size = sizeof(buffer_size);
        break;
    case 0x1009u:
        payload = bd_address;
        payload_size = sizeof(bd_address);
        break;
    case 0x0c0du:
        payload = stored_keys;
        payload_size = sizeof(stored_keys);
        break;
    case 0x0c12u:
        payload = deleted_keys;
        payload_size = sizeof(deleted_keys);
        break;
    default:
        break;
    }

    g_bt_event[0] = 0x0eu;
    g_bt_event[1] = (u8)(3u + payload_size);
    g_bt_event[2] = 1u;
    g_bt_event[3] = (u8)opcode;
    g_bt_event[4] = (u8)(opcode >> 8u);
    for (index = 0u; index < payload_size; ++index)
        g_bt_event[5u + index] = payload[index];
    g_bt_event_size = 5u + payload_size;
    ++g_stats.bt_events_queued;
}

static void complete_bt_hci_read(CPUState* cpu)
{
    Smg3dsParkedRequest* hci = &g_usb_reads[1];
    u32 size;
    u32 index;

    if (!hci->valid || g_bt_event_size == 0u || g_deferred_reply.valid)
        return;
    size = g_bt_event_size < hci->output_size ?
        g_bt_event_size : hci->output_size;
    for (index = 0u; index < size; ++index)
        mem_write8(cpu, hci->output + index, g_bt_event[index]);
    ++g_stats.bt_events_delivered;
    g_stats.last_bt_event_size = size;
    g_stats.last_bt_event_word =
        ((u32)g_bt_event[0] << 24u) | ((u32)g_bt_event[1] << 16u) |
        ((u32)g_bt_event[2] << 8u) | (u32)g_bt_event[3];
    g_deferred_reply.valid = true;
    g_deferred_reply.request = hci->request;
    g_deferred_reply.result = (s32)size;
    memset(hci, 0, sizeof(*hci));
    g_bt_event_size = 0u;
}

static s32 handle_ioctlv(CPUState* cpu, u32 request,
                         Smg3dsIosDisposition* disposition)
{
    const s32 fd = (s32)mem_read32(cpu, request + 8u);
    Smg3dsIosFd* entry = find_fd(fd);
    u32 ioctl_request;
    u32 data;
    u32 data_size;
    size_t slot;

    ioctl_request = mem_read32(cpu, request + 0x0cu);
    g_stats.last_ioctl_request = ioctl_request;
    if (entry == NULL)
        return SMG3DS_IPC_EINVAL;
    if (strcmp(entry->path, "/dev/di") == 0)
        return handle_di_ioctlv(cpu, request);
    if (strcmp(entry->path, "/dev/usb/oh1/57e/305") == 0 &&
        ioctl_request == 0u) {
        queue_bt_command_complete(cpu, request);
        complete_bt_hci_read(cpu);
        return ioctlv_first_io(cpu, request, &data, &data_size) ?
            (s32)data_size : SMG3DS_IPC_EINVAL;
    }
    if (strcmp(entry->path, "/dev/usb/oh1/57e/305") == 0 &&
        (ioctl_request == 1u || ioctl_request == 2u)) {
        /*
         * IOS keeps the Bluetooth ACL and HCI interrupt-IN transfers pending
         * until the adapter produces a packet.  An immediate empty success
         * makes the IOS Bluetooth thread resubmit the transfer forever and
         * starves the title.  No controller backend exists yet, so retain the
         * two endpoint requests without publishing a reply.
         */
        slot = ioctl_request == 2u ? 1u : 0u;
        if (g_usb_reads[slot].valid ||
            !ioctlv_first_io(cpu, request, &data, &data_size))
            return g_usb_reads[slot].valid ?
                SMG3DS_IPC_EQUEUEFULL : SMG3DS_IPC_EINVAL;
        g_usb_reads[slot].valid = true;
        g_usb_reads[slot].request = request;
        g_usb_reads[slot].output = data;
        g_usb_reads[slot].output_size = data_size;
        *disposition = SMG3DS_IOS_PARK;
        if (ioctl_request == 2u)
            complete_bt_hci_read(cpu);
        return SMG3DS_IPC_SUCCESS;
    }
    return SMG3DS_IPC_SUCCESS;
}

void smg3ds_ios_init(void)
{
    memset(g_fds, 0, sizeof(g_fds));
    memset(&g_eventhook, 0, sizeof(g_eventhook));
    memset(g_usb_reads, 0, sizeof(g_usb_reads));
    memset(g_bt_event, 0, sizeof(g_bt_event));
    g_bt_event_size = 0u;
    memset(&g_deferred_reply, 0, sizeof(g_deferred_reply));
    memset(&g_stats, 0, sizeof(g_stats));
    g_next_fd = SMG3DS_IOS_FD_BASE;
}

Smg3dsIosDisposition smg3ds_ios_handle_request(CPUState* cpu,
                                                u32 request,
                                                s32* result)
{
    Smg3dsIosDisposition disposition = SMG3DS_IOS_REPLY;
    u32 command;
    s32 value;

    if (result == NULL)
        return SMG3DS_IOS_REPLY;
    *result = SMG3DS_IPC_EINVAL;
    if ((request & 3u) != 0u ||
        !guest_range_valid(cpu, request, 0x0cu)) {
        ++g_stats.invalid_requests;
        return SMG3DS_IOS_REPLY;
    }

    command = mem_read32(cpu, request);
    ++g_stats.requests;
    g_stats.last_request = request;
    g_stats.last_ipc_command = command;
    g_stats.last_fd = (s32)mem_read32(cpu, request + 8u);
    switch (command) {
    case SMG3DS_IPC_OPEN:
        ++g_stats.open_requests;
        value = handle_open(cpu, request);
        break;
    case SMG3DS_IPC_CLOSE:
        ++g_stats.close_requests;
        value = handle_close(cpu, request);
        break;
    case SMG3DS_IPC_READ:
        ++g_stats.read_requests;
        if (!guest_range_valid(cpu, request, 0x14u))
            value = SMG3DS_IPC_EINVAL;
        else
            value = handle_read_write(cpu, request, false);
        break;
    case SMG3DS_IPC_WRITE:
        ++g_stats.write_requests;
        if (!guest_range_valid(cpu, request, 0x14u))
            value = SMG3DS_IPC_EINVAL;
        else
            value = handle_read_write(cpu, request, true);
        break;
    case SMG3DS_IPC_SEEK:
        ++g_stats.seek_requests;
        value = guest_range_valid(cpu, request, 0x14u) ?
            SMG3DS_IPC_SUCCESS : SMG3DS_IPC_EINVAL;
        break;
    case SMG3DS_IPC_IOCTL:
        ++g_stats.ioctl_requests;
        value = handle_ioctl(cpu, request, &disposition);
        break;
    case SMG3DS_IPC_IOCTLV:
        ++g_stats.ioctlv_requests;
            value = handle_ioctlv(cpu, request, &disposition);
        break;
    default:
        ++g_stats.unknown_requests;
        value = SMG3DS_IPC_UNKNOWN;
        break;
    }

    if (value == SMG3DS_IPC_EINVAL)
        ++g_stats.invalid_requests;
    g_stats.last_result = value;
    *result = value;
    return disposition;
}

bool smg3ds_ios_take_deferred_reply(u32* request, s32* result)
{
    if (!g_deferred_reply.valid || request == NULL || result == NULL)
        return false;
    *request = g_deferred_reply.request;
    *result = g_deferred_reply.result;
    memset(&g_deferred_reply, 0, sizeof(g_deferred_reply));
    return true;
}

const Smg3dsIosStats* smg3ds_ios_get_stats(void)
{
    return &g_stats;
}

const char* smg3ds_ios_fd_path(s32 fd)
{
    Smg3dsIosFd* entry = find_fd(fd);
    return entry == NULL ? NULL : entry->path;
}
