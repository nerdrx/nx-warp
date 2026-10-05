#!/usr/bin/env python3
"""Compare executed projection bodies with the supplied production checkout."""
import hashlib,json,sys
from pathlib import Path
repo=Path(sys.argv[1]);out=Path(sys.argv[2])
cpp=(out/'kernel-gate.cpp').read_text()
def body(text,signature):
 start=text.index(signature);begin=text.index('{',start);depth=0
 for end in range(begin,len(text)):
  if text[end]=='{':depth+=1
  elif text[end]=='}':
   depth-=1
   if depth==0:return text[begin:end+1]
 raise AssertionError(signature)
src=(repo/'client/decoder/shard_accumulator.cpp').read_text()
for signature in ['void shard_accumulator::push_shard(video_stream_data_shard && shard)','std::optional<XrTime> shard_accumulator::next_nack_deadline(XrTime now)','std::optional<XrTime> shard_accumulator::next_poll_deadline(XrTime now)','void shard_accumulator::poll_nacks(XrTime now)','void shard_accumulator::try_nack(XrTime now)','void shard_accumulator::pump(XrTime now)','shard_accumulator::window_t::step shard_accumulator::try_submit_front(shard_set & current)']:
 assert body(src,signature)==body(cpp,signature),signature
src=(repo/'client/scenes/stream_network.cpp').read_text()
assert body(src,'void scenes::stream::process_packets()')==body(cpp,'void caller_projection::scenes::stream::process_packets()')
sig='int poll(T && visitor, std::chrono::milliseconds max_timeout, TimeoutSupplier && timeout_supplier)'
assert body((repo/'client/wivrn_client.h').read_text(),sig)==body(cpp,sig)
meta=json.loads((out/'provenance.json').read_text())
assert meta['generated_cpp_sha256']==hashlib.sha256(cpp.encode()).hexdigest()
print('9 executed production bodies match exactly; generated CPP hash verified.')
