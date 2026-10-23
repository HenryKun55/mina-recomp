// Logs the original game's Metal draws for one frame, when the file
// MTLTRACE_TRIGGER exists (removed once the frame is written), to MTLTRACE_OUT.
// Per draw: primitive count, the fragment texture 0's size and pointer, the
// depth/stencil state's compare/write, the vertex constants (buffers 0 and 1
// as floats). The reference for what our Metal emulation must draw.
//
//   clang -dynamiclib -fobjc-arc -framework Metal -framework QuartzCore \
//         -framework Foundation -o mtltrace.dylib tools/mac/mtltrace.m
//   cp -R <the game's .app> orig.app            # a copy: the original stays signed
//   codesign --remove-signature orig.app/Contents/MacOS/MinaTheHollower
//   codesign -f -s - orig.app/Contents/MacOS/MinaTheHollower   # no hardened runtime
//   DYLD_INSERT_LIBRARIES=$PWD/mtltrace.dylib MTLTRACE_TRIGGER=/tmp/t MTLTRACE_OUT=trace.txt \
//         orig.app/Contents/MacOS/MinaTheHollower
//   touch /tmp/t     # at the moment to capture; compare with MINA_FRAMELOG
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

static FILE *out;
static volatile long n_next, n_draw, n_rpe, n_present;
static int active, frame_no;
static const char *trigger, *outpath;
static NSMapTable *ds_info;      // MTLDepthStencilState -> NSString "cmp W w X"
static id cur_ds, cur_tex0;
static const void *vcb[2]; static NSUInteger vcb_len[2];
static id vbuf0; static NSUInteger vbuf0_off;

static void swizzle(Class c, SEL s, IMP repl, IMP *orig)
{
    Method m = class_getInstanceMethod(c, s);
    if (!m) { fprintf(stderr, "[mtltrace] no %s on %s\n", sel_getName(s), class_getName(c)); return; }
    *orig = method_setImplementation(m, repl);
}

static IMP o_next;
static id h_next(id self, SEL _cmd)
{
    frame_no++; n_next++;
    if (active) { active = 0; fflush(out); unlink(trigger); fprintf(stderr, "[mtltrace] frame %d written\n", frame_no - 1); }
    else if (access(trigger, F_OK) == 0) active = 1;
    return ((id (*)(id, SEL))o_next)(self, _cmd);
}
static IMP o_newDS, o_setDS, o_setFTex, o_setVBytes, o_setVBuf, o_drawIdx, o_drawPrim, o_present, o_rpe;

static id h_newDS(id self, SEL _cmd, MTLDepthStencilDescriptor *d)
{
    id st = ((id (*)(id, SEL, id))o_newDS)(self, _cmd, d);
    NSString *info = [NSString stringWithFormat:@"cmp %d w %d st %d/%d/%d", (int)d.depthCompareFunction,
                      (int)d.depthWriteEnabled, (int)d.frontFaceStencil.stencilCompareFunction,
                      (int)d.frontFaceStencil.depthStencilPassOperation, (int)d.frontFaceStencil.writeMask];
    @synchronized(ds_info) { [ds_info setObject:info forKey:st]; }
    return st;
}
static void h_setDS(id self, SEL _cmd, id st) { cur_ds = st; ((void (*)(id, SEL, id))o_setDS)(self, _cmd, st); }
static void h_setFTex(id self, SEL _cmd, id t, NSUInteger i)
{
    if (i == 0) cur_tex0 = t;
    ((void (*)(id, SEL, id, NSUInteger))o_setFTex)(self, _cmd, t, i);
}
static void h_setVBytes(id self, SEL _cmd, const void *b, NSUInteger len, NSUInteger i)
{
    if (active && i < 2) { free((void *)vcb[i]); vcb[i] = malloc(len); memcpy((void *)vcb[i], b, len); vcb_len[i] = len; }
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))o_setVBytes)(self, _cmd, b, len, i);
}
static void h_setVBuf(id self, SEL _cmd, id buf, NSUInteger off, NSUInteger i)
{
    if (active) {
        if (i < 2 && buf && [(id<MTLBuffer>)buf storageMode] != MTLStorageModePrivate) {
            free((void *)vcb[i]); NSUInteger n = [(id<MTLBuffer>)buf length] - off; if (n > 256) n = 256;
            vcb[i] = malloc(n); memcpy((void *)vcb[i], (char *)[(id<MTLBuffer>)buf contents] + off, n); vcb_len[i] = n;
        }
        if (i == 30 || i == 2 || i == 3) { vbuf0 = buf; vbuf0_off = off; }
    }
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))o_setVBuf)(self, _cmd, buf, off, i);
}
static void log_draw(const char *kind, NSUInteger n)
{
    n_draw++;
    NSString *ds = cur_ds ? [ds_info objectForKey:cur_ds] : @"-";
    fprintf(out, "[orig %d] %s %lu tex0 %lux%lu %p ds %s", frame_no, kind, (unsigned long)n,
            cur_tex0 ? (unsigned long)[cur_tex0 width] : 0, cur_tex0 ? (unsigned long)[cur_tex0 height] : 0,
            (__bridge void *)cur_tex0, ds ? ds.UTF8String : "?");
    for (int k = 0; k < 2; k++) {
        fprintf(out, " vcb%d[", k);
        const float *f = vcb[k];
        for (NSUInteger j = 0; f && j < vcb_len[k] / 4 && j < 36; j++) fprintf(out, "%s%g", j ? " " : "", f[j]);
        fprintf(out, "]");
    }
    fprintf(out, "\n");
}
static void h_drawIdx(id self, SEL _cmd, MTLPrimitiveType t, NSUInteger n, MTLIndexType it, id ib, NSUInteger ibo)
{
    n_draw++; if (active) log_draw("indexed", n);
    ((void (*)(id, SEL, MTLPrimitiveType, NSUInteger, MTLIndexType, id, NSUInteger))o_drawIdx)(self, _cmd, t, n, it, ib, ibo);
}
static void h_drawPrim(id self, SEL _cmd, MTLPrimitiveType t, NSUInteger s, NSUInteger n)
{
    n_draw++; if (active) log_draw("arrays", n);
    ((void (*)(id, SEL, MTLPrimitiveType, NSUInteger, NSUInteger))o_drawPrim)(self, _cmd, t, s, n);
}
static id h_rpe(id self, SEL _cmd, MTLRenderPassDescriptor *d)
{
    n_rpe++;
    if (active) {
        id<MTLTexture> c0 = d.colorAttachments[0].texture, dt = d.depthAttachment.texture;
        fprintf(out, "[orig %d] pass %lux%lu c0 %p load %d depth %p load %d clear %g\n", frame_no,
                (unsigned long)[c0 width], (unsigned long)[c0 height], (__bridge void *)c0,
                (int)d.colorAttachments[0].loadAction, (__bridge void *)dt, (int)d.depthAttachment.loadAction,
                d.depthAttachment.clearDepth);
    }
    return ((id (*)(id, SEL, id))o_rpe)(self, _cmd, d);
}
static void h_present(id self, SEL _cmd, id drawable)
{
    ((void (*)(id, SEL, id))o_present)(self, _cmd, drawable);
    frame_no++;
    if (active) { active = 0; fflush(out); unlink(trigger); fprintf(stderr, "[mtltrace] frame %d written\n", frame_no - 1); }
    else if (access(trigger, F_OK) == 0) active = 1;
}

__attribute__((constructor)) static void init(void)
{
    trigger = getenv("MTLTRACE_TRIGGER");
    outpath = getenv("MTLTRACE_OUT");
    if (!trigger || !outpath) return;
    out = fopen(outpath, "a");
    ds_info = [NSMapTable weakToStrongObjectsMapTable];
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    swizzle([dev class], @selector(newDepthStencilStateWithDescriptor:), (IMP)h_newDS, &o_newDS);
    swizzle([cb class], @selector(renderCommandEncoderWithDescriptor:), (IMP)h_rpe, &o_rpe);
    swizzle([CAMetalLayer class], @selector(nextDrawable), (IMP)h_next, &o_next);
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    Class ec = [e class];
    [e endEncoding];
    swizzle(ec, @selector(setDepthStencilState:), (IMP)h_setDS, &o_setDS);
    swizzle(ec, @selector(setFragmentTexture:atIndex:), (IMP)h_setFTex, &o_setFTex);
    swizzle(ec, @selector(setVertexBytes:length:atIndex:), (IMP)h_setVBytes, &o_setVBytes);
    swizzle(ec, @selector(setVertexBuffer:offset:atIndex:), (IMP)h_setVBuf, &o_setVBuf);
    swizzle(ec, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:), (IMP)h_drawIdx, &o_drawIdx);
    swizzle(ec, @selector(drawPrimitives:vertexStart:vertexCount:), (IMP)h_drawPrim, &o_drawPrim);
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        for (;;) { sleep(3); fprintf(stderr, "[mtltrace] next %ld draws %ld passes %ld\n", n_next, n_draw, n_rpe); }
    });
    fprintf(stderr, "[mtltrace] encoder %s, command buffer %s, device %s\n", class_getName(ec),
            class_getName([cb class]), class_getName([dev class]));
}
