#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "descriptor_layouts.cpp"

struct draw_triangle
{
    vec3 Vertices[3]; // NOTE: WS Position
    vec3 Normal;
};

struct indirect_args
{
    uint NumVerticesPerInstance;
    uint NumInstances;
    uint StartVertexIndex;
    uint StartInstanceIndex;
};

layout(set = 0, binding = 0) buffer grass_triangles
{
    draw_triangle GrassTriangles[];
};

layout(set = 0, binding = 1) buffer indirect_arg_buffer
{
    indirect_args IndirectArgs;
};

#define MAX_BLADE_SEGMENTS 5
#define MAX_BLADE_POINTS MAX_BLADE_SEGMENTS * 2 + 1

layout(set = 0, binding = 2) uniform gen_input_buffer
{
    vec2 WorldMin;
    vec2 WorldMax;
    uvec2 NumBlades;
    uint MaterialId;
    uint MaxBladeSegments;
    float MaxBendAngle; // NOTE: Multiplier of Pi/2
    float BladeCurvature;
    float BladeHeight;
    float BladeHeightVariance;
    float BladeWidth;
    float BladeWidthVariance;
} GrassInputs;

GBUFFER_DESCRIPTOR_LAYOUT(1)
SCENE_DESCRIPTOR_LAYOUT(2)

//
// ======================================================================================================================================
//

#if GEN_GRASS

// NOTE: Rand Helpers

float RandomFloat(vec4 Value)
{
    vec4 SmallValue = sin(Value);
    float Result = dot(SmallValue, vec4(12.9898, 78.233, 37.719, 09.151));
    Result = fract(sin(Result) * 143758.5453);
    return Result;
}

float RandomFloat(vec3 Pos, float Offset)
{
    float Result = RandomFloat(vec4(Pos, Offset));
    return Result;
}

float RandomFloatNeg1To1(vec3 Pos, float Offset)
{
    float Result = RandomFloat(Pos, Offset) * 2 - 1;
    return Result;
}

// NOTE: Math helpers

#define Pi32 3.14159
mat3 AngleAxis3x3(float Angle, vec3 Axis)
{
    float c = cos(Angle);
    float s = sin(Angle);

    float t = 1 - c;
    float x = Axis.x;
    float y = Axis.y;
    float z = Axis.z;

    mat3 Result = mat3(t * x * x + c, t * x * y - s * z, t * x * z + s * y,
                       t * x * y + s * z, t * y * y + c, t * y * z - s * x,
                       t * x * z - s * y, t * y * z + s * x, t * z * z + c);

    return Result;
}

vec2 GetBladeDimensions(vec3 PositionWs)
{
    float Width = RandomFloatNeg1To1(PositionWs, 0) * GrassInputs.BladeWidthVariance + GrassInputs.BladeWidth;
    float Height = RandomFloatNeg1To1(PositionWs, 1) * GrassInputs.BladeHeightVariance + GrassInputs.BladeHeight;
    vec2 Result = vec2(Width, Height);
    return Result;
}

vec3 SetupBladePoint(vec3 BladeCenterWs, vec2 Dimensions, mat3 TBN, vec2 Uv)
{
    vec3 Result;

    // TODO: We can probably integrate dimensions into the mat
    vec3 OffsetTs = vec3((Uv.x - 0.5) * Dimensions.x, 0, Uv.y * Dimensions.y);
    vec3 OffsetWs = TBN * OffsetTs;
    Result = BladeCenterWs + OffsetWs;

    return Result;
}

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main()
{
    // NOTE: https://www.youtube.com/watch?v=DeATXF4Szqo&ab_channel=NedMakesGames
    if (gl_GlobalInvocationID.x > GrassInputs.NumBlades.x ||
        gl_GlobalInvocationID.y > GrassInputs.NumBlades.y)
    {
        return;
    }

    vec2 BladeCenter2D = gl_GlobalInvocationID.xy / vec2(GrassInputs.NumBlades);
    BladeCenter2D = mix(GrassInputs.WorldMin, GrassInputs.WorldMax, BladeCenter2D);
    vec3 BladeCenter = vec3(BladeCenter2D.x, -2.0f, BladeCenter2D.y); // TODO: Hacky

    vec2 Dimensions;
    mat3 BaseTransform;
    {
        // NOTE: Apply random scaling
        Dimensions = GetBladeDimensions(BladeCenter);
    
        // NOTE: Apply random rotation/scaling
        {
            mat3 TwistMatrix = AngleAxis3x3(RandomFloat(BladeCenter, 2) * Pi32 * 2, vec3(0, 0, 1));
            BaseTransform = TwistMatrix;
        }
    
        mat3 TBN = mat3(vec3(1, 0, 0), vec3(0, 0, 1), vec3(0, 1, 0));
        BaseTransform = TBN * BaseTransform;
    }

    uint NumBladeSegments = min(MAX_BLADE_SEGMENTS, max(1, GrassInputs.MaxBladeSegments));

    vec3 Vertices[MAX_BLADE_POINTS];
    float MaxBend = RandomFloat(BladeCenter, 3) * Pi32 * 0.5f * GrassInputs.MaxBendAngle;

    // NOTE: Generate blade segments
    for (int i = 0; i < NumBladeSegments; ++i)
    {
        float v = i / float(NumBladeSegments);
        float u = 0.5 - (1 - v) * 0.5;

        mat3 BendMatrix = AngleAxis3x3(MaxBend * pow(v, GrassInputs.BladeCurvature), vec3(1, 0, 0));
        mat3 Transform = BaseTransform * BendMatrix;
        
        Vertices[i*2 + 0] = SetupBladePoint(BladeCenter, Dimensions, Transform, vec2(u, v));
        Vertices[i*2 + 1] = SetupBladePoint(BladeCenter, Dimensions, Transform, vec2(1 - u, v));
    }

    // NOTE: Generate blade tip
    {
        mat3 BendMatrix = AngleAxis3x3(MaxBend * pow(1, GrassInputs.BladeCurvature), vec3(1, 0, 0));
        mat3 Transform = BaseTransform * BendMatrix;
        Vertices[NumBladeSegments * 2] = SetupBladePoint(BladeCenter, Dimensions, Transform, vec2(0.5, 1));
    }

    // NOTE: Append triangles
    uint NumTriangles = (NumBladeSegments - 1) * 2 + 1;
    uint SavedVertexId = atomicAdd(IndirectArgs.NumVerticesPerInstance, NumTriangles*3);
    uint StartTriangleId = SavedVertexId / 3;

    for (int i = 0; i < NumTriangles; ++i)
    {
        draw_triangle Triangle;
        Triangle.Vertices[0] = Vertices[i+0];
        Triangle.Vertices[1] = Vertices[i+1];
        Triangle.Vertices[2] = Vertices[i+2];
        Triangle.Normal = cross(Triangle.Vertices[0], Triangle.Vertices[2]);
        GrassTriangles[StartTriangleId + i] = Triangle;
    }
}

#endif

//
// ======================================================================================================================================
//

#if GBUFFER_VERT

layout(location = 0) out vec3 OutViewPos;
layout(location = 1) out vec3 OutViewNormal;
layout(location = 2) out flat uint OutMaterialId;

void main()
{
    draw_triangle GrassTriangle = GrassTriangles[gl_VertexIndex / 3];
    vec3 Vertex = GrassTriangle.Vertices[gl_VertexIndex % 3];
    
    gl_Position = SceneBuffer.VPTransform * vec4(Vertex, 1);
    OutViewPos = (SceneBuffer.VTransform * vec4(Vertex, 1)).xyz;
    OutViewNormal = (SceneBuffer.VTransform * vec4(GrassTriangle.Normal, 0)).xyz;
    OutMaterialId = GrassInputs.MaterialId;
    // TODO: NASTY HACK
    OutMaterialId = 0xFFFFFFFF - 1;
}

#endif

#if GBUFFER_FRAG

layout(location = 0) in vec3 InViewPos;
layout(location = 1) in vec3 InViewNormal;
layout(location = 2) in flat uint InMaterialId;

layout(location = 0) out vec4 OutViewPos;
layout(location = 1) out vec4 OutViewNormal;
layout(location = 2) out vec4 OutMaterial;

void main()
{
    OutViewPos = vec4(InViewPos, 0);
    OutViewNormal = vec4(normalize(InViewNormal), 0);
    OutMaterial.rgb = vec3(0, 1, 0);
    OutMaterial.w = uintBitsToFloat(InMaterialId);
}

#endif
