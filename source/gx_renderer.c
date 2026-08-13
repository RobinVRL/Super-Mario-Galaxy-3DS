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
    GX_EFB_WIDTH = 400,
    GX_EFB_HEIGHT = 240,
    GX_LOGICAL_EFB_WIDTH = 640,
    GX_LOGICAL_EFB_HEIGHT = 480,
    GX_MAX_TEXTURE_DIMENSION = 1024,
    GX_TMEM_SIZE = 1024 * 1024,
    GX_ATTR_SCRATCH = 64,
    GX_DL_MAX_DEPTH = 16
};

typedef struct GxVertex {
    float position[3];
    float screen[2];
    float depth;
    float inv_w;
    float texcoord[2];
    uint32_t color;
    uint8_t position_matrix;
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
    bool valid;
} GxTexture;

static CPUState* g_cpu;
static uint8_t g_fifo[GX_FIFO_CAPACITY];
static size_t g_fifo_size;
static uint32_t g_cp[256];
static uint32_t g_xf[GX_XF_COUNT];
static uint32_t g_bp[256];
static int32_t g_tev_color[4][4];
static uint32_t g_bp_mask;
static uint8_t g_clear_red;
static uint8_t g_clear_green;
static uint8_t g_clear_blue;
static uint8_t g_clear_alpha;
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
static uint32_t* g_texture_pixels;
static size_t g_texture_capacity;
static bool g_xfb_valid;
static bool g_efb_dirty;
static uint8_t g_position_matrix_index;

static bool color_is_visible(uint32_t rgba)
{
    return (rgba & 0xffffff00u) != 0u;
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

static const uint8_t* guest_bytes(uint32_t address, uint32_t size)
{
    uint32_t i;
    if (g_cpu == NULL || size == 0u || size > sizeof(g_attr_scratch))
        return NULL;
    for (i = 0; i < size; ++i)
        g_attr_scratch[i] = mem_read8(g_cpu, address + i);
    return g_attr_scratch;
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
    return g_cpu != NULL ? mem_read8(g_cpu, address) : 0u;
}

static uint16_t texture_read16(uint32_t address)
{
    return (uint16_t)(((uint16_t)texture_read8(address) << 8) |
                      texture_read8(address + 1u));
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
        return pack_rgba(intensity, intensity, intensity, 255u);
    case 1u:
        blocks_x = (width + 7u) / 8u;
        block = (y / 4u) * blocks_x + x / 8u;
        address = base + block * 32u + (y & 3u) * 8u + (x & 7u);
        intensity = texture_read8(address);
        return pack_rgba(intensity, intensity, intensity, 255u);
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

static uint32_t next_power_of_two(uint32_t value)
{
    uint32_t result = 1u;
    while (result < value && result < GX_MAX_TEXTURE_DIMENSION)
        result <<= 1u;
    return result;
}

static bool prepare_texture(uint32_t map, GxTexture* texture)
{
    const uint32_t slot = map & 3u;
    const bool high_bank = (map & 4u) != 0u;
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
    const uint32_t storage_width = next_power_of_two(width);
    const uint32_t storage_height = next_power_of_two(height);
    const size_t pixel_count = (size_t)storage_width * storage_height;
    uint32_t* resized;
    uint32_t y;

    memset(texture, 0, sizeof(*texture));
    g_stats.last_texture_width = width;
    g_stats.last_texture_height = height;
    g_stats.last_texture_storage_width = storage_width;
    g_stats.last_texture_storage_height = storage_height;
    g_stats.last_texture_format = format;
    g_stats.last_texture_base = (image3 & 0x00ffffffu) << 5u;

    if (g_cpu == NULL || width == 0u || height == 0u ||
        width > GX_MAX_TEXTURE_DIMENSION ||
        height > GX_MAX_TEXTURE_DIMENSION ||
        storage_width < width || storage_height < height)
        return false;
    if ((format > 6u && (format < 8u || format > 10u) && format != 14u) ||
        ((format == 8u || format == 9u || format == 10u) &&
         tlut_format > 2u)) {
        ++g_stats.unsupported_texture_formats;
        return false;
    }
    if (pixel_count > g_texture_capacity) {
        resized = (uint32_t*)realloc(g_texture_pixels,
                                     pixel_count * sizeof(uint32_t));
        if (resized == NULL)
            return false;
        g_texture_pixels = resized;
        g_texture_capacity = pixel_count;
    }

    for (y = 0u; y < storage_height; ++y) {
        const uint32_t source_y = y < height ? y : height - 1u;
        uint32_t x;
        for (x = 0u; x < storage_width; ++x) {
            const uint32_t source_x = x < width ? x : width - 1u;
            g_texture_pixels[(size_t)y * storage_width + x] =
                decode_texture_texel(g_stats.last_texture_base, width, format,
                                     tlut_base, tlut_format,
                                     source_x, source_y);
        }
    }

    texture->width = width;
    texture->height = height;
    texture->storage_width = storage_width;
    texture->storage_height = storage_height;
    texture->format = format;
    texture->wrap_s = bits(mode0, 0u, 2u);
    texture->wrap_t = bits(mode0, 2u, 2u);
    texture->pixels = g_texture_pixels;
    texture->valid = true;
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
    const int x = wrap_texture_coordinate(
        (int)floorf(s * (float)texture->width), texture->width,
        texture->wrap_s);
    const int y = wrap_texture_coordinate(
        (int)floorf(t * (float)texture->height), texture->height,
        texture->wrap_t);
    ++g_stats.texture_samples;
    return texture->pixels[(size_t)y * texture->storage_width + (size_t)x];
}

static uint32_t rgba_channel(uint32_t rgba, unsigned channel)
{
    return (rgba >> (24u - channel * 8u)) & 0xffu;
}
static uint32_t tev_register_channel(unsigned reg, unsigned channel)
{
    int32_t value = g_tev_color[reg][channel];
    if (value < 0)
        value = 0;
    if (value > 255)
        value = 255;
    return (uint32_t)value;
}


static uint32_t tev_color_argument(uint32_t argument, unsigned channel,
                                   uint32_t previous, uint32_t texture,
                                   uint32_t raster)
{
    switch (argument & 15u) {
    case 0u: return rgba_channel(previous, channel);
    case 1u: return rgba_channel(previous, 3u);
    case 8u: return rgba_channel(texture, channel);
    case 2u: return tev_register_channel(1u, channel);
    case 3u: return tev_register_channel(1u, 3u);
    case 4u: return tev_register_channel(2u, channel);
    case 5u: return tev_register_channel(2u, 3u);
    case 6u: return tev_register_channel(3u, channel);
    case 7u: return tev_register_channel(3u, 3u);
    case 9u: return rgba_channel(texture, 3u);
    case 10u: return rgba_channel(raster, channel);
    case 11u: return rgba_channel(raster, 3u);
    case 12u: return 255u;
    case 13u: return 128u;
    case 14u: return 255u;
    default: return 0u;
    }
}

static uint32_t tev_alpha_argument(uint32_t argument, uint32_t previous,
                                   uint32_t texture, uint32_t raster)
{
    switch (argument & 7u) {
    case 0u: return rgba_channel(previous, 3u);
    case 4u: return rgba_channel(texture, 3u);
    case 5u: return rgba_channel(raster, 3u);
    case 6u: return 255u;
    case 1u: return tev_register_channel(1u, 3u);
    case 2u: return tev_register_channel(2u, 3u);
    case 3u: return tev_register_channel(3u, 3u);
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

static uint32_t shade_fragment(uint32_t raster, uint32_t texture)
{
    const uint32_t color_env = g_bp[0xc0u];
    const uint32_t alpha_env = g_bp[0xc1u];
    uint32_t previous = raster;
    uint32_t result = 0u;
    unsigned channel;

    for (channel = 0u; channel < 3u; ++channel) {
        const uint32_t d = tev_color_argument(
            bits(color_env, 0u, 4u), channel, previous, texture, raster);
        const uint32_t c = tev_color_argument(
            bits(color_env, 4u, 4u), channel, previous, texture, raster);
        const uint32_t b = tev_color_argument(
            bits(color_env, 8u, 4u), channel, previous, texture, raster);
        const uint32_t a = tev_color_argument(
            bits(color_env, 12u, 4u), channel, previous, texture, raster);
        result |= tev_combine(a, b, c, d, bits(color_env, 16u, 2u),
                              bits(color_env, 18u, 1u) != 0u,
                              bits(color_env, 20u, 2u))
                  << (24u - channel * 8u);
    }

    {
        const uint32_t d = tev_alpha_argument(
            bits(alpha_env, 4u, 3u), previous, texture, raster);
        const uint32_t c = tev_alpha_argument(
            bits(alpha_env, 7u, 3u), previous, texture, raster);
        const uint32_t b = tev_alpha_argument(
            bits(alpha_env, 10u, 3u), previous, texture, raster);
        const uint32_t a = tev_alpha_argument(
            bits(alpha_env, 13u, 3u), previous, texture, raster);
        result |= tev_combine(a, b, c, d, bits(alpha_env, 16u, 2u),
                              bits(alpha_env, 18u, 1u) != 0u,
                              bits(alpha_env, 20u, 2u));
    }
    return result;
}

static void transform_vertex(GxVertex* vertex)
{
    uint32_t matrix_index = vertex->position_matrix;
    float eye[4];
    float clip[4];
    float raw[6];
    float matrix[12];
    unsigned i;
    uint32_t projection_type;
    float inv_w;
    float wd;
    float ht;
    float x_orig;
    float y_orig;
    float x_offset;
    float y_offset;

    if ((g_cp[0x50] & 1u) == 0u)
        matrix_index = g_position_matrix_index;

    for (i = 0; i < 12; ++i) {
        const uint32_t address = matrix_index * 4u + i;
        matrix[i] = address < GX_XF_COUNT ? bits_to_float(g_xf[address]) :
                                            (i == 0u || i == 5u || i == 10u ? 1.0f : 0.0f);
    }

    eye[0] = matrix[0] * vertex->position[0] + matrix[1] * vertex->position[1] +
             matrix[2] * vertex->position[2] + matrix[3];
    eye[1] = matrix[4] * vertex->position[0] + matrix[5] * vertex->position[1] +
             matrix[6] * vertex->position[2] + matrix[7];
    eye[2] = matrix[8] * vertex->position[0] + matrix[9] * vertex->position[1] +
             matrix[10] * vertex->position[2] + matrix[11];
    eye[3] = 1.0f;

    for (i = 0; i < 6; ++i)
        raw[i] = bits_to_float(g_xf[0x1020u + i]);
    projection_type = g_xf[0x1026];

    if (projection_type == 0u) {
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
        ++g_stats.clipped_vertices;
        return;
    }
    inv_w = 1.0f / clip[3];
    vertex->inv_w = inv_w;
    wd = bits_to_float(g_xf[0x101a]);
    ht = bits_to_float(g_xf[0x101b]);
    x_offset = (float)(bits(g_bp[0x59], 0, 9) << 1);
    y_offset = (float)(bits(g_bp[0x59], 10, 9) << 1);
    x_orig = bits_to_float(g_xf[0x101d]) - x_offset;
    y_orig = bits_to_float(g_xf[0x101e]) - y_offset;
    vertex->screen[0] = (clip[0] * inv_w * wd + x_orig) *
                        ((float)GX_EFB_WIDTH /
                         (float)GX_LOGICAL_EFB_WIDTH);
    vertex->screen[1] = (clip[1] * inv_w * ht + y_orig) *
                        ((float)GX_EFB_HEIGHT /
                         (float)GX_LOGICAL_EFB_HEIGHT);
    vertex->depth = clip[2] * inv_w * bits_to_float(g_xf[0x101cu]) +
                    bits_to_float(g_xf[0x101fu]);
    vertex->valid = true;
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

static uint32_t clamp_depth(float depth)
{
    if (!isfinite(depth) || depth <= 0.0f)
        return 0u;
    if (depth >= 16777215.0f)
        return 0x00ffffffu;
    return (uint32_t)(depth + 0.5f);
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
        g_efb_dirty = true;
    }
    return true;
}

static void fill_triangle(const GxVertex* a, const GxVertex* b,
                          const GxVertex* c, const GxTexture* texture)
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
    int x;
    int y;
    uint32_t pixels = 0u;
    uint32_t colored_pixels = 0u;
    const uint32_t z_mode = g_bp[0x40u];
    const bool depth_test = (z_mode & 1u) != 0u;
    const bool depth_update = (z_mode & (1u << 4)) != 0u;
    const uint32_t depth_function = bits(z_mode, 1, 3);

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

    for (y = (int)min_y; y <= (int)max_y; ++y) {
        for (x = (int)min_x; x <= (int)max_x; ++x) {
            const float px = (float)x + 0.5f;
            const float py = (float)y + 0.5f;
            const float w0 = (x1 - px) * (y2 - py) - (x2 - px) * (y1 - py);
            const float w1 = (x2 - px) * (y0 - py) - (x0 - px) * (y2 - py);
            const float w2 = (x0 - px) * (y1 - py) - (x1 - px) * (y0 - py);
            if ((w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f)) {
                const size_t offset =
                    (size_t)y * (size_t)GX_EFB_WIDTH + (size_t)x;
                const uint32_t depth = clamp_depth(
                    (w0 * a->depth + w1 * b->depth + w2 * c->depth) / area);
                uint32_t color = a->color;
                if (texture != NULL && texture->valid) {
                    const float reciprocal_w =
                        w0 * a->inv_w + w1 * b->inv_w + w2 * c->inv_w;
                    if (isfinite(reciprocal_w) && fabsf(reciprocal_w) > 1.0e-8f) {
                        const float s =
                            (w0 * a->texcoord[0] * a->inv_w +
                             w1 * b->texcoord[0] * b->inv_w +
                             w2 * c->texcoord[0] * c->inv_w) / reciprocal_w;
                        const float t =
                            (w0 * a->texcoord[1] * a->inv_w +
                             w1 * b->texcoord[1] * b->inv_w +
                             w2 * c->texcoord[1] * c->inv_w) / reciprocal_w;
                        color = shade_fragment(a->color, sample_texture(texture, s, t));
                    }
                }
                if (depth_test && g_zfb != NULL &&
                    !depth_compare(depth, g_zfb[offset], depth_function)) {
                    ++g_stats.depth_rejected_pixels;
                    continue;
                }
                if (depth_update && g_zfb != NULL)
                    g_zfb[offset] = depth;
                if (put_efb_pixel(x, y, color)) {
                    ++pixels;
                    if (color_is_visible(color))
                        ++colored_pixels;
                }
            }
        }
    }
    if (pixels != 0u) {
        ++g_stats.triangles;
        g_stats.rasterized_pixels += pixels;
        g_stats.colored_pixels += colored_pixels;
        g_stats.video_started = true;
    }
}

static bool rasterizer_self_test(void)
{
    GxVertex a;
    GxVertex b;
    GxVertex c;
    const uint32_t before = g_stats.rasterized_pixels;
    const uint32_t old_blend_mode = g_bp[0x41u];
    const uint32_t old_z_mode = g_bp[0x40u];
    const size_t sample = (size_t)40u * GX_EFB_WIDTH + 40u;
    uint32_t rejected_before;
    uint32_t first_sample;
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
    a.color = b.color = c.color = 0x00ff00ffu;
    a.valid = b.valid = c.valid = true;
    g_bp[0x41u] = (1u << 3) | (1u << 4);
    g_bp[0x40u] = 1u | (3u << 1) | (1u << 4);
    for (i = 0; i < (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT; ++i)
        g_zfb[i] = 0x00ffffffu;
    fill_triangle(&a, &b, &c, NULL);
    first_sample = g_efb[sample];
    rejected_before = g_stats.depth_rejected_pixels;
    a.depth = b.depth = c.depth = 200.0f;
    a.color = b.color = c.color = 0xff0000ffu;
    fill_triangle(&a, &b, &c, NULL);
    g_bp[0x41u] = old_blend_mode;
    g_bp[0x40u] = old_z_mode;
    return g_stats.rasterized_pixels > before &&
           first_sample == 0x00ff00ffu && g_efb[sample] == first_sample &&
           g_stats.depth_rejected_pixels > rejected_before;
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
    GxTexture texture_state;
    const GxTexture* active_texture = NULL;

    if (vertex_count > GX_MAX_VERTICES)
        vertex_count = GX_MAX_VERTICES;

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

        if ((vcd_low & 1u) != 0u) {
            if (!reader_u8(&reader, &vertex.position_matrix))
                return false;
            vertex.position_matrix &= 0x3fu;
        }
        for (i = 0; i < 8; ++i) {
            uint8_t unused;
            if ((vcd_low & (2u << i)) != 0u) {
                if (!reader_u8(&reader, &unused))
                    return false;
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
            if (source != NULL && color == 0u)
                vertex.color = decode_color(source, format);
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
            if (source != NULL && texture == 0u) {
                const uint32_t component = component_size(format);
                const uint32_t fraction = bits(g0, 25u, 5u);
                for (i = 0; i < elements + 1u && i < 2u; ++i) {
                    vertex.texcoord[i] =
                        read_component(source + i * component, format, fraction);
                }
            }
        }

        if (!skip_vertex) {
            transform_vertex(&vertex);
            g_vertices[produced++] = vertex;
        }
    }

    if (reader.offset != reader.size)
        return false;

    g_stats.vertices += produced;
    g_stats.last_z_mode = g_bp[0x40u];
    g_stats.last_blend_mode = g_bp[0x41u];
    g_stats.last_gen_mode = g_bp[0x00u];
    g_stats.last_tev_order = g_bp[0x28u];
    g_stats.last_tev_color_env = g_bp[0xc0u];
    g_stats.last_tev_alpha_env = g_bp[0xc1u];
    if ((g_bp[0x41u] & (1u << 3)) != 0u)
        ++g_stats.color_write_draws;
    if ((vcd_high & 3u) != 0u) {
        ++g_stats.textured_draws;
        g_stats.last_texture_image0 = g_bp[0x88u];
        g_stats.last_texture_image3 = g_bp[0x94u];
        g_stats.textured_gen_mode = g_bp[0x00u];
        g_stats.textured_tev_order = g_bp[0x28u];
        g_stats.textured_tev_color_env = g_bp[0xc0u];
        g_stats.textured_tev_alpha_env = g_bp[0xc1u];
    }
    if (produced != 0u)
        g_stats.last_vertex_color = g_vertices[0].color;
    memset(&texture_state, 0, sizeof(texture_state));
    {
        const uint32_t tev_order = g_bp[0x28u];
        const bool texture_enabled = (tev_order & (1u << 6)) != 0u;
        const uint32_t texture_map = bits(tev_order, 0u, 3u);
        if (texture_enabled && (vcd_high & 3u) != 0u &&
            prepare_texture(texture_map, &texture_state)) {
            active_texture = &texture_state;
        }
    }
    add_triangle_indices(primitive, produced, &index_count);
    for (i = 0; i + 2u < index_count; i += 3u) {
        const GxVertex* a = &g_vertices[g_indices[i]];
        const GxVertex* b = &g_vertices[g_indices[i + 1u]];
        const GxVertex* c = &g_vertices[g_indices[i + 2u]];
        if (a->valid && b->valid && c->valid)
            fill_triangle(a, b, c, active_texture);
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
    for (i = 0; i < size; ++i)
        g_xf[address + i] = mem_read32(g_cpu, (uint32_t)source + i * 4u);
}

static void process_command_stream(const uint8_t* data, size_t size);

static void execute_display_list(uint32_t address, uint32_t size)
{
    uint8_t* buffer;
    uint32_t i;
    if (g_cpu == NULL || size == 0u || g_dl_depth >= GX_DL_MAX_DEPTH)
        return;
    buffer = (uint8_t*)malloc(size);
    if (buffer == NULL)
        return;
    for (i = 0; i < size; ++i)
        buffer[i] = mem_read8(g_cpu, address + i);
    ++g_dl_depth;
    ++g_stats.display_lists;
    process_command_stream(buffer, size);
    --g_dl_depth;
    free(buffer);
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
    if (command >= 0xe0u && command <= 0xe7u &&
        (value & (1u << 23)) == 0u) {
        const unsigned reg = (unsigned)(command - 0xe0u) / 2u;
        if ((command & 1u) == 0u) {
            g_tev_color[reg][0] = (int32_t)(value << 21) >> 21;
            g_tev_color[reg][3] = (int32_t)((value >> 12u) << 21) >> 21;
        } else {
            g_tev_color[reg][2] = (int32_t)(value << 21) >> 21;
            g_tev_color[reg][1] = (int32_t)((value >> 12u) << 21) >> 21;
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
    }
    if (command == 0x45u && (value & 3u) == 2u)
        g_finish_requested = true;
    if (command == 0x52u) {
        const size_t efb_size =
            (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
            sizeof(uint32_t);
        if ((value & (1u << 14)) != 0u && g_efb != NULL && g_xfb != NULL) {
            size_t i;
            uint32_t colored = 0u;
            memcpy(g_xfb, g_efb, efb_size);
            for (i = 0; i < (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT; ++i) {
                if (color_is_visible(g_xfb[i]))
                    ++colored;
            }
            g_stats.xfb_colored_pixels = colored;
            g_xfb_valid = true;
            ++g_stats.xfb_copies;
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
        if (data[1] == 0x30u)
            g_position_matrix_index = (uint8_t)(g_cp[data[1]] & 0x3fu);
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
        for (i = 0; i < count && address + i < GX_XF_COUNT; ++i)
            g_xf[address + i] = gx_read_be32(data + 5u + i * 4u);
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
    memset(g_bp, 0, sizeof(g_bp));
    memset(g_tmem, 0, sizeof(g_tmem));
    memset(&g_stats, 0, sizeof(g_stats));
    g_finish_requested = false;
    memset(g_tev_color, 0, sizeof(g_tev_color));
    g_bp_mask = 0x00ffffffu;
    g_clear_red = g_clear_green = g_clear_blue = 0;
    g_clear_alpha = 0xffu;
    g_dl_depth = 0;
    g_efb_dirty = false;
    g_xfb_valid = false;
    g_position_matrix_index = 0u;
    if (g_efb == NULL)
        g_efb = (uint32_t*)malloc((size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                                  sizeof(uint32_t));
    if (g_xfb == NULL)
        g_xfb = (uint32_t*)malloc((size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                                  sizeof(uint32_t));
    if (g_zfb == NULL)
        g_zfb = (uint32_t*)malloc((size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                                  sizeof(uint32_t));
    if (g_efb != NULL && g_zfb != NULL) {
        size_t i;
        clear_efb(0x000000ffu);
        for (i = 0; i < (size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT; ++i)
            g_zfb[i] = 0x00ffffffu;
        g_stats.geometry_self_test_passed = rasterizer_self_test();
        memset(g_efb, 0, (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                         sizeof(uint32_t));
        g_stats.rasterized_pixels = 0u;
        g_stats.colored_pixels = 0u;
        g_stats.xfb_colored_pixels = 0u;
        g_stats.depth_rejected_pixels = 0u;
        g_stats.triangles = 0u;
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

    if (source == NULL || (!g_efb_dirty && !g_stats.video_started)) {
        const size_t pixels = (size_t)width * (size_t)height;
        size_t i;
        for (i = 0; i < pixels; ++i) {
            framebuffer[i * 3u] = g_clear_blue;
            framebuffer[i * 3u + 1u] = g_clear_green;
            framebuffer[i * 3u + 2u] = g_clear_red;
        }
        return false;
    }

    /*
     * libctru may report portrait-oriented framebuffer memory (240x400) on top-screen.
     * In portrait mode, the backing memory is transposed; keep that mapping explicit.
     * In landscape mode, map directly.
     */
    if (width < height) {
        for (y = 0; y < height; ++y) {
            for (x = 0; x < width; ++x) {
                const int ex = (int)y * GX_EFB_WIDTH / (int)height;
                const int ey = ((int)width - 1 - (int)x) * GX_EFB_HEIGHT /
                               (int)width;
                const uint32_t rgba =
                    source[(size_t)ey * (size_t)GX_EFB_WIDTH + (size_t)ex];
                uint8_t* pixel = framebuffer +
                    ((size_t)y * (size_t)width + (size_t)x) * 3u;
                pixel[0] = (uint8_t)(rgba >> 8);
                pixel[1] = (uint8_t)(rgba >> 16);
                pixel[2] = (uint8_t)(rgba >> 24);
            }
        }
    } else {
        for (y = 0; y < height; ++y) {
            for (x = 0; x < width; ++x) {
                const int ex = (int)x * GX_EFB_WIDTH / (int)width;
                const int ey = (int)y * GX_EFB_HEIGHT / (int)height;
                const uint32_t rgba =
                    source[(size_t)ey * (size_t)GX_EFB_WIDTH + (size_t)ex];
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
const Smg3dsGxStats* smg3ds_gx_get_stats(void)
{
    return &g_stats;
}
