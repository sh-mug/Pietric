#include "StackVM.h"
#include <algorithm>

// Create a new Stack.
Stack* createStack() {
    return new Stack();
}

// Destroy the Stack.
void destroyStack(Stack* stack) {
    if (stack) {
        delete stack;
    }
}

// Push a value onto the stack.
void stackPush(Stack* stack, int value) {
    if (!stack) return;
    stack->data.push_back(value);
}

// Pop a value from the stack. If empty, returns 0.
int stackPop(Stack* stack) {
    if (!stack || stack->data.empty())
        return 0;
    int value = stack->data.back();
    stack->data.pop_back();
    return value;
}

// Roll the top 'depth' values upward by 'rolls' positions.
// This rotates the top 'depth' items in the stack. If depth is invalid, do nothing.
void stackRoll(Stack* stack, int rolls, int depth) {
    if (!stack) return;
    int size = stack->data.size();
    if (depth <= 0 || depth > size)
        return;
    // Normalize the number of rolls.
    rolls = rolls % depth;
    if (rolls < 0)
        rolls += depth;
    if (rolls == 0)
        return;
    // Identify the portion to roll.
    auto start = stack->data.end() - depth;
    // Perform the rotation.
    std::rotate(start, stack->data.end() - rolls, stack->data.end());
}
