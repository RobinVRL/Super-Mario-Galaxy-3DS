// SPDX-License-Identifier: GPL-3.0-or-later
#include <3ds.h>

#include "cpu/cpu.h"
#include "smg3ds/disc.h"
#include "smg3ds/dol_loader.h"
#include "smg3ds/exi_hle.h"
#include "smg3ds/gx_renderer.h"
#include "smg3ds/hollywood_gpio.h"
#include "smg3ds/ios_hle.h"
#include "smg3ds/petari_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SMG3DS_WITH_GENERATED
#define DOLRECOMP_CPU_HEADER "cpu/cpu.h"
#define DOLRECOMP_ENABLE_REPLACEMENTS 1
#include "generated.h"
#endif

enum {
    SMG3DS_MEM2_SIZE = 64 * 1024 * 1024,
    SMG3DS_MEM2_PAGE_SHIFT = 12,
    SMG3DS_MEM2_PAGE_SIZE = 1 << SMG3DS_MEM2_PAGE_SHIFT,
    SMG3DS_MEM2_PAGE_COUNT = SMG3DS_MEM2_SIZE / SMG3DS_MEM2_PAGE_SIZE,
    SMG3DS_MAX_DISPATCHES_PER_FRAME = 4096,
    SMG3DS_CPU_SLICE_MILLISECONDS = 12,
    SMG3DS_DEBUG_LOG_FRAMES = 30,
    SMG3DS_TIMEBASE_PER_FRAME = 1012500,
    SMG3DS_TIMEBASE_FREQUENCY = 60750000,
    SMG3DS_BROADWAY_CYCLES_PER_TIMEBASE_TICK = 12,
    SMG3DS_INITIAL_STACK = 0x817fffc0,
    SMG3DS_WII_SETTINGS_ADDRESS = 0x80003800u,
    SMG3DS_WII_SETTINGS_SIZE = 256
};

static uint32_t g_mmio_reads;
static uint32_t g_mmio_writes;
static uint32_t g_last_mmio_read;
static uint32_t g_last_mmio_write;
static uint32_t g_last_mmio_value;
static uint32_t g_last_mmio_size;
static uint32_t g_gx_mmio_writes;
static uint32_t g_dsp_status_reads;
static uint32_t g_dsp_ready_mails;
static uint32_t g_dsp_mail_reads;
static uint32_t g_system_calls;
static uint32_t g_cache_range_skips;
static uint32_t g_case_compare_fast_paths;
static uint32_t g_yaz0_fast_paths;
static uint32_t g_yaz0_fast_bytes;
static uint32_t g_kpad_reads;
static uint32_t g_kpad_samples;
static u32 g_host_keys_held;
static u32 g_host_keys_latched_down;
static circlePosition g_host_circle;
static touchPosition g_host_touch;
static bool g_host_touch_active;
static u32 g_kpad_previous_hold;
static bool g_kpad_shake_phase;
static uint32_t g_debug_frames;
static uint32_t g_fault_raw;
static uint32_t g_fault_cia;
static uint8_t g_mmio_shadow[0x10000];
static uint8_t* g_mem2_pages[SMG3DS_MEM2_PAGE_COUNT];
static uint32_t g_mem2_pages_used;
static bool g_mem2_out_of_memory;
static uint16_t g_dsp_control = 0x0804u;
static uint16_t g_dsp_hardware_status;
static uint16_t g_pe_interrupt_control;
static uint8_t g_dsp_init_code_reads;
static uint32_t g_dsp_from_mail;
static uint32_t g_dsp_next_mail;
static uint32_t g_dsp_to_mail;
static uint32_t g_dsp_ready_mail;
static uint32_t g_dsp_boot_command;
static uint32_t g_dsp_packet_words;
static uint32_t g_dsp_packet_tag;
static uint32_t g_ipc_ppc_msg;
static uint32_t g_ipc_arm_msg;
static uint32_t g_ipc_control;
static uint32_t g_ipc_irq_flag;
static uint32_t g_ipc_irq_mask;
static uint32_t g_ipc_requests;
static bool g_ipc_external_pending;
static bool g_pi_external_pending;
static uint32_t g_pi_cause;
static uint32_t g_pi_mask;
static uint32_t g_vi_retraces;
static bool g_vi_demand_pacing;
static uint32_t g_external_interrupts;
static uint32_t g_decrementer_interrupts;
static bool g_decrementer_armed;
static bool g_decrementer_pending;
static uint32_t g_timebase_cycle_remainder;
static uint32_t g_ai_control;
static uint32_t g_ai_sample_count;
static u64 g_ai_sample_timebase;
static FILE* g_debug_file = NULL;

enum {
    SMG3DS_IPC_IRQ = 0x40000000u,
    SMG3DS_PI_EXI = 0x00000010u,
    SMG3DS_PI_DSP = 0x00000040u,
    SMG3DS_PI_VI = 0x00000100u,
    SMG3DS_PI_PE_TOKEN = 0x00000200u,
    SMG3DS_PI_PE_FINISH = 0x00000400u,
    SMG3DS_PI_WII_IPC = 0x00004000u,
    SMG3DS_VI_DI0_LOW = 0x0c002030u,
    SMG3DS_VI_DI1_LOW = 0x0c002034u,
    SMG3DS_VI_DI_ENABLE = 0x1000u,
    SMG3DS_VI_DI_PENDING = 0x8000u,
    SMG3DS_PE_INTERRUPT_CONTROL = 0x0c00100au,
    SMG3DS_MSR_EE = 0x00008000u,
    SMG3DS_MSR_FP = 0x00002000u,
    SMG3DS_PPC_EXC_EXTERNAL = 0x00000040u,
    SMG3DS_PPC_EXC_DECREMENTER = 0x00000080u,
    SMG3DS_PPC_VECTOR_EXTERNAL = 0x00000500u,
    SMG3DS_PPC_VECTOR_DECREMENTER = 0x00000900u,
    SMG3DS_OS_EXCEPTION_EXTERNAL = 4u,
    SMG3DS_OS_EXCEPTION_DECREMENTER = 8u,
    SMG3DS_OS_EXCEPTION_VECTOR_DISPATCH = 0x804A1D0Cu,
    SMG3DS_OS_PANIC = 0x804A3F24u,
    SMG3DS_PPC_HALT = 0x804A0970u,
    SMG3DS_OS_PANIC_HALT_RETURN = 0x804A4030u,
    SMG3DS_DVD_CONVERT_PATH_TO_ENTRYNUM = 0x804BEF5Cu,
    SMG3DS_DVD_FAST_OPEN = 0x804BF264u,
    SMG3DS_DVD_OPEN = 0x804BF2CCu,
    SMG3DS_DVD_READ_BOUNDS_RETURN = 0x804BF8C8u,
    SMG3DS_FILE_RIPPER_MISSING_RETURN = 0x80398678u,
    SMG3DS_FILE_RIPPER_AFTER_SAVE_GPRS = 0x8039862cu,
    SMG3DS_FILE_RIPPER_EXISTS_CALL = 0x80398654u,
    SMG3DS_FILE_RIPPER_EXISTS_RETURN = 0x8039865cu,
    SMG3DS_SETUP_RSO_HOME_BUTTON_MENU = 0x803ACA54u,
    SMG3DS_MR_IS_EQUAL_STRING_CASE = 0x803FD4C8u,
    SMG3DS_FILE_RIPPER_YAZ0_LOOP = 0x80398A20u,
    SMG3DS_FILE_RIPPER_YAZ0_EPILOGUE = 0x80398AECu,
    SMG3DS_KPAD_READ = 0x804506D8u,
    SMG3DS_CACHE_RANGE_BEGIN = 0x804A2F20u,
    SMG3DS_CACHE_RANGE_END = 0x804A2FD4u,
    SMG3DS_FILE_RIPPER_PATH_LIMIT = 256
};
enum {
    SMG3DS_KPAD_STATUS_SIZE = 132,
    SMG3DS_KPAD_BUTTON_LEFT = 0x0001u,
    SMG3DS_KPAD_BUTTON_RIGHT = 0x0002u,
    SMG3DS_KPAD_BUTTON_DOWN = 0x0004u,
    SMG3DS_KPAD_BUTTON_UP = 0x0008u,
    SMG3DS_KPAD_BUTTON_PLUS = 0x0010u,
    SMG3DS_KPAD_BUTTON_2 = 0x0100u,
    SMG3DS_KPAD_BUTTON_1 = 0x0200u,
    SMG3DS_KPAD_BUTTON_B = 0x0400u,
    SMG3DS_KPAD_BUTTON_A = 0x0800u,
    SMG3DS_KPAD_BUTTON_MINUS = 0x1000u,
    SMG3DS_KPAD_BUTTON_Z = 0x2000u,
    SMG3DS_KPAD_BUTTON_C = 0x4000u
};

static u32 f32_bits(float value)
{
    union {
        float value;
        u32 bits;
    } conversion;

    conversion.value = value;
    return conversion.bits;
}

static float clamp_unit(float value)
{
    if (value < -1.0f)
        return -1.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

static u32 map_host_kpad_buttons(u32 keys)
{
    u32 buttons = 0u;

    if ((keys & KEY_DLEFT) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_LEFT;
    if ((keys & KEY_DRIGHT) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_RIGHT;
    if ((keys & KEY_DDOWN) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_DOWN;
    if ((keys & KEY_DUP) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_UP;
    if ((keys & KEY_START) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_PLUS;
    if ((keys & KEY_Y) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_2;
    if ((keys & KEY_X) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_1;
    if ((keys & KEY_B) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_B;
    if ((keys & KEY_A) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_A;
    if ((keys & KEY_SELECT) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_MINUS;
    if ((keys & (KEY_L | KEY_ZL)) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_Z;
    if ((keys & (KEY_R | KEY_ZR)) != 0u)
        buttons |= SMG3DS_KPAD_BUTTON_C;
    return buttons;
}

static void update_host_controller(void)
{
    hidScanInput();
    g_host_keys_held = hidKeysHeld();
    g_host_keys_latched_down |= hidKeysDown();
    hidCircleRead(&g_host_circle);
    g_host_touch_active = (g_host_keys_held & KEY_TOUCH) != 0u;
    if (g_host_touch_active)
        hidTouchRead(&g_host_touch);
}


static bool guest_range_valid(const CPUState* cpu, u32 address, u32 size)
{
    const u32 physical = address & 0x3fffffffu;
    const u64 end = (u64)physical + size;

    if (size == 0u || end > 0x40000000ull)
        return false;
    if (physical < cpu->ram_size && end <= cpu->ram_size)
        return true;
    return physical >= 0x10000000u &&
           end <= 0x10000000ull + SMG3DS_MEM2_SIZE;
}
static bool service_kpad_read(CPUState* cpu)
{
    const u32 channel = cpu->gpr[3];
    const u32 status = cpu->gpr[4];
    const u32 capacity = cpu->gpr[5];
    const u32 host_keys = g_host_keys_held | g_host_keys_latched_down;
    u32 hold;
    float stick_x;
    float stick_y;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    float accel_x = 0.0f;

    ++g_kpad_reads;
    if (channel != 0u || capacity == 0u ||
        !guest_range_valid(cpu, status, SMG3DS_KPAD_STATUS_SIZE)) {
        cpu->gpr[3] = 0u;
        cpu->pc = cpu->lr & ~3u;
        return true;
    }

    for (u32 offset = 0u; offset < SMG3DS_KPAD_STATUS_SIZE; offset += 4u)
        mem_write32(cpu, status + offset, 0u);

    hold = map_host_kpad_buttons(host_keys);
    mem_write32(cpu, status + 0u, hold);
    mem_write32(cpu, status + 4u, hold & ~g_kpad_previous_hold);
    mem_write32(cpu, status + 8u, g_kpad_previous_hold & ~hold);
    g_kpad_previous_hold = hold;

    /* A stable, upright Wii Remote, with X also providing a shake/spin. */
    if ((host_keys & KEY_X) != 0u) {
        g_kpad_shake_phase = !g_kpad_shake_phase;
        accel_x = g_kpad_shake_phase ? 2.5f : -2.5f;
    }
    mem_write32(cpu, status + 12u, f32_bits(accel_x));
    mem_write32(cpu, status + 16u, f32_bits(0.0f));
    mem_write32(cpu, status + 20u, f32_bits(1.0f));
    mem_write32(cpu, status + 24u, f32_bits(1.0f));

    if (g_host_touch_active) {
        pointer_x = clamp_unit(((float)g_host_touch.px / 159.5f) - 1.0f);
        pointer_y = clamp_unit(1.0f - ((float)g_host_touch.py / 119.5f));
    }
    mem_write32(cpu, status + 32u, f32_bits(pointer_x));
    mem_write32(cpu, status + 36u, f32_bits(pointer_y));
    mem_write32(cpu, status + 48u, f32_bits(1.0f));
    mem_write32(cpu, status + 52u, f32_bits(0.0f));
    mem_write32(cpu, status + 56u, f32_bits(1.0f));

    /* KPAD device 1 is a Wii Remote with Nunchuk ("freestyle"). */
    mem_write8(cpu, status + 92u, 1u);
    mem_write8(cpu, status + 93u, 0u);
    mem_write8(cpu, status + 94u, g_host_touch_active ? 1u : 0u);
    stick_x = clamp_unit((float)g_host_circle.dx / 156.0f);
    stick_y = clamp_unit((float)g_host_circle.dy / 156.0f);
    mem_write32(cpu, status + 96u, f32_bits(stick_x));
    mem_write32(cpu, status + 100u, f32_bits(stick_y));

    g_host_keys_latched_down = 0u;
    ++g_kpad_samples;
    cpu->gpr[3] = 1u;
    cpu->pc = cpu->lr & ~3u;
    return true;
}


static bool guest_strings_equal_case(CPUState* cpu, u32 left, u32 right)
{
    enum { SMG3DS_GUEST_STRING_LIMIT = 4096 };

    if (left == 0u || right == 0u)
        return false;
    for (u32 offset = 0u; offset < SMG3DS_GUEST_STRING_LIMIT; ++offset) {
        u8 left_value;
        u8 right_value;

        if (!guest_range_valid(cpu, left + offset, 1u) ||
            !guest_range_valid(cpu, right + offset, 1u))
            return false;
        left_value = mem_read8(cpu, left + offset);
        right_value = mem_read8(cpu, right + offset);
        if (left_value >= (u8)'A' && left_value <= (u8)'Z')
            left_value = (u8)(left_value + ((u8)'a' - (u8)'A'));
        if (right_value >= (u8)'A' && right_value <= (u8)'Z')
            right_value = (u8)(right_value + ((u8)'a' - (u8)'A'));
        if (left_value != right_value)
            return false;
        if (left_value == 0u)
            return true;
    }
    return false;
}

static void guest_copy_string(CPUState* cpu, u32 address,
                              char* destination, size_t destination_size)
{
    size_t position = 0;

    if (destination_size == 0u)
        return;
    if (address == 0u || !guest_range_valid(cpu, address, 1u)) {
        snprintf(destination, destination_size, "<invalid:%08lX>",
                 (unsigned long)address);
        return;
    }

    while (position + 1u < destination_size &&
           guest_range_valid(cpu, address + (u32)position, 1u)) {
        const u8 value = mem_read8(cpu, address + (u32)position);
        if (value == 0u)
            break;
        destination[position++] =
            value >= 0x20u && value <= 0x7eu ? (char)value : '?';
    }
    destination[position] = '\0';
}

static bool canonicalize_guest_disc_path(CPUState* cpu, u32 address)
{
    u8 normalized[SMG3DS_FILE_RIPPER_PATH_LIMIT];
    u32 read_offset = 0u;
    u32 write_offset = 0u;
    bool previous_was_separator = false;
    bool changed = false;

    while (read_offset < SMG3DS_FILE_RIPPER_PATH_LIMIT &&
           guest_range_valid(cpu, address + read_offset, 1u)) {
        const u8 value = mem_read8(cpu, address + read_offset++);

        if (value == 0u) {
            if (changed) {
                normalized[write_offset] = 0u;
                for (u32 offset = 0u; offset <= write_offset; ++offset)
                    mem_write8(cpu, address + offset, normalized[offset]);
            }
            return changed;
        }
        if (value == (u8)'/' && previous_was_separator) {
            changed = true;
            continue;
        }
        normalized[write_offset++] = value;
        previous_was_separator = value == (u8)'/';
    }

    return false;
}

static bool recover_file_ripper_separator_panic(CPUState* cpu)
{
    if (cpu->pc != SMG3DS_OS_PANIC ||
        cpu->lr != SMG3DS_FILE_RIPPER_MISSING_RETURN)
        return false;
    if (!canonicalize_guest_disc_path(cpu, cpu->gpr[26]))
        return false;

    /*
     * The failed existence check is immediately followed by OSPanic.  Once
     * its malformed path is corrected, resume at the panic's return address;
     * FileRipper reloads r3 from r26 there before opening the asset.
     */
    cpu->pc = cpu->lr;
    return true;
}

#ifdef SMG3DS_WITH_GENERATED
int dolrecomp_dispatch_replacement(CPUState* cpu, u32 address)
{
    s32 entry_index;
    bool opened;

    if (address == SMG3DS_KPAD_READ)
        return service_kpad_read(cpu) ? 1 : 0;

    if (smg3ds_petari_dispatch(cpu, address))
        return 1;

    if (address == SMG3DS_MR_IS_EQUAL_STRING_CASE) {
        cpu->gpr[3] = guest_strings_equal_case(cpu, cpu->gpr[3],
                                                 cpu->gpr[4]) ? 1u : 0u;
        cpu->pc = cpu->lr & ~3u;
        ++g_case_compare_fast_paths;
        return 1;
    }

    if (address == SMG3DS_DVD_CONVERT_PATH_TO_ENTRYNUM) {
        if (!smg3ds_disc_resolve_path(cpu, cpu->gpr[3], &entry_index))
            return 0;
        cpu->gpr[3] = (u32)entry_index;
    } else if (address == SMG3DS_DVD_FAST_OPEN) {
        if (!smg3ds_disc_open_entry(cpu, (s32)cpu->gpr[3], cpu->gpr[4],
                                    &opened)) {
            return 0;
        }
        cpu->gpr[3] = opened ? 1u : 0u;
    } else if (address == SMG3DS_DVD_OPEN) {
        if (!smg3ds_disc_open_path(cpu, cpu->gpr[3], cpu->gpr[4], &opened))
            return 0;
        cpu->gpr[3] = opened ? 1u : 0u;
    } else {
        return 0;
    }

    cpu->pc = cpu->lr & ~3u;
    return 1;
}
#endif

typedef struct Smg3dsGuestPanic {
    u32 file_address;
    u32 line;
    u32 message_address;
    u32 caller;
    u32 arguments[5];
    u32 asset_path_address;
    u32 request_caller;
    u32 dvd_file_info;
    u32 dvd_destination;
    u32 dvd_length;
    u32 dvd_offset;
    u32 dvd_start;
    u32 dvd_file_size;
} Smg3dsGuestPanic;

static void decode_dvd_read_context(CPUState* cpu, Smg3dsGuestPanic* panic)
{
    if (panic->caller != SMG3DS_DVD_READ_BOUNDS_RETURN)
        return;

    panic->dvd_file_info = cpu->gpr[27];
    panic->dvd_destination = cpu->gpr[28];
    panic->dvd_length = cpu->gpr[29];
    panic->dvd_offset = cpu->gpr[31];
    if (guest_range_valid(cpu, panic->dvd_file_info, 0x38u)) {
        panic->dvd_start = mem_read32(cpu, panic->dvd_file_info + 0x30u);
        panic->dvd_file_size =
            mem_read32(cpu, panic->dvd_file_info + 0x34u);
    }
}

static void decode_file_ripper_context(CPUState* cpu,
                                       Smg3dsGuestPanic* panic,
                                       u32 file_ripper_frame)
{
    panic->asset_path_address = 0u;
    panic->request_caller = 0u;
    if (panic->caller != SMG3DS_FILE_RIPPER_MISSING_RETURN)
        return;

    /*
     * FileRipper's synchronous worker preserves its input path in r26.
     * Its 0xd0-byte frame stores the worker caller just above the frame.
     */
    panic->asset_path_address = cpu->gpr[26];
    if (guest_range_valid(cpu, file_ripper_frame, 0xd8u))
        panic->request_caller = mem_read32(cpu, file_ripper_frame + 0xd4u);
}

static bool decode_guest_panic(CPUState* cpu, Smg3dsGuestPanic* panic)
{
    if (cpu->pc == SMG3DS_OS_PANIC) {
        panic->file_address = cpu->gpr[3];
        panic->line = cpu->gpr[4];
        panic->message_address = cpu->gpr[5];
        panic->caller = cpu->lr;
        for (u32 index = 0; index < 5u; ++index)
            panic->arguments[index] = cpu->gpr[6u + index];
        decode_file_ripper_context(cpu, panic, cpu->gpr[1]);
        decode_dvd_read_context(cpu, panic);
        return true;
    }

    if (cpu->pc == SMG3DS_PPC_HALT &&
        cpu->lr == SMG3DS_OS_PANIC_HALT_RETURN &&
        guest_range_valid(cpu, cpu->gpr[1], 0x98u)) {
        const u32 stack = cpu->gpr[1];
        panic->file_address = mem_read32(cpu, stack + 0x08u);
        panic->line = mem_read32(cpu, stack + 0x0cu);
        panic->message_address = mem_read32(cpu, stack + 0x10u);
        panic->caller = mem_read32(cpu, stack + 0x94u);
        for (u32 index = 0; index < 5u; ++index) {
            panic->arguments[index] =
                mem_read32(cpu, stack + 0x14u + index * 4u);
        }
        decode_file_ripper_context(cpu, panic, mem_read32(cpu, stack));
        decode_dvd_read_context(cpu, panic);
        return true;
    }

    return false;
}

static bool report_guest_panic(CPUState* cpu)
{
    Smg3dsGuestPanic panic = {0};
    char file[160];
    char message[256];
    char asset_path[256];
    char report[1024];
    int length;

    if (!decode_guest_panic(cpu, &panic))
        return false;
    guest_copy_string(cpu, panic.file_address, file, sizeof(file));
    guest_copy_string(cpu, panic.message_address, message, sizeof(message));
    asset_path[0] = '\0';
    if (panic.asset_path_address != 0u) {
        guest_copy_string(cpu, panic.asset_path_address, asset_path,
                          sizeof(asset_path));
    }
    length = snprintf(report, sizeof(report),
        "GUEST PANIC caller=%08lX at %s:%lu\nformat: %s\n",
        (unsigned long)panic.caller, file, (unsigned long)panic.line, message);
    if (length > 0 && length < (int)sizeof(report)) {
        length += snprintf(report + length, sizeof(report) - (size_t)length,
            "args: %08lX %08lX %08lX %08lX %08lX\n",
            (unsigned long)panic.arguments[0],
            (unsigned long)panic.arguments[1],
            (unsigned long)panic.arguments[2],
            (unsigned long)panic.arguments[3],
            (unsigned long)panic.arguments[4]);
    }
    if (length > 0 && length < (int)sizeof(report) &&
        panic.asset_path_address != 0u) {
        length += snprintf(report + length, sizeof(report) - (size_t)length,
            "asset: %s\nasset-address=%08lX request-caller=%08lX\n",
            asset_path, (unsigned long)panic.asset_path_address,
            (unsigned long)panic.request_caller);
    }
    if (length > 0 && length < (int)sizeof(report) &&
        panic.dvd_file_info != 0u) {
        length += snprintf(report + length, sizeof(report) - (size_t)length,
            "dvd info=%08lX start=%08lX size=%08lX\n"
            "dvd dst=%08lX length=%08lX offset=%08lX\n",
            (unsigned long)panic.dvd_file_info,
            (unsigned long)panic.dvd_start,
            (unsigned long)panic.dvd_file_size,
            (unsigned long)panic.dvd_destination,
            (unsigned long)panic.dvd_length,
            (unsigned long)panic.dvd_offset);
    }
    if (length > 0) {
        if (length >= (int)sizeof(report))
            length = (int)sizeof(report) - 1;
        svcOutputDebugString(report, length);
        if (g_debug_file != NULL) {
            fwrite(report, 1, (size_t)length, g_debug_file);
            fflush(g_debug_file);
        }
    }
    printf("GUEST PANIC: %.26s\n", message);
    printf("%s:%lu caller=%08lX\n", file, (unsigned long)panic.line,
           (unsigned long)panic.caller);
    if (panic.asset_path_address != 0u) {
        printf("asset: %.31s\n", asset_path);
        printf("request caller=%08lX\n",
               (unsigned long)panic.request_caller);
    }
    if (panic.dvd_file_info != 0u) {
        printf("dvd sz=%08lX len=%08lX off=%08lX\n",
               (unsigned long)panic.dvd_file_size,
               (unsigned long)panic.dvd_length,
               (unsigned long)panic.dvd_offset);
    }
    return true;
}

static uint32_t mmio_physical(uint32_t address)
{
    return address & 0x3fffffffu;
}

static void advance_timebase(CPUState* cpu, u64 ticks)
{
    const u32 old_decrementer = cpu->spr[22];

    cpu->timebase += ticks;
    if (!g_decrementer_armed || ticks == 0u)
        return;

    cpu->spr[22] = old_decrementer - (u32)ticks;
    if (old_decrementer < 0x80000000u && ticks > old_decrementer)
        g_decrementer_pending = true;
}

static void advance_timebase_from_execution(CPUState* cpu)
{
    u64 guest_cycles;
    u64 accumulated_cycles;
    u64 ticks;

    if (cpu->downcount >= 0)
        return;
    guest_cycles = (u64)(-(cpu->downcount + 1)) + 1u;
    accumulated_cycles = guest_cycles + g_timebase_cycle_remainder;
    ticks = accumulated_cycles / SMG3DS_BROADWAY_CYCLES_PER_TIMEBASE_TICK;
    advance_timebase(cpu, ticks);
    g_timebase_cycle_remainder = (uint32_t)(
        accumulated_cycles % SMG3DS_BROADWAY_CYCLES_PER_TIMEBASE_TICK);
}

static bool service_coherent_cache_range(CPUState* cpu)
{
    if (cpu->pc < SMG3DS_CACHE_RANGE_BEGIN ||
        cpu->pc > SMG3DS_CACHE_RANGE_END)
        return false;

    cpu->ctr = 0u;
    cpu->pc = cpu->lr & ~3u;
    cpu->downcount = -8;
    ++g_cache_range_skips;
    return true;
}

static bool service_yaz0_decompression(CPUState* cpu)
{
    enum { SMG3DS_YAZ0_OUTPUT_BUDGET = 64 * 1024 };
    u32 source;
    u32 destination;
    const u32 destination_end = cpu->gpr[30];
    u32 bits_remaining;
    u32 code;
    u32 produced = 0u;

    if (cpu->pc != SMG3DS_FILE_RIPPER_YAZ0_LOOP ||
        cpu->gpr[13] < 10344u)
        return false;
    source = cpu->gpr[3];
    destination = cpu->gpr[31];
    bits_remaining = cpu->gpr[7];
    code = cpu->gpr[8];
    if (destination > destination_end)
        return false;

    /*
     * FileRipper's Yaz0 decoder spends most of scene loading interpreting
     * one byte at a time. Decode the current streaming input window natively,
     * then return to readSrcDataNext whenever the guest buffer needs refilling.
     * Completing whole tokens preserves overlapping back-references exactly.
     */
    while (destination < destination_end &&
           produced < SMG3DS_YAZ0_OUTPUT_BUDGET) {
        if (bits_remaining == 0u) {
            const u32 source_end = mem_read32(cpu, cpu->gpr[13] - 10344u);

            if (source > source_end)
                break;
            if (!guest_range_valid(cpu, source, 1u))
                return false;
            code = mem_read8(cpu, source++);
            bits_remaining = 8u;
        }

        if ((code & 0x80u) != 0u) {
            if (!guest_range_valid(cpu, source, 1u) ||
                !guest_range_valid(cpu, destination, 1u))
                return false;
            mem_write8(cpu, destination++, mem_read8(cpu, source++));
            ++produced;
        } else {
            u32 first;
            u32 second;
            u32 length;
            u32 copy_source;

            if (!guest_range_valid(cpu, source, 2u))
                return false;
            first = mem_read8(cpu, source++);
            second = mem_read8(cpu, source++);
            copy_source = destination - (((first & 0x0fu) << 8) | second) - 1u;
            length = first >> 4;
            if (length == 0u) {
                if (!guest_range_valid(cpu, source, 1u))
                    return false;
                length = mem_read8(cpu, source++) + 18u;
            } else {
                length += 2u;
            }
            while (length-- != 0u && destination < destination_end) {
                if (!guest_range_valid(cpu, copy_source, 1u) ||
                    !guest_range_valid(cpu, destination, 1u))
                    return false;
                mem_write8(cpu, destination++, mem_read8(cpu, copy_source++));
                ++produced;
            }
        }
        code = (code << 1) & 0xffu;
        --bits_remaining;
    }

    cpu->gpr[3] = source;
    cpu->gpr[7] = bits_remaining;
    cpu->gpr[8] = code;
    cpu->gpr[31] = destination;
    if (destination >= destination_end) {
        cpu->gpr[3] = 1u;
        cpu->pc = SMG3DS_FILE_RIPPER_YAZ0_EPILOGUE;
    } else if (produced == 0u) {
        return false;
    } else {
        cpu->pc = SMG3DS_FILE_RIPPER_YAZ0_LOOP;
    }
    cpu->downcount = -8;
    ++g_yaz0_fast_paths;
    g_yaz0_fast_bytes += produced;
    return true;
}

static uint32_t ai_current_sample_count(const CPUState* cpu)
{
    const u32 sample_rate = (g_ai_control & 0x02u) != 0u ? 48000u : 32000u;
    const u64 elapsed = cpu->timebase - g_ai_sample_timebase;

    if ((g_ai_control & 0x01u) == 0u)
        return g_ai_sample_count;
    return g_ai_sample_count + (u32)(
        (elapsed * sample_rate) / SMG3DS_TIMEBASE_FREQUENCY);
}

static void ai_write_control(CPUState* cpu, u32 value)
{
    const u32 interrupt_status = g_ai_control & 0x08u;

    g_ai_sample_count = ai_current_sample_count(cpu);
    g_ai_sample_timebase = cpu->timebase;
    if ((value & 0x20u) != 0u)
        g_ai_sample_count = 0u;

    /* SCRESET is a write pulse and AIINT is write-one-to-clear. */
    g_ai_control = value & 0x57u;
    if ((value & 0x08u) == 0u)
        g_ai_control |= interrupt_status;
}

static bool mem2_offset(uint32_t address, uint8_t size, uint32_t* offset)
{
    const uint32_t physical = mmio_physical(address);
    if (physical < 0x10000000u ||
        physical + size > 0x10000000u + SMG3DS_MEM2_SIZE)
        return false;
    *offset = physical - 0x10000000u;
    return true;
}

static uint64_t mem2_read_sparse(uint32_t offset, uint8_t size)
{
    uint64_t value = 0;
    uint8_t i;
    for (i = 0; i < size; ++i) {
        const uint32_t current = offset + i;
        const uint32_t page = current >> SMG3DS_MEM2_PAGE_SHIFT;
        const uint32_t in_page = current & (SMG3DS_MEM2_PAGE_SIZE - 1u);
        const uint8_t byte = g_mem2_pages[page] == NULL ? 0u :
                             g_mem2_pages[page][in_page];
        value = (value << 8) | byte;
    }
    return value;
}

static void mem2_write_sparse(uint32_t offset, uint64_t value, uint8_t size)
{
    uint8_t i;
    for (i = 0; i < size; ++i) {
        const uint32_t current = offset + i;
        const uint32_t page = current >> SMG3DS_MEM2_PAGE_SHIFT;
        const uint32_t in_page = current & (SMG3DS_MEM2_PAGE_SIZE - 1u);
        const uint8_t byte =
            (uint8_t)(value >> ((size - i - 1u) * 8u));
        if (g_mem2_pages[page] == NULL) {
            if (byte == 0u)
                continue;
            g_mem2_pages[page] =
                (uint8_t*)linearMemAlign(SMG3DS_MEM2_PAGE_SIZE, 0x80u);
            if (g_mem2_pages[page] != NULL)
                memset(g_mem2_pages[page], 0, SMG3DS_MEM2_PAGE_SIZE);
            if (g_mem2_pages[page] == NULL) {
                g_mem2_out_of_memory = true;
                return;
            }
            ++g_mem2_pages_used;
        }
        g_mem2_pages[page][in_page] = byte;
    }
}

static void mem2_free_sparse(void)
{
    uint32_t page;
    for (page = 0; page < SMG3DS_MEM2_PAGE_COUNT; ++page) {
        linearFree(g_mem2_pages[page]);
        g_mem2_pages[page] = NULL;
    }
    g_mem2_pages_used = 0;
}

static uint64_t mmio_shadow_read(uint32_t physical, uint8_t size)
{
    uint64_t value = 0;
    uint32_t offset;
    uint8_t i;
    if (physical < 0x0c000000u || physical + size > 0x0c010000u)
        return 0;
    /*
     * Keep the MMIO subtraction out of the relocatable array address.
     * Folding g_mmio_shadow - 0x0c000000 produces a negative absolute
     * relocation which the 3DSX loader correctly rejects.
     */
    offset = physical & 0xffffu;
    for (i = 0; i < size; ++i)
        value = (value << 8) | g_mmio_shadow[offset + i];
    return value;
}

static void mmio_shadow_write(uint32_t physical, uint64_t value, uint8_t size)
{
    uint32_t offset;
    uint8_t i;
    if (physical < 0x0c000000u || physical + size > 0x0c010000u)
        return;
    offset = physical & 0xffffu;
    for (i = 0; i < size; ++i)
        g_mmio_shadow[offset + i] =
            (uint8_t)(value >> ((size - i - 1u) * 8u));
}

static bool vi_interrupt_pending(void)
{
    const uint16_t di0 =
        (uint16_t)mmio_shadow_read(SMG3DS_VI_DI0_LOW, 2u);
    const uint16_t di1 =
        (uint16_t)mmio_shadow_read(SMG3DS_VI_DI1_LOW, 2u);
    const uint16_t armed = SMG3DS_VI_DI_ENABLE | SMG3DS_VI_DI_PENDING;
    return (di0 & armed) == armed || (di1 & armed) == armed;
}

static uint32_t pi_interrupt_cause(void)
{
    uint32_t cause = g_pi_cause;
    if (vi_interrupt_pending())
        cause |= SMG3DS_PI_VI;
    else
        cause &= ~SMG3DS_PI_VI;
    if ((g_pe_interrupt_control & 0x05u) == 0x05u)
        cause |= SMG3DS_PI_PE_TOKEN;
    else
        cause &= ~SMG3DS_PI_PE_TOKEN;
    if ((g_pe_interrupt_control & 0x0au) == 0x0au)
        cause |= SMG3DS_PI_PE_FINISH;
    else
        cause &= ~SMG3DS_PI_PE_FINISH;
    if (smg3ds_exi_interrupt_pending())
        cause |= SMG3DS_PI_EXI;
    else
        cause &= ~SMG3DS_PI_EXI;
    if (((g_dsp_control | g_dsp_hardware_status) & 0x0180u) == 0x0180u)
        cause |= SMG3DS_PI_DSP;
    else
        cause &= ~SMG3DS_PI_DSP;
    if (g_ipc_irq_flag != 0u)
        cause |= SMG3DS_PI_WII_IPC;
    return cause;
}

static bool is_gx_mmio_access(uint32_t physical)
{
    return (physical & ~0x1fu) == 0x0c008000u;
}

static void update_pi_external_pending(void)
{
    g_pi_external_pending = (pi_interrupt_cause() & g_pi_mask) != 0u;
}

static void signal_video_retrace(void)
{
    uint32_t address = (g_vi_retraces & 1u) == 0u ?
        SMG3DS_VI_DI0_LOW : SMG3DS_VI_DI1_LOW;
    uint16_t control = (uint16_t)mmio_shadow_read(address, 2u);

    ++g_vi_retraces;
    if ((control & SMG3DS_VI_DI_ENABLE) == 0u) {
        address = address == SMG3DS_VI_DI0_LOW ?
            SMG3DS_VI_DI1_LOW : SMG3DS_VI_DI0_LOW;
        control = (uint16_t)mmio_shadow_read(address, 2u);
    }
    if ((control & SMG3DS_VI_DI_ENABLE) != 0u) {
        control |= SMG3DS_VI_DI_PENDING;
        mmio_shadow_write(address, control, 2u);
    }
    update_pi_external_pending();
}
static u32 guest_retrace_queue(CPUState* cpu)
{
    u32 framework;

    if (cpu->gpr[13] == 0u)
        return 0u;
    framework = mem_read32(cpu, cpu->gpr[13] - 9296u);
    if (!guest_range_valid(cpu, framework, 88u))
        return 0u;
    return framework + 56u;
}

static void observe_guest_retrace_wait(CPUState* cpu)
{
    const u32 queue = guest_retrace_queue(cpu);

    /*
     * MainLoopFramework::waitForTick receives retraces through this blocking
     * OSMessageQueue. Once that path is live, pace VI delivery to the guest's
     * demand instead of the much faster host render loop. Otherwise a retrace
     * is always queued before OSReceiveMessage and the main thread never sleeps,
     * starving lower-priority scene-loading workers.
     */
    if (cpu->pc == 0x804A89ACu && cpu->gpr[5] != 0u &&
        queue != 0u && cpu->gpr[3] == queue)
        g_vi_demand_pacing = true;
}

static bool guest_is_waiting_for_retrace(CPUState* cpu)
{
    const u32 queue = guest_retrace_queue(cpu);

    if (queue == 0u)
        return false;
    /* OSMessageQueue::queueReceive is the OSThreadQueue at offset 8. */
    return mem_read32(cpu, queue + 8u) != 0u;
}


static bool service_optional_platform_module(CPUState* cpu)
{
    const u32 callback_offset = cpu->pc - 0x803ACB88u;

    /*
     * The Wii Home Button overlay is an optional dynamically loaded RSO.
     * It has no 3DS-side input path and its runtime code is outside the
     * statically recompiled DOL, so leave setup and its seven callback
     * trampolines inactive.
     */
    if (cpu->pc == SMG3DS_SETUP_RSO_HOME_BUTTON_MENU) {
        cpu->pc = cpu->lr & ~3u;
        return true;
    }
    if (callback_offset > 0x48u || callback_offset % 0x0cu != 0u)
        return false;

    cpu->gpr[3] = 0u;
    cpu->pc = cpu->lr & ~3u;
    return true;
}

static void ipc_signal(u32 interrupt_enable_mask)
{
    g_ipc_irq_flag |= SMG3DS_IPC_IRQ;
    if ((g_ipc_control & interrupt_enable_mask) != 0u)
        g_ipc_external_pending = true;
}

static void ipc_reply_request(CPUState* cpu, u32 request, s32 result,
                              bool acknowledge)
{
    mem_write32(cpu, request + 4u, (u32)result);
    g_ipc_arm_msg = request;
    if (acknowledge) {
        g_ipc_control &= ~1u;
        g_ipc_control |= 0x02u; /* Y2 acknowledgement. */
    }
    g_ipc_control |= 0x04u; /* Y1 reply. */
    ipc_signal(acknowledge ? 0x30u : 0x10u);
}

static void ipc_receive_request(CPUState* cpu)
{
    const u32 request = g_ipc_ppc_msg;
    s32 result = 0;
    const Smg3dsIosDisposition disposition =
        smg3ds_ios_handle_request(cpu, request, &result);

    ++g_ipc_requests;
    if (disposition == SMG3DS_IOS_PARK) {
        /* Async STM eventhooks are acknowledged now and replied to later. */
        g_ipc_control &= ~1u;
        g_ipc_control |= 0x02u;
        ipc_signal(0x20u);
        return;
    }
    ipc_reply_request(cpu, request, result, true);
}

static void service_deferred_ipc(CPUState* cpu)
{
    u32 request;
    s32 result;

    /*
     * Starlet may publish a Y1 reply only after Broadway has consumed the
     * preceding Y2 acknowledgement and cleared its IPC interrupt.  Sending
     * both phases together collapses two level-triggered interrupts into one;
     * asynchronous clients then observe the acknowledgement but never run the
     * completion callback for the reply.
     */
    if ((g_ipc_control & 0x07u) != 0u || g_ipc_irq_flag != 0u)
        return;
    if (smg3ds_ios_take_deferred_reply(&request, &result))
        ipc_reply_request(cpu, request, result, false);
}

static void dispatch_os_exception(CPUState* cpu, u32 ppc_exception,
                                  u32 vector, u32 os_exception)
{
    u32 context;
    u32 interrupted_r3;
    u32 interrupted_r4;
    u32 interrupted_r5;
    u32 interrupted_srr0;
    u32 interrupted_srr1;

    interrupted_r3 = cpu->gpr[3];
    interrupted_r4 = cpu->gpr[4];
    interrupted_r5 = cpu->gpr[5];
    ppc_take_exception(cpu, ppc_exception, vector, cpu->pc, 0u);
    interrupted_srr0 = cpu->srr0;
    interrupted_srr1 = cpu->srr1;

    /*
     * OSExceptionInit copies the vector template to 0x500 and patches its
     * embedded exception number. Copied RAM is not executable by the static
     * dispatcher, while jumping to the original template reintroduces
     * exception zero and reaches the SDK's fatal default handler. Reproduce
     * the template prefix here, then resume at its shared handler lookup with
     * the correct exception number.
     */
    context = mem_read32(cpu, 0x000000c0u);
    mem_write32(cpu, context + 12u, interrupted_r3);
    mem_write32(cpu, context + 16u, interrupted_r4);
    mem_write32(cpu, context + 20u, interrupted_r5);
    mem_write16(cpu, context + 418u,
                (u16)(mem_read16(cpu, context + 418u) | 0x0002u));
    mem_write32(cpu, context + 128u, cpu->cr);
    mem_write32(cpu, context + 132u, cpu->lr);
    mem_write32(cpu, context + 136u, cpu->ctr);
    mem_write32(cpu, context + 140u, cpu->xer);
    mem_write32(cpu, context + 408u, interrupted_srr0);
    mem_write32(cpu, context + 412u, interrupted_srr1);

    cpu->gpr[3] = os_exception;
    cpu->gpr[5] = interrupted_srr1;
    cpu->srr1 = cpu->msr | 0x00000030u;
    cpu->spr[272] = interrupted_r4;
    cpu->exception = 0u;
    cpu->pc = SMG3DS_OS_EXCEPTION_VECTOR_DISPATCH;
}

static void service_external_interrupt(CPUState* cpu)
{
    if ((!g_ipc_external_pending && !g_pi_external_pending) ||
        (cpu->msr & SMG3DS_MSR_EE) == 0u)
        return;
    g_ipc_external_pending = false;
    g_pi_external_pending = false;
    ++g_external_interrupts;
    dispatch_os_exception(cpu, SMG3DS_PPC_EXC_EXTERNAL,
                          SMG3DS_PPC_VECTOR_EXTERNAL,
                          SMG3DS_OS_EXCEPTION_EXTERNAL);
}

static void service_decrementer_interrupt(CPUState* cpu)
{
    if (!g_decrementer_pending || (cpu->msr & SMG3DS_MSR_EE) == 0u)
        return;
    g_decrementer_pending = false;
    ++g_decrementer_interrupts;
    dispatch_os_exception(cpu, SMG3DS_PPC_EXC_DECREMENTER,
                          SMG3DS_PPC_VECTOR_DECREMENTER,
                          SMG3DS_OS_EXCEPTION_DECREMENTER);
}

static u64 external_read(CPUState* cpu, u32 address, u8 size)
{
    const u32 physical = mmio_physical(address);
    u32 mem2_address;
    u64 value;
    if (mem2_offset(address, size, &mem2_address))
        return mem2_read_sparse(mem2_address, size);
    g_last_mmio_read = address;
    g_last_mmio_size = size;
    ++g_mmio_reads;
    if (smg3ds_exi_handles(physical, size))
        return smg3ds_exi_read(cpu, physical, size);
    if (smg3ds_hollywood_gpio_handles(physical, size))
        return smg3ds_hollywood_gpio_read(physical, size,
                                          smg3ds_disc_is_ready());
    if (physical == 0x0c006024u || physical == 0x0d006024u)
        return 1u;
    if (physical == 0x0d000000u)
        return g_ipc_ppc_msg;
    if (physical == 0x0d000004u)
        return g_ipc_control;
    if (physical == 0x0d000008u)
        return g_ipc_arm_msg;
    if (physical == 0x0d00000cu)
        return 0u;
    if (physical == 0x0d000030u)
        return g_ipc_irq_flag;
    if (physical == 0x0d000034u)
        return g_ipc_irq_mask;
    if (physical == 0x0d006c00u && size == 4u)
        return g_ai_control;
    if (physical == 0x0d006c08u && size == 4u)
        return ai_current_sample_count(cpu);
    if (physical == 0x0c003000u)
        return pi_interrupt_cause();
    if (physical == 0x0c003004u)
        return g_pi_mask;
    if (physical == 0x0c00500au) {
        const u16 status = g_dsp_control | g_dsp_hardware_status;
        ++g_dsp_status_reads;
        if (g_dsp_init_code_reads != 0u &&
            --g_dsp_init_code_reads == 0u)
            g_dsp_hardware_status &= (u16)~0x0400u;
        return status;
    }
    if (physical == 0x0c005000u)
        return g_dsp_to_mail >> 16;
    if (physical == 0x0c005004u)
        return g_dsp_from_mail >> 16;
    if (physical == 0x0c005006u) {
        value = g_dsp_from_mail & 0xffffu;
        if (g_dsp_next_mail != 0u) {
            g_dsp_from_mail = g_dsp_next_mail;
            g_dsp_next_mail = 0u;
        } else {
            g_dsp_from_mail &= 0x7fffffffu;
        }
        ++g_dsp_mail_reads;
        return value;
    }
    if (physical == 0x0c005016u)
        return 1u;
    if (physical == 0x0c00501au)
        return 156u;
    if (physical == SMG3DS_PE_INTERRUPT_CONTROL && size == 2u)
        return g_pe_interrupt_control;


    return mmio_shadow_read(physical, size);
}

static void external_write(CPUState* cpu, u32 address, u64 value, u8 size)
{
    const u32 physical = mmio_physical(address);
    u32 mem2_address;
    if (mem2_offset(address, size, &mem2_address)) {
        mem2_write_sparse(mem2_address, value, size);
        return;
    }
    g_last_mmio_write = address;
    g_last_mmio_value = (uint32_t)value;
    g_last_mmio_size = size;
    ++g_mmio_writes;
    if (is_gx_mmio_access(physical)) {
        ++g_gx_mmio_writes;
        smg3ds_gx_fifo_write(value, size);
        if (smg3ds_gx_take_finish_request()) {
            g_pe_interrupt_control |= 0x08u;
            update_pi_external_pending();
        }
        return;
    }
    if (smg3ds_exi_handles(physical, size)) {
        smg3ds_exi_write(cpu, physical, value, size);
        update_pi_external_pending();
        return;
    }
    if (smg3ds_hollywood_gpio_handles(physical, size)) {
        smg3ds_hollywood_gpio_write(physical, value, size);
        return;
    }
    if (physical == 0x0d000000u && size == 4u) {
        g_ipc_ppc_msg = (u32)value;
        return;
    }
    if (physical == 0x0d000004u && size == 4u) {
        const u32 requested = (u32)value;
        g_ipc_control = (g_ipc_control & 0x06u) | (requested & 0x39u);
        if ((requested & 0x02u) != 0u)
            g_ipc_control &= ~0x02u;
        if ((requested & 0x04u) != 0u) {
            g_ipc_control &= ~0x04u;
        }
        /* A parked async request has Y2 but no Y1, so acknowledging Y2
           must also release the level-triggered IPC interrupt. */
        if ((g_ipc_control & 0x06u) == 0u)
            g_ipc_irq_flag &= ~SMG3DS_IPC_IRQ;
        update_pi_external_pending();
        if ((requested & 1u) != 0u)
            ipc_receive_request(cpu);
        return;
    }
    if (physical == 0x0d000030u && size == 4u) {
        g_ipc_irq_flag &= ~(u32)value;
        update_pi_external_pending();
        return;
    }
    if (physical == 0x0d000034u && size == 4u) {
        g_ipc_irq_mask = (u32)value;
        return;
    }
    if (physical == 0x0d006c00u && size == 4u) {
        ai_write_control(cpu, (u32)value);
        return;
    }
    if (physical == 0x0d006c08u && size == 4u) {
        g_ai_sample_count = (u32)value;
        g_ai_sample_timebase = cpu->timebase;
        return;
    }
    if (physical == 0x0c003000u && size == 4u) {
        g_pi_cause &= ~(u32)value;
        update_pi_external_pending();
        return;
    }
    if (physical == 0x0c003004u && size == 4u) {
        g_pi_mask = (u32)value;
        update_pi_external_pending();
        return;
    }
    if (physical == 0x0c005000u && size == 2u) {
        g_dsp_to_mail = ((u32)value << 16) | (g_dsp_to_mail & 0xffffu);
        return;
    }
    if (physical == 0x0c005002u && size == 2u) {
        const u32 mail = (g_dsp_to_mail & 0xffff0000u) |
                         (u32)(u16)value | 0x80000000u;

        switch (mail) {
        case 0x80f3a001u:
        case 0x80f3c002u:
        case 0x80f3a002u:
        case 0x80f3b002u:
        case 0x80f3d001u:
            g_dsp_boot_command = mail;
            break;
        default:
            if (g_dsp_boot_command != 0u) {
                if (g_dsp_boot_command == 0x80f3d001u) {
                    g_dsp_from_mail = 0xdcd10000u;
                    g_dsp_next_mail = 0xf3551111u;
                    g_dsp_hardware_status |= 0x0080u;
                    ++g_dsp_ready_mails;
                }
                g_dsp_boot_command = 0u;
            } else if (g_dsp_packet_words == 0u &&
                       (mail & 0x7fffffffu) <= 16u) {
                g_dsp_packet_words = mail & 0x7fffffffu;
                g_dsp_packet_tag = 0u;
            } else if (g_dsp_packet_words != 0u) {
                if (g_dsp_packet_tag == 0u)
                    g_dsp_packet_tag = (mail >> 16) & 0xffffu;
                if (--g_dsp_packet_words == 0u) {
                    g_dsp_from_mail = 0xdcd10004u;
                    g_dsp_next_mail = 0xf3550000u | g_dsp_packet_tag;
                    g_dsp_hardware_status |= 0x0080u;
                    ++g_dsp_ready_mails;
                }
            }
            break;
        }
        g_dsp_to_mail = mail & 0x7fffffffu;
        g_dsp_control &= (u16)~0x0002u;
        update_pi_external_pending();
        return;
    }
    if (physical == SMG3DS_PE_INTERRUPT_CONTROL && size == 2u) {
        const u16 requested = (u16)value;
        u16 pending = g_pe_interrupt_control & 0x0cu;
        pending &= (u16)~(requested & 0x0cu);
        g_pe_interrupt_control = (requested & 0x03u) | pending;
        update_pi_external_pending();
        return;
    }
    mmio_shadow_write(physical, value, size);

    if (physical >= 0x0c002000u && physical < 0x0c003000u) {
        update_pi_external_pending();
        return;
    }
    if (physical == 0x0c00500au && size == 2u) {
        const u16 requested = (u16)value;
        const u16 old_control = g_dsp_control;
        const u16 write_one_to_clear = 0x00a8u;
        g_dsp_hardware_status &= (u16)~(requested & write_one_to_clear);
        g_dsp_control = requested &
            (u16)~(write_one_to_clear | 0x0001u | 0x0600u);
        if ((requested & 0x0001u) != 0u)
            g_dsp_ready_mail = 0x8071feedu;
        if ((old_control & 0x0800u) != 0u &&
            (requested & 0x0800u) == 0u) {
            g_dsp_ready_mail = 0x80544348u;
            g_dsp_hardware_status |= 0x0400u;
            g_dsp_init_code_reads = 1u;
        }
        if (g_dsp_ready_mail != 0u && (old_control & 0x0004u) != 0u &&
            (requested & 0x0004u) == 0u) {
            g_dsp_from_mail = g_dsp_ready_mail;
            ++g_dsp_ready_mails;
            g_dsp_ready_mail = 0u;
        }
        update_pi_external_pending();
        return;
    }
    if ((physical == 0x0c00502au && size == 2u) ||
        (physical == 0x0c005028u && size == 4u)) {
        g_dsp_hardware_status &= (u16)~0x0200u;
        g_dsp_hardware_status |= 0x0020u;
    }
}

static void instruction_fallback(CPUState* cpu, u32 raw, u32 cia)
{
    const u32 opcode = raw >> 26;
    const u32 xo = (raw >> 1) & 0x3ffu;
    const u32 reg = (raw >> 21) & 31u;
    const u16 spr = (u16)(((raw >> 16) & 31u) | ((raw >> 6) & 0x3e0u));

    if (opcode == 31u && xo == 339u) {
        cpu->gpr[reg] = ppc_mfspr(cpu, spr, cia);
        if (!cpu->exception) {
            cpu->pc = cia + 4u;
        } else {
            g_fault_raw = raw;
            g_fault_cia = cia;
        }
        return;
    }
    if (opcode == 31u && xo == 467u) {
        ppc_mtspr(cpu, spr, cpu->gpr[reg], cia);
        if (!cpu->exception) {
            if (spr == 22u) {
                g_decrementer_armed = true;
                g_decrementer_pending = false;
            }
            cpu->pc = cia + 4u;
        } else {
            g_fault_raw = raw;
            g_fault_cia = cia;
        }
        return;
    }
    if (opcode == 31u &&
        (xo == 54u || xo == 86u || xo == 470u || xo == 982u)) {
        const u32 ra = (raw >> 16) & 31u;
        const u32 rb = (raw >> 11) & 31u;
        const u32 address = (ra ? cpu->gpr[ra] : 0u) + cpu->gpr[rb];
        const u8 operation =
            xo == 54u  ? PPC_CACHE_DCBST :
            xo == 86u  ? PPC_CACHE_DCBF :
            xo == 470u ? PPC_CACHE_DCBI : PPC_CACHE_ICBI;
        ppc_cache_control(cpu, operation, address, cia);
        if (!cpu->exception)
            cpu->pc = cia + 4u;
        return;
    }
    g_fault_raw = raw;
    g_fault_cia = cia;
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

static bool initialize_cpu(CPUState* cpu)
{
    if (!cpu_init(cpu))
        return false;
    cpu->external_read = external_read;
    cpu->external_write = external_write;
    cpu->instruction_fallback = instruction_fallback;
    cpu->gpr[1] = SMG3DS_INITIAL_STACK;
    smg3ds_gx_init(cpu);
    return true;
}

static void initialize_wii_settings(CPUState* cpu)
{
    static const char settings[] =
        "AREA=USA\r\n"
        "MODEL=RVL-001(USA)\r\n"
        "DVD=0\r\n"
        "MPCH=0x7FFE\r\n"
        "CODE=LU\r\n"
        "SERNO=123456789\r\n"
        "VIDEO=NTSC\r\n"
        "GAME=US\r\n";
    u32 key = 0x73b5dbfau;
    size_t position;

    /*
     * The Wii bootloader places an encrypted setting.txt image here before
     * entering the title.  SMG uses its region fields to select a locale
     * directory, so an all-zero image can make this NTSC-U disc request a
     * locale that does not exist in its FST.
     */
    for (position = 0; position < SMG3DS_WII_SETTINGS_SIZE; ++position)
        mem_write8(cpu, SMG3DS_WII_SETTINGS_ADDRESS + (u32)position, 0u);
    for (position = 0; position + 1u < sizeof(settings); ++position) {
        mem_write8(cpu, SMG3DS_WII_SETTINGS_ADDRESS + (u32)position,
                   (u8)settings[position] ^ (u8)key);
        key = (key << 1u) | (key >> 31u);
    }
}

static void initialize_wii_low_memory(CPUState* cpu)
{
    mem_write32(cpu, 0x80000028u, 0x01800000u);
    mem_write32(cpu, 0x80000030u, 0x00000000u);
    mem_write32(cpu, 0x80000034u, 0x81800000u);
    mem_write32(cpu, 0x800000f0u, 0x01800000u);
    mem_write32(cpu, 0x800000f8u, 243000000u);
    mem_write32(cpu, 0x800000fcu, 729000000u);

    /* IOS33 v1040 boot-info and MEM2/IPC bounds used by this title. */
    mem_write32(cpu, 0x80003100u, 0x01800000u);
    mem_write32(cpu, 0x80003104u, 0x01800000u);
    mem_write32(cpu, 0x80003108u, 0x81800000u);
    mem_write32(cpu, 0x8000310cu, 0x00000000u);
    mem_write32(cpu, 0x80003110u, 0x81800000u);
    mem_write32(cpu, 0x80003114u, 0xdeadbeefu);
    mem_write32(cpu, 0x80003118u, 0x04000000u);
    mem_write32(cpu, 0x8000311cu, 0x04000000u);
    mem_write32(cpu, 0x80003120u, 0x93600000u);
    mem_write32(cpu, 0x80003124u, 0x90000800u);
    mem_write32(cpu, 0x80003128u, 0x935e0000u);
    mem_write32(cpu, 0x8000312cu, 0xdeadbeefu);
    mem_write32(cpu, 0x80003130u, 0x935e0000u);
    mem_write32(cpu, 0x80003134u, 0x93600000u);
    mem_write32(cpu, 0x80003138u, 0x00000011u);
    mem_write32(cpu, 0x8000313cu, 0xdeadbeefu);
    mem_write32(cpu, 0x80003140u, 0x00210e18u);
    mem_write32(cpu, 0x80003144u, 0x00030110u);
    mem_write32(cpu, 0x80003148u, 0x93600000u);
    mem_write32(cpu, 0x8000314cu, 0x93620000u);
    mem_write32(cpu, 0x80003150u, 0xdeadbeefu);
    mem_write32(cpu, 0x80003154u, 0xdeadbeefu);
    mem_write32(cpu, 0x80003158u, 0x0000ff01u);
    mem_write32(cpu, 0x8000315cu, 0x80ad0113u);
    mem_write32(cpu, 0x80003160u, 0x00000000u);
    initialize_wii_settings(cpu);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    printf("\x1b[1;1HSMG3DS / Azahar bring-up\n");
    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    printf("Hardware mode: %s\n", is_new_3ds ? "New 3DS" : "Old 3DS");
    printf("DolRecomp CPU core: linked\n");
    printf("CPUState: %lu bytes\n", (unsigned long)sizeof(CPUState));

    CPUState cpu;
    const bool mem1_ok = initialize_cpu(&cpu);
    smg3ds_exi_init();
    smg3ds_hollywood_gpio_init();
    smg3ds_ios_init();
    const Smg3dsGxStats* initial_gx = smg3ds_gx_get_stats();
    const bool geometry_ok = mem1_ok && initial_gx->geometry_self_test_passed;
    const bool exi_ok = mem1_ok && smg3ds_exi_self_test(&cpu);
    const bool gpio_ok = smg3ds_hollywood_gpio_self_test();
    printf("MEM1 (24 MiB): %s\n", mem1_ok ? "ok" : "FAILED");
    printf("software rasterizer: %s\n", geometry_ok ? "geometry verified" : "FAILED");
    printf("EXI/RTC/SRAM: %s\n",
           exi_ok ? "transactions verified" : "FAILED");
    printf("Hollywood GPIO/I2C: %s\n",
           gpio_ok ? "registers verified" : "FAILED");
    bool mem2_ok = false;
    if (mem1_ok) {
        mem_write32(&cpu, 0x80001000u, 0x534d4733u);
        initialize_wii_low_memory(&cpu);
        printf("big-endian memory: %s\n",
               mem_read32(&cpu, 0x80001000u) == 0x534d4733u ? "ok" : "FAILED");
        /* MEM2 is committed in 64 KiB pages as the guest touches it. */
        mem2_ok = true;
        printf("MEM2 (64 MiB): %s\n", mem2_ok ? "ok" : "unavailable");
    }

#ifdef SMG3DS_WITH_GENERATED
    bool dol_ok = false;
    bool disc_ok = false;
    bool running = false;
    u32 total_dispatches = 0;
    const Result sdmc_mount = archiveMountSdmc();
    FILE* debug_file = fopen("sdmc:/smg3ds-runtime.log", "w");
    g_debug_file = debug_file;
    const bool romfs_ok = mem1_ok && R_SUCCEEDED(romfsInit());
    if (romfs_ok) {
        char error[96];
        Smg3dsDolInfo dol;
        disc_ok = smg3ds_disc_init(&cpu);
        printf("Disc assets: %s\n", disc_ok ? "indexed" :
               smg3ds_disc_last_error());
        dol_ok = smg3ds_load_dol("romfs:/game/main.dol", &cpu, &dol,
                                error, sizeof(error));
        if (dol_ok && !smg3ds_disc_publish_guest_state(&cpu)) {
            snprintf(error, sizeof(error), "disc state: %s",
                     smg3ds_disc_last_error());
            dol_ok = false;
        }
        printf("DOL: %s\n", error);
        if (dol_ok) {
            printf("entry %08lX, %lu sections\n", (unsigned long)dol.entry_point,
                   (unsigned long)dol.loaded_sections);
            running = geometry_ok && exi_ok && gpio_ok && disc_ok;
            printf("PPC execution: %s\n",
                   running ? "auto-started" :
                   "blocked: renderer/EXI/GPIO/disc preflight failed");
        }
    }
#else
    printf("\nGenerated game code: not configured\n");
    printf("Run tools/configure.ps1 with your DOL.\n");
#endif
    while (aptMainLoop()) {
        update_host_controller();
#ifdef SMG3DS_WITH_GENERATED
        if (running && dol_ok) {
            const u64 frame_timebase_target =
                cpu.timebase + SMG3DS_TIMEBASE_PER_FRAME;
            const u64 cpu_slice_deadline =
                osGetTime() + SMG3DS_CPU_SLICE_MILLISECONDS;
            if (!g_vi_demand_pacing ||
                (guest_is_waiting_for_retrace(&cpu) &&
                 !vi_interrupt_pending()))
                signal_video_retrace();
            for (u32 slice = 0;
                 slice < SMG3DS_MAX_DISPATCHES_PER_FRAME && running;
                 ++slice) {
                /*
                 * A generated block may cover a sizeable guest function.  Keep
                 * at least one block per frame, then yield before another block
                 * can starve presentation, input, and runtime diagnostics.
                 */
                if (slice != 0u && osGetTime() >= cpu_slice_deadline)
                    break;
                observe_guest_retrace_wait(&cpu);
                service_deferred_ipc(&cpu);
                service_external_interrupt(&cpu);
                service_decrementer_interrupt(&cpu);
                recover_file_ripper_separator_panic(&cpu);
                if (report_guest_panic(&cpu)) {
                    running = false;
                    break;
                }
                /*
                 * FileRipper can queue paths with adjacent separators (for
                 * example /AudioRes//SMR.szs).  Canonicalize its private
                 * request buffer before the SDK's strict FST lookup.
                 */
                if (cpu.pc == SMG3DS_FILE_RIPPER_AFTER_SAVE_GPRS)
                    canonicalize_guest_disc_path(&cpu, cpu.gpr[3]);
                /*
                 * Calls that enter FileRipper from the same generated chunk
                 * may bypass the entry-side dispatcher hook above.  The DVD
                 * existence query necessarily returns through the dispatcher,
                 * and by then FileRipper has preserved the path in r26.  If
                 * normalization changes it, repeat only that query with the
                 * corrected path instead of continuing with its stale failure.
                 */
                if (cpu.pc == SMG3DS_FILE_RIPPER_EXISTS_RETURN &&
                    canonicalize_guest_disc_path(&cpu, cpu.gpr[26])) {
                    cpu.gpr[3] = cpu.gpr[26];
                    cpu.pc = SMG3DS_FILE_RIPPER_EXISTS_CALL;
                }
                int dispatched;
                cpu.downcount = 0;
                if (service_yaz0_decompression(&cpu))
                    dispatched = 1;
                else if (service_optional_platform_module(&cpu))
                    dispatched = 1;
                else if (service_coherent_cache_range(&cpu))
                    dispatched = 1;
                else
                    dispatched = dolrecomp_run_blocks(&cpu, 1u);
                advance_timebase_from_execution(&cpu);
                bool handled_system_call = false;
                if (cpu.exception == PPC_EXC_SYSTEM_CALL) {
                    ++g_system_calls;
                    cpu.exception = 0;
                    ppc_rfi(&cpu, PPC_VECTOR_SYSTEM_CALL);
                    handled_system_call = true;
                }
                if (cpu.exception == PPC_EXC_FP_UNAVAILABLE) {
                    cpu.exception = 0u;
                    cpu.msr = cpu.srr1 | SMG3DS_MSR_FP;
                    cpu.pc = cpu.srr0;
                    handled_system_call = true;
                }
                if (dispatched || handled_system_call)
                    ++total_dispatches;
                const bool stopped =
                    (!dispatched && !handled_system_call) || cpu.exception;
                if (stopped) {
                    const u32 fault_pc = cpu.exception ? cpu.srr0 : cpu.pc;
                    const u32 fault_raw =
                        g_fault_cia == fault_pc ? g_fault_raw : 0u;
                    char fault_message[512];
                    int fault_length = snprintf(
                        fault_message, sizeof(fault_message),
                        "STOP pc=%08lX ex=%08lX srr0=%08lX srr1=%08lX "
                        "cause=%08lX lr=%08lX raw=%08lX\n"
                        "IPC n=%lu req=%08lX cmd=%lu ctl=%02lX flag=%08lX "
                        "mask=%08lX\n"
                        "V5 %08lX %08lX %08lX %08lX H4=%08lX\n",
                        (unsigned long)cpu.pc,
                        (unsigned long)cpu.exception,
                        (unsigned long)cpu.srr0,
                        (unsigned long)cpu.srr1,
                        (unsigned long)cpu.program_exception,
                        (unsigned long)cpu.lr,
                        (unsigned long)fault_raw,
                        (unsigned long)g_ipc_requests,
                        (unsigned long)g_ipc_ppc_msg,
                        (unsigned long)(g_ipc_ppc_msg != 0u ?
                            mem_read32(&cpu, g_ipc_ppc_msg) : 0u),
                        (unsigned long)g_ipc_control,
                        (unsigned long)g_ipc_irq_flag,
                        (unsigned long)g_ipc_irq_mask,
                        (unsigned long)mem_read32(&cpu, 0x80000500u),
                        (unsigned long)mem_read32(&cpu, 0x80000504u),
                        (unsigned long)mem_read32(&cpu, 0x80000508u),
                        (unsigned long)mem_read32(&cpu, 0x8000050cu),
                        (unsigned long)mem_read32(&cpu, 0x80003010u));
                    if (fault_length > 0) {
                        if (fault_length >= (int)sizeof(fault_message))
                            fault_length = (int)sizeof(fault_message) - 1;
                        svcOutputDebugString(fault_message, fault_length);
                        if (debug_file != NULL) {
                            fwrite(fault_message, 1, (size_t)fault_length,
                                   debug_file);
                            fflush(debug_file);
                        }
                    }
                    running = false;
                }
            }
            if (total_dispatches != 0u) {
                const Smg3dsGxStats* gx = smg3ds_gx_get_stats();
                const Smg3dsExiStats* exi = smg3ds_exi_get_stats();
                const Smg3dsIosStats* ios = smg3ds_ios_get_stats();
                printf("\x1b[17;1Hpc=%08lX ex=%lu blk=%lu\x1b[K",
                       (unsigned long)cpu.pc, (unsigned long)cpu.exception,
                       (unsigned long)total_dispatches);
                printf("\x1b[18;1Hmmio r=%lu w=%lu gxw=%lu size=%lu\x1b[K",
                       (unsigned long)g_mmio_reads,
                       (unsigned long)g_mmio_writes,
                       (unsigned long)g_gx_mmio_writes,
                       (unsigned long)g_last_mmio_size);
                printf("\x1b[19;1Hread=%08lX write=%08lX\x1b[K",
                       (unsigned long)g_last_mmio_read,
                       (unsigned long)g_last_mmio_write);
                printf("\x1b[20;1Hsrr0=%08lX srr1=%08lX\x1b[K",
                       (unsigned long)cpu.srr0, (unsigned long)cpu.srr1);
                printf("\x1b[21;1Hcause=%08lX lr=%08lX\x1b[K",
                       (unsigned long)cpu.program_exception,
                       (unsigned long)cpu.lr);
                printf("\x1b[22;1Hdec=%08lX msr=%08lX\x1b[K",
                       (unsigned long)cpu.spr[22],
                       (unsigned long)cpu.msr);
                printf("\x1b[23;1Hctl=%04lX hw=%04lX fifo=%lu\x1b[K",
                       (unsigned long)g_dsp_control,
                       (unsigned long)g_dsp_hardware_status,
                       (unsigned long)gx->fifo_bytes);
                printf("\x1b[24;1Hgx d=%lu tri=%lu pix=%lu/%lu xfb=%lu\x1b[K",
                       (unsigned long)gx->draw_calls,
                       (unsigned long)gx->triangles,
                       (unsigned long)gx->rasterized_pixels,
                       (unsigned long)gx->colored_pixels,
                       (unsigned long)gx->xfb_colored_pixels);
                if (++g_debug_frames >= SMG3DS_DEBUG_LOG_FRAMES) {
                    const u32 run_queue_addr =
                        cpu.gpr[13] + (u32)(s32)(-8048);
                    const u32 run_queue_bits =
                        mem_read32(&cpu, run_queue_addr);
                    const u32 game_system = mem_read32(
                        &cpu, cpu.gpr[13] + (u32)(s32)(-14968));
                    const u32 game_spine =
                        guest_range_valid(&cpu, game_system, 12u) ?
                            mem_read32(&cpu, game_system + 4u) : 0u;
                    const u32 game_nerve =
                        guest_range_valid(&cpu, game_spine, 16u) ?
                            mem_read32(&cpu, game_spine + 4u) : 0u;
                    const u32 game_nerve_vtable =
                        guest_range_valid(&cpu, game_nerve, 4u) ?
                            mem_read32(&cpu, game_nerve) : 0u;
                    const u32 game_nerve_execute =
                        guest_range_valid(&cpu, game_nerve_vtable, 12u) ?
                            mem_read32(&cpu, game_nerve_vtable + 8u) : 0u;
                    const u32 game_next_nerve =
                        guest_range_valid(&cpu, game_spine, 16u) ?
                            mem_read32(&cpu, game_spine + 8u) : 0u;
                    const u32 game_nerve_step =
                        guest_range_valid(&cpu, game_spine, 16u) ?
                            mem_read32(&cpu, game_spine + 12u) : 0u;
                    const u32 scene_controller =
                        guest_range_valid(&cpu, game_system, 40u) ?
                            mem_read32(&cpu, game_system + 36u) : 0u;
                    const u32 scene_spine =
                        guest_range_valid(&cpu, scene_controller, 156u) ?
                            mem_read32(&cpu, scene_controller + 152u) : 0u;
                    const u32 scene_nerve =
                        guest_range_valid(&cpu, scene_spine, 16u) ?
                            mem_read32(&cpu, scene_spine + 4u) : 0u;
                    const u32 scene_nerve_vtable =
                        guest_range_valid(&cpu, scene_nerve, 4u) ?
                            mem_read32(&cpu, scene_nerve) : 0u;
                    const u32 scene_nerve_execute =
                        guest_range_valid(&cpu, scene_nerve_vtable, 12u) ?
                            mem_read32(&cpu, scene_nerve_vtable + 8u) : 0u;
                    const u32 scene_next_nerve =
                        guest_range_valid(&cpu, scene_spine, 16u) ?
                            mem_read32(&cpu, scene_spine + 8u) : 0u;
                    const u32 scene_nerve_step =
                        guest_range_valid(&cpu, scene_spine, 16u) ?
                            mem_read32(&cpu, scene_spine + 12u) : 0u;
                    const u32 scene_object =
                        guest_range_valid(&cpu, scene_controller, 180u) ?
                            mem_read32(&cpu, scene_controller + 164u) : 0u;
                    const u32 scene_init_state =
                        guest_range_valid(&cpu, scene_controller, 180u) ?
                            mem_read32(&cpu, scene_controller + 176u) : 0u;
                    const u32 game_obj_holder =
                        guest_range_valid(&cpu, game_system, 36u) ?
                            mem_read32(&cpu, game_system + 32u) : 0u;
                    const u32 async_executor =
                        guest_range_valid(&cpu, game_obj_holder, 44u) ?
                            mem_read32(&cpu, game_obj_holder + 40u) : 0u;
                    const u32 async_worker0 =
                        guest_range_valid(&cpu, async_executor, 8u) ?
                            mem_read32(&cpu, async_executor) : 0u;
                    const u32 async_worker1 =
                        guest_range_valid(&cpu, async_executor, 8u) ?
                            mem_read32(&cpu, async_executor + 4u) : 0u;
                    const u32 async_thread0 =
                        guest_range_valid(&cpu, async_worker0, 12u) ?
                            mem_read32(&cpu, async_worker0 + 8u) : 0u;
                    const u32 async_thread1 =
                        guest_range_valid(&cpu, async_worker1, 12u) ?
                            mem_read32(&cpu, async_worker1 + 8u) : 0u;
                    const u32 async_job_count =
                        guest_range_valid(&cpu, async_executor, 1040u) ?
                            mem_read32(&cpu, async_executor + 1036u) : 0u;
                    const u32 async_job0 =
                        async_job_count != 0u ?
                            mem_read32(&cpu, async_executor + 12u) : 0u;
                    const u8 async_job0_done =
                        guest_range_valid(&cpu, async_job0, 13u) ?
                            mem_read8(&cpu, async_job0 + 12u) : 0u;
                    const u16 async_thread0_state =
                        guest_range_valid(&cpu, async_thread0, 0x304u) ?
                            mem_read16(&cpu, async_thread0 + 0x2c8u) : 0u;
                    const u16 async_thread1_state =
                        guest_range_valid(&cpu, async_thread1, 0x304u) ?
                            mem_read16(&cpu, async_thread1 + 0x2c8u) : 0u;
                    const u32 async_thread0_pc =
                        guest_range_valid(&cpu, async_thread0, 0x304u) ?
                            mem_read32(&cpu, async_thread0 + 408u) : 0u;
                    const u32 async_thread1_pc =
                        guest_range_valid(&cpu, async_thread1, 0x304u) ?
                            mem_read32(&cpu, async_thread1 + 408u) : 0u;
                    const u8 async_worker0_busy =
                        guest_range_valid(&cpu, async_worker0, 61u) ?
                            mem_read8(&cpu, async_worker0 + 60u) : 0u;
                    const u8 async_worker1_busy =
                        guest_range_valid(&cpu, async_worker1, 61u) ?
                            mem_read8(&cpu, async_worker1 + 60u) : 0u;
                    const u32 vector_900_0 =
                        mem_read32(&cpu, 0x80000900u);
                    const u32 vector_900_1 =
                        mem_read32(&cpu, 0x80000904u);
                    const u32 current_thread =
                        mem_read32(&cpu, 0x800000e4u);
                    u32 thread_ptrs[4] = {0u, 0u, 0u, 0u};
                    u32 thread_pcs[4] = {0u, 0u, 0u, 0u};
                    u32 thread_queues[4] = {0u, 0u, 0u, 0u};
                    u32 thread_callers[4][3] = {{0u}};
                    u16 thread_states[4] = {0u, 0u, 0u, 0u};
                    u32 active_thread = mem_read32(&cpu, 0x800000dcu);
                    const char* ios_path = smg3ds_ios_fd_path(ios->last_fd);
                    char message[2048];
                    int length;
                    for (u32 i = 0u; i < 4u; ++i) {
                        if (!guest_range_valid(&cpu, active_thread, 0x304u))
                            break;
                        thread_ptrs[i] = active_thread;
                        thread_pcs[i] = mem_read32(&cpu, active_thread + 408u);
                        thread_states[i] = mem_read16(&cpu,
                                                     active_thread + 0x2c8u);
                        thread_queues[i] = mem_read32(&cpu,
                                                     active_thread + 0x2dcu);
                        u32 frame = mem_read32(&cpu, active_thread + 4u);
                        for (u32 depth = 0u; depth < 3u; ++depth) {
                            if (!guest_range_valid(&cpu, frame, 8u))
                                break;
                            thread_callers[i][depth] =
                                mem_read32(&cpu, frame + 4u);
                            const u32 next_frame = mem_read32(&cpu, frame);
                            if (next_frame == frame)
                                break;
                            frame = next_frame;
                        }
                        active_thread = mem_read32(&cpu,
                                                  active_thread + 0x2fcu);
                    }
                    g_debug_frames = 0;
                    length = snprintf(
                        message, sizeof(message),
                        "SMG3DS pc=%08lX ex=%lu blk=%lu ctl=%04lX hw=%04lX "
                        "val=%04lX mmio=%lu gxw=%lu w=%lu fifo=%lu cmd=%lu "
                        "draw=%lu tri=%lu pix=%lu/%lu xfb=%lu vtx=%lu clip=%lu dl=%lu "
                        "trunc=%lu copy=%lu/%lu fail=%lu self=%u "
                        "cw=%lu tx=%lu zr=%lu zm=%lX bm=%lX "
                        "vc=%08lX tex=%06lX/%06lX td=%lu/%lu/%lu "
                        "ts=%lux%lu>%lux%lu/%lu@%08lX "
                        "tev=%lX/%lX/%lX/%lX ttev=%lX/%lX/%lX/%lX "
                        "mem2=%lu oom=%u "
                        "dec=%08lX dirq=%lu msr=%08lX tb=%08lX:%08lX r13=%08lX "
                        "gs=%08lX:%08lX/%08lX/%08lX/%08lX/%lu "
                        "sn=%08lX:%08lX/%08lX/%08lX/%08lX/%lu/%08lX/%lu "
                        "ax=%08lX/%lu/%08lX/%u w=%08lX:%08lX/%04lX/%08lX/%u,%08lX:%08lX/%04lX/%08lX/%u "
                        "rq=%08lX:%08lX v9=%08lX/%08lX last=%08lX/%08lX "
                        "sc=%lu cache=%lu cmp=%lu yz=%lu/%lu kp=%lu/%lu/%04lX dsp=%lu dm=%08lX/%lu/%lu dt=%08lX/%08lX "
                        "lr=%08lX ctr=%08lX r31=%08lX "
                        "cur=%08lX th=%08lX:%04lX/%08lX/%08lX/"
                        "%08lX/%08lX/%08lX,"
                        "%08lX:%04lX/%08lX/%08lX/%08lX/%08lX/%08lX,"
                        "%08lX:%04lX/%08lX/%08lX/%08lX/%08lX/%08lX,"
                        "%08lX:%04lX/%08lX/%08lX/%08lX/%08lX/%08lX "
                        "ipc=%lu ios=%lu@%08lX fd=%ld ret=%ld io=%08lX "
                        "bt=%04lX/%lu/%lu/%lu:%08lX "
                        "path=%s bad=%lu/%lu exi=%lu/%lu xcmd=%08lX xs=%u "
                        "vi=%lu irq=%lu pi=%08lX/%08lX "
                        "di=%04lX/%04lX\n",
                        (unsigned long)cpu.pc,
                        (unsigned long)cpu.exception,
                        (unsigned long)total_dispatches,
                        (unsigned long)g_dsp_control,
                        (unsigned long)g_dsp_hardware_status,
                        (unsigned long)(g_last_mmio_value & 0xffffu),
                        (unsigned long)g_mmio_reads,
                        (unsigned long)g_gx_mmio_writes,
                        (unsigned long)g_mmio_writes,
                        (unsigned long)gx->fifo_bytes,
                        (unsigned long)gx->commands,
                        (unsigned long)gx->draw_calls,
                        (unsigned long)gx->triangles,
                        (unsigned long)gx->rasterized_pixels,
                        (unsigned long)gx->colored_pixels,
                        (unsigned long)gx->xfb_colored_pixels,
                        (unsigned long)gx->vertices,
                        (unsigned long)gx->clipped_vertices,
                        (unsigned long)gx->display_lists,
                        (unsigned long)gx->incomplete_commands,
                        (unsigned long)gx->efb_copies,
                        (unsigned long)gx->xfb_copies,
                        (unsigned long)gx->decode_failures,
                        gx->geometry_self_test_passed ? 1u : 0u,
                        (unsigned long)gx->color_write_draws,
                        (unsigned long)gx->textured_draws,
                        (unsigned long)gx->depth_rejected_pixels,
                        (unsigned long)gx->last_z_mode,
                        (unsigned long)gx->last_blend_mode,
                        (unsigned long)gx->last_vertex_color,
                        (unsigned long)gx->last_texture_image0,
                        (unsigned long)gx->last_texture_image3,
                        (unsigned long)gx->texture_decodes,
                        (unsigned long)gx->texture_samples,
                        (unsigned long)gx->unsupported_texture_formats,
                        (unsigned long)gx->last_texture_width,
                        (unsigned long)gx->last_texture_height,
                        (unsigned long)gx->last_texture_storage_width,
                        (unsigned long)gx->last_texture_storage_height,
                        (unsigned long)gx->last_texture_format,
                        (unsigned long)gx->last_texture_base,
                        (unsigned long)gx->last_gen_mode,
                        (unsigned long)gx->last_tev_order,
                        (unsigned long)gx->last_tev_color_env,
                        (unsigned long)gx->last_tev_alpha_env,
                        (unsigned long)gx->textured_gen_mode,
                        (unsigned long)gx->textured_tev_order,
                        (unsigned long)gx->textured_tev_color_env,
                        (unsigned long)gx->textured_tev_alpha_env,
                        (unsigned long)g_mem2_pages_used,
                        g_mem2_out_of_memory ? 1u : 0u,
                        (unsigned long)cpu.spr[22],
                        (unsigned long)g_decrementer_interrupts,
                        (unsigned long)cpu.msr,
                        (unsigned long)(cpu.timebase >> 32),
                        (unsigned long)cpu.timebase,
                        (unsigned long)cpu.gpr[13],
                        (unsigned long)game_system,
                        (unsigned long)game_nerve,
                        (unsigned long)game_nerve_vtable,
                        (unsigned long)game_nerve_execute,
                        (unsigned long)game_next_nerve,
                        (unsigned long)game_nerve_step,
                        (unsigned long)scene_controller,
                        (unsigned long)scene_nerve,
                        (unsigned long)scene_nerve_vtable,
                        (unsigned long)scene_nerve_execute,
                        (unsigned long)scene_next_nerve,
                        (unsigned long)scene_nerve_step,
                        (unsigned long)scene_object,
                        (unsigned long)scene_init_state,
                        (unsigned long)async_executor,
                        (unsigned long)async_job_count,
                        (unsigned long)async_job0,
                        (unsigned int)async_job0_done,
                        (unsigned long)async_worker0,
                        (unsigned long)async_thread0,
                        (unsigned long)async_thread0_state,
                        (unsigned long)async_thread0_pc,
                        (unsigned int)async_worker0_busy,
                        (unsigned long)async_worker1,
                        (unsigned long)async_thread1,
                        (unsigned long)async_thread1_state,
                        (unsigned long)async_thread1_pc,
                        (unsigned int)async_worker1_busy,
                        (unsigned long)run_queue_addr,
                        (unsigned long)run_queue_bits,
                        (unsigned long)vector_900_0,
                        (unsigned long)vector_900_1,
                        (unsigned long)g_last_mmio_read,
                        (unsigned long)g_last_mmio_write,
                        (unsigned long)g_system_calls,
                        (unsigned long)g_cache_range_skips,
                        (unsigned long)g_case_compare_fast_paths,
                        (unsigned long)g_yaz0_fast_paths,
                        (unsigned long)g_yaz0_fast_bytes,
                        (unsigned long)g_kpad_reads,
                        (unsigned long)g_kpad_samples,
                        (unsigned long)g_kpad_previous_hold,
                        (unsigned long)g_dsp_status_reads,
                        (unsigned long)g_dsp_from_mail,
                        (unsigned long)g_dsp_ready_mails,
                        (unsigned long)g_dsp_mail_reads,
                        (unsigned long)g_dsp_to_mail,
                        (unsigned long)g_dsp_boot_command,
                        (unsigned long)cpu.lr,
                        (unsigned long)cpu.ctr,
                        (unsigned long)cpu.gpr[31],
                        (unsigned long)current_thread,
                        (unsigned long)thread_ptrs[0],
                        (unsigned long)thread_states[0],
                        (unsigned long)thread_pcs[0],
                        (unsigned long)thread_queues[0],
                        (unsigned long)thread_callers[0][0],
                        (unsigned long)thread_callers[0][1],
                        (unsigned long)thread_callers[0][2],
                        (unsigned long)thread_ptrs[1],
                        (unsigned long)thread_states[1],
                        (unsigned long)thread_pcs[1],
                        (unsigned long)thread_queues[1],
                        (unsigned long)thread_callers[1][0],
                        (unsigned long)thread_callers[1][1],
                        (unsigned long)thread_callers[1][2],
                        (unsigned long)thread_ptrs[2],
                        (unsigned long)thread_states[2],
                        (unsigned long)thread_pcs[2],
                        (unsigned long)thread_queues[2],
                        (unsigned long)thread_callers[2][0],
                        (unsigned long)thread_callers[2][1],
                        (unsigned long)thread_callers[2][2],
                        (unsigned long)thread_ptrs[3],
                        (unsigned long)thread_states[3],
                        (unsigned long)thread_pcs[3],
                        (unsigned long)thread_queues[3],
                        (unsigned long)thread_callers[3][0],
                        (unsigned long)thread_callers[3][1],
                        (unsigned long)thread_callers[3][2],
                        (unsigned long)g_ipc_requests,
                        (unsigned long)ios->last_ipc_command,
                        (unsigned long)ios->last_request,
                        (long)ios->last_fd,
                        (long)ios->last_result,
                        (unsigned long)ios->last_ioctl_request,
                        (unsigned long)ios->last_bt_opcode,
                        (unsigned long)ios->bt_events_queued,
                        (unsigned long)ios->bt_events_delivered,
                        (unsigned long)ios->last_bt_event_size,
                        (unsigned long)ios->last_bt_event_word,
                        ios_path != NULL ? ios_path : "-",
                        (unsigned long)ios->invalid_requests,
                        (unsigned long)ios->unknown_requests,
                        (unsigned long)exi->immediate_transfers,
                        (unsigned long)exi->dma_transfers,
                        (unsigned long)exi->last_command,
                        exi->self_test_passed ? 1u : 0u,
                        (unsigned long)g_vi_retraces,
                        (unsigned long)g_external_interrupts,
                        (unsigned long)pi_interrupt_cause(),
                        (unsigned long)g_pi_mask,
                        (unsigned long)mmio_shadow_read(SMG3DS_VI_DI0_LOW, 2u),
                        (unsigned long)mmio_shadow_read(SMG3DS_VI_DI1_LOW, 2u));
                    if (length > 0) {
                        if (length >= (int)sizeof(message))
                            length = (int)sizeof(message) - 1;
                        svcOutputDebugString(message, length);
                        if (debug_file != NULL) {
                            fwrite(message, 1, (size_t)length, debug_file);
                            fflush(debug_file);
                        }
                    }
                }
            }
            if (cpu.timebase < frame_timebase_target)
                advance_timebase(&cpu, frame_timebase_target - cpu.timebase);
        }
#endif
        smg3ds_gx_present_top();
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

#ifdef SMG3DS_WITH_GENERATED
    smg3ds_disc_shutdown();
    if (debug_file != NULL)
        fclose(debug_file);
    g_debug_file = NULL;
    if (R_SUCCEEDED(sdmc_mount))
        archiveUnmount("sdmc");
    if (romfs_ok)
        romfsExit();
#endif
    if (mem1_ok)
        cpu_free(&cpu);
    mem2_free_sparse();
    gfxExit();
    return 0;
}
