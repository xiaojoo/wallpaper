// Shaders/Glyphs.hlsl - text for wallpapers that have to write words.
//
// The glyphs live in one atlas the engine's own overlay loads (Shaders/glyphs.png, built by
// tools/make-atlas.py) and a package that draws text binds the same file. An earlier version carried
// a 5x6 bitmap table compiled into the shader, which read as dot matrix and was retired once the
// atlas path worked - the wallpaper is a heads-up display, not a terminal.
//
// t1 is fixed, so a package that includes this file has to leave t1 free for it: the way to do that
// is to list the atlas itself as the second texture, pointing at the shared file rather than
// shipping a copy that can drift -
//
//     {"name": "glyphs", "file": "../../../Shaders/glyphs.png"}
//
// The engine's overlay pass needs no manifest entry at all; it binds the atlas there itself.
//
// The atlas is 16 columns x 6 rows of 32x48 px cells, index = ASCII - 32, so a character box is
// 32/48 = 0.6667 wide against its height. A caller's cell size has to keep that ratio or the letters
// come out stretched.
//
// kInkTop and kInkLeft are where the drawn part starts inside its box, measured off the atlas rather
// than taken from the font's own metrics: the cap band of this face runs 0.229..0.771 of the cell
// height, so a caller that wants its text's ink at a given y has to place the box 0.229 cells above
// it. Monospaced on purpose - a table of live numbers has to keep its columns.

#ifndef SW_GLYPHS_HLSL
#define SW_GLYPHS_HLSL

#include "Contract.hlsl"

// t1, because the package that draws text uses t0 for its own art. The engine binds the same
// linear-wrap sampler to every texture slot it has, so s0 covers this one too.
Texture2D GlyphAtlas : register(t1);
// Declared here and nowhere else: fxc rejects two samplers bound to the same slot
// ("overlapping register semantics s0"), and the engine binds one linear-wrap sampler per
// texture slot, so every texture in a package shares this handle. Same name as Image.hlsl.
SamplerState LinearSampler : register(s0);

#define kAtlasCols 16.0
#define kAtlasRows 6.0
#define kCellAspect (32.0 / 48.0)
#define kInkTop  0.229
#define kInkLeft 0.190
#define kInkRight 0.790
#define kDeg 176



// HLSL has no character literals, so the codes a label is built from are spelled as these
// names. Generated together with the glyph table by tools/make-glyphs.py, so a code can never
// name a glyph that is not drawn, or the other way round.
#define cA  65u
#define cB  66u
#define cC  67u
#define cD  68u
#define cE  69u
#define cF  70u
#define cG  71u
#define cH  72u
#define cI  73u
#define cJ  74u
#define cK  75u
#define cL  76u
#define cM  77u
#define cN  78u
#define cO  79u
#define cP  80u
#define cQ  81u
#define cR  82u
#define cS  83u
#define cT  84u
#define cU  85u
#define cV  86u
#define cW  87u
#define cX  88u
#define cY  89u
#define cZ  90u
#define c0  48u
#define c1  49u
#define c2  50u
#define c3  51u
#define c4  52u
#define c5  53u
#define c6  54u
#define c7  55u
#define c8  56u
#define c9  57u
#define cSP     32u   // space
#define cEXL    33u   // 
#define cHASH   35u   // 
#define cPCT    37u   // 
#define cAP     39u   // apostrophe
#define cOB     40u   // 
#define cCB     41u   // 
#define cSTAR   42u   // 
#define cPL     43u   // 
#define cCOM    44u   // 
#define cDASH   45u   // 
#define cDOT    46u   // 
#define cSL     47u   // 
#define cCOL    58u   // 
#define cSEM    59u   // 
#define cLT     60u   // 
#define cEQ     61u   // 
#define cGT     62u   // 
#define cQUE    63u   // 
#define cAT     64u   // 
#define cOSB    91u   // 
#define cBS     92u   // backslash
#define cCSB    93u   // 
#define cUND    95u   // 
#define cDEG   176u   // degree

// Coverage of one glyph. q runs 0..1 across the character box, q.y = 0 at the top.
float GlyphAtlasInk(uint code, float2 q) {
    q = saturate(q);
    int idx = int(clamp((int)code - 32, 0, 94));
    float2 cell = float2(idx % 16, idx / 16);
    // The atlas is stored top-down and WIC loads it without flipping, so the cell's own top is the
    // smaller v - no 1.0 - on this one.
    return GlyphAtlas.Sample(LinearSampler, (cell + q) / float2(kAtlasCols, kAtlasRows)).x;
}

// `count` characters from `codes`, from origin (top-left), one character per `cell`. Monospaced on
// purpose: a table of live numbers has to keep its columns, and a proportional face makes every row
// jitter as the digits change.
float HudText(float2 uv, float2 origin, float2 cell, int count, const uint codes[32]) {
    float2 rel = (uv - origin) / cell;
    int col = int(floor(rel.x));
    if (col < 0 || col >= count) return 0.0;
    return GlyphAtlasInk(codes[col], float2(frac(rel.x), rel.y));
}

// A constant label. `count` characters from the leading entries; the rest of the array is zero and
// never read. Zero-filling the tail is what lets a four character label be written without spelling
// out twenty-eight more, and why an unused slot is 0 rather than cSP - 0 draws nothing.
//
// Twenty named parameters because the header's longest line is eighteen characters. fxc requires the
// elements of a local array initialiser to be compile-time constants, which is why each code is
// assigned one at a time rather than passed as a list.
float Lbl(float2 uv, float2 o, float2 cell, int count,
          uint a, uint b = 0, uint c = 0, uint d = 0, uint e = 0, uint f = 0,
          uint g = 0, uint h = 0, uint i1 = 0, uint j1 = 0, uint k1 = 0, uint l1 = 0,
          uint m1 = 0, uint n1 = 0, uint o1 = 0, uint p1 = 0, uint q1 = 0, uint r1 = 0,
          uint s1 = 0, uint t1 = 0) {
    uint codes[32];
    codes[0] = a;  codes[1] = b;  codes[2] = c;  codes[3] = d;
    codes[4] = e;  codes[5] = f;  codes[6] = g;  codes[7] = h;
    codes[8] = i1; codes[9] = j1; codes[10] = k1; codes[11] = l1;
    codes[12] = m1; codes[13] = n1; codes[14] = o1; codes[15] = p1;
    codes[16] = q1; codes[17] = r1; codes[18] = s1; codes[19] = t1;
    for (int z = 20; z < 32; ++z) codes[z] = 0u;
    return HudText(uv, o, cell, count, codes);
}

// A fixed-point number written straight from a float, because the value changes on the GPU and
// pre-formatting it on the CPU would mean a round trip per frame. The field is always exactly
// intDigits (+ point + fracDigits) wide. `pad` is cSP to blank leading zeros, c0 to keep them.
float HudNumber(float2 uv, float2 origin, float2 cell, float value, int intDigits, int fracDigits,
                uint pad) {
    intDigits = clamp(intDigits, 1, 12);
    fracDigits = clamp(fracDigits, 0, 4);
    bool neg = value < 0.0;
    float scale = 1.0;
    for (int s = 0; s < fracDigits; ++s) scale *= 10.0;
    float rounded = floor(min(abs(value), 1e12) * scale + 0.5);

    // Distinct loop names: fxc puts a loop's control variable in the enclosing function's scope, so
    // several loops all called `i` draw warnings and make a later loop look like it depends on an
    // earlier one.
    uint digits[16];
    int total = intDigits + fracDigits;
    for (int z = 0; z < 16; ++z) digits[z] = c0;
    for (int k = 0; k < total; ++k) {
        digits[total - 1 - k] = uint(fmod(rounded, 10.0)) + c0;
        rounded = floor(rounded / 10.0);
    }

    uint codes[32];
    int n = 0;
    if (neg) codes[n++] = cDASH;
    for (int d = 0; d < intDigits; ++d) {
        bool blank = pad == cSP && digits[d] == c0 && d + 1 < intDigits;
        codes[n++] = blank ? cSP : digits[d];
    }
    if (fracDigits > 0) {
        codes[n++] = cDOT;
        for (int q = 0; q < fracDigits; ++q) codes[n++] = digits[intDigits + q];
    }
    return HudText(uv, origin, cell, n, codes);
}

#endif // SW_GLYPHS_HLSL
