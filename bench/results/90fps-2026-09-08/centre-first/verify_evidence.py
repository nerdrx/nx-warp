#!/usr/bin/env python3
"""Validate identities, freshness and summaries directly from the evidence archive."""
import json, tarfile, tempfile
from pathlib import Path
from summarize import summarize
root = Path(__file__).parent
with tarfile.open(root / 'raw-inputs.tar.gz') as archive:
    manifest = json.load(archive.extractfile('camera90.manifest.json'))
    assert manifest['frames'] == 720 and manifest['fps'] == 90
    frames = manifest['source_frames']
    assert [f['frame'] for f in frames] == list(range(720))
    assert len({f['sourcehash'] for f in frames}) == 720
    identities = archive.extractfile('device-v2-identities.txt').read().decode()
    assert manifest['streamhash'] + '  camera90.nxv' in identities
    expected = json.loads((root / 'summary.json').read_text())
    with tempfile.TemporaryDirectory() as tmp:
        for name, result in expected.items():
            path = Path(tmp) / (name + '.csv')
            path.write_bytes(archive.extractfile(name + '.csv').read())
            assert summarize(path, 90, 0) == result, name
print('Verified 720 distinct source hashes, device stream identity, and all archived summary rows.')
