#!/usr/bin/env python3
for step in (1,4):
 for lanes in (256,64):
  n=(64//step)**2; seen=[]
  for tid in range(lanes): seen += list(range(tid,n,lanes))
  assert sorted(seen)==list(range(n)) and len(seen)==len(set(seen))
print('compact flat 64/256 lane coverage: PASS')
