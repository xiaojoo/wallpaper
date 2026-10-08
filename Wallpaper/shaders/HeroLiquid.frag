#version 440
// Hero switch transition, ported to run as a Qt Quick ShaderEffect.
//
// This is a port of Codrops "Liquid Distortion" DEMO 3
// (https://tympanus.net/Development/LiquidDistortion/index3.html), read off the live page:
//
//   js/pixi.min.js  PIXI 4.5.1 DisplacementFilter fragment:
//       vec4 map = texture2D(mapSampler, vFilterCoord);
//       map -= 0.5;  map.xy *= scale;
//       gl_FragColor = texture2D(uSampler, clamp(vTextureCoord + map.xy, filterClamp...));
//   ...and DisplacementFilter.apply(), which is where the units come from:
//       scale.x = this.scale.x / destinationFrame.width      <- BOTH axes divided by the WIDTH
//   js/main3.js, with this demo's options (displaceScale [300,300], autoPlay false so
//   displaceScaleTo is [0,0], wacky true, displacementImage img/dmaps/512x512/crystalize.jpg):
//       .to(filter.scale, 1.0, {x:300, y:300, ease:Power1.easeOut})       t 0.0 -> 1.0
//       .to(currentImg,   0.5, {alpha:0, ease:Power2.easeOut}, 0.2)       t 0.2 -> 0.7
//       .to(newImg,       0.5, {alpha:1, ease:Power2.easeOut}, 0.3)       t 0.3 -> 0.8
//       .to(filter.scale, 1.0, {x:0, y:0, ease:Power2.easeOut}, 0.3)      t 0.3 -> 1.3
//       onUpdate:   sprite.rotation += progress * 0.02;  sprite.scale.set(progress * 3)
//       onComplete: sprite.scale.set(1)
//
// Two properties of that setup are what make demo 3 look like demo 3, and both are easy to miss:
// the map is *rotating and magnifying* while it churns (the Voronoi cells grow from fine grain to
// about six across), and crystalize.jpg measures as **exactly grayscale** (mean |R-G| = 0.0 over
// all 262144 pixels), so map.x == map.y and the push is always along the uv diagonal.
//
// The map is their asset, so the cell field is generated here instead: a periodic Voronoi with the
// same ~9.5 cells per 512 px tile and the same bright-interior / dark-boundary weighting.
// prog 0 reproduces fromTex and prog 1 reproduces toTex pixel for pixel, so the effect can be
// switched off at either end without moving the picture.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float prog;      // 0 -> 1 over the whole sweep
    float aspect;    // box width / height, so the cells stay round like the sprite's
    float scalePx;   // the reference's displaceScale: 300
    float stageW;    // the reference's render target width: 1920 (Pixi divides BOTH axes by it)
    float cells;     // Voronoi cells per map tile: 512 px / ~52 px
    float rotRad;    // total map rotation over one sweep
    float rotBase;   // rotation carried over from previous switches, like the reference's sprite
} ubuf;

layout(binding = 1) uniform sampler2D fromTex;
layout(binding = 2) uniform sampler2D toTex;

const float MAP_TILE = 512.0 / 1920.0;   // one map tile at sprite scale 1, in width units
const float SPRITE_MAX = 3.0;            // sprite.scale.set(progress * 3)
const float SPRITE_MIN = 0.2;            // clamped: below this the cells alias into noise

// Sin-free lattice hash. The Voronoi below evaluates nine of these per pixel, and a
// fract(sin(dot()))-style hash costs 18 transcendentals per pixel at 1360x860 - measured ~75
// rendered frames per sweep instead of the ~125 this window otherwise gets.
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 = p3 + dot(p3, p3.yzx + 33.33);
    return fract(vec2(p3.x + p3.y, p3.y + p3.z));
}

// Periodic Voronoi with a flat-topped cell profile, valued 1 over most of a cell and falling to 0
// at its boundary. Neither the density nor the profile is freehand. crystalize.jpg measures mean
// 167/255 with 23.8% of its pixels above 242 and only 2.3% below 13, and its autocorrelation does
// not fall to zero until about a 50 px lag - so the cells are ~52 px across, 9.5 per 512 px tile,
// not the ~5 a first read of the file suggests. Fitting the plateau and the rim width over that
// lattice reproduces all four numbers as rendered pixels: 162.8 / 23.5% / 3.1% and a mean gradient
// of 9.86 against the reference's 9.76. A cone-shaped cell gets none of them (108 mean, 8% black).
float crystalize(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float d1 = 8.0;
    for (int y = -1; y <= 1; y = y + 1) {
        for (int x = -1; x <= 1; x = x + 1) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = hash22(mod(i + g, vec2(ubuf.cells)));
            d1 = min(d1, length(g + o - f));
        }
    }
    float s = clamp((d1 - 0.20) / 0.60, 0.0, 1.0);
    return 1.0 - s * s * (3.0 - 2.0 * s);
}

// The grabs carry alpha (the layers do not paint the whole box), so composite over the box's own
// colour instead of mixing straight through: a morph between two opaque colours cannot expose the
// base as a blink.
vec3 overBase(vec4 s) {
    vec3 base = vec3(0.0313725, 0.0431373, 0.0627451);   // #080B10 == heroBox.color
    return s.rgb + base * (1.0 - s.a);
}

void main() {
    float p = clamp(ubuf.prog, 0.0, 1.0);
    float t = p * 1.3;                                    // the reference timeline is 1.3 s

    // filter.scale in the reference's own pixels: up with Power1.easeOut for a second, handed over
    // to the second tween at 0.3 s, which is where the first one has got to (300 * 0.51 = 153).
    float up = clamp(t, 0.0, 1.0);
    up = 1.0 - (1.0 - up) * (1.0 - up);                   // Power1.easeOut
    float dn = clamp(t - 0.3, 0.0, 1.0);
    dn = 1.0 - dn;
    dn = dn * dn * dn;                                    // 1 - Power2.easeOut
    float scaleNow = mix(ubuf.scalePx * up, ubuf.scalePx * 0.51 * dn, step(0.3, t));

    // The two alpha tweens, composited the way Pixi composites the sprites: what is still on screen
    // is the outgoing frame times the part of the new frame that has not arrived yet.
    float xo = clamp((t - 0.2) / 0.5, 0.0, 1.0);
    float aOld = 1.0 - xo;
    aOld = aOld * aOld * aOld;                            // 1 - Power2.easeOut
    float xn = clamp((t - 0.3) / 0.5, 0.0, 1.0);
    float aNew = 1.0 - pow(1.0 - xn, 3.0);                // Power2.easeOut
    float w = 1.0 - aOld * (1.0 - aNew);

    // The map sprite: anchored at the centre, magnifying from 0 to 3 and turning as it goes.
    float sprite = max(p * SPRITE_MAX, SPRITE_MIN);
    float th = ubuf.rotBase + ubuf.rotRad * p * p;
    vec2 s = (qt_TexCoord0 - 0.5) * vec2(1.0, ubuf.aspect);
    vec2 r = vec2(cos(th) * s.x + sin(th) * s.y, -sin(th) * s.x + cos(th) * s.y);
    vec2 m = fract(r / (MAP_TILE * sprite) * ubuf.cells + vec2(0.5));

    // Pixi's DisplacementFilter in its own units: both axes divided by the target *width*, and with
    // a grayscale map the two components are equal, so the push is along the uv diagonal.
    float off = (crystalize(m) - 0.5) * scaleNow / ubuf.stageW;
    vec2 uv = qt_TexCoord0 + vec2(off);

    // The reference displaces the whole stage, so the incoming picture churns too - and that push
    // outlives the crossfade: measured here, at prog 0.55 the fade is already done and still 35 % of
    // the pixels sat off their settled position (9.5 levels mean, best-fit translation 0 px, so it is
    // a shard-wise warp, not a pan), easing out only by prog 0.85. He asked for the wallpaper that
    // stays up to sit still, so the field is applied to the outgoing capture alone and the new
    // picture is always taken at its own coordinates.
    vec3 c = mix(overBase(texture(fromTex, uv)), overBase(texture(toTex, qt_TexCoord0)), w);
    fragColor = vec4(c, 1.0) * ubuf.qt_Opacity;
}
