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
    // A hard threshold would strobe between two frames; a 0.12 ramp over the threshold keeps the
    // leading edge soft, which is what makes it read as developing rather than as noise switching.
    float a = clamp((u.prog - n) / 0.12 + 0.5, 0.0, 1.0);
    vec2 r = (1.0 - a) * vec2(u.blurX, u.blurY);
    vec4 c = texture(sharpTex, uv);
    c += texture(sharpTex, uv + r * vec2(-0.326, -0.406));
    c += texture(sharpTex, uv + r * vec2(-0.840, -0.074));
    c += texture(sharpTex, uv + r * vec2(-0.696,  0.457));
    c += texture(sharpTex, uv + r * vec2(-0.203,  0.621));
    c += texture(sharpTex, uv + r * vec2( 0.962, -0.195));
    c += texture(sharpTex, uv + r * vec2( 0.473, -0.480));
    c += texture(sharpTex, uv + r * vec2( 0.519,  0.767));
    c += texture(sharpTex, uv + r * vec2( 0.185, -0.893));
    c += texture(sharpTex, uv + r * vec2( 0.507,  0.064));
    fragColor = u.qt_Opacity * (c / 10.0);
}
