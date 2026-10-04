# Rejected stale NACK suppression

The saved patch skips requests for a frame the same callback immediately retires under existing skew. Window checks passed 217 normal/ASan/UBSan. It was reverted before source commit: NACK counts also feed adaptive FEC. Suppression lowers `(reconstructed+nacked)/sent`; the incomplete-frame 3% floor remains, but can understate heavier loss and slow protection. Keeping recovery response intact outweighs saving those control requests. No installed or production source behavior changed from this prototype.

`gzip -dc proposal.patch.gz` retrieves the exact reviewable patch. Do not enable it as an optimization without preserving loss evidence.
