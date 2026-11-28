#!/usr/bin/env python3
"""Format or check C/C++ files by forwarding to the bbtools-pub formatter.

The push hook and CI must apply one formatter; a vendored copy drifted from pub
and made the two disagree, so this entry only locates pub and passes arguments.
"""

import os
from pathlib import Path
import sys


PUB_FORMAT = Path(__file__).resolve().parents[2] / 'third-party/bbtools-pub/tools/format/format-cpp.py'


if __name__ == '__main__':
    if not PUB_FORMAT.is_file():
        sys.exit(f'format_cpp: missing {PUB_FORMAT}; run git submodule update --init third-party/bbtools-pub')
    os.execv(sys.executable, [sys.executable, '-I', str(PUB_FORMAT), *sys.argv[1:]])
