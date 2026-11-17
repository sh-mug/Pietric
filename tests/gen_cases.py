#!/usr/bin/env python3
"""Generate the Piet test programs in tests/cases/.

Each case is written as <name>.txt (Pietric's text format: one row of
whitespace-separated hex colours per line), <name>.out (expected stdout) and,
if the case reads input, <name>.in.

The expected outputs below are derived by hand from the Piet specification;
they are not produced by running any interpreter.

Programs are described as a list of commands executed in order:
    "push N"  the current block is N codels wide, then push its size
    "<cmd>"   any other command (the current block is one codel wide)

Usage: gen_cases.py [output-dir]   (default: the cases/ directory next to this file)
"""

import os
import sys

HEX = [
    ["FFC0C0", "FFFFC0", "C0FFC0", "C0FFFF", "C0C0FF", "FFC0FF"],  # light
    ["FF0000", "FFFF00", "00FF00", "00FFFF", "0000FF", "FF00FF"],  # normal
    ["C00000", "C0C000", "00C000", "00C0C0", "0000C0", "C000C0"],  # dark
]
WHITE = "FFFFFF"
BLACK = "000000"

# command -> (hue steps, lightness steps)
STEPS = {
    "push": (0, 1), "pop": (0, 2),
    "add": (1, 0), "sub": (1, 1), "mul": (1, 2),
    "div": (2, 0), "mod": (2, 1), "not": (2, 2),
    "gt": (3, 0), "ptr": (3, 1), "sw": (3, 2),
    "dup": (4, 0), "roll": (4, 1), "inn": (4, 2),
    "inc": (5, 0), "outn": (5, 1), "outc": (5, 2),
}


def step(color, cmd):
    hue, light = color
    dh, dl = STEPS[cmd]
    return ((hue + dh) % 6, (light + dl) % 3)


def hexof(color):
    hue, light = color
    return HEX[light][hue]


def lay_out_row(program):
    """Lay the program out left to right as single-row blocks.

    Returns (cells, color): the hex colours of the row and the colour of the
    block entered by the last command (not included in cells)."""
    color = (0, 0)
    cells = []
    for token in program:
        parts = token.split()
        cmd = parts[0]
        width = int(parts[1]) if cmd == "push" else 1
        if width < 1:
            raise ValueError("push needs a positive block size: " + token)
        cells += [hexof(color)] * width
        color = step(color, cmd)
    return cells, color


def linear(program):
    """A single row of blocks that ends in a block with no way out:

        p p p ... p C
        K K K ... C C
    """
    cells, last = lay_out_row(program)
    x = len(cells)
    row0 = cells + [hexof(last)]
    row1 = [BLACK] * (x - 1) + [hexof(last)] * 2
    return [row0, row1]


def pointer_split(program):
    """The last command of `program` must be "ptr". The block B it enters
    branches on the new DP; afterwards the stack top is printed:

        prefix  B    R    W        DP right/up: B -> R is out(number)
        K...K   D    K    W        DP down:     B -> D is out(char)
        K..K W  W    K    K
    """
    assert program[-1] == "ptr"
    cells, b = lay_out_row(program)
    k = len(cells)
    assert k >= 2
    r = hexof(step(b, "outn"))
    d = hexof(step(b, "outc"))
    row0 = cells + [hexof(b), r, WHITE]
    row1 = [BLACK] * k + [d, BLACK, WHITE]
    row2 = [BLACK] * (k - 1) + [WHITE, WHITE, BLACK, BLACK]
    return [row0, row1, row2]


def switch_split(program):
    """The last command of `program` must be "sw". The 3-codel tall block A it
    enters exits to the right from its top codel (CC left) or its bottom
    codel (CC right); afterwards the stack top is printed:

        prefix  A    X    W        CC left:  A -> X is out(number)
        K...K   A    K    W        CC right: A -> Y is out(char)
        K...K   A    Y    K
        K...K   Y    Y    K
    """
    assert program[-1] == "sw"
    cells, a = lay_out_row(program)
    k = len(cells)
    x = hexof(step(a, "outn"))
    y = hexof(step(a, "outc"))
    ah = hexof(a)
    row0 = cells + [ah, x, WHITE]
    row1 = [BLACK] * k + [ah, BLACK, WHITE]
    row2 = [BLACK] * k + [ah, y, BLACK]
    row3 = [BLACK] * k + [y, y, BLACK]
    return [row0, row1, row2, row3]


# Pushes 65 ('A') using only small blocks.
PUSH_65 = ["push 8", "dup", "mul", "push 1", "add"]
SPACE = ["push 32", "outc"]


def neg(n):
    """Push -n (n >= 1) as 1 - (n + 1)."""
    return ["push 1", "push %d" % (n + 1), "sub"]


def zero():
    return ["push 1", "not"]


CASES = [
    # name, layout, program, expected output, stdin
    ("arith_basic", linear,
     ["push 8", "dup", "mul", "push 1", "add", "dup", "outc",
      "push 1", "add", "outc"],
     b"AB", None),
]


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "cases")
    for name, layout, program, expected, stdin in CASES:
        grid = layout(program)
        with open(os.path.join(out_dir, name + ".txt"), "w") as f:
            for row in grid:
                f.write(" ".join(row) + "\n")
        with open(os.path.join(out_dir, name + ".out"), "wb") as f:
            f.write(expected)
        in_path = os.path.join(out_dir, name + ".in")
        if stdin is not None:
            with open(in_path, "wb") as f:
                f.write(stdin)
        elif os.path.exists(in_path):
            os.remove(in_path)


if __name__ == "__main__":
    main()
