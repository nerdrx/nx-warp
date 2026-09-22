#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <fstream>
extern const std::map<std::string,std::vector<uint32_t>> shaders = [] {
std::ifstream f(std::getenv("NX_PALETTE_SPV"),std::ios::binary|std::ios::ate);if(!f)std::abort();auto n=f.tellg();std::vector<uint32_t> v(size_t(n)/4);f.seekg(0);f.read((char*)v.data(),n);if(!f)std::abort();return std::map<std::string,std::vector<uint32_t>>{{"direct_blocks_encode",std::move(v)}};
}();
