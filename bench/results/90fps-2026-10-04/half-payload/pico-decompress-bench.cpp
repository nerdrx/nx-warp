#include <lz4.h>
#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<char> read(const char *p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("open failed: ")+p);
    return {std::istreambuf_iterator<char>(f), {}};
}
static double pct(std::vector<double> x, double p) {
    std::sort(x.begin(), x.end()); return x[size_t((x.size()-1)*p)];
}
int main(int argc,char **argv) try {
    if(argc!=5) throw std::runtime_error("usage: bench compressed raw lz4|zstd tag");
    auto packed=read(argv[1]), expected=read(argv[2]);
    std::vector<char> out(expected.size()); std::vector<double> ms;
    auto decode=[&]() -> size_t {
        if(std::string(argv[3])=="lz4") return LZ4_decompress_safe(packed.data(),out.data(),int(packed.size()),int(out.size()));
        if(std::string(argv[3])=="zstd") return ZSTD_decompress(out.data(),out.size(),packed.data(),packed.size());
        throw std::runtime_error("codec must be lz4 or zstd");
    };
    for(int i=0;i<5;i++) if(decode()!=expected.size()) throw std::runtime_error("warmup decode failed");
    for(int i=0;i<30;i++) {
        auto a=std::chrono::steady_clock::now(); auto n=decode(); auto b=std::chrono::steady_clock::now();
        if(n!=expected.size()) throw std::runtime_error("decode size mismatch");
        ms.push_back(std::chrono::duration<double,std::milli>(b-a).count());
    }
    if(out!=expected) throw std::runtime_error("decompressed bytes differ from source");
    std::cout<<argv[4]<<','<<argv[3]<<','<<packed.size()<<','<<expected.size()<<','<<pct(ms,.5)<<','<<pct(ms,.95)<<",30,exact\n";
} catch(const std::exception &e) { std::cerr<<"error: "<<e.what()<<'\n'; return 1; }
