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

/*
 * The generated ARM code is large enough that libctru's automatic split would
 * leave only its 24 MiB minimum normal heap. Reserve enough normal heap for
 * the standard 24 MiB guest MEM1 plus host allocation overhead, and let libctru
 * assign every remaining New 3DS application-memory byte to sparse MEM2.
 */
u32 __ctru_heap_size = 32u * 1024u * 1024u;
u32 __ctru_linear_heap_size = 0u;

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
    SMG3DS_MEM2_SWAP_SLOT_NONE = 0xffff,
    SMG3DS_MAX_DISPATCHES_PER_FRAME = 4096,
    SMG3DS_DISPATCH_BATCH_BLOCKS = 4,
    SMG3DS_HOST_TIME_CHECK_INTERVAL = 8,
    SMG3DS_CPU_SLICE_MILLISECONDS = 12,
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
static bool g_emulated_ir_connected = true;
static circlePosition g_host_circle;
static touchPosition g_host_touch;
static bool g_host_touch_active;
static bool g_host_pointer_active;
static bool g_host_touch_click_ready;
static u32 g_kpad_previous_hold;
static bool g_kpad_shake_phase;
static uint32_t g_fault_raw;
static uint32_t g_fault_cia;
static uint8_t g_mmio_shadow[0x10000];
static uint8_t* g_mem2_pages[SMG3DS_MEM2_PAGE_COUNT];
static bool g_mem2_pages_normal[SMG3DS_MEM2_PAGE_COUNT];
static bool g_mem2_pages_dirty[SMG3DS_MEM2_PAGE_COUNT];
static uint16_t g_mem2_swap_slots[SMG3DS_MEM2_PAGE_COUNT];
static uint32_t g_mem2_page_last_use[SMG3DS_MEM2_PAGE_COUNT];
static FILE* g_mem2_swap_file;
static uint32_t g_mem2_page_clock;
static uint32_t g_mem2_pages_used;
static uint32_t g_mem2_pages_reclaimed;
static uint32_t g_mem2_pages_evicted;
static uint32_t g_mem2_pages_loaded;
static uint32_t g_mem2_swap_slots_used;
static uint32_t g_mem2_swap_failures;
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
static bool g_vi_background_handoff_attempted;
static uint32_t g_external_interrupts;
static uint32_t g_decrementer_interrupts;
static bool g_decrementer_armed;
static bool g_decrementer_pending;
static uint32_t g_timebase_cycle_remainder;
static uint32_t g_ai_control;
static uint32_t g_ai_sample_count;
static u64 g_ai_sample_timebase;
static FILE* const g_debug_file = NULL;
static char g_fault_message[4096];
static char g_last_stage_archive[96] = "-";
static uint32_t g_stage_archive_requests;
static uint32_t g_file_select_receives;
static u32 g_file_select_request_info;
static u32 g_file_select_entry;
static u32 g_file_select_context_before;
static u32 g_file_select_state_before;
static u32 g_file_select_queue_before;
static u32 g_file_select_set_context;
static uint32_t g_file_select_set_calls;
static u32 g_file_select_context_after;
static u32 g_file_select_state_after;
static u32 g_file_select_queue_after;
static uint32_t g_file_select_wait_repairs;
static uint32_t g_file_holder_null_wait_retries;
static u32 g_file_holder_null_wait_lr;
static bool g_file_holder_retry_pending;
static u32 g_file_holder_retry_thread;
static u32 g_file_holder_retry_return;
static u32 g_file_holder_retry_loader;
static u32 g_file_holder_retry_path;
static uint32_t g_archive_receive_preflight_retries;
static u32 g_archive_receive_priority_state;
static u32 g_archive_receive_thread;
static u32 g_archive_receive_original_priority;
static u32 g_archive_receive_loader;
static u32 g_archive_receive_path;
static u32 g_archive_receive_return;
static bool g_archive_repair_send_pending;
static u32 g_archive_repair_info;
static u32 g_archive_repair_file_entry;
static uint32_t g_archive_repair_submissions;
static bool g_archive_rebuild_pending;
static u32 g_archive_rebuild_thread;
static uint32_t g_archive_rebuild_attempts;
static u32 g_archive_rebuild_orphan_index;
static u32 g_archive_rebuild_file_count;
static u32 g_archive_rebuild_heap;
static u32 g_archive_holder_rebuild_state;
static u32 g_archive_holder_rebuild_thread;
static uint32_t g_archive_holder_rebuild_attempts;
static u32 g_archive_holder_before;
static u32 g_archive_holder_after;
static u32 g_archive_holder_vector;
static u32 g_archive_holder_capacity;
static u32 g_archive_holder_count;
static uint32_t g_archive_holder_lookup_fast_paths;
static uint32_t g_archive_holder_lookup_hits;
static uint32_t g_archive_holder_invalid_entries;
static uint32_t g_file_loader_identity_repairs;
static char g_archive_recovery_path[96];
static uint32_t g_audio_resource_skips;
static u32 g_audio_resource_bad_pointer;
static u32 g_audio_resource_bad_count;
static uint32_t g_collision_zone_duplicate_skips;
static uint32_t g_collision_zone_capacity_skips;
static uint32_t g_collision_zone_count_repairs;
static uint32_t g_collision_zone_max_parts;
static bool g_zone_count_query_pending;
static u32 g_zone_count_query_thread;
static u32 g_zone_count_query_return;
static uint32_t g_zone_count_minimum_repairs;
static uint32_t g_zone_count_maximum_repairs;
static bool g_archive_wait_redirect_pending;
static u32 g_archive_wait_redirect_thread;
static u32 g_archive_wait_redirect_loader;
static u32 g_archive_wait_redirect_path;
static uint32_t g_archive_wait_redirects;
static uint32_t g_file_wait_calls;
static uint32_t g_file_wait_ready_bypasses;
static uint32_t g_file_wait_mounted_bypasses;
static uint32_t g_file_wait_invalid_repairs;
static u32 g_file_wait_entry;
static u32 g_file_wait_state;
static u32 g_file_wait_context;
static u32 g_file_wait_lr;
static char g_file_wait_path[96];
static bool g_file_wait_reentry_pending;
static u32 g_file_wait_reentry_thread;
static u32 g_file_wait_reentry_loader;
static u32 g_file_wait_reentry_path;
static u32 g_file_wait_reconciled_entry;
static uint32_t g_async_wait_missing_bypasses;
static u32 g_async_wait_missing_executor;
static u32 g_async_wait_missing_name;
static uint32_t g_receive_archive_false_hits;
static u32 g_receive_archive_false_result;
static uint32_t g_receive_archive_identity_repairs;
static u32 g_receive_archive_stale_result;
static u32 g_receive_archive_canonical_result;
static bool g_scene_wipe_create_pending;
static u32 g_scene_wipe_create_thread;
static u32 g_scene_wipe_saved_name;
static u32 g_scene_wipe_saved_return;
static uint32_t g_scene_wipe_create_attempts;
static uint32_t g_scene_wipe_create_failures;
static bool g_system_wipe_create_pending;
static u32 g_system_wipe_create_thread;
static u32 g_system_wipe_saved_return;
static uint32_t g_system_wipe_create_attempts;
static uint32_t g_system_wipe_create_failures;
static bool g_ptr_array_count_reported;
static bool g_resource_table_null_reported;

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
    SMG3DS_FILE_RIPPER_YAZ0_LOOP = 0x80398A20u,
    SMG3DS_FILE_RIPPER_YAZ0_EPILOGUE = 0x80398AECu,
    SMG3DS_FILE_HOLDER_WAIT_READ_DONE = 0x80397A38u,
    SMG3DS_FILE_HOLDER_SET_CONTEXT = 0x80397A84u,
    SMG3DS_FILE_LOADER_REQUEST_MOUNT_ARCHIVE = 0x80397F7Cu,
    SMG3DS_FILE_LOADER_RECEIVE_FILE = 0x80398070u,
    SMG3DS_FILE_LOADER_RECEIVE_INFO_RETURN = 0x80398090u,
    SMG3DS_FILE_LOADER_RECEIVE_WAIT_RETURN = 0x803980A0u,
    SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE = 0x803980C4u,
    SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_WAIT_RETURN = 0x803980F4u,
    SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_LOOKUP_RETURN = 0x80398100u,
    SMG3DS_FILE_LOADER_RECEIVE_ALL = 0x80398118u,
    SMG3DS_FILE_LOADER_RECEIVE_ALL_WAIT_RETURN = 0x8039814Cu,
    SMG3DS_FILE_LOADER_GET_MOUNTED_ARCHIVE_AND_HEAP = 0x803981A0u,
    SMG3DS_FILE_LOADER_GET_REQUEST_INFO = 0x80398260u,
    SMG3DS_ARCHIVE_HOLDER_CTOR = 0x80394778u,
    SMG3DS_ARCHIVE_HOLDER_FIND_ENTRY = 0x803949F0u,
    SMG3DS_LAYOUT_MANAGER_AFTER_NAME_FORMAT = 0x80367F9Cu,
    SMG3DS_RECEIVE_ARCHIVE_WRAPPER_RETURN = 0x803CE014u,
    SMG3DS_MOUNTED_ARCHIVE_WRAPPER_RETURN = 0x803CE0C4u,
    SMG3DS_FUNCTION_ASYNC_WAIT_NOT_FOUND = 0x803991ACu,
    SMG3DS_FUNCTION_ASYNC_WAIT_EPILOGUE = 0x80399268u,
    SMG3DS_AUDIO_GROUP_RESOURCE_SETTER = 0x800310A4u,
    SMG3DS_AUDIO_ME_RESOURCE_SETTER = 0x8031B484u,
    SMG3DS_ARCHIVE_REBUILD_RETURN = 0x803936BCu,
    SMG3DS_AUDIO_SEQUENCE_RESOURCE_SETTER = 0x803936BCu,
    SMG3DS_AUDIO_SEQUENCE_RESOURCE_LOOKUP_RETURN = 0x8039378Cu,
    SMG3DS_STRCASECMP = 0x803FD4C8u,
    SMG3DS_CREATE_SCENE_OBJ = 0x80344A74u,
    SMG3DS_SCENE_WIPE_FORCE_OPEN = 0x8037EE88u,
    SMG3DS_SCENE_WIPE_OBJ_ID = 0x22u,
    SMG3DS_CREATE_SYSTEM_WIPE_HOLDER = 0x8038C89Cu,
    SMG3DS_SYSTEM_WIPE_REQUIRED = 0x803F718Cu,
    SMG3DS_STAGE_ARCHIVE_LOOKUP_RETURN = 0x8034670Cu,
    SMG3DS_STAGE_ARCHIVE_LOOKUP = 0x803F5F34u,
    SMG3DS_OPERATOR_NEW = 0x80409AF8u,
    SMG3DS_PTR_ARRAY_FIND_LOOP = 0x80404AE8u,
    SMG3DS_PTR_ARRAY_FIND_NOT_FOUND = 0x80404B10u,
    SMG3DS_RESOURCE_TABLE_FIND_LOOP = 0x803A74C8u,
    SMG3DS_COLLISION_ZONE_ADD_PARTS = 0x80174800u,
    SMG3DS_GET_STAGE_ZONE_COUNT = 0x803F6204u,
    SMG3DS_RESOURCE_TABLE_FIND_NOT_FOUND = 0x803A74F0u,
    SMG3DS_OS_YIELD_THREAD = 0x804AB44Cu,
    SMG3DS_OS_SET_THREAD_PRIORITY = 0x804AC19Cu,
    SMG3DS_OS_SEND_MESSAGE = 0x804A88E4u,
    SMG3DS_OS_UNLOCK_MUTEX = 0x804A9548u,
    SMG3DS_FUNCTION_ASYNC_MUTEX = 0x805F6998u,
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
    SMG3DS_KPAD_BUTTON_C = 0x4000u,
    SMG3DS_TOUCH_WIDTH = 320,
    SMG3DS_TOUCH_VIEW_Y = 24,
    SMG3DS_TOUCH_VIEW_HEIGHT = 192,
    SMG3DS_COLLISION_ZONE_CAPACITY = 0x200,
    SMG3DS_COLLISION_ZONE_COUNT_OFFSET = 0x804,
    SMG3DS_STAGE_ZONE_COUNT_MIN = 1,
    SMG3DS_STAGE_ZONE_COUNT_MAX = 0x20
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
    /* Touch click is armed one frame after IR capture; see the HID update. */
    if ((keys & KEY_A) != 0u || g_host_touch_click_ready)
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
    const bool pointer_was_active = g_host_pointer_active;

    hidScanInput();
    g_host_keys_held = hidKeysHeld();
    g_host_keys_latched_down |= hidKeysDown() & ~KEY_TOUCH;
    hidCircleRead(&g_host_circle);
    g_host_touch_active = (g_host_keys_held & KEY_TOUCH) != 0u;
    if (g_host_touch_active)
        hidTouchRead(&g_host_touch);
    g_host_pointer_active = g_host_touch_active &&
        g_host_touch.px < SMG3DS_TOUCH_WIDTH &&
        g_host_touch.py >= SMG3DS_TOUCH_VIEW_Y &&
        g_host_touch.py < SMG3DS_TOUCH_VIEW_Y + SMG3DS_TOUCH_VIEW_HEIGHT;
    /*
     * Galaxy first captures a valid IR point, then accepts A on a pointed
     * pane.  Delaying the touchscreen's synthetic A by one host frame gives
     * the pointer controller time to enter its pointing state.
     */
    g_host_touch_click_ready =
        g_host_pointer_active && pointer_was_active;
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

static bool guest_object_range_valid(const CPUState* cpu,
                                     u32 address, u32 size)
{
    return (address & 0xc0000000u) == 0x80000000u &&
           guest_range_valid(cpu, address, size);
}

static void publish_guest_wpad_connection(CPUState* cpu)
{
    const u32 game_system_address =
        cpu->gpr[13] + (u32)(s32)(-14968);
    const u32 game_system =
        guest_range_valid(cpu, game_system_address, 4u) ?
            mem_read32(cpu, game_system_address) : 0u;
    const u32 object_holder = guest_range_valid(cpu, game_system, 36u) ?
        mem_read32(cpu, game_system + 32u) : 0u;
    const u32 wpad_holder = guest_range_valid(cpu, object_holder, 40u) ?
        mem_read32(cpu, object_holder + 36u) : 0u;
    const u32 wpad = guest_range_valid(cpu, wpad_holder, 4u) ?
        mem_read32(cpu, wpad_holder) : 0u;
    const u32 info_checker = guest_range_valid(cpu, wpad, 52u) ?
        mem_read32(cpu, wpad + 48u) : 0u;

    if (!guest_range_valid(cpu, wpad, 55u))
        return;

    /* Keep Galaxy's connection cache coherent with the KPAD sample below. */
    mem_write8(cpu, wpad + 53u, 1u);
    mem_write8(cpu, wpad + 54u, 1u);
    if (guest_range_valid(cpu, info_checker, 36u))
        mem_write32(cpu, info_checker + 32u, 4u);
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

    if (g_host_pointer_active) {
        pointer_x = clamp_unit(
            ((float)g_host_touch.px /
             (float)(SMG3DS_TOUCH_WIDTH - 1)) * 2.0f - 1.0f);
        pointer_y = clamp_unit(
            (((float)g_host_touch.py - (float)SMG3DS_TOUCH_VIEW_Y) /
             (float)(SMG3DS_TOUCH_VIEW_HEIGHT - 1)) * 2.0f - 1.0f);
    }
    mem_write32(cpu, status + 32u, f32_bits(pointer_x));
    mem_write32(cpu, status + 36u, f32_bits(pointer_y));
    mem_write32(cpu, status + 52u, f32_bits(0.0f));
    mem_write32(cpu, status + 56u, f32_bits(1.0f));
    mem_write32(cpu, status + 72u, f32_bits(1.0f));

    /* KPAD device 1 is a Wii Remote with Nunchuk ("freestyle"). */
    mem_write8(cpu, status + 92u, 1u);
    mem_write8(cpu, status + 93u, 0u);
    /* KPAD only publishes IR history samples whose DPD validity is >= 2. */
    mem_write8(cpu, status + 94u,
               (g_host_pointer_active || g_emulated_ir_connected) ? 2u : 0u);
    mem_write8(cpu, status + 95u, 1u);
    stick_x = clamp_unit((float)g_host_circle.dx / 156.0f);
    stick_y = clamp_unit((float)g_host_circle.dy / 156.0f);
    mem_write32(cpu, status + 96u, f32_bits(stick_x));
    mem_write32(cpu, status + 100u, f32_bits(stick_y));

    publish_guest_wpad_connection(cpu);
    g_host_keys_latched_down = 0u;
    ++g_kpad_samples;
    cpu->gpr[3] = 1u;
    cpu->pc = cpu->lr & ~3u;
    return true;
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

static u8 ascii_fold_case(u8 value)
{
    if (value >= (u8)'A' && value <= (u8)'Z')
        return (u8)(value + ((u8)'a' - (u8)'A'));
    return value;
}

static bool guest_strings_equal_case_insensitive(CPUState* cpu,
                                                  u32 first, u32 second)
{
    for (u32 offset = 0u; offset < 512u; ++offset) {
        u8 first_value;
        u8 second_value;
        if (!guest_range_valid(cpu, first + offset, 1u) ||
            !guest_range_valid(cpu, second + offset, 1u)) {
            return false;
        }
        first_value = mem_read8(cpu, first + offset);
        second_value = mem_read8(cpu, second + offset);
        if (ascii_fold_case(first_value) != ascii_fold_case(second_value))
            return false;
        if (first_value == 0u)
            return true;
    }
    return false;
}

static s32 guest_string_case_compare(CPUState* cpu, u32 first, u32 second)
{
    for (u32 offset = 0u; offset < 1024u; ++offset) {
        const u32 first_address = first + offset;
        const u32 second_address = second + offset;
        u8 first_value;
        u8 second_value;

        if (!guest_range_valid(cpu, first_address, 1u) ||
            !guest_range_valid(cpu, second_address, 1u)) {
            if (first_address == second_address)
                return 0;
            return first_address < second_address ? -1 : 1;
        }
        first_value = ascii_fold_case(mem_read8(cpu, first_address));
        second_value = ascii_fold_case(mem_read8(cpu, second_address));
        if (first_value != second_value)
            return (s32)first_value - (s32)second_value;
        if (first_value == 0u)
            return 0;
    }

    if (first == second)
        return 0;
    return first < second ? -1 : 1;
}

static bool is_wiiremote_archive_path(const char* path)
{
    static const char target[] =
        "UsEnglish/LayoutData/WiiRemoteStrapReplace.arc";

    if (path == NULL)
        return false;
    while (*path == '/')
        ++path;
    return strcmp(path, target) == 0;
}

static u32 archive_holder_find_entry_safe(CPUState* cpu, u32 holder,
                                           u32 wanted_path)
{
    if (holder == 0u || wanted_path == 0u)
        return 0u;

    const u32 entries = guest_range_valid(cpu, holder, 12u) ?
        mem_read32(cpu, holder) : 0u;
    const u32 capacity = guest_range_valid(cpu, holder, 12u) ?
        mem_read32(cpu, holder + 4u) : 0u;
    const u32 count = guest_range_valid(cpu, holder, 12u) ?
        mem_read32(cpu, holder + 8u) : 0u;

    ++g_archive_holder_lookup_fast_paths;
    if (capacity > 384u || count > capacity ||
        (count != 0u && entries == 0u) ||
        !guest_range_valid(cpu, entries, count * 4u) ||
        !guest_range_valid(cpu, wanted_path, 1u)) {
        return 0u;
    }

    for (u32 i = 0u; i < count; ++i) {
        const u32 candidate = mem_read32(cpu, entries + i * 4u);
        const u32 archive = guest_range_valid(cpu, candidate, 12u) ?
            mem_read32(cpu, candidate) : 0u;
        const u32 heap = guest_range_valid(cpu, candidate, 12u) ?
            mem_read32(cpu, candidate + 4u) : 0u;
        const u32 candidate_path = guest_range_valid(cpu, candidate, 12u) ?
            mem_read32(cpu, candidate + 8u) : 0u;
        if (candidate == 0u || archive == 0u || heap == 0u ||
            candidate_path == 0u ||
            !guest_range_valid(cpu, candidate, 12u) ||
            !guest_range_valid(cpu, archive, 12u) ||
            !guest_range_valid(cpu, heap, 4u) ||
            !guest_range_valid(cpu, candidate_path, 1u)) {
            ++g_archive_holder_invalid_entries;
            continue;
        }
        u32 canonical_candidate_path = candidate_path;
        u32 canonical_wanted_path = wanted_path;
        while (guest_range_valid(cpu, canonical_candidate_path, 1u) &&
               mem_read8(cpu, canonical_candidate_path) == '/')
            ++canonical_candidate_path;
        while (guest_range_valid(cpu, canonical_wanted_path, 1u) &&
               mem_read8(cpu, canonical_wanted_path) == '/')
            ++canonical_wanted_path;
        if (guest_strings_equal_case_insensitive(
                cpu, canonical_candidate_path, canonical_wanted_path)) {
            ++g_archive_holder_lookup_hits;
            return candidate;
        }
    }
    return 0u;
}

static u32 guest_file_loader(CPUState* cpu)
{
    const u32 singleton = cpu->gpr[13] + (u32)(s32)(-10264);

    return guest_range_valid(cpu, singleton, 4u) ?
        mem_read32(cpu, singleton) : 0u;
}

static bool file_loader_core_valid(CPUState* cpu, u32 loader)
{
    u32 infos;
    u32 request_count;
    u32 holder;
    u32 entries;
    u32 capacity;
    u32 count;

    if (!guest_range_valid(cpu, loader, 0x2cu))
        return false;

    infos = mem_read32(cpu, loader + 0x1cu);
    request_count = mem_read32(cpu, loader + 0x20u);
    holder = mem_read32(cpu, loader + 0x24u);
    if (request_count > 320u ||
        (request_count != 0u &&
         !guest_range_valid(cpu, infos, request_count * 0x90u)) ||
        !guest_range_valid(cpu, holder, 12u)) {
        return false;
    }

    entries = mem_read32(cpu, holder);
    capacity = mem_read32(cpu, holder + 4u);
    count = mem_read32(cpu, holder + 8u);
    return capacity == 384u && count <= capacity &&
           guest_range_valid(cpu, entries, capacity * 4u);
}

static u32 reconcile_file_loader_identity(CPUState* cpu, u32 candidate)
{
    const u32 canonical = guest_file_loader(cpu);

    if (file_loader_core_valid(cpu, candidate) ||
        !file_loader_core_valid(cpu, canonical)) {
        return candidate;
    }

    ++g_file_loader_identity_repairs;
    return canonical;
}

static bool file_loader_archive_mounted(CPUState* cpu, u32 loader, u32 path)
{
    const u32 holder = guest_range_valid(cpu, loader, 0x2cu) ?
        mem_read32(cpu, loader + 0x28u) : 0u;

    return archive_holder_find_entry_safe(cpu, holder, path) != 0u;
}

static u32 guest_stage_archive_heap(CPUState* cpu)
{
    /*
     * Mirror MR::getAproposHeapForSceneArchive(0.03f).  The previous walk
     * through GameSystemObjHolder + 0x20 reached AudSystemWrapper, so every
     * recovered archive was incorrectly consuming the small audio heap.
     */
    const u32 watcher_addr = cpu->gpr[13] + (u32)(s32)(-10308);
    const u32 watcher = guest_range_valid(cpu, watcher_addr, 4u) ?
        mem_read32(cpu, watcher_addr) : 0u;
    const u32 file_cache_heap = guest_range_valid(cpu, watcher, 28u) ?
        mem_read32(cpu, watcher + 16u) : 0u;
    const u32 scene_gddr_heap = guest_range_valid(cpu, watcher, 28u) ?
        mem_read32(cpu, watcher + 24u) : 0u;

    if (guest_range_valid(cpu, file_cache_heap, 112u)) {
        const u32 heap_size = mem_read32(cpu, file_cache_heap + 56u);
        const u32 free_size = mem_read32(cpu, file_cache_heap + 108u);

        if (heap_size != 0u &&
            (u64)free_size * 100u >= (u64)heap_size * 3u) {
            return file_cache_heap;
        }
    }

    if (guest_range_valid(cpu, scene_gddr_heap, 4u)) {
        return scene_gddr_heap;
    }

    return guest_range_valid(cpu, file_cache_heap, 4u) ?
        file_cache_heap : 0u;
}

static u32 guest_scene_obj_holder(CPUState* cpu)
{
    const u32 game_system_address =
        cpu->gpr[13] + (u32)(s32)(-14968);
    const u32 game_system =
        guest_range_valid(cpu, game_system_address, 4u) ?
            mem_read32(cpu, game_system_address) : 0u;
    const u32 scene_controller =
        guest_range_valid(cpu, game_system, 40u) ?
            mem_read32(cpu, game_system + 36u) : 0u;
    const u32 scene_holder_owner =
        guest_range_valid(cpu, scene_controller, 176u) ?
            mem_read32(cpu, scene_controller + 172u) : 0u;
    const u32 scene_holder =
        guest_range_valid(cpu, scene_holder_owner, 20u) ?
            mem_read32(cpu, scene_holder_owner + 16u) : 0u;

    return guest_range_valid(
        cpu, scene_holder, (SMG3DS_SCENE_WIPE_OBJ_ID + 1u) * 4u) ?
            scene_holder : 0u;
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
static bool service_coherent_cache_range(CPUState* cpu);
static bool service_yaz0_decompression(CPUState* cpu);
static bool service_optional_platform_module(CPUState* cpu);
static bool vi_interrupt_pending(void);
static void signal_video_retrace(void);

static bool service_collision_zone_registration(CPUState* cpu)
{
    const u32 zone = cpu->gpr[3];
    const u32 parts = cpu->gpr[4];
    u32 count;

    if (!guest_object_range_valid(cpu, zone,
                                  SMG3DS_COLLISION_ZONE_COUNT_OFFSET + 4u))
        return false;

    count = mem_read32(cpu, zone + SMG3DS_COLLISION_ZONE_COUNT_OFFSET);
    if (count > SMG3DS_COLLISION_ZONE_CAPACITY) {
        ++g_collision_zone_count_repairs;
        mem_write32(cpu, zone + SMG3DS_COLLISION_ZONE_COUNT_OFFSET,
                    SMG3DS_COLLISION_ZONE_CAPACITY);
        if (g_debug_file != NULL) {
            fprintf(g_debug_file,
                    "COLLISION_ZONE_COUNT_REPAIR zone=%08lX "
                    "parts=%08lX count=%lu capacity=%u lr=%08lX\n",
                    (unsigned long)zone, (unsigned long)parts,
                    (unsigned long)count,
                    SMG3DS_COLLISION_ZONE_CAPACITY,
                    (unsigned long)cpu->lr);
            fflush(g_debug_file);
        }
        cpu->pc = cpu->lr & ~3u;
        return true;
    }

    for (u32 index = 0u; index < count; ++index) {
        if (mem_read32(cpu, zone + 4u + index * 4u) == parts) {
            ++g_collision_zone_duplicate_skips;
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "COLLISION_ZONE_DUPLICATE zone=%08lX "
                        "parts=%08lX count=%lu index=%lu lr=%08lX\n",
                        (unsigned long)zone, (unsigned long)parts,
                        (unsigned long)count, (unsigned long)index,
                        (unsigned long)cpu->lr);
                fflush(g_debug_file);
            }
            cpu->pc = cpu->lr & ~3u;
            return true;
        }
    }

    if (count == SMG3DS_COLLISION_ZONE_CAPACITY) {
        ++g_collision_zone_capacity_skips;
        if (g_debug_file != NULL) {
            fprintf(g_debug_file,
                    "COLLISION_ZONE_CAPACITY zone=%08lX "
                    "parts=%08lX count=%lu lr=%08lX\n",
                    (unsigned long)zone, (unsigned long)parts,
                    (unsigned long)count, (unsigned long)cpu->lr);
            fflush(g_debug_file);
        }
        cpu->pc = cpu->lr & ~3u;
        return true;
    }

    if (count + 1u > g_collision_zone_max_parts)
        g_collision_zone_max_parts = count + 1u;
    return false;
}

static void service_collision_zone_calculation(CPUState* cpu)
{
    const u32 return_address = cpu->lr & ~3u;
    const u32 zone = cpu->gpr[29];
    u32 count;

    if (return_address < 0x8017486Cu ||
        return_address >= 0x801749C0u ||
        !guest_object_range_valid(cpu, zone,
                                  SMG3DS_COLLISION_ZONE_COUNT_OFFSET + 4u))
        return;

    count = mem_read32(cpu, zone + SMG3DS_COLLISION_ZONE_COUNT_OFFSET);
    if (count > g_collision_zone_max_parts) {
        g_collision_zone_max_parts = count;
        if (g_debug_file != NULL) {
            fprintf(g_debug_file,
                    "COLLISION_ZONE_CALC zone=%08lX count=%lu "
                    "cursor=%08lX return=%08lX\n",
                    (unsigned long)zone, (unsigned long)count,
                    (unsigned long)cpu->gpr[30],
                    (unsigned long)return_address);
            fflush(g_debug_file);
        }
    }

    if (count > SMG3DS_COLLISION_ZONE_CAPACITY) {
        ++g_collision_zone_count_repairs;
        mem_write32(cpu, zone + SMG3DS_COLLISION_ZONE_COUNT_OFFSET,
                    SMG3DS_COLLISION_ZONE_CAPACITY);
        if (g_debug_file != NULL) {
            fprintf(g_debug_file,
                    "COLLISION_ZONE_CALC_CAP zone=%08lX "
                    "count=%lu capacity=%u cursor=%08lX return=%08lX\n",
                    (unsigned long)zone, (unsigned long)count,
                    SMG3DS_COLLISION_ZONE_CAPACITY,
                    (unsigned long)cpu->gpr[30],
                    (unsigned long)return_address);
            fflush(g_debug_file);
        }
    }
}

int dolrecomp_dispatch_replacement(CPUState* cpu, u32 address)
{
    service_collision_zone_calculation(cpu);

    if (address == SMG3DS_GET_STAGE_ZONE_COUNT) {
        const u32 current_thread = mem_read32(cpu, 0x800000e4u);
        const bool query_return =
            g_zone_count_query_pending &&
            current_thread == g_zone_count_query_thread &&
            (cpu->lr & ~3u) == SMG3DS_GET_STAGE_ZONE_COUNT;

        if (query_return) {
            const u32 reported_count = cpu->gpr[3];

            g_zone_count_query_pending = false;
            cpu->lr = g_zone_count_query_return;
            if ((s32)reported_count < SMG3DS_STAGE_ZONE_COUNT_MIN) {
                cpu->gpr[3] = SMG3DS_STAGE_ZONE_COUNT_MIN;
                ++g_zone_count_minimum_repairs;
            } else if (reported_count > SMG3DS_STAGE_ZONE_COUNT_MAX) {
                cpu->gpr[3] = SMG3DS_STAGE_ZONE_COUNT_MAX;
                ++g_zone_count_maximum_repairs;
            }
            if (cpu->gpr[3] != reported_count && g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "STAGE_ZONE_COUNT_REPAIR reported=%ld effective=%lu "
                        "thread=%08lX return=%08lX\n",
                        (long)(s32)reported_count,
                        (unsigned long)cpu->gpr[3],
                        (unsigned long)current_thread,
                        (unsigned long)cpu->lr);
                fflush(g_debug_file);
            }
            cpu->pc = cpu->lr & ~3u;
            return 1;
        }

        if (!g_zone_count_query_pending) {
            g_zone_count_query_pending = true;
            g_zone_count_query_thread = current_thread;
            g_zone_count_query_return = cpu->lr;
            cpu->lr = SMG3DS_GET_STAGE_ZONE_COUNT;
        }
    }

    if (address == SMG3DS_COLLISION_ZONE_ADD_PARTS &&
        service_collision_zone_registration(cpu))
        return 1;

    if (address == SMG3DS_SYSTEM_WIPE_REQUIRED) {
        const u32 current_thread = mem_read32(cpu, 0x800000e4u);
        const u32 game_system_address =
            cpu->gpr[13] + (u32)(s32)(-14968);
        const u32 game_system =
            guest_range_valid(cpu, game_system_address, 4u) ?
                mem_read32(cpu, game_system_address) : 0u;
        u32 system_wipe = guest_range_valid(cpu, game_system, 52u) ?
            mem_read32(cpu, game_system + 48u) : 0u;
        const bool create_return =
            g_system_wipe_create_pending &&
            current_thread == g_system_wipe_create_thread &&
            (cpu->lr & ~3u) == SMG3DS_SYSTEM_WIPE_REQUIRED;

        if (create_return) {
            const u32 create_result = cpu->gpr[3];

            g_system_wipe_create_pending = false;
            cpu->lr = g_system_wipe_saved_return;
            if (create_result != 0u &&
                guest_range_valid(cpu, game_system, 52u)) {
                mem_write32(cpu, game_system + 48u, create_result);
                system_wipe = create_result;
            }
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "SYSTEM_WIPE_CREATE_RESULT game=%08lX "
                        "wipe=%08lX result=%08lX return=%08lX\n",
                        (unsigned long)game_system,
                        (unsigned long)system_wipe,
                        (unsigned long)create_result,
                        (unsigned long)g_system_wipe_saved_return);
                fflush(g_debug_file);
            }
            if (system_wipe == 0u) {
                ++g_system_wipe_create_failures;
                cpu->gpr[3] = 0u;
                cpu->pc = cpu->lr & ~3u;
                return 1;
            }
        } else if (system_wipe == 0u &&
                   !g_system_wipe_create_pending && game_system != 0u) {
            g_system_wipe_create_pending = true;
            g_system_wipe_create_thread = current_thread;
            g_system_wipe_saved_return = cpu->lr;
            ++g_system_wipe_create_attempts;
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "SYSTEM_WIPE_CREATE_BEGIN game=%08lX "
                        "return=%08lX thread=%08lX\n",
                        (unsigned long)game_system,
                        (unsigned long)cpu->lr,
                        (unsigned long)current_thread);
                fflush(g_debug_file);
            }
            cpu->lr = SMG3DS_SYSTEM_WIPE_REQUIRED;
            cpu->pc = SMG3DS_CREATE_SYSTEM_WIPE_HOLDER;
            return 1;
        } else if (system_wipe == 0u) {
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "SYSTEM_WIPE_NULL_BYPASS game=%08lX pending=%u "
                        "thread=%08lX owner=%08lX lr=%08lX\n",
                        (unsigned long)game_system,
                        g_system_wipe_create_pending ? 1u : 0u,
                        (unsigned long)current_thread,
                        (unsigned long)g_system_wipe_create_thread,
                        (unsigned long)cpu->lr);
                fflush(g_debug_file);
            }
            cpu->gpr[3] = 0u;
            cpu->pc = cpu->lr & ~3u;
            return 1;
        }
    }

    if (address == SMG3DS_SCENE_WIPE_FORCE_OPEN) {
        const u32 current_thread = mem_read32(cpu, 0x800000e4u);
        const u32 scene_holder = guest_scene_obj_holder(cpu);
        const u32 wipe = scene_holder != 0u ?
            mem_read32(cpu, scene_holder +
                SMG3DS_SCENE_WIPE_OBJ_ID * 4u) : 0u;
        const bool create_return =
            g_scene_wipe_create_pending &&
            current_thread == g_scene_wipe_create_thread &&
            (cpu->lr & ~3u) == SMG3DS_SCENE_WIPE_FORCE_OPEN;

        if (create_return) {
            char wipe_name[64];
            const u32 create_result = cpu->gpr[3];

            guest_copy_string(cpu, g_scene_wipe_saved_name, wipe_name,
                              sizeof(wipe_name));
            g_scene_wipe_create_pending = false;
            cpu->gpr[3] = g_scene_wipe_saved_name;
            cpu->lr = g_scene_wipe_saved_return;
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "SCENE_WIPE_CREATE_RESULT holder=%08lX "
                        "wipe=%08lX result=%08lX name=%s "
                        "return=%08lX\n",
                        (unsigned long)scene_holder,
                        (unsigned long)wipe,
                        (unsigned long)create_result,
                        wipe_name[0] != '\0' ? wipe_name : "-",
                        (unsigned long)g_scene_wipe_saved_return);
                fflush(g_debug_file);
            }
            if (wipe == 0u) {
                ++g_scene_wipe_create_failures;
                cpu->pc = cpu->lr & ~3u;
                return 1;
            }
        } else if (wipe == 0u && !g_scene_wipe_create_pending &&
                   scene_holder != 0u) {
            char wipe_name[64];

            guest_copy_string(cpu, cpu->gpr[3], wipe_name,
                              sizeof(wipe_name));
            g_scene_wipe_create_pending = true;
            g_scene_wipe_create_thread = current_thread;
            g_scene_wipe_saved_name = cpu->gpr[3];
            g_scene_wipe_saved_return = cpu->lr;
            ++g_scene_wipe_create_attempts;
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "SCENE_WIPE_CREATE_BEGIN holder=%08lX "
                        "name=%s return=%08lX thread=%08lX\n",
                        (unsigned long)scene_holder,
                        wipe_name[0] != '\0' ? wipe_name : "-",
                        (unsigned long)cpu->lr,
                        (unsigned long)current_thread);
                fflush(g_debug_file);
            }
            cpu->gpr[3] = SMG3DS_SCENE_WIPE_OBJ_ID;
            cpu->lr = SMG3DS_SCENE_WIPE_FORCE_OPEN;
            cpu->pc = SMG3DS_CREATE_SCENE_OBJ;
            return 1;
        } else if (wipe == 0u) {
            /*
             * A nested wipe request can occur while createSceneObj is still
             * constructing the holder. Its slot is intentionally published
             * only after initWithoutIter finishes, so defer that cosmetic
             * request instead of dereferencing a partially built object.
             */
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "SCENE_WIPE_NULL_BYPASS holder=%08lX "
                        "pending=%u thread=%08lX owner=%08lX lr=%08lX\n",
                        (unsigned long)scene_holder,
                        g_scene_wipe_create_pending ? 1u : 0u,
                        (unsigned long)current_thread,
                        (unsigned long)g_scene_wipe_create_thread,
                        (unsigned long)cpu->lr);
                fflush(g_debug_file);
            }
            cpu->pc = cpu->lr & ~3u;
            return 1;
        }
    }

    if (address == SMG3DS_FILE_LOADER_REQUEST_MOUNT_ARCHIVE) {
        char mount_path[96];

        guest_copy_string(cpu, cpu->gpr[4], mount_path,
                          sizeof(mount_path));
        if (is_wiiremote_archive_path(mount_path) &&
            mount_path[0] != '/') {
            u32 length = 0u;
            while (length + 1u < 256u &&
                   guest_range_valid(cpu, cpu->gpr[4] + length, 1u) &&
                   mem_read8(cpu, cpu->gpr[4] + length) != 0u) {
                ++length;
            }
            if (length + 1u < 256u &&
                guest_range_valid(cpu, cpu->gpr[4], length + 2u)) {
                for (u32 offset = length + 1u; offset > 0u; --offset) {
                    mem_write8(cpu, cpu->gpr[4] + offset,
                               mem_read8(cpu, cpu->gpr[4] + offset - 1u));
                }
                mem_write8(cpu, cpu->gpr[4], '/');
                if (g_debug_file != NULL) {
                    fprintf(g_debug_file,
                            "WIIMOTE_ARCHIVE_PATH_CANONICALIZED "
                            "path=/%s loader=%08lX\n",
                            mount_path, (unsigned long)cpu->gpr[3]);
                    fflush(g_debug_file);
                }
            }
        }
        if (mount_path[0] != '\0') {
            const u32 loader = cpu->gpr[3];
            const u32 file_holder =
                loader != 0u && guest_range_valid(cpu, loader, 0x28u) ?
                    mem_read32(cpu, loader + 0x24u) : 0u;
            const u32 file_entries =
                file_holder != 0u &&
                guest_range_valid(cpu, file_holder, 12u) ?
                    mem_read32(cpu, file_holder) : 0u;
            u32 file_count =
                file_holder != 0u &&
                guest_range_valid(cpu, file_holder, 12u) ?
                    mem_read32(cpu, file_holder + 8u) : 0u;
            u32 invalid_relabels = 0u;
            u32 first_entry = 0u;
            u32 first_disc = 0u;

            if (file_count > 384u)
                file_count = 384u;
            if (file_entries != 0u &&
                guest_range_valid(cpu, file_entries,
                                  file_count * 4u)) {
                for (u32 i = 0u; i < file_count; ++i) {
                    const u32 file_entry =
                        mem_read32(cpu, file_entries + i * 4u);
                    if (i == 0u) {
                        first_entry = file_entry;
                        first_disc =
                            file_entry != 0u &&
                            guest_range_valid(cpu, file_entry, 4u) ?
                                mem_read32(cpu, file_entry) : 0u;
                    }
                    if (file_entry != 0u &&
                        guest_range_valid(cpu, file_entry, 4u) &&
                        mem_read32(cpu, file_entry) == 0xffffffffu) {
                        mem_write32(cpu, file_entry,
                                    0x80000000u | i);
                        ++invalid_relabels;
                    }
                }
            }
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "BOOT_ARCHIVE_FILEHOLDER_REPAIR path=%s "
                        "loader=%08lX lr=%08lX "
                        "holder=%08lX vector=%08lX count=%lu "
                        "first=%08lX/%08lX relabeled=%lu\n",
                        mount_path[0] != '\0' ? mount_path : "-",
                        (unsigned long)loader,
                        (unsigned long)cpu->lr,
                        (unsigned long)file_holder,
                        (unsigned long)file_entries,
                        (unsigned long)file_count,
                        (unsigned long)first_entry,
                        (unsigned long)first_disc,
                        (unsigned long)invalid_relabels);
                fflush(g_debug_file);
            }
            if (g_debug_file != NULL) {
                const u32 infos = guest_range_valid(cpu, loader, 0x24u) ?
                    mem_read32(cpu, loader + 0x1cu) : 0u;
                u32 request_count =
                    guest_range_valid(cpu, loader, 0x24u) ?
                        mem_read32(cpu, loader + 0x20u) : 0u;
                s32 target_disc = -1;

                smg3ds_disc_resolve_path(cpu, cpu->gpr[4], &target_disc);
                if (request_count > 320u)
                    request_count = 320u;
                fprintf(g_debug_file,
                        "BOOT_ARCHIVE_STATE path=%s target=%08lX "
                        "files=%lu requests=%lu infos=%08lX\n",
                        mount_path[0] != '\0' ? mount_path : "-",
                        (unsigned long)(u32)target_disc,
                        (unsigned long)file_count,
                        (unsigned long)request_count,
                        (unsigned long)infos);
                for (u32 i = 0u; i < file_count && i < 8u; ++i) {
                    const u32 entry = mem_read32(
                        cpu, file_entries + i * 4u);
                    fprintf(g_debug_file,
                            "BOOT_ARCHIVE_FILE index=%lu entry=%08lX "
                            "disc=%08lX context=%08lX heap=%08lX "
                            "state=%08lX\n",
                            (unsigned long)i,
                            (unsigned long)entry,
                            (unsigned long)(guest_range_valid(
                                cpu, entry, 16u) ?
                                    mem_read32(cpu, entry) : 0u),
                            (unsigned long)(guest_range_valid(
                                cpu, entry, 16u) ?
                                    mem_read32(cpu, entry + 4u) : 0u),
                            (unsigned long)(guest_range_valid(
                                cpu, entry, 16u) ?
                                    mem_read32(cpu, entry + 8u) : 0u),
                            (unsigned long)(guest_range_valid(
                                cpu, entry, 16u) ?
                                    mem_read32(cpu, entry + 12u) : 0u));
                }
                if (infos != 0u && guest_range_valid(
                        cpu, infos, request_count * 0x90u)) {
                    for (u32 i = 0u; i < request_count && i < 8u; ++i) {
                        const u32 info = infos + i * 0x90u;
                        char request_path[96];

                        guest_copy_string(cpu, info + 8u, request_path,
                                          sizeof(request_path));
                        fprintf(g_debug_file,
                                "BOOT_ARCHIVE_REQUEST index=%lu "
                                "path=%s kind=%08lX/%08lX "
                                "phase=%08lX file=%08lX\n",
                                (unsigned long)i,
                                request_path[0] != '\0' ?
                                    request_path : "-",
                                (unsigned long)mem_read32(cpu, info),
                                (unsigned long)mem_read32(cpu, info + 4u),
                                (unsigned long)mem_read32(cpu, info + 0x88u),
                                (unsigned long)mem_read32(cpu, info + 0x8cu));
                    }
                }
                fflush(g_debug_file);
            }
            if (loader != 0u &&
                guest_range_valid(cpu, loader, 0x24u)) {
                const u32 infos = mem_read32(cpu, loader + 0x1cu);
                const u32 request_count = mem_read32(cpu, loader + 0x20u);
                bool target_present = false;
                bool all_completed = true;

                if (request_count <= 320u && infos != 0u &&
                    guest_range_valid(cpu, infos,
                                      request_count * 0x90u)) {
                    for (u32 i = 0u; i < request_count; ++i) {
                        const u32 request_info = infos + i * 0x90u;

                        if (guest_strings_equal_case_insensitive(
                                cpu, request_info + 8u, cpu->gpr[4])) {
                            target_present = true;
                        }
                        if (mem_read32(cpu, request_info + 0x88u) != 2u)
                            all_completed = false;
                    }
                    if (!target_present && all_completed &&
                        request_count != 0u) {
                        mem_write32(cpu, loader + 0x20u, 0u);
                        if (g_debug_file != NULL) {
                            fprintf(g_debug_file,
                                    "BOOT_ARCHIVE_COMPLETED_REQUESTS_CLEARED "
                                    "path=%s old_count=%lu "
                                    "infos=%08lX\n",
                                    mount_path[0] != '\0' ?
                                        mount_path : "-",
                                    (unsigned long)request_count,
                                    (unsigned long)infos);
                            fflush(g_debug_file);
                        }
                    }
                }
            }
        }
    }

    if (address == SMG3DS_RECEIVE_ARCHIVE_WRAPPER_RETURN) {
        const u32 loader = mem_read32(
            cpu, cpu->gpr[13] + (u32)(s32)(-10264));
        const u32 holder = guest_range_valid(cpu, loader, 0x2cu) ?
            mem_read32(cpu, loader + 0x28u) : 0u;
        const u32 path = cpu->gpr[1] + 8u;
        const u32 entry = archive_holder_find_entry_safe(
            cpu, holder, path);
        const u32 published_archive =
            entry != 0u && guest_range_valid(cpu, entry, 12u) ?
                mem_read32(cpu, entry) : 0u;
        char receive_path[96];

        guest_copy_string(cpu, path, receive_path, sizeof(receive_path));
        if (g_debug_file != NULL) {
            fprintf(g_debug_file,
                    "RECEIVE_ARCHIVE_RESULT path=%s result=%08lX "
                    "published=%08lX entry=%08lX caller=%08lX\n",
                    receive_path[0] != '\0' ? receive_path : "-",
                    (unsigned long)cpu->gpr[3],
                    (unsigned long)published_archive,
                    (unsigned long)entry,
                    (unsigned long)(guest_range_valid(
                        cpu, cpu->gpr[1] + 276u, 4u) ?
                            mem_read32(cpu, cpu->gpr[1] + 276u) : 0u));
            fflush(g_debug_file);
        }
        if (is_wiiremote_archive_path(receive_path) &&
            g_debug_file != NULL) {
            const u32 infos =
                loader != 0u && guest_range_valid(cpu, loader, 0x2cu) ?
                    mem_read32(cpu, loader + 0x1cu) : 0u;
            const u32 request_count =
                loader != 0u && guest_range_valid(cpu, loader, 0x2cu) ?
                    mem_read32(cpu, loader + 0x20u) : 0u;
            u32 request_info = 0u;
            u32 file_entry = 0u;
            for (u32 i = 0u; i < request_count && i < 1024u; ++i) {
                const u32 candidate = infos + i * 0x90u;
                if (candidate == 0u ||
                    !guest_range_valid(cpu, candidate, 0x90u))
                    break;
                if (guest_strings_equal_case_insensitive(
                        cpu, candidate + 8u, path)) {
                    request_info = candidate;
                    file_entry = mem_read32(cpu, candidate + 0x8cu);
                    break;
                }
            }
            fprintf(g_debug_file,
                    "WIIMOTE_ARCHIVE_STATE loader=%08lX requests=%lu "
                    "info=%08lX phase=%08lX file=%08lX "
                    "disc=%08lX context=%08lX heap=%08lX state=%08lX "
                    "holder=%08lX vector=%08lX capacity=%lu count=%lu\n",
                    (unsigned long)loader,
                    (unsigned long)request_count,
                    (unsigned long)request_info,
                    (unsigned long)(request_info != 0u ?
                        mem_read32(cpu, request_info + 0x88u) : 0u),
                    (unsigned long)file_entry,
                    (unsigned long)(file_entry != 0u &&
                        guest_range_valid(cpu, file_entry, 0x38u) ?
                            mem_read32(cpu, file_entry) : 0u),
                    (unsigned long)(file_entry != 0u &&
                        guest_range_valid(cpu, file_entry, 0x38u) ?
                            mem_read32(cpu, file_entry + 4u) : 0u),
                    (unsigned long)(file_entry != 0u &&
                        guest_range_valid(cpu, file_entry, 0x38u) ?
                            mem_read32(cpu, file_entry + 8u) : 0u),
                    (unsigned long)(file_entry != 0u &&
                        guest_range_valid(cpu, file_entry, 0x38u) ?
                            mem_read32(cpu, file_entry + 0x0cu) : 0u),
                    (unsigned long)holder,
                    (unsigned long)(holder != 0u &&
                        guest_range_valid(cpu, holder, 12u) ?
                            mem_read32(cpu, holder) : 0u),
                    (unsigned long)(holder != 0u &&
                        guest_range_valid(cpu, holder, 12u) ?
                            mem_read32(cpu, holder + 4u) : 0u),
                    (unsigned long)(holder != 0u &&
                        guest_range_valid(cpu, holder, 12u) ?
                            mem_read32(cpu, holder + 8u) : 0u));
            fflush(g_debug_file);
        }
        if (published_archive != 0u &&
            cpu->gpr[3] != published_archive) {
            ++g_receive_archive_identity_repairs;
            g_receive_archive_stale_result = cpu->gpr[3];
            g_receive_archive_canonical_result = published_archive;
            if (g_debug_file != NULL) {
                fprintf(g_debug_file,
                        "RECEIVE_ARCHIVE_IDENTITY_REPAIRED path=%s "
                        "stale=%08lX canonical=%08lX entry=%08lX\n",
                        receive_path[0] != '\0' ? receive_path : "-",
                        (unsigned long)g_receive_archive_stale_result,
                        (unsigned long)g_receive_archive_canonical_result,
                        (unsigned long)entry);
                fflush(g_debug_file);
            }
            cpu->gpr[3] = published_archive;
        } else if (cpu->gpr[3] != 0u && published_archive == 0u) {
            ++g_receive_archive_false_hits;
            g_receive_archive_false_result = cpu->gpr[3];
            cpu->gpr[3] = 0u;
        }
    }

    if (address == SMG3DS_MOUNTED_ARCHIVE_WRAPPER_RETURN) {
        char mounted_path[96];

        guest_copy_string(cpu, cpu->gpr[1] + 8u, mounted_path,
                          sizeof(mounted_path));
        const u32 loader = mem_read32(
            cpu, cpu->gpr[13] + (u32)(s32)(-10264));
        const u32 holder = guest_range_valid(cpu, loader, 0x2cu) ?
            mem_read32(cpu, loader + 0x28u) : 0u;
        const u32 entry = archive_holder_find_entry_safe(
            cpu, holder, cpu->gpr[1] + 8u);
        const u32 archive =
            entry != 0u ? mem_read32(cpu, entry) : 0u;
        const u32 heap =
            entry != 0u ? mem_read32(cpu, entry + 4u) : 0u;

        if (guest_range_valid(cpu, cpu->gpr[30], 4u))
            mem_write32(cpu, cpu->gpr[30], archive);
        if (guest_range_valid(cpu, cpu->gpr[31], 4u))
            mem_write32(cpu, cpu->gpr[31], heap);
        if (g_debug_file != NULL) {
            fprintf(g_debug_file,
                    "MOUNTED_ARCHIVE_RESULT path=%s archive=%08lX "
                    "heap=%08lX outs=%08lX/%08lX caller=%08lX\n",
                    mounted_path[0] != '\0' ? mounted_path : "-",
                    (unsigned long)(guest_range_valid(
                        cpu, cpu->gpr[30], 4u) ?
                            mem_read32(cpu, cpu->gpr[30]) : 0u),
                    (unsigned long)(guest_range_valid(
                        cpu, cpu->gpr[31], 4u) ?
                            mem_read32(cpu, cpu->gpr[31]) : 0u),
                    (unsigned long)cpu->gpr[30],
                    (unsigned long)cpu->gpr[31],
                    (unsigned long)(guest_range_valid(
                        cpu, cpu->gpr[1] + 292u, 4u) ?
                            mem_read32(cpu, cpu->gpr[1] + 292u) : 0u));
            fflush(g_debug_file);
        }
    }

    if (address == SMG3DS_FUNCTION_ASYNC_WAIT_NOT_FOUND &&
        cpu->gpr[31] == cpu->gpr[29]) {
        const u32 wait_frame = cpu->gpr[1];
        const u32 wrapper_frame = guest_range_valid(cpu, wait_frame, 4u) ?
            mem_read32(cpu, wait_frame) : 0u;
        const u32 owner_frame =
            guest_range_valid(cpu, wrapper_frame, 4u) ?
                mem_read32(cpu, wrapper_frame) : 0u;
        char async_name[96];
        char owner_path[96];

        guest_copy_string(cpu, cpu->gpr[28], async_name,
                          sizeof(async_name));
        owner_path[0] = '\0';
        if (guest_range_valid(cpu, owner_frame + 20u, 1u))
            guest_copy_string(cpu, owner_frame + 20u, owner_path,
                              sizeof(owner_path));
        /*
         * FunctionAsyncExecutor::waitForEnd assumes the requested job is
         * still present and dereferences mHolders.end() when it is not.  A
         * recovered archive request can finish during the main-thread
         * handoff, so treat an absent job as already completed.  The search
         * still owns MutexHolder<2> here: release it through the guest OS,
         * then resume at waitForEnd's register/stack epilogue.
         */
        ++g_async_wait_missing_bypasses;
        g_async_wait_missing_executor = cpu->gpr[30];
        g_async_wait_missing_name = cpu->gpr[28];
        if (g_debug_file != NULL) {
            fprintf(g_debug_file,
                    "ASYNC_WAIT_MISSING name=%s exec=%08lX count=%lu "
                    "frames=%08lX/%08lX/%08lX owner=%08lX/%08lX/%08lX "
                    "path=%s\n",
                    async_name[0] != '\0' ? async_name : "-",
                    (unsigned long)cpu->gpr[30],
                    (unsigned long)(guest_range_valid(
                        cpu, cpu->gpr[30], 1040u) ?
                            mem_read32(cpu, cpu->gpr[30] + 1036u) :
                            0xffffffffu),
                    (unsigned long)wait_frame,
                    (unsigned long)wrapper_frame,
                    (unsigned long)owner_frame,
                    (unsigned long)(guest_range_valid(
                        cpu, owner_frame + 8u, 12u) ?
                            mem_read32(cpu, owner_frame + 8u) : 0u),
                    (unsigned long)(guest_range_valid(
                        cpu, owner_frame + 8u, 12u) ?
                            mem_read32(cpu, owner_frame + 12u) : 0u),
                    (unsigned long)(guest_range_valid(
                        cpu, owner_frame + 8u, 12u) ?
                            mem_read32(cpu, owner_frame + 16u) : 0u),
                    owner_path[0] != '\0' ? owner_path : "-");
            fflush(g_debug_file);
        }
        cpu->gpr[3] = SMG3DS_FUNCTION_ASYNC_MUTEX;
        cpu->lr = SMG3DS_FUNCTION_ASYNC_WAIT_EPILOGUE;
        cpu->pc = SMG3DS_OS_UNLOCK_MUTEX;
        return 1;
    }

    if (address == SMG3DS_LAYOUT_MANAGER_AFTER_NAME_FORMAT &&
        guest_range_valid(cpu, cpu->gpr[29], 4u) &&
        mem_read32(cpu, cpu->gpr[29]) == 0u) {
        char layout_name[96];

        guest_copy_string(cpu, cpu->gpr[30], layout_name,
                          sizeof(layout_name));
        if (g_debug_file != NULL) {
            fprintf(g_debug_file,
                    "LAYOUT_HOLDER_NULL this=%08lX name=%s sp=%08lX "
                    "lr=%08lX async_missing=%lu/%08lX\n",
                    (unsigned long)cpu->gpr[29],
                    layout_name[0] != '\0' ? layout_name : "-",
                    (unsigned long)cpu->gpr[1],
                    (unsigned long)cpu->lr,
                    (unsigned long)g_async_wait_missing_bypasses,
                    (unsigned long)g_async_wait_missing_name);
            fflush(g_debug_file);
        }
    }

    if (address == SMG3DS_STRCASECMP) {
        ++g_case_compare_fast_paths;
        cpu->gpr[3] = guest_string_case_compare(
            cpu, cpu->gpr[3], cpu->gpr[4]) == 0 ? 1u : 0u;
        cpu->pc = cpu->lr & ~3u;
        return 1;
    }

    if (address == SMG3DS_AUDIO_GROUP_RESOURCE_SETTER) {
        const u32 destination = cpu->gpr[3];
        const u32 resource = cpu->gpr[4];
        const u32 count = guest_range_valid(cpu, resource, 4u) ?
            mem_read32(cpu, resource) : 0xffffffffu;
        const bool resource_valid =
            resource != 0u && count <= 0x4000u &&
            guest_range_valid(cpu, resource, 4u + count * 4u);

        if (!resource_valid) {
            ++g_audio_resource_skips;
            g_audio_resource_bad_pointer = resource;
            g_audio_resource_bad_count = count;
            if (guest_range_valid(cpu, destination, 16u)) {
                mem_write32(cpu, destination + 4u, 0u);
                mem_write32(cpu, destination + 8u, 0u);
                mem_write32(cpu, destination + 12u, 0u);
            }
            cpu->pc = cpu->lr & ~3u;
            return 1;
        }
    }

    if (address == SMG3DS_AUDIO_ME_RESOURCE_SETTER) {
        const u32 destination = cpu->gpr[3] + 84u;
        const u32 resource = cpu->gpr[4];
        const u32 count = guest_range_valid(cpu, resource, 12u) ?
            mem_read32(cpu, resource) : 0xffffffffu;
        const u32 values_offset = guest_range_valid(cpu, resource, 12u) ?
            mem_read32(cpu, resource + 4u) : 0u;
        const u32 table_offset = guest_range_valid(cpu, resource, 12u) ?
            mem_read32(cpu, resource + 8u) : 0u;
        const bool resource_valid =
            resource != 0u && count <= 0x4000u &&
            values_offset <= 0x04000000u && table_offset <= 0x04000000u &&
            guest_range_valid(cpu, resource + values_offset, 1u) &&
            guest_range_valid(cpu, resource + table_offset, count * 4u);

        if (!resource_valid) {
            ++g_audio_resource_skips;
            g_audio_resource_bad_pointer = resource;
            g_audio_resource_bad_count = count;
            if (guest_range_valid(cpu, destination, 12u)) {
                mem_write32(cpu, destination, 0u);
                mem_write32(cpu, destination + 4u, 0u);
                mem_write32(cpu, destination + 8u, 0u);
            }
            cpu->pc = cpu->lr & ~3u;
            return 1;
        }
    }

    if (address == SMG3DS_ARCHIVE_REBUILD_RETURN &&
        g_archive_rebuild_pending &&
        mem_read32(cpu, 0x800000e4u) == g_archive_rebuild_thread) {
        cpu->gpr[3] = g_archive_receive_loader;
        cpu->gpr[4] = g_archive_receive_path;
        cpu->lr = g_archive_receive_return;
        g_archive_rebuild_pending = false;
        cpu->pc = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE;
        return 1;
    }

    if (address == SMG3DS_AUDIO_SEQUENCE_RESOURCE_SETTER) {
        const u32 destination = cpu->gpr[3];
        const u32 resource = cpu->gpr[4];
        const bool header_valid = guest_range_valid(cpu, resource, 16u);
        const u32 count = header_valid ?
            mem_read32(cpu, resource) : 0xffffffffu;
        const u32 values_offset = header_valid ?
            mem_read32(cpu, resource + 4u) : 0u;
        const u32 table_offset = header_valid ?
            mem_read32(cpu, resource + 8u) : 0u;
        const u32 relocated = header_valid ?
            mem_read32(cpu, resource + 12u) : 0u;
        const bool resource_valid =
            resource != 0u && count <= 0x4000u &&
            values_offset <= 0x04000000u && table_offset <= 0x04000000u &&
            guest_range_valid(cpu, resource + values_offset, 1u) &&
            guest_range_valid(cpu, resource + table_offset,
                              relocated != 0u ? 1u : count * 4u);

        if (!resource_valid) {
            ++g_audio_resource_skips;
            g_audio_resource_bad_pointer = resource;
            g_audio_resource_bad_count = count;
            if (guest_range_valid(cpu, destination, 16u)) {
                mem_write8(cpu, destination, 0u);
                mem_write32(cpu, destination + 4u, 0u);
                mem_write32(cpu, destination + 8u, 0u);
                mem_write32(cpu, destination + 12u, 0u);
            }
            cpu->pc = cpu->lr & ~3u;
            return 1;
        }
    }

    if (address == SMG3DS_AUDIO_SEQUENCE_RESOURCE_LOOKUP_RETURN &&
        cpu->gpr[3] == 0u) {
        const u32 destination = cpu->gpr[31];
        ++g_audio_resource_skips;
        g_audio_resource_bad_pointer = 0u;
        g_audio_resource_bad_count = 0xffffffffu;
        if (guest_range_valid(cpu, destination, 16u)) {
            mem_write8(cpu, destination, 0u);
            mem_write32(cpu, destination + 4u, 0u);
            mem_write32(cpu, destination + 8u, 0u);
            mem_write32(cpu, destination + 12u, 0u);
        }
        cpu->pc = 0x80393798u;
        return 1;
    }

    if (address == SMG3DS_FILE_LOADER_GET_MOUNTED_ARCHIVE_AND_HEAP) {
        const u32 loader = cpu->gpr[3];
        const u32 wanted_path = cpu->gpr[4];
        const u32 archive_out = cpu->gpr[5];
        const u32 heap_out = cpu->gpr[6];
        const u32 holder = guest_range_valid(cpu, loader, 0x2cu) ?
            mem_read32(cpu, loader + 0x28u) : 0u;
        const u32 entry = archive_holder_find_entry_safe(
            cpu, holder, wanted_path);

        if (entry != 0u) {
            if (guest_range_valid(cpu, archive_out, 4u))
                mem_write32(cpu, archive_out, mem_read32(cpu, entry));
            if (guest_range_valid(cpu, heap_out, 4u))
                mem_write32(cpu, heap_out, mem_read32(cpu, entry + 4u));
        }
        cpu->pc = cpu->lr & ~3u;
        return 1;
    }

    if (address == SMG3DS_FILE_LOADER_RECEIVE_FILE &&
        g_file_wait_reentry_pending &&
        mem_read32(cpu, 0x800000e4u) == g_file_wait_reentry_thread) {
        cpu->gpr[3] = g_file_wait_reentry_loader;
        cpu->gpr[4] = g_file_wait_reentry_path;
        cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_WAIT_RETURN;
        cpu->pc = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE;
        g_file_wait_reentry_pending = false;
        return 1;
    }

    if (address == SMG3DS_FILE_HOLDER_WAIT_READ_DONE &&
        guest_range_valid(cpu, cpu->gpr[3], 0x35u)) {
        const u32 entry = cpu->gpr[3];
        const u32 state = mem_read32(cpu, entry + 0x0cu);

        ++g_file_wait_calls;
        g_file_wait_entry = entry;
        g_file_wait_state = state;
        g_file_wait_context = mem_read32(cpu, entry + 4u);
        g_file_wait_lr = cpu->lr;
        g_file_wait_path[0] = '\0';
        if (cpu->lr == SMG3DS_FILE_LOADER_RECEIVE_ALL_WAIT_RETURN &&
            guest_range_valid(cpu, cpu->gpr[29], 0x24u)) {
            const u32 infos = mem_read32(cpu, cpu->gpr[29] + 0x1cu);
            const u32 info = infos + cpu->gpr[31];
            if (guest_range_valid(cpu, info, 0x90u))
                guest_copy_string(cpu, info + 8u, g_file_wait_path,
                                  sizeof(g_file_wait_path));
        } else {
            guest_copy_string(cpu, cpu->gpr[31], g_file_wait_path,
                              sizeof(g_file_wait_path));
        }

        if (cpu->lr == SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_WAIT_RETURN &&
            guest_range_valid(cpu, cpu->gpr[30], 0x2cu)) {
            const u32 archive_holder = mem_read32(cpu, cpu->gpr[30] + 0x28u);
            const u32 holder_entry = archive_holder_find_entry_safe(
                cpu, archive_holder, cpu->gpr[31]);
            const u32 archive = guest_range_valid(
                cpu, holder_entry, 12u) ?
                    mem_read32(cpu, holder_entry) : 0u;
            if (archive != 0u) {
                ++g_file_wait_mounted_bypasses;
                cpu->gpr[3] = archive;
                cpu->pc = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_LOOKUP_RETURN;
                return 1;
            }
        }

        /*
         * setContext publishes mContext before it changes mState to READING.
         * At that point the entry is complete; waitReadDone only consumes the
         * one-slot notification and changes READING to DONE.  If the guest OS
         * lost that notification during a scheduler handoff, blocking here can
         * never make additional progress, so finish the equivalent state
         * transition directly.
         */
        if (state == 1u) {
            mem_write32(cpu, entry + 0x0cu, 2u);
            ++g_file_wait_ready_bypasses;
            cpu->pc = cpu->lr & ~3u;
            return 1;
        }

        if (state > 2u &&
            cpu->lr == SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_WAIT_RETURN &&
            g_file_wait_invalid_repairs < 32u) {
            const u32 loader = cpu->gpr[30];
            const u32 path = cpu->gpr[31];
            const u32 infos = guest_range_valid(cpu, loader, 0x2cu) ?
                mem_read32(cpu, loader + 0x1cu) : 0u;
            u32 request_count = guest_range_valid(cpu, loader, 0x2cu) ?
                mem_read32(cpu, loader + 0x20u) : 0u;
            const u32 file_holder = guest_range_valid(cpu, loader, 0x2cu) ?
                mem_read32(cpu, loader + 0x24u) : 0u;
            const u32 file_entries =
                guest_range_valid(cpu, file_holder, 12u) ?
                    mem_read32(cpu, file_holder) : 0u;
            u32 file_count = guest_range_valid(cpu, file_holder, 12u) ?
                mem_read32(cpu, file_holder + 8u) : 0u;
            s32 disc_entry = -1;
            u32 matching_entry = 0u;

            if (file_count > 384u)
                file_count = 384u;
            if (smg3ds_disc_resolve_path(cpu, path, &disc_entry) &&
                guest_range_valid(cpu, file_entries, file_count * 4u)) {
                for (u32 i = 0u; i < file_count; ++i) {
                    const u32 candidate =
                        mem_read32(cpu, file_entries + i * 4u);
                    if (guest_range_valid(cpu, candidate, 0x38u) &&
                        (s32)mem_read32(cpu, candidate) == disc_entry &&
                        mem_read32(cpu, candidate + 0x0cu) <= 2u) {
                        matching_entry = candidate;
                        break;
                    }
                }
            }

            if (matching_entry != 0u) {
                if (request_count > 0u && request_count <= 1024u &&
                    guest_range_valid(cpu, infos, request_count * 0x90u)) {
                    for (u32 i = 0u; i < request_count; ++i) {
                        const u32 info = infos + i * 0x90u;
                        char request_path[96];
                        guest_copy_string(cpu, info + 8u, request_path,
                                          sizeof(request_path));
                        if (strcmp(request_path, g_file_wait_path) == 0) {
                            mem_write32(cpu, info + 0x8cu,
                                        matching_entry);
                            break;
                        }
                    }
                }
                g_file_wait_reconciled_entry = matching_entry;
                ++g_file_wait_invalid_repairs;
                cpu->gpr[3] = matching_entry;
                cpu->pc = SMG3DS_FILE_HOLDER_WAIT_READ_DONE;
                return 1;
            }

            if (request_count > 0u && request_count <= 1024u &&
                guest_range_valid(cpu, infos, request_count * 0x90u)) {
                for (u32 i = 0u; i < request_count; ++i) {
                    const u32 info = infos + i * 0x90u;
                    char request_path[96];
                    guest_copy_string(cpu, info + 8u, request_path,
                                      sizeof(request_path));
                    if (strcmp(request_path, g_file_wait_path) != 0)
                        continue;
                    for (u32 j = i; j + 1u < request_count; ++j) {
                        const u32 dst = infos + j * 0x90u;
                        const u32 src = dst + 0x90u;
                        for (u32 offset = 0u; offset < 0x90u;
                             offset += 4u) {
                            mem_write32(cpu, dst + offset,
                                        mem_read32(cpu, src + offset));
                        }
                    }
                    --request_count;
                    mem_write32(cpu, loader + 0x20u, request_count);
                    break;
                }
            }

            if (file_count > 0u && file_count <= 384u &&
                guest_range_valid(cpu, file_entries, file_count * 4u)) {
                for (u32 i = 0u; i < file_count; ++i) {
                    if (mem_read32(cpu, file_entries + i * 4u) != entry)
                        continue;
                    for (u32 j = i; j + 1u < file_count; ++j) {
                        mem_write32(cpu, file_entries + j * 4u,
                                    mem_read32(cpu,
                                               file_entries + (j + 1u) * 4u));
                    }
                    --file_count;
                    mem_write32(cpu, file_holder + 8u, file_count);
                    break;
                }
            }

            ++g_file_wait_invalid_repairs;
            g_file_wait_reentry_pending = true;
            g_file_wait_reentry_thread = mem_read32(cpu, 0x800000e4u);
            g_file_wait_reentry_loader = loader;
            g_file_wait_reentry_path = path;
            cpu->gpr[3] = loader;
            cpu->gpr[4] = path;
            cpu->gpr[5] = guest_stage_archive_heap(cpu);
            cpu->gpr[6] = 0u;
            cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_FILE;
            cpu->pc = SMG3DS_FILE_LOADER_REQUEST_MOUNT_ARCHIVE;
            return 1;
        }
    }

    if (address == SMG3DS_FILE_HOLDER_WAIT_READ_DONE &&
        cpu->gpr[3] != 0u &&
        cpu->lr == SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_WAIT_RETURN) {
        char archive_path[96];
        /*
         * receiveArchive's holder lookup lives in the same generated chunk as
         * its wait return, so neither address normally passes through this
         * dispatcher. Return through receiveFile's already-forced wait label
         * instead; it gives us a safe point to skip corrupt holder entries.
         */
        guest_copy_string(cpu, cpu->gpr[31], archive_path,
                          sizeof(archive_path));
        if (archive_path[0] != '\0' &&
            file_loader_core_valid(cpu, cpu->gpr[30])) {
            g_archive_wait_redirect_pending = true;
            g_archive_wait_redirect_thread = mem_read32(cpu, 0x800000e4u);
            g_archive_wait_redirect_loader = cpu->gpr[30];
            g_archive_wait_redirect_path = cpu->gpr[31];
            ++g_archive_wait_redirects;
            cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_WAIT_RETURN;
        }
    }

    if ((address == SMG3DS_FILE_LOADER_RECEIVE_WAIT_RETURN ||
         (address == SMG3DS_FILE_HOLDER_WAIT_READ_DONE &&
          cpu->gpr[3] == 0u)) &&
        g_archive_wait_redirect_pending &&
        mem_read32(cpu, 0x800000e4u) == g_archive_wait_redirect_thread) {
        const u32 archive_holder =
            guest_range_valid(cpu, g_archive_wait_redirect_loader, 0x2cu) ?
                mem_read32(cpu, g_archive_wait_redirect_loader + 0x28u) : 0u;
        const u32 holder_entry = archive_holder_find_entry_safe(
            cpu, archive_holder, g_archive_wait_redirect_path);
        const u32 archive = guest_range_valid(
            cpu, holder_entry, 12u) ?
                mem_read32(cpu, holder_entry) : 0u;
        /*
         * A generated receiveArchive block can call waitReadDone again before
         * its redirected return address reaches the dispatcher. Complete the
         * already-validated holder lookup here, ahead of the generic null-file
         * retry, once the archive publisher has made the entry visible.
         */
        if (archive != 0u ||
            address == SMG3DS_FILE_LOADER_RECEIVE_WAIT_RETURN) {
            cpu->gpr[3] = archive;
            g_archive_wait_redirect_pending = false;
            g_file_holder_retry_pending = false;
            cpu->pc = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_LOOKUP_RETURN;
            return 1;
        }
    }

    if (address == SMG3DS_FILE_HOLDER_WAIT_READ_DONE &&
        cpu->gpr[3] == 0u) {
        /*
         * receiveFile, receiveArchive, and receiveAllRequestedFile share the
         * same publication race: addRequest increments the visible request
         * count before its caller stores RequestFileInfo::mFileEntry. Never
         * let waitReadDone turn a null entry into the unsignalable queue at
         * address 0x18. Re-enter the owning receive operation and use a VI
         * interrupt to let the preempted publisher finish first.
         */
        ++g_file_holder_null_wait_retries;
        g_file_holder_null_wait_lr = cpu->lr;
        if (!vi_interrupt_pending())
            signal_video_retrace();
        if (cpu->lr == SMG3DS_FILE_LOADER_RECEIVE_WAIT_RETURN ||
            cpu->lr == SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_WAIT_RETURN) {
            g_file_holder_retry_pending = true;
            g_file_holder_retry_thread =
                mem_read32(cpu, 0x800000e4u);
            g_file_holder_retry_return = cpu->lr;
            g_file_holder_retry_loader = cpu->gpr[30];
            g_file_holder_retry_path = cpu->gpr[31];
            cpu->gpr[3] = g_file_holder_retry_loader;
            cpu->gpr[4] = g_file_holder_retry_path;
            cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_INFO_RETURN;
            cpu->pc = SMG3DS_FILE_LOADER_GET_REQUEST_INFO;
            return 1;
        }
        if (cpu->lr == SMG3DS_FILE_LOADER_RECEIVE_ALL_WAIT_RETURN) {
            const u32 infos = mem_read32(cpu, cpu->gpr[29] + 0x1cu);
            const u32 info = infos + cpu->gpr[31];
            const u32 entry = guest_range_valid(cpu, info, 0x90u) ?
                mem_read32(cpu, info + 0x8cu) : 0u;
            if (entry != 0u)
                cpu->gpr[3] = entry;
            cpu->pc = SMG3DS_FILE_HOLDER_WAIT_READ_DONE;
            return 1;
        }
        cpu->pc = SMG3DS_FILE_HOLDER_WAIT_READ_DONE;
        return 1;
    }

    if (address == SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE) {
        char path[96];
        const u32 current_thread = mem_read32(cpu, 0x800000e4u);
        if (g_archive_holder_rebuild_state != 0u &&
            current_thread == g_archive_holder_rebuild_thread) {
            if (g_archive_holder_rebuild_state == 1u &&
                cpu->gpr[3] != 0u) {
                g_archive_holder_after = cpu->gpr[3];
                g_archive_holder_rebuild_state = 2u;
                cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE;
                cpu->pc = SMG3DS_ARCHIVE_HOLDER_CTOR;
                return 1;
            }
            if (g_archive_holder_rebuild_state == 2u &&
                guest_range_valid(cpu, cpu->gpr[3], 12u)) {
                g_archive_holder_after = cpu->gpr[3];
                mem_write32(cpu, g_archive_receive_loader + 0x28u,
                            g_archive_holder_after);
            }
            g_archive_holder_rebuild_state = 0u;
            cpu->gpr[3] = g_archive_receive_loader;
            cpu->gpr[4] = g_archive_receive_path;
            cpu->lr = g_archive_receive_return;
        }
        if (g_archive_receive_priority_state != 0u &&
            current_thread == g_archive_receive_thread) {
            cpu->gpr[3] = g_archive_receive_loader;
            cpu->gpr[4] = g_archive_receive_path;
            if (g_archive_receive_priority_state == 2u)
                cpu->lr = g_archive_receive_return;
        }
        if (g_archive_repair_send_pending &&
            current_thread == g_archive_receive_thread) {
            cpu->gpr[3] = g_archive_receive_loader;
            cpu->gpr[4] = g_archive_receive_path;
            cpu->lr = g_archive_receive_return;
            g_archive_repair_send_pending = false;
        }
        cpu->gpr[3] = reconcile_file_loader_identity(cpu, cpu->gpr[3]);
        guest_copy_string(cpu, cpu->gpr[4], path, sizeof(path));
        if (!g_archive_rebuild_pending &&
            g_archive_holder_rebuild_state == 0u &&
            strcmp(path, g_archive_recovery_path) != 0) {
            snprintf(g_archive_recovery_path,
                     sizeof(g_archive_recovery_path), "%s", path);
            g_archive_rebuild_attempts = 0u;
            g_archive_holder_rebuild_attempts = 0u;
        }
        if (path[0] != '\0' && file_loader_core_valid(cpu, cpu->gpr[3]) &&
            !file_loader_archive_mounted(cpu, cpu->gpr[3], cpu->gpr[4])) {
            const u32 loader = cpu->gpr[3];
            const u32 archive_holder =
                guest_range_valid(cpu, loader, 0x2cu) ?
                    mem_read32(cpu, loader + 0x28u) : 0u;
            const u32 archive_vector =
                guest_range_valid(cpu, archive_holder, 12u) ?
                    mem_read32(cpu, archive_holder) : 0u;
            const u32 archive_capacity =
                guest_range_valid(cpu, archive_holder, 12u) ?
                    mem_read32(cpu, archive_holder + 4u) : 0u;
            const u32 archive_count =
                guest_range_valid(cpu, archive_holder, 12u) ?
                    mem_read32(cpu, archive_holder + 8u) : 0u;
            const bool archive_holder_valid =
                guest_range_valid(cpu, archive_holder, 36u) &&
                archive_capacity == 384u &&
                archive_count <= archive_capacity &&
                guest_range_valid(cpu, archive_vector,
                                  archive_capacity * 4u);
            g_archive_holder_before = archive_holder;
            g_archive_holder_vector = archive_vector;
            g_archive_holder_capacity = archive_capacity;
            g_archive_holder_count = archive_count;
            if (!archive_holder_valid &&
                g_archive_holder_rebuild_attempts < 3u) {
                g_archive_receive_loader = loader;
                g_archive_receive_path = cpu->gpr[4];
                g_archive_receive_return = cpu->lr;
                g_archive_holder_rebuild_thread = current_thread;
                ++g_archive_holder_rebuild_attempts;
                /* Never run the constructor through a non-null bad pointer. */
                g_archive_holder_rebuild_state = 1u;
                cpu->gpr[3] = 36u;
                cpu->pc = SMG3DS_OPERATOR_NEW;
                cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE;
                return 1;
            }
            const u32 infos = guest_range_valid(cpu, loader, 0x24u) ?
                mem_read32(cpu, loader + 0x1cu) : 0u;
            u32 count = guest_range_valid(cpu, loader, 0x24u) ?
                mem_read32(cpu, loader + 0x20u) : 0u;
            u32 entry = 0u;
            u32 request_info = 0u;
            bool found = false;
            for (u32 i = 0u; i < count && i < 1024u; ++i) {
                const u32 info = infos + i * 0x90u;
                char request_path[96];
                if (!guest_range_valid(cpu, info, 0x90u))
                    break;
                guest_copy_string(cpu, info + 8u, request_path,
                                  sizeof(request_path));
                if (strcmp(request_path, path) == 0) {
                    found = true;
                    request_info = info;
                    entry = mem_read32(cpu, info + 0x8cu);
                    break;
                }
            }
            if (found && entry == 0u) {
                s32 disc_entry = -1;
                const u32 file_holder =
                    mem_read32(cpu, loader + 0x24u);
                const u32 file_entries =
                    guest_range_valid(cpu, file_holder, 12u) ?
                        mem_read32(cpu, file_holder) : 0u;
                const u32 holder_count =
                    guest_range_valid(cpu, file_holder, 12u) ?
                        mem_read32(cpu, file_holder + 8u) : 0u;
                u32 file_count = holder_count;
                u32 matching_entry = 0u;
                if (file_count > 384u)
                    file_count = 384u;
                if (smg3ds_disc_resolve_path(cpu, cpu->gpr[4],
                                             &disc_entry)) {
                    for (u32 i = 0u; i < file_count; ++i) {
                        const u32 candidate =
                            mem_read32(cpu, file_entries + i * 4u);
                        if (guest_range_valid(cpu, candidate, 0x38u) &&
                            (s32)mem_read32(cpu, candidate) ==
                                disc_entry) {
                            matching_entry = candidate;
                            break;
                        }
                    }
                }
                g_archive_repair_info = request_info;
                g_archive_repair_file_entry = matching_entry;
                if (matching_entry != 0u) {
                    mem_write32(cpu, request_info + 0x8cu,
                                matching_entry);
                    entry = matching_entry;
                    if (mem_read32(cpu, matching_entry + 4u) == 0u &&
                        mem_read32(cpu, matching_entry + 0x0cu) == 0u) {
                        g_archive_receive_thread = current_thread;
                        g_archive_receive_loader = cpu->gpr[3];
                        g_archive_receive_path = cpu->gpr[4];
                        g_archive_receive_return = cpu->lr;
                        g_archive_repair_send_pending = true;
                        ++g_archive_repair_submissions;
                        cpu->gpr[3] =
                            mem_read32(cpu, loader) + 8u;
                        cpu->gpr[4] = request_info;
                        cpu->gpr[5] = 0u;
                        cpu->lr =
                            SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE;
                        cpu->pc = SMG3DS_OS_SEND_MESSAGE;
                        return 1;
                    }
                }

                /*
                 * The publisher can be stranded after addRequest made the
                 * RequestFileInfo visible but before FileHolder::add stored
                 * its entry. Remove that incomplete fixed-array record, trim
                 * a failed trailing FileHolder allocation if one exists, and
                 * invoke the game's normal requestMountArchive path again on
                 * this thread. That recreates both objects and submits the
                 * loader message atomically from the receiver's perspective.
                 */
                if (g_archive_rebuild_attempts < 3u) {
                    const u32 orphan_index =
                        (request_info - infos) / 0x90u;
                    if (count <= 1024u && orphan_index < count) {
                        for (u32 i = orphan_index; i + 1u < count; ++i) {
                            const u32 dst = infos + i * 0x90u;
                            const u32 src = dst + 0x90u;
                            for (u32 offset = 0u; offset < 0x90u;
                                 offset += 4u) {
                                mem_write32(cpu, dst + offset,
                                            mem_read32(cpu, src + offset));
                            }
                        }
                        --count;
                        mem_write32(cpu, loader + 0x20u, count);
                    } else {
                        mem_write8(cpu, request_info + 8u, 0u);
                    }

                    g_archive_rebuild_file_count = holder_count;
                    if (holder_count > 0u && holder_count <= 384u &&
                        guest_range_valid(cpu, file_entries,
                                          holder_count * 4u) &&
                        mem_read32(cpu, file_entries +
                                         (holder_count - 1u) * 4u) == 0u) {
                        g_archive_rebuild_file_count = holder_count - 1u;
                        mem_write32(cpu, file_holder + 8u,
                                    g_archive_rebuild_file_count);
                    }

                    g_archive_receive_thread = current_thread;
                    g_archive_receive_loader = loader;
                    g_archive_receive_path = cpu->gpr[4];
                    g_archive_receive_return = cpu->lr;
                    g_archive_rebuild_thread = current_thread;
                    g_archive_rebuild_orphan_index = orphan_index;
                    g_archive_rebuild_pending = true;
                    ++g_archive_rebuild_attempts;
                    cpu->gpr[3] = loader;
                    cpu->gpr[4] = g_archive_receive_path;
                    g_archive_rebuild_heap = guest_stage_archive_heap(cpu);
                    cpu->gpr[5] = g_archive_rebuild_heap;
                    cpu->gpr[6] = 0u;
                    cpu->lr = SMG3DS_ARCHIVE_REBUILD_RETURN;
                    cpu->pc = SMG3DS_FILE_LOADER_REQUEST_MOUNT_ARCHIVE;
                    return 1;
                }
            }
            if (!found && g_archive_rebuild_attempts < 3u) {
                const u32 file_holder =
                    mem_read32(cpu, loader + 0x24u);
                const u32 file_entries =
                    guest_range_valid(cpu, file_holder, 12u) ?
                        mem_read32(cpu, file_holder) : 0u;
                const u32 holder_count =
                    guest_range_valid(cpu, file_holder, 12u) ?
                        mem_read32(cpu, file_holder + 8u) : 0u;

                g_archive_rebuild_file_count = holder_count;
                if (holder_count > 0u && holder_count <= 384u &&
                    guest_range_valid(cpu, file_entries,
                                      holder_count * 4u) &&
                    mem_read32(cpu, file_entries +
                                     (holder_count - 1u) * 4u) == 0u) {
                    g_archive_rebuild_file_count = holder_count - 1u;
                    mem_write32(cpu, file_holder + 8u,
                                g_archive_rebuild_file_count);
                }

                g_archive_receive_thread = current_thread;
                g_archive_receive_loader = loader;
                g_archive_receive_path = cpu->gpr[4];
                g_archive_receive_return = cpu->lr;
                g_archive_rebuild_thread = current_thread;
                g_archive_rebuild_orphan_index = 0xffffffffu;
                g_archive_rebuild_pending = true;
                ++g_archive_rebuild_attempts;
                cpu->gpr[3] = loader;
                cpu->gpr[4] = g_archive_receive_path;
                g_archive_rebuild_heap = guest_stage_archive_heap(cpu);
                cpu->gpr[5] = g_archive_rebuild_heap;
                cpu->gpr[6] = 0u;
                cpu->lr = SMG3DS_ARCHIVE_REBUILD_RETURN;
                cpu->pc = SMG3DS_FILE_LOADER_REQUEST_MOUNT_ARCHIVE;
                return 1;
            }
            if (!found || entry == 0u) {
                ++g_archive_receive_preflight_retries;
                if (g_archive_receive_priority_state == 0u &&
                    guest_range_valid(cpu, current_thread, 0x2d8u)) {
                    g_archive_receive_priority_state = 1u;
                    g_archive_receive_thread = current_thread;
                    g_archive_receive_original_priority =
                        mem_read32(cpu, current_thread + 0x2d4u);
                    g_archive_receive_loader = cpu->gpr[3];
                    g_archive_receive_path = cpu->gpr[4];
                    g_archive_receive_return = cpu->lr;
                    cpu->gpr[3] = current_thread;
                    cpu->gpr[4] = 31u;
                    cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE;
                    cpu->pc = SMG3DS_OS_SET_THREAD_PRIORITY;
                } else {
                    cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE;
                    cpu->pc = SMG3DS_OS_YIELD_THREAD;
                }
                return 1;
            }
            if (g_archive_receive_priority_state == 1u &&
                current_thread == g_archive_receive_thread) {
                const u32 current_thread = g_archive_receive_thread;
                const u32 original_priority =
                    g_archive_receive_original_priority;
                g_archive_receive_priority_state = 2u;
                cpu->gpr[3] = current_thread;
                cpu->gpr[4] = original_priority;
                cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE;
                cpu->pc = SMG3DS_OS_SET_THREAD_PRIORITY;
                return 1;
            }
            if (g_archive_receive_priority_state == 2u &&
                current_thread == g_archive_receive_thread)
                g_archive_receive_priority_state = 0u;
        }
    }

    if (address == SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_WAIT_RETURN) {
        const u32 loader = cpu->gpr[30];
        const u32 archive_holder =
            guest_range_valid(cpu, loader, 0x2cu) ?
                mem_read32(cpu, loader + 0x28u) : 0u;
        const u32 holder_entry =
            archive_holder_find_entry_safe(cpu, archive_holder,
                                           cpu->gpr[31]);
        cpu->gpr[3] =
            holder_entry != 0u &&
            guest_range_valid(cpu, holder_entry, 12u) ?
                mem_read32(cpu, holder_entry) : 0u;
        cpu->pc = SMG3DS_FILE_LOADER_RECEIVE_ARCHIVE_LOOKUP_RETURN;
        return 1;
    }

    if (address == SMG3DS_ARCHIVE_HOLDER_FIND_ENTRY) {
        const u32 holder = cpu->gpr[3];
        const u32 wanted_path = cpu->gpr[4];
        cpu->gpr[3] = archive_holder_find_entry_safe(
            cpu, holder, wanted_path);
        cpu->pc = cpu->lr & ~3u;
        return 1;
    }

    if (address == SMG3DS_FILE_LOADER_RECEIVE_FILE) {
        char path[96];
        guest_copy_string(cpu, cpu->gpr[4], path, sizeof(path));
        if (strcmp(path, "/StageData/FileSelect.arc") == 0) {
            ++g_file_select_receives;
            g_file_select_request_info = 0u;
            g_file_select_entry = 0u;
            g_file_select_context_before = 0u;
            g_file_select_state_before = 0u;
            g_file_select_queue_before = 0u;
            g_file_select_set_context = 0u;
            g_file_select_set_calls = 0u;
            g_file_select_context_after = 0u;
            g_file_select_state_after = 0u;
            g_file_select_queue_after = 0u;
            g_file_select_wait_repairs = 0u;
        }
    } else if (address == SMG3DS_FILE_LOADER_RECEIVE_INFO_RETURN) {
        if (g_file_holder_retry_pending &&
            mem_read32(cpu, 0x800000e4u) ==
                g_file_holder_retry_thread) {
            const u32 info = cpu->gpr[3];
            const u32 entry = guest_range_valid(cpu, info, 0x90u) ?
                mem_read32(cpu, info + 0x8cu) : 0u;
            if (entry == 0u) {
                if (!vi_interrupt_pending())
                    signal_video_retrace();
                cpu->gpr[3] = g_file_holder_retry_loader;
                cpu->gpr[4] = g_file_holder_retry_path;
                cpu->lr = SMG3DS_FILE_LOADER_RECEIVE_INFO_RETURN;
                cpu->pc = SMG3DS_FILE_LOADER_GET_REQUEST_INFO;
                return 1;
            }
            cpu->gpr[3] = entry;
            cpu->lr = g_file_holder_retry_return;
            cpu->pc = SMG3DS_FILE_HOLDER_WAIT_READ_DONE;
            g_file_holder_retry_pending = false;
            return 1;
        }
        char path[96];
        guest_copy_string(cpu, cpu->gpr[31], path, sizeof(path));
        if (strcmp(path, "/StageData/FileSelect.arc") == 0) {
            g_file_select_request_info = cpu->gpr[3];
            if (guest_range_valid(cpu, cpu->gpr[3], 0x90u))
                g_file_select_entry = mem_read32(cpu, cpu->gpr[3] + 0x8cu);
        }
    } else if (address == SMG3DS_FILE_HOLDER_WAIT_READ_DONE &&
               cpu->gpr[3] == g_file_select_entry) {
        g_file_select_context_before = mem_read32(cpu, cpu->gpr[3] + 4u);
        g_file_select_state_before = mem_read32(cpu, cpu->gpr[3] + 0x0cu);
        g_file_select_queue_before = mem_read32(cpu, cpu->gpr[3] + 0x2cu);
    } else if (address == SMG3DS_FILE_HOLDER_SET_CONTEXT &&
               cpu->gpr[3] == g_file_select_entry) {
        g_file_select_set_context = cpu->gpr[4];
        ++g_file_select_set_calls;
    } else if (address == SMG3DS_FILE_LOADER_RECEIVE_WAIT_RETURN) {
        char path[96];
        guest_copy_string(cpu, cpu->gpr[31], path, sizeof(path));
        if (strcmp(path, "/StageData/FileSelect.arc") == 0 &&
            guest_range_valid(cpu, g_file_select_entry, 0x30u)) {
            g_file_select_context_after =
                mem_read32(cpu, g_file_select_entry + 4u);
            g_file_select_state_after =
                mem_read32(cpu, g_file_select_entry + 0x0cu);
            g_file_select_queue_after =
                mem_read32(cpu, g_file_select_entry + 0x2cu);
        }
    }

    if (address == SMG3DS_STAGE_ARCHIVE_LOOKUP) {
        guest_copy_string(cpu, cpu->gpr[3], g_last_stage_archive,
                          sizeof(g_last_stage_archive));
        ++g_stage_archive_requests;
    }

    if (service_yaz0_decompression(cpu) ||
        service_optional_platform_module(cpu) ||
        service_coherent_cache_range(cpu))
        return 1;

    s32 entry_index;
    bool opened;

    if (address == SMG3DS_KPAD_READ)
        return service_kpad_read(cpu) ? 1 : 0;

    if (smg3ds_petari_dispatch(cpu, address))
        return 1;

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
    if (panic.caller == 0x8040a02cu && panic.arguments[2] != 0u)
        panic.asset_path_address = panic.arguments[2];
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
        (panic.caller == 0x8040a02cu ||
         panic.caller == 0x8040ba48u)) {
        u32 frame = cpu->gpr[1];
        const u32 allocator_frame = guest_range_valid(cpu, frame, 4u) ?
            mem_read32(cpu, frame) : frame;
        const u32 heap = guest_range_valid(cpu, allocator_frame, 32u) ?
            mem_read32(cpu, allocator_frame + 16u) : 0u;
        const u32 alloc_size = guest_range_valid(cpu, allocator_frame, 32u) ?
            mem_read32(cpu, allocator_frame + 20u) : 0u;
        const u32 alloc_align = guest_range_valid(cpu, allocator_frame, 32u) ?
            mem_read32(cpu, allocator_frame + 24u) : 0u;
        const u32 alloc_result = guest_range_valid(cpu, allocator_frame, 32u) ?
            mem_read32(cpu, allocator_frame + 28u) : 0u;
        const u32 heap_size = guest_range_valid(cpu, heap, 112u) ?
            mem_read32(cpu, heap + 56u) : 0u;
        const u32 heap_free = guest_range_valid(cpu, heap, 112u) ?
            mem_read32(cpu, heap + 108u) : 0u;
        length += snprintf(report + length, sizeof(report) - (size_t)length,
            "alloc: sp=%08lX heap=%08lX size=%08lX align=%08lX result=%08lX "
            "capacity=%08lX free=%08lX\n",
            (unsigned long)allocator_frame, (unsigned long)heap,
            (unsigned long)alloc_size, (unsigned long)alloc_align,
            (unsigned long)alloc_result, (unsigned long)heap_size,
            (unsigned long)heap_free);
        if (length > 0 && length < (int)sizeof(report) &&
            guest_range_valid(cpu, frame, 40u)) {
            length += snprintf(report + length,
                sizeof(report) - (size_t)length,
                "trace: %08lX", (unsigned long)mem_read32(cpu, frame + 36u));
            frame = mem_read32(cpu, frame);
            for (u32 depth = 0u;
                 depth < 10u && length > 0 && length < (int)sizeof(report) &&
                 guest_range_valid(cpu, frame, 8u);
                 ++depth) {
                const u32 next = mem_read32(cpu, frame);
                length += snprintf(report + length,
                    sizeof(report) - (size_t)length,
                    " %08lX", (unsigned long)mem_read32(cpu, frame + 4u));
                if (next <= frame || (next & 3u) != 0u)
                    break;
                frame = next;
            }
            if (length > 0 && length < (int)sizeof(report))
                length += snprintf(report + length,
                    sizeof(report) - (size_t)length, "\n");
        }
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

static bool mem2_offset(uint32_t address, uint32_t size, uint32_t* offset)
{
    const uint32_t physical = mmio_physical(address);
    if (physical < 0x10000000u ||
        physical + size > 0x10000000u + SMG3DS_MEM2_SIZE)
        return false;
    *offset = physical - 0x10000000u;
    return true;
}

static uint8_t* mem2_restore_page(uint32_t page);

static void mem2_initialize(void)
{
    uint32_t page;
    for (page = 0u; page < SMG3DS_MEM2_PAGE_COUNT; ++page)
        g_mem2_swap_slots[page] = (uint16_t)SMG3DS_MEM2_SWAP_SLOT_NONE;
}

static bool mem2_swap_open(void)
{
    g_mem2_swap_file = fopen("sdmc:/smg3ds-mem2.swap", "w+b");
    if (g_mem2_swap_file == NULL)
        return false;
    setvbuf(g_mem2_swap_file, NULL, _IONBF, 0);
    return true;
}

static void mem2_touch_page(uint32_t page)
{
    g_mem2_page_last_use[page] = ++g_mem2_page_clock;
}

static uint8_t* mem2_access_page(uint32_t page)
{
    uint8_t* data = g_mem2_pages[page];
    if (data == NULL &&
        g_mem2_swap_slots[page] != SMG3DS_MEM2_SWAP_SLOT_NONE)
        data = mem2_restore_page(page);
    if (data != NULL)
        mem2_touch_page(page);
    return data;
}

static uint64_t mem2_read_sparse(uint32_t offset, uint8_t size)
{
    uint64_t value = 0;
    uint8_t i;
    const uint32_t page = offset >> SMG3DS_MEM2_PAGE_SHIFT;
    const uint32_t in_page = offset & (SMG3DS_MEM2_PAGE_SIZE - 1u);
    const uint8_t* data;

    if (size <= SMG3DS_MEM2_PAGE_SIZE - in_page) {
        data = mem2_access_page(page);
        if (data == NULL)
            return 0u;
        data += in_page;
        if (size == 1u)
            return data[0];
        if (size == 2u)
            return read_be16(data);
        if (size == 4u)
            return read_be32(data);
        if (size == 8u)
            return read_be64(data);
    }
    for (i = 0; i < size; ++i) {
        const uint32_t current = offset + i;
        const uint32_t page = current >> SMG3DS_MEM2_PAGE_SHIFT;
        const uint32_t in_page = current & (SMG3DS_MEM2_PAGE_SIZE - 1u);
        const uint8_t* data = mem2_access_page(page);
        const uint8_t byte = data == NULL ? 0u : data[in_page];
        value = (value << 8) | byte;
    }
    return value;
}

static void mem2_release_page(uint32_t page)
{
    if (g_mem2_pages[page] == NULL)
        return;
    if (g_mem2_pages_normal[page])
        free(g_mem2_pages[page]);
    else
        linearFree(g_mem2_pages[page]);
    g_mem2_pages[page] = NULL;
    g_mem2_pages_normal[page] = false;
    --g_mem2_pages_used;
}

static uint32_t mem2_reclaim_zero_pages(void)
{
    uint32_t page;
    uint32_t reclaimed = 0u;

    for (page = 0u; page < SMG3DS_MEM2_PAGE_COUNT; ++page) {
        uint32_t offset;
        uint8_t* data = g_mem2_pages[page];
        if (data == NULL)
            continue;
        for (offset = 0u; offset < SMG3DS_MEM2_PAGE_SIZE; ++offset) {
            if (data[offset] != 0u)
                break;
        }
        if (offset != SMG3DS_MEM2_PAGE_SIZE)
            continue;
        g_mem2_swap_slots[page] = (uint16_t)SMG3DS_MEM2_SWAP_SLOT_NONE;
        g_mem2_pages_dirty[page] = false;
        mem2_release_page(page);
        ++reclaimed;
    }
    g_mem2_pages_reclaimed += reclaimed;
    return reclaimed;
}

static bool mem2_write_swap_page(uint32_t page)
{
    uint16_t slot = g_mem2_swap_slots[page];
    bool new_slot = false;

    if (g_mem2_swap_file == NULL)
        return false;
    if (slot == SMG3DS_MEM2_SWAP_SLOT_NONE) {
        if (g_mem2_swap_slots_used >= SMG3DS_MEM2_PAGE_COUNT)
            return false;
        slot = (uint16_t)g_mem2_swap_slots_used++;
        g_mem2_swap_slots[page] = slot;
        new_slot = true;
    } else if (!g_mem2_pages_dirty[page]) {
        return true;
    }
    if (fseek(g_mem2_swap_file,
              (long)((uint32_t)slot * SMG3DS_MEM2_PAGE_SIZE), SEEK_SET) != 0 ||
        fwrite(g_mem2_pages[page], 1u, SMG3DS_MEM2_PAGE_SIZE,
               g_mem2_swap_file) != SMG3DS_MEM2_PAGE_SIZE) {
        if (new_slot) {
            g_mem2_swap_slots[page] =
                (uint16_t)SMG3DS_MEM2_SWAP_SLOT_NONE;
            --g_mem2_swap_slots_used;
        }
        ++g_mem2_swap_failures;
        return false;
    }
    g_mem2_pages_dirty[page] = false;
    return true;
}

static bool mem2_evict_page(uint32_t avoid_page)
{
    uint32_t page;
    uint32_t oldest_page = SMG3DS_MEM2_PAGE_COUNT;
    uint32_t oldest_use = 0xffffffffu;

    for (page = 0u; page < SMG3DS_MEM2_PAGE_COUNT; ++page) {
        if (page == avoid_page || g_mem2_pages[page] == NULL)
            continue;
        if (g_mem2_page_last_use[page] < oldest_use) {
            oldest_page = page;
            oldest_use = g_mem2_page_last_use[page];
        }
    }
    if (oldest_page == SMG3DS_MEM2_PAGE_COUNT ||
        !mem2_write_swap_page(oldest_page))
        return false;
    mem2_release_page(oldest_page);
    ++g_mem2_pages_evicted;
    return true;
}

static uint8_t* mem2_allocate_page(uint32_t page)
{
    uint8_t* data;
    bool normal = false;
    uint32_t attempt;

    for (attempt = 0u; attempt < 3u; ++attempt) {
        data = (uint8_t*)linearMemAlign(SMG3DS_MEM2_PAGE_SIZE, 0x80u);
        normal = false;
        if (data == NULL) {
            data = (uint8_t*)calloc(1, SMG3DS_MEM2_PAGE_SIZE);
            normal = data != NULL;
        }
        if (data != NULL)
            break;
        if (attempt == 0u && mem2_reclaim_zero_pages() != 0u)
            continue;
        if (!mem2_evict_page(page))
            break;
    }

    g_mem2_pages_normal[page] = normal;
    if (data != NULL && !normal)
        memset(data, 0, SMG3DS_MEM2_PAGE_SIZE);
    if (data == NULL) {
        g_mem2_out_of_memory = true;
        return NULL;
    }
    g_mem2_pages[page] = data;
    g_mem2_pages_dirty[page] = false;
    mem2_touch_page(page);
    ++g_mem2_pages_used;
    return data;
}

static uint8_t* mem2_restore_page(uint32_t page)
{
    const uint16_t slot = g_mem2_swap_slots[page];
    uint8_t* data;

    if (slot == SMG3DS_MEM2_SWAP_SLOT_NONE || g_mem2_swap_file == NULL)
        return NULL;
    data = mem2_allocate_page(page);
    if (data == NULL)
        return NULL;
    if (fseek(g_mem2_swap_file,
              (long)((uint32_t)slot * SMG3DS_MEM2_PAGE_SIZE), SEEK_SET) != 0 ||
        fread(data, 1u, SMG3DS_MEM2_PAGE_SIZE,
              g_mem2_swap_file) != SMG3DS_MEM2_PAGE_SIZE) {
        mem2_release_page(page);
        ++g_mem2_swap_failures;
        return NULL;
    }
    g_mem2_pages_dirty[page] = false;
    mem2_touch_page(page);
    ++g_mem2_pages_loaded;
    return data;
}

static void mem2_write_sparse(uint32_t offset, uint64_t value, uint8_t size)
{
    uint8_t i;
    const uint32_t first_page = offset >> SMG3DS_MEM2_PAGE_SHIFT;
    const uint32_t first_in_page =
        offset & (SMG3DS_MEM2_PAGE_SIZE - 1u);

    if (size <= SMG3DS_MEM2_PAGE_SIZE - first_in_page) {
        uint8_t* data = mem2_access_page(first_page);
        if (data == NULL && value == 0u &&
            g_mem2_swap_slots[first_page] == SMG3DS_MEM2_SWAP_SLOT_NONE)
            return;
        if (data == NULL)
            data = mem2_allocate_page(first_page);
        if (data == NULL)
            return;
        data += first_in_page;
        if (size == 1u)
            data[0] = (uint8_t)value;
        else if (size == 2u)
            write_be16(data, (uint16_t)value);
        else if (size == 4u)
            write_be32(data, (uint32_t)value);
        else if (size == 8u)
            write_be64(data, value);
        else
            return;
        g_mem2_pages_dirty[first_page] = true;
        mem2_touch_page(first_page);
        return;
    }
    for (i = 0; i < size; ++i) {
        const uint32_t current = offset + i;
        const uint32_t page = current >> SMG3DS_MEM2_PAGE_SHIFT;
        const uint32_t in_page = current & (SMG3DS_MEM2_PAGE_SIZE - 1u);
        const uint8_t byte =
            (uint8_t)(value >> ((size - i - 1u) * 8u));
        uint8_t* data = mem2_access_page(page);
        if (data == NULL) {
            if (byte == 0u &&
                g_mem2_swap_slots[page] == SMG3DS_MEM2_SWAP_SLOT_NONE)
                continue;
            data = mem2_allocate_page(page);
            if (data == NULL)
                return;
        }
        data[in_page] = byte;
        g_mem2_pages_dirty[page] = true;
        mem2_touch_page(page);
    }
}

static void mem2_free_sparse(void)
{
    uint32_t page;
    for (page = 0; page < SMG3DS_MEM2_PAGE_COUNT; ++page) {
        mem2_release_page(page);
    }
    g_mem2_pages_used = 0;
}

static void mem2_swap_shutdown(void)
{
    if (g_mem2_swap_file != NULL) {
        fclose(g_mem2_swap_file);
        g_mem2_swap_file = NULL;
        remove("sdmc:/smg3ds-mem2.swap");
    }
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

static bool guest_async_worker_is_running(CPUState* cpu)
{
    u32 game_system;
    u32 game_obj_holder;
    u32 async_executor;
    u32 current_thread;

    if (cpu->gpr[13] == 0u)
        return false;
    game_system = mem_read32(cpu,
                             cpu->gpr[13] + (u32)(s32)(-14968));
    if (!guest_range_valid(cpu, game_system, 36u))
        return false;
    game_obj_holder = mem_read32(cpu, game_system + 32u);
    if (!guest_range_valid(cpu, game_obj_holder, 44u))
        return false;
    async_executor = mem_read32(cpu, game_obj_holder + 40u);
    if (!guest_range_valid(cpu, async_executor, 8u))
        return false;

    current_thread = mem_read32(cpu, 0x800000e4u);
    for (u32 i = 0u; i < 2u; ++i) {
        const u32 worker = mem_read32(cpu, async_executor + i * 4u);
        const u32 thread = guest_range_valid(cpu, worker, 12u) ?
            mem_read32(cpu, worker + 8u) : 0u;
        if (thread != 0u && current_thread == thread)
            return true;
    }
    return false;
}

static bool guest_async_work_pending(CPUState* cpu)
{
    u32 game_system;
    u32 game_obj_holder;
    u32 async_executor;
    u32 job_count;

    if (cpu->gpr[13] == 0u)
        return false;
    game_system = mem_read32(cpu,
                             cpu->gpr[13] + (u32)(s32)(-14968));
    if (!guest_range_valid(cpu, game_system, 36u))
        return false;
    game_obj_holder = mem_read32(cpu, game_system + 32u);
    if (!guest_range_valid(cpu, game_obj_holder, 44u))
        return false;
    async_executor = mem_read32(cpu, game_obj_holder + 40u);
    if (!guest_range_valid(cpu, async_executor, 1040u))
        return false;

    job_count = mem_read32(cpu, async_executor + 1036u);
    if (job_count > 256u)
        job_count = 256u;
    for (u32 i = 0u; i < job_count; ++i) {
        const u32 job = mem_read32(cpu, async_executor + 12u + i * 4u);
        if (guest_range_valid(cpu, job, 13u) &&
            mem_read8(cpu, job + 12u) == 0u)
            return true;
    }
    return false;
}

static bool guest_background_thread_is_running(CPUState* cpu)
{
    const u32 queue = guest_retrace_queue(cpu);
    const u32 main_waiter = queue != 0u ? mem_read32(cpu, queue + 8u) : 0u;
    const u32 current_thread = mem_read32(cpu, 0x800000e4u);

    return main_waiter != 0u && current_thread != 0u &&
           current_thread != main_waiter;
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
    printf("\x1b[1;1HSMG3DS / Azahar bring-up\n");
    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    if (is_new_3ds)
        osSetSpeedupEnable(true);
    printf("Hardware mode: %s\n", is_new_3ds ? "New 3DS" : "Old 3DS");
    printf("App memory: %lu MiB, heap %lu MiB, linear %lu MiB\n",
           (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20),
           (unsigned long)(envGetHeapSize() >> 20),
           (unsigned long)(envGetLinearHeapSize() >> 20));
    printf("DolRecomp CPU core: linked\n");
    printf("CPUState: %lu bytes\n", (unsigned long)sizeof(CPUState));

    CPUState cpu;
    mem2_initialize();
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
    printf("PICA200 presentation: %s\n",
           initial_gx->pica200_available ? "active" : "CPU fallback");
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
        /* MEM2 is committed in 4 KiB pages as the guest touches it. */
        mem2_ok = true;
        printf("MEM2 (64 MiB): %s\n", mem2_ok ? "ok" : "unavailable");
    }

#ifdef SMG3DS_WITH_GENERATED
    bool dol_ok = false;
    bool disc_ok = false;
    bool running = false;
    const Result sdmc_mount = archiveMountSdmc();
    /* Azahar may publish sdmc: before main and report "already mounted" here. */
    const bool mem2_swap_ok = mem2_swap_open();
    printf("MEM2 paging: %s\n", mem2_swap_ok ? "ready" : "unavailable");
    FILE* const debug_file = NULL;
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
            const bool guest_waiting_for_retrace =
                guest_is_waiting_for_retrace(&cpu);
            if (guest_waiting_for_retrace)
                g_vi_demand_pacing = true;
            /*
             * A heavyweight generated block can reach the host deadline just
             * after the main thread enters its retrace queue but before the OS
             * dispatcher installs the selected background thread.  Give that
             * handoff one unsignalled host slice whenever async work is
             * pending, then keep every background thread running until it
             * yields.  This covers both FunctionAsyncExecutor and the loader
             * threads it can block behind without deadlocking an idle guest.
             */
            const bool async_work_pending =
                guest_async_work_pending(&cpu);
            const bool background_running =
                guest_waiting_for_retrace &&
                (guest_background_thread_is_running(&cpu) ||
                 guest_async_worker_is_running(&cpu));
            const bool begin_background_handoff =
                guest_waiting_for_retrace && async_work_pending &&
                !g_vi_background_handoff_attempted;
            if ((!g_vi_demand_pacing ||
                 (guest_waiting_for_retrace &&
                   !vi_interrupt_pending())) &&
                !background_running && !begin_background_handoff) {
                signal_video_retrace();
                g_vi_background_handoff_attempted = false;
            } else if (background_running || begin_background_handoff) {
                g_vi_background_handoff_attempted = true;
            }
            for (u32 slice = 0;
                 slice < SMG3DS_MAX_DISPATCHES_PER_FRAME && running;
                 ++slice) {
                /*
                 * A generated block may cover a sizeable guest function.  Keep
                 * at least one block per frame, then yield before another block
                 * can starve presentation, input, and runtime diagnostics.
                 */
                if (slice != 0u &&
                    (slice & (SMG3DS_HOST_TIME_CHECK_INTERVAL - 1u)) == 0u &&
                    osGetTime() >= cpu_slice_deadline)
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
                /*
                 * The SDK pointer-array lookup at 0x80404a8c copies its
                 * element count into r31 and uses it directly as CTR.  A
                 * stale archive record can expose payload bytes as that
                 * count; letting the generated loop run then stalls boot for
                 * hundreds of millions of iterations.  Preserve the SDK's
                 * ordinary "not found" result for only an impossible count.
                 */
                if (cpu.pc == SMG3DS_PTR_ARRAY_FIND_LOOP &&
                    cpu.gpr[31] > 4096u) {
                    const u32 container = cpu.gpr[30];
                    const u32 storage = guest_range_valid(
                        &cpu, container, 4u) ?
                            mem_read32(&cpu, container) : 0u;
                    const u32 stored_count = guest_range_valid(
                        &cpu, storage, 8u) ?
                            mem_read32(&cpu, storage + 4u) : 0u;
                    if (!g_ptr_array_count_reported &&
                        g_debug_file != NULL) {
                        g_ptr_array_count_reported = true;
                        fprintf(g_debug_file,
                                "PTR_ARRAY_COUNT_REPAIRED count=%08lX "
                                "container=%08lX storage=%08lX "
                                "stored=%08lX key=%08lX lr=%08lX\n",
                                (unsigned long)cpu.gpr[31],
                                (unsigned long)container,
                                (unsigned long)storage,
                                (unsigned long)stored_count,
                                (unsigned long)cpu.gpr[3],
                                (unsigned long)cpu.lr);
                        fflush(g_debug_file);
                    }
                    cpu.pc = SMG3DS_PTR_ARRAY_FIND_NOT_FOUND;
                }
                /*
                 * ResTable::getResIndex assumes its table pointer is valid.
                 * ResourceHolderManager can temporarily expose an entry whose
                 * ResourceHolder is null while a layout holder with the same
                 * archive name is being created. Address zero aliases low
                 * guest memory here, turning its first word into a multi-
                 * billion-iteration CTR loop. A null table has no resources,
                 * so preserve getResIndex's normal not-found result.
                 */
                if (cpu.pc == SMG3DS_RESOURCE_TABLE_FIND_LOOP &&
                    cpu.gpr[31] == 0u) {
                    const u32 find_frame = cpu.gpr[1];
                    const u32 table_frame = guest_range_valid(
                        &cpu, find_frame, 4u) ?
                            mem_read32(&cpu, find_frame) : 0u;
                    const u32 wrapper_frame = guest_range_valid(
                        &cpu, table_frame, 4u) ?
                            mem_read32(&cpu, table_frame) : 0u;
                    const u32 resource_name = guest_range_valid(
                        &cpu, table_frame + 12u, 4u) ?
                            mem_read32(&cpu, table_frame + 12u) : 0u;
                    const u32 caller = guest_range_valid(
                        &cpu, wrapper_frame + 20u, 4u) ?
                            mem_read32(&cpu, wrapper_frame + 20u) : 0u;
                    if (!g_resource_table_null_reported &&
                        g_debug_file != NULL) {
                        char resource[96];

                        g_resource_table_null_reported = true;
                        guest_copy_string(&cpu, resource_name, resource,
                                          sizeof(resource));
                        fprintf(g_debug_file,
                                "RESOURCE_TABLE_NULL_NOT_FOUND "
                                "resource=%s ptr=%08lX caller=%08lX "
                                "frames=%08lX/%08lX/%08lX hash=%08lX\n",
                                resource[0] != '\0' ? resource : "-",
                                (unsigned long)resource_name,
                                (unsigned long)caller,
                                (unsigned long)find_frame,
                                (unsigned long)table_frame,
                                (unsigned long)wrapper_frame,
                                (unsigned long)cpu.gpr[3]);
                        fflush(g_debug_file);
                    }
                    cpu.pc = SMG3DS_RESOURCE_TABLE_FIND_NOT_FOUND;
                }
                int dispatched;
                cpu.downcount = 0;
                dispatched = dolrecomp_run_blocks(
                    &cpu, SMG3DS_DISPATCH_BATCH_BLOCKS);
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
                const bool stopped =
                    (!dispatched && !handled_system_call) || cpu.exception;
                if (stopped) {
                    const Smg3dsDiscStats* disc = smg3ds_disc_get_stats();
                    const u32 fault_pc = cpu.exception ? cpu.srr0 : cpu.pc;
                    const u32 fault_raw =
                        g_fault_cia == fault_pc ? g_fault_raw : 0u;
                    int fault_length = snprintf(
                        g_fault_message, sizeof(g_fault_message),
                        "STOP pc=%08lX ex=%08lX srr0=%08lX srr1=%08lX "
                        "cause=%08lX lr=%08lX raw=%08lX\n"
                        "ctr=%08lX cr=%08lX xer=%08lX sp=%08lX "
                        "r3obj=%08lX vtbl=%08lX slot34=%08lX\n"
                        "r24=%08lX r24a4=%08lX stage=%s/%lu "
                        "disc=%lu/%lu/%lu last=%08llX+%lu error=%s\n"
                        "fs=%lu info=%08lX entry=%08lX pre=%08lX/%lu/%lu "
                        "set=%lu/%08lX post=%08lX/%lu/%lu repair=%lu\n"
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
                        (unsigned long)cpu.ctr,
                        (unsigned long)cpu.cr,
                        (unsigned long)cpu.spr[1],
                        (unsigned long)cpu.gpr[1],
                        (unsigned long)cpu.gpr[3],
                        (unsigned long)mem_read32(&cpu, cpu.gpr[3]),
                        (unsigned long)mem_read32(
                            &cpu, mem_read32(&cpu, cpu.gpr[3]) + 0x34u),
                        (unsigned long)cpu.gpr[24],
                        (unsigned long)mem_read32(&cpu, cpu.gpr[24] + 0xa4u),
                        g_last_stage_archive,
                        (unsigned long)g_stage_archive_requests,
                        (unsigned long)disc->file_count,
                        (unsigned long)disc->read_requests,
                        (unsigned long)disc->read_failures,
                        (unsigned long long)disc->last_offset,
                        (unsigned long)disc->last_length,
                        smg3ds_disc_last_error(),
                        (unsigned long)g_file_select_receives,
                        (unsigned long)g_file_select_request_info,
                        (unsigned long)g_file_select_entry,
                        (unsigned long)g_file_select_context_before,
                        (unsigned long)g_file_select_state_before,
                        (unsigned long)g_file_select_queue_before,
                        (unsigned long)g_file_select_set_calls,
                        (unsigned long)g_file_select_set_context,
                        (unsigned long)g_file_select_context_after,
                        (unsigned long)g_file_select_state_after,
                        (unsigned long)g_file_select_queue_after,
                        (unsigned long)g_file_select_wait_repairs,
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
                        if (fault_length >= (int)sizeof(g_fault_message))
                            fault_length = (int)sizeof(g_fault_message) - 1;
                        svcOutputDebugString(g_fault_message, fault_length);
                        if (debug_file != NULL) {
                            fwrite(g_fault_message, 1, (size_t)fault_length,
                                   debug_file);
                            fflush(debug_file);
                        }
                    }
                    running = false;
                }
            }
#if 0
            if (total_dispatches != 0u) {
                const Smg3dsGxStats* gx = smg3ds_gx_get_stats();
                const Smg3dsExiStats* exi = smg3ds_exi_get_stats();
                const Smg3dsIosStats* ios = smg3ds_ios_get_stats();
                if (++g_status_frames >= SMG3DS_STATUS_UPDATE_FRAMES) {
                    g_status_frames = 0u;
                /* Spread console traffic across frames instead of one burst. */
                if (g_status_line == 0u)
                    printf("\x1b[17;1Hpc=%08lX ex=%lu blk=%lu\x1b[K",
                       (unsigned long)cpu.pc, (unsigned long)cpu.exception,
                       (unsigned long)total_dispatches);
                if (g_status_line == 1u)
                    printf("\x1b[18;1Hmmio r=%lu w=%lu gxw=%lu size=%lu\x1b[K",
                       (unsigned long)g_mmio_reads,
                       (unsigned long)g_mmio_writes,
                       (unsigned long)g_gx_mmio_writes,
                       (unsigned long)g_last_mmio_size);
                if (g_status_line == 2u)
                    printf("\x1b[19;1Hread=%08lX write=%08lX\x1b[K",
                       (unsigned long)g_last_mmio_read,
                       (unsigned long)g_last_mmio_write);
                if (g_status_line == 3u)
                    printf("\x1b[20;1Hsrr0=%08lX srr1=%08lX\x1b[K",
                       (unsigned long)cpu.srr0, (unsigned long)cpu.srr1);
                if (g_status_line == 4u)
                    printf("\x1b[21;1Hcause=%08lX lr=%08lX\x1b[K",
                       (unsigned long)cpu.program_exception,
                       (unsigned long)cpu.lr);
                if (g_status_line == 5u)
                    printf("\x1b[22;1Hdec=%08lX msr=%08lX\x1b[K",
                       (unsigned long)cpu.spr[22],
                       (unsigned long)cpu.msr);
                if (g_status_line == 6u)
                    printf("\x1b[23;1Hctl=%04lX hw=%04lX fifo=%lu\x1b[K",
                       (unsigned long)g_dsp_control,
                       (unsigned long)g_dsp_hardware_status,
                       (unsigned long)gx->fifo_bytes);
                if (g_status_line == 7u)
                    printf("\x1b[24;1Hgx d=%lu tri=%lu pix=%lu/%lu xfb=%lu\x1b[K",
                       (unsigned long)gx->draw_calls,
                       (unsigned long)gx->triangles,
                       (unsigned long)gx->rasterized_pixels,
                       (unsigned long)gx->colored_pixels,
                       (unsigned long)gx->xfb_colored_pixels);
                g_status_line = (g_status_line + 1u) %
                                SMG3DS_STATUS_LINE_COUNT;
                }
                if (++g_debug_frames >= SMG3DS_DEBUG_LOG_FRAMES) {
                    const u32 run_queue_addr =
                        cpu.gpr[13] + (u32)(s32)(-8048);
                    const u32 run_queue_bits =
                        mem_read32(&cpu, run_queue_addr);
                    const u32 run_queue_hint =
                        mem_read32(&cpu, cpu.gpr[13] + (u32)(s32)(-8052));
                    const u32 scheduler_disable =
                        mem_read32(&cpu, cpu.gpr[13] + (u32)(s32)(-8056));
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
                    const u32 error_watcher =
                        guest_range_valid(&cpu, game_system, 24u) ?
                            mem_read32(&cpu, game_system + 20u) : 0u;
                    const u32 error_spine =
                        guest_range_valid(&cpu, error_watcher, 36u) ?
                            mem_read32(&cpu, error_watcher + 4u) : 0u;
                    const u32 error_nerve =
                        guest_range_valid(&cpu, error_spine, 16u) ?
                            mem_read32(&cpu, error_spine + 4u) : 0u;
                    const s32 error_drive_status =
                        guest_range_valid(&cpu, error_watcher, 36u) ?
                            (s32)mem_read32(&cpu, error_watcher + 24u) : 0;
                    const u32 error_message =
                        guest_range_valid(&cpu, error_watcher, 36u) ?
                            mem_read32(&cpu, error_watcher + 12u) : 0u;
                    const s32 error_wpad_status =
                        guest_range_valid(&cpu, error_watcher, 36u) ?
                            (s32)mem_read32(&cpu, error_watcher + 32u) : 0;
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
                    const u32 async_thread0_queue =
                        guest_range_valid(&cpu, async_thread0, 0x304u) ?
                            mem_read32(&cpu, async_thread0 + 0x2dcu) : 0u;
                    u32 async_frame =
                        guest_range_valid(&cpu, async_thread0, 8u) ?
                            mem_read32(&cpu, async_thread0 + 4u) : 0u;
                    u32 async_callers[4] = {0u, 0u, 0u, 0u};
                    for (u32 i = 0u; i < 4u; ++i) {
                        if (!guest_range_valid(&cpu, async_frame, 8u))
                            break;
                        async_callers[i] = mem_read32(&cpu, async_frame + 4u);
                        const u32 next_frame = mem_read32(&cpu, async_frame);
                        if (next_frame == async_frame)
                            break;
                        async_frame = next_frame;
                    }
                    const u32 vector_900_0 =
                        mem_read32(&cpu, 0x80000900u);
                    const u32 vector_900_1 =
                        mem_read32(&cpu, 0x80000904u);
                    const u32 current_thread =
                        mem_read32(&cpu, 0x800000e4u);
                    const u32 current_context =
                        mem_read32(&cpu, 0x800000d4u);
                    const u32 current_priority =
                        guest_range_valid(&cpu, current_thread, 0x2d4u) ?
                            mem_read32(&cpu, current_thread + 0x2d0u) : 0xffffffffu;
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
                    fprintf(g_debug_file,
                            "ERROR_WATCHER obj=%08lX nerve=%08lX "
                            "drive=%ld message=%08lX wpad=%ld\n",
                            (unsigned long)error_watcher,
                            (unsigned long)error_nerve,
                            (long)error_drive_status,
                            (unsigned long)error_message,
                            (long)error_wpad_status);
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
                        "mem2=%lu oom=%u reclaim=%lu swap=%lu/%lu/%lu/%lu "
                        "perf=%lu/%lu/%lu/%lu "
                        "dec=%08lX dirq=%lu msr=%08lX tb=%08lX:%08lX r13=%08lX "
                        "gs=%08lX:%08lX/%08lX/%08lX/%08lX/%lu "
                        "sn=%08lX:%08lX/%08lX/%08lX/%08lX/%lu/%08lX/%lu "
                         "ax=%08lX/%lu/%08lX/%u w=%08lX:%08lX/%04lX/%08lX/%u,%08lX:%08lX/%04lX/%08lX/%u "
                         "aw=%08lX/%08lX/%08lX/%08lX/%08lX "
                        "rq=%08lX:%08lX/%08lX/%08lX ctx=%08lX/%08lX "
                        "v9=%08lX/%08lX last=%08lX/%08lX "
                        "sc=%lu fwr=%lu/%08lX fw=%lu/%lu/%lu/%lu/%08lX/%lu/%08lX/%08lX/%08lX:%s arp=%lu ar=%lu/%08lX/%08lX arb=%lu/%u/%lu/%lu/%08lX ah=%lu/%lu/%08lX>%08lX/%08lX/%lu/%lu ahl=%lu/%lu/%lu awr=%lu/%u cache=%lu cmp=%lu me=%lu/%08lX/%08lX yz=%lu/%lu kp=%lu/%lu/%04lX dsp=%lu dm=%08lX/%lu/%lu dt=%08lX/%08lX "
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
                        (unsigned long)g_mem2_pages_reclaimed,
                        (unsigned long)g_mem2_pages_evicted,
                        (unsigned long)g_mem2_pages_loaded,
                        (unsigned long)g_mem2_swap_slots_used,
                        (unsigned long)g_mem2_swap_failures,
                        (unsigned long)(g_cpu_slice_ticks /
                            (SYSCLOCK_ARM11 / 1000u)),
                        (unsigned long)(gx->raster_ticks /
                            (SYSCLOCK_ARM11 / 1000u)),
                        (unsigned long)(gx->efb_copy_ticks /
                            (SYSCLOCK_ARM11 / 1000u)),
                        (unsigned long)(gx->present_ticks /
                            (SYSCLOCK_ARM11 / 1000u)),
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
                        (unsigned long)async_thread0_queue,
                        (unsigned long)async_callers[0],
                        (unsigned long)async_callers[1],
                        (unsigned long)async_callers[2],
                        (unsigned long)async_callers[3],
                        (unsigned long)run_queue_addr,
                        (unsigned long)run_queue_bits,
                        (unsigned long)run_queue_hint,
                        (unsigned long)scheduler_disable,
                        (unsigned long)current_context,
                        (unsigned long)current_priority,
                        (unsigned long)vector_900_0,
                        (unsigned long)vector_900_1,
                        (unsigned long)g_last_mmio_read,
                        (unsigned long)g_last_mmio_write,
                        (unsigned long)g_system_calls,
                        (unsigned long)g_file_holder_null_wait_retries,
                        (unsigned long)g_file_holder_null_wait_lr,
                        (unsigned long)g_file_wait_calls,
                        (unsigned long)g_file_wait_ready_bypasses,
                        (unsigned long)g_file_wait_mounted_bypasses,
                        (unsigned long)g_file_wait_invalid_repairs,
                        (unsigned long)g_file_wait_entry,
                        (unsigned long)g_file_wait_state,
                        (unsigned long)g_file_wait_context,
                        (unsigned long)g_file_wait_lr,
                        (unsigned long)g_file_wait_reconciled_entry,
                        g_file_wait_path[0] != '\0' ? g_file_wait_path : "-",
                        (unsigned long)g_archive_receive_preflight_retries,
                        (unsigned long)g_archive_repair_submissions,
                        (unsigned long)g_archive_repair_info,
                        (unsigned long)g_archive_repair_file_entry,
                        (unsigned long)g_archive_rebuild_attempts,
                        g_archive_rebuild_pending ? 1u : 0u,
                        (unsigned long)g_archive_rebuild_orphan_index,
                        (unsigned long)g_archive_rebuild_file_count,
                        (unsigned long)g_archive_rebuild_heap,
                        (unsigned long)g_archive_holder_rebuild_attempts,
                        (unsigned long)g_archive_holder_rebuild_state,
                        (unsigned long)g_archive_holder_before,
                        (unsigned long)g_archive_holder_after,
                        (unsigned long)g_archive_holder_vector,
                        (unsigned long)g_archive_holder_capacity,
                        (unsigned long)g_archive_holder_count,
                        (unsigned long)g_archive_holder_lookup_fast_paths,
                        (unsigned long)g_archive_holder_lookup_hits,
                        (unsigned long)g_archive_holder_invalid_entries,
                        (unsigned long)g_archive_wait_redirects,
                        g_archive_wait_redirect_pending ? 1u : 0u,
                        (unsigned long)g_cache_range_skips,
                        (unsigned long)g_case_compare_fast_paths,
                        (unsigned long)g_audio_resource_skips,
                        (unsigned long)g_audio_resource_bad_pointer,
                        (unsigned long)g_audio_resource_bad_count,
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
                        (unsigned long)ios->last_unknown_ipc_command,
                        (unsigned long)ios->last_unknown_fd,
                        (unsigned long)ios->last_unknown_ioctl_request,
                        (unsigned long)ios->di_reads,
                        (unsigned long)ios->di_read_failures,
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
#endif
            if (cpu.timebase < frame_timebase_target)
                advance_timebase(&cpu, frame_timebase_target - cpu.timebase);
        }
#endif
        smg3ds_gx_present_bottom(g_host_touch.px, g_host_touch.py,
                                 g_host_touch_active);
        if (!smg3ds_gx_present_top()) {
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
    }

#ifdef SMG3DS_WITH_GENERATED
    smg3ds_disc_shutdown();
    if (debug_file != NULL)
        fclose(debug_file);
    mem2_swap_shutdown();
    if (R_SUCCEEDED(sdmc_mount))
        archiveUnmount("sdmc");
    if (romfs_ok)
        romfsExit();
#endif
    if (mem1_ok)
        cpu_free(&cpu);
    smg3ds_gx_shutdown();
    mem2_free_sparse();
    gfxExit();
    return 0;
}
