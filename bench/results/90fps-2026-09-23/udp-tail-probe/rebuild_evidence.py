#!/usr/bin/env python3
import json, re, statistics, shutil, math
from pathlib import Path
out=Path(__file__).parent
root=out  # bundled numeric evidence; no private fixtures required
recv_files={
 'confirm0_count3_delay0':'udp-confirm0-count3-delay0-recv.txt',
 'confirm1_count48_delay1':'udp-confirm1-count48-delay1-recv.txt',
 'confirm2_count64_delay1':'udp-confirm2-count64-delay1-recv.txt',
 'confirm3_count64_delay500':'udp-confirm3-count64-delay500-recv.txt',
 'confirm4_count3_delay0':'udp-confirm4-count3-delay0-recv.txt',
 'confirm5_count64_delay1':'udp-confirm5-count64-delay1-recv.txt',
 'tailbytes16_count64_delay500':'udp-tailbytes16-count64-delay500-recv.txt',
 'tailbytes16_count64_delay1000':'udp-tailbytes16-count64-delay1000-recv.txt',
 'qos136_tail0':'udp-qos136-tail0-recv.txt',
 'qos184_tail0':'udp-qos184-tail0-recv.txt',
 'pace0_tail32':'udp-pace0-tail32-recv.txt',
}
send_files={k:v.replace('-recv.txt','-send.txt') for k,v in recv_files.items()}
def percentile(values, fraction):
 return sorted(values)[max(0, math.ceil(fraction*len(values))-1)]

def sender_info(k):
 text=(out/(k+'-send.txt')).read_text()
 footer=re.search(r'sender frames=(\d+) datagrams=(\d+) bytes=(\d+)[^\n]*\n?',text)
 assert footer, k
 frames,datagrams,byte_count=map(int,footer.groups())
 assert frames and datagrams%frames==0
 # stdout buffering can interleave the stderr footer inside one timing line.
 text=text[:footer.start()]+text[footer.end():]
 rows=[tuple(map(int,m)) for m in re.findall(r'frame=(\d+) sent=(\d+) send_span_ns=(\d+)',text)]
 assert len(rows)==frames and len({r[0] for r in rows})==frames, (k,frames,len(rows))
 assert sum(r[1] for r in rows)==datagrams
 return frames,datagrams,rows

def sender_rows(k):
 return sender_info(k)[2]

def recv(k,fn):
 sent={frame:count for frame,count,span in sender_rows(k)}
 received={}
 rx=re.compile(r'^frame=(\d+) got=(\d+)/(\d+) loss=(\d+) .*kernel_span_ns=(\d+)')
 for line in (out/(k+'-recv.txt')).read_text().splitlines():
  m=rx.match(line)
  if m:
   frame,got,count,lost,span=map(int,m.groups())
   assert frame in sent and count==sent[frame] and got<=count and lost==count-got
   assert frame not in received
   received[frame]=(got,span)
 expected=sum(sent.values());got=sum(x[0] for x in received.values())
 vals=[span/1e6 for frame,(count,span) in received.items() if count==sent[frame]]
 assert vals
 return {'source_file':fn,'frames_sent':len(sent),'frames_observed':len(received),
         'complete_frames':len(vals),'missing_frames':len(sent.keys()-received.keys()),
         'expected_datagrams':expected,'received_unique_datagrams':got,'lost_datagrams':expected-got,
         'loss_pct':100*(expected-got)/expected,
         'kernel_first_last_ms_median':statistics.median(vals),
         'kernel_first_last_ms_p99':percentile(vals,.99),
         'kernel_first_last_ms_min':min(vals),'kernel_first_last_ms_max':max(vals)}

def send(k,fn):
 rows=sender_rows(k);counts={count for _,count,_ in rows};vals=[span for _,_,span in rows]
 return {'source_file':fn,'frames_observed':len(vals),'datagrams_per_frame':min(counts) if len(counts)==1 else sorted(counts),
         'send_span_ns_median':statistics.median(vals),'send_span_ns_p99':percentile(vals,.99),
         'main_payload_mbps_at_90hz':min(counts)*1200*8*90/1e6}
summary={k:recv(k,v) for k,v in recv_files.items()}
summary['_method']={'frame_rate_hz':90,'kernel_span':'SO_TIMESTAMPNS earliest-to-latest per-frame burst interval; not one-way/display/photon latency','loss':'expected datagrams from sender records; includes wholly missing frames; span percentiles use complete frames only; nearest-rank percentile','tail_payload_mbps_at_90hz':{'16B_x64':16*64*90*8/1e6,'1200B_x32':1200*32*90*8/1e6}}
summary['sender']={k:send(k,v) for k,v in send_files.items()}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
