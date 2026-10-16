// Shared by tonemap.comp and the Engine Fidelity Off output (post_direct.glsl).
// Daltonization (correction filter) on display-encoded colour. mode 1 protan, 2 deutan, 3 tritan.
vec3 colorblind_correct(vec3 c, int mode) {
    float L = 17.8824 * c.r + 43.5161 * c.g + 4.11935 * c.b;
    float M = 3.45565 * c.r + 27.1554 * c.g + 3.86714 * c.b;
    float S = 0.0299566 * c.r + 0.184309 * c.g + 1.46709 * c.b;
    if (mode == 1) L = 2.02344 * M - 2.52581 * S;
    else if (mode == 2) M = 0.494207 * L + 1.24827 * S;
    else S = -0.395913 * L + 0.801109 * M;
    vec3 sim = vec3(0.0809444479 * L - 0.130504409 * M + 0.116721066 * S, -0.0102485335 * L + 0.0540193266 * M - 0.113614708 * S,
                    -0.000365296938 * L - 0.00412161469 * M + 0.693511405 * S);
    vec3 err = c - sim;
    return clamp(c + vec3(0.0, 0.7 * err.r + err.g, 0.7 * err.r + err.b), 0.0, 1.0);
}
