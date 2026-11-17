#include "IRBuilder.h"
#include "Runtime.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <deque>
#include <map>

using namespace llvm;

namespace {

// Stack depths up to this are tracked at compile time (see CodeGen).
constexpr int kMaxTrackedDepth = 16;
// Upper bound on the number of depth-tracked node copies, which keeps the code size of large
// programs in check. Nodes reached beyond it are emitted generically.
constexpr size_t kMaxTrackedCopies = 16384;
// Number of values the heap stack has room for before it first has to grow.
constexpr int64_t kInitialStackCapacity = 1024;
static_assert(kInitialStackCapacity > kMaxTrackedDepth, "spilling must not need to grow");

// The depth of code that keeps the stack depth at run time.
constexpr int kGeneric = -1;

// Emits main() for an execution graph.
//
// Every graph node is emitted once for each stack depth it can be reached with, up to
// kMaxTrackedDepth. In these copies the depth is a compile-time constant, so each stack position
// is a separate alloca, which mem2reg turns into SSA values: underflow checks disappear, and the
// optimizer can follow every value from push to pop, through roll and branches alike, and fold
// whatever does not depend on input. Commands whose effect on the depth depends on values
// (division by zero, an invalid roll, a failed read) branch to the copies for each outcome.
//
// When the depth would exceed kMaxTrackedDepth, the values are moved to a heap buffer that
// doubles in size when full, and execution continues in generic copies of the nodes, which keep
// the depth, the capacity and the buffer pointer in allocas and emit push and pop inline.
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
    Value *inputSlot = nullptr;  // receives values read by the input commands

    // Depth-tracked stack: slots[i] holds the value at position i (0 is the bottom).
    std::vector<AllocaInst *> slots;
    // Generic stack: buffer[0..size) holds the values, buffer[size - 1] being the top.
    AllocaInst *sizeSlot = nullptr;      // i64
    AllocaInst *capacitySlot = nullptr;  // i64: how many values the buffer has room for
    AllocaInst *bufferSlot = nullptr;    // ptr

    // The depth of the stack at the code being emitted, or kGeneric.
    int depth = 0;

    // Node copies, keyed by (node index, depth), and the ones still to be emitted.
    std::map<std::pair<int, int>, BasicBlock *> copies;
    std::deque<std::pair<int, int>> pending;
    size_t trackedCopies = 0;

    // Runtime library functions.
    FunctionCallee readNumberF, readCharF, writeNumberF, writeCharF;

    ConstantInt *constant(int value) { return ConstantInt::get(i32, value); }
    ConstantInt *index(int64_t value) { return ConstantInt::get(i64, value); }
    bool generic() const { return depth == kGeneric; }

    // Returns the block of the copy of `node` for `depth`, creating it if needed.
    BasicBlock *copyOf(int node, int depth);
    // Ends the current block with a branch to the copy of `node` for the current depth. If that
    // would need a depth-tracked copy beyond kMaxTrackedCopies, spills first.
    void jump(int node);

    // --- Stack operations, for either kind of copy ---
    // The number of values on the stack, as an i64.
    Value *stackSize();
    // Pops the top value. The stack must not be empty.
    Value *pop();
    void push(Value *value);
    // Moves the depth-tracked stack to the heap and continues generically.
    void spill();
    // Generic stack helpers.
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
    void emitInput(FunctionCallee readF, int next);
    // Emits a pointer or switch command, which picks the successor.
    void emitPointerSwitch(const GraphNode &node);
    void emitNode(const GraphNode &node);
};

CodeGen::CodeGen(LLVMContext &ctx, Module &module)
    : ctx(ctx), module(module), builder(ctx), i32(builder.getInt32Ty()),
      i64(builder.getInt64Ty()), ptr(builder.getPtrTy()), runtime(defineRuntime(module)) {
    Type *voidTy = Type::getVoidTy(ctx);
    readNumberF = module.getOrInsertFunction(
        "pietReadNumber", FunctionType::get(i32, { ptr }, false));
    readCharF = module.getOrInsertFunction(
        "pietReadChar", FunctionType::get(i32, { ptr }, false));
    writeNumberF = module.getOrInsertFunction(
        "pietWriteNumber", FunctionType::get(voidTy, { i32 }, false));
    writeCharF = module.getOrInsertFunction(
        "pietWriteChar", FunctionType::get(voidTy, { i32 }, false));
}

BasicBlock *CodeGen::copyOf(int node, int copyDepth) {
    auto key = std::make_pair(node, copyDepth);
    auto it = copies.find(key);
    if (it != copies.end())
        return it->second;
    std::string name = "node" + std::to_string(node) +
                       (copyDepth == kGeneric ? ".g" : ".d" + std::to_string(copyDepth));
    BasicBlock *bb = BasicBlock::Create(ctx, name, mainFunc);
    copies.emplace(key, bb);
    pending.push_back(key);
    if (copyDepth != kGeneric)
        trackedCopies++;
    return bb;
}

void CodeGen::jump(int node) {
    if (!generic() && !copies.count({ node, depth }) && trackedCopies >= kMaxTrackedCopies)
        spill();
    builder.CreateBr(copyOf(node, depth));
}

Value *CodeGen::stackSize() {
    return generic() ? static_cast<Value *>(builder.CreateLoad(i64, sizeSlot, "size"))
                     : index(depth);
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
    if (!generic() && depth == kMaxTrackedDepth)
        spill();
    if (!generic()) {
        builder.CreateStore(value, slots[depth]);
        depth++;
        return;
    }
    Value *size = stackSize();
    Value *capacity = builder.CreateLoad(i64, capacitySlot, "capacity");
    emitIf(builder.CreateICmpEQ(size, capacity), [&] {
        Value *newCapacity = builder.CreateNUWMul(capacity, index(2));
        builder.CreateStore(builder.CreateCall(runtime.grow, { stackBuffer(), newCapacity }),
                            bufferSlot);
        builder.CreateStore(newCapacity, capacitySlot);
    });
    builder.CreateStore(value, builder.CreateInBoundsGEP(i32, stackBuffer(), size));
    builder.CreateStore(builder.CreateNUWAdd(size, index(1)), sizeSlot);
}

void CodeGen::spill() {
    assert(!generic());
    Value *buffer = builder.CreateCall(
        runtime.malloc, { index(kInitialStackCapacity * sizeof(int32_t)) }, "buffer");
    emitIf(builder.CreateIsNull(buffer), [&] { builder.CreateCall(runtime.outOfMemory); });
    for (int i = 0; i < depth; i++)
        builder.CreateStore(builder.CreateLoad(i32, slots[i]),
                            builder.CreateConstInBoundsGEP1_64(i32, buffer, i));
    builder.CreateStore(index(depth), sizeSlot);
    builder.CreateStore(index(kInitialStackCapacity), capacitySlot);
    builder.CreateStore(buffer, bufferSlot);
    depth = kGeneric;
}

template <typename T, typename E> void CodeGen::emitBranch(Value *cond, T then, E otherwise) {
    BasicBlock *thenBB = BasicBlock::Create(ctx, "then", mainFunc);
    BasicBlock *elseBB = BasicBlock::Create(ctx, "else", mainFunc);
    builder.CreateCondBr(cond, thenBB, elseBB);
    int saved = depth;
    builder.SetInsertPoint(thenBB);
    then();
    depth = saved;
    builder.SetInsertPoint(elseBB);
    otherwise();
    depth = saved;
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
    if (!generic()) {
        if (depth >= count)
            then();
        else
            otherwise();
        return;
    }
    emitBranch(builder.CreateICmpSGE(stackSize(), index(count)), then, otherwise);
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
                    if (generic()) {
                        Value *base = builder.CreateInBoundsGEP(
                            i32, stackBuffer(), builder.CreateSub(size, wideDepth));
                        builder.CreateCall(runtime.roll, { base, rollDepth, rolls });
                    } else {
                        emitTrackedRoll(rollDepth, rolls);
                    }
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

void CodeGen::emitInput(FunctionCallee readF, int next) {
    // If nothing can be read (e.g. at end of input), the command is ignored.
    Value *ok = builder.CreateCall(readF, { inputSlot });
    emitBranch(builder.CreateICmpNE(ok, constant(0)),
        [&] {
            push(builder.CreateLoad(i32, inputSlot));
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
            emitInput(readNumberF, next);
            break;
        case Command::InputChar:
            emitInput(readCharF, next);
            break;
        case Command::OutputNum:
            emitIfStackHas(1,
                [&] {
                    builder.CreateCall(writeNumberF, { pop() });
                    jump(next);
                },
                proceed);
            break;
        case Command::OutputChar:
            emitIfStackHas(1,
                [&] {
                    builder.CreateCall(writeCharF, { pop() });
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
            int saved = depth;
            for (int j = 0; j < numEdges; j++) {
                targets.push_back(BasicBlock::Create(ctx, "case", mainFunc));
                builder.SetInsertPoint(targets[j]);
                jump(node.transitions[j].targetNode);
                depth = saved;
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
        // Terminal state: free the heap stack, if any, and return.
        if (generic())
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

    inputSlot = builder.CreateAlloca(i32, nullptr, "input");
    for (int i = 0; i < kMaxTrackedDepth; i++)
        slots.push_back(builder.CreateAlloca(i32, nullptr, "slot" + std::to_string(i)));
    sizeSlot = builder.CreateAlloca(i64, nullptr, "size.slot");
    capacitySlot = builder.CreateAlloca(i64, nullptr, "capacity.slot");
    bufferSlot = builder.CreateAlloca(ptr, nullptr, "buffer.slot");

    // Execution starts at node 0 with an empty stack.
    depth = 0;
    jump(0);

    while (!pending.empty()) {
        auto [nodeIndex, copyDepth] = pending.front();
        pending.pop_front();
        builder.SetInsertPoint(copies[{ nodeIndex, copyDepth }]);
        depth = copyDepth;
        emitNode(nodes[nodeIndex]);
    }
}

} // namespace

IRGenerator::IRGenerator(LLVMContext &ctx) : context(ctx) { }

Module* IRGenerator::generateModule(const Graph &graph) {
    Module *module = new Module("PietModule", context);
    CodeGen codegen(context, *module);
    codegen.emit(graph);
    verifyModule(*module, &errs());
    return module;
}
