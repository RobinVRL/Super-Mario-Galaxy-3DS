// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/gx_renderer.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    GX_FIFO_CAPACITY = 64 * 1024,
    GX_XF_COUNT = 0x1058,
    GX_MAX_VERTICES = 4096,
    GX_MAX_INDICES = 12288,
    GX_EFB_WIDTH = 640,
    GX_EFB_HEIGHT = 480,
    GX_ATTR_SCRATCH = 64,
    GX_DL_MAX_DEPTH = 16
};

typedef struct GxVertex {
    float position[3];
    float screen[2];
    uint32_t color;
    uint8_t position_matrix;
    bool valid;
} GxVertex;

static CPUState* g_cpu;
static uint8_t g_fifo[GX_FIFO_CAPACITY];
static size_t g_fifo_size;
static uint32_t g_cp[256];
static uint32_t g_xf[GX_XF_COUNT];
static uint32_t g_bp[256];
static uint32_t g_bp_mask;
static uint8_t g_clear_red;
static uint8_t g_clear_green;
static uint8_t g_clear_blue;
static uint8_t g_clear_alpha;
static Smg3dsGxStats g_stats;
static uint8_t g_dl_depth;
static uint8_t g_attr_scratch[GX_ATTR_SCRATCH];
static GxVertex g_vertices[GX_MAX_VERTICES];
static uint32_t g_indices[GX_MAX_INDICES];
static uint32_t* g_efb;
static uint32_t* g_xfb;
static bool g_xfb_valid;
static bool g_efb_dirty;
static uint8_t g_position_matrix_index;

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
    wd = bits_to_float(g_xf[0x101a]);
    ht = bits_to_float(g_xf[0x101b]);
    x_offset = (float)(bits(g_bp[0x59], 0, 9) << 1);
    y_offset = (float)(bits(g_bp[0x59], 10, 9) << 1);
    x_orig = bits_to_float(g_xf[0x101d]) - x_offset;
    y_orig = bits_to_float(g_xf[0x101e]) - y_offset;
    vertex->screen[0] = clip[0] * inv_w * wd + x_orig;
    vertex->screen[1] = clip[1] * inv_w * ht + y_orig;
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

static bool put_efb_pixel(int x, int y, uint32_t rgba)
{
    if (g_efb == NULL || x < 0 || y < 0 || x >= GX_EFB_WIDTH || y >= GX_EFB_HEIGHT)
        return false;
    g_efb[(size_t)y * (size_t)GX_EFB_WIDTH + (size_t)x] = rgba;
    g_efb_dirty = true;
    return true;
}

static void fill_triangle(const GxVertex* a, const GxVertex* b, const GxVertex* c)
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
    uint32_t color;
    uint32_t pixels = 0;

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

    color = a->color;
    for (y = (int)min_y; y <= (int)max_y; ++y) {
        for (x = (int)min_x; x <= (int)max_x; ++x) {
            const float px = (float)x + 0.5f;
            const float py = (float)y + 0.5f;
            const float w0 = (x1 - px) * (y2 - py) - (x2 - px) * (y1 - py);
            const float w1 = (x2 - px) * (y0 - py) - (x0 - px) * (y2 - py);
            const float w2 = (x0 - px) * (y1 - py) - (x1 - px) * (y0 - py);
            if ((w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f)) {
                if (put_efb_pixel(x, y, color))
                    ++pixels;
            }
        }
    }
    if (pixels != 0u) {
        ++g_stats.triangles;
        g_stats.rasterized_pixels += pixels;
        g_stats.video_started = true;
    }
}

static bool rasterizer_self_test(void)
{
    GxVertex a;
    GxVertex b;
    GxVertex c;
    const uint32_t before = g_stats.rasterized_pixels;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    a.screen[0] = 32.0f;
    a.screen[1] = 32.0f;
    b.screen[0] = 96.0f;
    b.screen[1] = 32.0f;
    c.screen[0] = 32.0f;
    c.screen[1] = 96.0f;
    a.color = b.color = c.color = 0x00ff00ffu;
    a.valid = b.valid = c.valid = true;
    fill_triangle(&a, &b, &c);
    return g_stats.rasterized_pixels > before;
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
            (void)source;
        }

        if (!skip_vertex) {
            transform_vertex(&vertex);
            g_vertices[produced++] = vertex;
        }
    }

    if (reader.offset != reader.size)
        return false;

    g_stats.vertices += produced;
    add_triangle_indices(primitive, produced, &index_count);
    for (i = 0; i + 2u < index_count; i += 3u) {
        const GxVertex* a = &g_vertices[g_indices[i]];
        const GxVertex* b = &g_vertices[g_indices[i + 1u]];
        const GxVertex* c = &g_vertices[g_indices[i + 2u]];
        if (a->valid && b->valid && c->valid)
            fill_triangle(a, b, c);
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
    if (command == 0x52u) {
        const size_t efb_size =
            (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
            sizeof(uint32_t);
        if ((value & (1u << 14)) != 0u && g_efb != NULL && g_xfb != NULL) {
            memcpy(g_xfb, g_efb, efb_size);
            g_xfb_valid = true;
        }
        const uint32_t color = ((g_bp[0x4fu] & 0xffu) << 24) |
                               (((g_bp[0x50u] >> 8) & 0xffu) << 16) |
                               ((g_bp[0x50u] & 0xffu) << 8) |
                               ((g_bp[0x4fu] >> 8) & 0xffu);
        g_clear_red = (uint8_t)(color >> 24);
        g_clear_green = (uint8_t)(color >> 16);
        g_clear_blue = (uint8_t)(color >> 8);
        g_clear_alpha = (uint8_t)color;
        if ((value & (1u << 11)) != 0u)
            clear_efb(color);
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
    memset(&g_stats, 0, sizeof(g_stats));
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
    if (g_efb != NULL) {
        clear_efb(0x000000ffu);
        g_stats.geometry_self_test_passed = rasterizer_self_test();
        memset(g_efb, 0, (size_t)GX_EFB_WIDTH * (size_t)GX_EFB_HEIGHT *
                         sizeof(uint32_t));
        g_stats.rasterized_pixels = 0u;
        g_stats.triangles = 0u;
        g_stats.video_started = false;
        g_efb_dirty = false;
    }
    /* Identity-ish defaults so early draws are not all clipped. */
    g_xf[0] = 0x3f800000u;
    g_xf[5] = 0x3f800000u;
    g_xf[10] = 0x3f800000u;
    g_xf[0x101a] = 0x43a00000u; /* wd = 320 */
    g_xf[0x101b] = 0xc3700000u; /* ht = -240 */
    g_xf[0x101d] = 0x43a00000u; /* xOrig = 320 */
    g_xf[0x101e] = 0x43700000u; /* yOrig = 240 */
    g_xf[0x1020] = 0x3f800000u;
    g_xf[0x1022] = 0x3f800000u;
    g_xf[0x1024] = 0x3f800000u;
    g_xf[0x1026] = 1u; /* orthographic until the game loads a projection */
}

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

void smg3ds_gx_present_top(void)
{
    uint16_t width;
    uint16_t height;
    uint8_t* framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    const uint32_t* source = g_xfb_valid ? g_xfb : g_efb;
    uint16_t x;
    uint16_t y;

    if (framebuffer == NULL)
        return;
    if (width == 0u || height == 0u)
        return;

    if (source == NULL || (!g_efb_dirty && !g_stats.video_started)) {
        const size_t pixels = (size_t)width * (size_t)height;
        size_t i;
        for (i = 0; i < pixels; ++i) {
            framebuffer[i * 3u] = g_clear_blue;
            framebuffer[i * 3u + 1u] = g_clear_green;
            framebuffer[i * 3u + 2u] = g_clear_red;
        }
        return;
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
}
const Smg3dsGxStats* smg3ds_gx_get_stats(void)
{
    return &g_stats;
}



