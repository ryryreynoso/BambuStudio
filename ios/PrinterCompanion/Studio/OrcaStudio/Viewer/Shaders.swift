// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 OrcaPad contributors

// MARK: - Metal Shading Language sources
//
// NOTE: The Linux/xtool toolchain cannot compile .metallib files (Apple's
// offline Metal compiler is macOS-only). Shaders are therefore compiled at
// runtime from these source strings via MTLDevice.makeLibrary(source:).
// Cost: a few ms of app-launch time. Keep all MSL in this file.

enum Shaders {
    static let msl = """
    #include <metal_stdlib>
    using namespace metal;

    // --- Lit (triangles) ---
    struct CubeUniforms {
        float4x4 mvp;
        float4x4 model;
        float4 color;
        float3 lightDir;
        float normalSign; // -1 for mirrored (left-handed) instances
        float3 eye;
    };

    struct VIn {
        float3 position [[attribute(0)]];
        float3 normal   [[attribute(1)]];
    };

    struct VOutLit {
        float4 position [[position]];
        float3 worldNormal;
        float3 eyeDir;
    };

    vertex VOutLit vertexMain(VIn in [[stage_in]],
                              constant CubeUniforms &u [[buffer(1)]]) {
        VOutLit out;
        out.position = u.mvp * float4(in.position, 1.0);
        out.worldNormal = (u.model * float4(in.normal, 0.0)).xyz * u.normalSign;
        out.eyeDir = u.eye - (u.model * float4(in.position, 1.0)).xyz;
        return out;
    }

    fragment float4 fragmentLit(VOutLit in [[stage_in]],
                                constant CubeUniforms &u [[buffer(1)]]) {
        float3 n = normalize(in.worldNormal);
        float3 key = -normalize(u.lightDir);
        float3 fill = normalize(float3(-0.6, 0.25, -0.5));
        float3 view = normalize(in.eyeDir);
        // Sky-and-floor ambient, a warm key light and a cool fill.
        float3 ambient = mix(float3(0.16, 0.16, 0.19), float3(0.46, 0.48, 0.54), n.y * 0.5 + 0.5);
        float3 diffuse = float3(1.0, 0.97, 0.92) * 0.78 * saturate(dot(n, key))
                       + float3(0.55, 0.66, 0.85) * 0.20 * saturate(dot(n, fill));
        // Satin plastic highlight and a soft edge light.
        float3 halfway = normalize(key + view);
        float specular = pow(saturate(dot(n, halfway)), 42.0) * 0.28;
        float rim = pow(1.0 - saturate(dot(n, view)), 3.0) * 0.14;
        float3 lit = u.color.rgb * (ambient + diffuse) + specular + rim;
        return float4(lit, u.color.a);
    }

    // --- Backdrop (full-screen vertical gradient, drawn first) ---
    struct VOutBackdrop {
        float4 position [[position]];
        float t;
    };

    vertex VOutBackdrop vertexBackdrop(uint id [[vertex_id]]) {
        float2 corners[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };
        VOutBackdrop out;
        out.position = float4(corners[id], 1.0, 1.0);
        out.t = corners[id].y * 0.5 + 0.5;
        return out;
    }

    fragment float4 fragmentBackdrop(VOutBackdrop in [[stage_in]]) {
        float3 bottom = float3(0.040, 0.043, 0.055);
        float3 top = float3(0.115, 0.125, 0.155);
        return float4(mix(bottom, top, saturate(in.t)), 1.0);
    }

    // --- Flat (lines) ---
    struct LineUniforms {
        float4x4 mvp;
    };

    struct VInLine {
        float3 position [[attribute(0)]];
        float4 color    [[attribute(1)]];
    };

    struct VOutFlat {
        float4 position [[position]];
        float4 color;
    };

    vertex VOutFlat vertexFlat(VInLine in [[stage_in]],
                               constant LineUniforms &u [[buffer(1)]]) {
        VOutFlat out;
        out.position = u.mvp * float4(in.position, 1.0);
        out.color = in.color;
        return out;
    }

    fragment float4 fragmentFlat(VOutFlat in [[stage_in]]) {
        return in.color;
    }

    // --- Selection outline (inverted hull — desktop's selection halo) ---
    // The selected mesh is drawn once more, expanded along its normals with
    // FRONT faces culled, before the opaque pass: the shell survives depth
    // testing only around the object's silhouette, producing a clean
    // outline without a post-process pass.
    struct OutlineUniforms {
        float4x4 mvp;
        float4x4 model;
        float4 color;
        float width; // object-space expansion (mm)
    };

    vertex VOutFlat vertexOutline(VIn in [[stage_in]],
                                  constant OutlineUniforms &u [[buffer(1)]]) {
        VOutFlat out;
        float3 expanded = in.position + normalize(in.normal) * u.width;
        out.position = u.mvp * float4(expanded, 1.0);
        out.color = u.color;
        return out;
    }

    // --- Shadow (mesh flattened onto the bed along the key light) ---
    // Depth writes keep overlapping triangles from darkening twice.
    vertex VOutFlat vertexShadow(VIn in [[stage_in]],
                                 constant OutlineUniforms &u [[buffer(1)]]) {
        VOutFlat out;
        float3 world = (u.model * float4(in.position, 1.0)).xyz;
        float3 light = float3(0.35, -0.85, 0.40);
        world -= light * (world.y / light.y);
        world.y = -0.02;
        out.position = u.mvp * float4(world, 1.0);
        out.color = u.color;
        return out;
    }
    """
}
