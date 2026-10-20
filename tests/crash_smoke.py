"""M2 process-kill smoke check. The full 500-cycle campaign belongs to M6."""

import argparse
import json
import platform
import queue
import random
import subprocess
import tempfile
import threading
from pathlib import Path


def run_cycle(executable: Path, root: Path, sync: bool, target: int) -> dict:
    mode = "sync" if sync else "async"
    with tempfile.TemporaryDirectory(prefix="kill-", dir=root) as temporary:
        directory = Path(temporary)
        lines = queue.Queue()
        with (directory / "writer.stderr").open("w", encoding="utf-8") as errors:
            proc = subprocess.Popen(
                [str(executable), "write", str(directory / "db"), mode, "0"],
                stdout=subprocess.PIPE,
                stderr=errors,
                text=True,
                bufsize=1,
            )

            def read_acknowledgments():
                for line in proc.stdout:
                    lines.put(line)
                lines.put(None)

            reader = threading.Thread(target=read_acknowledgments, daemon=True)
            reader.start()
            acknowledged = 0
            try:
                with (directory / "acknowledged.txt").open("w", encoding="utf-8") as log:
                    while acknowledged < target:
                        line = lines.get(timeout=30)
                        if line is None:
                            raise RuntimeError("writer exited before requested kill point")
                        acknowledged = int(line)
                        log.write(line)
                        log.flush()
                proc.kill()
                proc.wait(timeout=30)
            except Exception:
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=30)
                raise
            finally:
                reader.join(timeout=30)
                proc.stdout.close()
        stderr = (directory / "writer.stderr").read_text(encoding="utf-8")
        if stderr:
            raise RuntimeError(f"writer diagnostics: {stderr}")
        completed = subprocess.run(
            [str(executable), "verify", str(directory / "db"), mode, str(acknowledged)],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        if completed.returncode:
            raise RuntimeError(completed.stdout + completed.stderr)
        return {
            "sync": sync,
            "acknowledged": acknowledged,
            "kill_returncode": proc.returncode,
            "recovery_output": completed.stdout + completed.stderr,
        }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=24301)
    args = parser.parse_args()
    executable = args.executable.resolve()
    root = executable.parent / "crash-data"
    root.mkdir(exist_ok=True)
    randomizer = random.Random(args.seed)
    result = {
        "platform": platform.platform(),
        "system": platform.system(),
        "seed": args.seed,
        "scope": "M2 smoke; not the M6 campaign",
        "cycles": [],
    }
    for sync in (True, False):
        for _ in range(6):
            result["cycles"].append(
                run_cycle(executable, root, sync, randomizer.randrange(10, 101))
            )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
