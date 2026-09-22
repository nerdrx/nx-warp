#include <lz4.h>
#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

constexpr size_t chunk = 65536;
#ifndef DATA_ROOT
#define DATA_ROOT "/tmp/nx-user-scenes"
#endif
using bytes = std::vector<uint8_t>;
enum class transform { plain, shuffle, delta, both };

static bytes xform(const uint8_t *src, size_t n, transform t, bool inverse = false)
{
	if (t == transform::both && inverse) {
		auto un_delta = xform(src, n, transform::delta, true);
		return xform(un_delta.data(), n, transform::shuffle, true);
	}
	bytes out(n);
	for (size_t base = 0; base < n; base += chunk) {
		const size_t len = std::min(chunk, n - base), words = len / 4;
		if (t == transform::shuffle || t == transform::both) {
			if (!inverse) {
				for (size_t j = 0; j < 4; ++j)
					for (size_t p = 0; p < words; ++p)
						out[base + j * words + p] = src[base + 4 * p + j];
				std::copy(src + base + 4 * words, src + base + len, out.begin() + base + 4 * words);
			} else {
				for (size_t p = 0; p < words; ++p)
					for (size_t j = 0; j < 4; ++j)
						out[base + 4 * p + j] = src[base + j * words + p];
				std::copy(src + base + 4 * words, src + base + len, out.begin() + base + 4 * words);
			}
		} else std::copy(src + base, src + base + len, out.begin() + base);
		if (t == transform::delta || t == transform::both) {
			uint8_t prev = 0;
			for (size_t i = base; i < base + len; ++i) {
				const uint8_t v = out[i];
				if (inverse) out[i] = uint8_t(v + prev); else out[i] = uint8_t(v - prev);
				prev = inverse ? out[i] : v;
			}
		}
	}
	return out;
}

static bytes lz4_pack(const bytes &raw, transform t)
{
	bytes out(16); uint32_t count = (raw.size() + chunk - 1) / chunk;
	for (uint32_t i = 0; i < 4; ++i) out[i * 4] = uint8_t((i == 0 ? 0x4c44584e : i == 1 ? 1 : i == 2 ? raw.size() : count) >> 0);
	for (uint32_t i = 0; i < 4; ++i) { uint32_t v = i == 0 ? 0x4c44584e : i == 1 ? 1 : i == 2 ? uint32_t(raw.size()) : count; for (unsigned k=0;k<4;++k) out[i*4+k]=uint8_t(v>>(8*k)); }
	for (size_t pos = 0; pos < raw.size(); pos += chunk) {
		size_t n = std::min(chunk, raw.size() - pos); auto in = xform(raw.data() + pos, n, t);
		int cap = LZ4_compressBound(int(n)); bytes packed(cap);
		int got = LZ4_compress_default((char*)in.data(), (char*)packed.data(), int(n), cap);
		uint32_t stored = got > 0 && size_t(got) < n ? uint32_t(got) : uint32_t(n), flag = stored != n;
		size_t h = out.size(); out.resize(h + 12 + stored);
		if (flag) std::copy(packed.begin(), packed.begin() + stored, out.begin() + h + 12); else std::copy(in.begin(), in.end(), out.begin() + h + 12);
		for (unsigned k=0;k<4;++k) { out[h+k]=uint8_t(n>>(8*k)); out[h+4+k]=uint8_t(stored>>(8*k)); out[h+8+k]=uint8_t(flag>>(8*k)); }
	}
	return out;
}

static bytes lz4_unpack(const bytes &wire, transform t, size_t raw_size)
{
	bytes out; out.reserve(raw_size); size_t pos = 16;
	while (out.size() < raw_size) {
		uint32_t n=0,stored=0,flag=0; for(unsigned k=0;k<4;++k){n|=uint32_t(wire[pos+k])<<(8*k);stored|=uint32_t(wire[pos+4+k])<<(8*k);flag|=uint32_t(wire[pos+8+k])<<(8*k);} pos+=12;
		bytes decoded(n); if(flag) { int got=LZ4_decompress_safe((char*)wire.data()+pos,(char*)decoded.data(),int(stored),int(n)); if(got!=int(n)) std::abort(); } else std::copy(wire.begin()+pos,wire.begin()+pos+n,decoded.begin()); pos+=stored;
		auto plain=xform(decoded.data(),decoded.size(),t,true); out.insert(out.end(),plain.begin(),plain.end());
	} return out;
}

static bytes zstd_pack(const bytes &raw) { bytes out(ZSTD_compressBound(raw.size())); size_t n=ZSTD_compress(out.data(),out.size(),raw.data(),raw.size(),1); if(ZSTD_isError(n)) std::abort(); out.resize(n); return out; }
static bytes zstd_unpack(const bytes &wire, size_t n) { bytes out(n); size_t got=ZSTD_decompress(out.data(),n,wire.data(),wire.size()); if(ZSTD_isError(got)||got!=n) std::abort(); return out; }
static bytes zstd_pack_chunks(const bytes &raw) {
	bytes out(16); uint32_t count=(raw.size()+chunk-1)/chunk;
	for(unsigned i=0;i<4;++i){uint32_t v=i==0?0x5a44584e:i==1?1:i==2?uint32_t(raw.size()):count;for(unsigned k=0;k<4;++k)out[i*4+k]=uint8_t(v>>(8*k));}
	for(size_t pos=0;pos<raw.size();pos+=chunk){size_t n=std::min(chunk,raw.size()-pos);bytes packed(ZSTD_compressBound(n));size_t got=ZSTD_compress(packed.data(),packed.size(),raw.data()+pos,n,1);if(ZSTD_isError(got))std::abort();size_t h=out.size();out.resize(h+12+got);for(unsigned k=0;k<4;++k){out[h+k]=uint8_t(n>>(8*k));out[h+4+k]=uint8_t(got>>(8*k));out[h+8+k]=1;}std::copy(packed.begin(),packed.begin()+got,out.begin()+h+12);}return out;
}
static bytes zstd_unpack_chunks(const bytes &wire, size_t n) {
	bytes out;out.reserve(n);size_t pos=16;while(out.size()<n){uint32_t un=0,stored=0;for(unsigned k=0;k<4;++k){un|=uint32_t(wire[pos+k])<<(8*k);stored|=uint32_t(wire[pos+4+k])<<(8*k);}pos+=12;bytes block(un);size_t got=ZSTD_decompress(block.data(),un,wire.data()+pos,stored);if(ZSTD_isError(got)||got!=un)std::abort();out.insert(out.end(),block.begin(),block.end());pos+=stored;}return out;
}

template<class F> static std::pair<double,double> bench(F f, int reps=101) { std::vector<double> ms; ms.reserve(reps); for(int i=0;i<reps;++i){auto a=std::chrono::steady_clock::now(); f(); auto b=std::chrono::steady_clock::now(); ms.push_back(std::chrono::duration<double,std::milli>(b-a).count());} std::sort(ms.begin(),ms.end()); return {ms[reps/2],ms[reps*95/100]}; }

static void run_dir(const char *dir, const char *suffix) {
	for (const char *name : {"forest", "dark", "crowd"}) {
		const std::string path = std::string(DATA_ROOT) + "/" + dir + "/" + name + suffix;
		std::ifstream f(path,std::ios::binary); bytes raw((std::istreambuf_iterator<char>(f)),{});
		if (raw.empty()) continue;
		for (const auto method : {std::pair{"lz4",transform::plain}, {"shuffle",transform::shuffle}, {"delta",transform::delta}, {"both",transform::both}}) {
			const auto label = method.first; const auto t = method.second;
			bytes wire=lz4_pack(raw,t); auto enc=bench([&]{auto x=lz4_pack(raw,t); (void)x;}); auto dec=bench([&]{auto x=lz4_unpack(wire,t,raw.size()); (void)x;}); auto check=lz4_unpack(wire,t,raw.size()); if(check!=raw) std::abort();
			std::printf("%s/%s %s %zu %.2f%% enc %.3f/%.3f dec %.3f/%.3f\n",dir,name,label,wire.size(),100.0*(1.0-double(wire.size())/raw.size()),enc.first,enc.second,dec.first,dec.second);
		}
		bytes wire=zstd_pack(raw); auto enc=bench([&]{auto x=zstd_pack(raw); (void)x;}); auto dec=bench([&]{auto x=zstd_unpack(wire,raw.size()); (void)x;}); if(zstd_unpack(wire,raw.size())!=raw) std::abort();
		std::printf("%s/%s zstd1 %zu %.2f%% enc %.3f/%.3f dec %.3f/%.3f\n",dir,name,wire.size(),100.0*(1.0-double(wire.size())/raw.size()),enc.first,enc.second,dec.first,dec.second);
		wire=zstd_pack_chunks(raw); enc=bench([&]{auto x=zstd_pack_chunks(raw);(void)x;}); dec=bench([&]{auto x=zstd_unpack_chunks(wire,raw.size());(void)x;}); if(zstd_unpack_chunks(wire,raw.size())!=raw)std::abort();
		std::printf("%s/%s zstd1-64k %zu %.2f%% enc %.3f/%.3f dec %.3f/%.3f\n",dir,name,wire.size(),100.0*(1.0-double(wire.size())/raw.size()),enc.first,enc.second,dec.first,dec.second);
	}
}

int main() {
	run_dir(".", "-500.nxdf");
	run_dir("reference500", "-rgb565.nxdf");
	run_dir("reference500", "-rgb888.nxdf");
}
