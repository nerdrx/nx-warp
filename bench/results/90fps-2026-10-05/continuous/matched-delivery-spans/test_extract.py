import csv,io
from extract_spans import extract,union

def row(event,frame,stream,*fields):return [event,str(frame),"0",str(stream),*map(str,fields)]
def run(rows):return list(extract(rows))[0]
a=[row("frame_bytes",1,0,200000),row("frame_bytes",1,1,200000),row("feedback_spans",1,0,1000000000,1006000000,9000000000,9006000000,1),row("feedback_spans",1,1,1006000000,1012000000,9000000000,9006000000,1)]
r=run(a);assert r['matched_bytes']==400000 and r['matched_streams']==2
assert r['receive_union_ns']==6000000 and r['send_union_ns']==12000000
assert abs(r['max_span_rate_bps']-266666666.6667)<1
assert run(a+a[2:])==r # duplicate feedback, not duplicate sent bytes
assert run(a[:2]+a[2:][::-1])==r
b=[x.copy() for x in a];b[2][4:6]=['0','1006000000'];r=run(b);assert not r['all_send_valid'] and r['max_span_rate_bps']==r['receive_rate_bps']
b=[x.copy() for x in a];b[2][-1]='0';assert run(b)['lost'] and run(b)['max_span_rate_bps']==0
b=[x.copy() for x in a];b[2][4:6]=['1000000000','0'];c=b[2].copy();c[4:6]=['0','1006000000'];assert not run(b+[c])['all_send_valid']
assert union([(1,4),(3,6),(10,13)])==8
assert union([(1,4),(4,7)])==6
assert union([])==0
assert list(extract([['ignored','header']]))==[]
for bad in ([row('frame_bytes',1,0,-1)],[row('feedback_spans',1,0,1)]):
 try: list(extract(bad));raise AssertionError('malformed event accepted')
 except ValueError:pass
b=a+[row('frame_bytes',1,2,100)];assert run(b)['unmatched_byte_streams']==1 and run(b)['matched_bytes']==400000
b=a+[row('frame_bytes',1,3,9999)];assert run(b)==run(a)
with open('synthetic-capture.csv','w',newline='') as f:csv.writer(f,lineterminator='\n').writerows(a)
print('Extractor checks passed; synthetic capture is explicitly synthetic.')
