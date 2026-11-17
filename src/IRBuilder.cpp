#include "IRBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

// Emits main() for an execution graph. Every graph node becomes a basic block
// that performs the command of its outgoing edge and branches to the
// successor node; the stack lives in the runtime library (StackVM.cpp).
class CodeGen {
public:
    CodeGen(LLVMContext &ctx, Module &module);
    void emit(const Graph &graph);

private:
    LLVMContext &ctx;
    Module &module;
    IRBuilder<> builder;
    IntegerType *i32;
    Function *mainFunc = nullptr;
    Value *stack = nullptr;
    Value *inputSlot = nullptr;  // receives values read by the input commands

    // Runtime library functions.
    FunctionCallee stackPushF, stackPopF, createStackF, destroyStackF, stackRollF;
    FunctionCallee readNumberF, readCharF, writeNumberF, writeCharF;

    ConstantInt *constant(int value) { return ConstantInt::get(i32, value); }
    Value *pop() { return builder.CreateCall(stackPopF, { stack }); }
    void push(Value *value) { builder.CreateCall(stackPushF, { stack, value }); }

    // Emits `if (cond) { then(); }` and continues after it.
    template <typename F> void emitIf(Value *cond, F then);
    // Emits an input command: calls `readF` and pushes the value if one was read.
    void emitInput(FunctionCallee readF);

    // Emits the command executed when leaving a block of `blockSize` codels.
    void emitCommand(Command cmd, int blockSize);
    // Emits a multi-way branch on the value popped by a pointer or switch command.
    void emitBranch(const GraphNode &node, const std::vector<BasicBlock *> &bbNodes);
};

CodeGen::CodeGen(LLVMContext &ctx, Module &module)
    : ctx(ctx), module(module), builder(ctx), i32(Type::getInt32Ty(ctx)) {
    Type *voidTy = Type::getVoidTy(ctx);
    PointerType *stackPtrTy = PointerType::getUnqual(Type::getInt8Ty(ctx));

    stackPushF = module.getOrInsertFunction(
        "stackPush", FunctionType::get(voidTy, { stackPtrTy, i32 }, false));
    stackPopF = module.getOrInsertFunction(
        "stackPop", FunctionType::get(i32, { stackPtrTy }, false));
    createStackF = module.getOrInsertFunction(
        "createStack", FunctionType::get(stackPtrTy, {}, false));
    destroyStackF = module.getOrInsertFunction(
        "destroyStack", FunctionType::get(voidTy, { stackPtrTy }, false));
    stackRollF = module.getOrInsertFunction(
        "stackRoll", FunctionType::get(voidTy, { stackPtrTy, i32, i32 }, false));
    PointerType *intPtrTy = PointerType::getUnqual(i32);
    readNumberF = module.getOrInsertFunction(
        "pietReadNumber", FunctionType::get(i32, { intPtrTy }, false));
    readCharF = module.getOrInsertFunction(
        "pietReadChar", FunctionType::get(i32, { intPtrTy }, false));
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
            pop();
            break;
        case Command::Add: {
            Value *a = pop();
            Value *b = pop();
            push(builder.CreateAdd(a, b));
            break;
        }
        case Command::Subtract: {
            Value *a = pop();
            Value *b = pop();
            push(builder.CreateSub(b, a));
            break;
        }
        case Command::Multiply: {
            Value *a = pop();
            Value *b = pop();
            push(builder.CreateMul(a, b));
            break;
        }
        case Command::Divide: {
            Value *a = pop();
            Value *b = pop();
            push(builder.CreateSDiv(b, a));
            break;
        }
        case Command::Modulo: {
            Value *a = pop();
            Value *b = pop();
            push(builder.CreateSRem(b, a));
            break;
        }
        case Command::Not: {
            Value *a = pop();
            Value *isZero = builder.CreateICmpEQ(a, constant(0));
            push(builder.CreateSelect(isZero, constant(1), constant(0)));
            break;
        }
        case Command::Greater: {
            Value *a = pop();
            Value *b = pop();
            Value *greater = builder.CreateICmpSGT(b, a);
            push(builder.CreateSelect(greater, constant(1), constant(0)));
            break;
        }
        case Command::Duplicate: {
            Value *top = pop();
            push(top);
            push(top);
            break;
        }
        case Command::Roll: {
            Value *rolls = pop();
            Value *depth = pop();
            builder.CreateCall(stackRollF, { stack, rolls, depth });
            break;
        }
        case Command::InputNum:
            emitInput(readNumberF);
            break;
        case Command::InputChar:
            emitInput(readCharF);
            break;
        case Command::OutputNum:
            builder.CreateCall(writeNumberF, { pop() });
            break;
        case Command::OutputChar:
            builder.CreateCall(writeCharF, { pop() });
            break;
        case Command::None:
        case Command::Pointer:
        case Command::Switch:
            // Pointer and switch are emitted as branches by emitBranch.
            break;
    }
}

void CodeGen::emitBranch(const GraphNode &node, const std::vector<BasicBlock *> &bbNodes) {
    Value *choice = pop();
    // For safety, compute modulo (#edges) by using an unsigned remainder.
    int numEdges = node.transitions.size();
    Value *index = builder.CreateURem(choice, constant(numEdges), "choiceIndex");

    // Create a switch instruction that branches to each target basic block.
    BasicBlock *defaultBB = bbNodes[node.transitions[0].targetNode]; // arbitrary default.
    SwitchInst *swInst = builder.CreateSwitch(index, defaultBB, numEdges);
    for (int j = 0; j < numEdges; j++)
        swInst->addCase(constant(j), bbNodes[node.transitions[j].targetNode]);
}

void CodeGen::emit(const Graph &graph) {
    // Create main function: int main()
    mainFunc = Function::Create(FunctionType::get(i32, false), Function::ExternalLinkage,
                                "main", module);
    BasicBlock *entryBB = BasicBlock::Create(ctx, "entry", mainFunc);
    builder.SetInsertPoint(entryBB);

    inputSlot = builder.CreateAlloca(i32, nullptr, "input");

    // Create the runtime stack.
    stack = builder.CreateCall(createStackF, {});

    const std::vector<GraphNode> &nodes = graph.getNodes();
    if (nodes.empty()) {
        builder.CreateRet(constant(0));
        return;
    }

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
            // Terminal state: destroy stack and return.
            builder.CreateCall(destroyStackF, { stack });
            builder.CreateRet(constant(0));
        } else if (node.transitions.size() == 1) {
            // Single transition: execute the command, then go to the sole successor.
            emitCommand(node.transitions[0].command, node.blockSize);
            builder.CreateBr(bbNodes[node.transitions[0].targetNode]);
        } else {
            // Pointer or switch: the popped value selects the successor.
            emitBranch(node, bbNodes);
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
