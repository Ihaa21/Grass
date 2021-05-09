#pragma once

#define TILE_SIZE_IN_PIXELS 8
#define MAX_LIGHTS_PER_TILE 1024

struct tiled_deferred_globals
{
    // TODO: Move to camera?
    m4 InverseProjection;
    v2 ScreenSize;
    u32 GridSizeX;
    u32 GridSizeY;
};

struct directional_shadow
{
    vk_linear_arena Arena;
    
    u32 Width;
    u32 Height;
    VkSampler Sampler;
    VkImage Image;
    render_target_entry Entry;
    render_target Target;
    vk_pipeline* Pipeline;
};

struct grass_uniforms_gpu
{
    v2 WorldMin;
    v2 WorldMax;
    u32 NumBladesX;
    u32 NumBladesY;
    u32 MaterialId;
    u32 MaxBladeSegments;
    f32 MaxBendAngle; // NOTE: Multiplier of Pi/2
    f32 BladeCurvature;
    f32 BladeHeight;
    f32 BladeHeightVariance;
    f32 BladeWidth;
    f32 BladeWidthVariance;

    f32 WindTimeMult;
    f32 WindTexMult;
    f32 WindPosMult;
    f32 WindAmplitude;
    f32 CurrTime;
};

struct grass_draw_triangle_gpu
{
    v4 Vertices[3];
    u32 Pad0[12];
    v3 Normal;
    u32 Pad1;
};

struct indirect_args
{
    u32 NumVerticesPerInstance;
    u32 NumInstances;
    u32 StartVertexIndex;
    u32 StartInstanceIndex;
};

struct grass
{
    vk_linear_arena Arena;

    VkBuffer GrassTriangles;
    VkBuffer IndirectArg;
    
    grass_uniforms_gpu UniformsCpu;
    VkBuffer UniformsGpu;
    VkSampler WindSampler;
    vk_image WindNoiseTexture;

    VkDescriptorSetLayout DescLayout;
    VkDescriptorSet Descriptor;
    
    vk_pipeline* GenBladesPipeline;
    vk_pipeline* GBufferPipeline;
};

struct tiled_deferred_state
{
    vk_linear_arena RenderTargetArena;

    u32 RenderWidth;
    u32 RenderHeight;
    
    directional_shadow Shadow;
    grass Grass;
    
    // NOTE: GBuffer
    VkImage GBufferPositionImage;
    render_target_entry GBufferPositionEntry;
    VkImage GBufferNormalImage;
    render_target_entry GBufferNormalEntry;
    VkImage GBufferMaterialImage;
    render_target_entry GBufferMaterialEntry;
    VkImage DepthImage;
    render_target_entry DepthEntry;
    VkImage OutColorImage;
    render_target_entry OutColorEntry;
    render_target GBufferPass;
    render_target LightingPass;

    // NOTE: Global data
    VkBuffer TiledDeferredGlobals;
    VkBuffer GridFrustums;
    VkBuffer LightIndexList_O;
    VkBuffer LightIndexCounter_O;
    vk_image LightGrid_O;
    VkBuffer LightIndexList_T;
    VkBuffer LightIndexCounter_T;
    vk_image LightGrid_T;
    VkDescriptorSetLayout TiledDeferredDescLayout;
    VkDescriptorSet TiledDeferredDescriptor;

    render_mesh* QuadMesh;
    
    vk_pipeline* GridFrustumPipeline;
    vk_pipeline* GBufferPipeline;
    vk_pipeline* LightCullPipeline;
    vk_pipeline* LightingPipeline;
};

