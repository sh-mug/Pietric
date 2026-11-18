#include "IRBuilder.h"
#include "Runtime.h"
#include "llvm/IR/IRBuilder.h"
#include <cassert>
#include <deque>
#include <map>

using namespace llvm;

namespace {

// Stack depths up to this are tracked at compile time, and deeper stacks keep this many values
// at the top in SSA values (see CodeGen).
constexpr int kMaxTrackedDepth = 16;
// Deeper stacks keep up to this many values at the top in SSA values. Every node may get a
// copy for each number of values in this window, also on paths that are never taken (such as
// those where input fails), so a larger window costs compile time: 16 makes small programs take
// about 1.6 times as long to compile, while 4 is too small for the loop of a typical program.
constexpr int kWindowSize = 8;
// How many values a full window moves to the heap at once.
constexpr int kEvictCount = kWindowSize / 2;
static_assert(kWindowSize <= kMaxTrackedDepth, "the window lives in the depth-tracked slots");
// Upper bound on the number of depth-tracked and cached node copies, which keeps the code size
// of large programs in check. Nodes reached beyond it are emitted generically.
constexpr size_t kMaxTrackedCopies = 16384;
// Number of values the heap stack has room for before it first has to grow.
constexpr int64_t kInitialStackCapacity = 1024;
static_assert(kInitialStackCapacity > kMaxTrackedDepth, "spilling must not need to grow");

// The kinds of node copies, by how they keep the stack.
enum class Mode {
    Tracked,  // all values in SSA values; the depth is known at compile time
    Cached,   // the top values (the window) in SSA values, the ones below on the heap
    Generic,  // all values on the heap
};

// Copies are keyed by node and by kind: a depth-tracked copy by its depth, a cached one by
// kCachedKey plus the number of values in its window, and a generic one by kGenericKey.
constexpr int kCachedKey = 1000;
constexpr int kGenericKey = -1;

// Emits main() for an execution graph.
//
// Every graph node is emitted once for each stack depth it can be reached with, up to
// kMaxTrackedDepth. In these copies the depth is a compile-time constant, so each stack position
// is a separate alloca, which mem2reg turns into SSA values: underflow checks disappear, and the
// optimizer can follow every value from push to pop, through roll and branches alike, and fold
// whatever does not depend on input. Commands whose effect on the depth depends on values
// (division by zero, an invalid roll, a failed read) branch to the copies for each outcome.
//
// When the depth would exceed kMaxTrackedDepth, the bottom values are moved to a heap buffer that
// doubles in size when full, and execution continues in cached copies of the nodes. These keep
// up to kWindowSize values at the top, the window, in the same allocas as depth-tracked copies,
// with the number of values in the window known at compile time; only the number of values on
// the heap is kept at run time. Commands work on the window as in depth-tracked copies; a full
// window moves its bottom kEvictCount values to the heap, a command that needs more values than
// the window holds first checks the heap and moves the values it needs back, and a roll deeper
// than the window moves the whole window to the heap and rolls there.
//
// Past kMaxTrackedCopies, execution continues in generic copies, which keep every value on the
// heap and emit push and pop inline.
class CodeGen {
public:
    CodeGen(LLVMContext &ctx, Module &module);
    void emit(const Graph &graph);

private:
    LLVMContext &ctx;
    Module &module;
    IRBuilder<> builder;
    IntegerType *i32, *i64;
    PointerType *ptr;
    Runtime runtime;
    Function *mainFunc = nullptr;

    // Depth-tracked stack, and window of the cached stack: slots[i] holds the value at
    // position i (0 is the bottom, or the value just above the heap ones).
    std::vector<AllocaInst *> slots;
    // Heap stack: buffer[0..size) holds the values below the window, buffer[size - 1] being the
    // highest of them.
    AllocaInst *sizeSlot = nullptr;      // i64
    AllocaInst *capacitySlot = nullptr;  // i64: how many values the buffer has room for
    AllocaInst *bufferSlot = nullptr;    // ptr

    // How the code being emitted keeps the stack, and the number of values in the slots (the
    // depth of a depth-tracked stack, or the size of the window; 0 in generic copies).
    Mode mode = Mode::Tracked;
    int depth = 0;
    struct State {
        Mode mode;
        int depth;
    };
    State state() const { return { mode, depth }; }
    void restore(State saved) { mode = saved.mode; depth = saved.depth; }
    int stateKey() const;

    // Node copies, keyed by (node index, state key), and the ones still to be emitted.
    std::map<std::pair<int, int>, BasicBlock *> copies;
    std::deque<std::pair<int, int>> pending;
    size_t trackedCopies = 0;

    ConstantInt *constant(int value) { return ConstantInt::get(i32, value); }
    ConstantInt *index(int64_t value) { return ConstantInt::get(i64, value); }
    bool tracked() const { return mode == Mode::Tracked; }
    bool cached() const { return mode == Mode::Cached; }
    bool generic() const { return mode == Mode::Generic; }

    // Returns the block of the copy of `node` for the state `key`, creating it if needed.
    BasicBlock *copyOf(int node, int key);
    // Ends the current block with a branch to the copy of `node` for the current state. If that
    // would need a depth-tracked or cached copy beyond kMaxTrackedCopies, moves the stack to
    // the heap first and continues generically.
    void jump(int node);

    // --- Stack operations, for every kind of copy ---
    // The number of values on the stack, as an i64.
    Value *stackSize();
    // Pops the top value. The stack must not be empty, and in a cached copy the window must
    // not be either (emitIfStackHas sees to that).
    Value *pop();
    void push(Value *value);
    // Moves the bottom values of a full depth-tracked stack to a new heap buffer and continues
    // with the top ones as the window, or with the whole stack on the heap if !keepWindow.
    void spill(bool keepWindow);
    // Moves the bottom `count` values of the slots to the top of the heap stack.
    void moveToHeap(int count);
    // Moves `count` values from the top of the heap stack, which must hold them, to the bottom
    // of the window.
    void refill(int count);
    // Makes room in the heap buffer for `count` values above `size`.
    void reserve(Value *size, int count);
    // Rolls the top rollDepth values of the heap stack (the window must be empty).
    void emitHeapRoll(Value *rollDepth, Value *rolls);
    Value *stackBuffer() { return builder.CreateLoad(ptr, bufferSlot, "buffer"); }

    // Emits `if (cond) then(); else otherwise();`. Both must end the block they emit into (as
    // they may leave the stack at different depths, they cannot join).
    template <typename T, typename E> void emitBranch(Value *cond, T then, E otherwise);
    // Emits `if (cond) then();` and continues after it. `then` must not change the depth.
    template <typename F> void emitIf(Value *cond, F then);
    // Emits `then()` if the stack holds at least `count` values, and `otherwise()` if not.
    template <typename T, typename E> void emitIfStackHas(int count, T then, E otherwise);

    // Emits the command executed when leaving a node of `blockSize` codels, followed by the
    // branch to `next`.
    void emitCommand(Command cmd, int blockSize, int next);
    // Emits a command that pops two values and pushes op(second, top).
    template <typename F> void emitBinary(F op, int next);
    void emitDivMod(Command cmd, int next);
    void emitRoll(int next);
    // Rolls the top `rollDepth` values (0 <= rollDepth <= depth) of the depth-tracked stack.
    void emitTrackedRoll(Value *rollDepth, Value *rolls);
    // Emits an input command: calls `readF` and pushes the value if one was read.
    void emitInput(Function *readF, int next);
    // Emits a pointer or switch command, which picks the successor.
    void emitPointerSwitch(const GraphNode &node);
    void emitNode(const GraphNode &node);
};

CodeGen::CodeGen(LLVMContext &ctx, Module &module)
    : ctx(ctx), module(module), builder(ctx), i32(builder.getInt32Ty()),
      i64(builder.getInt64Ty()), ptr(builder.getPtrTy()), runtime(defineRuntime(module)) { }

int CodeGen::stateKey() const {
    switch (mode) {
        case Mode::Tracked: return depth;
        case Mode::Cached: return kCachedKey + depth;
        case Mode::Generic: return kGenericKey;
    }
    return kGenericKey;
}

BasicBlock *CodeGen::copyOf(int node, int stateKey) {
    auto key = std::make_pair(node, stateKey);
    auto it = copies.find(key);
    if (it != copies.end())
        return it->second;
    std::string name = "node" + std::to_string(node) +
                       (stateKey == kGenericKey ? ".g"
                        : stateKey >= kCachedKey ? ".c" + std::to_string(stateKey - kCachedKey)
                                                 : ".d" + std::to_string(stateKey));
    BasicBlock *bb = BasicBlock::Create(ctx, name, mainFunc);
    copies.emplace(key, bb);
    pending.push_back(key);
    if (stateKey != kGenericKey)
        trackedCopies++;
    return bb;
}

void CodeGen::jump(int node) {
    if (!generic() && !copies.count({ node, stateKey() }) && trackedCopies >= kMaxTrackedCopies) {
        if (tracked()) {
            spill(false);
        } else {
            moveToHeap(depth);
            mode = Mode::Generic;
        }
    }
    builder.CreateBr(copyOf(node, stateKey()));
}

Value *CodeGen::stackSize() {
    if (tracked())
        return index(depth);
    Value *heapSize = builder.CreateLoad(i64, sizeSlot, "size");
    return depth == 0 ? heapSize : builder.CreateNUWAdd(heapSize, index(depth));
}

Value *CodeGen::pop() {
    if (!generic()) {
        assert(depth > 0);
        depth--;
        return builder.CreateLoad(i32, slots[depth]);
    }
    // The callers check the size first, so it cannot wrap.
    Value *newSize = builder.CreateNUWSub(stackSize(), index(1));
    builder.CreateStore(newSize, sizeSlot);
    return builder.CreateLoad(i32, builder.CreateInBoundsGEP(i32, stackBuffer(), newSize));
}

void CodeGen::push(Value *value) {
    if (tracked() && depth == kMaxTrackedDepth)
        spill(true);
    else if (cached() && depth == kWindowSize)
        moveToHeap(kEvictCount);
    if (!generic()) {
        builder.CreateStore(value, slots[depth]);
        depth++;
        return;
    }
    Value *size = stackSize();
    reserve(size, 1);
    builder.CreateStore(value, builder.CreateInBoundsGEP(i32, stackBuffer(), size));
    builder.CreateStore(builder.CreateNUWAdd(size, index(1)), sizeSlot);
}

void CodeGen::reserve(Value *size, int count) {
    Value *capacity = builder.CreateLoad(i64, capacitySlot, "capacity");
    // The capacity is at least kInitialStackCapacity > count, so doubling it is enough.
    emitIf(builder.CreateICmpUGT(builder.CreateNUWAdd(size, index(count)), capacity), [&] {
        Value *newCapacity = builder.CreateNUWMul(capacity, index(2));
        builder.CreateStore(builder.CreateCall(runtime.grow, { stackBuffer(), newCapacity }),
                            bufferSlot);
        builder.CreateStore(newCapacity, capacitySlot);
    });
}

void CodeGen::spill(bool keepWindow) {
    assert(tracked());
    Value *buffer = builder.CreateCall(
        runtime.malloc, { index(kInitialStackCapacity * sizeof(int32_t)) }, "buffer");
    emitIf(builder.CreateIsNull(buffer), [&] { builder.CreateCall(runtime.outOfMemory); });
    builder.CreateStore(index(0), sizeSlot);
    builder.CreateStore(index(kInitialStackCapacity), capacitySlot);
    builder.CreateStore(buffer, bufferSlot);
    mode = Mode::Cached;
    if (keepWindow) {
        moveToHeap(depth - (kWindowSize - kEvictCount));
    } else {
        moveToHeap(depth);
        mode = Mode::Generic;
    }
}

void CodeGen::moveToHeap(int count) {
    assert(!generic() && count <= depth);
    if (count == 0)
        return;
    Value *size = builder.CreateLoad(i64, sizeSlot, "size");
    reserve(size, count);
    Value *buffer = stackBuffer();
    for (int i = 0; i < count; i++)
        builder.CreateStore(builder.CreateLoad(i32, slots[i]),
                            builder.CreateInBoundsGEP(i32, buffer,
                                                      builder.CreateNUWAdd(size, index(i))));
    for (int i = count; i < depth; i++)
        builder.CreateStore(builder.CreateLoad(i32, slots[i]), slots[i - count]);
    builder.CreateStore(builder.CreateNUWAdd(size, index(count)), sizeSlot);
    depth -= count;
}

void CodeGen::refill(int count) {
    assert(cached() && depth + count <= kWindowSize);
    for (int i = depth - 1; i >= 0; i--)
        builder.CreateStore(builder.CreateLoad(i32, slots[i]), slots[i + count]);
    Value *size = builder.CreateNUWSub(builder.CreateLoad(i64, sizeSlot, "size"), index(count));
    Value *buffer = stackBuffer();
    for (int i = 0; i < count; i++)
        builder.CreateStore(
            builder.CreateLoad(i32, builder.CreateInBoundsGEP(
                                        i32, buffer, builder.CreateNUWAdd(size, index(i)))),
            slots[i]);
    builder.CreateStore(size, sizeSlot);
    depth += count;
}

template <typename T, typename E> void CodeGen::emitBranch(Value *cond, T then, E otherwise) {
    BasicBlock *thenBB = BasicBlock::Create(ctx, "then", mainFunc);
    BasicBlock *elseBB = BasicBlock::Create(ctx, "else", mainFunc);
    builder.CreateCondBr(cond, thenBB, elseBB);
    State saved = state();
    builder.SetInsertPoint(thenBB);
    then();
    restore(saved);
    builder.SetInsertPoint(elseBB);
    otherwise();
    restore(saved);
}

template <typename F> void CodeGen::emitIf(Value *cond, F then) {
    BasicBlock *thenBB = BasicBlock::Create(ctx, "then", mainFunc);
    BasicBlock *contBB = BasicBlock::Create(ctx, "cont", mainFunc);
    builder.CreateCondBr(cond, thenBB, contBB);
    builder.SetInsertPoint(thenBB);
    then();
    builder.CreateBr(contBB);
    builder.SetInsertPoint(contBB);
}

// A command that needs more values than the stack holds is ignored.
template <typename T, typename E> void CodeGen::emitIfStackHas(int count, T then, E otherwise) {
    if (tracked() || (cached() && depth >= count)) {
        if (depth >= count)
            then();
        else
            otherwise();
        return;
    }
    // Check the heap; a cached copy then brings the values it lacks into the window.
    emitBranch(builder.CreateICmpSGE(stackSize(), index(count)),
        [&] {
            if (cached())
                refill(count - depth);
            then();
        },
        otherwise);
}

template <typename F> void CodeGen::emitBinary(F op, int next) {
    emitIfStackHas(2,
        [&] {
            Value *top = pop();
            Value *second = pop();
            push(op(second, top));
            jump(next);
        },
        [&] { jump(next); });
}

void CodeGen::emitDivMod(Command cmd, int next) {
    emitIfStackHas(2,
        [&] {
            Value *divisor = pop();
            Value *dividend = pop();
            // Division by zero is an implementation-defined error; as the specification
            // recommends, the command is ignored and the operands stay on the stack.
            emitBranch(builder.CreateICmpEQ(divisor, constant(0)),
                [&] {
                    push(dividend);
                    push(divisor);
                    jump(next);
                },
                [&] {
                    // sdiv/srem of INT_MIN by -1 is undefined behaviour in LLVM IR, so divide
                    // by 1 instead and fix up the result: x / -1 is -x (wrapping) and x mod -1
                    // is 0.
                    Value *isMinusOne = builder.CreateICmpEQ(divisor, constant(-1));
                    Value *safeDivisor = builder.CreateSelect(isMinusOne, constant(1), divisor);
                    if (cmd == Command::Divide) {
                        // Integer division truncates towards zero.
                        Value *quotient = builder.CreateSDiv(dividend, safeDivisor);
                        push(builder.CreateSelect(isMinusOne, builder.CreateNeg(dividend),
                                                  quotient));
                    } else {
                        // srem takes the sign of the dividend, but the result of mod has the
                        // sign of the divisor: add the divisor when the signs differ.
                        Value *rem = builder.CreateSRem(dividend, safeDivisor);
                        Value *signsDiffer = builder.CreateICmpSLT(
                            builder.CreateXor(rem, divisor), constant(0));
                        Value *adjust = builder.CreateAnd(
                            builder.CreateICmpNE(rem, constant(0)), signsDiffer);
                        push(builder.CreateSelect(adjust, builder.CreateAdd(rem, divisor), rem));
                    }
                    jump(next);
                });
        },
        [&] { jump(next); });
}

void CodeGen::emitRoll(int next) {
    emitIfStackHas(2,
        [&] {
            Value *rolls = pop();
            Value *rollDepth = pop();
            // A negative depth, or one greater than the number of remaining values, cannot
            // be rolled: the command is ignored, so the arguments go back on the stack.
            Value *size = stackSize();
            Value *wideDepth = builder.CreateSExt(rollDepth, i64);
            Value *valid = builder.CreateAnd(builder.CreateICmpSGE(rollDepth, constant(0)),
                                             builder.CreateICmpSLE(wideDepth, size));
            emitBranch(valid,
                [&] {
                    if (cached()) {
                        // Roll within the window if it holds the values to roll, else on the
                        // heap.
                        emitBranch(builder.CreateICmpSLE(rollDepth, constant(depth)),
                            [&] {
                                emitTrackedRoll(rollDepth, rolls);
                                jump(next);
                            },
                            [&] {
                                moveToHeap(depth);
                                emitHeapRoll(rollDepth, rolls);
                                jump(next);
                            });
                        return;
                    }
                    if (generic())
                        emitHeapRoll(rollDepth, rolls);
                    else
                        emitTrackedRoll(rollDepth, rolls);
                    jump(next);
                },
                [&] {
                    push(rollDepth);
                    push(rolls);
                    jump(next);
                });
        },
        [&] { jump(next); });
}

void CodeGen::emitHeapRoll(Value *rollDepth, Value *rolls) {
    Value *size = builder.CreateLoad(i64, sizeSlot, "size");
    Value *base = builder.CreateInBoundsGEP(
        i32, stackBuffer(), builder.CreateSub(size, builder.CreateSExt(rollDepth, i64)));
    builder.CreateCall(runtime.roll, { base, rollDepth, rolls });
}

// The values stay in SSA form: every slot gets a select between the old values, picked by
// arithmetic on rollDepth and rolls, which folds away once those are known.
void CodeGen::emitTrackedRoll(Value *rollDepth, Value *rolls) {
    if (depth < 2)
        return;
    std::vector<Value *> old;
    for (int i = 0; i < depth; i++)
        old.push_back(builder.CreateLoad(i32, slots[i]));

    // A roll of depth n by r moves every value r places towards the top, wrapping round, so
    // only r mod n (rounded towards negative infinity, in 0..n-1) matters. Depths 0 and 1
    // change nothing; dividing by 1 instead gives r = 0 for them.
    Value *n = builder.CreateSelect(builder.CreateICmpSGT(rollDepth, constant(1)), rollDepth,
                                    constant(1));
    Value *rem = builder.CreateSRem(rolls, n);
    Value *r = builder.CreateSelect(builder.CreateICmpSLT(rem, constant(0)),
                                    builder.CreateAdd(rem, n), rem);
    Value *base = builder.CreateSub(constant(depth), rollDepth);  // first rolled position

    std::vector<Value *> rolled;
    for (int i = 0; i < depth; i++) {
        // Position i receives the value from position base + ((i - base - r) mod n), if it is
        // among the rolled values.
        Value *offset = builder.CreateSub(constant(i), base);
        Value *from = builder.CreateSub(offset, r);
        from = builder.CreateSelect(builder.CreateICmpSLT(from, constant(0)),
                                    builder.CreateAdd(from, n), from);
        Value *source = builder.CreateSelect(builder.CreateICmpSGE(offset, constant(0)),
                                             builder.CreateAdd(base, from), constant(i));
        Value *value = old[depth - 1];
        for (int j = depth - 2; j >= 0; j--)
            value = builder.CreateSelect(builder.CreateICmpEQ(source, constant(j)), old[j], value);
        rolled.push_back(value);
    }
    for (int i = 0; i < depth; i++)
        builder.CreateStore(rolled[i], slots[i]);
}

void CodeGen::emitInput(Function *readF, int next) {
    // If nothing can be read (e.g. at end of input), the command is ignored.
    Value *result = builder.CreateCall(readF);
    emitBranch(builder.CreateExtractValue(result, 0),
        [&] {
            push(builder.CreateExtractValue(result, 1));
            jump(next);
        },
        [&] { jump(next); });
}

void CodeGen::emitCommand(Command cmd, int blockSize, int next) {
    auto proceed = [&] { jump(next); };
    switch (cmd) {
        case Command::Push:
            push(constant(blockSize));
            jump(next);
            break;
        case Command::Pop:
            emitIfStackHas(1, [&] { pop(); jump(next); }, proceed);
            break;
        case Command::Add:
            emitBinary([&](Value *a, Value *b) { return builder.CreateAdd(a, b); }, next);
            break;
        case Command::Subtract:
            emitBinary([&](Value *a, Value *b) { return builder.CreateSub(a, b); }, next);
            break;
        case Command::Multiply:
            emitBinary([&](Value *a, Value *b) { return builder.CreateMul(a, b); }, next);
            break;
        case Command::Divide:
        case Command::Modulo:
            emitDivMod(cmd, next);
            break;
        case Command::Not:
            emitIfStackHas(1,
                [&] {
                    Value *isZero = builder.CreateICmpEQ(pop(), constant(0));
                    push(builder.CreateZExt(isZero, i32));
                    jump(next);
                },
                proceed);
            break;
        case Command::Greater:
            emitBinary([&](Value *a, Value *b) {
                return builder.CreateZExt(builder.CreateICmpSGT(a, b), i32);
            }, next);
            break;
        case Command::Duplicate:
            emitIfStackHas(1,
                [&] {
                    Value *top = pop();
                    push(top);
                    push(top);
                    jump(next);
                },
                proceed);
            break;
        case Command::Roll:
            emitRoll(next);
            break;
        case Command::InputNum:
            emitInput(runtime.readNumber, next);
            break;
        case Command::InputChar:
            emitInput(runtime.readChar, next);
            break;
        case Command::OutputNum:
            emitIfStackHas(1,
                [&] {
                    builder.CreateCall(runtime.writeNumber, { pop() });
                    jump(next);
                },
                proceed);
            break;
        case Command::OutputChar:
            emitIfStackHas(1,
                [&] {
                    builder.CreateCall(runtime.writeChar, { pop() });
                    jump(next);
                },
                proceed);
            break;
        case Command::None:
        case Command::Pointer:
        case Command::Switch:
            // Pointer and switch are emitted by emitPointerSwitch.
            jump(next);
            break;
    }
}

// The graph gives a pointer node four successors, transitions[i] being the state after turning the
// DP clockwise i times, and a switch node two, transitions[i] being the state after toggling the
// CC i times.
void CodeGen::emitPointerSwitch(const GraphNode &node) {
    Command cmd = node.transitions[0].command;
    int numEdges = node.transitions.size();
    assert(numEdges == (cmd == Command::Pointer ? 4 : 2));

    // With an empty stack the command is ignored and DP and CC stay as they are.
    emitIfStackHas(1,
        [&] {
            Value *value = pop();
            Value *index;
            if (cmd == Command::Pointer) {
                // Turn clockwise `value` times, or anticlockwise if negative: -1 is the same
                // as 3, so the index is value mod 4 rounded towards negative infinity, in 0..3.
                Value *rem = builder.CreateSRem(value, constant(4));
                Value *isNegative = builder.CreateICmpSLT(rem, constant(0));
                index = builder.CreateSelect(isNegative, builder.CreateAdd(rem, constant(4)), rem);
            } else {
                // Toggle |value| times: only whether that is odd matters.
                Value *rem = builder.CreateSRem(value, constant(2));
                index = builder.CreateZExt(builder.CreateICmpNE(rem, constant(0)), i32);
            }
            // Each successor gets its own block, so that a copy whose depth-tracked budget
            // runs out can spill on the way.
            BasicBlock *switchBB = builder.GetInsertBlock();
            std::vector<BasicBlock *> targets;
            State saved = state();
            for (int j = 0; j < numEdges; j++) {
                targets.push_back(BasicBlock::Create(ctx, "case", mainFunc));
                builder.SetInsertPoint(targets[j]);
                jump(node.transitions[j].targetNode);
                restore(saved);
            }
            builder.SetInsertPoint(switchBB);
            SwitchInst *swInst = builder.CreateSwitch(index, targets[0], numEdges - 1);
            for (int j = 1; j < numEdges; j++)
                swInst->addCase(constant(j), targets[j]);
        },
        [&] { jump(node.transitions[0].targetNode); });
}

void CodeGen::emitNode(const GraphNode &node) {
    if (node.transitions.empty()) {
        // Terminal state: write out the output, free the heap stack, if any, and return.
        builder.CreateCall(runtime.flush);
        if (!tracked())
            builder.CreateCall(runtime.free, { stackBuffer() });
        builder.CreateRet(constant(0));
    } else if (node.transitions[0].command == Command::Pointer ||
               node.transitions[0].command == Command::Switch) {
        emitPointerSwitch(node);
    } else {
        // Single transition: execute the command, then go to the sole successor.
        emitCommand(node.transitions[0].command, node.blockSize, node.transitions[0].targetNode);
    }
}

void CodeGen::emit(const Graph &graph) {
    // Create main function: int main()
    mainFunc = Function::Create(FunctionType::get(i32, false), Function::ExternalLinkage,
                                "main", module);
    BasicBlock *entryBB = BasicBlock::Create(ctx, "entry", mainFunc);
    builder.SetInsertPoint(entryBB);

    const std::vector<GraphNode> &nodes = graph.getNodes();
    if (nodes.empty()) {
        builder.CreateRet(constant(0));
        return;
    }

    for (int i = 0; i < kMaxTrackedDepth; i++)
        slots.push_back(builder.CreateAlloca(i32, nullptr, "slot" + std::to_string(i)));
    sizeSlot = builder.CreateAlloca(i64, nullptr, "size.slot");
    capacitySlot = builder.CreateAlloca(i64, nullptr, "capacity.slot");
    bufferSlot = builder.CreateAlloca(ptr, nullptr, "buffer.slot");

    builder.CreateCall(runtime.start);
    // Execution starts at node 0 with an empty stack.
    restore({ Mode::Tracked, 0 });
    jump(0);

    while (!pending.empty()) {
        auto [nodeIndex, key] = pending.front();
        pending.pop_front();
        builder.SetInsertPoint(copies[{ nodeIndex, key }]);
        if (key == kGenericKey)
            restore({ Mode::Generic, 0 });
        else if (key >= kCachedKey)
            restore({ Mode::Cached, key - kCachedKey });
        else
            restore({ Mode::Tracked, key });
        emitNode(nodes[nodeIndex]);
    }
}

} // namespace

IRGenerator::IRGenerator(LLVMContext &ctx) : context(ctx) { }

Module* IRGenerator::generateModule(const Graph &graph) {
    Module *module = new Module("PietModule", context);
    CodeGen codegen(context, *module);
    codegen.emit(graph);
    return module;
}
