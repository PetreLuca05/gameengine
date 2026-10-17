#version 450
#extension GL_GOOGLE_include_directive : require
// Water surface + velocity (TAA / motion blur).
#define WATER_VELOCITY 1
#include "water/water_surface.glsl"
