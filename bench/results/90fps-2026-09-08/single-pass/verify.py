import json, sys, tarfile, tempfile
from pathlib import Path
root = Path(__file__).parent
sys.path.insert(0, str(root.parent / 'centre-first'))
from summarize import summarize
with tarfile.open(root / 'raw-inputs.tar.gz') as t:
    m = json.load(t.extractfile('camera90.manifest.json'))
    assert m['fps'] == 90 and m['frames'] == 720
    assert [x['frame'] for x in m['source_frames']] == list(range(720))
    assert len({x['sourcehash'] for x in m['source_frames']}) == 720
    assert m['streamhash'] + '  camera90.nxv' in t.extractfile('pixels-and-identities.log').read().decode()
    expected = json.loads((root / 'summary.json').read_text())
    with tempfile.TemporaryDirectory() as tmp:
        for name, result in expected.items():
            p = Path(tmp) / (name + '.csv')
            p.write_bytes(t.extractfile(name + '.csv').read())
            assert summarize(p, 90, 0) == result
print('Verified all six runs, distinct source hashes and device fixture identity.')
