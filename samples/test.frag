#version 450

layout(location = 0) out vec4 color;

void main() {
#ifdef FLAG_A
  colro = 0;
#else
  color = vec4(1.0, 1.0, 0.0, 1.0);
#endif
}