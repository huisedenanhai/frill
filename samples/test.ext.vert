#version 450

#include <inc.glsl>

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;

layout(location = 0) out vec3 color_vs;

void main() {
  gl_Position = vec4(position, 1.0);
  color_vs = color;
}