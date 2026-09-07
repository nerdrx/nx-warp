import matplotlib.pyplot as plt
labels=['baseline\nR8 reverse','scale 0.40\nprobe','scale 0.40\nreverse','scale 0.40\ndirect']
render=[2.55,1.65,2.60,1.45]; decwall=[2.3,2.7,2.2,2.0]; src=[88.5,88.25,88.0,87.0]
fig,(a,b)=plt.subplots(1,2,figsize=(8,3.6),constrained_layout=True)
import numpy as np
x=np.arange(len(labels)); width=.36
a.bar(x-width/2,render,width,color='#4472c4',label='renderer GPU')
a.bar(x+width/2,decwall,width,color='#c55a11',label='decoder wall')
a.set_xticks(x,labels)
a.set_ylabel('milliseconds (median window mean)');a.set_title('Active 2 s windows');a.legend(fontsize=8)
b.plot(labels,src,'o-',color='#2f5597');b.set_ylabel('new sources / s');b.set_ylim(0,100);b.set_title('Reported active cadence')
fig.suptitle('Output scale 0.40: matched live exploratory runs')
fig.subplots_adjust(bottom=.24)
fig.text(.5,.02,'Baseline/probe/reverse; session gaps retained; no FPS or quality equivalence claim',ha='center',fontsize=8)
fig.savefig('docs/figures/240fps/output-scale40.png',dpi=180)
