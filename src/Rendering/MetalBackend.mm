#include "ClimaxEngine/Rendering/MetalBackend.h"

#ifdef __APPLE__

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <iostream>

static const char* s_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexInput {
    float3 position  [[attribute(0)]];
    float2 uv        [[attribute(1)]];
    float4 color     [[attribute(2)]];
};

struct VertexOutput {
    float4 position [[position]];
    float2 uv;
    float4 color;
    float3 fragWorldPos;
};

struct Uniforms {
    float4x4 mvp;
    float4x4 model;
    int      flipU;
    int      flipV;
    float2   uvOffset;
    float2   uvScale;
    int      useVertexColors;
    float    brightness;
    int      renderMode;
    float3   eyePos;
    float    depthMax;
};

vertex VertexOutput metal_vertex_main(VertexInput in [[stage_in]],
                                      constant Uniforms& uniforms [[buffer(1)]]) {
    VertexOutput out;
    out.position = uniforms.mvp * float4(in.position, 1.0);
    out.fragWorldPos = (uniforms.model * float4(in.position, 1.0)).xyz;
    float2 coord = in.uv;
    if (uniforms.flipU != 0) coord.x = 1.0 - coord.x;
    if (uniforms.flipV != 0) coord.y = 1.0 - coord.y;
    out.uv = (coord * uniforms.uvScale) + uniforms.uvOffset;
    out.color = in.color;
    return out;
}

fragment float4 metal_fragment_main(VertexOutput in [[stage_in]],
                                    constant Uniforms& uniforms [[buffer(1)]],
                                    texture2d<float> tex [[texture(0)]],
                                    sampler sam [[sampler(0)]]) {
    float3 dx = dfdx(in.fragWorldPos);
    float3 dy = dfdy(in.fragWorldPos);
    float3 N  = normalize(cross(dx, dy));

    if (uniforms.renderMode == 1) { // VertexColor
        if (in.color.a < 0.05) discard_fragment();
        return float4(in.color.rgb * uniforms.brightness, in.color.a);
    } else if (uniforms.renderMode == 2) { // FlatShaded
        float3 L = normalize(float3(0.55, 1.0, 0.45));
        float d = max(dot(N, L), 0.0) * 0.72 + 0.28;
        return float4(float3(0.70, 0.72, 0.76) * d * uniforms.brightness, 1.0);
    } else if (uniforms.renderMode == 3) { // Normals
        return float4(N * 0.5 + 0.5, 1.0);
    } else if (uniforms.renderMode == 4) { // Depth
        float dist = distance(in.fragWorldPos, uniforms.eyePos);
        float v = clamp(1.0 - dist / uniforms.depthMax, 0.0, 1.0);
        v = v * v;
        return float4(float3(v), 1.0);
    } else if (uniforms.renderMode == 5) { // Checker
        float2 ch = floor(in.uv * 8.0);
        float c = fmod(ch.x + ch.y, 2.0) < 1.0 ? 0.82 : 0.18;
        return float4(float3(c), 1.0);
    } else if (uniforms.renderMode == 6) { // Unlit
        float4 color = tex.sample(sam, in.uv);
        if (color.a < 0.1) discard_fragment();
        return float4(color.rgb * uniforms.brightness, color.a);
    } else { // Textured (Default)
        float4 color = tex.sample(sam, in.uv);
        if (color.a < 0.1) discard_fragment();
        float3 col = color.rgb;
        if (uniforms.useVertexColors != 0) col *= in.color.rgb;
        float3 L = normalize(float3(0.55, 1.0, 0.45));
        float d = max(dot(N, L), 0.0) * 0.45 + 0.55;
        return float4(col * d * uniforms.brightness, color.a);
    }
}
)";

struct MetalBackend::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commandQueue = nil;
    id<MTLRenderPipelineState> pipelineState = nil;
    id<MTLDepthStencilState> depthState = nil;
    CAMetalLayer* metalLayer = nil;
    id<CAMetalDrawable> currentDrawable = nil;
    id<MTLCommandBuffer> commandBuffer = nil;
    id<MTLRenderCommandEncoder> renderEncoder = nil;
    RenderUniforms currentUniforms;
};

MetalBackend::MetalBackend() {
    m_Impl = new Impl();
}

MetalBackend::~MetalBackend() {
    Shutdown();
    delete m_Impl;
    m_Impl = nullptr;
}

bool MetalBackend::Init(void* /*windowHandle*/) {
    m_Impl->device = MTLCreateSystemDefaultDevice();
    if (!m_Impl->device) {
        std::cerr << "[MetalBackend] Metal is not supported on this device." << std::endl;
        return false;
    }
    m_Impl->commandQueue = [m_Impl->device newCommandQueue];

    NSError* error = nil;
    id<MTLLibrary> library = [m_Impl->device newLibraryWithSource:[NSString stringWithUTF8String:s_MSL]
                                                          options:nil
                                                            error:&error];
    if (!library) {
        std::cerr << "[MetalBackend] MSL compilation failed: "
                  << [[error localizedDescription] UTF8String] << std::endl;
        return false;
    }

    id<MTLFunction> vertFunc = [library newFunctionWithName:@"metal_vertex_main"];
    id<MTLFunction> fragFunc = [library newFunctionWithName:@"metal_fragment_main"];

    MTLRenderPipelineDescriptor* pdesc = [[MTLRenderPipelineDescriptor alloc] init];
    pdesc.vertexFunction = vertFunc;
    pdesc.fragmentFunction = fragFunc;
    pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pdesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    m_Impl->pipelineState = [m_Impl->device newRenderPipelineStateWithDescriptor:pdesc error:&error];

    MTLDepthStencilDescriptor* ddesc = [[MTLDepthStencilDescriptor alloc] init];
    ddesc.depthCompareFunction = MTLCompareFunctionLessEqual;
    ddesc.depthWriteEnabled = YES;
    m_Impl->depthState = [m_Impl->device newDepthStencilStateWithDescriptor:ddesc];

    std::cout << "[MetalBackend] Initialized native Apple Metal backend on "
              << [[m_Impl->device name] UTF8String] << std::endl;
    return true;
}

void MetalBackend::Shutdown() {
    if (m_Impl) {
        m_Impl->pipelineState = nil;
        m_Impl->depthState = nil;
        m_Impl->commandQueue = nil;
        m_Impl->device = nil;
    }
}

void MetalBackend::BeginFrame(int /*width*/, int /*height*/, float clearR, float clearG, float clearB) {
    if (!m_Impl || !m_Impl->commandQueue) return;

    m_Impl->commandBuffer = [m_Impl->commandQueue commandBuffer];
}

void MetalBackend::EndFrame() {
    if (m_Impl && m_Impl->commandBuffer) {
        [m_Impl->commandBuffer commit];
        m_Impl->commandBuffer = nil;
    }
}

void MetalBackend::SetUniforms(const RenderUniforms& uniforms) {
    if (m_Impl) m_Impl->currentUniforms = uniforms;
}

void MetalBackend::BindTexture(uint32_t /*textureID*/) {}

void MetalBackend::DrawMesh(uint32_t /*vao*/, uint32_t /*vertexCount*/) {}

void MetalBackend::DrawLines(uint32_t /*vao*/, uint32_t /*vertexCount*/) {}

#endif
