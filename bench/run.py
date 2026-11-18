#!/usr/bin/env python3
"""Runs the benchmarks of README.md: the same Piet programs and inputs on Pietric and on other
Piet implementations, checking every output.

Usage:
    bench/run.py --pietric build/Pietric [--pietcc PATH] [--npiet PATH] [--rpiet PATH]
                 [--piet-rs PATH] [--timeout 600] [--runs 3] [--work DIR]

Each implementation is optional except Pietric. A run that takes longer than --timeout seconds
is stopped and reported as such. Runs that take less than 20 s are repeated --runs times and
the median is reported. Needs Python 3 and Pillow, and downloads the Brainfuck interpreter
written in Piet and squares.b on the first run.
"""

import argparse
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys
import time
import urllib.request

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import programs  # noqa: E402

DOWNLOADS = {
    # The Brainfuck interpreter from the Piet samples page; it reads a Brainfuck program,
    # then "|", then the program's input.
    "piet_bfi.gif": "https://www.dangermouse.net/esoteric/piet/piet_bfi.gif",
    # Daniel B Cristofani's program that prints the squares from 0 to 10000.
    "squares.b": "https://brainfuck.org/squares.b",
}

# A Brainfuck program with four nested loops (64 * 10 * 10 * 10 iterations) that prints "A".
NESTED_LOOPS = ("++++++++[>++++++++<-]>[>++++++++++[>++++++++++[>++++++++++[-]<-]<-]<-]"
                "++++++++[>++++++++<-]>+.")


def download(work):
    paths = {}
    for name, url in DOWNLOADS.items():
        path = os.path.join(work, name)
        if not os.path.exists(path):
            print("downloading", url, file=sys.stderr)
            # Some servers refuse Python's default user agent.
            request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(request) as response, open(path, "wb") as f:
                f.write(response.read())
        paths[name] = path
    # Not every implementation reads GIF; all of them read PNG.
    png = os.path.join(work, "piet_bfi.png")
    Image.open(paths["piet_bfi.gif"]).convert("RGB").save(png)
    paths["piet_bfi.png"] = png
    return paths


def workloads(work):
    """(name, description, image, stdin, expected stdout)"""
    images = os.path.join(work, "programs")
    os.makedirs(images, exist_ok=True)
    for name in programs.PROGRAMS:
        programs.write_png(programs.grid(name), os.path.join(images, name + ".png"))
    generated = [
        ("summod", "sum of c mod 7, N = 10^8", (10 ** 8,)),
        ("primes", "primes up to N by trial division, N = 5 * 10^5", (5 * 10 ** 5,)),
        ("randaccess", "reads one of 50 values with rolls 3 to 52 deep, N = 10^7", (10 ** 7,)),
        ("countdown", "prints N numbers, N = 10^7", (10 ** 7,)),
        ("ackermann", "Ackermann function A(3, 12)", (3, 12)),
        ("takeuchi", "Takeuchi function tak(26, 16, 8)", (26, 16, 8)),
    ]
    result = [(name, description, os.path.join(images, name + ".png"),
               # One number per line: rpiet reads a line for each in(number).
               "".join("%d\n" % arg for arg in args).encode(),
               programs.expected(name, *args).encode())
              for name, description, args in generated]
    paths = download(work)
    squares = open(paths["squares.b"], "rb").read()
    result += [
        ("bf-squares", "Brainfuck interpreter in Piet running squares.b", paths["piet_bfi.png"],
         squares + b"|", "".join("%d\n" % (i * i) for i in range(101)).encode()),
        ("bf-loops", "Brainfuck interpreter in Piet running 4 nested loops (64,000 iterations)",
         paths["piet_bfi.png"], NESTED_LOOPS.encode() + b"|", b"A"),
    ]
    return result


def normalize_pietcc(out):
    """PietCC prints prompts before reading and the stack when the program ends."""
    out = out.replace(b"Enter number: ", b"").replace(b"Enter char: ", b"")
    return re.sub(rb"\n?Stack (empty|\(size \d+\): .*)\n?$", b"", out, flags=re.S)


def run(command, stdin, timeout, cwd):
    start = time.perf_counter()
    try:
        result = subprocess.run(command, input=stdin, capture_output=True, timeout=timeout,
                                cwd=cwd)
    except subprocess.TimeoutExpired:
        return None, None
    return time.perf_counter() - start, result.stdout


def measure(command, stdin, expected, normalize, args, cwd):
    times = []
    for _ in range(args.runs):
        elapsed, out = run(command, stdin, args.timeout, cwd)
        if elapsed is None:
            return {"timeout": args.timeout}
        if normalize(out).rstrip(b"\n") != expected.rstrip(b"\n"):
            return {"wrong": hashlib.md5(out).hexdigest(), "time": elapsed}
        times.append(elapsed)
        if elapsed >= 20:
            break
    return {"time": statistics.median(times), "runs": len(times)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pietric", required=True)
    parser.add_argument("--pietcc")
    parser.add_argument("--npiet")
    parser.add_argument("--rpiet")
    parser.add_argument("--piet-rs", help="piet_programming_language (your-diary)")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--work", default="bench-work")
    parser.add_argument("--json", help="write the results to this file")
    parser.add_argument("--only", help="run only these programs (comma-separated names)")
    args = parser.parse_args()
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    abs_or_none = lambda p: os.path.abspath(p) if p else None  # noqa: E731

    identity = lambda out: out  # noqa: E731
    compilers = [("Pietric", abs_or_none(args.pietric), identity)]
    if args.pietcc:
        compilers.append(("PietCC", abs_or_none(args.pietcc), normalize_pietcc))
    interpreters = [(name, abs_or_none(path), flags) for name, path, flags in [
        ("npiet", args.npiet, ["-q"]),
        ("rpiet", args.rpiet, ["-c", "1"]),
        ("piet_programming_language", args.piet_rs, ["-c", "1"]),
    ] if path]

    results = {"run": {}, "compile": {}}
    only = set(args.only.split(",")) if args.only else None
    for name, description, image, stdin, expected in workloads(work):
        if only and name not in only:
            continue
        results["run"][name] = {"description": description}
        results["compile"].setdefault(image, {})
        for compiler, path, normalize in compilers:
            exe = os.path.join(work, "%s-%s" % (compiler, os.path.basename(image).split(".")[0]))
            if compiler not in results["compile"][image]:
                commands = [[path, "-O2", image, "-o", exe]]
                if compiler == "PietCC":
                    # Without --o2 for programs that --o2 does not compile in time.
                    commands = [[path, image, "--o2", "-o", exe], [path, image, "-o", exe]]
                entry = None
                for i, command in enumerate(commands):
                    if os.path.exists(exe):
                        os.remove(exe)
                    elapsed, _ = run(command, None, args.timeout, work)
                    if elapsed is not None and os.path.exists(exe):
                        entry = {"time": elapsed, "size": os.path.getsize(exe),
                                 "optimized": i == 0}
                        break
                results["compile"][image][compiler] = entry or {"timeout": args.timeout}
            if "time" not in results["compile"][image][compiler]:
                results["run"][name][compiler] = {"no executable": True}
                continue
            result = measure([exe], stdin, expected, normalize, args, work)
            result["optimized"] = results["compile"][image][compiler]["optimized"]
            results["run"][name][compiler] = result
            print(name, compiler, results["run"][name][compiler], file=sys.stderr, flush=True)
        for interpreter, path, flags in interpreters:
            results["run"][name][interpreter] = measure([path] + flags + [image], stdin, expected,
                                                        identity, args, work)
            print(name, interpreter, results["run"][name][interpreter], file=sys.stderr,
                  flush=True)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)
    columns = [c[0] for c in compilers] + [i[0] for i in interpreters]
    print("| Program | " + " | ".join(columns) + " |")
    print("|---" * (len(columns) + 1) + "|")
    for name, row in results["run"].items():
        cells = []
        for column in columns:
            r = row.get(column, {})
            if "timeout" in r:
                cells.append("> %d s" % r["timeout"])
            elif "wrong" in r:
                cells.append("wrong output")
            elif "time" in r:
                cells.append("%.3f s%s" % (r["time"], "" if r.get("optimized", True) else " †"))
            else:
                cells.append("—")
        print("| %s | %s |" % (row["description"], " | ".join(cells)))


if __name__ == "__main__":
    main()
