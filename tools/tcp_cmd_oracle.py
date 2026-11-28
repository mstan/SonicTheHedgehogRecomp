#!/usr/bin/env python3
"""Send one Sonic 1 JSON diagnostic command (default port 4379)."""
import json
import os
from pathlib import Path
import sys
sys.path.insert(0, str(Path(os.environ.get("GENESIS_RECOMP_ROOT", Path(__file__).resolve().parents[1] / "segagenesisrecomp")) / "tools"))
from genesis_tcp import request_raw
command = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {"id": 1, "cmd": "ping"}
print(request_raw(4379, command, timeout=3))
