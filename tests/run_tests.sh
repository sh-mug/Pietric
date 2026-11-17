#!/usr/bin/env bash
# End-to-end tests: compile every Piet program in a cases directory with
# Pietric, link it with a C compiler, run it and compare its stdout with the
# expected output.
#
# Usage: run_tests.sh <pietric> <llc> <opt> <cc> <cases-dir> [name-filter]
#
# A test case <name> consists of
#   <name>.txt|.png|.gif|.bmp  the Piet program
#   <name>.out                 the expected stdout (byte for byte)
#   <name>.in                  optional stdin
#
# Each program is built twice: unoptimized (llc -O0) and optimized
# (opt -O2 + llc -O2), so behaviour that depends on undefined behaviour in
# the generated IR tends to show up as a mismatch.

set -u

if [ $# -lt 5 ]; then
    echo "usage: $0 <pietric> <llc> <opt> <cc> <cases-dir> [name-filter]" >&2
    exit 2
fi

# Pietric writes output.ll to its working directory, so it is run from a
# scratch directory; make the file arguments absolute first.
pietric=$(realpath "$1")
llc=$2
opt=$3
cc=$4
cases=$(realpath "$5")
filter=${6:-}

compile_timeout=10
run_timeout=5

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

pass=0
fail=0
failed_names=()

report_fail() {
    fail=$((fail + 1))
    failed_names+=("$1")
    echo "FAIL $1: $2"
}

for expected in "$cases"/*.out; do
    name=$(basename "$expected" .out)
    if [ -n "$filter" ] && [[ "$name" != *"$filter"* ]]; then
        continue
    fi

    src=""
    for ext in txt png gif bmp; do
        if [ -f "$cases/$name.$ext" ]; then
            src="$cases/$name.$ext"
            break
        fi
    done
    if [ -z "$src" ]; then
        report_fail "$name" "no program file found"
        continue
    fi

    input="$cases/$name.in"
    [ -f "$input" ] || input=/dev/null

    dir="$work/$name"
    mkdir -p "$dir"
    if ! (cd "$dir" && timeout "$compile_timeout" "$pietric" "$src" > compile.log 2>&1); then
        report_fail "$name" "Pietric failed or timed out (see output below)"
        cat "$dir/compile.log"
        continue
    fi

    for mode in O0 O2; do
        ll="$dir/output.ll"
        if [ "$mode" = O2 ]; then
            if ! "$opt" -O2 "$dir/output.ll" -S -o "$dir/opt.ll"; then
                report_fail "$name [$mode]" "opt failed"
                continue
            fi
            ll="$dir/opt.ll"
        fi
        if ! "$llc" "-$mode" -relocation-model=pic -filetype=obj "$ll" -o "$dir/prog.o" ||
           ! "$cc" "$dir/prog.o" -o "$dir/prog"; then
            report_fail "$name [$mode]" "llc or link failed"
            continue
        fi
        timeout "$run_timeout" "$dir/prog" < "$input" > "$dir/actual" 2> "$dir/stderr"
        status=$?
        if [ $status -eq 124 ]; then
            report_fail "$name [$mode]" "program timed out"
        elif [ $status -ne 0 ]; then
            report_fail "$name [$mode]" "program exited with status $status"
        elif ! cmp -s "$dir/actual" "$expected"; then
            report_fail "$name [$mode]" "output mismatch"
            echo "  expected: $(od -An -c "$expected" | tr -s ' ' | head -c 200)"
            echo "  actual:   $(od -An -c "$dir/actual" | tr -s ' ' | head -c 200)"
        else
            pass=$((pass + 1))
        fi
    done
done

echo "passed: $pass, failed: $fail"
[ $fail -eq 0 ]
