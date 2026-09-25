#include "region_helpers.cpp"

#include <fstream>
#include <numeric>

static double pctl(std::vector<double> v, double p)
{
	std::sort(v.begin(),v.end());
	return v[std::max<size_t>(1,size_t(std::ceil(v.size()*p)))-1];
}

int main(int argc,char **argv)
{
	const std::string path=argc>1?argv[1]:"restore.csv";
	auto l=regions_layout();
	l.width=2176; l.height=2176;
	auto old=make_aligned_regions_base(l);
	motion_native_info oi; assert(build_motion_native(l,old,oi));
	motion_region_vectors truth{};
	truth[0]={8,0,0,0}; truth[1]={-8,0,0,0}; truth[2]={0,8,0,0}; truth[3]={0,-8,0,0};
	auto now=make_regions_frame(l,old,oi,truth);
	motion_native_info ni; assert(build_motion_native(l,now,ni));
	auto global=estimate_motion(l,old,oi,now,ni);
	auto regions=estimate_motion_regions(l,old,oi,now,ni);
	auto global_body=motion_residual(l,old,oi,now,ni,global.dx,global.dy);
	auto region_body=motion_regions_residual(l,old,oi,now,ni,regions);
	auto global_wire=make_motion_wire(global.dx,global.dy,9,global_body);
	auto region_wire=make_motion_regions_wire(regions,9,region_body);
	assert(global_body.size()==region_body.size() && now.size()==561808);
	std::ofstream csv(path);
	csv<<"mode,phase,sample,us,raw_bytes,exact\n";
	std::array<std::vector<double>,2> samples;
	uint32_t rng=0x4d595df4u;
	auto next=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};
	// 12 ABBA/BAAB blocks warm both paths 24 times, then 12 measured blocks.
	for(int block=0;block<24;++block)
	{
		const bool reverse=next()&1u;
		const std::array<int,4> abba=reverse?std::array<int,4>{1,0,0,1}:std::array<int,4>{0,1,1,0};
		for(int order:abba)
		{
			auto wire=order?region_wire:global_wire;
			auto body=order?region_body:global_body;
			std::vector<uint8_t> decoded=body; // mirror decompressor output allocation outside timer
			const auto begin=std::chrono::steady_clock::now();
			bool ok=false;
			if(order)
			{
				auto h=parse_motion_regions_wire(wire);
				ok=h&&h->reference==9&&restore_motion_regions(l,old,oi,decoded,h->vectors);
			}
			else
			{
				auto h=parse_motion_wire(wire);
				ok=h&&h->reference==9&&restore_motion(l,old,oi,decoded,h->dx,h->dy);
			}
			const auto end=std::chrono::steady_clock::now();
			const double us=std::chrono::duration<double,std::micro>(end-begin).count();
			const bool exact=ok&&decoded==now; assert(exact);
			const char *phase=block<12?"warm":"measured";
			const int sample=block<12?block*2+(order==(reverse?1:0)?0:1):(block-12)*2+(order==(reverse?1:0)?0:1);
			csv<<(order?"four_regions":"global")<<','<<phase<<','<<sample<<','<<us<<','<<now.size()<<','<<exact<<'\n';
			if(block>=12)samples[order].push_back(us);
		}
	}
	for(int i=0;i<2;++i)
		std::fprintf(stderr,"%s decode parse+restore p50 %.2f us p95 %.2f us n=%zu raw=%zu\n",i?"four_regions":"global",pctl(samples[i],.5),pctl(samples[i],.95),samples[i].size(),now.size());
}
