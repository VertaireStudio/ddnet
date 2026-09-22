#ifdef TW_QUAD_TEXTURED
uniform sampler2D gTextureSampler;
#endif

#ifdef TW_QUAD_SSBO
struct SQuadEl
{
	vec4 gVertColor;
	vec2 gOffset;
	float gRotation;
};
layout(std430, binding = 7) buffer QuadDyn
{
	SQuadEl gUniEls[];
};
#define COLORS_OF(I) gUniEls[I].gVertColor
#else
#ifndef TW_QUAD_GROUPED
uniform vec4 gVertColors[TW_MAX_QUADS];
#else
uniform vec4 gVertColors[1];
#endif
#define COLORS_OF(I) gVertColors[I]
#endif

noperspective in vec4 QuadColor;
#ifndef TW_QUAD_GROUPED
flat in int QuadIndex;
#else
#define QuadIndex 0
#endif
#ifdef TW_QUAD_TEXTURED
noperspective in vec2 TexCoord;
#endif

out vec4 FragClr;
void main()
{
#ifdef TW_QUAD_TEXTURED
	vec4 TexColor = texture(gTextureSampler, TexCoord);
	FragClr = TexColor * QuadColor * COLORS_OF(QuadIndex);
#else
	FragClr = QuadColor * COLORS_OF(QuadIndex);
#endif
}
