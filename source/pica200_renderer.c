// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/pica200_renderer.h"

#include <3ds.h>
#include <citro3d.h>

#include "pica200_vshader_shbin.h"

#include <string.h>

enum {
    PICA_SCREEN_WIDTH = 400,
    PICA_SCREEN_HEIGHT = 240,
    PICA_TEXTURE_WIDTH = 256,
    PICA_TEXTURE_HEIGHT = 128
};

#define PICA_DISPLAY_TRANSFER_FLAGS                                      \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |              \
     GX_TRANSFER_RAW_COPY(0) |                                           \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                      \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                      \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define PICA_TEXTURE_UPLOAD_FLAGS                                       \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |              \
     GX_TRANSFER_RAW_COPY(0) |                                           \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                      \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |                     \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static DVLB_s* g_dvlb;
static shaderProgram_s g_program;
static C3D_RenderTarget* g_target;
static C3D_Tex g_frame_texture;
static uint32_t* g_upload_buffer;
static C3D_Mtx g_projection;
static int g_projection_location = -1;
static bool g_citro3d_initialized;
static bool g_program_initialized;
static bool g_texture_initialized;
static Smg3dsPica200Stats g_stats;

static uint32_t pica_rgba8(uint32_t rgba)
{
    /*
     * The GX renderer and PICA GPU_RGBA8 texture words both use
     * 0xRRGGBBAA. Keep the word intact: converting it to C2D_Color32's
     * little-endian byte layout swaps red and blue a second time.
     */
    return rgba;
}

static bool pica_rgba8_self_test(void)
{
    static const uint32_t calibration_colors[] = {
        0xff0000ffu, /* red */
        0x00ff00ffu, /* green */
        0x0000ffffu, /* blue */
        0x12345678u, /* catches accidental channel permutations */
    };
    size_t index;

    for (index = 0u;
         index < sizeof(calibration_colors) / sizeof(calibration_colors[0]);
         ++index) {
        if (pica_rgba8(calibration_colors[index]) != calibration_colors[index])
            return false;
    }
    return true;
}

static void upload_frame(const uint32_t* pixels,
                         uint16_t width, uint16_t height,
                         uint32_t clear_rgba)
{
    const size_t pixel_count =
        (size_t)PICA_TEXTURE_WIDTH * PICA_TEXTURE_HEIGHT;
    const uint32_t clear = pica_rgba8(clear_rgba);
    size_t index;
    uint32_t y;

    for (index = 0u; index < pixel_count; ++index)
        g_upload_buffer[index] = clear;
    if (pixels != NULL) {
        for (y = 0u; y < height; ++y) {
            memcpy(g_upload_buffer + (size_t)y * PICA_TEXTURE_WIDTH,
                   pixels + (size_t)y * width,
                   (size_t)width * sizeof(uint32_t));
        }
    }

    /*
     * Let the 3DS display-transfer engine perform the native linear-to-tiled
     * conversion. Besides being faster, this avoids reproducing PICA's texel
     * swizzle in CPU code, where a layout mismatch separates color planes.
     */
    GSPGPU_FlushDataCache(g_upload_buffer,
                          pixel_count * sizeof(uint32_t));
    C3D_SyncDisplayTransfer(
        (u32*)g_upload_buffer,
        GX_BUFFER_DIM(PICA_TEXTURE_WIDTH, PICA_TEXTURE_HEIGHT),
        (u32*)g_frame_texture.data,
        GX_BUFFER_DIM(PICA_TEXTURE_WIDTH, PICA_TEXTURE_HEIGHT),
        PICA_TEXTURE_UPLOAD_FLAGS);
    ++g_stats.texture_uploads;
    g_stats.uploaded_bytes += pixel_count * sizeof(uint32_t);
}

static void draw_fullscreen(uint16_t width, uint16_t height)
{
    const float max_s = (float)width / (float)PICA_TEXTURE_WIDTH;
    /*
     * Display transfer places a linear image at the top of the tiled
     * power-of-two texture. Select that top-aligned subtexture so the eight
     * padding rows below a 120-pixel frame never enter the displayed image.
     */
    const float min_t = 1.0f -
        (float)height / (float)PICA_TEXTURE_HEIGHT;

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_projection_location,
                     &g_projection);
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
        C3D_ImmSendAttrib(0.0f, 0.0f, 0.5f, 0.0f);
        C3D_ImmSendAttrib(0.0f, min_t, 0.0f, 0.0f);

        C3D_ImmSendAttrib(0.0f, (float)PICA_SCREEN_HEIGHT,
                          0.5f, 0.0f);
        C3D_ImmSendAttrib(0.0f, 1.0f, 0.0f, 0.0f);

        C3D_ImmSendAttrib((float)PICA_SCREEN_WIDTH, 0.0f,
                          0.5f, 0.0f);
        C3D_ImmSendAttrib(max_s, min_t, 0.0f, 0.0f);

        C3D_ImmSendAttrib((float)PICA_SCREEN_WIDTH,
                          (float)PICA_SCREEN_HEIGHT, 0.5f, 0.0f);
        C3D_ImmSendAttrib(max_s, 1.0f, 0.0f, 0.0f);
    C3D_ImmDrawEnd();
}

bool smg3ds_pica200_init(void)
{
    C3D_AttrInfo* attributes;
    C3D_TexEnv* environment;
    int stage;

    memset(&g_stats, 0, sizeof(g_stats));
    if (!pica_rgba8_self_test())
        return false;

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE))
        return false;
    g_citro3d_initialized = true;

    g_target = C3D_RenderTargetCreate(
        PICA_SCREEN_HEIGHT, PICA_SCREEN_WIDTH,
        GPU_RB_RGBA8, GPU_RB_DEPTH16);
    if (g_target == NULL)
        goto fail;
    C3D_RenderTargetSetOutput(g_target, GFX_TOP, GFX_LEFT,
                              PICA_DISPLAY_TRANSFER_FLAGS);

    g_dvlb = DVLB_ParseFile((u32*)pica200_vshader_shbin,
                            pica200_vshader_shbin_size);
    if (g_dvlb == NULL)
        goto fail;
    shaderProgramInit(&g_program);
    g_program_initialized = true;
    shaderProgramSetVsh(&g_program, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_program);
    g_projection_location = shaderInstanceGetUniformLocation(
        g_program.vertexShader, "projection");
    if (g_projection_location < 0)
        goto fail;

    attributes = C3D_GetAttrInfo();
    AttrInfo_Init(attributes);
    AttrInfo_AddLoader(attributes, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attributes, 1, GPU_FLOAT, 2);

    Mtx_OrthoTilt(&g_projection,
                  0.0f, (float)PICA_SCREEN_WIDTH,
                  0.0f, (float)PICA_SCREEN_HEIGHT,
                  0.0f, 1.0f, true);

    if (!C3D_TexInit(&g_frame_texture, PICA_TEXTURE_WIDTH,
                     PICA_TEXTURE_HEIGHT, GPU_RGBA8))
        goto fail;
    g_texture_initialized = true;
    g_upload_buffer = (uint32_t*)linearAlloc(
        (size_t)PICA_TEXTURE_WIDTH * PICA_TEXTURE_HEIGHT * sizeof(uint32_t));
    if (g_upload_buffer == NULL)
        goto fail;

    /*
     * The 200x120 software EFB maps exactly 2x onto 400x240. Nearest sampling
     * preserves that mapping and cannot blend a padding row into the frame.
     */
    C3D_TexSetFilter(&g_frame_texture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&g_frame_texture, GPU_CLAMP_TO_EDGE,
                   GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &g_frame_texture);

    environment = C3D_GetTexEnv(0);
    C3D_TexEnvInit(environment);
    C3D_TexEnvSrc(environment, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(environment, C3D_Both, GPU_REPLACE);
    for (stage = 1; stage < 6; ++stage)
        C3D_TexEnvInit(C3D_GetTexEnv(stage));

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    g_stats.available = true;
    return true;

fail:
    smg3ds_pica200_shutdown();
    return false;
}

void smg3ds_pica200_shutdown(void)
{
    if (g_upload_buffer != NULL) {
        linearFree(g_upload_buffer);
        g_upload_buffer = NULL;
    }
    if (g_texture_initialized) {
        C3D_TexDelete(&g_frame_texture);
        g_texture_initialized = false;
    }
    if (g_program_initialized) {
        shaderProgramFree(&g_program);
        g_program_initialized = false;
    }
    if (g_dvlb != NULL) {
        DVLB_Free(g_dvlb);
        g_dvlb = NULL;
    }
    if (g_target != NULL) {
        C3D_RenderTargetDelete(g_target);
        g_target = NULL;
    }
    if (g_citro3d_initialized) {
        C3D_Fini();
        g_citro3d_initialized = false;
    }
    g_stats.available = false;
}

bool smg3ds_pica200_present(const uint32_t* pixels,
                            uint16_t width, uint16_t height,
                            uint32_t clear_rgba)
{
    if (!g_stats.available || width == 0u || height == 0u ||
        width > PICA_TEXTURE_WIDTH || height > PICA_TEXTURE_HEIGHT) {
        ++g_stats.frame_failures;
        return false;
    }

    upload_frame(pixels, width, height, clear_rgba);
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        ++g_stats.frame_failures;
        return false;
    }
    C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, clear_rgba, 0u);
    if (!C3D_FrameDrawOn(g_target)) {
        C3D_FrameEnd(0);
        ++g_stats.frame_failures;
        return false;
    }
    C3D_BindProgram(&g_program);
    C3D_TexBind(0, &g_frame_texture);
    draw_fullscreen(width, height);

    /* Flush the CPU-rendered bottom screen before Citro3D swaps both buffers. */
    gfxFlushBuffers();
    C3D_FrameEnd(0);
    ++g_stats.presented_frames;
    return true;
}

const Smg3dsPica200Stats* smg3ds_pica200_get_stats(void)
{
    return &g_stats;
}
