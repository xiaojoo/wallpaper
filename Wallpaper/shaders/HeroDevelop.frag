#version 440
// Hero sharpness reveal: the downloaded original (sharpTex) is drawn blurred and sharpened cell by
// cell through a per-cell noise threshold, so the detail arrives as a scattered "develop" instead of
// one whole-frame step.
//
// Why the soft half is this texture blurred and not the site's preview layer: measured 2026-10-14
// against a settled frame of the same reveal, the preview layer and the original layer sit at
// sx=0.9984 / sy=1.0188 - 2.04 % of anisotropy plus 0.86 % zoom, constant from the first frame of
// the sweep to the last (preview-to-preview frames read 1.0000/1.0000 at 0.00 px residual). The
// anisotropy is preview-aspect / picture-aspect under 拉伸, because the preview is a differently
// framed crop. Compositing between the two therefore MOVES the picture by ~16 px across the box
// while it reveals it, which is the complaint the reveal was supposed to fix. Sampling one texture
// makes the two halves the same geometry by construction: a symmetric tap disc blurs features
// without displacing them, so every frame of the sweep is registered to the settled frame.
//
// a = 1 collapses every tap onto the pixel itself, so prog 1 reproduces sharpTex exactly and the
// effect can be dropped at either end without changing the picture - the contract HeroLiquid keeps.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float prog;      // 0 -> 1 over the sweep. Keep the QML property order identical.
    float gridX;     // cells across the picture, so the grain is the same at any window size
    float gridY;     // (grid and blur are pairs of floats, not vec2s: textureSize() is unavailable
    float blurX;     //  in the ESSL 100 stage the baker also emits, and a vec2 here would need an
    float blurY;     //  8-byte pad after `prog`.) Blur radius in uv, so the disc is round in px.
} u;

layout(binding = 1) uniform sampler2D sharpTex;

float hash12(vec2 p) {
    // Only needs to not look periodic at the sizes it is sampled at.
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

void main() {
    vec2 uv = qt_TexCoord0;
    float n = hash12(floor(vec2(uv.x * u.gridX, uv.y * u.gridY)));
    // The ramp is R wide per cell and the whole field has to be finished at prog = 1, because that
    // is the frame the effect is dropped. The obvious `prog - n` form leaves every cell with
    // n > 1 - R/2 still soft there (n = 0.99 sits at a = 0.58, half blurred), and those ~12 % of the
    // cells then snap sharp in the single frame the ShaderEffect hides - the 快结束的时候抖一下 he
    // reported on 2026-10-14. Scaling prog by (1 + R) puts the last cell's completion exactly at
    // prog = 1 and the first cell's start exactly at prog = 0, so both ends of the sweep reproduce
    // the plain picture pixel for pixel.
    const float R = 0.12;
    float a = clamp((u.prog * (1.0 + R) - n) / R, 0.0, 1.0);
    vec2 r = (1.0 - a) * vec2(u.blurX, u.blurY);
    // Two full symmetric rings plus the centre, all at weight 1/17. The first version used a
    // Poisson-style 9-tap disc whose offsets summed to (+0.581, -0.139): an asymmetric kernel
    // translates the picture by 0.058 * radius, so the sweep drifted sideways by a fraction of a
    // pixel while it sharpened. Symmetric rings cannot translate anything, at any radius.
    vec4 c = texture(sharpTex, uv);
    c += texture(sharpTex, uv + r * vec2( 1.000,  0.000));
    c += texture(sharpTex, uv + r * vec2(-1.000,  0.000));
    c += texture(sharpTex, uv + r * vec2( 0.000,  1.000));
    c += texture(sharpTex, uv + r * vec2( 0.000, -1.000));
    c += texture(sharpTex, uv + r * vec2( 0.707,  0.707));
    c += texture(sharpTex, uv + r * vec2(-0.707,  0.707));
    c += texture(sharpTex, uv + r * vec2( 0.707, -0.707));
    c += texture(sharpTex, uv + r * vec2(-0.707, -0.707));
    vec2 r2 = r * 0.45;
    c += texture(sharpTex, uv + r2 * vec2( 1.000,  0.000));
    c += texture(sharpTex, uv + r2 * vec2(-1.000,  0.000));
    c += texture(sharpTex, uv + r2 * vec2( 0.000,  1.000));
    c += texture(sharpTex, uv + r2 * vec2( 0.000, -1.000));
    c += texture(sharpTex, uv + r2 * vec2( 0.707,  0.707));
    c += texture(sharpTex, uv + r2 * vec2(-0.707,  0.707));
    c += texture(sharpTex, uv + r2 * vec2( 0.707, -0.707));
    c += texture(sharpTex, uv + r2 * vec2(-0.707, -0.707));
    fragColor = u.qt_Opacity * (c / 17.0);
}
