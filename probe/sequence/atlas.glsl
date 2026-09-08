// Copied verbatim from wt-atlas-live-240/client/shaders/reprojection_atlas_r8.glsl; probe build compiles VERT_SHADER or FRAG_SHADER.
#version 450
// R8 atlas-only display consumer; ordinary effects remain in reprojection.glsl.
layout(push_constant) uniform pc { ivec4 rgb_rect; ivec4 a_rect; vec4 scale; vec4 bias; vec4 post; vec4 motion; vec4 glow; vec4 deband; };

#ifdef VERT_SHADER
layout(constant_id=6) const int atlas_eye=0;
layout(constant_id=9) const bool atlas_vertex_warp=false;
layout(location=0) in vec2 vPosition;
layout(location=1) in uvec2 vUV;
layout(location=2) in uint vTile;
layout(location=0) out vec4 outUV;
layout(location=1) out vec4 outPosition;
layout(location=2) out vec3 outQ;
layout(location=3) flat out uint outValid;
layout(set=0,binding=3) uniform sampler2D atlas_y_v;
layout(std430,set=0,binding=5) readonly buffer atlas_table_v_t { uint e[]; } tbl_v;
void main() {
 gl_Position=vec4(vPosition,0.0,1.0);
 vec2 uv=vec2(vUV);
 outUV=vec4((uv+vec2(rgb_rect.xy))/vec2(rgb_rect.zw),(uv+vec2(a_rect.xy))/vec2(a_rect.zw));
 outPosition=vec4(vPosition,0.0,0.0);
 outQ=vec3(0.0);
 outValid=1u;
 if (atlas_vertex_warp) {
  vec2 image=vec2(textureSize(atlas_y_v,0));
  vec2 picture=vec2(image.x*0.5,image.y);
  uint cols=uint(ceil(picture.x/64.0));
  uint tx=vTile%cols;
  uint ty=vTile/cols;
  uint idx=(ty*cols*2u+uint(atlas_eye)*cols+tx)*16u;
  outValid=(tbl_v.e[idx+10u]>>16u)&1u;
  // vUV is already an eye-local source-pixel coordinate. Keep q homogeneous
  // across each triangle; dividing at vertices would distort the interpolation.
  vec3 h=vec3(uv-0.5*picture,1.0);
  vec3 r0=vec3(int(tbl_v.e[idx]),int(tbl_v.e[idx+1u]),int(tbl_v.e[idx+2u]))/2097152.0;
  vec3 r1=vec3(int(tbl_v.e[idx+3u]),int(tbl_v.e[idx+4u]),int(tbl_v.e[idx+5u]))/2097152.0;
  vec3 r2=vec3(int(tbl_v.e[idx+6u]),int(tbl_v.e[idx+7u]),int(tbl_v.e[idx+8u]))/536870912.0;
  outQ=vec3(dot(r0,h),dot(r1,h),dot(r2,h));
 }
}
#endif

#ifdef FRAG_SHADER
layout(constant_id=0) const int alpha=1;
layout(constant_id=1) const bool do_srgb=false;
layout(constant_id=2) const bool cas_full_kernel=false;
layout(constant_id=3) const bool fsr_enable=false;
layout(constant_id=4) const int atlas_mode=1;
layout(constant_id=5) const int atlas_tiles=17;
layout(constant_id=6) const int atlas_eye=0;
layout(constant_id=7) const bool lowpoly_enable=false;
layout(constant_id=8) const bool lowpoly_full_kernel=false;
layout(constant_id=9) const bool atlas_vertex_warp=false;
layout(set=0,binding=0) uniform sampler2D rgb[alpha+1];
layout(set=0,binding=3) uniform sampler2D atlas_y;
layout(set=0,binding=4) uniform sampler2D atlas_cbcr;
layout(std430,set=0,binding=5) readonly buffer atlas_table_t { uint e[]; } tbl;
layout(location=0) in vec4 inUV;
layout(location=1) in vec4 inPosition;
layout(location=2) in vec3 inQ;
layout(location=3) flat in uint inValid;
layout(location=0) out vec4 outColor;
// The decoder view spans both eyes horizontally; derive the geometry from the
// actual snapshot so non-1088 streams and clipped edge tiles use the same table.
vec3 srgb_linear(vec3 c) { return mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),step(vec3(0.04045),c)); }
void main() {
	vec2 luma_image=vec2(textureSize(atlas_y,0));
	vec2 picture=vec2(luma_image.x*0.5,luma_image.y);
	vec2 chroma_image=vec2(textureSize(atlas_cbcr,0));
 vec2 src;
 if (atlas_vertex_warp) {
  if (inValid==0u || abs(inQ.z)<1e-8) { outColor=vec4(0); return; }
  src=clamp(inQ.xy/inQ.z+picture*0.5,vec2(0),picture-1.0);
 } else {
  vec2 eye_uv=clamp((inUV.xy*vec2(rgb_rect.zw)-vec2(rgb_rect.xy))/picture,vec2(0),vec2(0.999999));
  int tiles_x=int(ceil(picture.x/64.0));
  ivec2 cell=ivec2(floor((eye_uv*picture)/64.0));
  cell=clamp(cell,ivec2(0),ivec2(tiles_x-1,int(ceil(picture.y/64.0))-1));
  int idx=(cell.y*tiles_x*2+atlas_eye*tiles_x+cell.x)*16;
  if (((tbl.e[idx+10]>>16u)&1u)==0u) { outColor=vec4(0); return; }
  vec3 h=vec3((eye_uv-0.5)*picture,1.0);
  vec3 r0=vec3(int(tbl.e[idx]),int(tbl.e[idx+1]),int(tbl.e[idx+2]))/2097152.0;
  vec3 r1=vec3(int(tbl.e[idx+3]),int(tbl.e[idx+4]),int(tbl.e[idx+5]))/2097152.0;
  vec3 r2=vec3(int(tbl.e[idx+6]),int(tbl.e[idx+7]),int(tbl.e[idx+8]))/536870912.0;
  vec3 q=vec3(dot(r0,h),dot(r1,h),dot(r2,h));
  if (abs(q.z)<1e-8) { outColor=vec4(0); return; }
  src=clamp(q.xy/q.z+picture*0.5,vec2(0),picture-1.0);
 }
 vec2 lp=src+vec2(float(atlas_eye)*picture.x,0);
 vec2 cp=src*0.5+vec2(float(atlas_eye)*picture.x*0.5,0);
 float y=texture(atlas_y,(lp+0.5)/luma_image).r;
 vec2 cbcr=texture(atlas_cbcr,(cp+0.5)/chroma_image).rg-0.5;
 vec3 c=vec3(y+1.5748*cbcr.y,y-0.1873*cbcr.x-0.4681*cbcr.y,y+1.8556*cbcr.x);
 c=clamp(c,0.0,1.0);
 if (do_srgb) c=srgb_linear(c);
 outColor=vec4(clamp(c,0.0,1.0)*scale.rgb+bias.rgb,1.0);
}
#endif
