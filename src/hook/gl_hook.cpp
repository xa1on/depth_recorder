#include "gl_hook.h"
#include "hook_common.h"
#include "fps_overlay_font.h"

#include <MinHook.h>
#include <windows.h>
#include <GL/gl.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dr {

// Defined in dllmain.cpp
extern SharedConfigView g_config_view;
extern IpcRing g_ipc_ring;

namespace {

// OpenGL types & constants beyond GL 1.1
using GLchar = char;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;

constexpr GLenum kGL_BGRA = 0x80E1;
constexpr GLenum kGL_RED = 0x1903;
constexpr GLenum kGL_R8 = 0x8229;
constexpr GLenum kGL_RGBA8 = 0x8058;
constexpr GLenum kGL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum kGL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum kGL_TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum kGL_TEXTURE0 = 0x84C0;
constexpr GLenum kGL_ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum kGL_FRAMEBUFFER = 0x8D40;
constexpr GLenum kGL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum kGL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum kGL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum kGL_READ_FRAMEBUFFER_BINDING = 0x8CAA;
constexpr GLenum kGL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum kGL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum kGL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
constexpr GLenum kGL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum kGL_PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum kGL_PIXEL_PACK_BUFFER_BINDING = 0x88ED;
constexpr GLenum kGL_STREAM_READ = 0x88E1;
constexpr GLenum kGL_READ_ONLY = 0x88B8;
constexpr GLenum kGL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum kGL_VERTEX_SHADER = 0x8B31;
constexpr GLenum kGL_COMPILE_STATUS = 0x8B81;
constexpr GLenum kGL_LINK_STATUS = 0x8B82;
constexpr GLenum kGL_CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum kGL_VERTEX_ARRAY_BINDING = 0x85B5;
constexpr GLenum kGL_BLEND_SRC_RGB = 0x80C9;
constexpr GLenum kGL_BLEND_DST_RGB = 0x80C8;
constexpr GLenum kGL_BLEND_SRC_ALPHA = 0x80CB;
constexpr GLenum kGL_BLEND_DST_ALPHA = 0x80CA;
constexpr GLenum kGL_TEXTURE_WIDTH = 0x1000;
constexpr GLenum kGL_TEXTURE_HEIGHT = 0x1001;

constexpr uint32_t kPboSlots = 3;
constexpr uintptr_t kDefaultFboDepthKey = 0xFFFFFFFFULL;

// GL extension function pointer types
using PFN_wglSwapBuffers = BOOL(WINAPI*)(HDC);
using PFN_wglGetProcAddress = PROC(WINAPI*)(LPCSTR);
using PFN_glClear = void(APIENTRY*)(GLbitfield);
using PFN_glDrawArrays = void(APIENTRY*)(GLenum, GLint, GLsizei);
using PFN_glDrawElements = void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*);
using PFN_glEnd = void(APIENTRY*)();

using PFN_glBindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
using PFN_glFramebufferTexture2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glDrawArraysInstanced = void(APIENTRY*)(GLenum, GLint, GLsizei, GLsizei);
using PFN_glDrawElementsInstanced = void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei);

using PFN_glGenFramebuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_glDeleteFramebuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using PFN_glCheckFramebufferStatus = GLenum(APIENTRY*)(GLenum);
using PFN_glGenBuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_glDeleteBuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using PFN_glBindBuffer = void(APIENTRY*)(GLenum, GLuint);
using PFN_glBufferData = void(APIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glMapBuffer = void*(APIENTRY*)(GLenum, GLenum);
using PFN_glUnmapBuffer = GLboolean(APIENTRY*)(GLenum);
using PFN_glCreateShader = GLuint(APIENTRY*)(GLenum);
using PFN_glShaderSource = void(APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void(APIENTRY*)(GLuint);
using PFN_glGetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glDeleteShader = void(APIENTRY*)(GLuint);
using PFN_glCreateProgram = GLuint(APIENTRY*)();
using PFN_glAttachShader = void(APIENTRY*)(GLuint, GLuint);
using PFN_glLinkProgram = void(APIENTRY*)(GLuint);
using PFN_glGetProgramiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glDeleteProgram = void(APIENTRY*)(GLuint);
using PFN_glUseProgram = void(APIENTRY*)(GLuint);
using PFN_glGetUniformLocation = GLint(APIENTRY*)(GLuint, const GLchar*);
using PFN_glUniform1i = void(APIENTRY*)(GLint, GLint);
using PFN_glUniform1f = void(APIENTRY*)(GLint, GLfloat);
using PFN_glUniform4f = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glActiveTexture = void(APIENTRY*)(GLenum);
using PFN_glGenVertexArrays = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_glDeleteVertexArrays = void(APIENTRY*)(GLsizei, const GLuint*);
using PFN_glBindVertexArray = void(APIENTRY*)(GLuint);
using PFN_glBlendFuncSeparate = void(APIENTRY*)(GLenum, GLenum, GLenum, GLenum);

// Hooked originals
PFN_wglSwapBuffers oWglSwapBuffers = nullptr;
PFN_wglGetProcAddress oWglGetProcAddress = nullptr;
PFN_glClear oGlClear = nullptr;
PFN_glDrawArrays oGlDrawArrays = nullptr;
PFN_glDrawElements oGlDrawElements = nullptr;
PFN_glEnd oGlEnd = nullptr;

PFN_glBindFramebuffer oGlBindFramebuffer = nullptr;
PFN_glFramebufferTexture2D oGlFramebufferTexture2D = nullptr;
PFN_glDrawArraysInstanced oGlDrawArraysInstanced = nullptr;
PFN_glDrawElementsInstanced oGlDrawElementsInstanced = nullptr;

// Internal GL dispatch table
struct GlDispatch {
    bool loaded = false;
    PFN_glGenFramebuffers GenFramebuffers = nullptr;
    PFN_glDeleteFramebuffers DeleteFramebuffers = nullptr;
    PFN_glBindFramebuffer BindFramebuffer = nullptr;
    PFN_glFramebufferTexture2D FramebufferTexture2D = nullptr;
    PFN_glCheckFramebufferStatus CheckFramebufferStatus = nullptr;
    PFN_glGenBuffers GenBuffers = nullptr;
    PFN_glDeleteBuffers DeleteBuffers = nullptr;
    PFN_glBindBuffer BindBuffer = nullptr;
    PFN_glBufferData BufferData = nullptr;
    PFN_glMapBuffer MapBuffer = nullptr;
    PFN_glUnmapBuffer UnmapBuffer = nullptr;
    PFN_glCreateShader CreateShader = nullptr;
    PFN_glShaderSource ShaderSource = nullptr;
    PFN_glCompileShader CompileShader = nullptr;
    PFN_glGetShaderiv GetShaderiv = nullptr;
    PFN_glDeleteShader DeleteShader = nullptr;
    PFN_glCreateProgram CreateProgram = nullptr;
    PFN_glAttachShader AttachShader = nullptr;
    PFN_glLinkProgram LinkProgram = nullptr;
    PFN_glGetProgramiv GetProgramiv = nullptr;
    PFN_glDeleteProgram DeleteProgram = nullptr;
    PFN_glUseProgram UseProgram = nullptr;
    PFN_glGetUniformLocation GetUniformLocation = nullptr;
    PFN_glUniform1i Uniform1i = nullptr;
    PFN_glUniform1f Uniform1f = nullptr;
    PFN_glUniform4f Uniform4f = nullptr;
    PFN_glActiveTexture ActiveTexture = nullptr;
    PFN_glGenVertexArrays GenVertexArrays = nullptr;
    PFN_glDeleteVertexArrays DeleteVertexArrays = nullptr;
    PFN_glBindVertexArray BindVertexArray = nullptr;
    PFN_glBlendFuncSeparate BlendFuncSeparate = nullptr;
};

struct FboDepthInfo {
    GLuint tex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct GlState {
    bool initialized = false;
    bool hooks_installed = false;
    HGLRC hglrc = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame_idx = 0;
    int64_t qpc_freq = 0;
    int64_t last_qpc = 0;
    LONG last_cycle_seq = 0;
    uint32_t selected_depth_idx = 0;

    GLuint dummy_vao = 0;

    // Depth conversion pipeline
    GLuint depth_prog = 0;
    GLint loc_depth_tex = -1;
    GLint loc_mode = -1;
    GLint loc_linearize = -1;
    GLint loc_reversed_z = -1;
    GLint loc_z_near = -1;
    GLint loc_z_far = -1;

    GLuint depth_conv_fbo = 0;
    GLuint out_gray_tex = 0;
    GLuint out_rgba_tex = 0;
    GLuint default_depth_tex = 0;

    // Clear snapshot
    GLuint clear_snap_tex = 0;
    uintptr_t clear_snap_key = 0;
    bool clear_snap_valid_this_frame = false;

    // 3-slot PBO ring
    GLuint color_pbo[kPboSlots]{};
    GLuint depth_pbo[kPboSlots]{};
    uint32_t depth_pbo_bpp[kPboSlots]{};
    bool slot_pending[kPboSlots]{};
    bool slot_has_color[kPboSlots]{};
    int64_t slot_qpc[kPboSlots]{};
    uint32_t slot_mode[kPboSlots]{};
    bool slot_has_depth[kPboSlots]{};
    int64_t last_preview_qpc = 0;

    // FPS overlay resources
    GLuint overlay_prog = 0;
    GLint loc_overlay_rect = -1;
    GLint loc_overlay_tex = -1;
    GLuint font_tex = 0;
    uint32_t font_w = 0;
    uint32_t font_h = 0;

    // FBO tracking
    GLuint current_draw_fbo = 0;
    std::unordered_map<GLuint, FboDepthInfo> fbo_depths;

    DepthHeuristic heuristic;
    std::vector<uint8_t> color_pack_buf;
    std::vector<uint8_t> depth_pack_buf;
};

std::recursive_mutex g_mutex;
GlDispatch g_gl_fn;
GlState g_gl;

template <typename T>
bool load_proc(T& fn, const char* name) {
    PROC p = oWglGetProcAddress ? oWglGetProcAddress(name) : wglGetProcAddress(name);
    if (!p) return false;
    fn = reinterpret_cast<T>(p);
    return true;
}

bool load_gl_dispatch() {
    if (g_gl_fn.loaded) return true;
    bool ok = true;
    ok &= load_proc(g_gl_fn.GenFramebuffers, "glGenFramebuffers");
    ok &= load_proc(g_gl_fn.DeleteFramebuffers, "glDeleteFramebuffers");
    ok &= load_proc(g_gl_fn.BindFramebuffer, "glBindFramebuffer");
    ok &= load_proc(g_gl_fn.FramebufferTexture2D, "glFramebufferTexture2D");
    ok &= load_proc(g_gl_fn.CheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= load_proc(g_gl_fn.GenBuffers, "glGenBuffers");
    ok &= load_proc(g_gl_fn.DeleteBuffers, "glDeleteBuffers");
    ok &= load_proc(g_gl_fn.BindBuffer, "glBindBuffer");
    ok &= load_proc(g_gl_fn.BufferData, "glBufferData");
    ok &= load_proc(g_gl_fn.MapBuffer, "glMapBuffer");
    ok &= load_proc(g_gl_fn.UnmapBuffer, "glUnmapBuffer");
    ok &= load_proc(g_gl_fn.CreateShader, "glCreateShader");
    ok &= load_proc(g_gl_fn.ShaderSource, "glShaderSource");
    ok &= load_proc(g_gl_fn.CompileShader, "glCompileShader");
    ok &= load_proc(g_gl_fn.GetShaderiv, "glGetShaderiv");
    ok &= load_proc(g_gl_fn.DeleteShader, "glDeleteShader");
    ok &= load_proc(g_gl_fn.CreateProgram, "glCreateProgram");
    ok &= load_proc(g_gl_fn.AttachShader, "glAttachShader");
    ok &= load_proc(g_gl_fn.LinkProgram, "glLinkProgram");
    ok &= load_proc(g_gl_fn.GetProgramiv, "glGetProgramiv");
    ok &= load_proc(g_gl_fn.DeleteProgram, "glDeleteProgram");
    ok &= load_proc(g_gl_fn.UseProgram, "glUseProgram");
    ok &= load_proc(g_gl_fn.GetUniformLocation, "glGetUniformLocation");
    ok &= load_proc(g_gl_fn.Uniform1i, "glUniform1i");
    ok &= load_proc(g_gl_fn.Uniform1f, "glUniform1f");
    ok &= load_proc(g_gl_fn.Uniform4f, "glUniform4f");
    ok &= load_proc(g_gl_fn.ActiveTexture, "glActiveTexture");
    ok &= load_proc(g_gl_fn.GenVertexArrays, "glGenVertexArrays");
    ok &= load_proc(g_gl_fn.DeleteVertexArrays, "glDeleteVertexArrays");
    ok &= load_proc(g_gl_fn.BindVertexArray, "glBindVertexArray");
    ok &= load_proc(g_gl_fn.BlendFuncSeparate, "glBlendFuncSeparate");
    g_gl_fn.loaded = ok;
    return ok;
}

GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = g_gl_fn.CreateShader(type);
    g_gl_fn.ShaderSource(s, 1, &src, nullptr);
    g_gl_fn.CompileShader(s);
    GLint status = 0;
    g_gl_fn.GetShaderiv(s, kGL_COMPILE_STATUS, &status);
    if (!status) {
        g_gl_fn.DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(kGL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(kGL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) g_gl_fn.DeleteShader(vs);
        if (fs) g_gl_fn.DeleteShader(fs);
        return 0;
    }
    GLuint prog = g_gl_fn.CreateProgram();
    g_gl_fn.AttachShader(prog, vs);
    g_gl_fn.AttachShader(prog, fs);
    g_gl_fn.LinkProgram(prog);
    g_gl_fn.DeleteShader(vs);
    g_gl_fn.DeleteShader(fs);
    GLint status = 0;
    g_gl_fn.GetProgramiv(prog, kGL_LINK_STATUS, &status);
    if (!status) {
        g_gl_fn.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

const char* kFullscreenVS = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    vUV = 0.5 * (pos + 1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

const char* kDepthConvertFS = R"(#version 330 core
uniform sampler2D uDepthTex;
uniform int uMode;
uniform int uLinearize;
uniform int uReversedZ;
uniform float uZNear;
uniform float uZFar;
layout(location = 0) out vec4 oColor;

float LinearizeDepth(float d) {
    d = clamp(d, 0.0, 1.0);
    if (uReversedZ != 0) d = 1.0 - d;
    float denom = uZFar - d * (uZFar - uZNear);
    if (denom <= 1e-6) return 1.0;
    float z_view = (uZNear * uZFar) / denom;
    return clamp((z_view - uZNear) / max(uZFar - uZNear, 1e-6), 0.0, 1.0);
}

void main() {
    float d_raw = clamp(texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r, 0.0, 1.0);
    if (uMode == 0) {
        float d = (uLinearize != 0) ? LinearizeDepth(d_raw) : d_raw;
        oColor = vec4(d, 0.0, 0.0, 1.0);
        return;
    }
    if (uMode == 1) {
        float d = (uLinearize != 0) ? LinearizeDepth(d_raw) : d_raw;
        float theta = 6.28318530718 * 8.0 * d;
        float g = 0.5 + 0.5 * cos(theta);
        float b = 0.5 + 0.5 * sin(theta);
        oColor = vec4(d, g, b, 1.0);
        return;
    }
    uint u24 = uint(clamp(round(d_raw * 16777215.0), 0.0, 16777215.0));
    float r = float((u24 >> 16) & 0xFFu) / 255.0;
    float g = float((u24 >> 8) & 0xFFu) / 255.0;
    float b = float(u24 & 0xFFu) / 255.0;
    oColor = vec4(r, g, b, 1.0);
}
)";

const char* kOverlayVS = R"(#version 330 core
uniform vec4 uRect; // xy = min_ndc, zw = max_ndc
out vec2 vUV;
void main() {
    vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
    );
    vec2 c = corners[gl_VertexID];
    gl_Position = vec4(mix(uRect.x, uRect.z, c.x),
                       mix(uRect.w, uRect.y, c.y), 0.0, 1.0);
    vUV = c;
}
)";

const char* kOverlayFS = R"(#version 330 core
uniform sampler2D uFontTex;
in vec2 vUV;
out vec4 oColor;
void main() {
    oColor = texture(uFontTex, vUV);
}
)";

// RAII helper to save and restore OpenGL state touched by hook passes
struct GlStateGuard {
    GLint draw_fbo = 0;
    GLint read_fbo = 0;
    GLint prog = 0;
    GLint vao = 0;
    GLint active_tex = 0;
    GLint tex2d = 0;
    GLint pbo = 0;
    GLint pack_align = 4;
    GLint unpack_align = 4;
    GLint read_buf = GL_BACK;
    GLint vp[4]{};
    GLboolean scissor = GL_FALSE;
    GLboolean depth_test = GL_FALSE;
    GLboolean depth_mask = GL_TRUE;
    GLboolean cull_face = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO;
    GLint blend_src_a = GL_ONE, blend_dst_a = GL_ZERO;

    GlStateGuard() {
        glGetIntegerv(kGL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(kGL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        glGetIntegerv(kGL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(kGL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(kGL_ACTIVE_TEXTURE, &active_tex);
        g_gl_fn.ActiveTexture(kGL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
        glGetIntegerv(kGL_PIXEL_PACK_BUFFER_BINDING, &pbo);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_align);
        glGetIntegerv(GL_READ_BUFFER, &read_buf);
        glGetIntegerv(GL_VIEWPORT, vp);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        depth_test = glIsEnabled(GL_DEPTH_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        cull_face = glIsEnabled(GL_CULL_FACE);
        blend = glIsEnabled(GL_BLEND);
        glGetIntegerv(kGL_BLEND_SRC_RGB, &blend_src_rgb);
        glGetIntegerv(kGL_BLEND_DST_RGB, &blend_dst_rgb);
        glGetIntegerv(kGL_BLEND_SRC_ALPHA, &blend_src_a);
        glGetIntegerv(kGL_BLEND_DST_ALPHA, &blend_dst_a);
    }

    ~GlStateGuard() {
        g_gl_fn.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
        g_gl_fn.BindFramebuffer(kGL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo));
        g_gl_fn.UseProgram(static_cast<GLuint>(prog));
        g_gl_fn.BindVertexArray(static_cast<GLuint>(vao));
        g_gl_fn.ActiveTexture(kGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex2d));
        g_gl_fn.ActiveTexture(static_cast<GLenum>(active_tex));
        g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pbo));
        glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_align);
        glReadBuffer(static_cast<GLenum>(read_buf));
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(depth_mask);
        if (cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        g_gl_fn.BlendFuncSeparate(static_cast<GLenum>(blend_src_rgb), static_cast<GLenum>(blend_dst_rgb),
                                  static_cast<GLenum>(blend_src_a), static_cast<GLenum>(blend_dst_a));
    }
};

void release_size_dependent() {
    if (!g_gl_fn.loaded || !wglGetCurrentContext()) return;
    for (uint32_t i = 0; i < kPboSlots; ++i) {
        if (g_gl.color_pbo[i]) { g_gl_fn.DeleteBuffers(1, &g_gl.color_pbo[i]); g_gl.color_pbo[i] = 0; }
        if (g_gl.depth_pbo[i]) { g_gl_fn.DeleteBuffers(1, &g_gl.depth_pbo[i]); g_gl.depth_pbo[i] = 0; }
        g_gl.depth_pbo_bpp[i] = 0;
        g_gl.slot_pending[i] = false;
    }
    auto del_tex = [](GLuint& t) {
        if (t) { glDeleteTextures(1, &t); t = 0; }
    };
    del_tex(g_gl.out_gray_tex);
    del_tex(g_gl.out_rgba_tex);
    del_tex(g_gl.default_depth_tex);
    del_tex(g_gl.clear_snap_tex);
    g_gl.width = 0;
    g_gl.height = 0;
}

GLuint create_tex2d(uint32_t w, uint32_t h, GLenum internal_fmt, GLenum fmt, GLenum type) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal_fmt),
                 static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, fmt, type, nullptr);
    return t;
}

bool ensure_pipeline(HDC hdc) {
    HGLRC ctx = wglGetCurrentContext();
    if (!ctx) return false;
    if (!load_gl_dispatch()) return false;

    if (!g_gl.initialized) {
        g_gl.hglrc = ctx;
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        g_gl.qpc_freq = freq.QuadPart;

        g_gl_fn.GenVertexArrays(1, &g_gl.dummy_vao);
        g_gl_fn.GenFramebuffers(1, &g_gl.depth_conv_fbo);

        g_gl.depth_prog = link_program(kFullscreenVS, kDepthConvertFS);
        if (g_gl.depth_prog) {
            g_gl.loc_depth_tex = g_gl_fn.GetUniformLocation(g_gl.depth_prog, "uDepthTex");
            g_gl.loc_mode = g_gl_fn.GetUniformLocation(g_gl.depth_prog, "uMode");
            g_gl.loc_linearize = g_gl_fn.GetUniformLocation(g_gl.depth_prog, "uLinearize");
            g_gl.loc_reversed_z = g_gl_fn.GetUniformLocation(g_gl.depth_prog, "uReversedZ");
            g_gl.loc_z_near = g_gl_fn.GetUniformLocation(g_gl.depth_prog, "uZNear");
            g_gl.loc_z_far = g_gl_fn.GetUniformLocation(g_gl.depth_prog, "uZFar");
        }

        g_gl.overlay_prog = link_program(kOverlayVS, kOverlayFS);
        if (g_gl.overlay_prog) {
            g_gl.loc_overlay_rect = g_gl_fn.GetUniformLocation(g_gl.overlay_prog, "uRect");
            g_gl.loc_overlay_tex = g_gl_fn.GetUniformLocation(g_gl.overlay_prog, "uFontTex");
        }

        g_gl.initialized = true;
    }

    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd) return false;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    const uint32_t w = static_cast<uint32_t>(rc.right - rc.left);
    const uint32_t h = static_cast<uint32_t>(rc.bottom - rc.top);
    if (w == 0 || h == 0) return false;

    if (g_gl.width != w || g_gl.height != h) {
        GlStateGuard guard;
        release_size_dependent();
        g_gl.width = w;
        g_gl.height = h;

        g_gl.out_gray_tex = create_tex2d(w, h, kGL_R8, kGL_RED, GL_UNSIGNED_BYTE);
        g_gl.out_rgba_tex = create_tex2d(w, h, kGL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        g_gl.default_depth_tex = create_tex2d(w, h, kGL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);

        const GLsizeiptr color_bytes = static_cast<GLsizeiptr>(w) * h * 4;
        g_gl_fn.GenBuffers(kPboSlots, g_gl.color_pbo);
        for (uint32_t i = 0; i < kPboSlots; ++i) {
            g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, g_gl.color_pbo[i]);
            g_gl_fn.BufferData(kGL_PIXEL_PACK_BUFFER, color_bytes, nullptr, kGL_STREAM_READ);
        }
        g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
    }
    return true;
}

void ensure_depth_pbo(uint32_t slot, uint32_t bpp) {
    if (g_gl.depth_pbo[slot] && g_gl.depth_pbo_bpp[slot] == bpp) return;
    if (g_gl.depth_pbo[slot]) {
        g_gl_fn.DeleteBuffers(1, &g_gl.depth_pbo[slot]);
        g_gl.depth_pbo[slot] = 0;
    }
    g_gl_fn.GenBuffers(1, &g_gl.depth_pbo[slot]);
    g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, g_gl.depth_pbo[slot]);
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(g_gl.width) * g_gl.height * bpp;
    g_gl_fn.BufferData(kGL_PIXEL_PACK_BUFFER, bytes, nullptr, kGL_STREAM_READ);
    g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
    g_gl.depth_pbo_bpp[slot] = bpp;
}

void copy_pbo_flipped(GLuint pbo, uint32_t w, uint32_t h, uint32_t bpp, std::vector<uint8_t>& dst) {
    g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, pbo);
    const auto* src = static_cast<const uint8_t*>(g_gl_fn.MapBuffer(kGL_PIXEL_PACK_BUFFER, kGL_READ_ONLY));
    if (src) {
        const size_t row_bytes = static_cast<size_t>(w) * bpp;
        dst.resize(row_bytes * h);
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(dst.data() + static_cast<size_t>(y) * row_bytes,
                        src + static_cast<size_t>(h - 1 - y) * row_bytes, row_bytes);
        }
        g_gl_fn.UnmapBuffer(kGL_PIXEL_PACK_BUFFER);
    }
    g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
}

void draw_fps_overlay(double fps, double rec_fps, bool recording, OverlayCorner corner) {
    if (!g_gl.overlay_prog || g_gl.width == 0 || g_gl.height == 0) return;

    std::vector<uint8_t> rgba;
    uint32_t bw = 0, bh = 0;
    render_fps_overlay_rgba(fps, rec_fps, recording, rgba, bw, bh);

    GlStateGuard guard;

    if (!g_gl.font_tex || g_gl.font_w != bw || g_gl.font_h != bh) {
        if (g_gl.font_tex) glDeleteTextures(1, &g_gl.font_tex);
        g_gl.font_tex = create_tex2d(bw, bh, kGL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        g_gl.font_w = bw;
        g_gl.font_h = bh;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g_gl_fn.ActiveTexture(kGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_gl.font_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(bw), static_cast<GLsizei>(bh),
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    float ndc_rect[4]{};
    compute_overlay_ndc_rect(bw, bh, g_gl.width, g_gl.height, corner, ndc_rect);

    g_gl_fn.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<GLsizei>(g_gl.width), static_cast<GLsizei>(g_gl.height));
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    g_gl_fn.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);

    g_gl_fn.UseProgram(g_gl.overlay_prog);
    g_gl_fn.Uniform4f(g_gl.loc_overlay_rect, ndc_rect[0], ndc_rect[1], ndc_rect[2], ndc_rect[3]);
    g_gl_fn.Uniform1i(g_gl.loc_overlay_tex, 0);
    g_gl_fn.BindVertexArray(g_gl.dummy_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void capture_frame(SharedConfig* cfg, int64_t now_qpc, bool capture_color) {
    GlStateGuard guard;

    const uint32_t write_slot = static_cast<uint32_t>(g_gl.frame_idx % kPboSlots);
    for (uint32_t off = 1; off <= kPboSlots; ++off) {
        const uint32_t read_slot = (write_slot + off) % kPboSlots;
        if (!g_gl.slot_pending[read_slot]) continue;

        uint32_t color_bytes = 0;
        if (g_gl.slot_has_color[read_slot] && g_gl.color_pbo[read_slot]) {
            copy_pbo_flipped(g_gl.color_pbo[read_slot], g_gl.width, g_gl.height, 4, g_gl.color_pack_buf);
            color_bytes = static_cast<uint32_t>(g_gl.color_pack_buf.size());
        }
        uint32_t depth_bytes = 0;
        if (g_gl.slot_has_depth[read_slot] && g_gl.depth_pbo[read_slot]) {
            const uint32_t bpp = g_gl.depth_pbo_bpp[read_slot];
            copy_pbo_flipped(g_gl.depth_pbo[read_slot], g_gl.width, g_gl.height, bpp, g_gl.depth_pack_buf);
            depth_bytes = static_cast<uint32_t>(g_gl.depth_pack_buf.size());
        }
        push_frame_to_ring(g_ipc_ring, cfg, g_gl.frame_idx, g_gl.slot_qpc[read_slot], g_gl.qpc_freq,
                           g_gl.width, g_gl.height, g_gl.slot_mode[read_slot], g_gl.slot_has_depth[read_slot],
                           color_bytes ? g_gl.color_pack_buf.data() : nullptr, color_bytes,
                           depth_bytes ? g_gl.depth_pack_buf.data() : nullptr, depth_bytes,
                           false, !g_gl.slot_has_color[read_slot]);
        g_gl.slot_pending[read_slot] = false;
    }

    if (capture_color) {
        GLboolean dbl = GL_FALSE;
        glGetBooleanv(GL_DOUBLEBUFFER, &dbl);

        g_gl_fn.BindFramebuffer(kGL_READ_FRAMEBUFFER, 0);
        glReadBuffer(dbl ? GL_BACK : GL_FRONT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(0x0D02 /*GL_PACK_ROW_LENGTH*/, 0);
        glPixelStorei(0x0D03 /*GL_PACK_SKIP_ROWS*/, 0);
        glPixelStorei(0x0D04 /*GL_PACK_SKIP_PIXELS*/, 0);
        g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, g_gl.color_pbo[write_slot]);
        glReadPixels(0, 0, static_cast<GLsizei>(g_gl.width), static_cast<GLsizei>(g_gl.height),
                     kGL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
    }

    const bool want_depth = (cfg && cfg->depth_enabled != 0);
    const uint32_t mode = cfg ? static_cast<uint32_t>(cfg->depth_mode) : static_cast<uint32_t>(DepthMode::Rgb24Lossless);
    bool captured_depth = false;

    DepthCandidate cand{};
    const bool has_cand = select_depth_candidate(g_gl.heuristic.ranked_candidates(), cfg, g_gl.selected_depth_idx, cand);

    if (want_depth && has_cand && g_gl.depth_prog && g_gl.depth_conv_fbo) {
        GLuint depth_tex = 0;
        if (g_gl.clear_snap_valid_this_frame && g_gl.clear_snap_key == cand.key && g_gl.clear_snap_tex) {
            depth_tex = g_gl.clear_snap_tex;
        } else if (cand.key == kDefaultFboDepthKey) {
            g_gl_fn.BindFramebuffer(kGL_READ_FRAMEBUFFER, 0);
            glBindTexture(GL_TEXTURE_2D, g_gl.default_depth_tex);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                static_cast<GLsizei>(g_gl.width), static_cast<GLsizei>(g_gl.height));
            depth_tex = g_gl.default_depth_tex;
        } else {
            depth_tex = static_cast<GLuint>(cand.key);
        }

        if (depth_tex != 0) {
            const uint32_t bpp = (mode == static_cast<uint32_t>(DepthMode::Gray8)) ? 1 : 4;
            const GLuint out_tex = (bpp == 1) ? g_gl.out_gray_tex : g_gl.out_rgba_tex;

            g_gl_fn.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, g_gl.depth_conv_fbo);
            g_gl_fn.FramebufferTexture2D(kGL_DRAW_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex, 0);
            if (g_gl_fn.CheckFramebufferStatus(kGL_DRAW_FRAMEBUFFER) == kGL_FRAMEBUFFER_COMPLETE) {
                g_gl_fn.ActiveTexture(kGL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, depth_tex);
                GLint prev_cmp = GL_NONE, prev_min = GL_NEAREST, prev_mag = GL_NEAREST;
                glGetTexParameteriv(GL_TEXTURE_2D, kGL_TEXTURE_COMPARE_MODE, &prev_cmp);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &prev_min);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &prev_mag);
                glTexParameteri(GL_TEXTURE_2D, kGL_TEXTURE_COMPARE_MODE, GL_NONE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                glViewport(0, 0, static_cast<GLsizei>(g_gl.width), static_cast<GLsizei>(g_gl.height));
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_BLEND);

                g_gl_fn.UseProgram(g_gl.depth_prog);
                g_gl_fn.Uniform1i(g_gl.loc_depth_tex, 0);
                g_gl_fn.Uniform1i(g_gl.loc_mode, static_cast<GLint>(mode));
                g_gl_fn.Uniform1i(g_gl.loc_linearize, cfg ? static_cast<GLint>(cfg->linearize) : 1);
                g_gl_fn.Uniform1i(g_gl.loc_reversed_z, cfg ? static_cast<GLint>(cfg->reversed_z) : 1);
                g_gl_fn.Uniform1f(g_gl.loc_z_near, cfg ? cfg->z_near : 0.1f);
                g_gl_fn.Uniform1f(g_gl.loc_z_far, cfg ? cfg->z_far : 500.0f);

                g_gl_fn.BindVertexArray(g_gl.dummy_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);

                glTexParameteri(GL_TEXTURE_2D, kGL_TEXTURE_COMPARE_MODE, prev_cmp);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, prev_min);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, prev_mag);

                ensure_depth_pbo(write_slot, bpp);
                g_gl_fn.BindFramebuffer(kGL_READ_FRAMEBUFFER, g_gl.depth_conv_fbo);
                glReadBuffer(kGL_COLOR_ATTACHMENT0);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, g_gl.depth_pbo[write_slot]);
                glReadPixels(0, 0, static_cast<GLsizei>(g_gl.width), static_cast<GLsizei>(g_gl.height),
                             (bpp == 1) ? kGL_RED : GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                g_gl_fn.BindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
                captured_depth = true;
            }
        }
    }

    g_gl.slot_qpc[write_slot] = now_qpc;
    g_gl.slot_mode[write_slot] = mode;
    g_gl.slot_has_depth[write_slot] = captured_depth;
    g_gl.slot_has_color[write_slot] = capture_color;
    g_gl.slot_pending[write_slot] = true;
}

void record_active_draw() {
    if (g_gl.width == 0 || g_gl.height == 0) return;
    std::lock_guard lock(g_mutex);
    if (g_gl.current_draw_fbo == 0) {
        g_gl.heuristic.record_draw(kDefaultFboDepthKey, g_gl.width, g_gl.height, g_gl.width, g_gl.height);
    } else {
        auto it = g_gl.fbo_depths.find(g_gl.current_draw_fbo);
        if (it != g_gl.fbo_depths.end() && it->second.tex != 0) {
            g_gl.heuristic.record_draw(static_cast<uintptr_t>(it->second.tex),
                                       it->second.width, it->second.height, g_gl.width, g_gl.height);
        }
    }
}

// Hook implementations
BOOL WINAPI hkWglSwapBuffers(HDC hdc) {
    {
        std::lock_guard lock(g_mutex);
        if (!g_config_view.get()) g_config_view.open_or_create(false);
        HWND hwnd = WindowFromDC(hdc);
        RECT rc{};
        const uint32_t w = (hwnd && GetClientRect(hwnd, &rc)) ? static_cast<uint32_t>(rc.right - rc.left) : 0;
        const uint32_t h = (hwnd && GetClientRect(hwnd, &rc)) ? static_cast<uint32_t>(rc.bottom - rc.top) : 0;
        const bool selected = !g_config_view.get() ||
            register_and_is_selected_swapchain(g_config_view.get(), reinterpret_cast<uint64_t>(hwnd ? hwnd : reinterpret_cast<HWND>(hdc)), w, h, hwnd);
        if (selected && ensure_pipeline(hdc)) {
            ensure_ipc_connected(g_config_view, g_ipc_ring);
            SharedConfig* cfg = g_config_view.get();
            int64_t now_qpc = 0;
            const double fps = tick_fps_and_telemetry(cfg, 2, g_gl.last_qpc, g_gl.qpc_freq,
                                                      g_gl.last_cycle_seq, g_gl.selected_depth_idx, now_qpc);

            const bool is_recording = (cfg && cfg->recording != 0 && g_ipc_ring.event_handle() != nullptr);
            const bool is_preview = (!is_recording && cfg && cfg->preview_enabled != 0 && g_ipc_ring.event_handle() != nullptr &&
                                     should_capture_preview_now(now_qpc, g_gl.qpc_freq, g_gl.last_preview_qpc));
            if (is_recording || is_preview) {
                capture_frame(cfg, now_qpc, is_recording);
            } else if (!cfg || (cfg->recording == 0 && cfg->preview_enabled == 0)) {
                for (bool& p : g_gl.slot_pending) p = false;
            }

            if (cfg && cfg->overlay_enabled != 0) {
                const double rec_fps = static_cast<double>(cfg->actual_rec_fps_x10) / 10.0;
                draw_fps_overlay(fps, rec_fps, cfg->recording != 0, static_cast<OverlayCorner>(cfg->overlay_corner));
            }

            g_gl.frame_idx++;
            g_gl.heuristic.end_frame_prune();
            g_gl.heuristic.begin_frame(g_gl.frame_idx);
            g_gl.clear_snap_valid_this_frame = false;
        }
    }
    return oWglSwapBuffers(hdc);
}

void APIENTRY hkGlBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (target == kGL_FRAMEBUFFER || target == kGL_DRAW_FRAMEBUFFER) {
        std::lock_guard lock(g_mutex);
        g_gl.current_draw_fbo = framebuffer;
    }
    oGlBindFramebuffer(target, framebuffer);
}

void APIENTRY hkGlFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    oGlFramebufferTexture2D(target, attachment, textarget, texture, level);
    if ((target == kGL_FRAMEBUFFER || target == kGL_DRAW_FRAMEBUFFER) &&
        (attachment == kGL_DEPTH_ATTACHMENT || attachment == kGL_DEPTH_STENCIL_ATTACHMENT)) {
        std::lock_guard lock(g_mutex);
        if (texture == 0) {
            g_gl.fbo_depths.erase(g_gl.current_draw_fbo);
        } else {
            GLint tw = 0, th = 0;
            GLint prev_tex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, level, kGL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, level, kGL_TEXTURE_HEIGHT, &th);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
            g_gl.fbo_depths[g_gl.current_draw_fbo] = {texture, static_cast<uint32_t>(tw), static_cast<uint32_t>(th)};
        }
    }
}

void APIENTRY hkGlClear(GLbitfield mask) {
    if ((mask & GL_DEPTH_BUFFER_BIT) && g_gl.width > 0) {
        std::lock_guard lock(g_mutex);
        uintptr_t key = 0;
        uint32_t w = 0, h = 0;
        if (g_gl.current_draw_fbo == 0) {
            key = kDefaultFboDepthKey;
            w = g_gl.width;
            h = g_gl.height;
        } else {
            auto it = g_gl.fbo_depths.find(g_gl.current_draw_fbo);
            if (it != g_gl.fbo_depths.end() && it->second.tex != 0) {
                key = static_cast<uintptr_t>(it->second.tex);
                w = it->second.width;
                h = it->second.height;
            }
        }
        if (key != 0) {
            const auto ranked = g_gl.heuristic.ranked_candidates();
            if (!ranked.empty() && ranked[0].key == key && ranked[0].draw_calls >= 5 && g_gl_fn.loaded) {
                if (!g_gl.clear_snap_tex) {
                    g_gl.clear_snap_tex = create_tex2d(g_gl.width, g_gl.height, kGL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);
                }
                if (g_gl.clear_snap_tex && w == g_gl.width && h == g_gl.height) {
                    GLint prev_read = 0, prev_tex = 0;
                    glGetIntegerv(kGL_READ_FRAMEBUFFER_BINDING, &prev_read);
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
                    g_gl_fn.BindFramebuffer(kGL_READ_FRAMEBUFFER, g_gl.current_draw_fbo);
                    glBindTexture(GL_TEXTURE_2D, g_gl.clear_snap_tex);
                    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
                    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
                    g_gl_fn.BindFramebuffer(kGL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
                    g_gl.clear_snap_key = key;
                    g_gl.clear_snap_valid_this_frame = true;
                }
            }
            g_gl.heuristic.record_clear(key, w, h, g_gl.width, g_gl.height);
        }
    }
    oGlClear(mask);
}

void APIENTRY hkGlDrawArrays(GLenum mode, GLint first, GLsizei count) {
    record_active_draw();
    oGlDrawArrays(mode, first, count);
}

void APIENTRY hkGlDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    record_active_draw();
    oGlDrawElements(mode, count, type, indices);
}

void APIENTRY hkGlEnd() {
    record_active_draw();
    oGlEnd();
}

void APIENTRY hkGlDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    record_active_draw();
    oGlDrawArraysInstanced(mode, first, count, instancecount);
}

void APIENTRY hkGlDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
    record_active_draw();
    oGlDrawElementsInstanced(mode, count, type, indices, instancecount);
}

PROC WINAPI hkWglGetProcAddress(LPCSTR name) {
    PROC real = oWglGetProcAddress(name);
    if (!real || !name) return real;

    if (std::strcmp(name, "glBindFramebuffer") == 0 || std::strcmp(name, "glBindFramebufferEXT") == 0) {
        if (!oGlBindFramebuffer) oGlBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(real);
        return reinterpret_cast<PROC>(&hkGlBindFramebuffer);
    }
    if (std::strcmp(name, "glFramebufferTexture2D") == 0 || std::strcmp(name, "glFramebufferTexture2DEXT") == 0) {
        if (!oGlFramebufferTexture2D) oGlFramebufferTexture2D = reinterpret_cast<PFN_glFramebufferTexture2D>(real);
        return reinterpret_cast<PROC>(&hkGlFramebufferTexture2D);
    }
    if (std::strcmp(name, "glDrawArraysInstanced") == 0 || std::strcmp(name, "glDrawArraysInstancedARB") == 0) {
        if (!oGlDrawArraysInstanced) oGlDrawArraysInstanced = reinterpret_cast<PFN_glDrawArraysInstanced>(real);
        return reinterpret_cast<PROC>(&hkGlDrawArraysInstanced);
    }
    if (std::strcmp(name, "glDrawElementsInstanced") == 0 || std::strcmp(name, "glDrawElementsInstancedARB") == 0) {
        if (!oGlDrawElementsInstanced) oGlDrawElementsInstanced = reinterpret_cast<PFN_glDrawElementsInstanced>(real);
        return reinterpret_cast<PROC>(&hkGlDrawElementsInstanced);
    }
    return real;
}

} // namespace

bool install_gl_hooks() {
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) gl = LoadLibraryW(L"opengl32.dll");
    if (!gl) return false;

    auto hook_export = [&](const char* name, void* detour, void** orig) {
        if (FARPROC proc = GetProcAddress(gl, name)) {
            MH_CreateHook(reinterpret_cast<void*>(proc), detour, orig);
        }
    };

    hook_export("wglSwapBuffers", reinterpret_cast<void*>(&hkWglSwapBuffers), reinterpret_cast<void**>(&oWglSwapBuffers));
    hook_export("wglGetProcAddress", reinterpret_cast<void*>(&hkWglGetProcAddress), reinterpret_cast<void**>(&oWglGetProcAddress));
    hook_export("glClear", reinterpret_cast<void*>(&hkGlClear), reinterpret_cast<void**>(&oGlClear));
    hook_export("glDrawArrays", reinterpret_cast<void*>(&hkGlDrawArrays), reinterpret_cast<void**>(&oGlDrawArrays));
    hook_export("glDrawElements", reinterpret_cast<void*>(&hkGlDrawElements), reinterpret_cast<void**>(&oGlDrawElements));
    hook_export("glEnd", reinterpret_cast<void*>(&hkGlEnd), reinterpret_cast<void**>(&oGlEnd));

    g_gl.hooks_installed = (MH_EnableHook(MH_ALL_HOOKS) == MH_OK);
    return g_gl.hooks_installed;
}

void remove_gl_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    std::lock_guard lock(g_mutex);
    release_size_dependent();
    if (g_gl_fn.loaded && wglGetCurrentContext()) {
        if (g_gl.depth_conv_fbo) g_gl_fn.DeleteFramebuffers(1, &g_gl.depth_conv_fbo);
        if (g_gl.dummy_vao) g_gl_fn.DeleteVertexArrays(1, &g_gl.dummy_vao);
        if (g_gl.depth_prog) g_gl_fn.DeleteProgram(g_gl.depth_prog);
        if (g_gl.overlay_prog) g_gl_fn.DeleteProgram(g_gl.overlay_prog);
        if (g_gl.font_tex) glDeleteTextures(1, &g_gl.font_tex);
    }
    g_gl = GlState{};
}

} // namespace dr
