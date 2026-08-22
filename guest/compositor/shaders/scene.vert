#version 450

layout(push_constant) uniform ScenePush {
    vec4 destination;
    vec4 source;
    uint opaque;
} pushData;

layout(location = 0) out vec2 textureCoordinate;

void main()
{
    const vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
        vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    vec2 corner = corners[gl_VertexIndex];
    gl_Position = vec4(mix(pushData.destination.xy,
                           pushData.destination.zw, corner), 0.0, 1.0);
    textureCoordinate = mix(pushData.source.xy, pushData.source.zw, corner);
}
