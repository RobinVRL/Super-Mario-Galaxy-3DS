// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/disc.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SMG3DS_DISC_ID_SIZE 32u
#define SMG3DS_FST_ENTRY_SIZE 12u
#define SMG3DS_FST_MAX_SIZE (4u * 1024u * 1024u)
#define SMG3DS_FST_GUEST_TOP 0x817fc000u
#define SMG3DS_FST_GUEST_MIN 0x81000000u
#define SMG3DS_DISC_PATH_MAX 1024u
#define SMG3DS_DISC_READ_BUFFER_SIZE (64u * 1024u)
#define SMG3DS_GUEST_COPY_PAGE_SIZE 4096u

/* NTSC-U RVL SDK DVD filesystem globals in this title's DOL. */
#define SMG3DS_DVD_CURRENT_DIRECTORY 0x806a2f08u
#define SMG3DS_DVD_MAX_ENTRY_NUM 0x806a2f18u
#define SMG3DS_DVD_FST_STRING_START 0x806a2f1cu
#define SMG3DS_DVD_FST_START 0x806a2f20u
#define SMG3DS_DVD_BOOT_INFO 0x806a2f24u

typedef struct Smg3dsDiscFile {
    u64 offset;
    u32 size;
    u32 fst_index;
    bool host_validated;
} Smg3dsDiscFile;

static u8 g_disc_id[SMG3DS_DISC_ID_SIZE];
static u8 *g_fst;
static size_t g_fst_size;
static u32 g_fst_entry_count;
static u32 g_fst_guest_address;
static u32 *g_fst_parents;
static Smg3dsDiscFile *g_files;
static u32 g_file_count;
static char g_data_root[SMG3DS_DISC_PATH_MAX];
static char g_last_error[256];
static u8 g_read_buffer[SMG3DS_DISC_READ_BUFFER_SIZE];
static FILE *g_cached_host_file;
static u32 g_cached_host_file_index = UINT32_MAX;
static u64 g_cached_host_file_position;
static Smg3dsDiscStats g_stats;

static void set_error(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(g_last_error, sizeof(g_last_error), format, arguments);
    va_end(arguments);
    g_last_error[sizeof(g_last_error) - 1u] = '\0';
}

static u32 disc_read_be32(const u8 *data)
{
    return ((u32)data[0] << 24) |
           ((u32)data[1] << 16) |
           ((u32)data[2] << 8) |
           (u32)data[3];
}

static const u8 *fst_entry(u32 index)
{
    return g_fst + ((size_t)index * SMG3DS_FST_ENTRY_SIZE);
}

static bool fst_entry_is_directory(u32 index)
{
    return (disc_read_be32(fst_entry(index)) & 0xff000000u) != 0u;
}

static unsigned char fold_ascii(unsigned char character)
{
    if (character >= (unsigned char)'A' && character <= (unsigned char)'Z') {
        return (unsigned char)(character +
                               ((unsigned char)'a' - (unsigned char)'A'));
    }
    return character;
}

static const char *fst_entry_name(u32 index)
{
    u32 name_offset;
    size_t strings_offset;

    name_offset = disc_read_be32(fst_entry(index)) & 0x00ffffffu;
    strings_offset = (size_t)g_fst_entry_count * SMG3DS_FST_ENTRY_SIZE;
    return (const char *)(g_fst + strings_offset + name_offset);
}

static void free_disc_state(void)
{
    if (g_cached_host_file != NULL)
        fclose(g_cached_host_file);
    free(g_files);
    free(g_fst_parents);
    free(g_fst);

    g_files = NULL;
    g_fst_parents = NULL;
    g_fst = NULL;
    g_fst_size = 0u;
    g_fst_entry_count = 0u;
    g_fst_guest_address = 0u;
    g_file_count = 0u;
    g_cached_host_file = NULL;
    g_cached_host_file_index = UINT32_MAX;
    g_cached_host_file_position = 0u;
    g_data_root[0] = '\0';
    g_stats.initialized = false;
    g_stats.file_count = 0u;
}

static bool load_disc_id_from(const char *path)
{
    FILE *file;
    size_t count;

    file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    count = fread(g_disc_id, 1u, sizeof(g_disc_id), file);
    fclose(file);
    return count == sizeof(g_disc_id);
}

static bool load_fst_from(const char *path)
{
    FILE *file;
    long length;
    u8 *data;
    size_t count;

    file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    length = ftell(file);
    if (length < (long)SMG3DS_FST_ENTRY_SIZE ||
        (unsigned long)length > (unsigned long)SMG3DS_FST_MAX_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    data = (u8 *)malloc((size_t)length);
    if (data == NULL) {
        fclose(file);
        return false;
    }

    count = fread(data, 1u, (size_t)length, file);
    fclose(file);
    if (count != (size_t)length) {
        free(data);
        return false;
    }

    g_fst = data;
    g_fst_size = (size_t)length;
    return true;
}

static bool directory_exists(const char *path)
{
    struct stat info;

    if (stat(path, &info) != 0) {
        return false;
    }

    return S_ISDIR(info.st_mode) != 0;
}

static bool validate_component(const char *name, size_t length, u32 index)
{
    size_t position;

    if (length == 0u) {
        set_error("FST entry %lu has an empty name", (unsigned long)index);
        return false;
    }

    if ((length == 1u && name[0] == '.') ||
        (length == 2u && name[0] == '.' && name[1] == '.')) {
        set_error("FST entry %lu contains a traversal component",
                  (unsigned long)index);
        return false;
    }

    for (position = 0u; position < length; ++position) {
        unsigned char character;

        character = (unsigned char)name[position];
        if (character < 0x20u || character == '/' || character == '\\' ||
            character == ':' || character == '<' || character == '>' ||
            character == '"' || character == '|' || character == '?' ||
            character == '*') {
            set_error("FST entry %lu contains an unsafe path character",
                      (unsigned long)index);
            return false;
        }
    }

    return true;
}

static bool validate_entry_name(u32 index, size_t strings_offset)
{
    const u8 *entry;
    u32 name_offset;
    const char *name;
    const char *terminator;
    size_t remaining;
    size_t length;

    entry = fst_entry(index);
    name_offset = disc_read_be32(entry) & 0x00ffffffu;
    if ((size_t)name_offset >= g_fst_size - strings_offset) {
        set_error("FST entry %lu has an out-of-range name",
                  (unsigned long)index);
        return false;
    }

    name = (const char *)(g_fst + strings_offset + name_offset);
    remaining = g_fst_size - strings_offset - (size_t)name_offset;
    terminator = (const char *)memchr(name, '\0', remaining);
    if (terminator == NULL) {
        set_error("FST entry %lu has an unterminated name",
                  (unsigned long)index);
        return false;
    }

    if (index == 0u) {
        return true;
    }

    length = (size_t)(terminator - name);
    return validate_component(name, length, index);
}

static bool build_relative_path(u32 index, char *path, size_t path_size)
{
    u32 current;
    u32 components;
    size_t total;
    size_t position;

    if (index == 0u || index >= g_fst_entry_count || path_size == 0u) {
        set_error("Invalid FST path request for entry %lu",
                  (unsigned long)index);
        return false;
    }

    current = index;
    components = 0u;
    total = 0u;
    while (current != 0u) {
        const char *name;
        size_t length;

        if (current >= g_fst_entry_count || components >= g_fst_entry_count) {
            set_error("FST entry %lu has an invalid parent chain",
                      (unsigned long)index);
            return false;
        }

        name = fst_entry_name(current);
        length = strlen(name);
        if (length > SIZE_MAX - total ||
            (components != 0u && total + length == SIZE_MAX)) {
            set_error("FST path for entry %lu is too long",
                      (unsigned long)index);
            return false;
        }

        total += length;
        if (components != 0u) {
            ++total;
        }

        ++components;
        current = g_fst_parents[current];
    }

    if (total + 1u > path_size) {
        set_error("FST path for entry %lu exceeds %lu bytes",
                  (unsigned long)index,
                  (unsigned long)(path_size - 1u));
        return false;
    }

    path[total] = '\0';
    position = total;
    current = index;
    while (current != 0u) {
        const char *name;
        size_t length;

        name = fst_entry_name(current);
        length = strlen(name);
        position -= length;
        memcpy(path + position, name, length);
        current = g_fst_parents[current];
        if (current != 0u) {
            --position;
            path[position] = '/';
        }
    }

    if (position != 0u) {
        set_error("Internal FST path construction error");
        return false;
    }

    return true;
}

static int compare_disc_files(const void *left_pointer,
                              const void *right_pointer)
{
    const Smg3dsDiscFile *left;
    const Smg3dsDiscFile *right;

    left = (const Smg3dsDiscFile *)left_pointer;
    right = (const Smg3dsDiscFile *)right_pointer;
    if (left->offset < right->offset) {
        return -1;
    }
    if (left->offset > right->offset) {
        return 1;
    }
    if (left->size < right->size) {
        return -1;
    }
    if (left->size > right->size) {
        return 1;
    }
    if (left->fst_index < right->fst_index) {
        return -1;
    }
    if (left->fst_index > right->fst_index) {
        return 1;
    }
    return 0;
}

static bool parse_fst(void)
{
    u32 entry_count;
    size_t entries_size;
    size_t strings_size;
    u32 file_count;
    u32 index;
    u32 *directory_indices;
    u32 *directory_ends;
    u32 directory_depth;
    u32 file_position;
    u64 covered_end;
    bool have_covered_file;
    char relative_path[SMG3DS_DISC_PATH_MAX];

    if (g_fst_size < SMG3DS_FST_ENTRY_SIZE ||
        (disc_read_be32(g_fst) & 0xff000000u) == 0u) {
        set_error("The FST root entry is missing or is not a directory");
        return false;
    }

    entry_count = disc_read_be32(g_fst + 8u);
    if (entry_count == 0u ||
        (size_t)entry_count > g_fst_size / SMG3DS_FST_ENTRY_SIZE) {
        set_error("The FST root contains an invalid entry count");
        return false;
    }

    entries_size = (size_t)entry_count * SMG3DS_FST_ENTRY_SIZE;
    strings_size = g_fst_size - entries_size;
    if (strings_size == 0u) {
        set_error("The FST has no string table");
        return false;
    }

    g_fst_entry_count = entry_count;
    file_count = 0u;
    for (index = 0u; index < entry_count; ++index) {
        if (!validate_entry_name(index, entries_size)) {
            return false;
        }
        if (!fst_entry_is_directory(index)) {
            ++file_count;
        }
    }

    g_fst_parents = (u32 *)calloc((size_t)entry_count, sizeof(u32));
    directory_indices = (u32 *)malloc((size_t)entry_count * sizeof(u32));
    directory_ends = (u32 *)malloc((size_t)entry_count * sizeof(u32));
    if (g_fst_parents == NULL || directory_indices == NULL ||
        directory_ends == NULL) {
        free(directory_indices);
        free(directory_ends);
        set_error("Out of memory while indexing the FST directories");
        return false;
    }

    if (file_count != 0u) {
        g_files = (Smg3dsDiscFile *)malloc((size_t)file_count *
                                           sizeof(Smg3dsDiscFile));
        if (g_files == NULL) {
            free(directory_indices);
            free(directory_ends);
            set_error("Out of memory while indexing the FST files");
            return false;
        }
    }

    directory_indices[0] = 0u;
    directory_ends[0] = entry_count;
    directory_depth = 1u;
    file_position = 0u;

    for (index = 1u; index < entry_count; ++index) {
        const u8 *entry;

        while (directory_depth != 0u &&
               index >= directory_ends[directory_depth - 1u]) {
            --directory_depth;
        }
        if (directory_depth == 0u) {
            free(directory_indices);
            free(directory_ends);
            set_error("FST entry %lu falls outside the root directory",
                      (unsigned long)index);
            return false;
        }

        entry = fst_entry(index);
        g_fst_parents[index] = directory_indices[directory_depth - 1u];
        if (fst_entry_is_directory(index)) {
            u32 encoded_parent;
            u32 next_index;

            encoded_parent = disc_read_be32(entry + 4u);
            next_index = disc_read_be32(entry + 8u);
            if (encoded_parent != g_fst_parents[index] ||
                next_index <= index || next_index > entry_count ||
                next_index > directory_ends[directory_depth - 1u]) {
                free(directory_indices);
                free(directory_ends);
                set_error("FST directory %lu has invalid bounds",
                          (unsigned long)index);
                return false;
            }

            directory_indices[directory_depth] = index;
            directory_ends[directory_depth] = next_index;
            ++directory_depth;
        } else {
            u32 word_offset;

            word_offset = disc_read_be32(entry + 4u);
            g_files[file_position].offset = (u64)word_offset << 2;
            g_files[file_position].size = disc_read_be32(entry + 8u);
            g_files[file_position].fst_index = index;
            g_files[file_position].host_validated = false;
            ++file_position;
        }
    }

    free(directory_indices);
    free(directory_ends);
    directory_indices = NULL;
    directory_ends = NULL;

    g_file_count = file_count;
    if (file_count != 0u) {
        qsort(g_files, (size_t)file_count, sizeof(Smg3dsDiscFile),
              compare_disc_files);
    }

    covered_end = 0u;
    have_covered_file = false;
    for (index = 0u; index < file_count; ++index) {
        u64 file_end;

        file_end = g_files[index].offset + (u64)g_files[index].size;
        if (file_end < g_files[index].offset) {
            set_error("FST file %lu has an overflowing disc range",
                      (unsigned long)g_files[index].fst_index);
            return false;
        }
        if (g_files[index].size != 0u) {
            if (have_covered_file && g_files[index].offset < covered_end) {
                set_error("FST files %lu and an earlier file overlap",
                          (unsigned long)g_files[index].fst_index);
                return false;
            }
            covered_end = file_end;
            have_covered_file = true;
        }

        if (!build_relative_path(g_files[index].fst_index, relative_path,
                                 sizeof(relative_path))) {
            return false;
        }
    }

    return true;
}

static bool select_data_root(void)
{
    static const char *roots[] = {
        "sdmc:/smg3ds/DATA/files",
        "romfs:/game/files"
    };
    size_t index;

    for (index = 0u; index < sizeof(roots) / sizeof(roots[0]); ++index) {
        if (directory_exists(roots[index])) {
            snprintf(g_data_root, sizeof(g_data_root), "%s", roots[index]);
            g_data_root[sizeof(g_data_root) - 1u] = '\0';
            return true;
        }
    }

    set_error("Extracted disc files are unavailable under sdmc:/smg3ds/DATA/files or romfs:/game/files");
    return false;
}

static bool format_host_path(u32 file_index,
                             char *path,
                             size_t path_size)
{
    char relative_path[SMG3DS_DISC_PATH_MAX];
    int count;

    if (file_index >= g_file_count ||
        !build_relative_path(g_files[file_index].fst_index,
                             relative_path, sizeof(relative_path))) {
        return false;
    }

    count = snprintf(path, path_size, "%s/%s", g_data_root, relative_path);
    if (count < 0 || (size_t)count >= path_size) {
        set_error("Host path for FST entry %lu is too long",
                  (unsigned long)g_files[file_index].fst_index);
        return false;
    }

    return true;
}

static FILE *open_cached_host_file(u32 file_index, u64 position,
                                   char *host_path, size_t path_size)
{
    static const char read_mode[] = {'r', 'b', 0};
    Smg3dsDiscFile *file;

    if (file_index >= g_file_count || position > (u64)LONG_MAX ||
        !format_host_path(file_index, host_path, path_size))
        return NULL;
    file = &g_files[file_index];
    if (g_cached_host_file == NULL ||
        g_cached_host_file_index != file_index) {
        long host_length;

        if (g_cached_host_file != NULL)
            fclose(g_cached_host_file);
        g_cached_host_file = fopen(host_path, read_mode);
        g_cached_host_file_index = UINT32_MAX;
        g_cached_host_file_position = 0u;
        if (g_cached_host_file == NULL) {
            set_error("Extracted disc file is missing: %s", host_path);
            return NULL;
        }
        setvbuf(g_cached_host_file, NULL, _IOFBF,
                SMG3DS_DISC_READ_BUFFER_SIZE);
        if (!file->host_validated) {
            if (file->size > (u32)LONG_MAX ||
                fseek(g_cached_host_file, 0, SEEK_END) != 0 ||
                (host_length = ftell(g_cached_host_file)) < 0 ||
                (unsigned long)host_length < (unsigned long)file->size) {
                fclose(g_cached_host_file);
                g_cached_host_file = NULL;
                return NULL;
            }
            file->host_validated = true;
            g_cached_host_file_position = (u64)(unsigned long)host_length;
        }
        g_cached_host_file_index = file_index;
    }
    if (g_cached_host_file_position != position) {
        if (fseek(g_cached_host_file, (long)position, SEEK_SET) != 0)
            return NULL;
        g_cached_host_file_position = position;
    }
    return g_cached_host_file;
}

static bool find_file_at_offset(u64 offset, u32 *file_index)
{
    u32 low;
    u32 high;
    u32 position;

    low = 0u;
    high = g_file_count;
    while (low < high) {
        u32 middle;

        middle = low + ((high - low) / 2u);
        if (g_files[middle].offset < offset) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }

    position = low;
    while (position != 0u) {
        const Smg3dsDiscFile *candidate;
        u64 candidate_end;

        --position;
        candidate = &g_files[position];
        if (candidate->size == 0u) {
            continue;
        }
        candidate_end = candidate->offset + (u64)candidate->size;
        if (offset >= candidate->offset && offset < candidate_end) {
            *file_index = position;
            return true;
        }
        break;
    }

    position = low;
    while (position < g_file_count &&
           g_files[position].offset == offset) {
        if (g_files[position].size != 0u) {
            *file_index = position;
            return true;
        }
        ++position;
    }

    return false;
}

static bool validate_disc_range(u64 offset, u32 length)
{
    u64 position;
    u64 remaining;

    position = offset;
    remaining = (u64)length;
    while (remaining != 0u) {
        u32 file_index;
        const Smg3dsDiscFile *file;
        u64 file_end;
        u64 available;
        u64 step;

        if (!find_file_at_offset(position, &file_index)) {
            set_error("Disc range is unmapped at byte offset 0x%llx",
                      (unsigned long long)position);
            return false;
        }

        file = &g_files[file_index];
        file_end = file->offset + (u64)file->size;
        available = file_end - position;
        step = remaining < available ? remaining : available;
        if (step == 0u) {
            set_error("Disc range cannot advance at byte offset 0x%llx",
                      (unsigned long long)position);
            return false;
        }

        position += step;
        remaining -= step;
    }

    return true;
}

static bool validate_host_files(u64 offset, u32 length)
{
    u64 position;
    u64 remaining;
    u32 previous_file_index;

    position = offset;
    remaining = (u64)length;
    previous_file_index = UINT32_MAX;
    while (remaining != 0u) {
        u32 file_index;
        const Smg3dsDiscFile *file;
        u64 file_end;
        u64 available;
        u64 step;

        if (!find_file_at_offset(position, &file_index)) {
            set_error("Disc range changed while validating offset 0x%llx",
                      (unsigned long long)position);
            return false;
        }

        file = &g_files[file_index];
        file_end = file->offset + (u64)file->size;
        available = file_end - position;
        step = remaining < available ? remaining : available;

        if (file_index != previous_file_index) {
            char host_path[SMG3DS_DISC_PATH_MAX];
            FILE *host_file;

            host_file = open_cached_host_file(
                file_index, 0u, host_path, sizeof(host_path));
            if (host_file == NULL)
                return false;
            previous_file_index = file_index;
        }

        position += step;
        remaining -= step;
    }

    return true;
}

static void write_guest_bytes(CPUState *cpu,
                              u32 guest_address,
                              const u8 *data,
                              size_t length)
{
    size_t position = 0u;

    while (position < length) {
        const u32 address = guest_address + (u32)position;
        const size_t page_remaining =
            SMG3DS_GUEST_COPY_PAGE_SIZE -
            (address & (SMG3DS_GUEST_COPY_PAGE_SIZE - 1u));
        const size_t chunk = length - position < page_remaining ?
                             length - position : page_remaining;
        u8 *destination = ppc_memory_pointer(
            cpu, address, (u32)chunk);
        size_t copied = 0u;

        if (destination != NULL) {
            memcpy(destination, data + position, chunk);
            position += chunk;
            continue;
        }
        while (chunk - copied >= 8u) {
            const u8 *source = data + position + copied;
            const u64 value =
                ((u64)source[0] << 56u) | ((u64)source[1] << 48u) |
                ((u64)source[2] << 40u) | ((u64)source[3] << 32u) |
                ((u64)source[4] << 24u) | ((u64)source[5] << 16u) |
                ((u64)source[6] << 8u) | (u64)source[7];
            mem_write64(cpu, address + (u32)copied, value);
            copied += 8u;
        }
        while (copied < chunk) {
            mem_write8(cpu, address + (u32)copied,
                       data[position + copied]);
            ++copied;
        }
        position += chunk;
    }
}

static void write_guest_disc_state(CPUState *cpu)
{
    const u32 strings_address =
        g_fst_guest_address + g_fst_entry_count * SMG3DS_FST_ENTRY_SIZE;

    write_guest_bytes(cpu, 0x80000000u, g_disc_id, sizeof(g_disc_id));
    write_guest_bytes(cpu, g_fst_guest_address, g_fst, g_fst_size);
    mem_write32(cpu, 0x80000034u, g_fst_guest_address);
    mem_write32(cpu, 0x80000038u, g_fst_guest_address);
    mem_write32(cpu, 0x8000003cu, (u32)g_fst_size);

    /*
     * __DVDFSInit normally derives these cached values from boot info.  The
     * static bring-up loads disc metadata before the DOL, whose BSS clear
     * resets the cache.  Publish it explicitly after loading the DOL so every
     * SDK path lookup sees the same validated FST used by the host disc layer.
     */
    mem_write32(cpu, SMG3DS_DVD_CURRENT_DIRECTORY, 0u);
    mem_write32(cpu, SMG3DS_DVD_MAX_ENTRY_NUM, g_fst_entry_count);
    mem_write32(cpu, SMG3DS_DVD_FST_STRING_START, strings_address);
    mem_write32(cpu, SMG3DS_DVD_FST_START, g_fst_guest_address);
    mem_write32(cpu, SMG3DS_DVD_BOOT_INFO, 0x80000000u);
}

static bool stream_disc_range(CPUState *cpu,
                              u32 guest_address,
                              u64 offset,
                              u32 length)
{
    u64 position;
    u64 remaining;
    u32 guest_position;

    position = offset;
    remaining = (u64)length;
    guest_position = guest_address;
    while (remaining != 0u) {
        u32 file_index;
        const Smg3dsDiscFile *file;
        u64 file_end;
        u64 available;
        u64 segment_size;
        u64 file_position;
        char host_path[SMG3DS_DISC_PATH_MAX];
        FILE *host_file;

        if (!find_file_at_offset(position, &file_index)) {
            set_error("Disc range changed while reading offset 0x%llx",
                      (unsigned long long)position);
            return false;
        }

        file = &g_files[file_index];
        file_end = file->offset + (u64)file->size;
        available = file_end - position;
        segment_size = remaining < available ? remaining : available;
        file_position = position - file->offset;

        if (file_position > (u64)LONG_MAX ||
            !format_host_path(file_index, host_path, sizeof(host_path))) {
            if (g_last_error[0] == '\0') {
                set_error("File offset is too large for FST entry %lu",
                          (unsigned long)file->fst_index);
            }
            return false;
        }

        host_file = open_cached_host_file(
            file_index, file_position, host_path, sizeof(host_path));
        if (host_file == NULL) {
            set_error("Could not seek extracted disc file: %s", host_path);
            return false;
        }

        while (segment_size != 0u) {
            size_t chunk_size;
            size_t count;

            chunk_size = segment_size < sizeof(g_read_buffer)
                             ? (size_t)segment_size
                             : sizeof(g_read_buffer);
            count = fread(g_read_buffer, 1u, chunk_size, host_file);
            if (count != chunk_size) {
                set_error("Short read from extracted disc file: %s", host_path);
                return false;
            }

            g_cached_host_file_position += (u64)count;

            write_guest_bytes(cpu, guest_position, g_read_buffer, chunk_size);
            guest_position += (u32)chunk_size;
            position += (u64)chunk_size;
            remaining -= (u64)chunk_size;
            segment_size -= (u64)chunk_size;
        }

    }

    return true;
}

bool smg3ds_disc_init(CPUState *cpu)
{
    size_t aligned_size;

    smg3ds_disc_shutdown();
    if (cpu == NULL) {
        set_error("Cannot initialize the disc layer without a CPU state");
        return false;
    }

    if (!load_disc_id_from("romfs:/game/boot.bin") &&
        !load_disc_id_from("sdmc:/smg3ds/DATA/sys/boot.bin")) {
        set_error("Could not read a 32-byte disc ID from boot.bin");
        goto failure;
    }

    if (!load_fst_from("romfs:/game/fst.bin") &&
        !load_fst_from("sdmc:/smg3ds/DATA/sys/fst.bin")) {
        set_error("Could not load a valid fst.bin (maximum size is %lu bytes)",
                  (unsigned long)SMG3DS_FST_MAX_SIZE);
        goto failure;
    }

    if (!parse_fst() || !select_data_root()) {
        goto failure;
    }

    aligned_size = (g_fst_size + 31u) & ~(size_t)31u;
    if (aligned_size > (size_t)(SMG3DS_FST_GUEST_TOP -
                                SMG3DS_FST_GUEST_MIN)) {
        set_error("fst.bin does not fit in the reserved guest-memory range");
        goto failure;
    }
    g_fst_guest_address = SMG3DS_FST_GUEST_TOP - (u32)aligned_size;

    write_guest_disc_state(cpu);

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.initialized = true;
    g_stats.file_count = g_file_count;
    g_last_error[0] = '\0';
    return true;

failure:
    free_disc_state();
    return false;
}

bool smg3ds_disc_publish_guest_state(CPUState *cpu)
{
    if (!g_stats.initialized || g_fst == NULL) {
        set_error("Cannot publish guest disc state before initialization");
        return false;
    }
    if (cpu == NULL) {
        set_error("Cannot publish guest disc state without a CPU state");
        return false;
    }

    write_guest_disc_state(cpu);
    g_last_error[0] = '\0';
    return true;
}

static bool component_matches_entry(const char *component,
                                    size_t component_length,
                                    u32 entry_index)
{
    const char *name = fst_entry_name(entry_index);
    size_t position;

    if (strlen(name) != component_length)
        return false;
    for (position = 0u; position < component_length; ++position) {
        if (fold_ascii((unsigned char)component[position]) !=
            fold_ascii((unsigned char)name[position])) {
            return false;
        }
    }
    return true;
}

static s32 find_child_entry(u32 parent,
                            const char *component,
                            size_t component_length)
{
    u32 index;

    for (index = 1u; index < g_fst_entry_count; ++index) {
        if (g_fst_parents[index] == parent &&
            component_matches_entry(component, component_length, index)) {
            return (s32)index;
        }
    }
    return -1;
}

bool smg3ds_disc_resolve_path(CPUState *cpu,
                              u32 guest_path_address,
                              s32 *entry_index)
{
    char path[SMG3DS_DISC_PATH_MAX];
    size_t length;
    size_t position;
    u32 current;

    if (!g_stats.initialized || g_fst == NULL || cpu == NULL ||
        entry_index == NULL) {
        return false;
    }

    *entry_index = -1;
    if (guest_path_address == 0u)
        return true;
    for (length = 0u; length < sizeof(path); ++length) {
        path[length] = (char)mem_read8(cpu, guest_path_address + (u32)length);
        if (path[length] == '\0')
            break;
    }
    if (length == sizeof(path))
        return true;

    position = 0u;
    current = path[0] == '/' ? 0u :
        mem_read32(cpu, SMG3DS_DVD_CURRENT_DIRECTORY);
    if (current >= g_fst_entry_count ||
        (current != 0u && !fst_entry_is_directory(current))) {
        current = 0u;
    }

    while (position < length) {
        size_t component_start;
        size_t component_length;
        s32 child;

        while (position < length && path[position] == '/')
            ++position;
        if (position == length)
            break;

        component_start = position;
        while (position < length && path[position] != '/')
            ++position;
        component_length = position - component_start;

        if (component_length == 1u && path[component_start] == '.')
            continue;
        if (component_length == 2u && path[component_start] == '.' &&
            path[component_start + 1u] == '.') {
            current = current == 0u ? 0u : g_fst_parents[current];
            continue;
        }

        child = find_child_entry(current, path + component_start,
                                 component_length);
        if (child < 0)
            return true;
        current = (u32)child;

        if (position < length && !fst_entry_is_directory(current))
            return true;
    }

    *entry_index = (s32)current;
    return true;
}

bool smg3ds_disc_open_entry(CPUState *cpu,
                            s32 entry_index,
                            u32 guest_file_info_address,
                            bool *opened)
{
    const u8 *entry;

    if (!g_stats.initialized || g_fst == NULL || cpu == NULL ||
        opened == NULL) {
        return false;
    }

    *opened = false;
    if (entry_index < 0 || (u32)entry_index >= g_fst_entry_count ||
        fst_entry_is_directory((u32)entry_index) ||
        guest_file_info_address == 0u) {
        return true;
    }

    entry = fst_entry((u32)entry_index);

    /*
     * DVDOpen stores the partition-relative word offset and byte length in
     * DVDFileInfo, then clears the callback and transferred-size fields.
     * The DI HLE converts the word offset to bytes when servicing DVDRead.
     */
    mem_write32(cpu, guest_file_info_address + 0x30u,
                disc_read_be32(entry + 4u));
    mem_write32(cpu, guest_file_info_address + 0x34u,
                disc_read_be32(entry + 8u));
    mem_write32(cpu, guest_file_info_address + 0x38u, 0u);
    mem_write32(cpu, guest_file_info_address + 0x0cu, 0u);
    *opened = true;
    return true;
}

bool smg3ds_disc_open_path(CPUState *cpu,
                           u32 guest_path_address,
                           u32 guest_file_info_address,
                           bool *opened)
{
    s32 entry_index;

    if (opened == NULL)
        return false;

    *opened = false;
    if (!smg3ds_disc_resolve_path(cpu, guest_path_address, &entry_index))
        return false;
    return smg3ds_disc_open_entry(cpu, entry_index,
                                  guest_file_info_address, opened);
}

void smg3ds_disc_shutdown(void)
{
    free_disc_state();
    memset(g_disc_id, 0, sizeof(g_disc_id));
    memset(&g_stats, 0, sizeof(g_stats));
    g_last_error[0] = '\0';
}

bool smg3ds_disc_read_id(CPUState *cpu, u32 guest_address, u32 length)
{
    if (!g_stats.initialized) {
        set_error("The disc layer is not initialized");
        return false;
    }
    if (cpu == NULL) {
        set_error("Cannot copy the disc ID without a CPU state");
        return false;
    }
    if (length > SMG3DS_DISC_ID_SIZE ||
        (u64)guest_address + (u64)length > 0x100000000ull) {
        set_error("Invalid disc ID destination or length");
        return false;
    }

    write_guest_bytes(cpu, guest_address, g_disc_id, (size_t)length);
    g_last_error[0] = '\0';
    return true;
}

bool smg3ds_disc_read(CPUState *cpu,
                      u32 guest_address,
                      u64 byte_offset,
                      u32 length)
{
    bool success;

    ++g_stats.read_requests;
    g_stats.last_offset = byte_offset;
    g_stats.last_length = length;

    success = false;
    if (!g_stats.initialized) {
        set_error("The disc layer is not initialized");
    } else if (cpu == NULL) {
        set_error("Cannot read disc data without a CPU state");
    } else if (byte_offset + (u64)length < byte_offset) {
        set_error("Disc read range overflows the byte offset");
    } else if ((u64)guest_address + (u64)length > 0x100000000ull) {
        set_error("Disc read destination overflows guest address space");
    } else if (length == 0u) {
        success = true;
    } else if (validate_disc_range(byte_offset, length) &&
               validate_host_files(byte_offset, length) &&
               stream_disc_range(cpu, guest_address, byte_offset, length)) {
        success = true;
    }

    if (!success) {
        ++g_stats.read_failures;
        return false;
    }

    g_stats.bytes_read += (u64)length;
    g_last_error[0] = '\0';
    return true;
}

bool smg3ds_disc_is_ready(void)
{
    return g_stats.initialized;
}

const Smg3dsDiscStats *smg3ds_disc_get_stats(void)
{
    return &g_stats;
}

const char *smg3ds_disc_last_error(void)
{
    return g_last_error;
}
