# Where the tests find ground truth. The public repository leaves some files out (ground_truth/README.md: the stock
# firmware dumps and the longer recordings of everyday house traffic; their SHA-256 sums are in
# ground_truth/SHA256SUMS.unpublished). OWS_GROUND_TRUTH=<dir> points the tests at a full copy; without one, the tests
# that need a missing file skip.
import os
import pathlib

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PUBLIC = ROOT / 'ground_truth'
GT = pathlib.Path(os.environ.get('OWS_GROUND_TRUTH') or PUBLIC)


def need(rel):
    """The path of ground_truth/<rel>, or skip the test if this copy does not have it."""
    p = GT / rel
    if not p.exists():
        pytest.skip(
            f'ground_truth/{rel} is not published (ground_truth/README.md); OWS_GROUND_TRUTH=<full copy> runs it'
        )
    return p


def read(rel):
    return need(rel).read_bytes()
