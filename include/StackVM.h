#ifndef STACK_VM_H
#define STACK_VM_H

#ifdef __cplusplus
extern "C" {
#endif

// Input and output for compiled Piet programs. The stack itself is emitted
// inline in the generated code.

// Read an integer from stdin: optional whitespace, an optional sign and at
// least one digit (out-of-range values wrap around). On success, store it in
// *value and return 1. Return 0 if no number could be read; the character that
// stopped the attempt is left unread.
int pietReadNumber(int* value);

// Read one byte from stdin into *value and return 1, or return 0 at end of input.
int pietReadChar(int* value);

// Write a value to stdout as a decimal number.
void pietWriteNumber(int value);

// Write the low byte of a value to stdout.
void pietWriteChar(int value);

#ifdef __cplusplus
}
#endif

#endif // STACK_VM_H
