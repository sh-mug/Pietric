#include "StackVM.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

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

// Return the number of values on the stack.
int stackSize(Stack* stack) {
    if (!stack) return 0;
    return static_cast<int>(stack->data.size());
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

// Input is read with stdio, so flush pending output first: an interactive
// program's prompt should be visible before it waits for input.
int pietReadNumber(int* value) {
    std::fflush(stdout);
    int ch;
    do {
        ch = std::getchar();
    } while (ch != EOF && std::isspace(ch));

    bool negative = false;
    if (ch == '+' || ch == '-') {
        negative = (ch == '-');
        ch = std::getchar();
    }
    if (ch == EOF || !std::isdigit(ch)) {
        if (ch != EOF)
            std::ungetc(ch, stdin);
        return 0;
    }

    unsigned int magnitude = 0;
    while (ch != EOF && std::isdigit(ch)) {
        magnitude = magnitude * 10u + static_cast<unsigned int>(ch - '0');
        ch = std::getchar();
    }
    if (ch != EOF)
        std::ungetc(ch, stdin);
    *value = static_cast<int>(negative ? 0u - magnitude : magnitude);
    return 1;
}

int pietReadChar(int* value) {
    std::fflush(stdout);
    int ch = std::getchar();
    if (ch == EOF)
        return 0;
    *value = ch;
    return 1;
}

void pietWriteNumber(int value) {
    std::printf("%d", value);
}

void pietWriteChar(int value) {
    std::putchar(value);
}
