#define ZSTD_STATIC_LINKING_ONLY
#include "nxastc_packet.h"
#include "nxastc_packet_decode.h"

#include <lz4.h>
#include <zstd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;
using Packet = std::shared_ptr<Bytes>;
constexpr uint32_t W=2176,H=2176;
constexpr size_t RAW=1183744;

struct Treatment { const char *name; int workers, job_size, overlap_log; };
static constexpr std::array<Treatment,3> treatments{{
	{"parallel_eye_l3",0,0,0},
	{"parallel_eye_worker2_job512k_nooverlap",2,512*1024,1},
	{"parallel_eye_worker2_auto",2,0,0}
}};
struct ZCtx {
	ZSTD_CCtx *p=nullptr;
	~ZCtx(){ZSTD_freeCCtx(p);}
	ZCtx()=default; ZCtx(const ZCtx&)=delete; ZCtx& operator=(const ZCtx&)=delete;
};
struct EyeState { ZCtx ctx; Bytes zstd, lz4; };
struct Packed { Packet packet; double zstd_ms=0, lz4_ms=0; size_t zstd_bytes=0; bool zstd_selected=false; };
struct Frame { double wall_ms=0,cpu_ms=0,decode_cpu_ms=0,zstd_sum_ms=0,lz4_sum_ms=0; size_t packet_bytes=0; std::array<size_t,2> zstd_bytes{},packet_eye_bytes{}; };

static double cpu_ms() {
	rusage r{}; getrusage(RUSAGE_SELF,&r);
	return 1000.0*(r.ru_utime.tv_sec+r.ru_stime.tv_sec)+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)*1e-3;
}
static void check(size_t n,const char *what) {
	if(ZSTD_isError(n)) throw std::runtime_error(std::string(what)+": "+ZSTD_getErrorName(n));
}
static Bytes read_bytes(const char *path) {
	std::ifstream f(path,std::ios::binary);
	if(!f) throw std::runtime_error(std::string("cannot open ")+path);
	return Bytes(std::istreambuf_iterator<char>(f),{});
}
static void configure(EyeState &e,const Treatment &t) {
	e.ctx.p=ZSTD_createCCtx();
	if(!e.ctx.p) throw std::runtime_error("ZSTD_createCCtx failed");
	check(ZSTD_CCtx_setParameter(e.ctx.p,ZSTD_c_compressionLevel,3),"level");
	check(ZSTD_CCtx_setParameter(e.ctx.p,ZSTD_c_nbWorkers,t.workers),"workers");
	check(ZSTD_CCtx_setParameter(e.ctx.p,ZSTD_c_jobSize,t.job_size),"jobSize");
	check(ZSTD_CCtx_setParameter(e.ctx.p,ZSTD_c_overlapLog,t.overlap_log),"overlapLog");
}
static size_t zstd_compress(EyeState &e,const Bytes &raw,size_t cap,int workers) {
	e.zstd.resize(cap);
	return workers ? ZSTD_compress2(e.ctx.p,e.zstd.data(),e.zstd.size(),raw.data(),raw.size())
	               : ZSTD_compressCCtx(e.ctx.p,e.zstd.data(),e.zstd.size(),raw.data(),raw.size(),3);
}
static Packed encode_eye(EyeState &e,const Treatment &t,const Bytes &raw,uint32_t width,uint32_t height) {
	const size_t raw_size=raw.size(), bound=ZSTD_compressBound(raw_size);
	const auto z0=Clock::now();
	const size_t zn=zstd_compress(e,raw,bound,t.workers);
	const double zms=std::chrono::duration<double,std::milli>(Clock::now()-z0).count();
	const bool valid_zstd=!ZSTD_isError(zn)&&zn>0;
	size_t fallback_size=raw_size;
	const uint8_t *fallback=raw.data();
	double lzms=0;
	if(!valid_zstd||zn>raw_size/2) {
		const auto l0=Clock::now();
		e.lz4.resize(size_t(LZ4_compressBound(int(raw_size))));
		const int ln=LZ4_compress_default(reinterpret_cast<const char *>(raw.data()),
			reinterpret_cast<char *>(e.lz4.data()),int(raw_size),int(e.lz4.size()));
		lzms=std::chrono::duration<double,std::milli>(Clock::now()-l0).count();
		if(ln>0&&size_t(ln)<raw_size){fallback_size=size_t(ln);fallback=e.lz4.data();}
	}
	const bool use_zstd=valid_zstd&&uint64_t(zn)*100<=uint64_t(fallback_size)*90;
	const size_t payload_size=use_zstd?zn:fallback_size;
	const auto encoding=use_zstd?wivrn::nxastc_packet::compression::zstd:
		(fallback==e.lz4.data()?wivrn::nxastc_packet::compression::lz4:wivrn::nxastc_packet::compression::none);
	const auto header=wivrn::nxastc_packet::make_header(width,height,uint32_t(payload_size),encoding);
	auto packet=std::make_shared<Bytes>(header.size()+payload_size);
	std::memcpy(packet->data(),header.data(),header.size());
	const uint8_t *payload=use_zstd?e.zstd.data():fallback;
	std::memcpy(packet->data()+header.size(),payload,payload_size);
	return {std::move(packet),zms,lzms,valid_zstd?zn:0,use_zstd};
}
static void decode_exact(const Packet &packet,const Bytes &raw,bool require_zstd=true) {
	auto h=wivrn::nxastc_packet::parse_packet(*packet);
	if(!h||h->header_bytes!=24||(require_zstd&&h->encoding!=wivrn::nxastc_packet::compression::zstd))
		throw std::runtime_error("production header parse/encoding mismatch");
	Bytes decoded(raw.size());
	const auto payload=std::span<const uint8_t>(*packet).subspan(h->header_bytes);
	if(wivrn::nxastc_packet::decode_payload(*h,payload,decoded)!=wivrn::nxastc_packet::decode_status::ok||decoded!=raw)
		throw std::runtime_error("production decoder did not reproduce exact ASTC bytes");
}
static Frame frame(EyeState (&eyes)[3][2],size_t ti,const std::array<Bytes,2>&raw,
			   std::array<Packet,2>*keep=nullptr) {
	const auto &t=treatments[ti];
	std::array<Packed,2> packed;
	const double c0=cpu_ms(); const auto start=Clock::now();
	auto right=std::async(std::launch::async,[&]{return encode_eye(eyes[ti][1],t,raw[1],W,H);});
	packed[0]=encode_eye(eyes[ti][0],t,raw[0],W,H);
	packed[1]=right.get();
	const auto end=Clock::now(); const double c1=cpu_ms();
	Frame out; out.wall_ms=std::chrono::duration<double,std::milli>(end-start).count(); out.cpu_ms=c1-c0;
	for(size_t i=0;i<2;++i) {
		if(!packed[i].zstd_selected) throw std::runtime_error("q6 production Zstd admission gate did not select Zstd");
		out.zstd_sum_ms+=packed[i].zstd_ms; out.lz4_sum_ms+=packed[i].lz4_ms;
		out.zstd_bytes[i]=packed[i].zstd_bytes; out.packet_eye_bytes[i]=packed[i].packet->size();
		out.packet_bytes+=packed[i].packet->size();
		const double d0=cpu_ms(); decode_exact(packed[i].packet,raw[i]); out.decode_cpu_ms+=cpu_ms()-d0;
		if(keep) (*keep)[i]=std::move(packed[i].packet);
	}
	return out;
}
static void save_packet(const Packet &p,const std::string &name) {
	std::ofstream f(name,std::ios::binary);
	if(!f||!f.write(reinterpret_cast<const char *>(p->data()),std::streamsize(p->size())))
		throw std::runtime_error("cannot save packet "+name);
}
static void boundary_roundtrip() {
	// Synthetic 256 KiB ASTC-like block buffer: unit framing/fallback check only.
	Bytes raw(256*1024);
	uint32_t x=0x9e3779b9u;
	for(auto &b:raw){x^=x<<13;x^=x>>17;x^=x<<5;b=uint8_t(x);}
	EyeState eye;
	const Treatment t{"boundary_worker2_job512k_nooverlap",2,512*1024,1};
	configure(eye,t);
	Packed p=encode_eye(eye,t,raw,1024,1024);
	decode_exact(p.packet,raw,false);
	auto h=wivrn::nxastc_packet::parse_packet(*p.packet);
	std::cerr<<"boundary_256k,worker_request=2,job_size=524288,overlap_log=1,encoding="<<int(h->encoding)<<",packet_bytes="<<p.packet->size()<<",decode=exact\n";
}
int main(int argc,char **argv) try {
	if(argc!=4) throw std::runtime_error("usage: production_like EYE0.astc EYE1.astc OUTPUT_DIR");
	const std::string dir=argv[3];
	std::array<Bytes,2> raw{read_bytes(argv[1]),read_bytes(argv[2])};
	for(const auto &r:raw) if(r.size()!=RAW) throw std::runtime_error("unexpected retained ASTC fixture size");
	EyeState eyes[3][2];
	const auto setup0=Clock::now();
	for(size_t t=0;t<treatments.size();++t)for(auto &e:eyes[t])configure(e,treatments[t]);
	std::cerr<<"context_setup_ms="<<std::chrono::duration<double,std::milli>(Clock::now()-setup0).count()<<'\n';
	std::cerr<<"mode,nbWorkers,jobSize,overlapLog\n";
	for(size_t i=0;i<treatments.size();++i) {
		int w=-1,j=-1,o=-1;
		check(ZSTD_CCtx_getParameter(eyes[i][0].ctx.p,ZSTD_c_nbWorkers,&w),"get workers");
		check(ZSTD_CCtx_getParameter(eyes[i][0].ctx.p,ZSTD_c_jobSize,&j),"get jobSize");
		check(ZSTD_CCtx_getParameter(eyes[i][0].ctx.p,ZSTD_c_overlapLog,&o),"get overlapLog");
		std::cerr<<treatments[i].name<<','<<w<<','<<j<<','<<o<<'\n';
	}
	std::cout<<"kind,mode,trial,wall_ms,process_cpu_ms,zstd_sum_ms,lz4_sum_ms,decode_cpu_ms,packet_bytes,eye0_zstd_bytes,eye1_zstd_bytes,eye0_packet_bytes,eye1_packet_bytes\n";
	std::array<Packet,2> last[3];
	// Separate first frame exposes worker-pool startup and first output-buffer allocation.
	for(size_t t=0;t<treatments.size();++t) {
		Frame f=frame(eyes,t,raw,&last[t]);
		std::cout<<"first,"<<treatments[t].name<<",0,"<<f.wall_ms<<','<<f.cpu_ms<<','<<f.zstd_sum_ms<<','<<f.lz4_sum_ms<<','<<f.decode_cpu_ms<<','<<f.packet_bytes<<','<<f.zstd_bytes[0]<<','<<f.zstd_bytes[1]<<','<<f.packet_eye_bytes[0]<<','<<f.packet_eye_bytes[1]<<'\n';
	}
	for(size_t t=0;t<treatments.size();++t) for(int i=0;i<5;++i) (void)frame(eyes,t,raw,&last[t]);
	constexpr int N=30;
	for(int trial=0;trial<N;++trial) for(int k=0;k<3;++k) {
		const size_t t=(trial%2==0)?size_t(k):size_t(2-k);
		Frame f=frame(eyes,t,raw,&last[t]);
		std::cout<<"steady,"<<treatments[t].name<<','<<trial<<','<<f.wall_ms<<','<<f.cpu_ms<<','<<f.zstd_sum_ms<<','<<f.lz4_sum_ms<<','<<f.decode_cpu_ms<<','<<f.packet_bytes<<','<<f.zstd_bytes[0]<<','<<f.zstd_bytes[1]<<','<<f.packet_eye_bytes[0]<<','<<f.packet_eye_bytes[1]<<'\n';
	}
	for(size_t t=0;t<treatments.size();++t)for(size_t eye=0;eye<2;++eye)
		save_packet(last[t][eye],dir+"/packet-"+std::to_string(t)+"-"+std::to_string(eye)+".bin");
	boundary_roundtrip();
} catch(const std::exception &e) { std::cerr<<"error: "<<e.what()<<'\n';return 1; }
