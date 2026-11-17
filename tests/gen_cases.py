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


def white_trap(program):
    """After the last command the program slides into a white region it can
    never leave, which terminates it:

        prefix  B  W  W
        K...K   K  K  W
    """
    cells, b = lay_out_row(program)
    k = len(cells)
    row0 = cells + [hexof(b), WHITE, WHITE]
    row1 = [BLACK] * (k + 2) + [WHITE]
    return [row0, row1]


def white_turn(program):
    """After the last command the program slides from B through white, turns
    twice at the edges and enters S without executing a command. S -> T is
    out(char); T then slides into a white region with no way out:

        prefix      B  W
        K...K K  K  K  W
        K...K W  K  K  W
        K...K W  T  S  W

    The colour of S is chosen so that executing B -> S would change the
    output (it would be an add)."""
    cells, b = lay_out_row(program)
    k = len(cells)
    assert k >= 2
    s = step(b, "add")
    t = step(s, "outc")
    row0 = cells + [hexof(b), WHITE]
    row1 = [BLACK] * (k + 1) + [WHITE]
    row2 = [BLACK] * (k - 2) + [WHITE, BLACK, BLACK, WHITE]
    row3 = [BLACK] * (k - 2) + [WHITE, hexof(t), hexof(s), WHITE]
    return [row0, row1, row2, row3]


def exit_down(program):
    """The last command enters the 2-codel block A at the right edge. Moving
    right fails twice, so DP turns down with CC right, which selects the
    leftmost codel of A's bottom edge: A -> X is out(char), after which X
    slides into a white region with no way out. (Picking the rightmost codel
    would give A -> Y, a pop.)

        prefix  A  A
        K...K   X  Y
        K..K W  W  K
    """
    cells, a = lay_out_row(program)
    k = len(cells)
    assert k >= 2
    x = hexof(step(a, "outc"))
    y = hexof(step(a, "pop"))
    row0 = cells + [hexof(a)] * 2
    row1 = [BLACK] * k + [x, y]
    row2 = [BLACK] * (k - 1) + [WHITE, WHITE, BLACK]
    return [row0, row1, row2]


def exit_up(program):
    """The last command enters the 2-codel tall block E, which can only be
    left downwards: E -> B is a dup. B can only be left upwards; the first
    attempt is DP up with CC right, which selects the rightmost codel of B's
    top edge: B -> Z is out(char), after which Z slides into a white region
    with no way out. (Picking the leftmost codel would give B -> E, a div.)

        prefix  E  K  W  W
        K...K   E  K  Z  K
        K...K   B  B  B  K
    """
    cells, e = lay_out_row(program)
    k = len(cells)
    b = step(e, "dup")
    z = hexof(step(b, "outc"))
    eh, bh = hexof(e), hexof(b)
    row0 = cells + [eh, BLACK, WHITE, WHITE]
    row1 = [BLACK] * k + [eh, BLACK, z, BLACK]
    row2 = [BLACK] * k + [bh, bh, bh, BLACK]
    return [row0, row1, row2]


# Pushes 65 ('A') using only small blocks.
PUSH_65 = ["push 8", "dup", "mul", "push 1", "add"]
SPACE = ["push 32", "outc"]


def neg(n):
    """Push -n (n >= 1) as 1 - (n + 1)."""
    return ["push 1", "push %d" % (n + 1), "sub"]


def zero():
    return ["push 1", "not"]


# Pushes -2147483648 (2^16 * 2^15, which wraps around in 32 bits).
INT_MIN = ["push 2", "dup", "mul", "dup", "mul", "dup", "mul", "dup", "mul",
           "dup", "push 2", "div", "mul"]


CASES = [
    # name, layout, program, expected output, stdin
    ("arith_basic", linear,
     ["push 8", "dup", "mul", "push 1", "add", "dup", "outc",
      "push 1", "add", "outc"],
     b"AB", None),

    # White regions that cannot be left terminate the program.
    ("white_trap", white_trap, PUSH_65 + ["outc"], b"A", None),
    ("white_turn", white_turn, PUSH_65 + ["dup"], b"A", None),

    # The exit codel of the edge facing DP is chosen by CC.
    ("exit_down", exit_down, PUSH_65, b"A", None),
    ("exit_up", exit_up, PUSH_65, b"A", None),

    # Input and output.
    ("out_number", linear, ["push 3", "outn"] + neg(12) + ["outn"], b"3-12", None),
    ("in_char", linear, ["inc", "inc", "outc", "outc"], b"iH", b"Hi"),
    # 12 + -5 = 7; +7; reading "x" as a number fails and pushes nothing (the 2
    # below it is printed), and "x" is then read as a character; reading a
    # character at end of input pushes nothing (the 3 below it is printed).
    ("in_number", linear,
     ["inn", "inn", "add", "outn", "inn", "outn",
      "push 2", "inn", "outn", "inc", "outc", "push 3", "inc", "outn"],
     b"772x3", b"12 -5\n+7 x"),

    # Commands that need more values than the stack holds are ignored.
    ("underflow_unary", linear,
     ["dup", "not", "pop", "outn", "outc", "push 2", "outn"], b"2", None),
    ("underflow_binary", linear,
     ["push 3", "add", "outn", "push 3", "sub", "outn", "push 3", "mul", "outn",
      "push 3", "div", "outn", "push 3", "mod", "outn", "push 3", "gt", "outn"],
     b"333333", None),
    ("underflow_roll", linear, ["push 2", "roll", "outn"], b"2", None),

    # roll
    ("roll_basic", linear,
     ["push 1", "push 2", "push 3", "push 3", "push 1", "roll", "outn", "outn", "outn"],
     b"213", None),
    ("roll_negative", linear,
     ["push 1", "push 2", "push 3", "push 3"] + neg(1) + ["roll", "outn", "outn", "outn"],
     b"132", None),
    ("roll_many", linear,
     ["push 1", "push 2", "push 3", "push 3", "push 7", "roll", "outn", "outn", "outn"],
     b"213", None),
    ("roll_zero_depth", linear,
     ["push 5"] + zero() + ["push 2", "roll", "outn", "outn"], b"5", None),
    # A depth greater than the rest of the stack or a negative depth is an
    # error: the command is ignored and the arguments stay on the stack.
    ("roll_too_deep", linear,
     ["push 1", "push 2", "push 5", "push 1", "roll", "outn", "outn", "outn", "outn"],
     b"1521", None),
    ("roll_negative_depth", linear,
     ["push 7"] + neg(1) + ["push 1", "roll", "outn", "outn", "outn"], b"1-17", None),

    # Division truncates; mod has the sign of the divisor.
    ("div_trunc", linear,
     ["push 7", "push 2", "div", "outn"] + SPACE +
     neg(7) + ["push 2", "div", "outn"] + SPACE +
     ["push 7"] + neg(2) + ["div", "outn"] + SPACE +
     neg(7) + neg(2) + ["div", "outn"],
     b"3 -3 -3 3", None),
    ("mod_sign", linear,
     ["push 7", "push 3", "mod", "outn"] + SPACE +
     neg(7) + ["push 3", "mod", "outn"] + SPACE +
     ["push 7"] + neg(3) + ["mod", "outn"] + SPACE +
     neg(7) + neg(3) + ["mod", "outn"] + SPACE +
     ["push 6"] + neg(3) + ["mod", "outn"],
     b"1 2 -2 -1 0", None),
    # Division by zero is ignored; the operands stay on the stack.
    ("div_zero", linear, ["push 5"] + zero() + ["div", "outn", "outn"], b"05", None),
    ("mod_zero", linear, ["push 5"] + zero() + ["mod", "outn", "outn"], b"05", None),
    # INT_MIN / -1 wraps around; INT_MIN mod -1 is 0.
    ("div_overflow", linear,
     INT_MIN + ["dup"] + neg(1) + ["div", "outn"] + SPACE + neg(1) + ["mod", "outn"],
     b"-2147483648 0", None),

    # pointer turns DP clockwise by the popped value mod 4 (anticlockwise if
    # negative). Stack below the value: 65; "A" means DP down, "65" right or up.
    ("pointer_1", pointer_split, PUSH_65 + ["push 1", "ptr"], b"A", None),
    ("pointer_5", pointer_split, PUSH_65 + ["push 5", "ptr"], b"A", None),
    ("pointer_neg3", pointer_split, PUSH_65 + neg(3) + ["ptr"], b"A", None),
    ("pointer_neg7", pointer_split, PUSH_65 + neg(7) + ["ptr"], b"A", None),
    ("pointer_4", pointer_split, PUSH_65 + ["push 4", "ptr"], b"65", None),
    ("pointer_neg1", pointer_split, PUSH_65 + neg(1) + ["ptr"], b"65", None),
    ("pointer_neg4", pointer_split, PUSH_65 + neg(4) + ["ptr"], b"65", None),

    # switch toggles CC |value| times. "A" means CC right, "65" CC left.
    ("switch_1", switch_split, PUSH_65 + ["push 1", "sw"], b"A", None),
    ("switch_2", switch_split, PUSH_65 + ["push 2", "sw"], b"65", None),
    ("switch_3", switch_split, PUSH_65 + ["push 3", "sw"], b"A", None),
    ("switch_neg1", switch_split, PUSH_65 + neg(1) + ["sw"], b"A", None),
    ("switch_neg2", switch_split, PUSH_65 + neg(2) + ["sw"], b"65", None),
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
