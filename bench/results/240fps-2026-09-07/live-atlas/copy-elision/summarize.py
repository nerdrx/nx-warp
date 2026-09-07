from pathlib import Path
import json,re,statistics
p=Path(__file__).resolve().parent
text=(p/'measure-filtered.log').read_text(errors='replace')
rows=[]
for m in re.finditer(r'render: (\d+) iterations in ([\d.]+) s \(([\d.]+)/s\), .*?, (\d+) new-source',text):
 rows.append({'iterations':int(m[1]),'window_s':float(m[2]),'reported_rate':float(m[3]),'new_source':int(m[4])})
copy=[float(m[1]) for m in re.finditer(r'fence-post [\d.]+ ms = nxvc gpu [\d.]+ \+ copy gpu ([\d.]+)',text)]
atlas=[]
for m in re.finditer(r'atlas: frames (\d+), atlas (\d+), picture (\d+), avg dispatches ([\d.]+), avg assembled ([\d.]+), avg valid ([\d.]+)',text):
    atlas.append({'frames':int(m[1]),'atlas':int(m[2]),'picture':int(m[3]),'dispatches':float(m[4]),'assembled':float(m[5]),'valid':float(m[6])})
mode=re.search(r'defoveate .* atlas-mode (\d+)',text)
json.dump({'apk_sha256':'39ee6502c84c6ca1f706d9733da2f277a58c12b8c642039707bb74dbe52cc70f','atlas_mode_reported':int(mode[1]) if mode else None,'windows':rows,'decoder_reports':atlas,'copy_gpu_ms_aggregated':copy,'copy_gpu_ms_p50':statistics.median(copy) if copy else None,'source_per_s_p50':statistics.median([r['new_source']/r['window_s'] for r in rows]) if rows else None},(p/'summary-recomputed.json').open('w'),indent=2); print(len(rows),len(copy),len(atlas))
