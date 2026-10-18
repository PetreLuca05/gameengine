// Alpha test sample of a masked material: isotropic mip (the larger pixel footprint, as the vgeo
// rasterisers' per-triangle LOD), so anisotropic filtering does not smear the coverage of thin blades.
// bias: the sampler's mipLodBias, cancelled (the shadow views: PF-0029 biases the scene's samplers for the
// display's texel density, which a shadow map has nothing to do with).
float cl_mask_alpha_b(sampler2D t, vec2 uv, float bias) {
    vec2 sz = vec2(textureSize(t, 0));
    vec2 dx = dFdx(uv) * sz, dy = dFdy(uv) * sz;
    float lod = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8));
    return textureLod(t, uv, max(lod, 0.0) - bias).a;
}
float cl_mask_alpha(sampler2D t, vec2 uv) { return cl_mask_alpha_b(t, uv, 0.0); }
