import json
from pathlib import Path
import matplotlib.pyplot as plt
p=Path(__file__).parent
d=json.loads((p/'summary.json').read_text())
v=[d['legacy_mean_ms'],d['crc_mean_ms']]
fig,ax=plt.subplots(figsize=(7,4),layout='constrained')
bars=ax.bar(['Legacy inner SHA','Trusted-LAN CRC'],v,color=['#7700ff','#00a6c7'])
ax.bar_label(bars,fmt='%.2f ms',padding=4)
ax.set_ylim(0,15)
ax.set_ylabel('Receiver CPU ms per frame-equivalent')
ax.set_title('Pico: ~4.4× faster packet processing')
ax.text(.5,.94,'Synthetic FEC batch; excludes network, GPU and display',ha='center',transform=ax.transAxes,fontsize=9)
fig.savefig(p/'pico-receiver-cost.png',dpi=160)
