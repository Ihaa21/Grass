#pragma once

#define VALIDATION 1

#include "framework_vulkan\framework_vulkan.h"

struct directional_light_gpu
{
    v3 Color;
    u32 Pad0;
    v3 Dir;
    u32 Pad1;
    v3 AmbientColor;
    u32 Pad2;
    m4 VPTransform;
};

struct shadow_directional_light
{
    directional_light_gpu GpuData;
    VkBuffer Globals;
    VkBuffer ShadowTransforms;
};

struct point_light
{
    v3 Color;
    u32 Pad0;
    v3 Pos;
    f32 MaxDistance;
};

struct scene_globals
{
    v3 CameraPos;
    u32 NumPointLights;
    m4 VPTransform;
    m4 VTransform;
};

struct gpu_instance_entry
{
    m4 WVTransform;
    m4 WVPTransform;
    v4 Color;
    float SpecularPower;
    u32 Pad[3];
};

struct instance_entry
{
    u32 MeshId;
    m4 ShadowWVP;
    gpu_instance_entry GpuData;
};

struct render_mesh
{
    vk_image Color;
    vk_image Normal;
    VkDescriptorSet MaterialDescriptor;
    
    VkBuffer VertexBuffer;
    VkBuffer IndexBuffer;
    u32 NumIndices;
};

struct render_scene;
struct renderer_create_info
{
    u32 Width;
    u32 Height;
    VkFormat ColorFormat;

    VkDescriptorSetLayout MaterialDescLayout;
    VkDescriptorSetLayout SceneDescLayout;
    render_scene* Scene;
};

#include "tiled_deferred.h"

struct render_scene
{
    // NOTE: General Render Data
    camera Camera;
    VkDescriptorSetLayout MaterialDescLayout;
    VkDescriptorSetLayout SceneDescLayout;
    VkBuffer SceneBuffer;
    VkDescriptorSet SceneDescriptor;

    shadow_directional_light DirectionalLight;

    // NOTE: Scene Lights
    u32 MaxNumPointLights;
    u32 NumPointLights;
    point_light* PointLights;
    VkBuffer PointLightBuffer;
    
    // NOTE: Scene Meshes
    u32 MaxNumRenderMeshes;
    u32 NumRenderMeshes;
    render_mesh* RenderMeshes;
    
    // NOTE: Opaque Instances
    u32 MaxNumOpaqueInstances;
    u32 NumOpaqueInstances;
    instance_entry* OpaqueInstances;
    VkBuffer OpaqueInstanceBuffer;
};

struct demo_state
{
    linear_arena Arena;
    linear_arena TempArena;

    // NOTE: Samplers
    VkSampler PointSampler;
    VkSampler LinearSampler;
    VkSampler AnisoSampler;

    // NOTE: Render Target Entries
    u32 RenderWidth;
    u32 RenderHeight;
    VkFormat RenderFormat;
    render_target_entry SwapChainEntry;
    render_target CopyToSwapTarget;
    VkDescriptorSet CopyToSwapDesc;
    vk_pipeline* CopyToSwapPipeline;

    render_scene Scene;
    
    // NOTE: Saved model ids
    u32 Quad;
    u32 Cube;
    u32 Sphere;
    
    tiled_deferred_state TiledDeferredState;
    
    ui_state UiState;
};

global demo_state* DemoState;
