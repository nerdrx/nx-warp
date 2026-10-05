#!/usr/bin/env python3
"""Observed standalone API outcomes, never frame rate or latency."""
from pathlib import Path
import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
base=Path(__file__).resolve().parent
rows=list(csv.DictReader((base/'raw/gpu/outcomes.csv').open()))
labels=['Blocked fence polls (VK_TIMEOUT)','Pool resets while blocked','Completed fence polls (VK_SUCCESS)','Pool resets after completion (VK_SUCCESS)']
values=[sum(r['blocked_poll']=='2' for r in rows),sum(int(r['reset_while_pending']) for r in rows),sum(r['completed_poll']=='0' for r in rows),sum(r['retired_pool_reset']=='0' for r in rows)]
fig,ax=plt.subplots(figsize=(10,4.5))
ax.barh(labels,values,color=['#7953c5','#b58f9a','#44a095','#44a095']);ax.invert_yaxis()
ax.set_xlim(0,4.6);ax.set_xticks(range(5));ax.set_xlabel('Observed count across four controlled submissions')
ax.set_title('RX 7900 XTX: isolated Vulkan fence retirement gate',loc='left',fontsize=14,pad=14)
for i,v in enumerate(values):ax.text(v+.07,i,str(v),va='center')
ax.spines[['top','right']].set_visible(False)
fig.text(.03,.025,'Empty command buffer; validation enabled. No codec image work, compositor throughput or latency measurement.',fontsize=9,color='#455065')
fig.tight_layout(rect=(0,.065,1,1))
for ext in ('png','svg'):fig.savefig(base/f'outcomes.{ext}',dpi=160)
p=base/'outcomes.svg';p.write_text('\n'.join(s.rstrip() for s in p.read_text().splitlines())+'\n')
