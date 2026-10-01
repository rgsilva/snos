"""Materialize the unmodified main console in an ignored build directory."""
import subprocess

MAIN = "99b6002"

def prepare(root, build):
    kernel = build / "main-console"
    kernel.mkdir(parents=True, exist_ok=True)
    for name in ("net.c", "link.h"):
        source = subprocess.check_output(
            ["git", "show", f"{MAIN}:snes/kernel/{name}"], cwd=root)
        (kernel / name).write_bytes(source)
    # Local includes select main's link.h; other unchanged headers come from
    # the repository. No substitutions inside either production source.
    return ["-I", str(root / "snes/kernel"),
            '-DCONSOLE_NET_SOURCE="' + str(kernel / "net.c") + '"']
