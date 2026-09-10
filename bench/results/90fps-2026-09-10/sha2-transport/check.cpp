#include "nxvc/transport/aead.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <array>
#include <algorithm>
#include <cassert>
int main(int argc,char**argv){
 auto a=nxt::make_null_aead();nxt::Key key{};nxt::Nonce nonce{};
 std::vector<unsigned char> aad(129),pt(4096),ct(4112),out(4096);
 auto init=[&](int seed){for(size_t i=0;i<key.size();++i)key[i]=i*13+seed;for(size_t i=0;i<nonce.size();++i)nonce[i]=i*7+seed;for(size_t i=0;i<aad.size();++i)aad[i]=i*17+seed;for(size_t i=0;i<pt.size();++i)pt[i]=i*31+seed;};
 init(3);
 if(argc>1){
  constexpr int loops=12000;auto ad=std::span<const uint8_t>(aad.data(),20);auto plain=std::span<const uint8_t>(pt.data(),1166);
  auto size=a->seal(key,nonce,ad,plain,ct.data());unsigned checksum=0;
  auto t=std::chrono::steady_clock::now();for(int i=0;i<loops;++i){auto n=a->open(key,nonce,ad,{ct.data(),size},out.data());assert(n==1166);checksum+=out[i%1166];}auto end=std::chrono::steady_clock::now();
  printf("{\"packets\":%d,\"payload_bytes\":1166,\"open_us\":%.6f,\"checksum\":%u}\n",loops,std::chrono::duration<double,std::micro>(end-t).count()/loops,checksum);return 0;
 }
 int cases=0;for(int seed=0;seed<4;++seed){init(seed);for(size_t n:std::array<size_t,22>{0,1,15,16,31,32,33,55,56,63,64,65,111,112,127,128,129,1023,1166,1280,2048,4096})for(size_t an:std::array<size_t,6>{0,1,20,55,64,129}){
  auto ad=std::span<const uint8_t>(aad.data(),an);auto size=a->seal(key,nonce,ad,{pt.data(),n},ct.data());assert(size==n+nxt::kTagBytes);fwrite(ct.data(),1,size,stdout);
  auto got=a->open(key,nonce,ad,{ct.data(),size},out.data());assert(got==n&&std::equal(pt.begin(),pt.begin()+n,out.begin()));
  ct[size-1]^=1;assert(a->open(key,nonce,ad,{ct.data(),size},out.data())==SIZE_MAX);ct[size-1]^=1;
  nonce[0]^=1;assert(a->open(key,nonce,ad,{ct.data(),size},out.data())==SIZE_MAX);nonce[0]^=1;
  assert(a->open(key,nonce,ad,{ct.data(),nxt::kTagBytes-1},out.data())==SIZE_MAX);++cases;
 }}fprintf(stderr,"%d cases: round-trip and corruption/nonce/truncation rejection passed\n",cases);
}
