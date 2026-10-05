#!/usr/bin/env python3
"""Observed CPU lifecycle outcomes; no timing or GPU inference."""
from pathlib import Path
import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
rows=list(csv.DictReader((root/'raw/root/launch-failure-outcomes.csv').open()))
labels={'option_absent_fallback':'Option absent','option_off_fallback':'Option off','nonexact_flag_fallback':'Non-exact option','hardware_fallback':'Hardware backend','auxiliary_fallback':'Alpha auxiliary','missing_eye_fallback':'Missing right eye','quad_fallback':'Quad auxiliary','parallel_two_astc_snapshot_release':'Two ASTC eyes, opt-in','injected_launch_failure_fallback':'Injected launch failure'}
selected=[r for r in rows if r['scenario'] in labels]
fig,ax=plt.subplots(figsize=(10,5.5))
values=[int(r['max_active']) for r in selected]
colors=['#754cc3' if v==2 else '#9ca9bc' for v in values]
ax.barh([labels[r['scenario']] for r in selected],values,color=colors)
ax.invert_yaxis();ax.set_xlim(0,2.3);ax.set_xticks([0,1,2]);ax.set_xlabel('Maximum concurrent fake encoder calls (count)')
ax.set_title('Existing parallel-eye control flow: real CPU threads, mock encoders',loc='left',fontsize=13,pad=15)
for i,v in enumerate(values):ax.text(v+.04,i,str(v),va='center')
ax.spines[['top','right']].set_visible(False)
fig.text(.03,.02,'Controlled latches prove overlap and join/release order. This is not a GPU or frame-rate benchmark.',fontsize=9,color='#455065')
fig.tight_layout(rect=(0,.055,1,1))
for ext in ('png','svg'):
 fig.savefig(root/f'outcomes.{ext}',dpi=160)
p=root/'outcomes.svg';p.write_text('\n'.join(s.rstrip() for s in p.read_text().splitlines())+'\n')
