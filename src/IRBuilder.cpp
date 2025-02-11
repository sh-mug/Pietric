#include "IRBuilder.h"
#include "StackVM.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

IRGenerator::IRGenerator(LLVMContext &ctx) : context(ctx) { }

Module* IRGenerator::generateModule(const Graph &graph) {
    Module *module = new Module("PietModule", context);
    IRBuilder<> builder(context);

    // Declare external runtime functions.
    PointerType *stackPtrTy = PointerType::getUnqual(Type::getInt8Ty(context));

    FunctionType *pushType = FunctionType::get(Type::getVoidTy(context),
                                               {stackPtrTy, Type::getInt32Ty(context)}, false);
    Function *stackPushF = Function::Create(pushType, Function::ExternalLinkage, "stackPush", module);

    FunctionType *popType = FunctionType::get(Type::getInt32Ty(context),
                                              {stackPtrTy}, false);
    Function *stackPopF = Function::Create(popType, Function::ExternalLinkage, "stackPop", module);

    FunctionType *createType = FunctionType::get(stackPtrTy, {}, false);
    Function *createStackF = Function::Create(createType, Function::ExternalLinkage, "createStack", module);

    FunctionType *destroyType = FunctionType::get(Type::getVoidTy(context),
                                                  {stackPtrTy}, false);
    Function *destroyStackF = Function::Create(destroyType, Function::ExternalLinkage, "destroyStack", module);

    FunctionType *rollType = FunctionType::get(Type::getVoidTy(context),
                                               {stackPtrTy, Type::getInt32Ty(context), Type::getInt32Ty(context)},
                                               false);
    Function *stackRollF = Function::Create(rollType, Function::ExternalLinkage, "stackRoll", module);

    FunctionType *putcharType = FunctionType::get(Type::getInt32Ty(context),
                                                  {Type::getInt32Ty(context)}, false);
    Function *putcharF = Function::Create(putcharType, Function::ExternalLinkage, "putchar", module);

    // Create main function: int main()
    FunctionType *mainType = FunctionType::get(Type::getInt32Ty(context), false);
    Function *mainFunc = Function::Create(mainType, Function::ExternalLinkage, "main", module);
    BasicBlock *entryBB = BasicBlock::Create(context, "entry", mainFunc);
    builder.SetInsertPoint(entryBB);

    // Create the runtime stack.
    CallInst *stackInst = builder.CreateCall(createStackF, {});

    // Retrieve the execution graph.
    const std::vector<GraphNode>& nodes = graph.getNodes();
    if (nodes.empty()) {
        builder.CreateRet(ConstantInt::get(Type::getInt32Ty(context), 0));
        return module;
    }

    // Create a basic block for each graph node.
    std::vector<BasicBlock*> bbNodes;
    for (size_t i = 0; i < nodes.size(); ++i) {
        bbNodes.push_back(BasicBlock::Create(context, "node" + std::to_string(i), mainFunc));
    }

    // Branch from entry to the initial node.
    builder.CreateBr(bbNodes[0]);

    // For each node, generate code.
    for (size_t i = 0; i < nodes.size(); ++i) {
        builder.SetInsertPoint(bbNodes[i]);
        const GraphNode &node = nodes[i];
        
        // Now, branch based on outgoing transitions.
        if (node.transitions.empty()) {
            // Terminal state: destroy stack and return.
            builder.CreateCall(destroyStackF, { stackInst });
            builder.CreateRet(ConstantInt::get(Type::getInt32Ty(context), 0));
        } else if (node.transitions.size() == 1) {
            // Single transition: execute the command associated with the edge.
            Command cmd = node.transitions[0].command;
            // For arithmetic commands we simulate inline operations (this is similar to previous IR generation).
            switch (cmd) {
                case Command::Push: {
                    builder.CreateCall(stackPushF,
                        { stackInst, ConstantInt::get(Type::getInt32Ty(context), node.blockSize) });
                    break;
                }
                case Command::Pop: {
                    builder.CreateCall(stackPopF, { stackInst });
                    break;
                }
                case Command::Add: {
                    Value *a = builder.CreateCall(stackPopF, { stackInst });
                    Value *b = builder.CreateCall(stackPopF, { stackInst });
                    Value *sum = builder.CreateAdd(a, b);
                    builder.CreateCall(stackPushF, { stackInst, sum });
                    break;
                }
                case Command::Subtract: {
                    Value *a = builder.CreateCall(stackPopF, { stackInst });
                    Value *b = builder.CreateCall(stackPopF, { stackInst });
                    Value *diff = builder.CreateSub(b, a);
                    builder.CreateCall(stackPushF, { stackInst, diff });
                    break;
                }
                case Command::Multiply: {
                    Value *a = builder.CreateCall(stackPopF, { stackInst });
                    Value *b = builder.CreateCall(stackPopF, { stackInst });
                    Value *prod = builder.CreateMul(a, b);
                    builder.CreateCall(stackPushF, { stackInst, prod });
                    break;
                }
                case Command::Divide: {
                    Value *a = builder.CreateCall(stackPopF, { stackInst });
                    Value *b = builder.CreateCall(stackPopF, { stackInst });
                    Value *quot = builder.CreateSDiv(b, a);
                    builder.CreateCall(stackPushF, { stackInst, quot });
                    break;
                }
                case Command::Modulo: {
                    Value *a = builder.CreateCall(stackPopF, { stackInst });
                    Value *b = builder.CreateCall(stackPopF, { stackInst });
                    Value *rem = builder.CreateSRem(b, a);
                    builder.CreateCall(stackPushF, { stackInst, rem });
                    break;
                }
                case Command::Not: {
                    Value *a = builder.CreateCall(stackPopF, { stackInst });
                    Value *cmp = builder.CreateICmpEQ(a, ConstantInt::get(Type::getInt32Ty(context), 0));
                    Value *result = builder.CreateSelect(cmp,
                                             ConstantInt::get(Type::getInt32Ty(context), 1),
                                             ConstantInt::get(Type::getInt32Ty(context), 0));
                    builder.CreateCall(stackPushF, { stackInst, result });
                    break;
                }
                case Command::Greater: {
                    Value *a = builder.CreateCall(stackPopF, { stackInst });
                    Value *b = builder.CreateCall(stackPopF, { stackInst });
                    Value *cmp = builder.CreateICmpSGT(b, a);
                    Value *result = builder.CreateSelect(cmp,
                                             ConstantInt::get(Type::getInt32Ty(context), 1),
                                             ConstantInt::get(Type::getInt32Ty(context), 0));
                    builder.CreateCall(stackPushF, { stackInst, result });
                    break;
                }
                case Command::Duplicate: {
                    Value *top = builder.CreateCall(stackPopF, { stackInst });
                    builder.CreateCall(stackPushF, { stackInst, top });
                    builder.CreateCall(stackPushF, { stackInst, top });
                    break;
                }
                case Command::Roll: {
                    Value *rolls = builder.CreateCall(stackPopF, { stackInst });
                    Value *depth = builder.CreateCall(stackPopF, { stackInst });
                    builder.CreateCall(stackRollF, { stackInst, rolls, depth });
                    break;
                }
                case Command::OutputChar: {
                    Value *ch = builder.CreateCall(stackPopF, { stackInst });
                    builder.CreateCall(putcharF, { builder.CreateIntCast(ch, Type::getInt32Ty(context), false) });
                    break;
                }
                default:
                    break;
            }
            // Unconditional branch to the sole successor.
            builder.CreateBr(bbNodes[node.transitions[0].targetNode]);
        } else {
            // Multiple transitions: pop an integer from the stack and use it to choose the branch.
            Value *choice = builder.CreateCall(stackPopF, { stackInst });
            // For safety, compute modulo (#edges) by using an unsigned remainder.
            int numEdges = node.transitions.size();
            Value *modVal = ConstantInt::get(Type::getInt32Ty(context), numEdges);
            Value *index = builder.CreateURem(choice, modVal, "choiceIndex");

            // Create a switch instruction that branches to each target basic block.
            BasicBlock *defaultBB = bbNodes[node.transitions[0].targetNode]; // arbitrary default.
            SwitchInst *swInst = builder.CreateSwitch(index, defaultBB, numEdges);
            for (unsigned j = 0; j < node.transitions.size(); j++) {
                int tgt = node.transitions[j].targetNode;
                swInst->addCase(ConstantInt::get(Type::getInt32Ty(context), j), bbNodes[tgt]);
            }
            // (In a full implementation, the command would be executed as part of each transition.)
        }
    }
    // Verify the module.
    verifyModule(*module, &errs());
    return module;
}
