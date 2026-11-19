# The terminal half of docs/demo.gif; docs/make_demo.sh runs it in a directory with Pietric,
# pietric.png, summod.png (see bench/programs.py) and demo.rc.
Output terminal.gif

Set Shell bash
Set FontSize 20
Set Width 900
Set Height 460
Set Padding 24
Set TypingSpeed 45ms
Set Theme "Builtin Dark"

Hide
Type "source demo.rc && clear"
Enter
Show

Type "./Pietric pietric.png -o pietric"
Sleep 400ms
Enter
Sleep 1s
Type "./pietric"
Sleep 300ms
Enter
Sleep 2s

Type "# sum of c mod 7 for c up to 100,000,000"
Sleep 600ms
Enter
Type "./Pietric summod.png -o summod"
Sleep 400ms
Enter
Sleep 1s
Type "time (echo 100000000 | ./summod)"
Sleep 400ms
Enter
Sleep 4s
