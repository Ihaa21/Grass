
#include "grass_demo.h"
#include "tiled_deferred.cpp"

//
// NOTE: Asset Storage System
//

inline u32 SceneMeshAdd(render_scene* Scene, vk_image Color, vk_image Normal, VkBuffer VertexBuffer, VkBuffer IndexBuffer, u32 NumIndices)
{
    Assert(Scene->NumRenderMeshes < Scene->MaxNumRenderMeshes);
    
    u32 MeshId = Scene->NumRenderMeshes++;
    render_mesh* Mesh = Scene->RenderMeshes + MeshId;
    Mesh->Color = Color;
    Mesh->Normal = Normal;
    Mesh->VertexBuffer = VertexBuffer;
    Mesh->IndexBuffer = IndexBuffer;
    Mesh->NumIndices = NumIndices;
    Mesh->MaterialDescriptor = VkDescriptorSetAllocate(RenderState->Device, RenderState->DescriptorPool, Scene->MaterialDescLayout);
    VkDescriptorImageWrite(&RenderState->DescriptorManager, Mesh->MaterialDescriptor, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           Color.View, DemoState->PointSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageWrite(&RenderState->DescriptorManager, Mesh->MaterialDescriptor, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           Normal.View, DemoState->PointSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return MeshId;
}

inline u32 SceneMeshAdd(render_scene* Scene, vk_image Color, vk_image Normal, procedural_mesh Mesh)
{
    u32 Result = SceneMeshAdd(Scene, Color, Normal, Mesh.Vertices, Mesh.Indices, Mesh.NumIndices);
    return Result;
}

inline void SceneOpaqueInstanceAdd(render_scene* Scene, u32 MeshId, m4 WTransform, v4 Color, float SpecularPower)
{
    Assert(Scene->NumOpaqueInstances < Scene->MaxNumOpaqueInstances);

    instance_entry* Instance = Scene->OpaqueInstances + Scene->NumOpaqueInstances++;
    Instance->MeshId = MeshId;
    Instance->ShadowWVP = Scene->DirectionalLight.GpuData.VPTransform * WTransform;
    Instance->GpuData.WVTransform = CameraGetV(&Scene->Camera)*WTransform;
    Instance->GpuData.WVPTransform = CameraGetP(&Scene->Camera)*Instance->GpuData.WVTransform;
    Instance->GpuData.Color = Color;
    Instance->GpuData.SpecularPower = SpecularPower;
}

inline void ScenePointLightAdd(render_scene* Scene, v3 Pos, v3 Color, f32 MaxDistance)
{
    Assert(Scene->NumPointLights < Scene->MaxNumPointLights);

    // TODO: Specify strength or a sphere so that we can visualize nicely too?
    point_light* PointLight = Scene->PointLights + Scene->NumPointLights++;
    PointLight->Pos = Pos;
    PointLight->Color = Color;
    PointLight->MaxDistance = MaxDistance;
}

inline void SceneDirectionalLightSet(render_scene* Scene, v3 LightDir, v3 Color, v3 AmbientColor, v3 BoundsMin, v3 BoundsMax)
{
    // NOTE: Lighting is done in camera space
    Scene->DirectionalLight.GpuData.Dir = (CameraGetV(&Scene->Camera) * V4(LightDir, 0)).xyz;
    Scene->DirectionalLight.GpuData.Color = Color;
    Scene->DirectionalLight.GpuData.AmbientColor = AmbientColor;

    v3 Up = V3(0, 1, 0);
    f32 DotValue = Abs(Dot(Up, LightDir));
    if (DotValue > 0.99f && DotValue < 1.01f)
    {
        Up = V3(1, 0, 0);
    }

    Scene->DirectionalLight.GpuData.VPTransform = (VkOrthoProjM4(BoundsMin.x, BoundsMax.x, BoundsMax.y, BoundsMin.y, BoundsMin.z, BoundsMax.z) *
                                                   LookAtM4(LightDir, Up, V3(0, 0, 0)));
}

//
// NOTE: Demo Code
//

inline void DemoAllocGlobals(linear_arena* Arena)
{
    // IMPORTANT: These are always the top of the program memory
    DemoState = PushStruct(Arena, demo_state);
    RenderState = PushStruct(Arena, render_state);
}

DEMO_INIT(Init)
{
    // NOTE: Init Memory
    {
        linear_arena Arena = LinearArenaCreate(ProgramMemory, ProgramMemorySize);
        DemoAllocGlobals(&Arena);
        *DemoState = {};
        *RenderState = {};
        DemoState->Arena = Arena;
        DemoState->TempArena = LinearSubArena(&DemoState->Arena, MegaBytes(10));
    }

    // NOTE: Init Vulkan
    {
        {
            const char* DeviceExtensions[] =
            {
                "VK_EXT_shader_viewport_index_layer",
            };
            
            render_init_params InitParams = {};
            InitParams.ValidationEnabled = true;
            InitParams.WindowWidth = WindowWidth;
            InitParams.WindowHeight = WindowHeight;
            InitParams.GpuLocalSize = MegaBytes(400);
            InitParams.DeviceExtensionCount = ArrayCount(DeviceExtensions);
            InitParams.DeviceExtensions = DeviceExtensions;
            VkInit(VulkanLib, hInstance, WindowHandle, &DemoState->Arena, &DemoState->TempArena, InitParams);
        }
    }
    
    // NOTE: Create samplers
    DemoState->PointSampler = VkSamplerCreate(RenderState->Device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, 0.0f);
    DemoState->LinearSampler = VkSamplerCreate(RenderState->Device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, 0.0f);
    DemoState->AnisoSampler = VkSamplerMipMapCreate(RenderState->Device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 16.0f,
                                                    VK_SAMPLER_MIPMAP_MODE_LINEAR, 0, 0, 5);    
        
    // NOTE: Init render target entries
    DemoState->SwapChainEntry = RenderTargetSwapChainEntryCreate(RenderState->WindowWidth, RenderState->WindowHeight,
                                                                 RenderState->SwapChainFormat);

    // NOTE: Copy To Swap RT
    {
        render_target_builder Builder = RenderTargetBuilderBegin(&DemoState->Arena, &DemoState->TempArena, RenderState->WindowWidth,
                                                                 RenderState->WindowHeight);
        RenderTargetAddTarget(&Builder, &DemoState->SwapChainEntry, VkClearColorCreate(0, 0, 0, 1));
                            
        vk_render_pass_builder RpBuilder = VkRenderPassBuilderBegin(&DemoState->TempArena);

        u32 ColorId = VkRenderPassAttachmentAdd(&RpBuilder, RenderState->SwapChainFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED,
                                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderPassSubPassBegin(&RpBuilder, VK_PIPELINE_BIND_POINT_GRAPHICS);
        VkRenderPassColorRefAdd(&RpBuilder, ColorId, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderPassSubPassEnd(&RpBuilder);

        DemoState->CopyToSwapTarget = RenderTargetBuilderEnd(&Builder, VkRenderPassBuilderEnd(&RpBuilder, RenderState->Device));
        DemoState->CopyToSwapDesc = VkDescriptorSetAllocate(RenderState->Device, RenderState->DescriptorPool, RenderState->CopyImageDescLayout);
        DemoState->CopyToSwapPipeline = FullScreenCopyImageCreate(DemoState->CopyToSwapTarget.RenderPass, 0);
    }

    // NOTE: Init scene system
    {
        render_scene* Scene = &DemoState->Scene;

        Scene->Camera = CameraFpsCreate(V3(0, 0, -5), V3(0, 0, 1), true, 1.0f, 0.05f);
        CameraSetPersp(&Scene->Camera, f32(RenderState->WindowWidth / RenderState->WindowHeight), 90.0f, 0.01f, 1000.0f);

        Scene->SceneBuffer = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            sizeof(scene_globals));

        Scene->MaxNumRenderMeshes = 1000;
        Scene->RenderMeshes = PushArray(&DemoState->Arena, render_mesh, Scene->MaxNumRenderMeshes);

        Scene->MaxNumOpaqueInstances = 1000;
        Scene->OpaqueInstances = PushArray(&DemoState->Arena, instance_entry, Scene->MaxNumOpaqueInstances);
        Scene->OpaqueInstanceBuffer = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     sizeof(gpu_instance_entry)*Scene->MaxNumOpaqueInstances);
        
        Scene->MaxNumPointLights = 1000;
        Scene->PointLights = PushArray(&DemoState->Arena, point_light, Scene->MaxNumPointLights);
        Scene->PointLightBuffer = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 sizeof(point_light)*Scene->MaxNumPointLights);

        Scene->DirectionalLight.Globals = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         sizeof(directional_light_gpu));
        Scene->DirectionalLight.ShadowTransforms = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                                  sizeof(m4)*Scene->MaxNumOpaqueInstances);
        
        // NOTE: Create general descriptor set layouts
        {
            {
                vk_descriptor_layout_builder Builder = VkDescriptorLayoutBegin(&Scene->MaterialDescLayout);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
                VkDescriptorLayoutEnd(RenderState->Device, &Builder);
            }

            {
                vk_descriptor_layout_builder Builder = VkDescriptorLayoutBegin(&Scene->SceneDescLayout);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
                VkDescriptorLayoutEnd(RenderState->Device, &Builder);
            }
        }

        // NOTE: Populate descriptors
        Scene->SceneDescriptor = VkDescriptorSetAllocate(RenderState->Device, RenderState->DescriptorPool, Scene->SceneDescLayout);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Scene->SceneBuffer);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Scene->OpaqueInstanceBuffer);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Scene->PointLightBuffer);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Scene->DirectionalLight.Globals);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Scene->DirectionalLight.ShadowTransforms);
    }

    // NOTE: Create render data
    DemoState->RenderWidth = 1920;
    DemoState->RenderHeight = 1080;
    DemoState->RenderFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    {
        renderer_create_info CreateInfo = {};
        CreateInfo.Width = DemoState->RenderWidth;
        CreateInfo.Height = DemoState->RenderHeight;
        CreateInfo.ColorFormat = DemoState->RenderFormat;
        CreateInfo.MaterialDescLayout = DemoState->Scene.MaterialDescLayout;
        CreateInfo.SceneDescLayout = DemoState->Scene.SceneDescLayout;
        CreateInfo.Scene = &DemoState->Scene;
        TiledDeferredCreate(CreateInfo, &DemoState->CopyToSwapDesc, &DemoState->TiledDeferredState);
    }
    
    // NOTE: Upload assets
    vk_commands* Commands = &RenderState->Commands;
    VkCommandsBegin(Commands, RenderState->Device);
    {
        render_scene* Scene = &DemoState->Scene;
        
        // NOTE: Push textures
        vk_image WhiteTexture = {};
        {
            u32 Texels[] =
            {
                0xFFFFFFFF,
            };

            u32 Dim = 1;
            u32 ImageSize = Dim*Dim*sizeof(u32);
            WhiteTexture = VkImageCreate(RenderState->Device, &RenderState->GpuArena, Dim, Dim, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

            // TODO: Better barrier here pls
            u8* GpuMemory = VkCommandsPushWriteImage(Commands, WhiteTexture.Image, Dim, Dim, ImageSize,
                                                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                     BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
                                                     BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));

            Copy(Texels, GpuMemory, ImageSize);
        }

        vk_image CheckerBoardTexture = {};
        {
            u32 Texels[] =
            {
                0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 
                0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
                0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 
                0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
                0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 
                0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
                0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 
                0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
            };

            u32 Dim = 8;
            u32 ImageSize = Dim*Dim*sizeof(u32);
            CheckerBoardTexture = VkImageCreate(RenderState->Device, &RenderState->GpuArena, Dim, Dim, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

            // TODO: Better barrier here pls
            u8* GpuMemory = VkCommandsPushWriteImage(Commands, CheckerBoardTexture.Image, Dim, Dim, ImageSize,
                                                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                     BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
                                                     BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));

            Copy(Texels, GpuMemory, ImageSize);
        }
        
        // NOTE: Push meshes
        DemoState->Quad = SceneMeshAdd(Scene, WhiteTexture, WhiteTexture, AssetsPushQuad());
        DemoState->Cube = SceneMeshAdd(Scene, WhiteTexture, WhiteTexture, AssetsPushCube());
        DemoState->Sphere = SceneMeshAdd(Scene, WhiteTexture, WhiteTexture, AssetsPushSphere(64, 64));
        TiledDeferredAddMeshes(&DemoState->TiledDeferredState, Scene->RenderMeshes + DemoState->Quad);

        // NOTE: Create UI
        UiStateCreate(RenderState->Device, &DemoState->Arena, &DemoState->TempArena, RenderState->LocalMemoryId,
                      &RenderState->DescriptorManager, &RenderState->PipelineManager, Commands,
                      RenderState->SwapChainFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &DemoState->UiState);
        
        VkDescriptorManagerFlush(RenderState->Device, &RenderState->DescriptorManager);
    }
    
    VkCommandsSubmit(Commands, RenderState->Device, RenderState->GraphicsQueue);
}

DEMO_DESTROY(Destroy)
{
}

DEMO_SWAPCHAIN_CHANGE(SwapChainChange)
{
    VkCheckResult(vkDeviceWaitIdle(RenderState->Device));
    VkSwapChainReCreate(&DemoState->TempArena, WindowWidth, WindowHeight, RenderState->PresentMode);

    DemoState->SwapChainEntry.Width = RenderState->WindowWidth;
    DemoState->SwapChainEntry.Height = RenderState->WindowHeight;

    DemoState->Scene.Camera.PerspAspectRatio = f32(RenderState->WindowWidth / RenderState->WindowHeight);
}

DEMO_CODE_RELOAD(CodeReload)
{
    linear_arena Arena = LinearArenaCreate(ProgramMemory, ProgramMemorySize);
    // IMPORTANT: We are relying on the memory being the same here since we have the same base ptr with the VirtualAlloc so we just need
    // to patch our global pointers here
    DemoAllocGlobals(&Arena);

    VkGetGlobalFunctionPointers(VulkanLib);
    VkGetInstanceFunctionPointers();
    VkGetDeviceFunctionPointers();
}

DEMO_MAIN_LOOP(MainLoop)
{
    u32 ImageIndex;
    VkCheckResult(vkAcquireNextImageKHR(RenderState->Device, RenderState->SwapChain, UINT64_MAX, RenderState->ImageAvailableSemaphore,
                                        VK_NULL_HANDLE, &ImageIndex));
    DemoState->SwapChainEntry.View = RenderState->SwapChainViews[ImageIndex];

    vk_commands* Commands = &RenderState->Commands;
    VkCommandsBegin(Commands, RenderState->Device);

    // NOTE: Update pipelines
    VkPipelineUpdateShaders(RenderState->Device, &RenderState->CpuArena, &RenderState->PipelineManager);

    RenderTargetUpdateEntries(&DemoState->TempArena, &DemoState->CopyToSwapTarget);
    
    // NOTE: Update Ui State
    {
        ui_state* UiState = &DemoState->UiState;
        
        ui_frame_input UiCurrInput = {};
        UiCurrInput.MouseDown = CurrInput->MouseDown;
        UiCurrInput.MousePixelPos = V2(CurrInput->MousePixelPos);
        UiCurrInput.MouseScroll = CurrInput->MouseScroll;
        Copy(CurrInput->KeysDown, UiCurrInput.KeysDown, sizeof(UiCurrInput.KeysDown));
        UiStateBegin(UiState, FrameTime, RenderState->WindowWidth, RenderState->WindowHeight, UiCurrInput);
        local_global v2 PanelPos = V2(100, 800);
        ui_panel Panel = UiPanelBegin(UiState, &PanelPos, "Snow Panel");

        grass_uniforms_gpu* Uniforms = &DemoState->TiledDeferredState.Grass.UniformsCpu;
        
        {
            UiPanelText(&Panel, "World Min:");
            UiPanelNextRowIndent(&Panel);
            UiPanelText(&Panel, "X:");
            UiPanelNumberBox(&Panel, &Uniforms->WorldMin.x);
            UiPanelText(&Panel, "Y:");
            UiPanelNumberBox(&Panel, &Uniforms->WorldMin.y);
            UiPanelNextRow(&Panel);
        }
        
        {
            UiPanelText(&Panel, "World Max:");
            UiPanelNextRowIndent(&Panel);
            UiPanelText(&Panel, "X:");
            UiPanelNumberBox(&Panel, &Uniforms->WorldMax.x);
            UiPanelText(&Panel, "Y:");
            UiPanelNumberBox(&Panel, &Uniforms->WorldMax.y);
            UiPanelNextRow(&Panel);
        }

        // TODO: Add text boxes for uint
        
        {
            UiPanelText(&Panel, "Blade Curvature:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 1, &Uniforms->BladeCurvature);
            UiPanelNumberBox(&Panel, 0, 1, &Uniforms->BladeCurvature);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Max Bend Angle:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 1, &Uniforms->MaxBendAngle);
            UiPanelNumberBox(&Panel, 0, 1, &Uniforms->MaxBendAngle);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Blade Width:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 4, &Uniforms->BladeWidth);
            UiPanelNumberBox(&Panel, 0, 1, &Uniforms->BladeWidth);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Blade Width Variance:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 4, &Uniforms->BladeWidthVariance);
            UiPanelNumberBox(&Panel, 0, 1, &Uniforms->BladeWidthVariance);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Blade Height:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 4, &Uniforms->BladeHeight);
            UiPanelNumberBox(&Panel, 0, 1, &Uniforms->BladeHeight);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Blade Height Variance:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 1, &Uniforms->BladeHeightVariance);
            UiPanelNumberBox(&Panel, 0, 1, &Uniforms->BladeHeightVariance);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Wind Time Mult:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 4, &Uniforms->WindTimeMult);
            UiPanelNumberBox(&Panel, 0, 4, &Uniforms->WindTimeMult);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Wind Tex Mult:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 4, &Uniforms->WindTexMult);
            UiPanelNumberBox(&Panel, 0, 4, &Uniforms->WindTexMult);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Wind Pos Mult:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 4, &Uniforms->WindPosMult);
            UiPanelNumberBox(&Panel, 0, 4, &Uniforms->WindPosMult);
            UiPanelNextRow(&Panel);
        }

        {
            UiPanelText(&Panel, "Wind Amplitude:");
            UiPanelNextRowIndent(&Panel);
            UiPanelHorizontalSlider(&Panel, 0, 4, &Uniforms->WindAmplitude);
            UiPanelNumberBox(&Panel, 0, 4, &Uniforms->WindAmplitude);
            UiPanelNextRow(&Panel);
        }

        UiPanelEnd(&Panel);
        
        UiStateEnd(UiState, &RenderState->DescriptorManager);
    }

    // NOTE: Upload scene data
    {
        render_scene* Scene = &DemoState->Scene;
        Scene->NumOpaqueInstances = 0;
        Scene->NumPointLights = 0;
        if (!(DemoState->UiState.MouseTouchingUi || DemoState->UiState.ProcessedInteraction))
        {
            CameraUpdate(&Scene->Camera, CurrInput, PrevInput);
        }

        //
        // NOTE: Populate scene
        //
        {
            //v3 LightDir = Normalize(V3(0.4f, -1.0f, 0.0f));
            v3 LightDir = Normalize(V3(1.0f, 0.0f, 0.0f));
            
            SceneDirectionalLightSet(Scene, LightDir, V3(1.0f, 1.0f, 1.0f), V3(0.4),
                                     V3(-50.0f, -10.0f, -50.0f), V3(50.0f, 10.0f, 50.0f));

            v4 GrassColor = (1.0f / 255.0f) * V4(54.0f, 102.0f, 80.0f, 255.0f);
            v4 PillarColor = (1.0f / 255.0f) * V4(41.0f, 74.0f, 86.0f, 255.0f);
            
            SceneOpaqueInstanceAdd(Scene, DemoState->Cube, M4Pos(V3(0.0f, -3.0f, 0.0f)) * M4Scale(V3(100.0f, 1.0f, 100.0f)),
                                   GrassColor, 2);

            //SceneOpaqueInstanceAdd(Scene, DemoState->Grass, M4Pos(V3(2.0f, 0.0f, 0.0f)) * M4Scale(V3(2.0f)),
            //                       GrassColor, 2);

            SceneOpaqueInstanceAdd(Scene, DemoState->Cube, M4Pos(V3(2.0f, 0.0f, 2.0f)) * M4Scale(V3(1.0f, 5.0f, 1.0f)),
                                   PillarColor, 2);

            SceneOpaqueInstanceAdd(Scene, DemoState->Sphere, M4Pos(V3(0.0f, 0.0f, 0.0f)) * M4Scale(V3(1.0f)),
                                   V4(0.0f, 0.0f, 1.0f, 1.0f), 2);
        }

        // NOTE: Push opaque instances
        if (Scene->NumOpaqueInstances > 0)
        {
            gpu_instance_entry* GpuData = VkCommandsPushWriteArray(Commands, Scene->OpaqueInstanceBuffer, gpu_instance_entry, Scene->NumOpaqueInstances,
                                                                   BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                   BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));

            for (u32 InstanceId = 0; InstanceId < Scene->NumOpaqueInstances; ++InstanceId)
            {
                GpuData[InstanceId] = Scene->OpaqueInstances[InstanceId].GpuData;
            }
        }
        
        // NOTE: Push Point Lights
        if (Scene->NumPointLights > 0)
        {
            point_light* PointLights = VkCommandsPushWriteArray(Commands, Scene->PointLightBuffer, point_light, Scene->NumPointLights,
                                                                BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));

            for (u32 LightId = 0; LightId < Scene->NumPointLights; ++LightId)
            {
                point_light* CurrLight = Scene->PointLights + LightId;
                PointLights[LightId] = *CurrLight;
                // NOTE: Convert to view space
                v4 Test = CameraGetV(&Scene->Camera) * V4(CurrLight->Pos, 1.0f);
                PointLights[LightId].Pos = (CameraGetV(&Scene->Camera) * V4(CurrLight->Pos, 1.0f)).xyz;
            }
        }

        // NOTE: Push Directional Lights
        {
            {
                directional_light_gpu* GpuData = VkCommandsPushWriteStruct(Commands, Scene->DirectionalLight.Globals, directional_light_gpu,
                                                                           BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                           BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
                Copy(&Scene->DirectionalLight.GpuData, GpuData, sizeof(directional_light_gpu));

                // NOTE: Undo camera transform since GBuffer is stored in view space
                GpuData->VPTransform = Scene->DirectionalLight.GpuData.VPTransform*Inverse(CameraGetV(&Scene->Camera));
            }
            
            // NOTE: Copy shadow data
            {
                m4* GpuData = VkCommandsPushWriteArray(Commands, Scene->DirectionalLight.ShadowTransforms, m4, Scene->NumOpaqueInstances,
                                                       BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                       BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
                for (u32 InstanceId = 0; InstanceId < Scene->NumOpaqueInstances; ++InstanceId)
                {
                    GpuData[InstanceId] = Scene->OpaqueInstances[InstanceId].ShadowWVP;
                }
            }
        }

        // NOTE: Push Scene Globals
        {
            scene_globals* Data = VkCommandsPushWriteStruct(Commands, Scene->SceneBuffer, scene_globals,
                                                            BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                            BarrierMask(VK_ACCESS_UNIFORM_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
            *Data = {};
            Data->CameraPos = Scene->Camera.Pos;
            Data->NumPointLights = Scene->NumPointLights;
            Data->VPTransform = CameraGetVP(&Scene->Camera);
            Data->VTransform = CameraGetV(&Scene->Camera);
        }

        // NOTE: Push grass data
        {
            grass* Grass = &DemoState->TiledDeferredState.Grass;

            grass_uniforms_gpu* Uniforms = VkCommandsPushWriteStruct(Commands, Grass->UniformsGpu, grass_uniforms_gpu,
                                                                     BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                     BarrierMask(VK_ACCESS_UNIFORM_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
            Grass->UniformsCpu.CurrTime += FrameTime;
            *Uniforms = Grass->UniformsCpu;

            indirect_args* IndirectArgs = VkCommandsPushWriteStruct(Commands, Grass->IndirectArg, indirect_args,
                                                                    BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                    BarrierMask(VK_ACCESS_UNIFORM_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
            *IndirectArgs = {};
            IndirectArgs->NumInstances = 1;
        }
        
        VkCommandsTransferFlush(Commands, RenderState->Device);
    }

    // NOTE: Render Scene
    TiledDeferredRender(Commands, &DemoState->TiledDeferredState, &DemoState->Scene);

    RenderTargetPassBegin(&DemoState->CopyToSwapTarget, Commands, RenderTargetRenderPass_SetViewPort | RenderTargetRenderPass_SetScissor);
    FullScreenPassRender(Commands, DemoState->CopyToSwapPipeline, 1, &DemoState->CopyToSwapDesc);
    RenderTargetPassEnd(Commands);
    UiStateRender(&DemoState->UiState, RenderState->Device, Commands, DemoState->SwapChainEntry.View);

    VkCommandsEnd(Commands, RenderState->Device);
                    
    // NOTE: Render to our window surface
    // NOTE: Tell queue where we render to surface to wait
    VkPipelineStageFlags WaitDstMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo SubmitInfo = {};
    SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    SubmitInfo.waitSemaphoreCount = 1;
    SubmitInfo.pWaitSemaphores = &RenderState->ImageAvailableSemaphore;
    SubmitInfo.pWaitDstStageMask = &WaitDstMask;
    SubmitInfo.commandBufferCount = 1;
    SubmitInfo.pCommandBuffers = &Commands->Buffer;
    SubmitInfo.signalSemaphoreCount = 1;
    SubmitInfo.pSignalSemaphores = &RenderState->FinishedRenderingSemaphore;
    VkCheckResult(vkQueueSubmit(RenderState->GraphicsQueue, 1, &SubmitInfo, Commands->Fence));
    
    VkPresentInfoKHR PresentInfo = {};
    PresentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    PresentInfo.waitSemaphoreCount = 1;
    PresentInfo.pWaitSemaphores = &RenderState->FinishedRenderingSemaphore;
    PresentInfo.swapchainCount = 1;
    PresentInfo.pSwapchains = &RenderState->SwapChain;
    PresentInfo.pImageIndices = &ImageIndex;
    VkResult Result = vkQueuePresentKHR(RenderState->PresentQueue, &PresentInfo);

    switch (Result)
    {
        case VK_SUCCESS:
        {
        } break;

        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_SUBOPTIMAL_KHR:
        {
            // NOTE: Window size changed
            InvalidCodePath;
        } break;

        default:
        {
            InvalidCodePath;
        } break;
    }
}
