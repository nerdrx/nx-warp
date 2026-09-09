#version 450
// Cost probe for the "Low poly" display filter of
// wivrn-nx-lowpoly/client/shaders/reprojection.glsl. The low_poly() body below is
// that file's, verbatim; everything around it is the smallest harness that can put
// it under a timestamp query.
layout(push_constant) uniform pc
{
	ivec4 rgb_rect;   // zw: the decoded image size the taps step through
	vec4 deband;      // y: strength, z: posterise levels -- as in the client
} P;

layout(constant_id = 0) const bool lowpoly_enable = false;
// 0 = hold all 25 taps in a vec3[25] (the straightforward transcription);
// 1 = re-fetch each quadrant's 9 taps, keeping one tap live at a time.
layout(constant_id = 1) const int lowpoly_variant = 0;

layout(set = 0, binding = 0) uniform sampler2D rgb;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 low_poly(vec2 uv, float strength, float levels)
{
	vec2 texel = 1.0 / vec2(P.rgb_rect.zw);

	vec3 c[25];
	float l[25];
	for (int j = 0; j < 5; ++j)
	{
		for (int i = 0; i < 5; ++i)
		{
			int k = j * 5 + i;
			c[k] = texture(rgb, uv + vec2(float(i - 2), float(j - 2)) * texel).rgb;
			l[k] = dot(c[k], vec3(0.299, 0.587, 0.114));
		}
	}

	const ivec2 corner[4] = ivec2[4](ivec2(0, 0), ivec2(2, 0), ivec2(0, 2), ivec2(2, 2));

	vec3 best_mean = c[12];
	float best_var = 3.4e38;
	for (int q = 0; q < 4; ++q)
	{
		vec3 sum = vec3(0.0);
		float ls = 0.0;
		float lss = 0.0;
		for (int j = 0; j < 3; ++j)
		{
			for (int i = 0; i < 3; ++i)
			{
				int k = (corner[q].y + j) * 5 + (corner[q].x + i);
				sum += c[k];
				ls += l[k];
				lss += l[k] * l[k];
			}
		}
		float mean_l = ls * (1.0 / 9.0);
		float var = max(lss * (1.0 / 9.0) - mean_l * mean_l, 0.0);
		if (var < best_var)
		{
			best_var = var;
			best_mean = sum * (1.0 / 9.0);
		}
	}

	vec3 result = mix(c[12], best_mean, strength);

	if (levels >= 2.0)
	{
		float steps = levels - 1.0;
		result = floor(clamp(result, 0.0, 1.0) * steps + 0.5) / steps;
	}

	return result;
}

// Same kernel, no 25-tap array: each quadrant re-fetches its own nine taps, so at
// most one tap plus the running sums is live. Thirty-seven taps instead of
// twenty-five, but the overlapping ones are texture cache hits, and nothing spills.
vec3 low_poly_refetch(vec2 uv, float strength, float levels)
{
	vec2 texel = 1.0 / vec2(P.rgb_rect.zw);
	vec3 best_mean = vec3(0.0);
	float best_var = 3.4e38;
	for (int q = 0; q < 4; ++q)
	{
		ivec2 o = ivec2((q & 1) * 2, (q >> 1) * 2) - ivec2(2, 2);
		vec3 sum = vec3(0.0);
		float ls = 0.0;
		float lss = 0.0;
		for (int j = 0; j < 3; ++j)
		{
			for (int i = 0; i < 3; ++i)
			{
				vec3 c = texture(rgb, uv + vec2(o + ivec2(i, j)) * texel).rgb;
				sum += c;
				float l = dot(c, vec3(0.299, 0.587, 0.114));
				ls += l;
				lss += l * l;
			}
		}
		float mean_l = ls * (1.0 / 9.0);
		float var = max(lss * (1.0 / 9.0) - mean_l * mean_l, 0.0);
		if (var < best_var)
		{
			best_var = var;
			best_mean = sum * (1.0 / 9.0);
		}
	}
	vec3 result = mix(texture(rgb, uv).rgb, best_mean, strength);
	if (levels >= 2.0)
	{
		float steps = levels - 1.0;
		result = floor(clamp(result, 0.0, 1.0) * steps + 0.5) / steps;
	}
	return result;
}

// Variant 2: the same sectored idea over a 6x6 support, but every tap is a BILINEAR
// tap placed on a texel corner, so the hardware returns the mean of a 2x2 block in one
// fetch. Four quadrants of four such taps: sixteen fetches covering thirty-six samples.
// The variance scored is that of the four block means rather than of sixteen samples,
// which is if anything the better statistic here -- it responds to edges and ignores
// the fine noise the filter is meant to discard.
vec3 low_poly_2x2(vec2 uv, float strength, float levels)
{
	vec2 texel = 1.0 / vec2(P.rgb_rect.zw);
	vec3 best_mean = vec3(0.0);
	float best_var = 3.4e38;
	for (int q = 0; q < 4; ++q)
	{
		// Which way this quadrant faces. Its four taps sit on TEXEL CORNERS at
		// +-0.5 and +-2.5 texels, so each bilinear fetch returns the mean of one
		// 2x2 block and the quadrant covers a 4x4 corner of a 7x7 neighbourhood.
		vec2 s = vec2(float((q & 1) * 2 - 1), float((q >> 1) * 2 - 1));
		vec3 sum = vec3(0.0);
		float ls = 0.0, lss = 0.0;
		for (int j = 0; j < 2; ++j)
		{
			for (int i = 0; i < 2; ++i)
			{
				vec2 off = s * (vec2(0.5) + 2.0 * vec2(float(i), float(j)));
				vec3 c = texture(rgb, uv + off * texel).rgb;
				sum += c;
				float l = dot(c, vec3(0.299, 0.587, 0.114));
				ls += l;
				lss += l * l;
			}
		}
		float mean_l = ls * 0.25;
		float var = max(lss * 0.25 - mean_l * mean_l, 0.0);
		if (var < best_var)
		{
			best_var = var;
			best_mean = sum * 0.25;
		}
	}
	vec3 result = mix(texture(rgb, uv).rgb, best_mean, strength);
	if (levels >= 2.0)
	{
		float steps = levels - 1.0;
		result = floor(clamp(result, 0.0, 1.0) * steps + 0.5) / steps;
	}
	return result;
}

// Variant 3: plain 3x3 sectored -- four 2x2 quadrants sharing the centre. Seventeen
// fetches, the smallest kernel that is still a sectored filter.
// Eight extra taps: approximate quadrant variance from two bilinear blocks.
vec3 low_poly_3x3(vec2 uv, float strength, float levels)
{
 vec2 texel=1.0/vec2(P.rgb_rect.zw);
 vec3 best=vec3(0.0); float score=3.4e38;
 for(int q=0;q<4;++q) {
  vec2 s=vec2(float((q&1)*2-1),float((q>>1)*2-1));
  vec3 a=texture(rgb,uv+s*vec2(0.5,2.5)*texel).rgb;
  vec3 b=texture(rgb,uv+s*vec2(2.5,0.5)*texel).rgb;
  float delta=dot(a-b,vec3(0.299,0.587,0.114));
  float v=delta*delta;
  if(v<score) {score=v;best=(a+b)*0.5;}
 }
 return mix(texture(rgb,uv).rgb,best,strength);
}

// Four neighbouring block means, each scored against the shared centre.
// This is a Kuwahara-inspired approximation, not the full quadrant variance.
vec3 low_poly_four(vec2 uv, float strength, float levels) {
 vec2 texel=1.0/vec2(P.rgb_rect.zw);
 vec3 centre=texture(rgb,uv).rgb;
 vec3 best=centre; float score=3.4e38;
 for(int q=0;q<4;++q) {
  vec2 s=vec2(float((q&1)*2-1),float((q>>1)*2-1));
  vec3 c=texture(rgb,uv+s*1.5*texel).rgb;
  float delta=dot(c-centre,vec3(0.299,0.587,0.114));
  float v=delta*delta;
  if(v<score) {score=v;best=(centre+c)*0.5;}
 }
 return mix(centre,best,strength);
}

void main()
{
	vec4 colour = texture(rgb, vUV);
	if (lowpoly_enable && P.deband.y > 0.0)
	{
		if (lowpoly_variant == 0)
			colour.rgb = low_poly(vUV, P.deband.y, P.deband.z);
		else if (lowpoly_variant == 1)
			colour.rgb = low_poly_refetch(vUV, P.deband.y, P.deband.z);
		else if (lowpoly_variant == 2)
			colour.rgb = low_poly_2x2(vUV, P.deband.y, P.deband.z);
		else if (lowpoly_variant == 3)
			colour.rgb = low_poly_3x3(vUV, P.deband.y, P.deband.z);
        else colour.rgb = low_poly_four(vUV, P.deband.y, P.deband.z);
	}
	outColor = colour;
}
