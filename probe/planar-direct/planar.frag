#version 450
#ifndef PLANAR_OUTPUT_YUV
#define PLANAR_OUTPUT_YUV 0
#endif
// Standalone native PLANAR consumer.  Scope: CT_NONE, 8-bit 4:2:0, no alpha,
// 64x64 tiles. Binding 0 is 26 uints/tile (header,map[16],coef[9]); binding 1
// is four uints/tile, with w1 (the second uint) holding mode in low bits and
// signed QP delta in bits 8..15.
// Push constants are { int frameQP; int chromaQPOffset; int width; int height; }.
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0,std430) readonly buffer Planar { uint w[]; } plan;
layout(set=0,binding=1,std430) readonly buffer TileRec { uint r[]; } tiles;
layout(push_constant) uniform PC { int frameQP; int chromaQPOffset; int width; int height; } pc;
const int qstep[64] = int[64](16,18,20,23,25,29,32,36,40,45,51,57,64,72,81,91,102,114,128,144,161,181,203,228,256,287,323,362,406,456,512,575,645,724,813,912,1024,1149,1290,1448,1625,1825,2048,2299,2580,2896,3251,3649,4096,4598,5161,5793,6502,7298,8192,9195,10321,11585,13004,14596,16384,18390,20643,23170);
int dqstep(int qp) {
  // PLANAR coefficients use the DC quantizer, exactly as Pass B.
  int dcqp=clamp(qp,0,63)>>1;
  return (qstep[dcqp]*16+8)>>4;
}
int dq(int q,int t) { return clamp((q*t+8)>>4,-32768,32767); }
int sb(uint z,int n) { return (int(z << (24-8*n))) >> 24; }
int coeff(int base,int i,int qp) {
  int b=base+17+(i>>2); int q=dq(sb(plan.w[b],i&3),dqstep(qp)); return q;
}
int samplePlane(int tile,int plane,int x,int y,int size,int qp) {
  int base=tile*26; uint h=plan.w[base]; int regions=int(h&3u)+2;
  int mlog=((h>>3)&1u)!=0u?4:3; int M=1<<mlog; int lb=regions>2?2:1;
  int lg=(size==64)?6:5; int shift=lg-mlog;
  int row=(y>>shift)*M, cell=row+(x>>shift), bits=cell*lb;
  int mb=bits>>3; int mapw=base+1+(mb>>2);
  int region=int((plan.w[mapw]>>(((mb&3)*8)+(bits&7)))&uint((1<<lb)-1));
  int a=coeff(base,(region*3+plane)*3+0,qp);
  int dh=coeff(base,(region*3+plane)*3+1,qp);
  int dv=coeff(base,(region*3+plane)*3+2,qp);
  int dc=(plane==0)?128:128;
  return clamp(dc+a+((dh*(2*x-size+1))>>lg)+((dv*(2*y-size+1))>>lg),0,255);
}
void main() {
  ivec2 p=ivec2(clamp(uv*vec2(pc.width,pc.height),vec2(0),vec2(pc.width-1,pc.height-1)));
  int tx=p.x>>6, ty=p.y>>6, tilesX=(pc.width+63)>>6, tile=ty*tilesX+tx;
  uint rec=tiles.r[tile*4+1];
  uint mode=rec&7u;
  if(mode!=5u) { outColor=vec4(0,0,0,1); return; }
  int lx=p.x&63, ly=p.y&63;
  int qd=int((rec>>8)&63u); if(qd>=32) qd-=64;
  int qp=clamp(pc.frameQP+qd,0,63);
  int y=samplePlane(tile,0,lx,ly,64,qp);
  int cqp=clamp(qp+pc.chromaQPOffset,0,63);
  int cb=samplePlane(tile,1,lx>>1,ly>>1,32,cqp);
  int cr=samplePlane(tile,2,lx>>1,ly>>1,32,cqp);
#if PLANAR_OUTPUT_YUV
  // Diagnostic mode preserves coded samples for byte-wise comparison.
  outColor=vec4(vec3(y,cb,cr)/255.0,1.0);
#else
  // CT_NONE in the decoder's RGBA path is coded-domain RGB (no matrix).
  outColor=vec4(vec3(y,cb,cr)/255.0,1.0);
#endif
}
