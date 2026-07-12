#version 450 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require

struct Vertex{
    vec2 pos;
    vec2 uv;
    vec4 color;
};
layout(buffer_reference, scalar) readonly buffer Vertex_Buffer {
    Vertex vertices[];
};

layout(push_constant) uniform Push {
    Vertex_Buffer vertex_address;
    int texture_index;

    vec2 scale;
    vec2 translate;
} push;

out gl_PerVertex {
    vec4 gl_Position;
};

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 uv;
layout(location = 2) out flat int texture_index;

void main()
{
    color = push.vertex_address.vertices[gl_vertexIndex].color;
    uv = push.vertex_address.vertices[gl_vertexIndex].uv;

    gl_Position = vec4(
        push.vertex_address.vertices[gl_vertexIndex].pos * push.transform.scale + push.transform.translate, 
        0.0, 1.0
    );
}
