import matplotlib.pyplot as plt
labels=['baseline\nR8 reverse','output scale\n0.40 probe','output scale\n0.40 reverse']
render=[2.55,1.65,2.60]; decwall=[2.3,2.7,2.2]; src=[88.5,88.25,88.0]
fig,(a,b)=plt.subplots(1,2,figsize=(8,3.6),constrained_layout=True)
a.bar(labels,render,color=['#777','#4472c4','#777'],label='renderer GPU')
a.bar(labels,decwall,bottom=render,color='#c55a11',label='decoder wall')
a.set_ylabel('milliseconds (median window mean)');a.set_title('Active 2 s windows');a.legend(fontsize=8)
b.plot(labels,src,'o-',color='#2f5597');b.set_ylabel('new sources / s');b.set_ylim(0,100);b.set_title('Reported active cadence')
fig.suptitle('Output scale 0.40: matched live exploratory runs')
fig.text(.5,.01,'Baseline/probe/reverse; session gaps retained; no FPS or quality equivalence claim',ha='center',fontsize=8)
fig.savefig('docs/figures/240fps/output-scale40.png',dpi=180)
