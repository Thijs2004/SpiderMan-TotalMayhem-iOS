// Shows the game's OpenGL ES frames through Metal. OpenGL ES content presented with CAEAGLLayer stays at
// 60 Hz on ProMotion iPhones; Metal can use 120 Hz. The game renders into one of three IOSurface-backed
// buffers shared between GL and Metal; a GL fence marks when a frame is done, and the newest finished
// frame is drawn (flipped right side up) into the CAMetalLayer's drawable.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES1/gl.h>
#import <OpenGLES/ES1/glext.h>
#import <OpenGLES/ES2/glext.h>
#include "elfload.h"

#define NBUF 3

static CAMetalLayer *g_layer;
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLRenderPipelineState> g_pipe;
static CVOpenGLESTextureCacheRef g_glCache;
static CVMetalTextureCacheRef g_mtlCache;
static struct {
    CVPixelBufferRef pb;
    CVOpenGLESTextureRef glTex;
    CVMetalTextureRef mtlTex;
    GLuint tex;
    GLsync fence;
    BOOL ready;   // holds a finished frame not shown yet (or the newest shown one)
} g_buf[NBUF];
static int g_cur = -1, g_last = -1;
// One framebuffer object for the whole run: the 2010 engine remembers the framebuffer bound at start-up and
// keeps drawing into it, so each frame only the colour image attached to it changes.
static GLuint g_fbo, g_depth;
// Fallback when GL can't draw into the shared images (the simulator's software GL): one plain GL texture,
// read back into a Metal texture each frame.
static BOOL g_readback;
static GLuint g_plainTex;
static id<MTLTexture> g_readTex;
static uint8_t *g_pixels;
static int g_w, g_h;

static NSString *const kShader = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint id [[vertex_id]]) {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, p.y); return o;\n"   // GL rows are bottom-up
    "}\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
    "  constexpr sampler s(filter::linear);\n"
    "  return float4(t.sample(s, in.uv).rgb, 1);\n"
    "}\n";

int present_init(CAMetalLayer *layer, EAGLContext *ctx, int w, int h) {
    g_layer = layer;
    g_dev = MTLCreateSystemDefaultDevice();
    g_queue = [g_dev newCommandQueue];
    layer.device = g_dev;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.drawableSize = CGSizeMake(w, h);
    layer.maximumDrawableCount = 3;
    NSError *err;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:kShader options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!g_pipe) { elf_log("present: pipeline failed: %s", err.localizedDescription.UTF8String); return -1; }
    CVOpenGLESTextureCacheCreate(NULL, NULL, ctx, NULL, &g_glCache);
    CVMetalTextureCacheCreate(NULL, NULL, g_dev, NULL, &g_mtlCache);
    g_w = w; g_h = h;
    NSDictionary *attrs = @{
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
        (id)kCVPixelBufferOpenGLESCompatibilityKey : @YES,
        (id)kCVPixelBufferMetalCompatibilityKey : @YES,
    };
    for (int i = 0; i < NBUF; i++) {
        if (CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &g_buf[i].pb) != kCVReturnSuccess) return -1;
        CVOpenGLESTextureCacheCreateTextureFromImage(NULL, g_glCache, g_buf[i].pb, NULL, GL_TEXTURE_2D, GL_RGBA, w, h,
                                                     GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0, &g_buf[i].glTex);
        CVMetalTextureCacheCreateTextureFromImage(NULL, g_mtlCache, g_buf[i].pb, NULL, MTLPixelFormatBGRA8Unorm, w, h, 0, &g_buf[i].mtlTex);
        GLuint tex = CVOpenGLESTextureGetName(g_buf[i].glTex);
        g_buf[i].tex = tex;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glGenFramebuffersOES(1, &g_fbo);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, g_fbo);
    glGenRenderbuffersOES(1, &g_depth);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, g_depth);
    glRenderbufferStorageOES(GL_RENDERBUFFER_OES, GL_DEPTH24_STENCIL8_OES, w, h);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_DEPTH_ATTACHMENT_OES, GL_RENDERBUFFER_OES, g_depth);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_STENCIL_ATTACHMENT_OES, GL_RENDERBUFFER_OES, g_depth);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, g_buf[0].tex, 0);
    GLenum st = glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES);
    // Can GL draw into the shared image? Clear a pixel and read it back.
    uint8_t px[4] = {0};
    if (st == GL_FRAMEBUFFER_COMPLETE_OES) {
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glClearColor(0, 0, 0, 1);
    }
    if (px[0] != 255) {
        g_readback = YES;
        glGenTextures(1, &g_plainTex);
        glBindTexture(GL_TEXTURE_2D, g_plainTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, g_plainTex, 0);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        g_readTex = [g_dev newTextureWithDescriptor:td];
        g_pixels = malloc((size_t)w * h * 4);
        st = glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES);
    }
    if (st != GL_FRAMEBUFFER_COMPLETE_OES) { elf_log("present: framebuffer incomplete %#x", st); return -1; }
    g_cur = 0;
    elf_log("present: Metal, %s %dx%d", g_readback ? "read-back fallback (GL can't draw into shared images here)" : "3 shared GL/Metal images", w, h);
    return 0;
}

/// Binds the buffer the next frame renders into (one not on screen and not waiting to be shown).
void present_begin(void) {
    if (g_readback) { glBindFramebufferOES(GL_FRAMEBUFFER_OES, g_fbo); return; }
    int next = (g_last + 1) % NBUF;
    if (g_buf[next].fence) {   // its previous frame must have finished on the GPU
        glClientWaitSyncAPPLE(g_buf[next].fence, GL_SYNC_FLUSH_COMMANDS_BIT_APPLE, 100000000);
        glDeleteSyncAPPLE(g_buf[next].fence);
        g_buf[next].fence = NULL;
    }
    g_cur = next;
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &prev);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, g_fbo);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, g_buf[g_cur].tex, 0);
}

/// Marks the current frame done and shows the newest finished one.
void present_end(void) {
    if (g_readback) {
        GLint fb = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &fb);
        glBindFramebufferOES(GL_FRAMEBUFFER_OES, g_fbo);
        glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
        glBindFramebufferOES(GL_FRAMEBUFFER_OES, fb);
        [g_readTex replaceRegion:MTLRegionMake2D(0, 0, g_w, g_h) mipmapLevel:0 withBytes:g_pixels bytesPerRow:g_w * 4];
    }
    if (!g_readback) {
        g_buf[g_cur].fence = glFenceSyncAPPLE(GL_SYNC_GPU_COMMANDS_COMPLETE_APPLE, 0);
        glFlush();
        // GL work must be complete before Metal samples it: wait for this frame's fence (the GPU is fast; this
        // keeps latency at one frame instead of adding a frame of delay).
        glClientWaitSyncAPPLE(g_buf[g_cur].fence, GL_SYNC_FLUSH_COMMANDS_BIT_APPLE, 50000000);
        g_last = g_cur;
    }
    id<CAMetalDrawable> d = [g_layer nextDrawable];
    if (!d) return;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:g_pipe];
    [enc setFragmentTexture:g_readback ? g_readTex : CVMetalTextureGetTexture(g_buf[g_last].mtlTex) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb presentDrawable:d];
    [cb commit];
}


