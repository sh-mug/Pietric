#!/usr/bin/env python3
"""The Piet programs of the benchmarks.

Each program reads its input with in(number) and runs one loop. In summod, primes, randaccess
and countdown the body is straight-line code ending in a pointer command (0: go on, 1: leave);
on leaving, the program prints the top of the stack with out(number). The Ackermann and
Takeuchi functions keep their call stack on the Piet stack and branch inside the loop (see
branch_grid). The programs are written as PNG images with a codel size of 1.

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


class Asm:
    """Emits commands for a stack whose top values are tracked by name (top last)."""

    def __init__(self, names):
        self.st = list(names)
        self.out = []

    def depth(self, name):
        return len(self.st) - self.st.index(name)

    def const(self, value, name):
        if value >= 1:
            self.out.append("push %d" % value)
        elif value == 0:
            self.out += ["push 1", "not"]
        else:
            self.out += ["push 1", "push %d" % (1 - value), "sub"]
        self.st.append(name)

    def read(self, name):
        """inn"""
        self.out.append("inn")
        self.st.append(name)

    def pick(self, name, new):
        """Copies `name` to the top as `new`."""
        d = self.depth(name)
        self.out += ["dup"] if d == 1 else (roll(d, d - 1) + ["dup"] + roll(d + 1, 1))
        self.st.append(new)

    def op(self, cmd, new):
        """Applies a command to the top value (not) or the top two values."""
        del self.st[-1 if cmd == "not" else -2:]
        self.out.append(cmd)
        self.st.append(new)

    def to_top(self, name):
        d = self.depth(name)
        if d > 1:
            self.out += roll(d, d - 1)
            self.st.append(self.st.pop(len(self.st) - d))

    def drop(self, name):
        self.to_top(name)
        self.out.append("pop")
        self.st.pop()

    def arrange(self, order):
        """Reorders the top values to `order` (bottom to top)."""
        assert sorted(order) == sorted(self.st[-len(order):]), (order, self.st)
        for name in order:
            self.to_top(name)


def ackermann():
    """Prints A(m, n), read as "m n". Stack: [-1, pending m values..., m, n]: pop m; if m is
    0, n becomes n + 1; else if n is 0, push m - 1 and n becomes 1; else push m - 1 and m,
    and n becomes n - 1. The -1 at the bottom ends the loop."""
    init = Asm([])
    init.const(-1, "end"); init.read("m"); init.read("n")
    top = ["m", "n"]
    body = []
    a = Asm(top)
    a.pick("m", "t"); a.const(1, "one"); a.op("add", "t"); a.op("not", "p")
    body += a.out + [("exit",)]
    a = Asm(top)
    a.pick("m", "t"); a.op("not", "p")
    lane = Asm(top); lane.const(1, "one"); lane.op("add", "n1"); lane.drop("m")
    body += a.out + [("branch", lane.out)]
    a = Asm(top)
    a.pick("n", "t"); a.op("not", "p")
    lane = Asm(top); lane.drop("n"); lane.const(1, "one"); lane.op("sub", "m1")
    lane.const(1, "n1")
    body += a.out + [("branch", lane.out)]
    a = Asm(top)
    a.const(1, "one"); a.op("sub", "n1")
    a.pick("m", "m1"); a.const(1, "one"); a.op("sub", "m1")
    a.arrange(["m1", "m", "n1"])
    body += a.out
    return init.out, body


def takeuchi():
    """Prints tak(x, y, z), read as "x y z". The stack holds frames of five values
    [f1, f2, f3, f4, tag] under a state [mode, A, B, C]. In mode 1 the program evaluates
    tak(A, B, C): C if not A > B, else it pushes K1(x, y, z) and evaluates tak(x - 1, y, z).
    In mode 0 it returns C to the top frame: K1(x, y, z) becomes K2(C, x, y, z) and evaluates
    tak(y - 1, z, x); K2(a, x, y, z) becomes K3(a, C) and evaluates tak(z - 1, x, y); K3(a, b)
    goes and evaluates tak(a, b, C); the frame of tag 0 at the bottom ends the loop."""
    init = Asm([])
    for name in ("f1", "f2", "f3", "f4", "tag"):
        init.const(0, name)
    init.const(1, "mode")
    for name in ("A", "B", "C"):
        init.read(name)
    top = ["f1", "f2", "f3", "f4", "tag", "mode", "A", "B", "C"]
    body = []

    def returning(frame, args):
        """Replaces the top frame by `frame` (values or names; none if None) and evaluates
        tak(*args), each arg a name and whether to subtract 1."""
        lane = Asm(top)
        names = []
        for i, source in enumerate(frame or []):
            if isinstance(source, int):
                lane.const(source, "g%d" % i)
            else:
                lane.pick(source, "g%d" % i)
            names.append("g%d" % i)
        lane.const(1, "mode1"); names.append("mode1")
        for i, (source, minus) in enumerate(args):
            lane.pick(source, "e%d" % i)
            if minus:
                lane.const(1, "one"); lane.op("sub", "e%d" % i)
            names.append("e%d" % i)
        for name in top:
            lane.drop(name)
        lane.arrange(names)
        return lane.out

    a = Asm(top)                                       # mode 0 at the bottom frame: done
    a.pick("mode", "t"); a.pick("tag", "u"); a.op("add", "t"); a.op("not", "p")
    body += a.out + [("exit",)]
    a = Asm(top)                                       # mode 1 and not A > B: return C
    a.pick("A", "t"); a.pick("B", "u"); a.op("gt", "t"); a.op("not", "t")
    a.pick("mode", "u"); a.op("mul", "p")
    lane = Asm(top); lane.drop("mode"); lane.const(0, "mode")
    lane.arrange(["mode", "A", "B", "C"])
    body += a.out + [("branch", lane.out)]
    a = Asm(top)                                       # mode 1: push K1, evaluate
    a.pick("mode", "p")
    lane = Asm(top)
    lane.pick("A", "g1"); lane.pick("B", "g2"); lane.pick("C", "g3")
    lane.const(0, "g4"); lane.const(1, "gtag"); lane.const(1, "mode1")
    lane.pick("A", "nA"); lane.const(1, "one"); lane.op("sub", "nA")
    lane.pick("B", "nB"); lane.pick("C", "nC")
    for name in ("mode", "A", "B", "C"):
        lane.drop(name)
    lane.arrange(["g1", "g2", "g3", "g4", "gtag", "mode1", "nA", "nB", "nC"])
    body += a.out + [("branch", lane.out)]
    a = Asm(top)                                       # return to K1
    a.pick("tag", "t"); a.const(1, "one"); a.op("sub", "t"); a.op("not", "p")
    body += a.out + [("branch", returning(["C", "f1", "f2", "f3", 2],
                                          [("f2", True), ("f3", False), ("f1", False)]))]
    a = Asm(top)                                       # return to K2
    a.pick("tag", "t"); a.const(2, "two"); a.op("sub", "t"); a.op("not", "p")
    body += a.out + [("branch", returning(["f1", "C", 0, 0, 3],
                                          [("f4", True), ("f2", False), ("f3", False)]))]
    body += returning(None, [("f1", False), ("f2", False), ("C", False)])  # return to K3
    return init.out, body


def branch_grid(init, body):
    """Lays out init and a loop body along row 0, like loop_grid, with branches: a
    ("branch", commands) item pops a value with ptr, and on 1 the pointer turns down into a
    lane of those commands, stacked in that column, which ends in white and joins the white
    path back to the loop along the bottom row; on 0 the body goes on along row 0. An
    ("exit",) item pops a value and on 1 prints the top of the stack and stops."""
    def big_push(token):
        return isinstance(token, str) and token.startswith("push ") and token != "push 1"

    # The block a branch happens in is one codel, so the command after it (on row 0 or first
    # in a lane) can push only 1: put "push 1, pop" before a larger push.
    fixed = []
    for i, item in enumerate(body):
        if isinstance(item, tuple) and item[0] == "branch" and big_push(item[1][0]):
            item = ("branch", ["push 1", "pop"] + item[1])
        fixed.append(item)
        if isinstance(item, tuple) and i + 1 < len(body) and big_push(body[i + 1]):
            fixed += ["push 1", "pop"]

    def blocks(tokens, color):
        """The colours of the blocks that the tokens leave, each repeated by its size."""
        out = []
        for token in tokens:
            parts = token.split()
            out += [hexof(color)] * (int(parts[1]) if parts[0] == "push" else 1)
            color = step(color, parts[0])
        return out, color

    cells, color = blocks(init, (0, 0))
    x = len(cells)
    branches = []
    for item in fixed:
        if isinstance(item, tuple):
            out, color = blocks(["ptr"], color)
            cells += out
            branches.append((len(cells), item, color))
        else:
            out, color = blocks([item], color)
            cells += out
    cells.append(hexof(color))
    k = len(cells) - 1
    n = k + 2
    lanes = {}
    for column, item, color in branches:
        if item[0] == "branch":
            first = item[1][0].split()[0]
            out, last = blocks(item[1][1:], step(color, first))
            lanes[column] = out + [hexof(last)]
    rows = max([6] + [len(lane) + 3 for lane in lanes.values()])
    grid = [[BLACK] * (n + 1) for _ in range(rows)]
    grid[0][:len(cells)] = cells
    grid[0][k + 1] = grid[0][n] = WHITE
    for r in range(1, rows):
        grid[r][x] = grid[r][n] = WHITE
    for c in range(x, n + 1):
        grid[rows - 1][c] = WHITE
    for column, item, color in branches:
        if item[0] == "branch":
            for r in range(1, rows - 1):
                grid[r][column] = lanes[column][r - 1] if r - 1 < len(lanes[column]) else WHITE
        else:
            e = step(color, "outn")
            grid[1][column] = grid[2][column] = hexof(e)
            grid[3][column] = grid[3][column - 1] = grid[2][column - 1] = hexof(step(e, "pop"))
    return grid


def write_png(grid, path):
    image = Image.new("RGB", (len(grid[0]), len(grid)))
    for y, row in enumerate(grid):
        for x, color in enumerate(row):
            image.putpixel((x, y), tuple(int(color[i:i + 2], 16) for i in (0, 2, 4)))
    image.save(path)


PROGRAMS = {"summod": summod, "primes": primes, "randaccess": randaccess,
            "countdown": countdown, "ackermann": ackermann, "takeuchi": takeuchi}


def grid(name):
    program = PROGRAMS[name]()
    return branch_grid(*program) if name in ("ackermann", "takeuchi") else loop_grid(*program)


def expected(name, *args):
    """The output of program `name` for the input numbers args."""
    if name == "ackermann":
        m, n = args
        pending = [m]
        while pending:
            m = pending.pop()
            if m == 0:
                n += 1
            elif n == 0:
                pending.append(m - 1)
                n = 1
            else:
                pending += [m - 1, m]
                n -= 1
        return str(n)
    if name == "takeuchi":
        import functools

        @functools.lru_cache(None)
        def tak(x, y, z):
            return tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y)) if y < x else z
        return str(tak(*args))
    n, = args
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
    for name in PROGRAMS:
        write_png(grid(name), os.path.join(out, name + ".png"))


if __name__ == "__main__":
    main()
