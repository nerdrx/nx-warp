#include "nxwarp_direct_recovery.h"
#include <chrono>
#include <cstdio>
#include <vector>
using namespace wivrn::nxwarp_direct;
using B = std::vector<uint8_t>;
static void put(B & b, uint32_t v) { append32(b, v); }
int main(int argc, char **) {
  layout l{2176, 2176, 2};
  const uint32_t n=l.tile_count(), words=n*80;
  B unit=frame_header(n,words);
  for(uint32_t i=0;i<n;i++) put(unit,i*80);
  unit.resize(unit.size()+size_t(words)*4,0x5a);
  const size_t chunk=1400;
  B wire; put(wire,uint32_t(unit.size())); wire.insert(wire.end(),unit.begin(),unit.end());
  std::vector<B> slots((wire.size()+chunk-1)/chunk);
  for(size_t i=0;i<slots.size();i++) slots[i]=B(wire.begin()+long(i*chunk),wire.begin()+long(std::min(wire.size(),(i+1)*chunk)));
  if(argc>1) { for(size_t i=slots.size()/2;i<slots.size();i++) slots[i].clear(); }
  else slots[slots.size()/2].clear();
  constexpr int runs=200;
  size_t accepted=0, bytes=0;
  auto t=std::chrono::steady_clock::now();
  for(int i=0;i<runs;i++) { auto r=recover_partial(l,slots,chunk,unit,n/10); if(r){accepted++;bytes+=r->unit.size();} }
  auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
  std::printf("runs=%d accepted=%zu bytes=%zu mean_ms=%.6f\n",runs,accepted,bytes,ms/runs);
  return accepted==(argc>1?0:runs)?0:2;
}
