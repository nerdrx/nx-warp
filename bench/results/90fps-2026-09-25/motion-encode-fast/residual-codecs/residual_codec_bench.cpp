#include "mixed_motion_bench.cpp"

static Bytes own(std::span<const uint8_t> b) { return {b.begin(),b.end()}; }
static bool unpack_any(std::span<const uint8_t> b, Bytes& out) {
 layout l{W,H,E,true,256,false,true,true,false};
 if(is_zstd(b)) return decompress_zstd(l,b,out);
 if(is_lz4(b)) return decompress_lz4(l,b,out);
 out.assign(b.begin(),b.end()); return true;
}
static double quant(std::vector<double> v,int p) { std::sort(v.begin(),v.end()); return v[(v.size()*p+99)/100-1]; }
static bool test(std::string name, const Bytes& r) {
 Bytes fast,z,p,s; auto f=compress_lz4(r,fast); auto d=compress_zstd(r,z); auto q=compress_zstd_predicted(r,p,s);
 Bytes full; auto pred=q.size()*100<=d.size()*95?q:d; auto selected=pred.size()*100<=f.size()*90?pred:f; full=own(selected);
 Bytes plain=own(d), predicted=own(q);
 struct M { const char* n; Bytes* b; } modes[]={{"plain",&plain},{"pred",&predicted},{"full",&full}};
 std::cout<<name<<",raw="<<r.size();
 for(auto m:modes) {
  Bytes decoded; if(!unpack_any(*m.b,decoded)||decoded!=r) return false;
  std::vector<double> times;
  for(int i=0;i<32;i++) { Bytes a,b,c,sc; auto t=Clock::now();
   std::span<const uint8_t> out;
   if(std::string(m.n)=="plain") out=compress_zstd(r,a);
   else if(std::string(m.n)=="pred") out=compress_zstd_predicted(r,b,sc);
   else { auto lf=compress_lz4(r,a); auto lz=compress_zstd(r,b); auto lp=compress_zstd_predicted(r,c,sc); auto dd=lp.size()*100<=lz.size()*95?lp:lz; out=dd.size()*100<=lf.size()*90?dd:lf; }
   if(i>=8) times.push_back(std::chrono::duration<double,std::micro>(Clock::now()-t).count());
  }
  std::cout<<","<<m.n<<"_bytes="<<m.b->size()<<","<<m.n<<"_p50_us="<<quant(times,50)<<","<<m.n<<"_p95_us="<<quant(times,95);
 }
 std::cout<<",exact=1\n"; return true;
}
int main(int argc, char** argv) {
 if (argc != 3) return 2;
 Bytes forest = read(argv[1]), dark = read(argv[2]);
 NativeInfo fi, di;
 if (!build_native(forest, fi) || !build_native(dark, di)) return 3;
 Bytes shift = make_regional_frame(forest, fi, 1, {{{8, 0}, {-8, 0}, {0, 0}, {0, 0}}});
 NativeInfo si;
 if (!build_native(shift, si)) return 4;
 Bytes mixed = residual_frame(forest, fi, shift, si, 0, 0);
 Bytes scene = residual_frame(forest, fi, dark, di, 0, 0);
 Bytes moving = residual_frame(forest, fi, shift, si, 8, 0);
 if (!test("forest_photo_residual", forest) || !test("dark_photo_residual", dark) ||
     !test("mixed_halves_global_residual", mixed) || !test("mixed_halves_correct_residual", moving) ||
     !test("scene_cut_residual", scene)) return 5;
}
