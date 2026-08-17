// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/ios_hle.h"

#include "smg3ds/disc.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

    SMG3DS_ISFS_EINVAL = -101,
    SMG3DS_ISFS_EACCESS = -102,
    SMG3DS_ISFS_EEXIST = -105,
    SMG3DS_ISFS_ENOENT = -106,
    SMG3DS_ISFS_UNKNOWN = -117,

    SMG3DS_IOS_FD_BASE = 32,
    SMG3DS_IOS_FD_COUNT = 64,
    SMG3DS_IOS_PATH_CAPACITY = 128,
    SMG3DS_HOST_PATH_CAPACITY = 192,
    SMG3DS_IOS_MAX_VECTORS = 16,
    SMG3DS_IOS_TRANSFER_CHUNK = 64 * 1024,
    SMG3DS_GUEST_COPY_PAGE_SIZE = 4096,

    SMG3DS_FS_CREATE_DIR = 3,
    SMG3DS_FS_READ_DIR = 4,
    SMG3DS_FS_GET_ATTR = 6,
    SMG3DS_FS_DELETE = 7,
    SMG3DS_FS_RENAME = 8,
    SMG3DS_FS_CREATE_FILE = 9,
    SMG3DS_FS_GET_FILE_STATS = 11,
    SMG3DS_FS_GET_USAGE = 12,
    SMG3DS_FS_SHUTDOWN = 13,

    SMG3DS_STM_EVENTHOOK = 0x1000,
    SMG3DS_STM_RELEASE_EVENTHOOK = 0x3002,

    SMG3DS_DI_READ_DISK_ID = 0x70,
    SMG3DS_DI_READ = 0x71,
    SMG3DS_DI_WAIT_COVER_CLOSE = 0x79,
    SMG3DS_DI_CLEAR_COVER_INTERRUPT = 0x86,
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
    FILE* file;
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
static u8 g_io_buffer[SMG3DS_IOS_TRANSFER_CHUNK];
static Smg3dsIosStats g_stats;
static char g_last_non_device_path[SMG3DS_IOS_PATH_CAPACITY];

static const char g_nand_root[] = "sdmc:/3ds/smg3ds/nand";
static const char g_title_data_dir[] =
    "/title/00010000/524d4745/data";

static void remember_non_device_path(const char* path)
{
    const char* recorded = path;

    if (path == NULL)
        return;
    if (path[0] == '\0')
        recorded = "(home)";
    snprintf(g_last_non_device_path, sizeof(g_last_non_device_path),
             "%s", recorded);
}

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

static bool write_guest_bytes(CPUState* cpu, u32 address,
                              const void* source, u32 size)
{
    const u8* bytes = (const u8*)source;
    u32 completed = 0u;

    if ((size != 0u && source == NULL) ||
        !guest_range_valid(cpu, address, size))
        return false;
    while (completed < size) {
        const u32 current = address + completed;
        const u32 page_remaining = SMG3DS_GUEST_COPY_PAGE_SIZE -
            (current & (SMG3DS_GUEST_COPY_PAGE_SIZE - 1u));
        const u32 chunk = size - completed < page_remaining ?
                          size - completed : page_remaining;
        u8* destination = ppc_memory_pointer(cpu, current, chunk);
        u32 copied = 0u;

        if (destination != NULL) {
            memcpy(destination, bytes + completed, chunk);
            completed += chunk;
            continue;
        }
        while (chunk - copied >= 8u) {
            const u8* input = bytes + completed + copied;
            const u64 value =
                ((u64)input[0] << 56u) | ((u64)input[1] << 48u) |
                ((u64)input[2] << 40u) | ((u64)input[3] << 32u) |
                ((u64)input[4] << 24u) | ((u64)input[5] << 16u) |
                ((u64)input[6] << 8u) | (u64)input[7];
            mem_write64(cpu, current + copied, value);
            copied += 8u;
        }
        while (copied < chunk) {
            mem_write8(cpu, current + copied,
                       bytes[completed + copied]);
            ++copied;
        }
        completed += chunk;
    }
    return true;
}

static bool read_guest_bytes(CPUState* cpu, u32 address,
                             void* destination, u32 size)
{
    u8* bytes = (u8*)destination;
    u32 completed = 0u;

    if ((size != 0u && destination == NULL) ||
        !guest_range_valid(cpu, address, size))
        return false;
    while (completed < size) {
        const u32 current = address + completed;
        const u32 page_remaining = SMG3DS_GUEST_COPY_PAGE_SIZE -
            (current & (SMG3DS_GUEST_COPY_PAGE_SIZE - 1u));
        const u32 chunk = size - completed < page_remaining ?
                          size - completed : page_remaining;
        const u8* source = ppc_memory_pointer(cpu, current, chunk);
        u32 copied = 0u;

        if (source != NULL) {
            memcpy(bytes + completed, source, chunk);
            completed += chunk;
            continue;
        }
        while (chunk - copied >= 8u) {
            const u64 value = mem_read64(cpu, current + copied);
            u8* output = bytes + completed + copied;
            output[0] = (u8)(value >> 56u);
            output[1] = (u8)(value >> 48u);
            output[2] = (u8)(value >> 40u);
            output[3] = (u8)(value >> 32u);
            output[4] = (u8)(value >> 24u);
            output[5] = (u8)(value >> 16u);
            output[6] = (u8)(value >> 8u);
            output[7] = (u8)value;
            copied += 8u;
        }
        while (copied < chunk) {
            bytes[completed + copied] =
                mem_read8(cpu, current + copied);
            ++copied;
        }
        completed += chunk;
    }
    return true;
}

static bool read_ioctlv_vector(CPUState* cpu, u32 request, u32 index,
                               u32* address, u32* size)
{
    u32 input_count;
    u32 output_count;
    u32 vectors;
    u32 total_count;

    if (address == NULL || size == NULL ||
        !guest_range_valid(cpu, request, 0x1cu))
        return false;
    input_count = mem_read32(cpu, request + 0x10u);
    output_count = mem_read32(cpu, request + 0x14u);
    total_count = input_count + output_count;
    vectors = mem_read32(cpu, request + 0x18u);
    if (total_count > SMG3DS_IOS_MAX_VECTORS || index >= total_count ||
        vectors == 0u ||
        !guest_range_valid(cpu, vectors, total_count * 8u))
        return false;
    *address = mem_read32(cpu, vectors + index * 8u);
    *size = mem_read32(cpu, vectors + index * 8u + 4u);
    return *size == 0u ||
           (*address != 0u && guest_range_valid(cpu, *address, *size));
}

static bool nand_path_is_safe(const char* path)
{
    const char* component;

    if (path == NULL || path[0] != '/' || strchr(path, 92) != NULL)
        return false;
    component = path;
    while (*component != '\0') {
        if ((component[0] == '.' && component[1] == '.' &&
             (component[2] == '/' || component[2] == '\0')) &&
            (component == path || component[-1] == '/'))
            return false;
        ++component;
    }
    return true;
}

static bool title_relative_path_is_safe(const char* path)
{
    const char* component;

    if (path == NULL || path[0] == '\0' || path[0] == '/' ||
        strchr(path, 92) != NULL)
        return false;
    component = path;
    while (*component != '\0') {
        if ((component[0] == '.' && component[1] == '.' &&
             (component[2] == '/' || component[2] == '\0')) &&
            (component == path || component[-1] == '/'))
            return false;
        ++component;
    }
    return true;
}

static bool title_root_file_is_save(const char* path)
{
    return path != NULL &&
           (strcmp(path, "/GameData.bin") == 0 ||
            strcmp(path, "/banner.bin") == 0);
}

static bool title_temp_path_is_safe(const char* path)
{
    return path != NULL && nand_path_is_safe(path) &&
           strncmp(path, "/tmp/sys", 8u) == 0 &&
           (path[8] == '\0' || path[8] == '/');
}

static bool nand_path_is_title_data(const char* path)
{
    const size_t prefix_length = strlen(g_title_data_dir);

    if (title_relative_path_is_safe(path))
        return true;
    if (title_root_file_is_save(path) || title_temp_path_is_safe(path))
        return true;
    return path != NULL && path[0] == '/' &&
           strncmp(path, g_title_data_dir, prefix_length) == 0 &&
           (path[prefix_length] == '\0' || path[prefix_length] == '/');
}

static bool host_path_for_nand(const char* path, char* output,
                               size_t capacity)
{
    int length;

    if (output == NULL || capacity == 0u)
        return false;
    if (title_relative_path_is_safe(path))
        length = snprintf(output, capacity, "%s%s/%s", g_nand_root,
                          g_title_data_dir, path);
    else if (title_root_file_is_save(path))
        length = snprintf(output, capacity, "%s%s%s", g_nand_root,
                          g_title_data_dir, path);
    else if (title_temp_path_is_safe(path))
        length = snprintf(output, capacity, "%s%s/.tmp%s", g_nand_root,
                          g_title_data_dir, path + 8u);
    else if (nand_path_is_safe(path))
        length = snprintf(output, capacity, "%s%s", g_nand_root, path);
    else
        return false;
    return length > 0 && (size_t)length < capacity;
}

static bool host_directory_exists(const char* path)
{
    struct stat info;
    return path != NULL && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool make_host_directory(const char* path)
{
    if (mkdir(path, 0777) == 0 || errno == EEXIST)
        return host_directory_exists(path);
    return false;
}

static bool make_host_directories(const char* path, bool include_leaf)
{
    char partial[SMG3DS_HOST_PATH_CAPACITY];
    size_t length;
    size_t position;

    if (path == NULL)
        return false;
    length = strlen(path);
    if (length == 0u || length >= sizeof(partial))
        return false;
    memcpy(partial, path, length + 1u);

    /* Keep the sdmc:/ archive prefix intact while creating each component. */
    for (position = 6u; position < length; ++position) {
        if (partial[position] != '/')
            continue;
        partial[position] = '\0';
        if (!make_host_directory(partial))
            return false;
        partial[position] = '/';
    }
    return !include_leaf || make_host_directory(partial);
}

static bool nand_path_exists(const char* path, bool* is_directory)
{
    char host_path[SMG3DS_HOST_PATH_CAPACITY];
    struct stat info;

    if (!host_path_for_nand(path, host_path, sizeof(host_path)) ||
        stat(host_path, &info) != 0)
        return false;
    if (is_directory != NULL)
        *is_directory = S_ISDIR(info.st_mode) != 0;
    return true;
}

static bool read_fs_attr_path(CPUState* cpu, const Smg3dsIoctl* ioctl,
                              char* path, size_t capacity)
{
    /* ISFSPathAttrArgs stores owner/group before its 64-byte path. */
    return ioctl->input != 0u && ioctl->input_size >= 74u &&
           guest_range_valid(cpu, ioctl->input, ioctl->input_size) &&
           read_guest_string(cpu, ioctl->input + 6u, path, capacity);
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
    case SMG3DS_DI_CLEAR_COVER_INTERRUPT:
        return SMG3DS_DI_SUCCESS;
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
        g_stats.last_unknown_ioctl_request = ioctl->request;
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
        g_stats.last_unknown_ioctl_request = ioctl_request;
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
    char host_path[SMG3DS_HOST_PATH_CAPACITY];
    const char* host_mode = NULL;
    u32 path_address;
    u32 mode;
    FILE* file = NULL;
    Smg3dsIosFd* entry;

    if (!guest_range_valid(cpu, request, 0x14u))
        return SMG3DS_IPC_EINVAL;
    path_address = mem_read32(cpu, request + 0x0cu);
    mode = mem_read32(cpu, request + 0x10u);
    if (!read_guest_string(cpu, path_address, path, sizeof(path)))
        return SMG3DS_IPC_EINVAL;
    if (strncmp(path, "/dev/", 5u) != 0)
        remember_non_device_path(path);

    if (strncmp(path, "/dev/", 5u) != 0 &&
        nand_path_is_title_data(path)) {
        if (!host_path_for_nand(path, host_path, sizeof(host_path)))
            return SMG3DS_ISFS_EINVAL;
        if (mode == 1u)
            host_mode = "rb";
        else if (mode == 2u || mode == 3u)
            host_mode = "r+b";
        else
            return SMG3DS_ISFS_EINVAL;
        file = fopen(host_path, host_mode);
        if (file == NULL)
            return errno == ENOENT ? SMG3DS_ISFS_ENOENT :
                   SMG3DS_ISFS_UNKNOWN;
    }

    entry = allocate_fd();
    if (entry == NULL) {
        if (file != NULL)
            fclose(file);
        return SMG3DS_IPC_EQUEUEFULL;
    }
    entry->mode = mode;
    entry->file = file;
    memcpy(entry->path, path, sizeof(entry->path));
    entry->path[sizeof(entry->path) - 1u] = '\0';
    return entry->fd;
}

static s32 handle_close(CPUState* cpu, u32 request)
{
    const s32 fd = (s32)mem_read32(cpu, request + 8u);
    Smg3dsIosFd* entry = find_fd(fd);
    s32 result = SMG3DS_IPC_SUCCESS;

    if (entry == NULL)
        return SMG3DS_IPC_EINVAL;
    if (entry->file != NULL && fclose(entry->file) != 0)
        result = SMG3DS_ISFS_UNKNOWN;
    memset(entry, 0, sizeof(*entry));
    return result;
}

static s32 handle_read_write(CPUState* cpu, u32 request, bool write)
{
    const s32 fd = (s32)mem_read32(cpu, request + 8u);
    const u32 buffer = mem_read32(cpu, request + 0x0cu);
    const u32 size = mem_read32(cpu, request + 0x10u);
    Smg3dsIosFd* entry = find_fd(fd);
    u32 completed = 0u;

    if (entry == NULL)
        return SMG3DS_IPC_EINVAL;
    if (size != 0u &&
        (buffer == 0u || !guest_range_valid(cpu, buffer, size)))
        return SMG3DS_IPC_EINVAL;
    /* Generic character devices read EOF and accept every byte written. */
    if (entry->file == NULL)
        return write ? (s32)size : 0;
    if ((write && entry->mode == 1u) || (!write && entry->mode == 2u))
        return SMG3DS_ISFS_EACCESS;

    while (completed < size) {
        const u32 remaining = size - completed;
        const u32 requested = remaining < sizeof(g_io_buffer) ?
                              remaining : (u32)sizeof(g_io_buffer);
        size_t transferred;

        if (write) {
            if (!read_guest_bytes(cpu, buffer + completed,
                                  g_io_buffer, requested))
                return SMG3DS_ISFS_EINVAL;
            transferred = fwrite(
                g_io_buffer, 1u, requested, entry->file);
        } else {
            transferred = fread(
                g_io_buffer, 1u, requested, entry->file);
            if (!write_guest_bytes(cpu, buffer + completed,
                                   g_io_buffer, (u32)transferred))
                return SMG3DS_ISFS_EINVAL;
        }
        completed += (u32)transferred;
        if (transferred != requested)
            break;
    }
    if (ferror(entry->file)) {
        clearerr(entry->file);
        return SMG3DS_ISFS_UNKNOWN;
    }
    return (s32)completed;
}

static s32 handle_seek(CPUState* cpu, u32 request)
{
    const s32 fd = (s32)mem_read32(cpu, request + 8u);
    const s32 offset = (s32)mem_read32(cpu, request + 0x0cu);
    const u32 whence = mem_read32(cpu, request + 0x10u);
    Smg3dsIosFd* entry = find_fd(fd);
    int host_whence;
    long position;

    if (entry == NULL || entry->file == NULL || whence > 2u)
        return SMG3DS_ISFS_EINVAL;
    host_whence = whence == 0u ? SEEK_SET :
                   whence == 1u ? SEEK_CUR : SEEK_END;
    if (fseek(entry->file, (long)offset, host_whence) != 0)
        return SMG3DS_ISFS_EINVAL;
    position = ftell(entry->file);
    return position < 0 || (unsigned long)position > UINT_MAX ?
           SMG3DS_ISFS_UNKNOWN : (s32)position;
}

static s32 handle_fs_ioctl(CPUState* cpu, Smg3dsIosFd* entry,
                           const Smg3dsIoctl* ioctl)
{
    char path[65];
    char second_path[65];
    char host_path[SMG3DS_HOST_PATH_CAPACITY];
    char second_host_path[SMG3DS_HOST_PATH_CAPACITY];
    bool is_directory;

    switch (ioctl->request) {
    case SMG3DS_FS_CREATE_DIR:
        if (!read_fs_attr_path(cpu, ioctl, path, sizeof(path)) ||
            !host_path_for_nand(path, host_path, sizeof(host_path)))
            return SMG3DS_ISFS_EINVAL;
        remember_non_device_path(path);
        if (!nand_path_is_title_data(path))
            return SMG3DS_IPC_SUCCESS;
        if (nand_path_exists(path, NULL))
            return SMG3DS_ISFS_EEXIST;
        return make_host_directories(host_path, true) ?
               SMG3DS_IPC_SUCCESS : SMG3DS_ISFS_UNKNOWN;
    case SMG3DS_FS_GET_ATTR:
        if (ioctl->input == 0u || ioctl->input_size < 64u ||
            ioctl->output == 0u || ioctl->output_size < 74u ||
            !guest_range_valid(cpu, ioctl->input, ioctl->input_size) ||
            !guest_range_valid(cpu, ioctl->output, ioctl->output_size) ||
            !read_guest_string(cpu, ioctl->input, path, sizeof(path)))
            return SMG3DS_ISFS_EINVAL;
        remember_non_device_path(path);
        if (!nand_path_is_title_data(path))
            return SMG3DS_IPC_SUCCESS;
        if (!nand_path_exists(path, &is_directory))
            return SMG3DS_ISFS_ENOENT;
        for (u32 index = 0u; index < 74u; ++index)
            mem_write8(cpu, ioctl->output + index, 0u);
        if (!write_guest_bytes(cpu, ioctl->output + 6u, path,
                               (u32)strlen(path) + 1u))
            return SMG3DS_ISFS_EINVAL;
        mem_write8(cpu, ioctl->output + 70u, 3u);
        mem_write8(cpu, ioctl->output + 71u, 3u);
        mem_write8(cpu, ioctl->output + 72u, 3u);
        mem_write8(cpu, ioctl->output + 73u, is_directory ? 1u : 0u);
        return SMG3DS_IPC_SUCCESS;
    case SMG3DS_FS_DELETE:
        if (ioctl->input == 0u || ioctl->input_size < 64u ||
            !read_guest_string(cpu, ioctl->input, path, sizeof(path)) ||
            !host_path_for_nand(path, host_path, sizeof(host_path)))
            return SMG3DS_ISFS_EINVAL;
        remember_non_device_path(path);
        if (!nand_path_is_title_data(path))
            return SMG3DS_IPC_SUCCESS;
        if (!nand_path_exists(path, NULL))
            return SMG3DS_ISFS_ENOENT;
        return remove(host_path) == 0 ? SMG3DS_IPC_SUCCESS :
               SMG3DS_ISFS_UNKNOWN;
    case SMG3DS_FS_RENAME:
        if (ioctl->input == 0u || ioctl->input_size < 128u ||
            !read_guest_string(cpu, ioctl->input, path, sizeof(path)) ||
            !read_guest_string(cpu, ioctl->input + 64u, second_path,
                               sizeof(second_path)) ||
            !host_path_for_nand(path, host_path, sizeof(host_path)) ||
            !host_path_for_nand(second_path, second_host_path,
                                sizeof(second_host_path)))
            return SMG3DS_ISFS_EINVAL;
        remember_non_device_path(path);
        if (!nand_path_is_title_data(path) ||
            !nand_path_is_title_data(second_path))
            return SMG3DS_IPC_SUCCESS;
        if (!nand_path_exists(path, NULL))
            return SMG3DS_ISFS_ENOENT;
        if (!make_host_directories(second_host_path, false))
            return SMG3DS_ISFS_UNKNOWN;
        /* ISFS rename replaces an existing destination (banner updates). */
        if (nand_path_exists(second_path, NULL) &&
            remove(second_host_path) != 0)
            return SMG3DS_ISFS_UNKNOWN;
        return rename(host_path, second_host_path) == 0 ?
               SMG3DS_IPC_SUCCESS : SMG3DS_ISFS_UNKNOWN;
    case SMG3DS_FS_CREATE_FILE: {
        FILE* file;
        if (!read_fs_attr_path(cpu, ioctl, path, sizeof(path)) ||
            !host_path_for_nand(path, host_path, sizeof(host_path)))
            return SMG3DS_ISFS_EINVAL;
        remember_non_device_path(path);
        if (!nand_path_is_title_data(path))
            return SMG3DS_IPC_SUCCESS;
        if (nand_path_exists(path, NULL))
            return SMG3DS_ISFS_EEXIST;
        if (!make_host_directories(host_path, false))
            return SMG3DS_ISFS_UNKNOWN;
        file = fopen(host_path, "wb");
        if (file == NULL)
            return SMG3DS_ISFS_UNKNOWN;
        return fclose(file) == 0 ? SMG3DS_IPC_SUCCESS :
               SMG3DS_ISFS_UNKNOWN;
    }
    case SMG3DS_FS_GET_FILE_STATS: {
        long current;
        long size;
        if (entry->file == NULL || ioctl->output == 0u ||
            ioctl->output_size < 8u ||
            !guest_range_valid(cpu, ioctl->output, 8u))
            return SMG3DS_ISFS_EINVAL;
        if (fflush(entry->file) != 0)
            return SMG3DS_ISFS_UNKNOWN;
        current = ftell(entry->file);
        if (current < 0 || fseek(entry->file, 0, SEEK_END) != 0)
            return SMG3DS_ISFS_UNKNOWN;
        size = ftell(entry->file);
        if (size < 0 || (unsigned long)size > UINT_MAX ||
            fseek(entry->file, current, SEEK_SET) != 0)
            return SMG3DS_ISFS_UNKNOWN;
        mem_write32(cpu, ioctl->output, (u32)size);
        mem_write32(cpu, ioctl->output + 4u, (u32)current);
        return SMG3DS_IPC_SUCCESS;
    }
    case SMG3DS_FS_SHUTDOWN: {
        size_t index;

        for (index = 0u; index < SMG3DS_IOS_FD_COUNT; ++index) {
            if (g_fds[index].used && g_fds[index].file != NULL &&
                fflush(g_fds[index].file) != 0)
                return SMG3DS_ISFS_UNKNOWN;
        }
        return SMG3DS_IPC_SUCCESS;
    }
    default:
        return SMG3DS_IPC_UNKNOWN;
    }
}

static s32 handle_fs_read_dir(CPUState* cpu, u32 request)
{
    const u32 input_count = mem_read32(cpu, request + 0x10u);
    const u32 output_count = mem_read32(cpu, request + 0x14u);
    char path[65];
    char host_path[SMG3DS_HOST_PATH_CAPACITY];
    u32 path_address;
    u32 path_size;
    u32 count_address;
    u32 count_size;
    u32 names_address = 0u;
    u32 names_size = 0u;
    u32 maximum_names = UINT_MAX;
    u32 found = 0u;
    u32 names_bytes = 0u;
    DIR* directory;
    struct dirent* item;

    if (!((input_count == 1u && output_count == 1u) ||
          (input_count == 2u && output_count == 2u)) ||
        !read_ioctlv_vector(cpu, request, 0u, &path_address, &path_size) ||
        path_size < 64u ||
        !read_guest_string(cpu, path_address, path, sizeof(path)) ||
        !host_path_for_nand(path, host_path, sizeof(host_path)))
        return SMG3DS_ISFS_EINVAL;
    remember_non_device_path(path);
    if (!nand_path_is_title_data(path))
        return SMG3DS_IPC_SUCCESS;

    if (input_count == 2u) {
        u32 maximum_address;
        u32 maximum_size;
        if (!read_ioctlv_vector(cpu, request, 1u, &maximum_address,
                                &maximum_size) || maximum_size < 4u ||
            !read_ioctlv_vector(cpu, request, 2u, &names_address,
                                &names_size))
            return SMG3DS_ISFS_EINVAL;
        maximum_names = mem_read32(cpu, maximum_address);
    }
    if (!read_ioctlv_vector(cpu, request, input_count + output_count - 1u,
                            &count_address, &count_size) || count_size < 4u)
        return SMG3DS_ISFS_EINVAL;

    directory = opendir(host_path);
    if (directory == NULL)
        return errno == ENOENT ? SMG3DS_ISFS_ENOENT : SMG3DS_ISFS_EINVAL;
    while ((item = readdir(directory)) != NULL) {
        const size_t length = strlen(item->d_name);
        if ((length == 1u && item->d_name[0] == '.') ||
            (length == 2u && item->d_name[0] == '.' &&
             item->d_name[1] == '.'))
            continue;
        if (length > 12u)
            continue;
        if (input_count == 2u && found < maximum_names &&
            names_bytes + (u32)length + 1u <= names_size) {
            write_guest_bytes(cpu, names_address + names_bytes,
                              item->d_name, (u32)length + 1u);
            names_bytes += (u32)length + 1u;
        }
        ++found;
    }
    closedir(directory);
    if (input_count == 2u && found > maximum_names)
        found = maximum_names;
    mem_write32(cpu, count_address, found);
    return SMG3DS_IPC_SUCCESS;
}

static s32 handle_fs_ioctlv(CPUState* cpu, u32 request, u32 ioctl_request)
{
    u32 input_count;
    u32 output_count;
    u32 path_address;
    u32 path_size;
    char path[65];

    if (!guest_range_valid(cpu, request, 0x1cu))
        return SMG3DS_ISFS_EINVAL;
    if (ioctl_request == SMG3DS_FS_READ_DIR)
        return handle_fs_read_dir(cpu, request);
    if (ioctl_request != SMG3DS_FS_GET_USAGE)
        return SMG3DS_IPC_UNKNOWN;

    input_count = mem_read32(cpu, request + 0x10u);
    output_count = mem_read32(cpu, request + 0x14u);
    if (input_count != 1u || output_count != 2u)
        return SMG3DS_ISFS_EINVAL;
    if (!read_ioctlv_vector(cpu, request, 0u, &path_address, &path_size) ||
        path_size < 1u ||
        !read_guest_string(cpu, path_address, path, sizeof(path)))
        return SMG3DS_ISFS_EINVAL;
    remember_non_device_path(path);
    for (u32 index = 0u; index < 2u; ++index) {
        u32 address;
        u32 size;
        if (!read_ioctlv_vector(cpu, request, input_count + index,
                                &address, &size) || size < 4u)
            return SMG3DS_ISFS_EINVAL;
        mem_write32(cpu, address, 0u);
    }
    return SMG3DS_IPC_SUCCESS;
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
    if (strcmp(entry->path, "/dev/fs") == 0 ||
        (ioctl.request == SMG3DS_FS_GET_FILE_STATS && entry->file != NULL))
        return handle_fs_ioctl(cpu, entry, &ioctl);
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
    if (strcmp(entry->path, "/dev/fs") == 0)
        return handle_fs_ioctlv(cpu, request, ioctl_request);
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
    memset(g_last_non_device_path, 0, sizeof(g_last_non_device_path));
    g_next_fd = SMG3DS_IOS_FD_BASE;
}

Smg3dsIosDisposition smg3ds_ios_handle_request(CPUState* cpu,
                                                u32 request,
                                                s32* result)
{
    Smg3dsIosDisposition disposition = SMG3DS_IOS_REPLY;
    u32 command;
    u32 unknown_before;
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
    unknown_before = g_stats.unknown_requests;
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
            handle_seek(cpu, request) : SMG3DS_IPC_EINVAL;
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
        g_stats.last_unknown_ioctl_request = 0u;
        ++g_stats.unknown_requests;
        value = SMG3DS_IPC_UNKNOWN;
        break;
    }

    if (g_stats.unknown_requests != unknown_before) {
        g_stats.last_unknown_ipc_command = command;
        g_stats.last_unknown_fd = g_stats.last_fd;
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
    if (entry != NULL && strcmp(entry->path, "/dev/fs") != 0)
        return entry->path;
    return g_last_non_device_path[0] == '\0' ? NULL :
           g_last_non_device_path;
}
