// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/gx_renderer.h"
#include "smg3ds/pica200_renderer.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    GX_FIFO_CAPACITY = 64 * 1024,
    GX_XF_COUNT = 0x1058,
    GX_MAX_VERTICES = 4096,
    GX_MAX_INDICES = 12288,
    /*
     * The ARM11 software rasterizer is the current presentation fallback.
     * Render at exactly half the top-screen dimensions and upscale on copy;
     * this removes 75% of its fragment work without changing guest timing.
     */
    GX_EFB_WIDTH = 200,
    GX_EFB_HEIGHT = 120,
    GX_CONTENT_X = 0,
    GX_CONTENT_Y = 0,
    GX_CONTENT_WIDTH = 200,
    GX_CONTENT_HEIGHT = 120,
    GX_TOP_SCREEN_WIDTH = 400,
    GX_TOP_SCREEN_HEIGHT = 240,
    GX_BOTTOM_SCREEN_WIDTH = 320,
    GX_BOTTOM_SCREEN_HEIGHT = 240,
    GX_BOTTOM_VIEW_Y = 24,
    GX_BOTTOM_VIEW_HEIGHT = 192,
    GX_LOGICAL_EFB_WIDTH = 640,
    GX_LOGICAL_EFB_HEIGHT = 480,
    GX_MAX_TEXTURE_DIMENSION = 1024,
    GX_TEXTURE_MAP_COUNT = 8,
    GX_TEV_STAGE_COUNT = 16,
    GX_TEXTURE_VIEWPORT_WIDTH = 320,
    GX_TEXTURE_VIEWPORT_HEIGHT = 240,
    GX_TMEM_SIZE = 1024 * 1024,
    GX_ATTR_SCRATCH = 64,
    GX_DL_MAX_DEPTH = 16,
    GX_GUEST_PAGE_SIZE = 4096,
    GX_POINTER_CACHE_SLOTS = 3,
    GX_EFB_CAPTURE_KEY_COUNT = 4
};

typedef struct GxVertex {
    float position[3];
    float screen[2];
    float depth;
    float inv_w;
    float raw_texcoord[GX_TEXTURE_MAP_COUNT][2];
    float texcoord[GX_TEXTURE_MAP_COUNT][3];
    uint32_t color;
    uint32_t color1;
    uint8_t position_matrix;
    uint8_t texture_matrix[GX_TEXTURE_MAP_COUNT];
    bool raw_texcoord_valid[GX_TEXTURE_MAP_COUNT];
    bool texture_matrix_valid[GX_TEXTURE_MAP_COUNT];
    bool valid;
} GxVertex;

typedef struct GxTexture {
    uint32_t width;
    uint32_t height;
    uint32_t storage_width;
    uint32_t storage_height;
    uint32_t format;
    uint32_t wrap_s;
    uint32_t wrap_t;
    const uint32_t* pixels;
    bool linear_filter;
    bool valid;
} GxTexture;

typedef struct GxTextureCache {
    uint32_t* pixels;
    size_t capacity;
    uint32_t image0;
    uint32_t image3;
    uint32_t tlut;
    uint32_t signature;
    bool valid;
    bool needs_validation;
    bool from_efb_capture;
} GxTextureCache;

typedef struct GxEfbCaptureKey {
    uint32_t address;
    bool valid;
} GxEfbCaptureKey;

typedef struct GxEfbCapture {
    uint32_t* pixels;
    uint32_t* pending_pixels;
    GxEfbCaptureKey keys[GX_EFB_CAPTURE_KEY_COUNT];
    uint32_t generation;
    uint32_t pending_address;
    uint16_t source_x;
    uint16_t source_y;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t pending_source_x;
    uint16_t pending_source_y;
    uint16_t pending_source_width;
    uint16_t pending_source_height;
    uint8_t next_key;
    bool valid;
    bool pending;
} GxEfbCapture;

typedef struct GxDrawTextures {
    GxTexture maps[GX_TEXTURE_MAP_COUNT];
    uint8_t stage_texgen[GX_TEV_STAGE_COUNT];
    uint8_t stage_map[GX_TEV_STAGE_COUNT];
    bool stage_textured[GX_TEV_STAGE_COUNT];
    uint8_t stage_count;
} GxDrawTextures;

/*
 * GX transform state is constant for the duration of one primitive command.
 * Keep its float conversion out of the per-vertex path; only the position
 * matrix can vary through a vertex matrix-index attribute.
 */
typedef struct GxPositionTransform {
    float matrix[12];
    float projection[6];
    float viewport_width;
    float viewport_height;
    float viewport_x;
    float viewport_y;
    float depth_range;
    float far_z;
    uint32_t matrix_index;
    uint32_t projection_type;
    bool matrix_valid;
} GxPositionTransform;

typedef struct GxPointerCache {
    uint8_t* framebuffer;
    uint16_t touch_x;
    uint16_t touch_y;
    bool touch_active;
    bool valid;
} GxPointerCache;

static CPUState* g_cpu;
static uint8_t g_fifo[GX_FIFO_CAPACITY];
static size_t g_fifo_size;
static uint32_t g_cp[256];
static uint32_t g_xf[GX_XF_COUNT];
static bool g_xf_written[GX_XF_COUNT];
static uint32_t g_bp[256];
static int32_t g_tev_color[4][4];
static uint32_t g_bp_mask;
static uint8_t g_clear_red;
static uint8_t g_clear_green;
static uint8_t g_clear_blue;
static uint8_t g_clear_alpha;
static uint8_t g_tev_kcolor[4][4];
static Smg3dsGxStats g_stats;
static bool g_finish_requested;
static uint8_t g_dl_depth;
static uint8_t g_attr_scratch[GX_ATTR_SCRATCH];
static GxVertex g_vertices[GX_MAX_VERTICES];
static uint32_t g_indices[GX_MAX_INDICES];
static uint32_t* g_efb;
static uint32_t* g_xfb;
static uint32_t* g_zfb;
static uint8_t g_tmem[GX_TMEM_SIZE];
static GxTextureCache g_texture_cache[GX_TEXTURE_MAP_COUNT];
static uint32_t g_texture_read_page = UINT32_MAX;
static const uint8_t* g_texture_read_pointer;
static uint8_t* g_display_list_buffers[GX_DL_MAX_DEPTH];
static size_t g_display_list_capacities[GX_DL_MAX_DEPTH];
static bool g_xfb_valid;
static bool g_efb_dirty;
static uint8_t g_position_matrix_index;
static GxPointerCache g_pointer_cache[GX_POINTER_CACHE_SLOTS];
static uint32_t g_pointer_cache_next;
static GxEfbCapture g_efb_capture;

static bool color_is_visible(uint32_t rgba)
{
    return (rgba & 0xffffff00u) != 0u;
}

static uint32_t count_visible_pixels(const uint32_t* pixels)
{
    uint32_t colored = 0u;
    size_t i;

    if (pixels == NULL)
        return 0u;
    for (i = 0u; i < (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT; ++i) {
        if (color_is_visible(pixels[i]))
            ++colored;
    }
    return colored;
}

static void preserve_visible_efb(void)
{
    uint32_t colored;

    /* This is only a bootstrap fallback until the guest performs a real copy. */
    if (g_xfb_valid || g_efb == NULL || g_xfb == NULL)
        return;
    colored = count_visible_pixels(g_efb);
    if (colored == 0u)
        return;
    memcpy(g_xfb, g_efb,
           (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT * sizeof(uint32_t));
    g_stats.xfb_colored_pixels = colored;
    g_xfb_valid = true;
}

static uint16_t gx_read_be16(const uint8_t* data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t gx_read_be32(const uint8_t* data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint32_t bits(uint32_t value, unsigned offset, unsigned count)
{
    return (value >> offset) & ((1u << count) - 1u);
}

static float bits_to_float(uint32_t raw)
{
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static uint32_t component_size(uint32_t format)
{
    return format < 2u ? 1u : format < 4u ? 2u : 4u;
}

static uint32_t attribute_size(uint32_t descriptor, uint32_t direct_size)
{
    if (descriptor == 1u)
        return direct_size;
    if (descriptor == 2u)
        return 1u;
    if (descriptor == 3u)
        return 2u;
    return 0u;
}

static uint32_t color_size(uint32_t format)
{
    static const uint8_t sizes[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return sizes[format & 7u];
}

static void texture_format(uint32_t g0, uint32_t g1, uint32_t g2, unsigned index,
                           uint32_t* format, uint32_t* elements)
{
    switch (index) {
    case 0: *format = bits(g0, 22, 3); *elements = bits(g0, 21, 1); break;
    case 1: *format = bits(g1, 1, 3);  *elements = bits(g1, 0, 1);  break;
    case 2: *format = bits(g1, 10, 3); *elements = bits(g1, 9, 1);  break;
    case 3: *format = bits(g1, 19, 3); *elements = bits(g1, 18, 1); break;
    case 4: *format = bits(g1, 28, 3); *elements = bits(g1, 27, 1); break;
    case 5: *format = bits(g2, 6, 3);  *elements = bits(g2, 5, 1);  break;
    case 6: *format = bits(g2, 15, 3); *elements = bits(g2, 14, 1); break;
    default: *format = bits(g2, 24, 3); *elements = bits(g2, 23, 1); break;
    }
}

static uint32_t texture_fraction(uint32_t g0, uint32_t g1, uint32_t g2,
                                 unsigned index)
{
    switch (index) {
    case 0: return bits(g0, 25u, 5u);
    case 1: return bits(g1, 4u, 5u);
    case 2: return bits(g1, 13u, 5u);
    case 3: return bits(g1, 22u, 5u);
    case 4: return bits(g2, 0u, 5u);
    case 5: return bits(g2, 9u, 5u);
    case 6: return bits(g2, 18u, 5u);
    default: return bits(g2, 27u, 5u);
    }
}

static uint32_t vertex_size(uint8_t vat)
{
    const uint32_t vcd_low = g_cp[0x50];
    const uint32_t vcd_high = g_cp[0x60];
    const uint32_t g0 = g_cp[0x70u + (vat & 7u)];
    const uint32_t g1 = g_cp[0x80u + (vat & 7u)];
    const uint32_t g2 = g_cp[0x90u + (vat & 7u)];
    uint32_t size = 0;
    unsigned i;
    for (i = 0; i < 9; ++i)
        size += (vcd_low >> i) & 1u;
    size += attribute_size(bits(vcd_low, 9, 2),
                           component_size(bits(g0, 1, 3)) * (bits(g0, 0, 1) + 2u));
    {
        const uint32_t descriptor = bits(vcd_low, 11, 2);
        const bool ntb = bits(g0, 9, 1) != 0u;
        const bool index3 = bits(g0, 31, 1) != 0u;
        if (descriptor == 1u)
            size += component_size(bits(g0, 10, 3)) * (ntb ? 9u : 3u);
        else if (descriptor == 2u || descriptor == 3u)
            size += (descriptor - 1u) * (ntb && index3 ? 3u : 1u);
    }
    for (i = 0; i < 2; ++i)
        size += attribute_size(bits(vcd_low, 13u + i * 2u, 2),
                               color_size(bits(g0, 14u + i * 4u, 3)));
    for (i = 0; i < 8; ++i) {
        uint32_t format;
        uint32_t elements;
        texture_format(g0, g1, g2, i, &format, &elements);
        size += attribute_size(bits(vcd_high, i * 2u, 2),
                               component_size(format) * (elements + 1u));
    }
    return size;
}

typedef struct ByteReader {
    const uint8_t* data;
    size_t size;
    size_t offset;
} ByteReader;

static bool reader_u8(ByteReader* reader, uint8_t* value)
{
    if (reader->offset >= reader->size)
        return false;
    *value = reader->data[reader->offset++];
    return true;
}

static bool reader_index(ByteReader* reader, uint32_t descriptor, uint16_t* index)
{
    uint8_t first;
    uint8_t second;
    if (!reader_u8(reader, &first))
        return false;
    if (descriptor == 2u) {
        *index = first;
        return true;
    }
    if (!reader_u8(reader, &second))
        return false;
    *index = (uint16_t)(((uint16_t)first << 8) | second);
    return true;
}

static const uint8_t* reader_take(ByteReader* reader, size_t size)
{
    const uint8_t* result;
    if (size > reader->size - reader->offset)
        return NULL;
    result = reader->data + reader->offset;
    reader->offset += size;
    return result;
}

static bool guest_copy_bytes(uint32_t address, void* destination, size_t size)
{
    uint8_t* output = (uint8_t*)destination;
    size_t copied = 0u;

    if (g_cpu == NULL || destination == NULL)
        return false;
    while (copied < size) {
        const uint32_t current = address + (uint32_t)copied;
        const size_t page_remaining =
            GX_GUEST_PAGE_SIZE - (current & (GX_GUEST_PAGE_SIZE - 1u));
        const size_t chunk = size - copied < page_remaining ?
                             size - copied : page_remaining;
        const uint8_t* source = ppc_memory_pointer(
            g_cpu, current, (uint32_t)chunk);
        if (source != NULL) {
            memcpy(output + copied, source, chunk);
        } else {
            size_t i;
            for (i = 0u; i < chunk; ++i)
                output[copied + i] = mem_read8(g_cpu, current + (uint32_t)i);
        }
        copied += chunk;
    }
    return true;
}

static const uint8_t* guest_bytes(uint32_t address, uint32_t size)
{
    if (g_cpu == NULL || size == 0u || size > sizeof(g_attr_scratch))
        return NULL;
    return guest_copy_bytes(address, g_attr_scratch, size) ?
           g_attr_scratch : NULL;
}

static const uint8_t* attribute_data(ByteReader* reader, unsigned array,
                                     uint32_t descriptor, size_t direct_size)
{
    uint16_t index;
    uint64_t address;
    if (descriptor == 1u)
        return reader_take(reader, direct_size);
    if (descriptor != 2u && descriptor != 3u)
        return NULL;
    if (!reader_index(reader, descriptor, &index))
        return NULL;
    address = (uint64_t)g_cp[0xA0u + array] +
              (uint64_t)(g_cp[0xB0u + array] & 0xffu) * index;
    if (address > 0xffffffffu)
        return NULL;
    return guest_bytes((uint32_t)address, (uint32_t)direct_size);
}

static float read_component(const uint8_t* data, uint32_t format, uint32_t fraction)
{
    const float scale = ldexpf(1.0f, -(int)fraction);
    switch (format) {
    case 0: return (float)data[0] * scale;
    case 1: return (float)(int8_t)data[0] * scale;
    case 2: return (float)(((uint16_t)data[0] << 8) | data[1]) * scale;
    case 3: return (float)(int16_t)(((uint16_t)data[0] << 8) | data[1]) * scale;
    default:
        return bits_to_float(gx_read_be32(data));
    }
}

static uint32_t expand_channel(uint32_t value, unsigned channel_bits)
{
    const uint32_t maximum = (1u << channel_bits) - 1u;
    return (value * 255u + maximum / 2u) / maximum;
}

static uint32_t decode_color(const uint8_t* data, uint32_t format)
{
    uint32_t r = 255;
    uint32_t g = 255;
    uint32_t b = 255;
    uint32_t a = 255;
    if (format == 0u) {
        const uint16_t value = gx_read_be16(data);
        r = expand_channel(value >> 11, 5);
        g = expand_channel((value >> 5) & 63u, 6);
        b = expand_channel(value & 31u, 5);
    } else if (format == 1u || format == 2u) {
        r = data[0];
        g = data[1];
        b = data[2];
    } else if (format == 3u) {
        const uint16_t value = gx_read_be16(data);
        r = expand_channel(value >> 12, 4);
        g = expand_channel((value >> 8) & 15u, 4);
        b = expand_channel((value >> 4) & 15u, 4);
        a = expand_channel(value & 15u, 4);
    } else if (format == 4u) {
        const uint32_t value = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
        r = expand_channel(value >> 18, 6);
        g = expand_channel((value >> 12) & 63u, 6);
        b = expand_channel((value >> 6) & 63u, 6);
        a = expand_channel(value & 63u, 6);
    } else if (format == 5u) {
        r = data[0];
        g = data[1];
        b = data[2];
        a = data[3];
    }
    return (r << 24) | (g << 16) | (b << 8) | a;
}

static uint32_t pack_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (r << 24) | (g << 16) | (b << 8) | a;
}

static uint8_t texture_read8(uint32_t address)
{
    const uint32_t page = address & ~(uint32_t)(GX_GUEST_PAGE_SIZE - 1u);

    if (g_cpu == NULL)
        return 0u;
    if (page != g_texture_read_page) {
        g_texture_read_page = page;
        g_texture_read_pointer = ppc_memory_pointer(
            g_cpu, page, GX_GUEST_PAGE_SIZE);
    }
    if (g_texture_read_pointer != NULL)
        return g_texture_read_pointer[address - page];
    return mem_read8(g_cpu, address);
}

static uint16_t texture_read16(uint32_t address)
{
    return (uint16_t)(((uint16_t)texture_read8(address) << 8) |
                      texture_read8(address + 1u));
}

static size_t texture_storage_size(uint32_t width, uint32_t height,
                                   uint32_t format)
{
    uint32_t block_width;
    uint32_t block_height;
    uint32_t block_bytes = 32u;

    switch (format) {
    case 0u:
    case 8u:
    case 14u:
        block_width = 8u;
        block_height = 8u;
        break;
    case 1u:
    case 2u:
    case 9u:
        block_width = 8u;
        block_height = 4u;
        break;
    case 6u:
        block_width = 4u;
        block_height = 4u;
        block_bytes = 64u;
        break;
    default:
        block_width = 4u;
        block_height = 4u;
        break;
    }
    return (size_t)((width + block_width - 1u) / block_width) *
           ((height + block_height - 1u) / block_height) * block_bytes;
}

static uint32_t texture_source_signature(uint32_t base, uint32_t width,
                                         uint32_t height, uint32_t format)
{
    const size_t size = texture_storage_size(width, height, format);
    const size_t step = size > 512u ? (size + 511u) / 512u : 1u;
    uint32_t hash = 2166136261u;
    size_t offset;

    g_texture_read_page = UINT32_MAX;
    g_texture_read_pointer = NULL;
    for (offset = 0u; offset < size; offset += step) {
        hash ^= texture_read8(base + (uint32_t)offset);
        hash *= 16777619u;
    }
    if (size != 0u && (size - 1u) % step != 0u) {
        hash ^= texture_read8(base + (uint32_t)(size - 1u));
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t decode_rgb565(uint16_t value)
{
    return pack_rgba(expand_channel(value >> 11, 5),
                     expand_channel((value >> 5) & 63u, 6),
                     expand_channel(value & 31u, 5), 255u);
}
static uint32_t decode_rgb5a3(uint16_t value)
{
    uint32_t r;
    uint32_t g;
    uint32_t b;
    uint32_t alpha;

    if ((value & 0x8000u) != 0u) {
        r = expand_channel((value >> 10u) & 31u, 5u);
        g = expand_channel((value >> 5u) & 31u, 5u);
        b = expand_channel(value & 31u, 5u);
        alpha = 255u;
    } else {
        alpha = expand_channel((value >> 12u) & 7u, 3u);
        r = expand_channel((value >> 8u) & 15u, 4u);
        g = expand_channel((value >> 4u) & 15u, 4u);
        b = expand_channel(value & 15u, 4u);
    }
    return pack_rgba(r, g, b, alpha);
}

static uint32_t decode_tlut_entry(uint32_t base, uint32_t format,
                                  uint32_t index)
{
    const size_t offset = (size_t)base + (size_t)index * 2u;
    uint16_t value;
    if (offset + 1u >= GX_TMEM_SIZE)
        return 0xffff00ffu;

    if (format == 0u) {
        const uint32_t alpha = g_tmem[offset];
        const uint32_t intensity = g_tmem[offset + 1u];
        return pack_rgba(intensity, intensity, intensity, alpha);
    }
    value = (uint16_t)(((uint16_t)g_tmem[offset] << 8) |
                       g_tmem[offset + 1u]);
    if (format == 1u)
        return decode_rgb565(value);
    if (format == 2u)
        return decode_rgb5a3(value);
    return 0xffff00ffu;
}

static uint32_t mix_rgba(uint32_t first, uint32_t second,
                         uint32_t first_weight, uint32_t second_weight,
                         uint32_t divisor)
{
    uint32_t result = 0u;
    unsigned shift;
    for (shift = 0u; shift <= 24u; shift += 8u) {
        const uint32_t a = (first >> shift) & 0xffu;
        const uint32_t b = (second >> shift) & 0xffu;
        const uint32_t value =
            (a * first_weight + b * second_weight + divisor / 2u) / divisor;
        result |= value << shift;
    }
    return result;
}

static uint32_t decode_cmpr_texel(uint32_t base, uint32_t width,
                                  uint32_t x, uint32_t y)
{
    const uint32_t blocks_x = (width + 7u) / 8u;
    const uint32_t block = (y / 8u) * blocks_x + x / 8u;
    const uint32_t subblock = ((y & 4u) != 0u ? 2u : 0u) +
                              ((x & 4u) != 0u ? 1u : 0u);
    const uint32_t address = base + block * 32u + subblock * 8u;
    const uint16_t endpoint0 = texture_read16(address);
    const uint16_t endpoint1 = texture_read16(address + 2u);
    const uint32_t color0 = decode_rgb565(endpoint0);
    const uint32_t color1 = decode_rgb565(endpoint1);
    const uint8_t selectors = texture_read8(address + 4u + (y & 3u));
    const uint32_t selector = (selectors >> (6u - (x & 3u) * 2u)) & 3u;

    if (selector == 0u)
        return color0;
    if (selector == 1u)
        return color1;
    if (endpoint0 > endpoint1)
        return selector == 2u ? mix_rgba(color0, color1, 2u, 1u, 3u) :
                                mix_rgba(color0, color1, 1u, 2u, 3u);
    if (selector == 2u)
        return mix_rgba(color0, color1, 1u, 1u, 2u);
    return 0u;
}

static uint32_t decode_texture_texel(uint32_t base, uint32_t width,
                                     uint32_t format, uint32_t tlut_base,
                                     uint32_t tlut_format, uint32_t x, uint32_t y)
{
    uint32_t blocks_x;
    uint32_t block;
    uint32_t offset;
    uint32_t address;
    uint32_t intensity;
    uint32_t alpha = 255u;

    switch (format) {
    case 0u:
        blocks_x = (width + 7u) / 8u;
        block = (y / 8u) * blocks_x + x / 8u;
        address = base + block * 32u + (y & 7u) * 4u + (x & 7u) / 2u;
        intensity = texture_read8(address);
        intensity = (x & 1u) == 0u ? intensity >> 4u : intensity & 15u;
        intensity = expand_channel(intensity, 4u);
        return pack_rgba(intensity, intensity, intensity, intensity);
    case 1u:
        blocks_x = (width + 7u) / 8u;
        block = (y / 4u) * blocks_x + x / 8u;
        address = base + block * 32u + (y & 3u) * 8u + (x & 7u);
        intensity = texture_read8(address);
        return pack_rgba(intensity, intensity, intensity, intensity);
    case 2u:
        blocks_x = (width + 7u) / 8u;
        block = (y / 4u) * blocks_x + x / 8u;
        address = base + block * 32u + (y & 3u) * 8u + (x & 7u);
        intensity = texture_read8(address);
        alpha = expand_channel(intensity >> 4u, 4u);
        intensity = expand_channel(intensity & 15u, 4u);
        return pack_rgba(intensity, intensity, intensity, alpha);
    case 3u:
        blocks_x = (width + 3u) / 4u;
        block = (y / 4u) * blocks_x + x / 4u;
        offset = ((y & 3u) * 4u + (x & 3u)) * 2u;
        address = base + block * 32u + offset;
        alpha = texture_read8(address);
        intensity = texture_read8(address + 1u);
        return pack_rgba(intensity, intensity, intensity, alpha);
    case 4u:
        blocks_x = (width + 3u) / 4u;
        block = (y / 4u) * blocks_x + x / 4u;
        offset = ((y & 3u) * 4u + (x & 3u)) * 2u;
        return decode_rgb565(texture_read16(base + block * 32u + offset));
    case 5u:
        blocks_x = (width + 3u) / 4u;
        block = (y / 4u) * blocks_x + x / 4u;
        offset = ((y & 3u) * 4u + (x & 3u)) * 2u;
        return decode_rgb5a3(texture_read16(base + block * 32u + offset));
    case 6u:
        blocks_x = (width + 3u) / 4u;
        block = (y / 4u) * blocks_x + x / 4u;
        offset = ((y & 3u) * 4u + (x & 3u)) * 2u;
        address = base + block * 64u;
        alpha = texture_read8(address + offset);
        return pack_rgba(texture_read8(address + offset + 1u),
                         texture_read8(address + 32u + offset),
                         texture_read8(address + 33u + offset), alpha);
    case 8u:
        blocks_x = (width + 7u) / 8u;
        block = (y / 8u) * blocks_x + x / 8u;
        address = base + block * 32u + (y & 7u) * 4u + (x & 7u) / 2u;
        intensity = texture_read8(address);
        intensity = (x & 1u) == 0u ? intensity >> 4u : intensity & 15u;
        return decode_tlut_entry(tlut_base, tlut_format, intensity);
    case 9u:
        blocks_x = (width + 7u) / 8u;
        block = (y / 4u) * blocks_x + x / 8u;
        address = base + block * 32u + (y & 3u) * 8u + (x & 7u);
        return decode_tlut_entry(tlut_base, tlut_format,
                                 texture_read8(address));
    case 10u:
        blocks_x = (width + 3u) / 4u;
        block = (y / 4u) * blocks_x + x / 4u;
        offset = ((y & 3u) * 4u + (x & 3u)) * 2u;
        return decode_tlut_entry(
            tlut_base, tlut_format,
            texture_read16(base + block * 32u + offset) & 0x3fffu);
    case 14u:
        return decode_cmpr_texel(base, width, x, y);
    default:
        return 0xffff00ffu;
    }
}

static void texture_viewport_size(uint32_t width, uint32_t height,
                                  uint32_t* storage_width,
                                  uint32_t* storage_height)
{
    uint32_t scaled_width = width;
    uint32_t scaled_height = height;

    if (scaled_width > GX_TEXTURE_VIEWPORT_WIDTH) {
        scaled_height = (uint32_t)(((uint64_t)scaled_height *
                                    GX_TEXTURE_VIEWPORT_WIDTH +
                                    scaled_width / 2u) / scaled_width);
        scaled_width = GX_TEXTURE_VIEWPORT_WIDTH;
    }
    if (scaled_height > GX_TEXTURE_VIEWPORT_HEIGHT) {
        scaled_width = (uint32_t)(((uint64_t)scaled_width *
                                   GX_TEXTURE_VIEWPORT_HEIGHT +
                                   scaled_height / 2u) / scaled_height);
        scaled_height = GX_TEXTURE_VIEWPORT_HEIGHT;
    }
    if (scaled_width == 0u)
        scaled_width = 1u;
    if (scaled_height == 0u)
        scaled_height = 1u;
    *storage_width = scaled_width;
    *storage_height = scaled_height;
}

static const GxEfbCapture* find_efb_capture(uint32_t texture_base)
{
    unsigned key;

    if (!g_efb_capture.valid || g_efb_capture.pixels == NULL)
        return NULL;
    for (key = 0u; key < GX_EFB_CAPTURE_KEY_COUNT; ++key) {
        if (g_efb_capture.keys[key].valid &&
            g_efb_capture.keys[key].address == texture_base)
            return &g_efb_capture;
    }
    return NULL;
}

static bool prepare_efb_capture_texture(const GxEfbCapture* capture,
                                        GxTextureCache* cache,
                                        GxTexture* texture,
                                        uint32_t image0, uint32_t image3,
                                        uint32_t tlut,
                                        uint32_t storage_width,
                                        uint32_t storage_height)
{
    const size_t pixel_count = (size_t)storage_width * storage_height;
    uint32_t* resized;
    uint32_t y;

    if (cache->valid && cache->from_efb_capture &&
        cache->pixels != NULL && image0 == cache->image0 &&
        image3 == cache->image3 && tlut == cache->tlut &&
        capture->generation == cache->signature) {
        cache->needs_validation = false;
        texture->pixels = cache->pixels;
        texture->valid = true;
        return true;
    }
    if (pixel_count > cache->capacity) {
        resized = (uint32_t*)realloc(cache->pixels,
                                     pixel_count * sizeof(uint32_t));
        if (resized == NULL)
            return false;
        cache->pixels = resized;
        cache->capacity = pixel_count;
    }

    /*
     * The guest describes the copy in its 640x480 EFB coordinate space,
     * while this renderer deliberately keeps a 200x120 EFB. Resample the
     * captured source rectangle directly into the normal texture cache.
     */
    for (y = 0u; y < storage_height; ++y) {
        const uint32_t logical_y = capture->source_y +
            (uint32_t)((((uint64_t)y * 2u + 1u) *
                        capture->source_height) /
                       ((uint64_t)storage_height * 2u));
        uint32_t source_y = (uint32_t)(((uint64_t)logical_y *
                                        GX_EFB_HEIGHT) /
                                       GX_LOGICAL_EFB_HEIGHT);
        uint32_t x;

        if (source_y >= GX_EFB_HEIGHT)
            source_y = GX_EFB_HEIGHT - 1u;
        for (x = 0u; x < storage_width; ++x) {
            const uint32_t logical_x = capture->source_x +
                (uint32_t)((((uint64_t)x * 2u + 1u) *
                            capture->source_width) /
                           ((uint64_t)storage_width * 2u));
            uint32_t source_x = (uint32_t)(((uint64_t)logical_x *
                                            GX_EFB_WIDTH) /
                                           GX_LOGICAL_EFB_WIDTH);

            if (source_x >= GX_EFB_WIDTH)
                source_x = GX_EFB_WIDTH - 1u;
            cache->pixels[(size_t)y * storage_width + x] =
                capture->pixels[(size_t)source_y * GX_EFB_WIDTH + source_x];
        }
    }

    cache->image0 = image0;
    cache->image3 = image3;
    cache->tlut = tlut;
    cache->signature = capture->generation;
    cache->valid = true;
    cache->needs_validation = false;
    cache->from_efb_capture = true;
    texture->pixels = cache->pixels;
    texture->valid = true;
    ++g_stats.texture_decodes;
    return true;
}

static uint32_t average_four_rgba(uint32_t a, uint32_t b,
                                  uint32_t c, uint32_t d)
{
    uint32_t result = 0u;
    unsigned shift;

    for (shift = 0u; shift <= 24u; shift += 8u) {
        const uint32_t value = ((a >> shift) & 0xffu) +
                               ((b >> shift) & 0xffu) +
                               ((c >> shift) & 0xffu) +
                               ((d >> shift) & 0xffu);
        result |= ((value + 2u) >> 2u) << shift;
    }
    return result;
}

static uint32_t decode_scaled_texture_texel(
    uint32_t base, uint32_t width, uint32_t height, uint32_t format,
    uint32_t tlut_base, uint32_t tlut_format,
    uint32_t storage_width, uint32_t storage_height,
    uint32_t x, uint32_t y)
{
    uint32_t sx0;
    uint32_t sx1;
    uint32_t sy0;
    uint32_t sy1;

    if (storage_width == width && storage_height == height) {
        return decode_texture_texel(base, width, format, tlut_base,
                                    tlut_format, x, y);
    }

    /*
     * Four stratified samples give downscaled textures a stable box-filtered
     * look without decoding the full Wii-resolution image first.
     */
    sx0 = (uint32_t)(((uint64_t)(x * 4u + 1u) * width) /
                     ((uint64_t)storage_width * 4u));
    sx1 = (uint32_t)(((uint64_t)(x * 4u + 3u) * width) /
                     ((uint64_t)storage_width * 4u));
    sy0 = (uint32_t)(((uint64_t)(y * 4u + 1u) * height) /
                     ((uint64_t)storage_height * 4u));
    sy1 = (uint32_t)(((uint64_t)(y * 4u + 3u) * height) /
                     ((uint64_t)storage_height * 4u));
    if (sx0 >= width)
        sx0 = width - 1u;
    if (sx1 >= width)
        sx1 = width - 1u;
    if (sy0 >= height)
        sy0 = height - 1u;
    if (sy1 >= height)
        sy1 = height - 1u;
    return average_four_rgba(
        decode_texture_texel(base, width, format, tlut_base, tlut_format,
                             sx0, sy0),
        decode_texture_texel(base, width, format, tlut_base, tlut_format,
                             sx1, sy0),
        decode_texture_texel(base, width, format, tlut_base, tlut_format,
                             sx0, sy1),
        decode_texture_texel(base, width, format, tlut_base, tlut_format,
                             sx1, sy1));
}

static bool prepare_texture(uint32_t map, GxTexture* texture)
{
    const uint32_t map_index = map & 7u;
    const uint32_t slot = map_index & 3u;
    const bool high_bank = (map_index & 4u) != 0u;
    GxTextureCache* const cache = &g_texture_cache[map_index];
    const uint32_t mode0_register = (high_bank ? 0xa0u : 0x80u) + slot;
    const uint32_t image0_register = (high_bank ? 0xa8u : 0x88u) + slot;
    const uint32_t image3_register = (high_bank ? 0xb4u : 0x94u) + slot;
    const uint32_t tlut_register = (high_bank ? 0xb8u : 0x98u) + slot;
    const uint32_t image0 = g_bp[image0_register];
    const uint32_t image3 = g_bp[image3_register];
    const uint32_t mode0 = g_bp[mode0_register];
    const uint32_t tlut = g_bp[tlut_register];
    const uint32_t tlut_base = bits(tlut, 0u, 10u) << 9u;
    const uint32_t tlut_format = bits(tlut, 10u, 2u);
    const uint32_t width = bits(image0, 0u, 10u) + 1u;
    const uint32_t height = bits(image0, 10u, 10u) + 1u;
    const uint32_t format = bits(image0, 20u, 4u);
    const uint32_t texture_base = (image3 & 0x00ffffffu) << 5u;
    const GxEfbCapture* capture;
    uint32_t storage_width;
    uint32_t storage_height;
    size_t pixel_count;
    uint32_t* resized;
    uint32_t y;

    texture_viewport_size(width, height, &storage_width, &storage_height);
    pixel_count = (size_t)storage_width * storage_height;
    memset(texture, 0, sizeof(*texture));
    g_stats.last_texture_width = width;
    g_stats.last_texture_height = height;
    g_stats.last_texture_storage_width = storage_width;
    g_stats.last_texture_storage_height = storage_height;
    g_stats.last_texture_format = format;
    g_stats.last_texture_base = texture_base;

    if (g_cpu == NULL || width == 0u || height == 0u ||
        width > GX_MAX_TEXTURE_DIMENSION ||
        height > GX_MAX_TEXTURE_DIMENSION)
        return false;
    if ((format > 6u && (format < 8u || format > 10u) && format != 14u) ||
        ((format == 8u || format == 9u || format == 10u) &&
         tlut_format > 2u)) {
        ++g_stats.unsupported_texture_formats;
        return false;
    }
    texture->width = width;
    texture->height = height;
    texture->storage_width = storage_width;
    texture->storage_height = storage_height;
    texture->format = format;
    texture->wrap_s = bits(mode0, 0u, 2u);
    texture->wrap_t = bits(mode0, 2u, 2u);
    texture->linear_filter = bits(mode0, 4u, 1u) != 0u ||
                             bits(mode0, 7u, 1u) != 0u;
    capture = find_efb_capture(texture_base);
    if (capture != NULL) {
        return prepare_efb_capture_texture(
            capture, cache, texture, image0, image3, tlut,
            storage_width, storage_height);
    }
    texture->pixels = cache->pixels;
    if (cache->valid && !cache->from_efb_capture &&
        cache->pixels != NULL &&
        image0 == cache->image0 &&
        image3 == cache->image3 &&
        tlut == cache->tlut) {
        /*
         * GXInvalidateTexAll is the guest's notification that unchanged
         * descriptors may now refer to modified image data. Avoid hashing
         * the texture on every mesh draw when no invalidation occurred.
         */
        if (!cache->needs_validation ||
            texture_source_signature(texture_base, width, height, format) ==
            cache->signature) {
            cache->needs_validation = false;
            texture->valid = true;
            return true;
        }
        cache->valid = false;
    }

    if (pixel_count > cache->capacity) {
        resized = (uint32_t*)realloc(cache->pixels,
                                     pixel_count * sizeof(uint32_t));
        if (resized == NULL)
            return false;
        cache->pixels = resized;
        cache->capacity = pixel_count;
    }

    g_texture_read_page = UINT32_MAX;
    g_texture_read_pointer = NULL;
    for (y = 0u; y < storage_height; ++y) {
        uint32_t x;
        for (x = 0u; x < storage_width; ++x) {
            cache->pixels[(size_t)y * storage_width + x] =
                decode_scaled_texture_texel(
                    texture_base, width, height, format, tlut_base,
                    tlut_format, storage_width, storage_height, x, y);
        }
    }

    texture->pixels = cache->pixels;
    texture->valid = true;
    cache->image0 = image0;
    cache->image3 = image3;
    cache->tlut = tlut;
    cache->signature =
        texture_source_signature(texture_base, width, height, format);
    cache->valid = true;
    cache->needs_validation = false;
    cache->from_efb_capture = false;
    ++g_stats.texture_decodes;
    return true;
}

static int wrap_texture_coordinate(int coordinate, uint32_t size, uint32_t mode)
{
    int period;
    if (size == 0u)
        return 0;
    if (mode == 0u) {
        if (coordinate < 0)
            return 0;
        if ((uint32_t)coordinate >= size)
            return (int)size - 1;
        return coordinate;
    }
    if (mode == 1u) {
        coordinate %= (int)size;
        return coordinate < 0 ? coordinate + (int)size : coordinate;
    }

    period = (int)size * 2;
    coordinate %= period;
    if (coordinate < 0)
        coordinate += period;
    return coordinate < (int)size ? coordinate : period - 1 - coordinate;
}

static uint32_t sample_texture(const GxTexture* texture, float s, float t)
{
    const float scaled_s = s * (float)texture->storage_width;
    const float scaled_t = t * (float)texture->storage_height;
    int floor_s = (int)scaled_s;
    int floor_t = (int)scaled_t;
    if ((float)floor_s > scaled_s)
        --floor_s;
    if ((float)floor_t > scaled_t)
        --floor_t;
    if (!texture->linear_filter) {
        const int x = wrap_texture_coordinate(
            floor_s, texture->storage_width, texture->wrap_s);
        const int y = wrap_texture_coordinate(
            floor_t, texture->storage_height, texture->wrap_t);
        return texture->pixels[(size_t)y * texture->storage_width +
                               (size_t)x];
    } else {
        const float filter_s = scaled_s - 0.5f;
        const float filter_t = scaled_t - 0.5f;
        int x0 = (int)filter_s;
        int y0 = (int)filter_t;
        uint32_t fx;
        uint32_t fy;
        uint32_t upper;
        uint32_t lower;

        if ((float)x0 > filter_s)
            --x0;
        if ((float)y0 > filter_t)
            --y0;
        fx = (uint32_t)((filter_s - (float)x0) * 256.0f);
        fy = (uint32_t)((filter_t - (float)y0) * 256.0f);
        if (fx > 256u)
            fx = 256u;
        if (fy > 256u)
            fy = 256u;
        {
            const int wrapped_x0 = wrap_texture_coordinate(
                x0, texture->storage_width, texture->wrap_s);
            const int wrapped_x1 = wrap_texture_coordinate(
                x0 + 1, texture->storage_width, texture->wrap_s);
            const int wrapped_y0 = wrap_texture_coordinate(
                y0, texture->storage_height, texture->wrap_t);
            const int wrapped_y1 = wrap_texture_coordinate(
                y0 + 1, texture->storage_height, texture->wrap_t);
            upper = mix_rgba(
                texture->pixels[(size_t)wrapped_y0 *
                                texture->storage_width +
                                (size_t)wrapped_x0],
                texture->pixels[(size_t)wrapped_y0 *
                                texture->storage_width +
                                (size_t)wrapped_x1],
                256u - fx, fx, 256u);
            lower = mix_rgba(
                texture->pixels[(size_t)wrapped_y1 *
                                texture->storage_width +
                                (size_t)wrapped_x0],
                texture->pixels[(size_t)wrapped_y1 *
                                texture->storage_width +
                                (size_t)wrapped_x1],
                256u - fx, fx, 256u);
        }
        return mix_rgba(upper, lower, 256u - fy, fy, 256u);
    }
}

static uint32_t rgba_channel(uint32_t rgba, unsigned channel)
{
    return (rgba >> (24u - channel * 8u)) & 0xffu;
}
static uint32_t tev_register_channel(const int32_t registers[4][4],
                                     unsigned reg, unsigned channel)
{
    int32_t value = registers[reg][channel];
    if (value < 0)
        value = 0;
    if (value > 255)
        value = 255;
    return (uint32_t)value;
}

static uint32_t tev_konst_fraction(uint32_t selection)
{
    if (selection > 7u)
        return 0u;
    return ((8u - selection) * 255u + 4u) / 8u;
}

static uint32_t tev_konst_color_channel(unsigned stage, unsigned channel)
{
    const uint32_t ksel = g_bp[0xf6u + stage / 2u];
    const unsigned shift = (stage & 1u) != 0u ? 14u : 4u;
    const uint32_t selection = bits(ksel, shift, 5u);

    if (selection < 8u)
        return tev_konst_fraction(selection);
    if (selection >= 12u && selection <= 15u)
        return g_tev_kcolor[selection - 12u][channel];
    if (selection >= 16u) {
        const unsigned reg = (unsigned)(selection & 3u);
        const unsigned selected_channel = (unsigned)((selection - 16u) / 4u);
        return g_tev_kcolor[reg][selected_channel];
    }
    return 0u;
}

static uint32_t tev_konst_alpha(unsigned stage)
{
    const uint32_t ksel = g_bp[0xf6u + stage / 2u];
    const unsigned shift = (stage & 1u) != 0u ? 19u : 9u;
    const uint32_t selection = bits(ksel, shift, 5u);

    if (selection < 8u)
        return tev_konst_fraction(selection);
    if (selection >= 16u) {
        const unsigned reg = (unsigned)(selection & 3u);
        const unsigned channel = (unsigned)((selection - 16u) / 4u);
        return g_tev_kcolor[reg][channel];
    }
    return 0u;
}

static uint32_t tev_color_argument(uint32_t argument, unsigned stage,
                                   unsigned channel,
                                   const int32_t registers[4][4],
                                   uint32_t texture, uint32_t raster)
{
    switch (argument & 15u) {
    case 0u: return tev_register_channel(registers, 0u, channel);
    case 1u: return tev_register_channel(registers, 0u, 3u);
    case 8u: return rgba_channel(texture, channel);
    case 2u: return tev_register_channel(registers, 1u, channel);
    case 3u: return tev_register_channel(registers, 1u, 3u);
    case 4u: return tev_register_channel(registers, 2u, channel);
    case 5u: return tev_register_channel(registers, 2u, 3u);
    case 6u: return tev_register_channel(registers, 3u, channel);
    case 7u: return tev_register_channel(registers, 3u, 3u);
    case 9u: return rgba_channel(texture, 3u);
    case 10u: return rgba_channel(raster, channel);
    case 11u: return rgba_channel(raster, 3u);
    case 12u: return 255u;
    case 13u: return 128u;
    case 14u: return tev_konst_color_channel(stage, channel);
    default: return 0u;
    }
}

static uint32_t tev_alpha_argument(uint32_t argument, unsigned stage,
                                   const int32_t registers[4][4],
                                   uint32_t texture, uint32_t raster)
{
    switch (argument & 7u) {
    case 0u: return tev_register_channel(registers, 0u, 3u);
    case 4u: return rgba_channel(texture, 3u);
    case 5u: return rgba_channel(raster, 3u);
    case 6u: return tev_konst_alpha(stage);
    case 1u: return tev_register_channel(registers, 1u, 3u);
    case 2u: return tev_register_channel(registers, 2u, 3u);
    case 3u: return tev_register_channel(registers, 3u, 3u);
    default: return 0u;
    }
}

static uint32_t tev_combine(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                            uint32_t bias, bool subtract, uint32_t scale)
{
    int value = (int)((a * (255u - c) + b * c + 127u) / 255u);
    value = subtract ? (int)d - value : (int)d + value;
    if (bias == 1u)
        value += 128;
    else if (bias == 2u)
        value -= 128;
    if (scale == 1u)
        value *= 2;
    else if (scale == 2u)
        value *= 4;
    else if (scale == 3u)
        value /= 2;
    if (value < 0)
        value = 0;
    if (value > 255)
        value = 255;
    return (uint32_t)value;
}

static uint32_t tev_raster_color(unsigned stage, uint32_t color0,
                                 uint32_t color1)
{
    const uint32_t order = g_bp[0x28u + stage / 2u];
    const unsigned shift = (stage & 1u) != 0u ? 12u : 0u;

    /*
     * BP stores the two vertex-color channels as 0/1. Values 5/6 are
     * indirect-texture alpha bump channels (not implemented yet), while 7
     * is the explicit zero channel. Treating every selection as COLOR0A0
     * made zero/bump UI stages inherit an unrelated pane alpha.
     */
    switch (bits(order, shift + 7u, 3u)) {
    case 0u: return color0;
    case 1u: return color1;
    default: return 0u;
    }
}

static uint32_t shade_fragment(uint32_t raster0, uint32_t raster1,
                               const uint32_t* textures)
{
    int32_t registers[4][4];
    unsigned stage_count = (unsigned)bits(g_bp[0x00u], 10u, 4u) + 1u;
    unsigned stage;
    unsigned channel;

    memcpy(registers, g_tev_color, sizeof(registers));
    if (stage_count > 16u)
        stage_count = 16u;
    if (stage_count == 1u &&
        (g_bp[0xc0u] == 0x08428fu ||
         g_bp[0xc0u] == 0x08248fu) &&
        (g_bp[0xc1u] == 0x08f0f0u ||
         g_bp[0xc1u] == 0x08f2f0u)) {
        const uint32_t order = g_bp[0x28u];
        const uint32_t texture =
            bits(order, 6u, 1u) != 0u && textures != NULL ?
            textures[0] : 0xffffffffu;
        const uint32_t raster = tev_raster_color(
            0u, raster0, raster1);
        const unsigned first =
            g_bp[0xc0u] == 0x08428fu ? 2u : 1u;
        const unsigned second = first == 2u ? 1u : 2u;
        const uint32_t alpha_factor =
            g_bp[0xc1u] == 0x08f0f0u ?
            tev_register_channel(registers, 1u, 3u) :
            rgba_channel(raster, 3u);
        uint32_t result = 0u;

        for (channel = 0u; channel < 3u; ++channel) {
            const uint32_t a = tev_register_channel(
                registers, first, channel);
            const uint32_t b = tev_register_channel(
                registers, second, channel);
            const uint32_t c = rgba_channel(texture, channel);
            const uint32_t value =
                (a * (255u - c) + b * c + 127u) / 255u;
            result |= value << (24u - channel * 8u);
        }
        result |= (rgba_channel(texture, 3u) * alpha_factor + 127u) /
                  255u;
        return result;
    }
    for (stage = 0u; stage < stage_count; ++stage) {
        const uint32_t color_env = g_bp[0xc0u + stage * 2u];
        const uint32_t alpha_env = g_bp[0xc1u + stage * 2u];
        const uint32_t order = g_bp[0x28u + stage / 2u];
        const unsigned order_shift = (stage & 1u) != 0u ? 12u : 0u;
        const bool texture_enabled =
            bits(order, order_shift + 6u, 1u) != 0u;
        const uint32_t stage_texture =
            texture_enabled && textures != NULL ?
            textures[stage] : 0xffffffffu;
        const uint32_t raster = tev_raster_color(
            stage, raster0, raster1);
        const unsigned color_dest = (unsigned)bits(color_env, 22u, 2u);
        const unsigned alpha_dest = (unsigned)bits(alpha_env, 22u, 2u);
        int32_t color_result[3];
        int32_t alpha_result;

        for (channel = 0u; channel < 3u; ++channel) {
            const uint32_t d = tev_color_argument(
                bits(color_env, 0u, 4u), stage, channel, registers,
                stage_texture, raster);
            const uint32_t c = tev_color_argument(
                bits(color_env, 4u, 4u), stage, channel, registers,
                stage_texture, raster);
            const uint32_t b = tev_color_argument(
                bits(color_env, 8u, 4u), stage, channel, registers,
                stage_texture, raster);
            const uint32_t a = tev_color_argument(
                bits(color_env, 12u, 4u), stage, channel, registers,
                stage_texture, raster);
            color_result[channel] = (int32_t)tev_combine(
                a, b, c, d, bits(color_env, 16u, 2u),
                bits(color_env, 18u, 1u) != 0u,
                bits(color_env, 20u, 2u));
        }
        {
            const uint32_t d = tev_alpha_argument(
                bits(alpha_env, 4u, 3u), stage, registers,
                stage_texture, raster);
            const uint32_t c = tev_alpha_argument(
                bits(alpha_env, 7u, 3u), stage, registers,
                stage_texture, raster);
            const uint32_t b = tev_alpha_argument(
                bits(alpha_env, 10u, 3u), stage, registers,
                stage_texture, raster);
            const uint32_t a = tev_alpha_argument(
                bits(alpha_env, 13u, 3u), stage, registers,
                stage_texture, raster);
            alpha_result = (int32_t)tev_combine(
                a, b, c, d, bits(alpha_env, 16u, 2u),
                bits(alpha_env, 18u, 1u) != 0u,
                bits(alpha_env, 20u, 2u));
        }
        for (channel = 0u; channel < 3u; ++channel)
            registers[color_dest][channel] = color_result[channel];
        registers[alpha_dest][3] = alpha_result;
    }

    return (tev_register_channel(registers, 0u, 0u) << 24u) |
           (tev_register_channel(registers, 0u, 1u) << 16u) |
           (tev_register_channel(registers, 0u, 2u) << 8u) |
           tev_register_channel(registers, 0u, 3u);
}

static uint32_t tev_stage_texgen(unsigned stage)
{
    const uint32_t order = g_bp[0x28u + stage / 2u];
    const unsigned shift = (stage & 1u) != 0u ? 12u : 0u;

    return bits(order, shift + 3u, 3u);
}

static uint32_t texgen_count(void)
{
    uint32_t count = g_xf[0x103fu] & 0x0fu;

    if (!g_xf_written[0x103fu])
        count = bits(g_bp[0x00u], 0u, 4u);
    if (count > 8u)
        count = 8u;
    return count;
}

static uint32_t raw_texture_index(uint32_t texgen)
{
    if (texgen < texgen_count() && g_xf_written[0x1040u + texgen]) {
        const uint32_t source_row =
            bits(g_xf[0x1040u + texgen], 7u, 5u);
        if (source_row >= 5u && source_row <= 12u)
            return source_row - 5u;
    }
    return texgen;
}

static bool tev_stage_uses_texture(unsigned stage)
{
    const uint32_t color_env = g_bp[0xc0u + stage * 2u];
    const uint32_t alpha_env = g_bp[0xc1u + stage * 2u];
    unsigned argument;

    for (argument = 0u; argument < 4u; ++argument) {
        const uint32_t color =
            bits(color_env, argument * 4u, 4u);
        const uint32_t alpha =
            bits(alpha_env, 4u + argument * 3u, 3u);
        if (color == 8u || color == 9u || alpha == 4u)
            return true;
    }
    return false;
}

static bool texture_binding_for_stage(unsigned stage, uint32_t vcd_high,
                                      uint32_t* texgen_out,
                                      uint32_t* map_out)
{
    const uint32_t order = g_bp[0x28u + stage / 2u];
    const unsigned shift = (stage & 1u) != 0u ? 12u : 0u;
    const uint32_t texgen = tev_stage_texgen(stage);
    const uint32_t raw = raw_texture_index(texgen);
    bool coordinate_available =
        raw < GX_TEXTURE_MAP_COUNT &&
        bits(vcd_high, raw * 2u, 2u) != 0u;

    if (texgen < texgen_count() &&
        g_xf_written[0x1040u + texgen]) {
        const uint32_t info = g_xf[0x1040u + texgen];
        const uint32_t type = bits(info, 4u, 3u);
        const uint32_t source_row = bits(info, 7u, 5u);

        coordinate_available = type == 2u || type == 3u ||
                               (type == 0u && source_row == 0u) ||
                               coordinate_available;
    }

    if (bits(order, shift + 6u, 1u) == 0u ||
        !tev_stage_uses_texture(stage) || !coordinate_available)
        return false;
    *texgen_out = texgen;
    *map_out = bits(order, shift, 3u);
    return true;
}

static bool load_xf_matrix_rows(uint32_t base, uint32_t row,
                                uint32_t row_mask, unsigned row_count,
                                float rows[3][4])
{
    unsigned i;
    unsigned j;

    for (i = 0u; i < row_count; ++i) {
        const uint32_t address =
            base + (((row + i) & row_mask) * 4u);
        if (address + 3u >= GX_XF_COUNT)
            return false;
        for (j = 0u; j < 4u; ++j) {
            if (!g_xf_written[address + j])
                return false;
            rows[i][j] = bits_to_float(g_xf[address + j]);
        }
    }
    return true;
}

static void identity_texture_rows(float rows[3][4])
{
    memset(rows, 0, sizeof(float) * 12u);
    rows[0][0] = 1.0f;
    rows[1][1] = 1.0f;
    rows[2][2] = 1.0f;
}

static void transform_texture_coordinate(GxVertex* vertex, uint32_t texgen)
{
    const uint32_t count = texgen_count();
    uint32_t info = 0u;
    uint32_t type = 0u;
    uint32_t source_row = 5u + texgen;
    uint32_t matrix_row = 60u;
    unsigned matrix_rows = 2u;
    float coord[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float rows[3][4];
    float result[3];
    unsigned i;

    if (texgen >= GX_TEXTURE_MAP_COUNT)
        return;
    vertex->texcoord[texgen][0] = vertex->raw_texcoord[texgen][0];
    vertex->texcoord[texgen][1] = vertex->raw_texcoord[texgen][1];
    vertex->texcoord[texgen][2] = 1.0f;
    if (texgen >= count)
        return;

    if (g_xf_written[0x1040u + texgen]) {
        info = g_xf[0x1040u + texgen];
        type = bits(info, 4u, 3u);
        source_row = bits(info, 7u, 5u);
    }

    if (type == 2u || type == 3u) {
        vertex->texcoord[texgen][0] =
            (float)rgba_channel(vertex->color, 0u) / 255.0f;
        vertex->texcoord[texgen][1] =
            (float)rgba_channel(vertex->color, 1u) / 255.0f;
        return;
    }
    if (type != 0u)
        return;

    if (source_row == 0u) {
        coord[0] = vertex->position[0];
        coord[1] = vertex->position[1];
        coord[2] = vertex->position[2];
    } else if (source_row >= 5u && source_row <= 12u) {
        const uint32_t source = source_row - 5u;
        if (vertex->raw_texcoord_valid[source]) {
            coord[0] = vertex->raw_texcoord[source][0];
            coord[1] = vertex->raw_texcoord[source][1];
        }
    }
    if (bits(info, 2u, 1u) == 0u)
        coord[2] = 1.0f;

    if (vertex->texture_matrix_valid[texgen]) {
        matrix_row = vertex->texture_matrix[texgen];
    } else if (texgen < 4u && g_xf_written[0x1018u]) {
        matrix_row = bits(g_xf[0x1018u], 6u + texgen * 6u, 6u);
    } else if (texgen >= 4u && g_xf_written[0x1019u]) {
        matrix_row = bits(g_xf[0x1019u], (texgen - 4u) * 6u, 6u);
    }
    if (bits(info, 1u, 1u) != 0u)
        matrix_rows = 3u;
    identity_texture_rows(rows);
    if (matrix_row != 60u &&
        !load_xf_matrix_rows(0u, matrix_row, 0x3fu,
                             matrix_rows, rows)) {
        identity_texture_rows(rows);
    }

    result[0] = result[1] = 0.0f;
    result[2] = 1.0f;
    for (i = 0u; i < matrix_rows; ++i) {
        result[i] = rows[i][0] * coord[0] +
                    rows[i][1] * coord[1] +
                    rows[i][2] * coord[2] +
                    rows[i][3] * coord[3];
    }

    if ((g_xf[0x1012u] & 1u) != 0u &&
        g_xf_written[0x1050u + texgen]) {
        const uint32_t post_info = g_xf[0x1050u + texgen];
        const uint32_t post_row = bits(post_info, 0u, 6u);
        float post[3][4];

        if (bits(post_info, 8u, 1u) != 0u) {
            const float length = sqrtf(result[0] * result[0] +
                                       result[1] * result[1] +
                                       result[2] * result[2]);
            if (isfinite(length) && length > 1.0e-8f) {
                result[0] /= length;
                result[1] /= length;
                result[2] /= length;
            }
        }
        if (load_xf_matrix_rows(0x500u, post_row, 0x3fu, 3u, post)) {
            float transformed[3];
            for (i = 0u; i < 3u; ++i) {
                transformed[i] = post[i][0] * result[0] +
                                 post[i][1] * result[1] +
                                 post[i][2] * result[2] + post[i][3];
            }
            memcpy(result, transformed, sizeof(result));
        }
    }

    if (result[2] == 0.0f) {
        result[0] = fmaxf(-1.0f, fminf(1.0f, result[0] * 0.5f));
        result[1] = fmaxf(-1.0f, fminf(1.0f, result[1] * 0.5f));
    }
    if (isfinite(result[0]) && isfinite(result[1]) &&
        isfinite(result[2])) {
        memcpy(vertex->texcoord[texgen], result, sizeof(result));
    }
}

static void initialize_position_transform(GxPositionTransform* transform)
{
    unsigned i;
    const float x_offset =
        (float)(bits(g_bp[0x59u], 0u, 9u) << 1u);
    const float y_offset =
        (float)(bits(g_bp[0x59u], 10u, 9u) << 1u);

    transform->matrix_valid = false;
    for (i = 0u; i < 6u; ++i)
        transform->projection[i] = bits_to_float(g_xf[0x1020u + i]);
    transform->viewport_width = bits_to_float(g_xf[0x101au]);
    transform->viewport_height = bits_to_float(g_xf[0x101bu]);
    transform->viewport_x = bits_to_float(g_xf[0x101du]) - x_offset;
    transform->viewport_y = bits_to_float(g_xf[0x101eu]) - y_offset;
    transform->depth_range = bits_to_float(g_xf[0x101cu]);
    transform->far_z = bits_to_float(g_xf[0x101fu]);
    transform->projection_type = g_xf[0x1026u];
}

static const float* position_matrix(GxPositionTransform* transform,
                                    uint32_t matrix_index)
{
    unsigned i;

    if (transform->matrix_valid &&
        transform->matrix_index == matrix_index)
        return transform->matrix;

    for (i = 0u; i < 12u; ++i) {
        const uint32_t address = matrix_index * 4u + i;
        transform->matrix[i] = address < GX_XF_COUNT ?
            bits_to_float(g_xf[address]) :
            (i == 0u || i == 5u || i == 10u ? 1.0f : 0.0f);
    }
    transform->matrix_index = matrix_index;
    transform->matrix_valid = true;
    return transform->matrix;
}

static bool transform_vertex_position(GxVertex* vertex,
                                      GxPositionTransform* transform)
{
    uint32_t matrix_index = vertex->position_matrix;
    float eye[4];
    float clip[4];
    const float* raw = transform->projection;
    const float* matrix;
    float inv_w;

    if ((g_cp[0x50] & 1u) == 0u)
        matrix_index = g_position_matrix_index;
    matrix = position_matrix(transform, matrix_index);

    eye[0] = matrix[0] * vertex->position[0] + matrix[1] * vertex->position[1] +
             matrix[2] * vertex->position[2] + matrix[3];
    eye[1] = matrix[4] * vertex->position[0] + matrix[5] * vertex->position[1] +
             matrix[6] * vertex->position[2] + matrix[7];
    eye[2] = matrix[8] * vertex->position[0] + matrix[9] * vertex->position[1] +
             matrix[10] * vertex->position[2] + matrix[11];
    eye[3] = 1.0f;

    if (transform->projection_type == 0u) {
        clip[0] = raw[0] * eye[0] + raw[1] * eye[2];
        clip[1] = raw[2] * eye[1] + raw[3] * eye[2];
        clip[2] = raw[4] * eye[2] + raw[5] * eye[3];
        clip[3] = -eye[2];
    } else {
        clip[0] = raw[0] * eye[0] + raw[1] * eye[3];
        clip[1] = raw[2] * eye[1] + raw[3] * eye[3];
        clip[2] = raw[4] * eye[2] + raw[5] * eye[3];
        clip[3] = eye[3];
    }

    if (!isfinite(clip[0]) || !isfinite(clip[1]) ||
        !isfinite(clip[2]) || !isfinite(clip[3]) ||
        fabsf(clip[3]) < 1.0e-8f) {
        vertex->valid = false;
        return false;
    }
    inv_w = 1.0f / clip[3];
    vertex->inv_w = inv_w;
    vertex->screen[0] =
        (clip[0] * inv_w * transform->viewport_width +
         transform->viewport_x) *
                        ((float)GX_CONTENT_WIDTH /
                         (float)GX_LOGICAL_EFB_WIDTH) +
                        (float)GX_CONTENT_X;
    vertex->screen[1] =
        (clip[1] * inv_w * transform->viewport_height +
         transform->viewport_y) *
                        ((float)GX_CONTENT_HEIGHT /
                         (float)GX_LOGICAL_EFB_HEIGHT) +
                        (float)GX_CONTENT_Y;
    vertex->depth = clip[2] * inv_w * transform->depth_range +
                    transform->far_z;
    vertex->valid = true;
    return true;
}

static void transform_vertex(GxVertex* vertex,
                             GxPositionTransform* transform)
{
    uint32_t texgen;

    if (!transform_vertex_position(vertex, transform)) {
        ++g_stats.clipped_vertices;
        return;
    }
    for (texgen = 0u; texgen < texgen_count(); ++texgen)
        transform_texture_coordinate(vertex, texgen);
}

static void clear_efb(uint32_t rgba)
{
    size_t i;
    const size_t count = (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT;
    if (g_efb == NULL)
        return;
    for (i = 0; i < count; ++i)
        g_efb[i] = rgba;
    g_efb_dirty = true;
}

static void queue_efb_texture_capture(void)
{
    const uint32_t source = g_bp[0x49u];
    const uint32_t dimensions = g_bp[0x4au];
    const uint32_t destination = (g_bp[0x4bu] & 0x00ffffffu) << 5u;

    /*
     * Match the linked submenu fix's lifecycle: remember the GXCopyTex request
     * now, but do not publish it to texture lookup until the rendered frame is
     * complete. The staging copy is necessary here because GXCopyTex may also
     * clear or continue drawing into the software EFB before that boundary.
     *
     * PE format Z24 denotes a depth copy. Submenu backdrops are color copies;
     * leave depth textures on the existing guest-memory path.
     */
    if (destination == 0u || g_efb == NULL ||
        g_efb_capture.pending_pixels == NULL ||
        bits(g_bp[0x43u], 0u, 3u) == 3u)
        return;

    memcpy(g_efb_capture.pending_pixels, g_efb,
           (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT * sizeof(uint32_t));
    g_efb_capture.pending_address = destination;
    g_efb_capture.pending_source_x =
        (uint16_t)bits(source, 10u, 10u);
    g_efb_capture.pending_source_y =
        (uint16_t)bits(source, 0u, 10u);
    g_efb_capture.pending_source_width =
        (uint16_t)(bits(dimensions, 10u, 10u) + 1u);
    g_efb_capture.pending_source_height =
        (uint16_t)(bits(dimensions, 0u, 10u) + 1u);
    g_efb_capture.pending = true;
}

static void finish_efb_texture_capture(void)
{
    uint32_t* published_pixels;
    unsigned key = GX_EFB_CAPTURE_KEY_COUNT;
    unsigned i;

    if (!g_efb_capture.pending || g_efb_capture.pixels == NULL ||
        g_efb_capture.pending_pixels == NULL)
        return;

    /*
     * The reference keeps one pinned capture texture and updates it only after
     * frame submission. Swap the preallocated software buffers to do the same
     * without an extra full-frame copy or any allocation in the menu path.
     */
    published_pixels = g_efb_capture.pixels;
    g_efb_capture.pixels = g_efb_capture.pending_pixels;
    g_efb_capture.pending_pixels = published_pixels;
    g_efb_capture.source_x = g_efb_capture.pending_source_x;
    g_efb_capture.source_y = g_efb_capture.pending_source_y;
    g_efb_capture.source_width = g_efb_capture.pending_source_width;
    g_efb_capture.source_height = g_efb_capture.pending_source_height;
    if (++g_efb_capture.generation == 0u)
        g_efb_capture.generation = 1u;
    g_efb_capture.valid = true;

    for (i = 0u; i < GX_EFB_CAPTURE_KEY_COUNT; ++i) {
        if (g_efb_capture.keys[i].valid &&
            g_efb_capture.keys[i].address ==
                g_efb_capture.pending_address) {
            key = i;
            break;
        }
    }
    if (key == GX_EFB_CAPTURE_KEY_COUNT) {
        for (i = 0u; i < GX_EFB_CAPTURE_KEY_COUNT; ++i) {
            if (!g_efb_capture.keys[i].valid) {
                key = i;
                break;
            }
        }
    }
    if (key == GX_EFB_CAPTURE_KEY_COUNT)
        key = g_efb_capture.next_key;
    g_efb_capture.keys[key].address = g_efb_capture.pending_address;
    g_efb_capture.keys[key].valid = true;
    g_efb_capture.next_key =
        (uint8_t)((key + 1u) % GX_EFB_CAPTURE_KEY_COUNT);
    g_efb_capture.pending = false;
}

static void clear_efb_after_copy(uint32_t rgba)
{
    const bool color_update = (g_bp[0x41u] & (1u << 3)) != 0u;
    const bool alpha_update = (g_bp[0x41u] & (1u << 4)) != 0u;
    const bool depth_update = (g_bp[0x40u] & (1u << 4)) != 0u;
    const size_t count = (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT;
    size_t i;

    if (g_efb != NULL && (color_update || alpha_update)) {
        for (i = 0; i < count; ++i) {
            uint32_t result = g_efb[i];
            if (color_update)
                result = (result & 0x000000ffu) | (rgba & 0xffffff00u);
            if (alpha_update)
                result = (result & 0xffffff00u) | (rgba & 0x000000ffu);
            g_efb[i] = result;
        }
        g_efb_dirty = true;
    }
    if (g_zfb != NULL && depth_update) {
        const uint32_t depth = g_bp[0x51u] & 0x00ffffffu;
        for (i = 0; i < count; ++i)
            g_zfb[i] = depth;
    }
}

static bool depth_compare(uint32_t incoming, uint32_t stored, uint32_t function)
{
    switch (function & 7u) {
    case 0u: return false;
    case 1u: return incoming < stored;
    case 2u: return incoming == stored;
    case 3u: return incoming <= stored;
    case 4u: return incoming > stored;
    case 5u: return incoming != stored;
    case 6u: return incoming >= stored;
    default: return true;
    }
}

static bool alpha_compare(uint32_t alpha, uint32_t reference,
                          uint32_t function)
{
    switch (function & 7u) {
    case 0u: return false;
    case 1u: return alpha < reference;
    case 2u: return alpha == reference;
    case 3u: return alpha <= reference;
    case 4u: return alpha > reference;
    case 5u: return alpha != reference;
    case 6u: return alpha >= reference;
    default: return true;
    }
}

static bool alpha_test_passes(uint32_t rgba)
{
    const uint32_t alpha_test = g_bp[0xf3u];
    const uint32_t alpha = rgba & 0xffu;
    const bool compare0 = alpha_compare(
        alpha, bits(alpha_test, 0u, 8u), bits(alpha_test, 16u, 3u));
    const bool compare1 = alpha_compare(
        alpha, bits(alpha_test, 8u, 8u), bits(alpha_test, 19u, 3u));

    switch (bits(alpha_test, 22u, 2u)) {
    case 0u: return compare0 && compare1;
    case 1u: return compare0 || compare1;
    case 2u: return compare0 != compare1;
    default: return compare0 == compare1;
    }
}

static uint32_t clamp_depth(float depth)
{
    if (!isfinite(depth) || depth <= 0.0f)
        return 0u;
    if (depth >= 16777215.0f)
        return 0x00ffffffu;
    return (uint32_t)(depth + 0.5f);
}

static uint32_t interpolate_vertex_color(const GxVertex* a,
                                         const GxVertex* b,
                                         const GxVertex* c,
                                         uint32_t color_a,
                                         uint32_t color_b,
                                         uint32_t color_c,
                                         float w0, float w1, float w2)
{
    const float denominator = w0 * a->inv_w + w1 * b->inv_w +
                              w2 * c->inv_w;
    uint32_t result = 0u;
    unsigned channel;

    if (color_a == color_b && color_a == color_c)
        return color_a;
    if (!isfinite(denominator) || fabsf(denominator) < 1.0e-8f)
        return color_a;

    for (channel = 0u; channel < 4u; ++channel) {
        const float numerator =
            w0 * (float)rgba_channel(color_a, channel) * a->inv_w +
            w1 * (float)rgba_channel(color_b, channel) * b->inv_w +
            w2 * (float)rgba_channel(color_c, channel) * c->inv_w;
        float value = numerator / denominator;

        if (!isfinite(value) || value < 0.0f)
            value = 0.0f;
        if (value > 255.0f)
            value = 255.0f;
        result |= (uint32_t)(value + 0.5f) << (24u - channel * 8u);
    }
    return result;
}

static uint32_t apply_unlit_channel_state(uint32_t vertex_color,
                                          unsigned channel)
{
    const uint32_t material_address = 0x100cu + channel;
    /* XF keeps both color controls before both alpha controls. */
    const uint32_t color_control_address = 0x100eu + channel;
    const uint32_t alpha_control_address = 0x1010u + channel;
    uint32_t result = vertex_color;

    if (channel >= 2u || !g_xf_written[material_address])
        return result;

    /*
     * With lighting disabled, GX independently chooses register or vertex
     * material sources for RGB and alpha. Full-screen Galaxy image effects
     * have no vertex color and put their fade alpha in XF material0; treating
     * the absent vertex attribute as opaque left only the uncovered bottom
     * edge visible.
     */
    if (g_xf_written[color_control_address] &&
        bits(g_xf[color_control_address], 1u, 1u) == 0u &&
        bits(g_xf[color_control_address], 0u, 1u) == 0u) {
        result = (result & 0x000000ffu) |
                 (g_xf[material_address] & 0xffffff00u);
    }
    if (g_xf_written[alpha_control_address] &&
        bits(g_xf[alpha_control_address], 1u, 1u) == 0u &&
        bits(g_xf[alpha_control_address], 0u, 1u) == 0u) {
        result = (result & 0xffffff00u) |
                 (g_xf[material_address] & 0x000000ffu);
    }
    return result;
}

static bool put_efb_pixel(int x, int y, uint32_t rgba)
{
    uint32_t* destination;
    uint32_t result;
    const uint32_t blend_mode = g_bp[0x41u];
    const bool blend_enable = (blend_mode & 1u) != 0u;
    const bool color_update = (blend_mode & (1u << 3)) != 0u;
    const bool alpha_update = (blend_mode & (1u << 4)) != 0u;

    if (g_efb == NULL || x < 0 || y < 0 || x >= GX_EFB_WIDTH || y >= GX_EFB_HEIGHT)
        return false;
    destination = &g_efb[(size_t)y * (size_t)GX_EFB_WIDTH + (size_t)x];
    result = *destination;
    if (blend_enable) {
        const uint32_t source_factor = bits(blend_mode, 8u, 3u);
        const uint32_t destination_factor = bits(blend_mode, 5u, 3u);
        const bool subtract = (blend_mode & (1u << 11)) != 0u;
        uint32_t blended = 0u;
        unsigned channel;
        for (channel = 0u; channel < 4u; ++channel) {
            const uint32_t source = rgba_channel(rgba, channel);
            const uint32_t dest = rgba_channel(*destination, channel);
            const uint32_t source_color_factor =
                rgba_channel(*destination, channel);
            const uint32_t destination_color_factor =
                rgba_channel(rgba, channel);
            uint32_t sf;
            uint32_t df;
            int value;

            switch (source_factor) {
            case 0u: sf = 0u; break;
            case 1u: sf = 255u; break;
            case 2u: sf = source_color_factor; break;
            case 3u: sf = 255u - source_color_factor; break;
            case 4u: sf = rgba_channel(rgba, 3u); break;
            case 5u: sf = 255u - rgba_channel(rgba, 3u); break;
            case 6u: sf = rgba_channel(*destination, 3u); break;
            default: sf = 255u - rgba_channel(*destination, 3u); break;
            }
            switch (destination_factor) {
            case 0u: df = 0u; break;
            case 1u: df = 255u; break;
            case 2u: df = destination_color_factor; break;
            case 3u: df = 255u - destination_color_factor; break;
            case 4u: df = rgba_channel(rgba, 3u); break;
            case 5u: df = 255u - rgba_channel(rgba, 3u); break;
            case 6u: df = rgba_channel(*destination, 3u); break;
            default: df = 255u - rgba_channel(*destination, 3u); break;
            }
            value = subtract ? (int)source - (int)dest :
                               (int)((source * sf + dest * df + 127u) / 255u);
            if (value < 0)
                value = 0;
            if (value > 255)
                value = 255;
            blended |= (uint32_t)value << (24u - channel * 8u);
        }
        rgba = blended;
    }
    if (color_update)
        result = (result & 0x000000ffu) | (rgba & 0xffffff00u);
    if (alpha_update)
        result = (result & 0xffffff00u) | (rgba & 0x000000ffu);
    if (result != *destination) {
        *destination = result;
    }
    return true;
}

static void fill_triangle(const GxVertex* a, const GxVertex* b,
                          const GxVertex* c,
                          const GxDrawTextures* textures)
{
    const float x0 = a->screen[0];
    const float y0 = a->screen[1];
    const float x1 = b->screen[0];
    const float y1 = b->screen[1];
    const float x2 = c->screen[0];
    const float y2 = c->screen[1];
    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    float min_x;
    float max_x;
    float min_y;
    float max_y;
    float inv_area;
    float edge0_row;
    float edge1_row;
    float edge2_row;
    const float edge0_dx = y1 - y2;
    const float edge1_dx = y2 - y0;
    const float edge2_dx = y0 - y1;
    const float edge0_dy = x2 - x1;
    const float edge1_dy = x0 - x2;
    const float edge2_dy = x1 - x0;
    int x;
    int y;
    int start_x;
    int start_y;
    uint32_t pixels = 0u;
    uint32_t colored_pixels = 0u;
    uint32_t depth_rejected_pixels = 0u;
    uint32_t texture_samples = 0u;
    const uint32_t z_mode = g_bp[0x40u];
    const bool depth_test = (z_mode & 1u) != 0u;
    const bool depth_update = (z_mode & (1u << 4)) != 0u;
    const uint32_t depth_function = bits(z_mode, 1, 3);
    const bool early_depth_test = depth_test &&
                                  (g_bp[0x43u] & (1u << 6)) != 0u;

    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) ||
        !isfinite(y1) || !isfinite(x2) || !isfinite(y2) ||
        !isfinite(area) || fabsf(area) < 1.0e-4f)
        return;

    min_x = fminf(x0, fminf(x1, x2));
    max_x = fmaxf(x0, fmaxf(x1, x2));
    min_y = fminf(y0, fminf(y1, y2));
    max_y = fmaxf(y0, fmaxf(y1, y2));
    if (max_x < 0.0f || max_y < 0.0f ||
        min_x > (float)(GX_EFB_WIDTH - 1) ||
        min_y > (float)(GX_EFB_HEIGHT - 1))
        return;

    if (min_x < 0.0f)
        min_x = 0.0f;
    if (min_y < 0.0f)
        min_y = 0.0f;
    if (max_x > (float)(GX_EFB_WIDTH - 1))
        max_x = (float)(GX_EFB_WIDTH - 1);
    if (max_y > (float)(GX_EFB_HEIGHT - 1))
        max_y = (float)(GX_EFB_HEIGHT - 1);

    inv_area = 1.0f / area;
    start_x = (int)min_x;
    start_y = (int)min_y;
    {
        const float px = (float)start_x + 0.5f;
        const float py = (float)start_y + 0.5f;
        edge0_row = (x1 - px) * (y2 - py) - (x2 - px) * (y1 - py);
        edge1_row = (x2 - px) * (y0 - py) - (x0 - px) * (y2 - py);
        edge2_row = (x0 - px) * (y1 - py) - (x1 - px) * (y0 - py);
    }
    for (y = start_y; y <= (int)max_y; ++y) {
        float w0 = edge0_row;
        float w1 = edge1_row;
        float w2 = edge2_row;
        for (x = start_x; x <= (int)max_x; ++x) {
            if ((w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f)) {
                const size_t offset =
                    (size_t)y * (size_t)GX_EFB_WIDTH + (size_t)x;
                const uint32_t depth = clamp_depth(
                    (w0 * a->depth + w1 * b->depth + w2 * c->depth) *
                    inv_area);
                uint32_t color = a->color;
                uint32_t raster1 = a->color1;
                bool depth_passed = true;

                if (early_depth_test && g_zfb != NULL) {
                    depth_passed = depth_compare(
                        depth, g_zfb[offset], depth_function);
                    if (!depth_passed) {
                        ++depth_rejected_pixels;
                    } else if (depth_update) {
                        g_zfb[offset] = depth;
                    }
                }
                if (depth_passed) {
                    uint32_t stage_samples[GX_TEV_STAGE_COUNT];
                    const unsigned stage_count =
                        textures != NULL ? textures->stage_count : 0u;
                    unsigned stage;

                    color = interpolate_vertex_color(
                        a, b, c, a->color, b->color, c->color,
                        w0, w1, w2);
                    raster1 = interpolate_vertex_color(
                        a, b, c, a->color1, b->color1, c->color1,
                        w0, w1, w2);

                    for (stage = 0u; stage < stage_count; ++stage) {
                        stage_samples[stage] = 0xffffffffu;
                        if (textures->stage_textured[stage]) {
                            const uint32_t texgen =
                                textures->stage_texgen[stage];
                            const GxTexture* texture =
                                &textures->maps[textures->stage_map[stage]];
                            if (texgen < GX_TEXTURE_MAP_COUNT &&
                                texture->valid) {
                                const float projected_q =
                                    w0 * a->texcoord[texgen][2] * a->inv_w +
                                    w1 * b->texcoord[texgen][2] * b->inv_w +
                                    w2 * c->texcoord[texgen][2] * c->inv_w;
                                if (isfinite(projected_q) &&
                                    fabsf(projected_q) > 1.0e-8f) {
                                    const float s =
                                        (w0 * a->texcoord[texgen][0] * a->inv_w +
                                         w1 * b->texcoord[texgen][0] * b->inv_w +
                                         w2 * c->texcoord[texgen][0] * c->inv_w) /
                                        projected_q;
                                    const float t =
                                        (w0 * a->texcoord[texgen][1] * a->inv_w +
                                         w1 * b->texcoord[texgen][1] * b->inv_w +
                                         w2 * c->texcoord[texgen][1] * c->inv_w) /
                                        projected_q;
                                    stage_samples[stage] =
                                        sample_texture(texture, s, t);
                                    ++texture_samples;
                                }
                            }
                        }
                    }
                    color = shade_fragment(
                        color, raster1,
                        textures != NULL ? stage_samples : NULL);
                }
                if (depth_passed && alpha_test_passes(color)) {
                    if (!early_depth_test) {
                        if (depth_test && g_zfb != NULL &&
                            !depth_compare(depth, g_zfb[offset],
                                           depth_function)) {
                            depth_passed = false;
                            ++depth_rejected_pixels;
                        } else if (depth_update && g_zfb != NULL) {
                            g_zfb[offset] = depth;
                        }
                    }
                    if (depth_passed) {
                        if (put_efb_pixel(x, y, color)) {
                            ++pixels;
                            if (color_is_visible(color))
                                ++colored_pixels;
                        }
                    }
                }
            }
            w0 += edge0_dx;
            w1 += edge1_dx;
            w2 += edge2_dx;
        }
        edge0_row += edge0_dy;
        edge1_row += edge1_dy;
        edge2_row += edge2_dy;
    }
    g_stats.depth_rejected_pixels += depth_rejected_pixels;
    g_stats.texture_samples += texture_samples;
    if (pixels != 0u) {
        ++g_stats.triangles;
        g_stats.rasterized_pixels += pixels;
        g_stats.colored_pixels += colored_pixels;
        g_stats.video_started = true;
        g_efb_dirty = true;
    }
}

static bool rasterizer_self_test(void)
{
    GxVertex a;
    GxVertex b;
    GxVertex c;
    const uint32_t before = g_stats.rasterized_pixels;
    const uint32_t old_gen_mode = g_bp[0x00u];
    const uint32_t old_blend_mode = g_bp[0x41u];
    const uint32_t old_z_mode = g_bp[0x40u];
    const uint32_t old_pe_control = g_bp[0x43u];
    const uint32_t old_alpha_test = g_bp[0xf3u];
    const uint32_t old_color_env = g_bp[0xc0u];
    const uint32_t old_alpha_env = g_bp[0xc1u];
    const size_t sample = (size_t)40u * GX_EFB_WIDTH + 40u;
    const size_t blend_sample = (size_t)10u * GX_EFB_WIDTH + 10u;
    const uint32_t blend_background = 0x204060ffu;
    const uint32_t old_blend_sample = g_efb[blend_sample];
    uint32_t rejected_before;
    uint32_t first_sample;
    uint32_t first_depth;
    uint32_t late_alpha_depth;
    uint32_t early_alpha_depth;
    bool transparent_blend_ok;
    bool destination_factor_ok;
    bool source_color_factor_ok;
    bool interpolated_alpha_ok;
    bool material_alpha_ok;
    uint32_t old_material0;
    uint32_t old_color0_control;
    uint32_t old_alpha0_control;
    bool old_material0_written;
    bool old_color0_control_written;
    bool old_alpha0_control_written;
    size_t i;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    a.screen[0] = 32.0f;
    a.screen[1] = 32.0f;
    b.screen[0] = 96.0f;
    b.screen[1] = 32.0f;
    c.screen[0] = 32.0f;
    c.screen[1] = 96.0f;
    a.depth = b.depth = c.depth = 100.0f;
    a.inv_w = b.inv_w = c.inv_w = 1.0f;
    a.color = b.color = c.color = 0x00ff00ffu;
    a.color1 = b.color1 = c.color1 = 0xffffffffu;
    a.valid = b.valid = c.valid = true;
    g_bp[0x00u] &= ~0x00003c00u;
    g_bp[0x41u] = (1u << 3) | (1u << 4);
    g_bp[0x40u] = 1u | (3u << 1) | (1u << 4);
    g_bp[0x43u] = 0u;
    g_bp[0xf3u] = (7u << 16) | (7u << 19);
    g_bp[0xc0u] = 0x0008fffau;
    g_bp[0xc1u] = 0x0008ffd0u;
    for (i = 0; i < (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT; ++i)
        g_zfb[i] = 0x00ffffffu;
    fill_triangle(&a, &b, &c, NULL);
    first_sample = g_efb[sample];
    first_depth = g_zfb[sample];
    g_bp[0xf3u] = (4u << 16) | (4u << 19);
    a.depth = b.depth = c.depth = 50.0f;
    a.color = b.color = c.color = 0xffffff00u;
    fill_triangle(&a, &b, &c, NULL);
    late_alpha_depth = g_zfb[sample];
    g_bp[0x43u] = 1u << 6;
    fill_triangle(&a, &b, &c, NULL);
    early_alpha_depth = g_zfb[sample];
    g_bp[0xf3u] = (7u << 16) | (7u << 19);
    rejected_before = g_stats.depth_rejected_pixels;
    a.depth = b.depth = c.depth = 200.0f;
    a.color = b.color = c.color = 0xff0000ffu;
    fill_triangle(&a, &b, &c, NULL);
    g_efb[blend_sample] = blend_background;
    g_bp[0x41u] = 1u | (1u << 3) | (1u << 4) |
                  (5u << 5) | (4u << 8);
    transparent_blend_ok =
        put_efb_pixel(10, 10, 0xff000000u) &&
        g_efb[blend_sample] == blend_background;
    g_efb[blend_sample] = 0x804020ffu;
    g_bp[0x41u] = 1u | (1u << 3) | (1u << 4) | (2u << 5);
    destination_factor_ok =
        put_efb_pixel(10, 10, 0x4080c0ffu) &&
        g_efb[blend_sample] == 0x202018ffu;
    g_efb[blend_sample] = 0x804020ffu;
    g_bp[0x41u] = 1u | (1u << 3) | (1u << 4) | (2u << 8);
    source_color_factor_ok =
        put_efb_pixel(10, 10, 0x4080c0ffu) &&
        g_efb[blend_sample] == 0x202018ffu;
    a.color = 0xffffff00u;
    b.color = c.color = 0xffffffffu;
    interpolated_alpha_ok =
        (interpolate_vertex_color(&a, &b, &c,
                                  a.color, b.color, c.color,
                                  1.0f, 1.0f, 0.0f) & 0xffu) == 128u;
    old_material0 = g_xf[0x100cu];
    old_color0_control = g_xf[0x100eu];
    old_alpha0_control = g_xf[0x1010u];
    old_material0_written = g_xf_written[0x100cu];
    old_color0_control_written = g_xf_written[0x100eu];
    old_alpha0_control_written = g_xf_written[0x1010u];
    g_xf[0x100cu] = 0x80a0c040u;
    g_xf[0x100eu] = 1u;
    g_xf[0x1010u] = 0u;
    g_xf_written[0x100cu] = true;
    g_xf_written[0x100eu] = true;
    g_xf_written[0x1010u] = true;
    material_alpha_ok =
        apply_unlit_channel_state(0x102030ffu, 0u) == 0x10203040u;
    g_xf[0x100cu] = old_material0;
    g_xf[0x100eu] = old_color0_control;
    g_xf[0x1010u] = old_alpha0_control;
    g_xf_written[0x100cu] = old_material0_written;
    g_xf_written[0x100eu] = old_color0_control_written;
    g_xf_written[0x1010u] = old_alpha0_control_written;
    g_efb[blend_sample] = old_blend_sample;
    g_bp[0x00u] = old_gen_mode;
    g_bp[0x41u] = old_blend_mode;
    g_bp[0x40u] = old_z_mode;
    g_bp[0x43u] = old_pe_control;
    g_bp[0xf3u] = old_alpha_test;
    g_bp[0xc0u] = old_color_env;
    g_bp[0xc1u] = old_alpha_env;
    return g_stats.rasterized_pixels > before &&
           first_sample == 0x00ff00ffu && g_efb[sample] == first_sample &&
           late_alpha_depth == first_depth && early_alpha_depth == 50u &&
           g_stats.depth_rejected_pixels > rejected_before &&
           transparent_blend_ok && destination_factor_ok &&
           source_color_factor_ok && interpolated_alpha_ok &&
           material_alpha_ok;
}

static bool efb_capture_self_test(void)
{
    const uint32_t destination = 0x00002000u;
    const uint32_t expected = 0x183c72ffu;
    const uint32_t old_source = g_bp[0x49u];
    const uint32_t old_dimensions = g_bp[0x4au];
    const uint32_t old_destination = g_bp[0x4bu];
    const uint32_t old_pe_control = g_bp[0x43u];
    const uint32_t old_pixel = g_efb != NULL ? g_efb[0] : 0u;
    bool queued;
    bool published;

    if (g_efb == NULL || g_efb_capture.pixels == NULL ||
        g_efb_capture.pending_pixels == NULL)
        return false;

    memset(g_efb_capture.keys, 0, sizeof(g_efb_capture.keys));
    g_efb_capture.generation = 0u;
    g_efb_capture.next_key = 0u;
    g_efb_capture.valid = false;
    g_efb_capture.pending = false;
    g_bp[0x49u] = 0u;
    g_bp[0x4au] = ((GX_LOGICAL_EFB_WIDTH - 1u) << 10u) |
                  (GX_LOGICAL_EFB_HEIGHT - 1u);
    g_bp[0x4bu] = destination >> 5u;
    g_bp[0x43u] = 0u;
    g_efb[0] = expected;

    queue_efb_texture_capture();
    queued = g_efb_capture.pending &&
             find_efb_capture(destination) == NULL;
    g_efb[0] = 0u;
    finish_efb_texture_capture();
    published = find_efb_capture(destination) != NULL &&
                g_efb_capture.pixels[0] == expected &&
                !g_efb_capture.pending;

    g_bp[0x49u] = old_source;
    g_bp[0x4au] = old_dimensions;
    g_bp[0x4bu] = old_destination;
    g_bp[0x43u] = old_pe_control;
    g_efb[0] = old_pixel;
    memset(g_efb_capture.keys, 0, sizeof(g_efb_capture.keys));
    g_efb_capture.generation = 0u;
    g_efb_capture.next_key = 0u;
    g_efb_capture.valid = false;
    g_efb_capture.pending = false;
    return queued && published;
}

static void add_triangle_indices(uint8_t primitive, uint32_t count, uint32_t* index_count)
{
    uint32_t i;
    *index_count = 0;
    if (primitive == 2u) {
        for (i = 0; i + 2u < count && *index_count + 3u <= GX_MAX_INDICES; i += 3u) {
            g_indices[(*index_count)++] = i;
            g_indices[(*index_count)++] = i + 1u;
            g_indices[(*index_count)++] = i + 2u;
        }
    } else if (primitive == 0u || primitive == 1u) {
        for (i = 0; i + 3u < count && *index_count + 6u <= GX_MAX_INDICES; i += 4u) {
            g_indices[(*index_count)++] = i;
            g_indices[(*index_count)++] = i + 1u;
            g_indices[(*index_count)++] = i + 2u;
            g_indices[(*index_count)++] = i;
            g_indices[(*index_count)++] = i + 2u;
            g_indices[(*index_count)++] = i + 3u;
        }
        if (count - i == 3u && *index_count + 3u <= GX_MAX_INDICES) {
            g_indices[(*index_count)++] = i;
            g_indices[(*index_count)++] = i + 1u;
            g_indices[(*index_count)++] = i + 2u;
        }
    } else if (primitive == 4u) {
        for (i = 2u; i < count && *index_count + 3u <= GX_MAX_INDICES; ++i) {
            g_indices[(*index_count)++] = 0;
            g_indices[(*index_count)++] = i - 1u;
            g_indices[(*index_count)++] = i;
        }
    } else if (primitive == 3u) {
        for (i = 0; i + 2u < count && *index_count + 3u <= GX_MAX_INDICES; ++i) {
            if ((i & 1u) == 0u) {
                g_indices[(*index_count)++] = i;
                g_indices[(*index_count)++] = i + 1u;
                g_indices[(*index_count)++] = i + 2u;
            } else {
                g_indices[(*index_count)++] = i + 1u;
                g_indices[(*index_count)++] = i;
                g_indices[(*index_count)++] = i + 2u;
            }
        }
    }
}

/*
 * This is the conservative half of the reference port's whole-batch frustum
 * cull. If every drawable triangle vertex is beyond the same screen edge, the
 * rasterizer's per-triangle bounds test would reject all of them. Detect that
 * before preparing textures or entering the fragment path.
 */
static bool triangle_batch_may_reach_viewport(uint32_t index_count)
{
    bool any_triangle = false;
    bool outside_left = true;
    bool outside_right = true;
    bool outside_top = true;
    bool outside_bottom = true;
    uint32_t i;

    for (i = 0u; i + 2u < index_count; i += 3u) {
        const GxVertex* triangle[3] = {
            &g_vertices[g_indices[i]],
            &g_vertices[g_indices[i + 1u]],
            &g_vertices[g_indices[i + 2u]]
        };
        unsigned vertex;

        if (!triangle[0]->valid || !triangle[1]->valid ||
            !triangle[2]->valid)
            continue;
        any_triangle = true;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const float x = triangle[vertex]->screen[0];
            const float y = triangle[vertex]->screen[1];

            outside_left = outside_left && x < 0.0f;
            outside_right = outside_right &&
                x > (float)(GX_EFB_WIDTH - 1);
            outside_top = outside_top && y < 0.0f;
            outside_bottom = outside_bottom &&
                y > (float)(GX_EFB_HEIGHT - 1);
        }
    }

    return any_triangle && !outside_left && !outside_right &&
           !outside_top && !outside_bottom;
}

static bool skip_vertex_attribute(ByteReader* reader, uint32_t descriptor,
                                  size_t direct_size,
                                  uint32_t indexed_components)
{
    size_t size;

    if (descriptor == 0u)
        return true;
    if (descriptor == 1u)
        size = direct_size;
    else if (descriptor == 2u || descriptor == 3u)
        size = (size_t)(descriptor - 1u) * indexed_components;
    else
        return false;
    return reader_take(reader, size) != NULL;
}

/*
 * Most retail draw commands in the current scenes are fully outside the
 * reduced 200x120 viewport. Parse only matrix indices and positions first so
 * those draws do not fetch unused normal/color/texture arrays or run texture
 * coordinate matrices. Visible batches take the complete decoder below.
 */
static bool predecode_positions(uint8_t command, const uint8_t* vertex_data,
                                size_t vertex_bytes, uint16_t vertex_count,
                                GxPositionTransform* transform,
                                uint32_t* produced_out,
                                uint32_t* index_count_out,
                                uint32_t* clipped_out)
{
    const uint8_t vat = command & 7u;
    const uint8_t primitive = (command & 0x78u) >> 3;
    const uint32_t vcd_low = g_cp[0x50u];
    const uint32_t vcd_high = g_cp[0x60u];
    const uint32_t g0 = g_cp[0x70u + vat];
    const uint32_t g1 = g_cp[0x80u + vat];
    const uint32_t g2 = g_cp[0x90u + vat];
    ByteReader reader = {vertex_data, vertex_bytes, 0u};
    uint32_t produced = 0u;
    uint32_t clipped = 0u;
    uint16_t vertex_index;

    for (vertex_index = 0u; vertex_index < vertex_count; ++vertex_index) {
        GxVertex vertex;
        const uint32_t position_desc = bits(vcd_low, 9u, 2u);
        const uint32_t position_format = bits(g0, 1u, 3u);
        const uint32_t position_count = bits(g0, 0u, 1u) + 2u;
        const uint32_t position_size =
            component_size(position_format) * position_count;
        const uint8_t* position = NULL;
        bool skip_vertex = false;
        uint32_t normal_desc;
        uint32_t normal_count;
        uint32_t normal_indices;
        unsigned i;

        memset(&vertex, 0, sizeof(vertex));
        if ((vcd_low & 1u) != 0u) {
            if (!reader_u8(&reader, &vertex.position_matrix))
                return false;
            vertex.position_matrix &= 0x3fu;
        }
        for (i = 0u; i < 8u; ++i) {
            uint8_t ignored;
            if ((vcd_low & (2u << i)) != 0u &&
                !reader_u8(&reader, &ignored))
                return false;
        }

        if (position_desc == 1u) {
            position = reader_take(&reader, position_size);
        } else if (position_desc == 2u || position_desc == 3u) {
            uint16_t index = 0u;
            if (!reader_index(&reader, position_desc, &index))
                return false;
            skip_vertex = index ==
                (position_desc == 2u ? 0xffu : 0xffffu);
            if (!skip_vertex) {
                const uint64_t address =
                    (uint64_t)g_cp[0xa0u] +
                    (uint64_t)(g_cp[0xb0u] & 0xffu) * index;
                if (address > 0xffffffffu)
                    return false;
                position = guest_bytes((uint32_t)address, position_size);
            }
        }
        if (position_desc != 0u && position == NULL && !skip_vertex)
            return false;
        for (i = 0u; i < position_count && position != NULL; ++i) {
            vertex.position[i] = read_component(
                position + i * component_size(position_format),
                position_format, bits(g0, 4u, 5u));
        }

        normal_desc = bits(vcd_low, 11u, 2u);
        normal_count = bits(g0, 9u, 1u) != 0u ? 9u : 3u;
        normal_indices = normal_desc >= 2u && normal_count == 9u &&
                         bits(g0, 31u, 1u) != 0u ? 3u : 1u;
        if (!skip_vertex_attribute(
                &reader, normal_desc,
                (size_t)component_size(bits(g0, 10u, 3u)) * normal_count,
                normal_indices))
            return false;
        for (i = 0u; i < 2u; ++i) {
            const uint32_t descriptor = bits(vcd_low, 13u + i * 2u, 2u);
            if (!skip_vertex_attribute(
                    &reader, descriptor,
                    color_size(bits(g0, 14u + i * 4u, 3u)), 1u))
                return false;
        }
        for (i = 0u; i < 8u; ++i) {
            uint32_t format;
            uint32_t elements;
            const uint32_t descriptor = bits(vcd_high, i * 2u, 2u);
            texture_format(g0, g1, g2, i, &format, &elements);
            if (!skip_vertex_attribute(
                    &reader, descriptor,
                    component_size(format) * (elements + 1u), 1u))
                return false;
        }

        if (!skip_vertex) {
            if (!transform_vertex_position(&vertex, transform))
                ++clipped;
            g_vertices[produced++] = vertex;
        }
    }
    if (reader.offset != reader.size)
        return false;

    add_triangle_indices(primitive, produced, index_count_out);
    *produced_out = produced;
    *clipped_out = clipped;
    return true;
}

static void record_draw_state(uint32_t produced, bool colors_decoded,
                              bool has_texture_binding,
                              uint32_t selected_texture_map)
{
    g_stats.vertices += produced;
    g_stats.last_z_mode = g_bp[0x40u];
    g_stats.last_blend_mode = g_bp[0x41u];
    g_stats.last_gen_mode = g_bp[0x00u];
    g_stats.last_tev_order = g_bp[0x28u];
    g_stats.last_tev_color_env = g_bp[0xc0u];
    g_stats.last_tev_alpha_env = g_bp[0xc1u];
    if ((g_bp[0x41u] & (1u << 3)) != 0u)
        ++g_stats.color_write_draws;
    if (has_texture_binding) {
        const uint32_t slot = selected_texture_map & 3u;
        const bool high_bank = (selected_texture_map & 4u) != 0u;

        ++g_stats.textured_draws;
        g_stats.last_texture_image0 =
            g_bp[(high_bank ? 0xa8u : 0x88u) + slot];
        g_stats.last_texture_image3 =
            g_bp[(high_bank ? 0xb4u : 0x94u) + slot];
        g_stats.textured_gen_mode = g_bp[0x00u];
        g_stats.textured_tev_order = g_bp[0x28u];
        g_stats.textured_tev_color_env = g_bp[0xc0u];
        g_stats.textured_tev_alpha_env = g_bp[0xc1u];
    }
    if (produced != 0u && colors_decoded)
        g_stats.last_vertex_color = g_vertices[0].color;
}

static bool decode_and_draw(uint8_t command, const uint8_t* vertex_data, size_t vertex_bytes,
                            uint16_t vertex_count)
{
    const uint8_t vat = command & 7u;
    const uint8_t primitive = (command & 0x78u) >> 3;
    const uint32_t vcd_low = g_cp[0x50];
    const uint32_t vcd_high = g_cp[0x60];
    const uint32_t g0 = g_cp[0x70u + vat];
    const uint32_t g1 = g_cp[0x80u + vat];
    const uint32_t g2 = g_cp[0x90u + vat];
    ByteReader reader = {vertex_data, vertex_bytes, 0};
    uint32_t produced = 0;
    uint32_t index_count = 0;
    uint16_t vertex_index;
    uint32_t i;
    uint32_t selected_texture_map = 0u;
    bool has_texture_binding = false;
    bool texture_map_used[GX_TEXTURE_MAP_COUNT] = {false};
    GxDrawTextures draw_textures;
    GxPositionTransform position_transform;

    memset(&draw_textures, 0, sizeof(draw_textures));
    initialize_position_transform(&position_transform);
    draw_textures.stage_count =
        (uint8_t)(bits(g_bp[0x00u], 10u, 4u) + 1u);
    if (draw_textures.stage_count > GX_TEV_STAGE_COUNT)
        draw_textures.stage_count = GX_TEV_STAGE_COUNT;
    for (i = 0u; i < draw_textures.stage_count; ++i) {
        uint32_t stage_texgen;
        uint32_t stage_map;

        if (texture_binding_for_stage(
                i, vcd_high, &stage_texgen, &stage_map)) {
            draw_textures.stage_texgen[i] = (uint8_t)stage_texgen;
            draw_textures.stage_map[i] = (uint8_t)stage_map;
            draw_textures.stage_textured[i] = true;
            texture_map_used[stage_map] = true;
            if (!has_texture_binding)
                selected_texture_map = stage_map;
            has_texture_binding = true;
        }
    }

    /*
     * Some early boot passes render a useful image and immediately cover it
     * with an opaque black quad before the copy command. Keep that completed
     * intermediate available as a presentation fallback; a later non-black
     * EFB copy still replaces it normally.
     */
    preserve_visible_efb();

    if (vertex_count > GX_MAX_VERTICES)
        vertex_count = GX_MAX_VERTICES;

    {
        uint32_t early_produced = 0u;
        uint32_t early_index_count = 0u;
        uint32_t early_clipped = 0u;

        if (!predecode_positions(
                command, vertex_data, vertex_bytes, vertex_count,
                &position_transform, &early_produced,
                &early_index_count, &early_clipped))
            return false;
        if (!triangle_batch_may_reach_viewport(early_index_count)) {
            g_stats.clipped_vertices += early_clipped;
            record_draw_state(early_produced, false,
                              has_texture_binding,
                              selected_texture_map);
            return true;
        }
    }

    for (vertex_index = 0; vertex_index < vertex_count; ++vertex_index) {
        GxVertex vertex;
        bool skip_vertex = false;
        uint32_t position_desc;
        uint32_t position_format;
        uint32_t position_count;
        uint32_t position_size;
        const uint8_t* position = NULL;
        uint32_t normal_desc;
        uint32_t normal_format;
        bool has_ntb;
        bool normal_index3;
        uint32_t normal_component;
        unsigned color;
        unsigned texture;

        memset(&vertex, 0, sizeof(vertex));
        vertex.color = 0xffffffffu;
        vertex.color1 = 0xffffffffu;

        if ((vcd_low & 1u) != 0u) {
            if (!reader_u8(&reader, &vertex.position_matrix))
                return false;
            vertex.position_matrix &= 0x3fu;
        }
        for (i = 0; i < 8; ++i) {
            uint8_t matrix_index;
            if ((vcd_low & (2u << i)) != 0u) {
                if (!reader_u8(&reader, &matrix_index))
                    return false;
                vertex.texture_matrix[i] = matrix_index & 0x3fu;
                vertex.texture_matrix_valid[i] = true;
            }
        }

        position_desc = bits(vcd_low, 9, 2);
        position_format = bits(g0, 1, 3);
        position_count = bits(g0, 0, 1) + 2u;
        position_size = component_size(position_format) * position_count;
        if (position_desc == 1u) {
            position = reader_take(&reader, position_size);
        } else if (position_desc == 2u || position_desc == 3u) {
            uint16_t index = 0;
            if (!reader_index(&reader, position_desc, &index))
                return false;
            skip_vertex = index == (position_desc == 2u ? 0xffu : 0xffffu);
            if (!skip_vertex) {
                const uint64_t address =
                    (uint64_t)g_cp[0xA0u] + (uint64_t)(g_cp[0xB0u] & 0xffu) * index;
                if (address > 0xffffffffu)
                    return false;
                position = guest_bytes((uint32_t)address, position_size);
            }
        }
        if (position_desc != 0u && position == NULL && !skip_vertex)
            return false;
        for (i = 0; i < position_count && position != NULL; ++i)
            vertex.position[i] = read_component(
                position + i * component_size(position_format), position_format, bits(g0, 4, 5));

        normal_desc = bits(vcd_low, 11, 2);
        normal_format = bits(g0, 10, 3);
        has_ntb = bits(g0, 9, 1) != 0u;
        normal_index3 = bits(g0, 31, 1) != 0u;
        normal_component = component_size(normal_format);
        if ((normal_desc == 2u || normal_desc == 3u) && has_ntb && normal_index3) {
            for (i = 0; i < 3u; ++i) {
                uint16_t index = 0;
                if (!reader_index(&reader, normal_desc, &index))
                    return false;
            }
        } else {
            const uint32_t normal_count = has_ntb ? 9u : 3u;
            const uint8_t* normal = attribute_data(
                &reader, 1u, normal_desc, (size_t)normal_component * normal_count);
            if (normal_desc != 0u && normal == NULL)
                return false;
            (void)normal;
        }

        for (color = 0; color < 2; ++color) {
            const uint32_t descriptor = bits(vcd_low, 13u + color * 2u, 2);
            const uint32_t format = bits(g0, 14u + color * 4u, 3);
            const uint8_t* source =
                attribute_data(&reader, 2u + color, descriptor, color_size(format));
            if (descriptor != 0u && source == NULL)
                return false;
            if (source != NULL) {
                if (color == 0u)
                    vertex.color = decode_color(source, format);
                else
                    vertex.color1 = decode_color(source, format);
            }
        }

        for (texture = 0; texture < 8; ++texture) {
            uint32_t format;
            uint32_t elements;
            const uint32_t descriptor = bits(vcd_high, texture * 2u, 2);
            const uint8_t* source;
            texture_format(g0, g1, g2, texture, &format, &elements);
            source = attribute_data(&reader, 4u + texture, descriptor,
                                    component_size(format) * (elements + 1u));
            if (descriptor != 0u && source == NULL)
                return false;
            if (source != NULL) {
                const uint32_t component = component_size(format);
                const uint32_t fraction =
                    texture_fraction(g0, g1, g2, texture);
                for (i = 0; i < elements + 1u && i < 2u; ++i) {
                    vertex.raw_texcoord[texture][i] =
                        read_component(source + i * component, format, fraction);
                }
                vertex.raw_texcoord_valid[texture] = true;
            }
        }

        vertex.color = apply_unlit_channel_state(vertex.color, 0u);
        vertex.color1 = apply_unlit_channel_state(vertex.color1, 1u);

        if (!skip_vertex) {
            transform_vertex(&vertex, &position_transform);
            g_vertices[produced++] = vertex;
        }
    }

    if (reader.offset != reader.size)
        return false;

    record_draw_state(produced, true, has_texture_binding,
                      selected_texture_map);
    add_triangle_indices(primitive, produced, &index_count);
    if (!triangle_batch_may_reach_viewport(index_count))
        return true;
    for (i = 0u; i < GX_TEXTURE_MAP_COUNT; ++i) {
        if (texture_map_used[i])
            prepare_texture(i, &draw_textures.maps[i]);
    }
    for (i = 0; i + 2u < index_count; i += 3u) {
        const GxVertex* a = &g_vertices[g_indices[i]];
        const GxVertex* b = &g_vertices[g_indices[i + 1u]];
        const GxVertex* c = &g_vertices[g_indices[i + 2u]];
        if (a->valid && b->valid && c->valid)
            fill_triangle(a, b, c, &draw_textures);
    }
    return true;
}

static void load_indexed_xf(uint8_t array, uint16_t index, uint16_t address, uint8_t size)
{
    uint8_t i;
    const uint32_t base = g_cp[0xA0u + array];
    const uint32_t stride = g_cp[0xB0u + array] & 0xffu;
    const uint64_t source = (uint64_t)base + (uint64_t)stride * index;
    if (g_cpu == NULL || array >= 16u || address >= GX_XF_COUNT)
        return;
    if (source > 0xffffffffu)
        return;
    if (size > GX_XF_COUNT - address)
        size = (uint8_t)(GX_XF_COUNT - address);
    {
        const uint8_t* direct = ppc_memory_pointer(
            g_cpu, (uint32_t)source, (uint32_t)size * 4u);
        if (direct != NULL) {
            for (i = 0; i < size; ++i) {
                g_xf[address + i] = gx_read_be32(direct + i * 4u);
                g_xf_written[address + i] = true;
            }
            return;
        }
    }
    for (i = 0; i < size; ++i) {
        g_xf[address + i] = mem_read32(g_cpu, (uint32_t)source + i * 4u);
        g_xf_written[address + i] = true;
    }
}

static void process_command_stream(const uint8_t* data, size_t size);

static void execute_display_list(uint32_t address, uint32_t size)
{
    const uint8_t* direct;
    uint8_t* buffer;
    size_t capacity;
    uint8_t depth;
    if (g_cpu == NULL || size == 0u || g_dl_depth >= GX_DL_MAX_DEPTH)
        return;
    depth = g_dl_depth;
    ++g_dl_depth;
    ++g_stats.display_lists;
    direct = ppc_memory_pointer(g_cpu, address, size);
    if (direct != NULL) {
        process_command_stream(direct, size);
        --g_dl_depth;
        return;
    }
    buffer = g_display_list_buffers[depth];
    capacity = g_display_list_capacities[depth];
    if (capacity < size) {
        uint8_t* resized = (uint8_t*)realloc(buffer, size);
        if (resized == NULL) {
            --g_dl_depth;
            return;
        }
        buffer = resized;
        g_display_list_buffers[depth] = buffer;
        g_display_list_capacities[depth] = size;
    }
    if (guest_copy_bytes(address, buffer, size))
        process_command_stream(buffer, size);
    --g_dl_depth;
}

static void load_bp(uint32_t packed)
{
    const uint8_t command = (uint8_t)(packed >> 24);
    uint32_t value = packed & 0x00ffffffu;
    ++g_stats.bp_writes;
    if (command == 0xfeu) {
        g_bp_mask = value;
        g_bp[command] = value;
        return;
    }
    value = (g_bp[command] & ~g_bp_mask) | (value & g_bp_mask);
    g_bp[command] = value;
    g_bp_mask = 0x00ffffffu;
    if (command >= 0xe0u && command <= 0xe7u) {
        const unsigned reg = (unsigned)(command - 0xe0u) / 2u;
        if ((value & (1u << 23)) == 0u) {
            if ((command & 1u) == 0u) {
                g_tev_color[reg][0] = (int32_t)(value << 21) >> 21;
                g_tev_color[reg][3] = (int32_t)((value >> 12u) << 21) >> 21;
            } else {
                g_tev_color[reg][2] = (int32_t)(value << 21) >> 21;
                g_tev_color[reg][1] = (int32_t)((value >> 12u) << 21) >> 21;
            }
        } else if ((command & 1u) == 0u) {
            g_tev_kcolor[reg][0] = (uint8_t)value;
            g_tev_kcolor[reg][3] = (uint8_t)(value >> 12u);
        } else {
            g_tev_kcolor[reg][2] = (uint8_t)value;
            g_tev_kcolor[reg][1] = (uint8_t)(value >> 12u);
        }
    }
    if (command == 0x65u) {
        const uint32_t source = (g_bp[0x64u] & 0x00ffffffu) << 5u;
        const uint32_t destination = bits(value, 0u, 10u) << 9u;
        size_t count = (size_t)bits(value, 10u, 11u) * 32u;
        size_t i;
        if (destination < GX_TMEM_SIZE) {
            if (count > GX_TMEM_SIZE - destination)
                count = GX_TMEM_SIZE - destination;
            for (i = 0u; i < count; ++i)
                g_tmem[destination + i] = texture_read8(source + (uint32_t)i);
        }
        {
            unsigned map;
            for (map = 0u; map < GX_TEXTURE_MAP_COUNT; ++map) {
                g_texture_cache[map].valid = false;
                g_texture_cache[map].needs_validation = false;
            }
        }
    }
    /* Explicit GX texture invalidation can make an unchanged descriptor stale. */
    if (command == 0x66u) {
        unsigned map;
        for (map = 0u; map < GX_TEXTURE_MAP_COUNT; ++map) {
            if (g_texture_cache[map].valid)
                g_texture_cache[map].needs_validation = true;
        }
    }
    if (command == 0x45u && (value & 3u) == 2u)
        g_finish_requested = true;
    if (command == 0x52u) {
        const size_t efb_size =
            (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
            sizeof(uint32_t);
        if ((value & (1u << 14)) != 0u) {
            if (g_efb != NULL && g_xfb != NULL) {
                const uint32_t colored = count_visible_pixels(g_efb);
                if (colored != 0u || !g_xfb_valid) {
                    memcpy(g_xfb, g_efb, efb_size);
                    g_stats.xfb_colored_pixels = colored;
                    g_xfb_valid = true;
                }
                ++g_stats.xfb_copies;
            }
            /*
             * GXCopyDisp is the retail command stream's completed-frame
             * boundary, equivalent to the linked port's post-FrameEnd hook.
             */
            finish_efb_texture_capture();
        } else {
            queue_efb_texture_capture();
        }
        const uint32_t color = (((g_bp[0x4fu] >> 8) & 0xffu) << 24) |
                               ((g_bp[0x50u] & 0xffu) << 16) |
                               (((g_bp[0x50u] >> 8) & 0xffu) << 8) |
                               (g_bp[0x4fu] & 0xffu);
        g_clear_red = (uint8_t)(color >> 24);
        g_clear_green = (uint8_t)(color >> 16);
        g_clear_blue = (uint8_t)(color >> 8);
        g_clear_alpha = (uint8_t)color;
        if ((value & (1u << 11)) != 0u)
            clear_efb_after_copy(color);
        ++g_stats.efb_copies;
        g_stats.video_started = true;
        g_efb_dirty = true;
    }
}

static size_t decode_one(const uint8_t* data, size_t available)
{
    const uint8_t command = data[0];
    if (command == 0x00u) {
        size_t count = 1;
        while (count < available && data[count] == 0u)
            ++count;
        return count;
    }
    if (command == 0x44u || command == 0x48u)
        return 1u;
    if (command == 0x08u) {
        if (available < 6u)
            return 0u;
        g_cp[data[1]] = gx_read_be32(data + 2);
        if (data[1] == 0x30u) {
            g_position_matrix_index = (uint8_t)(g_cp[data[1]] & 0x3fu);
            g_xf[0x1018u] = g_cp[data[1]];
            g_xf_written[0x1018u] = true;
        } else if (data[1] == 0x40u) {
            g_xf[0x1019u] = g_cp[data[1]];
            g_xf_written[0x1019u] = true;
        }
        return 6u;
    }
    if (command == 0x10u) {
        uint32_t header;
        uint32_t count;
        uint32_t address;
        uint32_t i;
        size_t size;
        if (available < 5u)
            return 0u;
        header = gx_read_be32(data + 1);
        count = ((header >> 16) & 0x0fu) + 1u;
        size = 5u + (size_t)count * 4u;
        if (available < size)
            return 0u;
        address = header & 0xffffu;
        for (i = 0; i < count && address + i < GX_XF_COUNT; ++i) {
            g_xf[address + i] = gx_read_be32(data + 5u + i * 4u);
            g_xf_written[address + i] = true;
        }
        if (address <= 0x1018u && address + count > 0x1018u)
            g_position_matrix_index = (uint8_t)(bits(g_xf[0x1018u], 0, 6));
        return size;
    }
    if (command == 0x20u || command == 0x28u || command == 0x30u || command == 0x38u) {
        uint32_t value;
        if (available < 5u)
            return 0u;
        value = gx_read_be32(data + 1);
        load_indexed_xf((uint8_t)(command / 8u + 8u), (uint16_t)(value >> 16),
                        (uint16_t)(value & 0xfffu),
                        (uint8_t)(((value >> 12) & 0xfu) + 1u));
        return 5u;
    }
    if (command == 0x40u) {
        if (available < 9u)
            return 0u;
        execute_display_list(gx_read_be32(data + 1) & ~31u,
                             gx_read_be32(data + 5) & ~31u);
        return 9u;
    }
    if (command == 0x61u) {
        if (available < 5u)
            return 0u;
        load_bp(gx_read_be32(data + 1));
        return 5u;
    }
    if (command >= 0x80u && command <= 0xbfu) {
        uint32_t stride;
        uint32_t count;
        size_t size;
        if (available < 3u)
            return 0u;
        stride = vertex_size(command & 7u);
        count = gx_read_be16(data + 1);
        if (stride != 0u && count > (GX_FIFO_CAPACITY - 3u) / stride) {
            ++g_stats.unknown_commands;
            return 1u;
        }
        size = 3u + (size_t)stride * count;
        if (available < size)
            return 0u;
        ++g_stats.draw_calls;
        if (!decode_and_draw(command, data + 3, size - 3u, (uint16_t)count))
            ++g_stats.decode_failures;
        return size;
    }
    ++g_stats.unknown_commands;
    return 1u;
}

static void process_command_stream(const uint8_t* data, size_t size)
{
    size_t consumed = 0;
    while (consumed < size) {
        const size_t command_size = decode_one(data + consumed, size - consumed);
        if (command_size == 0u) {
            ++g_stats.incomplete_commands;
            break;
        }
        consumed += command_size;
        ++g_stats.commands;
    }
}

static void process_fifo(void)
{
    size_t consumed = 0;
    while (consumed < g_fifo_size) {
        const size_t size = decode_one(g_fifo + consumed, g_fifo_size - consumed);
        if (size == 0u)
            break;
        consumed += size;
        ++g_stats.commands;
    }
    if (consumed != 0u) {
        g_fifo_size -= consumed;
        memmove(g_fifo, g_fifo + consumed, g_fifo_size);
    }
}

void smg3ds_gx_init(CPUState* cpu)
{
    g_cpu = cpu;
    g_fifo_size = 0;
    memset(g_cp, 0, sizeof(g_cp));
    memset(g_xf, 0, sizeof(g_xf));
    memset(g_xf_written, 0, sizeof(g_xf_written));
    memset(g_bp, 0, sizeof(g_bp));
    memset(g_tmem, 0, sizeof(g_tmem));
    memset(&g_stats, 0, sizeof(g_stats));
    g_finish_requested = false;
    memset(g_tev_color, 0, sizeof(g_tev_color));
    memset(g_tev_kcolor, 0, sizeof(g_tev_kcolor));
    g_bp_mask = 0x00ffffffu;
    g_clear_red = g_clear_green = g_clear_blue = 0;
    g_clear_alpha = 0xffu;
    g_dl_depth = 0;
    g_efb_dirty = false;
    g_xfb_valid = false;
    {
        unsigned map;
        for (map = 0u; map < GX_TEXTURE_MAP_COUNT; ++map) {
            g_texture_cache[map].valid = false;
            g_texture_cache[map].needs_validation = false;
            g_texture_cache[map].from_efb_capture = false;
        }
    }
    g_texture_read_page = UINT32_MAX;
    g_texture_read_pointer = NULL;
    g_position_matrix_index = 0u;
    memset(g_pointer_cache, 0, sizeof(g_pointer_cache));
    g_pointer_cache_next = 0u;
    if (g_efb == NULL)
        g_efb = (uint32_t*)malloc((size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                                  sizeof(uint32_t));
    if (g_xfb == NULL)
        g_xfb = (uint32_t*)malloc((size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                                  sizeof(uint32_t));
    if (g_zfb == NULL)
        g_zfb = (uint32_t*)malloc((size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                                  sizeof(uint32_t));
    if (g_efb_capture.pixels == NULL)
        g_efb_capture.pixels = (uint32_t*)malloc(
            (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
            sizeof(uint32_t));
    if (g_efb_capture.pending_pixels == NULL)
        g_efb_capture.pending_pixels = (uint32_t*)malloc(
            (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
            sizeof(uint32_t));
    memset(g_efb_capture.keys, 0, sizeof(g_efb_capture.keys));
    g_efb_capture.generation = 0u;
    g_efb_capture.pending_address = 0u;
    g_efb_capture.next_key = 0u;
    g_efb_capture.valid = false;
    g_efb_capture.pending = false;
    if (g_efb != NULL && g_zfb != NULL) {
        size_t i;
        clear_efb(0x000000ffu);
        for (i = 0; i < (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT; ++i)
            g_zfb[i] = 0x00ffffffu;
        g_stats.geometry_self_test_passed =
            rasterizer_self_test() && efb_capture_self_test();
        memset(g_efb, 0, (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                         sizeof(uint32_t));
        g_stats.rasterized_pixels = 0u;
        g_stats.colored_pixels = 0u;
        g_stats.xfb_colored_pixels = 0u;
        g_stats.depth_rejected_pixels = 0u;
        g_stats.triangles = 0u;
        g_stats.raster_ticks = 0u;
        for (i = 0; i < (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT; ++i)
            g_zfb[i] = 0x00ffffffu;
        g_stats.video_started = false;
        g_efb_dirty = false;
    }
    /* Identity-ish defaults so early draws are not all clipped. */
    g_xf[0] = 0x3f800000u;
    g_xf[5] = 0x3f800000u;
    g_xf[10] = 0x3f800000u;
    g_xf[0x101a] = 0x43a00000u; /* wd = 320 */
    g_xf[0x101b] = 0xc3700000u; /* ht = -240 */
    g_xf[0x101c] = 0x4b7fffffu; /* zRange = 16777215 */
    g_xf[0x101d] = 0x43a00000u; /* xOrig = 320 */
    g_xf[0x101e] = 0x43700000u; /* yOrig = 240 */
    g_xf[0x101f] = 0x4b7fffffu; /* farZ = 16777215 */
    g_xf[0x1020] = 0x3f800000u;
    g_xf[0x1022] = 0x3f800000u;
    g_xf[0x1024] = 0x3f800000u;
    g_xf[0x1026] = 1u; /* orthographic until the game loads a projection */
    g_stats.pica200_available = smg3ds_pica200_init();
}

void smg3ds_gx_shutdown(void) { smg3ds_pica200_shutdown(); }

void smg3ds_gx_fifo_write(uint64_t value, uint8_t size)
{
    uint8_t i;
    if (size != 1u && size != 2u && size != 4u && size != 8u)
        return;
    ++g_stats.fifo_writes;
    if (g_fifo_size + size > sizeof(g_fifo)) {
        g_fifo_size = 0;
        ++g_stats.fifo_overflows;
        ++g_stats.unknown_commands;
    }
    for (i = 0; i < size; ++i)
        g_fifo[g_fifo_size++] = (uint8_t)(value >> ((size - i - 1u) * 8u));
    g_stats.fifo_bytes += size;
    process_fifo();
}

bool smg3ds_gx_take_finish_request(void)
{
    const bool requested = g_finish_requested;
    g_finish_requested = false;
    return requested;
}

bool smg3ds_gx_present_top(void)
{
    uint16_t width;
    uint16_t height;
    uint8_t* framebuffer;
    const uint32_t* source = g_xfb_valid ? g_xfb : g_efb;
    const uint32_t clear = pack_rgba(g_clear_red, g_clear_green,
                                     g_clear_blue, g_clear_alpha);
    const Smg3dsPica200Stats* pica;
    uint16_t x;
    uint16_t y;

    /*
     * A texture copy can occur in a frame that has no GXCopyDisp. Publish it
     * here as the same fallback used by the linked port after frame submission.
     */
    finish_efb_texture_capture();
    if (g_stats.pica200_available && smg3ds_pica200_present(
            g_stats.video_started ? source : NULL,
            GX_EFB_WIDTH, GX_EFB_HEIGHT, clear)) {
        pica = smg3ds_pica200_get_stats();
        g_stats.pica_presented_frames = pica->presented_frames;
        g_stats.pica_texture_uploads = pica->texture_uploads;
        g_stats.pica_frame_failures = pica->frame_failures;
        g_stats.pica_uploaded_bytes = pica->uploaded_bytes;
        g_stats.pica200_available = pica->available;
        g_efb_dirty = false;
        return true;
    }
    pica = smg3ds_pica200_get_stats();
    g_stats.pica_presented_frames = pica->presented_frames;
    g_stats.pica_texture_uploads = pica->texture_uploads;
    g_stats.pica_frame_failures = pica->frame_failures;
    g_stats.pica_uploaded_bytes = pica->uploaded_bytes;
    g_stats.pica200_available = pica->available;

    framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    if (framebuffer == NULL)
        return false;
    if (width == 0u || height == 0u)
        return false;

    if (source == NULL || !g_stats.video_started) {
        const size_t pixels = (size_t)width * (size_t)height;
        size_t i;
        for (i = 0; i < pixels; ++i) {
            framebuffer[i * 3u] = g_clear_blue;
            framebuffer[i * 3u + 1u] = g_clear_green;
            framebuffer[i * 3u + 2u] = g_clear_red;
        }
        g_efb_dirty = false;
        return false;
    }

    /*
     * Refresh the active top backbuffer on every host presentation. The 3DS
     * framebuffer swap owns multiple backing allocations, so EFB/XFB dirty
     * state alone cannot prove that the allocation returned above already
     * contains the newest game frame. Skipping this copy made the bottom
     * cursor continue at the host rate while the top alternated stale buffers.
     *
     * The 200x120 software playfield is an exact half-resolution version of
     * the complete 400x240 top screen. libctru normally exposes the
     * framebuffer as portrait 240x400.
     */
    if (width == GX_TOP_SCREEN_HEIGHT &&
        height == GX_TOP_SCREEN_WIDTH) {
        for (y = 0; y < height; ++y) {
            const size_t source_x = (size_t)y >> 1;
            for (x = 0; x < width; ++x) {
                const size_t source_y = ((size_t)width - 1u - x) >> 1;
                const uint32_t rgba =
                    source[source_y * GX_EFB_WIDTH + source_x];
                uint8_t* pixel = framebuffer +
                    ((size_t)y * width + x) * 3u;
                pixel[0] = (uint8_t)(rgba >> 8);
                pixel[1] = (uint8_t)(rgba >> 16);
                pixel[2] = (uint8_t)(rgba >> 24);
            }
        }
    } else if (width == GX_TOP_SCREEN_WIDTH &&
               height == GX_TOP_SCREEN_HEIGHT) {
        for (y = 0; y < height; ++y) {
            const uint32_t* row =
                source + ((size_t)y >> 1) * GX_EFB_WIDTH;
            for (x = 0; x < width; ++x) {
                const uint32_t rgba = row[(size_t)x >> 1];
                uint8_t* pixel = framebuffer +
                    ((size_t)y * width + x) * 3u;
                pixel[0] = (uint8_t)(rgba >> 8);
                pixel[1] = (uint8_t)(rgba >> 16);
                pixel[2] = (uint8_t)(rgba >> 24);
            }
        }
    } else if (width < height) {
        const uint32_t logical_width = height;
        const uint32_t logical_height = width;
        uint32_t display_width =
            logical_height * GX_CONTENT_WIDTH / GX_CONTENT_HEIGHT;
        uint32_t display_height = logical_height;
        uint32_t x_origin;
        uint32_t y_origin = 0u;
        if (display_width > logical_width) {
            display_width = logical_width;
            display_height =
                logical_width * GX_CONTENT_HEIGHT / GX_CONTENT_WIDTH;
            y_origin = (logical_height - display_height) / 2u;
        }
        x_origin = (logical_width - display_width) / 2u;
        for (y = 0; y < height; ++y) {
            for (x = 0; x < width; ++x) {
                const uint32_t logical_x = y;
                const uint32_t logical_y = width - 1u - x;
                uint32_t rgba = 0x000000ffu;
                if (logical_x >= x_origin &&
                    logical_x < x_origin + display_width &&
                    logical_y >= y_origin &&
                    logical_y < y_origin + display_height) {
                    const uint32_t ex = GX_CONTENT_X +
                        (logical_x - x_origin) * GX_CONTENT_WIDTH /
                        display_width;
                    const uint32_t ey = GX_CONTENT_Y +
                        (logical_y - y_origin) * GX_CONTENT_HEIGHT /
                        display_height;
                    rgba = source[(size_t)ey * GX_EFB_WIDTH + ex];
                }
                uint8_t* pixel = framebuffer +
                    ((size_t)y * (size_t)width + (size_t)x) * 3u;
                pixel[0] = (uint8_t)(rgba >> 8);
                pixel[1] = (uint8_t)(rgba >> 16);
                pixel[2] = (uint8_t)(rgba >> 24);
            }
        }
    } else {
        uint32_t display_width =
            height * GX_CONTENT_WIDTH / GX_CONTENT_HEIGHT;
        uint32_t display_height = height;
        uint32_t x_origin;
        uint32_t y_origin = 0u;
        if (display_width > width) {
            display_width = width;
            display_height =
                width * GX_CONTENT_HEIGHT / GX_CONTENT_WIDTH;
            y_origin = (height - display_height) / 2u;
        }
        x_origin = (width - display_width) / 2u;
        for (y = 0; y < height; ++y) {
            for (x = 0; x < width; ++x) {
                uint32_t rgba = 0x000000ffu;
                if (x >= x_origin && x < x_origin + display_width &&
                    y >= y_origin && y < y_origin + display_height) {
                    const uint32_t ex = GX_CONTENT_X +
                        (x - x_origin) * GX_CONTENT_WIDTH / display_width;
                    const uint32_t ey = GX_CONTENT_Y +
                        (y - y_origin) * GX_CONTENT_HEIGHT / display_height;
                    rgba = source[(size_t)ey * GX_EFB_WIDTH + ex];
                }
                uint8_t* pixel = framebuffer +
                    ((size_t)y * (size_t)width + (size_t)x) * 3u;
                pixel[0] = (uint8_t)(rgba >> 8);
                pixel[1] = (uint8_t)(rgba >> 16);
                pixel[2] = (uint8_t)(rgba >> 24);
            }
        }
    }

    g_efb_dirty = false;
    return false;
}

static void put_bottom_pointer_pixel(uint8_t* framebuffer,
                                     uint16_t width, uint16_t height,
                                     int logical_x, int logical_y)
{
    int memory_x;
    int memory_y;
    int logical_width = width < height ? height : width;
    int logical_height = width < height ? width : height;

    if (logical_x < 0 || logical_y < 0 ||
        logical_x >= logical_width || logical_y >= logical_height)
        return;
    if (width < height) {
        memory_x = (int)width - 1 - logical_y;
        memory_y = logical_x;
    } else {
        memory_x = logical_x;
        memory_y = logical_y;
    }
    {
        uint8_t* pixel = framebuffer +
            ((size_t)memory_y * width + (size_t)memory_x) * 3u;
        pixel[0] = 0u;
        pixel[1] = 0xffu;
        pixel[2] = 0xffu;
    }
}

void smg3ds_gx_present_bottom(uint16_t touch_x, uint16_t touch_y,
                              bool touch_active)
{
    uint16_t width;
    uint16_t height;
    uint8_t* framebuffer = gfxGetFramebuffer(
        GFX_BOTTOM, GFX_LEFT, &width, &height);
    GxPointerCache* cache = NULL;
    uint32_t slot;
    int along;
    int across;

    if (framebuffer == NULL || width == 0u || height == 0u)
        return;
    if (touch_x >= GX_BOTTOM_SCREEN_WIDTH)
        touch_x = GX_BOTTOM_SCREEN_WIDTH - 1u;
    if (touch_y >= GX_BOTTOM_SCREEN_HEIGHT)
        touch_y = GX_BOTTOM_SCREEN_HEIGHT - 1u;

    for (slot = 0u; slot < GX_POINTER_CACHE_SLOTS; ++slot) {
        if (g_pointer_cache[slot].framebuffer == framebuffer) {
            cache = &g_pointer_cache[slot];
            break;
        }
    }
    if (cache == NULL) {
        cache = &g_pointer_cache[g_pointer_cache_next];
        g_pointer_cache_next =
            (g_pointer_cache_next + 1u) % GX_POINTER_CACHE_SLOTS;
        memset(cache, 0, sizeof(*cache));
        cache->framebuffer = framebuffer;
    } else if (cache->valid &&
               cache->touch_active == touch_active &&
               (!touch_active ||
                (cache->touch_x == touch_x && cache->touch_y == touch_y))) {
        return;
    }
    memset(framebuffer, 0, (size_t)width * height * 3u);
    if (touch_active) {
        for (along = -10; along <= 10; ++along) {
            for (across = -1; across <= 1; ++across) {
                put_bottom_pointer_pixel(
                    framebuffer, width, height,
                    (int)touch_x + along, (int)touch_y + across);
                put_bottom_pointer_pixel(
                    framebuffer, width, height,
                    (int)touch_x + across, (int)touch_y + along);
            }
        }
    }
    cache->touch_x = touch_x;
    cache->touch_y = touch_y;
    cache->touch_active = touch_active;
    cache->valid = true;
}

const Smg3dsGxStats* smg3ds_gx_get_stats(void)
{
    return &g_stats;
}
