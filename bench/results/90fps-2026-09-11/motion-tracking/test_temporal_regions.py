import numpy as np
from temporal_regions import TemporalRegionTracker, segment_regions

def frame(rects):
    a=np.zeros((128,128,3),np.uint8)
    for x,y,w,h,c in rects: a[y:y+h,x:x+w]=c
    return a

def main():
    # Closed hue bands remain disjoint, and circular hue averaging handles 179/0.
    hue=np.zeros((128,128,3),np.uint8); hue[:,:]=[255,255,255]
    hsv=np.zeros((128,128,3),np.uint8); hsv[:,:]=[0,220,180]; hsv[:,64:]=[179,220,180]
    rgb=np.asarray(__import__('cv2').cvtColor(hsv,__import__('cv2').COLOR_HSV2RGB))
    rs=segment_regions(rgb,min_area=10); assert rs and sum(x['mask'].sum() for x in rs)==np.count_nonzero(np.logical_or.reduce([x['mask'] for x in rs]))
    assert min(abs(rs[0]['hue']),abs(rs[0]['hue']-180)) < 5 or min(abs(rs[-1]['hue']),abs(rs[-1]['hue']-180)) < 5
    shaded=np.zeros((128,128,3),np.uint8); shaded[:]=[8,8,8]; shaded[35:90,20:45]=[35,190,55]; shaded[35:90,45:70]=[45,170,50]
    merged=segment_regions(shaded,hue_bins=8,min_area=20,merge_adjacent=True)
    assert len(merged)==1
    separated=shaded.copy(); separated[35:90,85:110]=[35,190,55]
    split=segment_regions(separated,hue_bins=8,min_area=20,merge_adjacent=True)
    assert len(split)>=2 and sum(x['mask'].sum() for x in split)==np.count_nonzero(np.logical_or.reduce([x['mask'] for x in split]))
    huge=np.zeros((128,128,3),np.uint8); huge[:]=[8,8,8]; huge[5:123,10:64]=[20,20,40]; huge[5:123,64:118]=[20,40,60]
    assert len(segment_regions(huge,hue_bins=24,min_area=20,merge_adjacent=False))>=2
    assert len(segment_regions(huge,hue_bins=24,min_area=20,merge_adjacent=True))>=2
    t=TemporalRegionTracker(max_jump=35)
    a=t.update(frame([(20,40,24,30,(30,210,50))])); b=t.update(frame([(25,40,24,30,(30,210,50))])); c0=t.update(frame([(30,40,24,30,(30,210,50))]))
    assert len(a)==1 and len(b)==1 and a[0]['id']==b[0]['id'] and b[0]['confidence']==0
    assert c0[0]['id']==a[0]['id'] and c0[0]['matches']>=2 and c0[0]['velocity'][0]>1
    # Same-hue distractor is farther than the conservative gate and gets a new ID.
    c=t.update(frame([(35,40,24,30,(30,210,50)),(100,15,20,20,(32,205,48))]))
    assert len(c)==2 and len({x['id'] for x in c})==2
    # Disappearance removes the region; it is not extrapolated into pixels.
    d=t.update(frame([])); assert d==[]
    print('PASS: translation, same-hue distractor, disappearance')

if __name__=='__main__': main()
