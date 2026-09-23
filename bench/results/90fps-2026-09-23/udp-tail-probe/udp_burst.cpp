// Local-only UDP burst probe. Build: c++ -O2 -std=c++17 udp_burst.cpp -o udp_burst
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

using clock_m = std::chrono::steady_clock;
static constexpr uint32_t magic = 0x42525553; // SUBR
static constexpr size_t datagram_bytes = 1200, header_bytes = 16;
static uint64_t ns(clock_m::time_point t) { return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count(); }
static void die(const char *s) { std::perror(s); std::exit(2); }
static uint32_t rd(const uint8_t *p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
static void wr(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=uint8_t(v>>(8*i));}

static int receiver(uint16_t port, double seconds) {
 if (!port || !(seconds > 0.0) || seconds > 15.0) { fprintf(stderr,"bad receiver arguments\n"); return 2; }
 int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0)die("socket"); int one=1; if(setsockopt(fd,SOL_SOCKET,SO_TIMESTAMPNS,&one,sizeof(one))<0)die("SO_TIMESTAMPNS");
 sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(port);if(bind(fd,(sockaddr*)&a,sizeof(a))<0)die("bind");
 fcntl(fd,F_SETFL,O_NONBLOCK); struct Rec{uint32_t n=0,got=0;uint64_t uf=0,ul=0,kf=0,kl=0;std::vector<uint8_t> seen;}; std::vector<Rec> rec(10000);
 auto until=clock_m::now()+std::chrono::duration<double>(seconds); uint8_t b[2048]; pollfd pfd{fd,POLLIN,0};
 while(clock_m::now()<until){ auto left=std::chrono::duration_cast<std::chrono::milliseconds>(until-clock_m::now()).count(); int pr=poll(&pfd,1,int(std::clamp<int64_t>(left,1,1000))); if(pr<0){if(errno==EINTR)continue;die("poll");} if(!pr)continue; char control[256]; iovec io{b,sizeof(b)}; msghdr m{};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=control;m.msg_controllen=sizeof(control); sockaddr_in from{};m.msg_name=&from;m.msg_namelen=sizeof(from);
  ssize_t n=recvmsg(fd,&m,0); if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)continue;if(errno==EINTR)continue;die("recvmsg");} if(size_t(n)<header_bytes||size_t(n)>datagram_bytes)continue;
  if(rd(b)!=magic)continue;uint32_t f=rd(b+4),q=rd(b+8),count=rd(b+12);if(count==0||count>10000||q>=count||f>=rec.size())continue; auto now=clock_m::now();uint64_t kt=0;for(cmsghdr*c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c))if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_TIMESTAMPNS&&c->cmsg_len>=CMSG_LEN(sizeof(timespec))){timespec*t=(timespec*)CMSG_DATA(c);kt=uint64_t(t->tv_sec)*1000000000ull+t->tv_nsec;}
  auto &r=rec[f];if(r.n!=count){r.n=count;r.seen.assign(count,0);r.got=0;r.uf=r.ul=r.kf=r.kl=0;}if(!r.seen[q]){r.seen[q]=1;r.got++;}uint64_t u=ns(now);if(!r.uf)r.uf=u;r.ul=u;if(kt){if(!r.kf||kt<r.kf)r.kf=kt;if(kt>r.kl)r.kl=kt;}
 }
 printf("receiver port=%u\n",port);for(uint32_t f=0;f<rec.size();f++)if(rec[f].n){auto&r=rec[f];printf("frame=%u got=%u/%u loss=%u user_span_ns=%llu kernel_span_ns=%llu\n",f,r.got,r.n,r.n-r.got,(unsigned long long)(r.ul-r.uf),(unsigned long long)(r.kl-r.kf));}close(fd);return 0;
}
static unsigned env_u(const char *name,unsigned fallback,unsigned max){const char *s=std::getenv(name);if(!s)return fallback;char *e=nullptr;unsigned v=std::strtoul(s,&e,10);return e&&*e==0&&v<=max?v:fallback;}
static int sender(const char *ip,uint16_t port,uint32_t frames,uint32_t count,double fps,int tos,unsigned tail_us,unsigned pace_us){if(!port||!frames||!count||count>10000||!(fps>0.0)||fps>1000.0||tos<0||tos>255||tail_us>10000||pace_us>10000){fprintf(stderr,"bad sender arguments\n");return 2;}int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)die("socket");if(setsockopt(fd,IPPROTO_IP,IP_TOS,&tos,sizeof(tos))<0)die("IP_TOS");sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);if(inet_pton(AF_INET,ip,&a.sin_addr)!=1){fprintf(stderr,"IPv4 only\n");return 2;}std::vector<uint8_t>b(datagram_bytes);wr(b.data(),magic);const unsigned tail_count=env_u("NX_UDP_TAIL_COUNT",tail_us?3:0,1000),tail_bytes=env_u("NX_UDP_TAIL_BYTES",16,datagram_bytes);if(tail_count&&tail_bytes<header_bytes){fprintf(stderr,"tail bytes must be >=16\n");return 2;}auto next=clock_m::now();uint64_t total=0;for(uint32_t f=0;f<frames;f++){wr(b.data()+4,f);wr(b.data()+12,count);auto first=clock_m::now();auto packet_next=first;for(uint32_t q=0;q<count;q++){if(pace_us&&q)std::this_thread::sleep_until(packet_next);wr(b.data()+8,q);ssize_t n=sendto(fd,b.data(),b.size(),0,(sockaddr*)&a,sizeof(a));if(n!=ssize_t(b.size()))die("sendto");total+=n;packet_next+=std::chrono::microseconds(pace_us);}auto last=clock_m::now();printf("frame=%u sent=%u send_span_ns=%llu\n",f,count,(unsigned long long)ns(last)-ns(first));if(tail_count){std::this_thread::sleep_for(std::chrono::microseconds(tail_us));std::vector<uint8_t>tail(tail_bytes);wr(tail.data(),0x5441494c);for(unsigned i=0;i<tail_count;i++)if(sendto(fd,tail.data(),tail.size(),0,(sockaddr*)&a,sizeof(a))!=ssize_t(tail.size()))die("sendto tail");}next+=std::chrono::duration_cast<clock_m::duration>(std::chrono::duration<double>(1.0/fps));std::this_thread::sleep_until(next);}fprintf(stderr,"sender frames=%u datagrams=%llu bytes=%llu tos=%d tail_us=%u tail_count=%u tail_bytes=%u pace_us=%u\n",frames,(unsigned long long)frames*count,(unsigned long long)total,tos,tail_us,tail_count,tail_bytes,pace_us);close(fd);return 0;}
int main(int argc,char**argv){if(argc<3){fprintf(stderr,"usage: %s recv PORT [SECONDS<=15]\n       %s send IP PORT [FRAMES COUNT FPS [TOS [TAIL_US [PACE_US]]]]\n",argv[0],argv[0]);return 2;}try{std::string mode=argv[1];if(mode=="recv"&&argc<=4)return receiver(uint16_t(std::stoul(argv[2])),argc>3?std::stod(argv[3]):15.0);if(mode=="send"&&argc>=4)return sender(argv[2],uint16_t(std::stoul(argv[3])),argc>4?uint32_t(std::stoul(argv[4])):180,argc>5?uint32_t(std::stoul(argv[5])):64,argc>6?std::stod(argv[6]):90.0,argc>7?int(std::stoul(argv[7])):0,argc>8?unsigned(std::stoul(argv[8])):0,argc>9?unsigned(std::stoul(argv[9])):0);}catch(...){fprintf(stderr,"bad numeric argument\n");}return 2;}
