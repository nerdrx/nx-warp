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
    if (b.size()!=1183744) throw std::runtime_error("expected headerless native2176 ASTC8x8 blocks");
    return b;
}
static std::string path(const std::string& dir, int mode, int eye) {
    return dir+"/packet-"+std::to_string(mode)+"-"+std::to_string(eye)+".bin";
}
int main(int argc, char** argv) {
    try {
        if(argc!=5) throw std::runtime_error("usage: decode decode LEFT.raw RIGHT.raw PACKET_DIR");
        const std::array<Bytes,2> raw{astc(argv[2]),astc(argv[3])};
        const std::array<const char*,3> names{"ordinary_l3","bounded_jobs","automatic_workers"};
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
        // ABCCBA; 10 warm-up and 40 measured calls per mode; exact-byte checks outside timed region.
        for(int block=0;block<25;block++)for(int m:{0,1,2,2,1,0}) {
            std::array<double,2> us{}; std::array<decode_status,2> status{};
            auto start=Clock::now();
            for(int e=0;e<2;e++) {
                auto t=Clock::now(); const auto& h=headers[m][e];
                status[e]=decode_payload(h,std::span<const uint8_t>(packets[m][e]).subspan(h.header_bytes),output[e]);
                us[e]=std::chrono::duration<double,std::micro>(Clock::now()-t).count();
            }
            double pair=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
            for(int e=0;e<2;e++)if(status[e]!=decode_status::ok || output[e]!=raw[e])throw std::runtime_error("decode exactness");
            if(block>=5)std::cout<<seq++<<','<<names[m]<<','<<pair<<','<<us[0]<<','<<us[1]<<','<<packets[m][0].size()<<','<<packets[m][1].size()<<'\n';
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
