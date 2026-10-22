// The end of the classic deferred prepass (gbuffer_body.glsl main with GB_DEFERRED; statements): the
// lit pass's surface stage (lit_surface.glsl), material and weather terms of this fragment into the
// deferred targets (classic_deferred.glsl). Its own scope: the names shadow the G-buffer body's.
    {
#include "lit_surface.glsl"
        vec3 cld_albedo = base.rgb;
        float cld_rough = 0.0, cld_snow = 0.0, cld_ao = 1.0;
        uint cld_f = CLD_DEFERRED;
        if ((flags & MAT_UNLIT) != 0u) {
            cld_f |= CLD_UNLIT;
        } else {
            float rough = clamp(surf_rough >= 0.0 ? surf_rough : MAT_PARAMS.x, 0.04, 1.0);
            float metal = clamp(surf_metal >= 0.0 ? surf_metal : MAT_PARAMS.y, 0.0, 1.0);
#ifdef PBR_MAPS
            pbr_metal_rough(flags, map_uv, rough, metal);
            rough = max(rough, 0.04);
#endif
            cld_snow = weather_surface(v_world, n, cld_albedo, rough, metal, false);
            cld_rough = rough;
            cld_ao = vertex_ao ? v_color.a : 1.0;
#ifdef PBR_MAPS
            cld_ao *= pbr_occlusion(flags, map_uv);
#endif
            cld_ao *= surf_ao;
            if (g_cv.on) cld_ao *= g_cv.ao;
        }
        if ((flags & MAT_NO_RECEIVE) != 0u) cld_f |= CLD_NO_RECEIVE;
        if (g_cv.on) cld_f |= CLD_COVER | (MAT_COVER_ID(flags) << CLD_COVER_ID_SHIFT);
        o_cl_albedo = vec4(cld_albedo, cld_rough);
        o_cl_params = uvec4(packHalf2x16(vec2(cld_ao, 0.0)), packHalf2x16(vec2(cld_snow, 0.0)), packHalf2x16(vec2(g_cv.on ? g_cv.thick : 0.0, 0.0)),
                            cld_f | ((ao_b & 0xFFu) << CLD_AO_SHIFT));
    }
