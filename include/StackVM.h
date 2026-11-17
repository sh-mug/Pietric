#ifndef STACK_VM_H
#define STACK_VM_H

#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

// A simple Stack structure.
struct Stack {
    std::vector<int> data;
};

// Create a new stack and return a pointer to it.
Stack* createStack();

// Destroy a stack.
void destroyStack(Stack* stack);

// Push a value onto the stack.
void stackPush(Stack* stack, int value);

// Pop the top value off the stack and return it.
// If the stack is empty, returns 0.
int stackPop(Stack* stack);

// Roll the top 'depth' values on the stack upward by 'rolls' positions.
// (If rolls is negative, rotate in the opposite direction.)
void stackRoll(Stack* stack, int rolls, int depth);

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
