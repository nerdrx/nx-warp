"""Image-only coarse region matching for motion-field experiments.

The returned affine displacement is in normalized 64-cell coordinates and has
the same convention as the renderer: ``d(p) = p - q(p)``.
"""
from collections import deque
import numpy as np


def _block_colour(rgb):
    h, w = rgb.shape[:2]
    assert h % 64 == 0 and w % 64 == 0
    sy, sx = h // 64, w // 64
    return rgb.reshape(64, sy, 64, sx, 3).mean((1, 3))


def _flood(colour, threshold):
    labels = np.full((64, 64), -1, np.int32); comps = []
    for y in range(64):
        for x in range(64):
            if labels[y, x] >= 0: continue
            label = len(comps); labels[y, x] = label; q = [(y, x)]
            for yy, xx in q:
                for ny, nx in ((yy-1,xx),(yy+1,xx),(yy,xx-1),(yy,xx+1)):
                    if 0 <= ny < 64 and 0 <= nx < 64 and labels[ny,nx] < 0:
                        if np.linalg.norm(colour[ny,nx] - colour[yy,xx]) < threshold:
                            labels[ny,nx] = label; q.append((ny,nx))
            comps.append(q)
    return labels, comps


def _descriptor(points, colour):
    ys, xs = np.asarray(points).T
    xy = np.stack(((xs + .5) / 64, (ys + .5) / 64), 1)
    c = xy.mean(0); z = xy - c
    cov = (z.T @ z) / max(1, len(points) - 1)
    rgb = colour[ys, xs].mean(0)
    ev, vec = np.linalg.eigh(cov); order = np.argsort(ev)[::-1]
    return dict(n=len(points), centroid=c, colour=rgb, cov=cov,
                eig=ev[order], axis=vec[:, order[0]])


def match_regions(previous_rgb, current_rgb, current_labels, threshold=38.0,
                  min_cells=12, max_centroid_distance=.2):
    """Match current labels to colour regions in the previous image.

    Returns ``(coefficients, valid, report)`` where coefficients has shape
    ``(max_label+1, 3, 2)`` and maps current normalized cell coordinates to
    displacement vectors.  Invalid labels are zeroed and separately reported.
    """
    previous_rgb = np.asarray(previous_rgb, float); current_rgb = np.asarray(current_rgb, float)
    labels = np.asarray(current_labels, int); assert labels.shape == (64, 64)
    previous_colour = _block_colour(previous_rgb)
    pc, prev_comps = _flood(previous_colour, threshold)
    cc = _block_colour(current_rgb)
    max_label = int(labels.max()) if labels.size and labels.max() >= 0 else -1
    coeff = np.zeros((max_label + 1, 3, 2), np.float32); valid = np.zeros(max_label + 1, bool)
    cur_comps = [np.argwhere(labels == k) for k in range(max_label + 1)]
    prev_desc = [_descriptor(p, previous_colour) for p in prev_comps if len(p) >= min_cells]
    used = set(); report = {"matches": [], "unmatched": [], "previous_regions": len(prev_desc)}
    for k, arr in enumerate(cur_comps):
        if len(arr) < min_cells:
            report["unmatched"].append(k); continue
        cur = _descriptor(arr, cc); candidates=[]
        for j, old in enumerate(prev_desc):
            if j in used: continue
            dc=np.linalg.norm(cur['colour']-old['colour'])/255.0
            da=abs(np.log((cur['n']+.5)/(old['n']+.5)))
            dp=np.linalg.norm(cur['centroid']-old['centroid'])
            shape=np.linalg.norm(cur['eig']-old['eig'])/(cur['eig'][0]+old['eig'][0]+1e-5)
            if dc > .18 or da > np.log(2.5) or dp > max_centroid_distance: continue
            score=2.2*dc + .35*min(da,3) + .8*dp + .25*shape
            candidates.append((score,j,old))
        if not candidates: report["unmatched"].append(k); continue
        score,j,old=min(candidates)
        confidence=float(np.exp(-score))
        if np.linalg.norm(cur['centroid']-old['centroid']) > max_centroid_distance or confidence < .18:
            report["unmatched"].append(k); continue
        used.add(j); A=np.eye(2)
        # Only elongated regions get an orientation-derived rotation; round
        # regions retain translation because their axis is unstable.
        if cur['eig'][0] > 2.5 * max(cur['eig'][1], 1e-5) and old['eig'][0] > 2.5 * max(old['eig'][1], 1e-5):
            a=np.arctan2(cur['axis'][1],cur['axis'][0]); b=np.arctan2(old['axis'][1],old['axis'][0])
            ang=float(np.clip((b-a+np.pi/2)%np.pi-np.pi/2, -np.pi/4, np.pi/4)); ca,sa=np.cos(ang),np.sin(ang)
            A=np.array([[ca,-sa],[sa,ca]])
        # q = A (p-cur_c) + old_c; d = p-q
        cvec=A @ (2*cur['centroid']-old['centroid']) - cur['centroid']
        coeff[k, :2] = (np.eye(2)-A).T; coeff[k, 2] = cvec
        valid[k]=True; report['matches'].append(dict(label=k, previous=j, confidence=confidence, score=float(score)))
    report['valid'] = int(valid.sum()); report['current_regions'] = sum(len(a) >= min_cells for a in cur_comps)
    return coeff, valid, report
