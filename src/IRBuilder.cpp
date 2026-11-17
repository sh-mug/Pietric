#include "IRBuilder.h"
#include "Runtime.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>

using namespace llvm;

namespace {

// Number of values the stack has room for before it first has to grow.
constexpr int64_t kInitialStackCapacity = 1024;

// Emits main() for an execution graph. Every graph node becomes a basic block
// that performs the command of its outgoing edge and branches to the
// successor node.
//
// The stack is a heap buffer of i32 values that doubles in size when it is
// full. Its size, capacity and buffer pointer are kept in allocas of main()
// and every push and pop is emitted inline, so once mem2reg has promoted
// them, the optimizer sees plain loads and stores: the stack depth becomes a
// constant wherever the control flow allows, and values can be forwarded from
// the push that stores them to the pop that loads them.
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

    // The stack: buffer[0..size) holds its values, buffer[size - 1] being the top.
    AllocaInst *sizeSlot = nullptr;      // i64
    AllocaInst *capacitySlot = nullptr;  // i64: how many values the buffer has room for
    AllocaInst *bufferSlot = nullptr;    // ptr

    // Runtime library functions.
    FunctionCallee readNumberF, readCharF, writeNumberF, writeCharF;

    ConstantInt *constant(int value) { return ConstantInt::get(i32, value); }
    ConstantInt *index(int64_t value) { return ConstantInt::get(i64, value); }
    Value *stackSize() { return builder.CreateLoad(i64, sizeSlot, "size"); }
    Value *stackBuffer() { return builder.CreateLoad(ptr, bufferSlot, "buffer"); }
    // Pops the top value. The stack must not be empty.
    Value *pop();
    void push(Value *value);

    // Allocates the stack at the start of main().
    void emitStackInit();

    // Emits `if (cond) { then(); }` and continues after it.
    template <typename F> void emitIf(Value *cond, F then);
    // Emits `if (cond) { then(); } else { otherwise(); }` and continues after it.
    template <typename T, typename E> void emitIfElse(Value *cond, T then, E otherwise);
    // Emits `then` guarded by a check that the stack holds at least `count` values.
    // A command that needs more values than the stack holds is ignored.
    template <typename F> void emitIfStackHas(int count, F then);
    // Emits a command that pops two values and pushes op(second, top).
    template <typename F> void emitBinary(F op);
    // Emits divide or modulo.
    void emitDivMod(Command cmd);
    // Emits an input command: calls `readF` and pushes the value if one was read.
    void emitInput(FunctionCallee readF);

    // Emits the command executed when leaving a block of `blockSize` codels.
    void emitCommand(Command cmd, int blockSize);
    // Emits a multi-way branch on the value popped by a pointer or switch command.
    void emitBranch(const GraphNode &node, const std::vector<BasicBlock *> &bbNodes);
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

template <typename F> void CodeGen::emitIf(Value *cond, F then) {
    BasicBlock *thenBB = BasicBlock::Create(ctx, "then", mainFunc);
    BasicBlock *contBB = BasicBlock::Create(ctx, "cont", mainFunc);
    builder.CreateCondBr(cond, thenBB, contBB);
    builder.SetInsertPoint(thenBB);
    then();
    builder.CreateBr(contBB);
    builder.SetInsertPoint(contBB);
}

template <typename T, typename E> void CodeGen::emitIfElse(Value *cond, T then, E otherwise) {
    BasicBlock *thenBB = BasicBlock::Create(ctx, "then", mainFunc);
    BasicBlock *elseBB = BasicBlock::Create(ctx, "else", mainFunc);
    BasicBlock *contBB = BasicBlock::Create(ctx, "cont", mainFunc);
    builder.CreateCondBr(cond, thenBB, elseBB);
    builder.SetInsertPoint(thenBB);
    then();
    builder.CreateBr(contBB);
    builder.SetInsertPoint(elseBB);
    otherwise();
    builder.CreateBr(contBB);
    builder.SetInsertPoint(contBB);
}

Value *CodeGen::pop() {
    // The callers check the size first, so it cannot wrap.
    Value *newSize = builder.CreateNUWSub(stackSize(), index(1));
    builder.CreateStore(newSize, sizeSlot);
    return builder.CreateLoad(i32, builder.CreateInBoundsGEP(i32, stackBuffer(), newSize));
}

void CodeGen::push(Value *value) {
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

void CodeGen::emitStackInit() {
    sizeSlot = builder.CreateAlloca(i64, nullptr, "size.slot");
    capacitySlot = builder.CreateAlloca(i64, nullptr, "capacity.slot");
    bufferSlot = builder.CreateAlloca(ptr, nullptr, "buffer.slot");
    builder.CreateStore(index(0), sizeSlot);
    builder.CreateStore(index(kInitialStackCapacity), capacitySlot);
    Value *buffer = builder.CreateCall(
        runtime.malloc, { index(kInitialStackCapacity * sizeof(int32_t)) }, "buffer");
    emitIf(builder.CreateIsNull(buffer), [&] { builder.CreateCall(runtime.outOfMemory); });
    builder.CreateStore(buffer, bufferSlot);
}

template <typename F> void CodeGen::emitIfStackHas(int count, F then) {
    emitIf(builder.CreateICmpSGE(stackSize(), index(count)), then);
}

template <typename F> void CodeGen::emitBinary(F op) {
    emitIfStackHas(2, [&] {
        Value *top = pop();
        Value *second = pop();
        push(op(second, top));
    });
}

void CodeGen::emitDivMod(Command cmd) {
    emitIfStackHas(2, [&] {
        Value *divisor = pop();
        Value *dividend = pop();
        // Division by zero is an implementation-defined error; as the specification
        // recommends, the command is ignored and the operands stay on the stack.
        emitIfElse(builder.CreateICmpEQ(divisor, constant(0)),
            [&] {
                push(dividend);
                push(divisor);
            },
            [&] {
                // sdiv/srem of INT_MIN by -1 is undefined behaviour in LLVM IR, so divide by 1
                // instead and fix up the result: x / -1 is -x (wrapping) and x mod -1 is 0.
                Value *isMinusOne = builder.CreateICmpEQ(divisor, constant(-1));
                Value *safeDivisor = builder.CreateSelect(isMinusOne, constant(1), divisor);
                if (cmd == Command::Divide) {
                    // Integer division truncates towards zero.
                    Value *quotient = builder.CreateSDiv(dividend, safeDivisor);
                    push(builder.CreateSelect(isMinusOne, builder.CreateNeg(dividend), quotient));
                } else {
                    // srem takes the sign of the dividend, but the result of mod has the sign
                    // of the divisor: add the divisor when the signs differ.
                    Value *rem = builder.CreateSRem(dividend, safeDivisor);
                    Value *signsDiffer = builder.CreateICmpSLT(builder.CreateXor(rem, divisor),
                                                               constant(0));
                    Value *adjust = builder.CreateAnd(builder.CreateICmpNE(rem, constant(0)),
                                                      signsDiffer);
                    push(builder.CreateSelect(adjust, builder.CreateAdd(rem, divisor), rem));
                }
            });
    });
}

void CodeGen::emitInput(FunctionCallee readF) {
    // If nothing can be read (e.g. at end of input), the command is ignored.
    Value *ok = builder.CreateCall(readF, { inputSlot });
    emitIf(builder.CreateICmpNE(ok, constant(0)), [&] {
        push(builder.CreateLoad(i32, inputSlot));
    });
}

void CodeGen::emitCommand(Command cmd, int blockSize) {
    switch (cmd) {
        case Command::Push:
            push(constant(blockSize));
            break;
        case Command::Pop:
            emitIfStackHas(1, [&] { pop(); });
            break;
        case Command::Add:
            emitBinary([&](Value *a, Value *b) { return builder.CreateAdd(a, b); });
            break;
        case Command::Subtract:
            emitBinary([&](Value *a, Value *b) { return builder.CreateSub(a, b); });
            break;
        case Command::Multiply:
            emitBinary([&](Value *a, Value *b) { return builder.CreateMul(a, b); });
            break;
        case Command::Divide:
        case Command::Modulo:
            emitDivMod(cmd);
            break;
        case Command::Not:
            emitIfStackHas(1, [&] {
                Value *isZero = builder.CreateICmpEQ(pop(), constant(0));
                push(builder.CreateZExt(isZero, i32));
            });
            break;
        case Command::Greater:
            emitBinary([&](Value *a, Value *b) {
                return builder.CreateZExt(builder.CreateICmpSGT(a, b), i32);
            });
            break;
        case Command::Duplicate:
            emitIfStackHas(1, [&] {
                Value *top = pop();
                push(top);
                push(top);
            });
            break;
        case Command::Roll:
            emitIfStackHas(2, [&] {
                Value *rolls = pop();
                Value *depth = pop();
                // A negative depth, or one greater than the number of remaining values, cannot
                // be rolled: the command is ignored, so the arguments go back on the stack.
                Value *size = stackSize();
                Value *wideDepth = builder.CreateSExt(depth, i64);
                Value *valid = builder.CreateAnd(builder.CreateICmpSGE(depth, constant(0)),
                                                 builder.CreateICmpSLE(wideDepth, size));
                emitIfElse(valid,
                           [&] {
                               Value *base = builder.CreateInBoundsGEP(
                                   i32, stackBuffer(), builder.CreateSub(size, wideDepth));
                               builder.CreateCall(runtime.roll, { base, depth, rolls });
                           },
                           [&] {
                               push(depth);
                               push(rolls);
                           });
            });
            break;
        case Command::InputNum:
            emitInput(readNumberF);
            break;
        case Command::InputChar:
            emitInput(readCharF);
            break;
        case Command::OutputNum:
            emitIfStackHas(1, [&] { builder.CreateCall(writeNumberF, { pop() }); });
            break;
        case Command::OutputChar:
            emitIfStackHas(1, [&] { builder.CreateCall(writeCharF, { pop() }); });
            break;
        case Command::None:
        case Command::Pointer:
        case Command::Switch:
            // Pointer and switch are emitted as branches by emitBranch.
            break;
    }
}

// The graph gives a pointer node four successors, transitions[i] being the state after turning the
// DP clockwise i times, and a switch node two, transitions[i] being the state after toggling the
// CC i times.
void CodeGen::emitBranch(const GraphNode &node, const std::vector<BasicBlock *> &bbNodes) {
    Command cmd = node.transitions[0].command;
    int numEdges = node.transitions.size();
    assert(numEdges == (cmd == Command::Pointer ? 4 : 2));
    BasicBlock *unchangedBB = bbNodes[node.transitions[0].targetNode];

    // With an empty stack the command is ignored and DP and CC stay as they are.
    BasicBlock *selectBB = BasicBlock::Create(ctx, "select", mainFunc);
    builder.CreateCondBr(builder.CreateICmpSGT(stackSize(), index(0)), selectBB, unchangedBB);
    builder.SetInsertPoint(selectBB);

    Value *value = pop();
    Value *index;
    if (cmd == Command::Pointer) {
        // Turn clockwise `value` times, or anticlockwise if negative: -1 is the same as 3, so
        // the index is value mod 4 rounded towards negative infinity, in 0..3.
        Value *rem = builder.CreateSRem(value, constant(4));
        Value *isNegative = builder.CreateICmpSLT(rem, constant(0));
        index = builder.CreateSelect(isNegative, builder.CreateAdd(rem, constant(4)), rem);
    } else {
        // Toggle |value| times: only whether that is odd matters.
        Value *rem = builder.CreateSRem(value, constant(2));
        index = builder.CreateZExt(builder.CreateICmpNE(rem, constant(0)), i32);
    }

    SwitchInst *swInst = builder.CreateSwitch(index, unchangedBB, numEdges - 1);
    for (int j = 1; j < numEdges; j++)
        swInst->addCase(constant(j), bbNodes[node.transitions[j].targetNode]);
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
    emitStackInit();

    // Create a basic block for each graph node.
    std::vector<BasicBlock *> bbNodes;
    for (size_t i = 0; i < nodes.size(); ++i)
        bbNodes.push_back(BasicBlock::Create(ctx, "node" + std::to_string(i), mainFunc));

    // Branch from entry to the initial node.
    builder.CreateBr(bbNodes[0]);

    for (size_t i = 0; i < nodes.size(); ++i) {
        builder.SetInsertPoint(bbNodes[i]);
        const GraphNode &node = nodes[i];

        if (node.transitions.empty()) {
            // Terminal state: free the stack and return.
            builder.CreateCall(runtime.free, { stackBuffer() });
            builder.CreateRet(constant(0));
        } else if (node.transitions[0].command == Command::Pointer ||
                   node.transitions[0].command == Command::Switch) {
            // Pointer or switch: the popped value selects the successor.
            emitBranch(node, bbNodes);
        } else {
            // Single transition: execute the command, then go to the sole successor.
            emitCommand(node.transitions[0].command, node.blockSize);
            builder.CreateBr(bbNodes[node.transitions[0].targetNode]);
        }
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
