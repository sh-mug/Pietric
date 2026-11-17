#include "StackVM.h"
#include <cctype>
#include <cstdio>

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
