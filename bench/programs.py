#!/usr/bin/env python3
"""The Piet programs of the benchmarks.

Each program reads N with in(number), runs one loop whose body is straight-line code ending
in a pointer command (0: go on, 1: leave), and on leaving prints the top of the stack with
out(number). The programs are written as PNG images with a codel size of 1.

Usage: programs.py <output-dir>   (needs Pillow)
"""

import os
import sys

from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
from gen_cases import BLACK, WHITE, hexof, lay_out_row, step  # noqa: E402


def swap():
    return ["push 2", "push 1", "roll"]


def roll(depth, rolls):
    """roll(depth, rolls); -1 is pushed as 1 - 2."""
    if rolls == -1:
        return ["push %d" % depth, "push 1", "push 2", "sub", "roll"]
    return ["push %d" % depth, "push %d" % rolls, "roll"]


def pick(k):
    """Copies the value k deep (1 is the top) to the top."""
    if k == 1:
        return ["dup"]
    return roll(k, -1) + ["dup"] + roll(k + 1, 1)


def summod():
    """Prints the sum of c mod 7 for c = N..1. Stack: [c, sum]."""
    init = ["inn", "push 1", "push 1", "sub"]
    body = (swap() + ["dup", "push 7", "mod"] + roll(3, 2) + ["add"] + swap() +
            ["push 1", "sub", "dup"] + roll(3, 1) + ["not", "ptr"])
    return init, body


def primes():
    """Prints how many of 2..N are prime, by trial division (N >= 2). Stack: [n, d, count]."""
    init = ["inn", "push 2", "push 1", "push 1", "sub"]
    body = (roll(3, 1) +                                       # [count, n, d]
            ["dup", "dup", "mul"] + pick(3) + ["gt"] +          # prime: d * d > n
            pick(3) + pick(3) + ["mod", "not"] +                # divides: n % d == 0
            pick(2) + ["add", "not", "not"] +                   # next = prime or divides
            roll(5, -1) + roll(3, 2) + ["add"] +                # count += prime
            roll(4, 1) +
            ["dup"] + roll(4, -1) + swap() + ["sub"] +          # n -= next
            roll(3, 1) +
            pick(2) + ["push 1"] + swap() + ["sub", "mul", "add", "push 1", "add"] +  # d
            roll(3, 2) +                                        # [n, d, count]
            pick(3) + ["push 1", "gt", "not", "ptr"])           # leave when n <= 1
    return init, body


def randaccess(k=50):
    """Keeps 1..k on the stack and adds up the (c mod k)-th of them for c = N..1, reaching
    it with rolls 3 to k + 2 deep. Stack: [v1..vk, c, sum]."""
    init = ["push 1"] + ["dup", "push 1", "add"] * (k - 1) + ["inn", "push 1", "push 1", "sub"]
    body = (swap() +
            ["dup", "push %d" % k, "mod", "push 3", "add"] +            # depth e of the value
            ["push 1", "push 2", "sub", "roll"] +                       # bring it to the top
            ["dup"] + pick(3) + ["push %d" % k, "mod", "push 4", "add", "push 1", "roll"] +
            swap() + roll(3, 1) + ["add"] + swap() +                     # sum += value
            ["push 1", "sub", "dup"] + roll(3, 1) + ["not", "ptr"])
    return init, body


def countdown():
    """Prints N..1, one per line, then 0. Stack: [c]."""
    init = ["inn"]
    body = ["dup", "outn", "push 10", "outc", "push 1", "sub", "dup", "not", "ptr"]
    return init, body


def loop_grid(init, body):
    """Lays out init and the loop body in row 0. The loop returns through the white path
    on the right, along row 5 and up column x into the body; leaving it, the pointer turns
    down into E (out(number), two codels tall) and then into C (pop), whose exits are all
    black."""
    init_cells, first = lay_out_row(init)
    x = len(init_cells)
    cells = []
    color = first
    for token in body:
        parts = token.split()
        width = int(parts[1]) if parts[0] == "push" else 1
        cells += [hexof(color)] * width
        color = step(color, parts[0])
    last = color
    cells.append(hexof(last))
    k = x + len(cells) - 1
    n = k + 2
    grid = [[BLACK] * (n + 1) for _ in range(6)]
    grid[0][:x] = init_cells
    grid[0][x:k + 1] = cells
    grid[0][k + 1] = grid[0][n] = WHITE
    for r in range(1, 6):
        grid[r][x] = grid[r][n] = WHITE
    for c in range(x, n + 1):
        grid[5][c] = WHITE
    e = step(last, "outn")
    grid[1][k] = grid[2][k] = hexof(e)
    grid[3][k] = grid[3][k - 1] = grid[2][k - 1] = hexof(step(e, "pop"))
    return grid


def write_png(grid, path):
    image = Image.new("RGB", (len(grid[0]), len(grid)))
    for y, row in enumerate(grid):
        for x, color in enumerate(row):
            image.putpixel((x, y), tuple(int(color[i:i + 2], 16) for i in (0, 2, 4)))
    image.save(path)


PROGRAMS = {"summod": summod, "primes": primes, "randaccess": randaccess,
            "countdown": countdown}


def expected(name, n):
    """The output of program `name` for input n."""
    if name == "summod":
        return str(sum(c % 7 for c in range(1, n + 1)))
    if name == "primes":
        sieve = bytearray([1]) * (n + 1)
        sieve[0:2] = b"\0\0"
        for i in range(2, int(n ** 0.5) + 1):
            if sieve[i]:
                sieve[i * i::i] = bytearray(len(sieve[i * i::i]))
        return str(sum(sieve))
    if name == "randaccess":
        k = 50
        return str(sum(k - (c % k) for c in range(n, 0, -1)))
    if name == "countdown":
        return "".join("%d\n" % c for c in range(n, 0, -1)) + "0"
    raise KeyError(name)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, program in PROGRAMS.items():
        write_png(loop_grid(*program()), os.path.join(out, name + ".png"))


if __name__ == "__main__":
    main()
