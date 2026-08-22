#version 450

layout(set = 0, binding = 0) uniform sampler2D surfaceImage;
layout(push_constant) uniform ScenePush {
    vec4 destination;
    vec4 source;
    uint opaque;
} pushData;

layout(location = 0) in vec2 textureCoordinate;
layout(location = 0) out vec4 outputColor;

void main()
{
    outputColor = texture(surfaceImage, textureCoordinate);
    if (pushData.opaque != 0)
        outputColor.a = 1.0;
}
