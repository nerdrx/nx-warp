#!/usr/bin/env python3

def layout(w):
    cols=w//64; cc=max(2,(cols//4)&~1); c0=(cols-cc)//2
    centre=cc*64; packed=centre+(w-centre)//4
    out=[]
    for t in range(cols):
        if c0<=t<c0+cc: step,start=1,c0*16+(t-c0)*64
        elif t<c0: step,start=4,t*16
        else: step,start=4,c0*16+centre+(t-c0-cc)*16
        out += [start+i for i in range(64//step)]
    return cols,c0,cc,packed,out

def old2176(t):
    if 13<=t<21: return 1,208+(t-13)*64
    if t<13: return 4,t*16
    return 4,720+(t-21)*16

_,_,_,_,new=layout(2176)
old=[]
for t in range(34):
    s,st=old2176(t); old += [st+i for i in range(64//s)]
assert new==old, (new[:20],old[:20])
for w in (2176,2688):
    cols,c0,cc,packed,vals=layout(w)
    assert len(vals)==packed and sorted(vals)==list(range(packed)), (w,cols,c0,cc,packed,len(vals),len(set(vals)))
print('2176 exact old mapping: PASS')
print('2688 layout: PASS (42 tiles, centre 10 tiles/640px, packed 1152px)')
