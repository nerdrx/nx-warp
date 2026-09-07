#!/usr/bin/env python3
import matplotlib.pyplot as plt
x=['control','r16','reverse']; gpu=[1.2,.9,1.0]; copy=[.29,.59,.28]; render=[1.9,3.9,2.7]; src=[74.25,89,89]
fig,(a,b)=plt.subplots(2,1,figsize=(7,5.5),layout='constrained'); a.plot(x,gpu,'o-',label='decoder GPU ms');a.plot(x,copy,'s--',label='copy GPU ms');a.plot(x,render,'^-',label='render GPU ms');b.plot(x,src,'o-',color='#c06c84',label='active source/s');a.set_ylabel('GPU time (ms)');b.set_ylabel('Source rate / s');b.set_xlabel('Capture');a.grid(alpha=.25);b.grid(alpha=.25);a.legend(frameon=False,ncol=3,loc='lower center',bbox_to_anchor=(.5,1.01));fig.savefig('r16-handoff-windows.svg');fig.savefig('r16-handoff-windows.png',dpi=180)
