#version 450 core

layout(set = 0, binding = 1) uniform sampler2D bindless[];


layout(location = 0) in vec4 color;
layout(location = 1) in vec2 uv;
layout(location = 2) in flat int texture_index;

layout(location = 0) out vec4 fColor;

void main() {   

    //its only nonuniformext if it's drawindirectcount?
    fColor = color * texture(bindless[texture_index], uv);
}
