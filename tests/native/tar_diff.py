# Python tarfile + GNU tar + our TarReader agree on random archives written by our TarWriter (names near the 100 / 155 limits, unicode), and our reader reads
# Python's ustar / gnu / pax archives.  usage: python3 -I tar_diff.py <first seed> <last seed>   (needs ./tw built from tw_tar.cpp)
import sys, subprocess, tarfile, hashlib, os, random
bad = 0
for seed in range(int(sys.argv[1]), int(sys.argv[2])):
    tarp = f"/tmp/loopA2tard_{seed}.tar"; man = f"/tmp/loopA2tard_{seed}.tsv"
    subprocess.run(["./tw", "write", str(seed), tarp, man], check=True)
    want = {}
    for line in open(man, encoding="utf-8").read().split("\n"):
        if not line: continue
        n, sz, mt, d, h = line.split("\t"); want[n] = (int(sz), int(mt), d == "1", h)
    # 1. python reads ours
    try:
        with tarfile.open(tarp) as t:
            seen = {}
            for m in t:
                nm = m.name.rstrip("/") if m.isdir() else m.name
                h = "-" if m.isdir() else hashlib.sha256(t.extractfile(m).read()).hexdigest()
                seen[nm] = (m.size, int(m.mtime), m.isdir(), h)
    except Exception as e:
        print("seed", seed, "python cannot read our tar:", e); bad += 1; continue
    for n, v in want.items():
        got = seen.get(n)
        if got is None: print("seed", seed, "python: missing", repr(n)); bad += 1
        elif (got[0] if not v[2] else 0, got[1], got[2], got[3]) != (v[0], v[1], v[2], v[3]): print("seed", seed, "python: differs", repr(n), got, v); bad += 1
    if len(seen) != len(want): print("seed", seed, "python entries", len(seen), "want", len(want)); bad += 1
    # 2. GNU tar lists names
    r = subprocess.run(["tar", "tf", tarp], capture_output=True)
    names = [x for x in r.stdout.decode("utf-8", "replace").split("\n") if x]
    if r.returncode != 0 or len(names) != len(want): print("seed", seed, "GNU tar:", r.returncode, len(names), len(want), r.stderr[:200]); bad += 1
    # 3. our reader reads ours
    subprocess.run(["./tw", "read", tarp, man + ".out"], check=True)
    out = [l for l in open(man + ".out", encoding="utf-8").read().split("\n") if l]
    if not out[-1].startswith("END\t0"): print("seed", seed, "our reader:", out[-1]); bad += 1
    got = {}
    for l in out[:-1]:
        n, sz, mt, d, h, sk = l.split("\t"); got[n] = (int(sz), int(mt), d == "1", h)
    for n, v in want.items():
        g = got.get(n)
        if g is None: print("seed", seed, "ours: missing", repr(n)); bad += 1
        elif (g[0] if not v[2] else 0, g[1], g[2], g[3]) != (v[0], v[1], v[2], v[3]): print("seed", seed, "ours: differs", repr(n), g, v); bad += 1
    # 4. python/GNU write -> our reader
    for fmt in (tarfile.USTAR_FORMAT, tarfile.GNU_FORMAT, tarfile.PAX_FORMAT):
        rr = random.Random(seed * 7 + fmt)
        p2 = f"/tmp/loopA2tard_py_{seed}_{fmt}.tar"; ents = {}
        try:
            with tarfile.open(p2, "w", format=fmt) as t:
                for k in range(rr.randrange(1, 15)):
                    nm = "vm/" + "".join(rr.choice("abcXYZ09 -_.é日") for _ in range(rr.randrange(1, 60 if fmt != tarfile.USTAR_FORMAT else 30))) + f"-{k}"
                    data = rr.randbytes(rr.randrange(0, 1500)); ti = tarfile.TarInfo(nm); ti.size = len(data); ti.mtime = rr.randrange(1000000000, 1900000000)
                    import io; t.addfile(ti, io.BytesIO(data)); ents[nm] = (len(data), ti.mtime, hashlib.sha256(data).hexdigest())
        except Exception as e:
            continue
        subprocess.run(["./tw", "read", p2, p2 + ".out"], check=True)
        out = [l for l in open(p2 + ".out", encoding="utf-8").read().split("\n") if l]
        if not out[-1].startswith("END\t0"): print("seed", seed, "fmt", fmt, "our reader on python tar:", out[-1]); bad += 1
        got = {l.split("\t")[0]: l.split("\t") for l in out[:-1]}
        for n, v in ents.items():
            g = got.get(n)
            if g is None or (int(g[1]), int(g[2]), g[4]) != v: print("seed", seed, "fmt", fmt, "ours differs on python tar:", repr(n), g, v); bad += 1
        os.remove(p2); os.remove(p2 + ".out")
    os.remove(tarp); os.remove(man); os.remove(man + ".out")
print("done, problems:", bad)
