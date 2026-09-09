"""CPU quality model, not a codec, GPU benchmark or latency measurement."""
from pathlib import Path
import json
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

OUT=Path(__file__).resolve().parent
H=W=512; N=48; T=32; BUDGET=60; MAX_AGE=8
Y,X=np.mgrid[:H,:W]
CENTRE=(Y>=192)&(Y<320)&(X>=192)&(X<320)
PERIPH=np.array([i for i in range(256) if not (6<=i//16<10 and 6<=i%16<10)])
STEPS=np.array([0]+[1 if f%2 else 3 for f in range(1,N)])

def shift(a,dx,fill=0):
    out=np.full_like(a,fill); valid=np.zeros(a.shape,bool)
    if dx>=0 and dx<W:
        out[...,dx:]=a[...,:W-dx]; valid[...,dx:]=True
    elif -W<dx<0:
        out[...,:W+dx]=a[...,-dx:]; valid[...,:W+dx]=True
    return out,valid

def source(f):
    images=[]; masks=[]; pan=int(STEPS[:f+1].sum())
    for eye in range(2):
        wx=X-pan+eye*2
        bg=.42+.12*np.sin(wx/13)+.10*np.sin(Y/19)+.08*np.sin((wx+Y)/7)
        bg+=.13*((wx%11)<2)-.09*((Y%17)<2)
        x0=55+5*f+eye*8; y0=280+int(22*np.sin(f/6))
        fg=(X>=x0)&(X<x0+104)&(Y>=y0)&(Y<y0+92)
        qx=375-3*f+eye*5; qy=90+int(16*np.cos(f/5))
        q=(X>=qx)&(X<qx+45)&(Y>=qy)&(Y<qy+58)
        img=np.where(fg,.82+.12*((X-x0)%7<2),bg)
        img=np.where(q,.15+.2*((Y-qy)%9<3),img)
        images.append(np.rint(np.clip(img,0,1)*255).astype(np.float32)/255)
        masks.append(fg|q)
    return np.array(images),np.array(masks)

def guide(a):
    return a.reshape(2,H//2,2,W//2,2).mean((2,4))

def up(a):
    return a.repeat(2,axis=1).repeat(2,axis=2)

def tile_mean(a):
    return a.reshape(2,16,T,16,T).mean((2,4)).max(axis=0)

def select(cg,warped,valid,age,detail,f):
    # This function cannot inspect fresh high-resolution source pixels.
    err=np.abs(cg-guide(warped))
    fallback=(up(err)>.035)|~valid|(age>MAX_AGE)
    score=tile_mean(up(err))+.4*tile_mean(~valid)+.08*tile_mean(~detail)+.012*tile_mean(age)
    # Reserve half the budget for a sweep; use the remainder for change/age.
    mandatory=PERIPH[np.arange(len(PERIPH))%8==f%8]
    rest=np.setdiff1d(PERIPH,mandatory)
    ranked=rest[np.argsort(-score.ravel()[rest],kind='stable')]
    chosen=np.r_[mandatory,ranked[:BUDGET-len(mandatory)]]
    grid=np.zeros((16,16),bool);grid.ravel()[chosen]=True
    mask=grid.repeat(T,0).repeat(T,1)
    assert len(chosen)==BUDGET and not mask[CENTRE].any()
    return mask,fallback

def estimate_motion(hist,cg,details=False):
    # Bounded horizontal search using only current guide and retained imagery.
    # This is not a general 6-DoF flow/depth estimator; CPU cost is not benchmarked.
    interior=np.array([i for i in PERIPH if 0<i//16<15 and 0<i%16<15])
    candidates=[]
    for dx in range(-4,5):
        candidate,_=shift(hist,dx)
        error=np.abs(cg-guide(candidate))
        per_tile=error.reshape(2,16,16,16,16).mean((2,4)).max(axis=0)
        candidates.append((float(np.median(per_tile.ravel()[interior])),abs(dx),dx))
    ranked=sorted(candidates)
    gap=ranked[1][0]-ranked[0][0]
    return (ranked[0][2],gap) if details else ranked[0][2]

def run(mode):
    hist,prev_fg=source(0)
    age=np.zeros_like(hist,dtype=np.int16);detail=np.ones_like(hist,bool)
    records=[]; last={}
    for f in range(1,N):
        fresh,fg=source(f);cg=guide(fresh);dx=0;ambiguous=False
        old_fg,old_valid=shift(prev_fg,int(STEPS[f]),False)
        exposed=old_fg & ~fg & old_valid & ~CENTRE[None]
        if mode=='half':
            output=up(cg);repair=np.zeros((H,W),bool);fallback=np.ones_like(hist,bool)
            age.fill(0);detail.fill(False)
        elif mode=='held':
            output=hist.copy();age+=1;repair=np.zeros((H,W),bool);fallback=np.zeros_like(hist,bool)
        else:
            dx=int(STEPS[f]) if mode=='guided' else 0
            if mode=='estimated':
                dx,gap=estimate_motion(hist,cg,details=True)
                ambiguous=gap<.001
            warped,valid=shift(hist,dx)
            age,_=shift(age,dx);age+=1
            detail,_=shift(detail,dx,False)
            repair,fallback=select(cg,warped,valid,age,detail,f)
            if ambiguous:fallback[:]=True
            output=np.where(fallback,up(cg),warped)
            age[fallback]=0;detail[fallback]=False
            # Simulated fresh detail payload: only these addresses are read.
            output[:,repair]=fresh[:,repair]
            age[:,repair]=0;detail[:,repair]=True
        output[:,CENTRE]=fresh[:,CENTRE];age[:,CENTRE]=0;detail[:,CENTRE]=True
        assert np.array_equal(output[:,192:320,192:320],fresh[:,192:320,192:320])
        effective_fallback=fallback & ~repair[None] & ~CENTRE[None]
        error=np.abs(output-fresh)*255
        records.append({'frame':f,'mae':float(error.mean()),
            'periphery_mae':float(error[:,~CENTRE].mean()),
            'disocclusion_mae':float(error[exposed].mean()) if exposed.any() else 0.,
            'ambiguous_motion':ambiguous,'disocclusion_pixels':int(exposed.sum()),'motion_dx':dx,'true_dx':int(STEPS[f]),
            'repairs_per_eye':int(repair.sum()//(T*T)),
            'guide_fallback_fraction':float(effective_fallback.mean()),
            'max_retained_detail_age':int(age[detail].max()) if detail.any() else 0,
            'sample_fraction':float((H*W/4 if mode!='held' else 0)+CENTRE.sum()+repair.sum())/(H*W)})
        if mode in ('guided','estimated','no_motion'):
            assert records[-1]['max_retained_detail_age']<=MAX_AGE
        hist=output;prev_fg=fg
        if f==N-1:last={'source':fresh,'output':output.copy(),'repair':repair,'fallback':effective_fallback,'age':age.copy()}
    return records,last

def ambiguity_test():
    # A one-pixel checker shift is invisible to a 2x2 box guide.
    hist=np.stack([((X+Y)%2).astype(np.float32)]*2)
    fresh=1-hist;cg=guide(fresh)
    assert np.array_equal(cg,guide(hist))
    dx,gap=estimate_motion(hist,cg,details=True)
    warped,valid=shift(hist,dx)
    repair,fallback=select(cg,warped,valid,np.ones_like(hist,dtype=np.int16),np.ones_like(hist,bool),1)
    out=np.where(fallback,up(cg),warped);out[:,repair]=fresh[:,repair];out[:,CENTRE]=fresh[:,CENTRE]
    half=up(cg);half[:,CENTRE]=fresh[:,CENTRE]
    result={'true_dx':1,'estimated_dx':dx,'guides_identical':True,
      'periphery_mae_estimated':float((np.abs(out-fresh)*255)[:,~CENTRE].mean()),
      'periphery_mae_half':float((np.abs(half-fresh)*255)[:,~CENTRE].mean())}
    assert dx!=1 and gap<.001 and result['periphery_mae_estimated']>result['periphery_mae_half']
    guarded=half.copy();guarded[:,repair]=fresh[:,repair]
    result['confidence_gap']=gap
    result['periphery_mae_guarded']=float((np.abs(guarded-fresh)*255)[:,~CENTRE].mean())
    assert result['periphery_mae_guarded']<=result['periphery_mae_half']
    ty,tx=next((i//16,i%16) for i in PERIPH if i%16>0 and not repair[(i//16)*T,(i%16)*T])
    fig,axes=plt.subplots(1,3,figsize=(8,3))
    for ax,img,label in zip(axes,[fresh[0],out[0],half[0]],['Current detail','Wrong retained phase','Fresh half-res guide']):
        ax.imshow(img[ty*T:ty*T+16,tx*T:tx*T+16],cmap='gray',vmin=0,vmax=1,interpolation='nearest');ax.set_title(label,fontsize=10);ax.axis('off')
    fig.suptitle('Counterexample: the guide cannot see a one-pixel checker shift',fontsize=11)
    fig.tight_layout(rect=[0,.04,1,.9]);fig.savefig(OUT/'ambiguity.png',dpi=150);plt.close(fig)
    return result

def main():
    # Independent border/translation check with recognizable source values.
    a=np.broadcast_to(np.arange(W),(2,H,W)).copy()
    b,v=shift(a,3,-1)
    assert (b[...,:3]==-1).all() and (b[...,3:]==a[...,:-3]).all()
    assert not v[...,:3].any() and v[...,3:].all()
    traces={}; finals={}; summaries={}
    for mode in ['half','held','no_motion','guided','estimated']:
        traces[mode],finals[mode]=run(mode)
        rows=traces[mode]
        summaries[mode]={k:float(np.mean([r[k] for r in rows])) for k in
          ['mae','periphery_mae','guide_fallback_fraction','sample_fraction']}
        summaries[mode]['motion_matches_fraction']=float(np.mean([r['motion_dx']==r['true_dx'] for r in rows]))
        count=sum(r['disocclusion_pixels'] for r in rows)
        summaries[mode]['disocclusion_mae']=sum(r['disocclusion_mae']*r['disocclusion_pixels'] for r in rows)/count
        summaries[mode]['max_retained_detail_age']=max(r['max_retained_detail_age'] for r in rows)
        summaries[mode]['max_repairs_per_eye']=max(r['repairs_per_eye'] for r in rows)
    metrics={'width_per_eye':W,'height':H,'frames':N,'bootstrap':'one full-detail stereo frame; excluded from steady-state summaries',
      'repair_budget_per_eye':BUDGET,'peripheral_tiles_per_eye':len(PERIPH),'centre_size':128,
      'motion':'guided uses known renderer background translation; estimated searches horizontal -4..4 from guide/history only; true motion alternates +1/+3, foreground motion NOT supplied',
      'confidence_guard':'estimated mode uses fresh guide when best-vs-runner-up median error gap <0.001; heuristic, not proof of correct motion',
      'guide':'half width and half height, box-averaged grayscale; uncompressed float model',
      'limits':'CPU synthetic translation model. No codec/GPU/transport/latency/rotation/depth/loss validation. Sample fraction excludes metadata and computational costs; not bitrate or speedup.',
      'summary':summaries,'traces':traces,'ambiguity_counterexample':ambiguity_test()}
    (OUT/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')
    np.savez_compressed(OUT/'final_review.npz',source=finals['guided']['source'],
      **{k:v['output'] for k,v in finals.items()},repair=finals['guided']['repair'],fallback=finals['guided']['fallback'])
    fig,axes=plt.subplots(2,3,figsize=(12,8))
    panels=[(finals['guided']['source'][0],'Source (left eye)'),(finals['half']['output'][0],'Half-res guide + fresh centre'),
      (finals['held']['output'][0],'Held history + fresh centre'),(finals['no_motion']['output'][0],'Guide/repairs, motion disabled'),
      (finals['guided']['output'][0],'Warped history + guide/repairs'),(finals['estimated']['output'][0],'Guide-estimated motion + repairs')]
    for ax,(img,label) in zip(axes.flat,panels):
        ax.imshow(img,cmap='gray',vmin=0,vmax=1);ax.set_title(label,fontsize=10);ax.axis('off')
    fig.suptitle('Synthetic stereo motion model — not Pico output',fontsize=14)
    fig.tight_layout(rect=[0,.04,1,.95]);fig.text(.5,.015,'Final frame. Both native centres are exact; only selected peripheral repairs read fresh high-resolution source.',ha='center',fontsize=9)
    fig.savefig(OUT/'comparison.png',dpi=140);plt.close(fig)
    fig,axes=plt.subplots(1,2,figsize=(11,4))
    for mode,rows in traces.items():axes[0].plot([r['frame'] for r in rows],[r['periphery_mae'] for r in rows],label=mode)
    rows=traces['guided'];axes[1].plot([r['frame'] for r in rows],[r['guide_fallback_fraction']*100 for r in rows],label='Guide fallback %')
    axes[1].plot([r['frame'] for r in rows],[r['sample_fraction']*100 for r in rows],label='New sample budget %')
    axes[0].set(title='Peripheral error',ylabel='MAE (0–255)');axes[1].set(title='Guided variant',ylabel='Percent of stereo pixels')
    for ax in axes:ax.set_xlabel('Frame');ax.legend(fontsize=8);ax.grid(alpha=.2)
    fig.tight_layout(rect=[0,.06,1,1]);fig.text(.5,.01,'Sample budget is not bitrate or GPU cost. Horizontal motion only; newly exposed pixels use the guide or repairs.',ha='center',fontsize=8)
    fig.savefig(OUT/'trace.png',dpi=140);plt.close(fig)
    print(json.dumps(summaries,indent=2))
if __name__=='__main__':main()
