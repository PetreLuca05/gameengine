// Alpha test sample of a masked material: isotropic mip (the larger pixel footprint, as the vgeo
// rasterisers' per-triangle LOD), so anisotropic filtering does not smear the coverage of thin blades.
float cl_mask_alpha(sampler2D t, vec2 uv) {
    vec2 sz = vec2(textureSize(t, 0));
    vec2 dx = dFdx(uv) * sz, dy = dFdy(uv) * sz;
    float lod = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8));
    return textureLod(t, uv, max(lod, 0.0)).a;
}
