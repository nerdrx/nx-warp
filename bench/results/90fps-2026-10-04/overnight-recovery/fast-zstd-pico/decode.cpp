#include "nxastc_packet_decode.h"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
using namespace wivrn::nxastc_packet;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
static Bytes read(const std::string& name) {
    std::ifstream f(name, std::ios::binary);
    if (!f) throw std::runtime_error("input open");
    return Bytes(std::istreambuf_iterator<char>(f), {});
}
static Bytes astc(const std::string& name) {
    Bytes b=read(name);
    if (b.size()!=1183760 || b[0]!=0x13 || b[1]!=0xab || b[2]!=0xa1 || b[3]!=0x5c || b[4]!=8 || b[5]!=8 || b[6]!=1)
        throw std::runtime_error("expected native ASTC 8x8 fixture");
    auto u24=[&](int i) { return unsigned(b[i])|(unsigned(b[i+1])<<8)|(unsigned(b[i+2])<<16); };
    if(u24(7)!=2176 || u24(10)!=2176 || u24(13)!=1) throw std::runtime_error("dimensions");
    return Bytes(b.begin()+16,b.end());
}
static std::string path(const std::string& dir, int mode, int eye) {
    return dir+"/packet-"+std::to_string(mode)+"-"+std::to_string(eye)+".bin";
}
int main(int argc, char** argv) {
    try {
        if(argc!=5) throw std::runtime_error("usage: decode pack|decode LEFT.astc RIGHT.astc PACKET_DIR");
        const std::array<Bytes,2> raw{astc(argv[2]),astc(argv[3])};
        const std::array<const char*,3> names{"ordinary_l3","ordinary_l1","compact_l1"};
        if(std::string(argv[1])=="pack") {
            ZSTD_CCtx* ctx=ZSTD_createCCtx();
            if(!ctx) throw std::runtime_error("context");
            for(int mode=0;mode<3;mode++) for(int eye=0;eye<2;eye++) {
                Bytes input=raw[eye];
                if(mode==2) {
                    input.resize(raw[eye].size()/16*14);
                    if(!compact_blocks(raw[eye],input)) throw std::runtime_error("compact unsupported");
                }
                Bytes payload(ZSTD_compressBound(input.size()));
                size_t n=ZSTD_compressCCtx(ctx,payload.data(),payload.size(),input.data(),input.size(),mode==0?3:1);
                if(ZSTD_isError(n) || n>raw[eye].size()/2 || n*100>raw[eye].size()*90) throw std::runtime_error("fixture does not select production Zstd");
                payload.resize(n);
                auto h=make_header(2176,2176,uint32_t(n),mode==2?compression::compact_zstd:compression::zstd);
                Bytes packet(h.begin(),h.end()); packet.insert(packet.end(),payload.begin(),payload.end());
                Bytes check(raw[eye].size()); auto parsed=parse_packet(packet);
                if(!parsed || decode_payload(*parsed,std::span<const uint8_t>(packet).subspan(parsed->header_bytes),check)!=decode_status::ok || check!=raw[eye]) throw std::runtime_error("packet exactness");
                std::ofstream out(path(argv[4],mode,eye),std::ios::binary);
                out.write(reinterpret_cast<const char*>(packet.data()),packet.size());
                if(!out) throw std::runtime_error("packet write");
                std::cout<<names[mode]<<','<<eye<<','<<packet.size()<<'\n';
            }
            ZSTD_freeCCtx(ctx); return 0;
        }
        if(std::string(argv[1])!="decode") throw std::runtime_error("unknown operation");
        std::array<std::array<Bytes,2>,3> packets;
        std::array<std::array<packet_header,2>,3> headers;
        std::array<Bytes,2> output{Bytes(raw[0].size()),Bytes(raw[1].size())};
        for(int m=0;m<3;m++)for(int e=0;e<2;e++) {
            packets[m][e]=read(path(argv[4],m,e)); auto h=parse_packet(packets[m][e]);
            if(!h || h->width!=2176 || h->height!=2176)throw std::runtime_error("packet header");
            headers[m][e]=*h;
        }
        std::cout<<"sequence,mode,pair_us,left_us,right_us,left_bytes,right_bytes\n";
        int seq=0;
        // ABCCBA; 20 warm-up and 100 measured calls per mode.
        for(int block=0;block<60;block++)for(int m:{0,1,2,2,1,0}) {
            std::array<double,2> us{}; std::array<decode_status,2> status{};
            auto start=Clock::now();
            for(int e=0;e<2;e++) {
                auto t=Clock::now(); const auto& h=headers[m][e];
                status[e]=decode_payload(h,std::span<const uint8_t>(packets[m][e]).subspan(h.header_bytes),output[e]);
                us[e]=std::chrono::duration<double,std::micro>(Clock::now()-t).count();
            }
            double pair=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
            for(int e=0;e<2;e++)if(status[e]!=decode_status::ok || output[e]!=raw[e])throw std::runtime_error("decode exactness");
            if(block>=10)std::cout<<seq++<<','<<names[m]<<','<<pair<<','<<us[0]<<','<<us[1]<<','<<packets[m][0].size()<<','<<packets[m][1].size()<<'\n';
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
