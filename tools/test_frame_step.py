"""Constant game speed (runtime/hle/framepace.c) against simulated frames (tools/test_frame_step.c), one process per
case (the pacing code keeps its state in statics): 1 - ~45 fps with the per-channel hook: channel 0 gets the world's
ticks, the first 30 fps frame gives it one, the world at 60 ticks/s (#179 #188); 2 - no hook: the per-frame mode never
runs; 3 - 60 fps: nothing changes. No game data."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('frame-step-test.exe' if os.name == 'nt' else 'frame-step-test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-O1', '-Iruntime', 'tools/test_frame_step.c', '-lm', '-o', str(exe)], cwd=ROOT, check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('LARECOMP_')}
    failed = 0
    for case in ('1', '2', '3'):
        r = subprocess.run([str(exe), case], capture_output=True, text=True, env=env)
        print(r.stdout, end='')
        if r.returncode:
            failed += 1
            print(r.stderr[-2000:])
    print(f'{3 - failed} of 3 constant game speed cases passed')
    sys.exit(1 if failed else 0)
