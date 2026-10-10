#include "Fullscreen.hlsl"

// Matrix Clock: columns of falling 0/1 with the real wall clock in the middle.
//
// Measured off matrix1337.mp4 (1920x1080): the whole picture sits on a 13px character lattice - 147
// columns across - and "15:16" is burnt into the centre in a 3x5 block font whose blocks are exactly
// 5 lattice cells (65px) square, one cell-column of advance gap between digits. That burnt-in time
// is the reason this is a shader and not a video file: a picture of a clock can never become the
// current hour, so the digits come from uClock instead.
//
// Column width is kept at the reference's 13px (0.6 * the cell height, which is what keeps the 3x5
// blocks square). The rows are 48 to the reference's 83, and that is the one place this deliberately
// does not match: a 3x5 '0' needs about 4px per block before it stops being a smudge, and at 83 rows
// a block is 2.6px.

cbuffer Params : register(b1) {
    float rows;          // glyph rows on screen; column width follows at 0.6 of the cell height
    float fall_speed;    // lattice cells per second the bright head travels down
    float trail;         // streak length in cells
    float trail_shape;   // how the trail dims with distance from the head; 1 is even, 3 is a stub
    float density;       // fraction of columns raining at once
    float mutate;        // times per second a cell changes which bit it shows
    float head_gain;     // extra brightness on the leading glyph
    float rain_gain;     // level of the rain on its own, since `brightness` sets the clock's too
    float bloom;         // how far each glyph's light spills into the dark around it
    float haze;          // the dark is not empty: a slow low fog the streaks read against
    float chroma;        // hue cycles across the screen width; the reference measures 1.0
    float clock_size;    // clock digit height as a fraction of screen height, before the fit clamp
    float clock_y;       // clock centre, 0 = top of the monitor
    float clock_ink;     // clock brightness against the rain
    float clock_fade;    // seconds a digit spends dissolving into the one that replaced it
    float tick_flash;    // how much the seconds digits overshoot for a moment after they change
    float brightness;
    float vignette;
};

// 3 columns x 5 rows, bit row*3+col, row 0 at the top and col 0 at the left. Index 10 is the colon.
// Every one of these was read back out of the reference's own pixels and re-decoded to check it,
// including the two details a hand-written font usually gets wrong: '1' is a bare centre bar with no
// flag, and ':' sits on rows 1 and 3 rather than 1 and 4.
static const uint FONT[11] = {
    31599u, 9362u, 29671u, 31207u, 18925u,
    31183u, 31695u, 18727u, 31727u, 31215u, 1040u
};

float Glow(float d, float r) { return exp(-d * d / max(r * r, 1e-5)); }

float3 Hsv2Rgb(float h, float s, float v) {
    float3 p = abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return v * lerp(float3(1.0, 1.0, 1.0), saturate(p - 1.0), s);
}

// Ink coverage of one 3x5 glyph. q runs 0..1 across the glyph box, `feather` is how much of a block
// the soft edge eats, as a fraction of the block.
float GlyphInk(uint bits, float2 q, float feather) {
    float2 g = q * float2(3.0, 5.0);
    float2 b = floor(g);
    float2 c = frac(g) - 0.5;
    uint idx = uint(clamp(b.y, 0.0, 4.9)) * 3u + uint(clamp(b.x, 0.0, 2.9));
    float on = float((bits >> idx) & 1u);
    // A feather fixed in pixels, not in proportion of the block, because the two users of this
    // function differ by 12x in block size. At clock scale a proportional feather was a 13px bevel
    // and the strokes fell apart into separate cushions.
    float box = 1.0 - smoothstep(0.5 - max(feather, 0.004), 0.5, max(abs(c.x), abs(c.y)));
    float inside = step(-0.0001, min(q.x, q.y)) * (1.0 - step(1.0001, max(q.x, q.y)));
    return on * box * inside;
}

// How brightly this cell is lit right now - the streak's own envelope, without its glyph. Sampled at
// three adjacent columns so a column's light reaches the dark between it and its neighbours.
float Emit(float2 c, float t, float span) {
    float2 ch = Hash22(float2(c.x, 11.0));
    float head = fmod(t * fall_speed * (0.55 + ch.y * 0.9) + ch.x * span, span) - trail;
    float d = head - c.y;
    // The head used to arrive at full brightness the instant it crossed into a new cell, which is a
    // click every 1/7th of a second per column. A third of a cell of ramp is the cheapest thing that
    // stops the column reading as a row of switches.
    float band = step(1.0 - density, ch.x) * smoothstep(-0.35, 0.05, d) * step(d, trail);
    return band * (pow(saturate(1.0 - d / max(trail, 0.001)), max(0.5, trail_shape))
                 + head_gain * saturate(1.0 - d / 1.3));
}

// slot 0..7 -> HH:MM:SS, with 2 and 5 standing on the colons. `back` is whole seconds to go back, so
// the shader can ask what the display showed before the last tick and dissolve one into the other.
int ClockCode(int slot, float back) {
    float sod = fmod(uClock.x * 3600.0 + uClock.y * 60.0 + uClock.z - back + 86400.0, 86400.0);
    int h = int(sod / 3600.0), m = int(sod / 60.0) % 60, s = int(sod) % 60;
    if (slot == 2 || slot == 5) return 10;
    int v = slot < 2 ? h : (slot < 5 ? m : s);
    int idx = slot < 2 ? slot : (slot < 5 ? slot - 3 : slot - 6);
    return idx == 0 ? v / 10 : v % 10;
}

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float aspect = uRes.x / uRes.y;
    float2 auv = float2(uv.x * aspect, uv.y);   // one unit = one screen height, y grows downward
    float t = uTime.x;

    float cellH = 1.0 / max(4.0, rows);
    float cellW = cellH * 0.6;
    float2 p = auv / float2(cellW, cellH);
    float2 cell = floor(p);
    float2 q = (p - cell - 0.5) * (1.0 / 0.92) + 0.5;
    float2 cc = p - cell - 0.5;                 // -0.5..0.5 within the cell, in cells
    float px = 1.2 / uRes.y;                    // one and a bit device pixels, in auv

    float span = rows + trail;
    float amp = Emit(cell, t, span);

    // A glyph now holds its shape for most of its interval and then dissolves into the next one.
    // Driven by floor() it re-drew every character 3 times a second as a hard swap, which is most of
    // what felt mechanical about the first version.
    float mphase = t * mutate + Hash21(cell * float2(1.31, 0.97)) * 7.0;
    float mstep = floor(mphase);
    float ease = smoothstep(0.55, 1.0, mphase - mstep);
    float f = px * 5.0 / cellH;
    float2 h0 = Hash22(float2(cell.x * 1.71 + (mstep - 1.0) * 3.3, cell.y * 0.97 + (mstep - 1.0) * 1.13));
    float2 h1 = Hash22(float2(cell.x * 1.71 + mstep * 3.3, cell.y * 0.97 + mstep * 1.13));
    float g0 = GlyphInk(FONT[h0.x > 0.5 ? 1 : 0], q, f) * step(0.06, h0.y);
    float g1 = GlyphInk(FONT[h1.x > 0.5 ? 1 : 0], q, f) * step(0.06, h1.y);
    float glyph = lerp(g0, g1, ease);

    // The bloom is deliberately a scalar per column rather than a copy of the glyph: light that has
    // spilled has lost its shape, and taking it from Emit keeps it continuous across the cell border.
    float bleed = Emit(cell + float2(-1.0, 0.0), t, span) * Glow(cc.x + 1.0, 0.52)
                + amp * Glow(cc.x, 0.52)
                + Emit(cell + float2(1.0, 0.0), t, span) * Glow(cc.x - 1.0, 0.52);
    float rain = rain_gain * (glyph * amp + bloom * bleed);

    float bh = min(clock_size / 5.0, 0.90 * aspect / 31.0);
    // The fit clamp shortens the digit, so the vertical centring has to come from the block size that
    // survived it; off clock_size the clock sits too high whenever the clamp binds.
    float digitH = 5.0 * bh;
    float2 cp = (auv - float2((aspect - 31.0 * bh) * 0.5, clock_y - digitH * 0.5)) / bh;
    int slot = clamp(int(floor(cp.x / 4.0)), 0, 7);
    float2 cq = float2((cp.x - slot * 4.0) / 3.0, cp.y / 5.0);
    // Texture inside the digits at five subdivisions per block - the reference's own ratio, which is
    // 13px of grain in a 65px block. Smooth noise, not a per-cell hash: a hash keyed to a quantised
    // lattice jumps once per mutation, and at this block size that is a visible twitch.
    float grain = 0.84 + 0.16 * Noise2(cp * 1.35 + float2(3.0, -t * 0.30));
    float swap = 1.0 - smoothstep(0.0, max(0.05, clock_fade), uClock.w);
    float cf = px / bh;
    // The glow has to be a dilated copy of the digit, not the same footprint with a soft edge: soft
    // within a block puts a groove on every inside border and the digits arrived looking like a row
    // of cushions. Shrinking q before sampling grows each block outward instead, so the halo appears
    // around the outside of a stroke and the joins between touching blocks fill level.
    float2 dq = (cq - 0.5) * 0.80 + 0.5;
    float core = lerp(GlyphInk(FONT[ClockCode(slot, 0.0)], cq, cf),
                      GlyphInk(FONT[ClockCode(slot, 1.0)], cq, cf), swap);
    float soft = lerp(GlyphInk(FONT[ClockCode(slot, 0.0)], dq, 0.55),
                      GlyphInk(FONT[ClockCode(slot, 1.0)], dq, 0.55), swap);
    float flash = slot >= 6 ? tick_flash * (1.0 - smoothstep(0.0, 0.35, uClock.w)) : 0.0;
    float clock = (core + bloom * 0.8 * soft) * grain * clock_ink * (1.0 + flash);

    // The sweep breathes: a slow ±0.035 rotation of the whole ramp plus a gentle vertical warp. The
    // source's colour is not a static gradient either, and a fixed one was the other half of why this
    // read as a diagram rather than as light.
    float breathe = 0.035 * sin(t * 0.07);
    float warp = 0.030 * sin(uv.y * 5.3 + t * 0.21) + 0.020 * sin(uv.x * 3.1 - t * 0.13);
    float hue = frac(chroma * (uv.x - 0.040 + breathe + warp));
    // Saturation is not constant across the reference's sweep: it measures 0.40 through the yellow,
    // green and cyan half and 1.0 through the blue and violet half.
    float3 tint = Hsv2Rgb(hue, lerp(0.42, 1.0, smoothstep(0.52, 0.72, hue)), 1.0);

    float fog = Noise2(float2(auv.x * 1.5, auv.y * 1.5 - t * 0.022));
    float3 c = float3(0.010, 0.010, 0.015);      // the source's own background: 2.6 / 2.5 / 3.8 of 255
    c += float3(0.004, 0.011, 0.017) * (0.35 + 0.65 * saturate(fog)) * haze;
    c += tint * (rain + clock);
    c *= 1.0 - vignette * length((uv - float2(0.5, 0.5)) * float2(1.04, 0.96));
    // Composed in display-referred units, unlike the other packages. The rainbow was measured off the
    // source's 8-bit pixels, so its saturation is an sRGB-space fact - green at 0.42 with its dark
    // channel at 0.55 of its bright one. Run those same numbers through a linear working space plus
    // ToSrgb and that floor lifts to 0.79, which is the green half arriving as cream.
    return float4(saturate(c * brightness), 1.0);
}
