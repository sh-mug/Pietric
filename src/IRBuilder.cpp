#include "IRBuilder.h"
#include "Graph.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

IRGenerator::IRGenerator(LLVMContext &ctx) : context(ctx) { }

// --------------------------------------------------------------------------
// Inline Stack Operation Helpers
// Our custom stack is defined as a struct { i32*, i32, i32 }:
//    - Field 0: pointer to i32 (the buffer)
//    - Field 1: current size (i32)
//    - Field 2: capacity (i32)
// --------------------------------------------------------------------------

// inlineStackPush: push 'value' onto the stack.
static void inlineStackPush(IRBuilder<> &builder, Value* stackInst, Value* value,
                              StructType* stackTy, Type* i32Ty) {
    // Get pointer to the 'size' field (index 1)
    SmallVector<Value*, 2> idx1;
    idx1.push_back(ConstantInt::get(i32Ty, 0));
    idx1.push_back(ConstantInt::get(i32Ty, 1));
    Value *sizePtr = builder.CreateInBoundsGEP(stackTy, stackInst, idx1, "size_ptr");
    Value *sizeVal = builder.CreateLoad(i32Ty, sizePtr, "size");
    // Get pointer to the 'buffer' field (index 0)
    SmallVector<Value*, 2> idx0;
    idx0.push_back(ConstantInt::get(i32Ty, 0));
    idx0.push_back(ConstantInt::get(i32Ty, 0));
    Value *bufPtrPtr = builder.CreateInBoundsGEP(stackTy, stackInst, idx0, "buf_ptr_ptr");
    Value *buf = builder.CreateLoad(PointerType::getUnqual(i32Ty), bufPtrPtr, "buf");
    // Compute destination pointer: buf + size
    Value *destPtr = builder.CreateInBoundsGEP(i32Ty, buf, sizeVal, "dest_ptr");
    builder.CreateStore(value, destPtr);
    // Increment the size field
    Value *one = ConstantInt::get(i32Ty, 1);
    Value *newSize = builder.CreateAdd(sizeVal, one, "new_size");
    builder.CreateStore(newSize, sizePtr);
}

// inlineStackPop: pop the top value from the stack and return it.
static Value* inlineStackPop(IRBuilder<> &builder, Value* stackInst,
                             StructType* stackTy, Type* i32Ty) {
    // Get pointer to the 'size' field.
    SmallVector<Value*, 2> idx1;
    idx1.push_back(ConstantInt::get(i32Ty, 0));
    idx1.push_back(ConstantInt::get(i32Ty, 1));
    Value *sizePtr = builder.CreateInBoundsGEP(stackTy, stackInst, idx1, "size_ptr");
    Value *sizeVal = builder.CreateLoad(i32Ty, sizePtr, "size");
    Value *one = ConstantInt::get(i32Ty, 1);
    Value *newSize = builder.CreateSub(sizeVal, one, "new_size");
    builder.CreateStore(newSize, sizePtr);
    // Get pointer to the 'buffer' field.
    SmallVector<Value*, 2> idx0;
    idx0.push_back(ConstantInt::get(i32Ty, 0));
    idx0.push_back(ConstantInt::get(i32Ty, 0));
    Value *bufPtrPtr = builder.CreateInBoundsGEP(stackTy, stackInst, idx0, "buf_ptr_ptr");
    Value *buf = builder.CreateLoad(PointerType::getUnqual(i32Ty), bufPtrPtr, "buf");
    // Compute pointer to element: buf + newSize
    Value *elemPtr = builder.CreateInBoundsGEP(i32Ty, buf, newSize, "elem_ptr");
    Value *popped = builder.CreateLoad(i32Ty, elemPtr, "popped");
    return popped;
}

// inlineStackRoll: rotates the top 'depth' elements upward by 'rolls' positions.
static void inlineStackRoll(IRBuilder<> &builder, Value* stackInst, Value* rolls, Value* depth,
                              StructType* stackTy, Type* i32Ty) {
    // Get current stack size.
    SmallVector<Value*, 2> idx1;
    idx1.push_back(ConstantInt::get(i32Ty, 0));
    idx1.push_back(ConstantInt::get(i32Ty, 1));
    Value *sizePtr = builder.CreateInBoundsGEP(stackTy, stackInst, idx1, "size_ptr");
    Value *sizeVal = builder.CreateLoad(i32Ty, sizePtr, "size");

    // Check if depth is valid: if (depth <= 0 || depth > size) then skip the roll.
    Value *zero = ConstantInt::get(i32Ty, 0);
    Value *cmpDepthLeZero = builder.CreateICmpSLE(depth, zero, "depth_le_zero");
    Value *cmpDepthGtSize = builder.CreateICmpSGT(depth, sizeVal, "depth_gt_size");
    Value *invalidDepth = builder.CreateOr(cmpDepthLeZero, cmpDepthGtSize, "invalid_depth");

    Function *parentFunc = builder.GetInsertBlock()->getParent();
    BasicBlock *rollContBB = BasicBlock::Create(builder.getContext(), "roll_cont", parentFunc);
    BasicBlock *rollEndBB = BasicBlock::Create(builder.getContext(), "roll_end", parentFunc);
    builder.CreateCondBr(invalidDepth, rollEndBB, rollContBB);

    // In roll_cont: perform the rotation.
    builder.SetInsertPoint(rollContBB);
    // --- HOIST: Compute invariant pointer to the stack buffer ---
    SmallVector<Value*, 2> idx0;
    idx0.push_back(ConstantInt::get(i32Ty, 0));
    idx0.push_back(ConstantInt::get(i32Ty, 0));
    Value *bufPtrPtr = builder.CreateInBoundsGEP(stackTy, stackInst, idx0, "roll_buf_ptr_ptr");
    Value *invariantBuf = builder.CreateLoad(PointerType::getUnqual(i32Ty), bufPtrPtr, "roll_buf");
    // -----------------------------------------------------------

    // Compute effective rolls: adjustedRolls = (rolls % depth + depth) % depth.
    Value *modVal = builder.CreateSRem(rolls, depth, "mod");
    Value *cmpModNeg = builder.CreateICmpSLT(modVal, zero, "mod_neg");
    Value *adjustedRolls = builder.CreateSelect(cmpModNeg,
                                                  builder.CreateAdd(modVal, depth),
                                                  modVal,
                                                  "adjustedRolls");
    // If adjustedRolls == 0, no rotation is needed.
    Value *cmpRollsZero = builder.CreateICmpEQ(adjustedRolls, zero, "rolls_zero");
    BasicBlock *rollNoOpBB = BasicBlock::Create(builder.getContext(), "roll_noop", parentFunc);
    BasicBlock *rollDoOpBB = BasicBlock::Create(builder.getContext(), "roll_doop", parentFunc);
    builder.CreateCondBr(cmpRollsZero, rollNoOpBB, rollDoOpBB);

    // rollNoOp: branch to roll_end.
    builder.SetInsertPoint(rollNoOpBB);
    builder.CreateBr(rollEndBB);

    // rollDoOp: perform rotation.
    builder.SetInsertPoint(rollDoOpBB);
    // Compute starting index: start = size - depth.
    Value *startIdx = builder.CreateSub(sizeVal, depth, "startIdx");

    // Allocate a temporary buffer on the stack to hold 'depth' elements.
    Value *temp = builder.CreateAlloca(i32Ty, depth, "temp");

    // Loop: Copy top 'depth' elements from the stack buffer into temp.
    BasicBlock *copyLoopBB = BasicBlock::Create(builder.getContext(), "copy_loop", parentFunc);
    BasicBlock *copyAfterBB = BasicBlock::Create(builder.getContext(), "copy_after", parentFunc);
    AllocaInst *copyIdxAlloca = builder.CreateAlloca(i32Ty, nullptr, "copyIdx");
    builder.CreateStore(zero, copyIdxAlloca);
    builder.CreateBr(copyLoopBB);

    builder.SetInsertPoint(copyLoopBB);
    Value *copyIdx = builder.CreateLoad(i32Ty, copyIdxAlloca, "copyIdx");
    Value *cmpCopy = builder.CreateICmpSLT(copyIdx, depth, "cmp_copy");
    BasicBlock *copyLoopBodyBB = BasicBlock::Create(builder.getContext(), "copy_loop_body", parentFunc);
    builder.CreateCondBr(cmpCopy, copyLoopBodyBB, copyAfterBB);

    builder.SetInsertPoint(copyLoopBodyBB);
    // source index = startIdx + copyIdx
    Value *srcIdx = builder.CreateAdd(startIdx, copyIdx, "srcIdx");
    // Use the invariantBuf (hoisted earlier) instead of recomputing.
    Value *srcElemPtr = builder.CreateInBoundsGEP(i32Ty, invariantBuf, srcIdx, "srcElemPtr");
    Value *elemVal = builder.CreateLoad(i32Ty, srcElemPtr, "elemVal");
    // temp[copyIdx] = elemVal
    Value *tempElemPtr = builder.CreateInBoundsGEP(i32Ty, temp, copyIdx, "tempElemPtr");
    builder.CreateStore(elemVal, tempElemPtr);
    // Increment copyIdx
    Value *copyIdxNext = builder.CreateAdd(copyIdx, ConstantInt::get(i32Ty, 1), "copyIdxNext");
    builder.CreateStore(copyIdxNext, copyIdxAlloca);
    builder.CreateBr(copyLoopBB);

    builder.SetInsertPoint(copyAfterBB);

    // Loop: Copy values from temp back into the stack in rotated order.
    BasicBlock *copyBackLoopBB = BasicBlock::Create(builder.getContext(), "copy_back_loop", parentFunc);
    BasicBlock *copyBackAfterBB = BasicBlock::Create(builder.getContext(), "copy_back_after", parentFunc);
    AllocaInst *copyBackIdxAlloca = builder.CreateAlloca(i32Ty, nullptr, "copyBackIdx");
    builder.CreateStore(zero, copyBackIdxAlloca);
    builder.CreateBr(copyBackLoopBB);

    builder.SetInsertPoint(copyBackLoopBB);
    Value *copyBackIdx = builder.CreateLoad(i32Ty, copyBackIdxAlloca, "copyBackIdx");
    Value *cmpCopyBack = builder.CreateICmpSLT(copyBackIdx, depth, "cmp_copy_back");
    BasicBlock *copyBackBodyBB = BasicBlock::Create(builder.getContext(), "copy_back_body", parentFunc);
    builder.CreateCondBr(cmpCopyBack, copyBackBodyBB, copyBackAfterBB);

    builder.SetInsertPoint(copyBackBodyBB);
    // new position = (copyBackIdx + adjustedRolls) mod depth
    Value *sumIdx = builder.CreateAdd(copyBackIdx, adjustedRolls, "sumIdx");
    Value *newPos = builder.CreateSRem(sumIdx, depth, "newPos");
    // destination index = startIdx + newPos
    Value *destIdx = builder.CreateAdd(startIdx, newPos, "destIdx");
    // Use invariantBuf again.
    Value *destElemPtr = builder.CreateInBoundsGEP(i32Ty, invariantBuf, destIdx, "destElemPtr");
    Value *tempElemPtr2 = builder.CreateInBoundsGEP(i32Ty, temp, copyBackIdx, "tempElemPtr2");
    Value *tempVal = builder.CreateLoad(i32Ty, tempElemPtr2, "tempVal");
    builder.CreateStore(tempVal, destElemPtr);
    // Increment copyBackIdx
    Value *copyBackIdxNext = builder.CreateAdd(copyBackIdx, ConstantInt::get(i32Ty, 1), "copyBackIdxNext");
    builder.CreateStore(copyBackIdxNext, copyBackIdxAlloca);
    builder.CreateBr(copyBackLoopBB);

    builder.SetInsertPoint(copyBackAfterBB);
    builder.CreateBr(rollEndBB);

    builder.SetInsertPoint(rollEndBB);
    // End of roll operation.
}

// --------------------------------------------------------------------------
// IRGenerator::generateModule
// This function creates the LLVM module and defines main().
// It allocates our custom stack (a %Stack structure) and emits inline code
// for stack operations. External functions remain for pointer, switch, and I/O.
// --------------------------------------------------------------------------

Module* IRGenerator::generateModule(const Graph &graph) {
    Module *module = new Module("PietModule", context);
    IRBuilder<> builder(context);
    Type *i32Ty = Type::getInt32Ty(context);
    PointerType *i32PtrTy = PointerType::getUnqual(i32Ty);
    Type *i8Ty = Type::getInt8Ty(context);
    PointerType *i8PtrTy = PointerType::getUnqual(i8Ty);

    // Define the custom stack type: { i32*, i32, i32 }
    std::vector<Type*> stackFields = { i32PtrTy, i32Ty, i32Ty };
    StructType *stackTy = StructType::create(context, stackFields, "Stack");

    // Create main function: int main()
    FunctionType *mainType = FunctionType::get(i32Ty, false);
    Function *mainFunc = Function::Create(mainType, Function::ExternalLinkage, "main", module);
    BasicBlock *entryBB = BasicBlock::Create(context, "entry", mainFunc);
    builder.SetInsertPoint(entryBB);

    // Allocate the custom stack structure on the function's stack.
    AllocaInst *stackInst = builder.CreateAlloca(stackTy, nullptr, "stack");

    // Allocate a fixed-capacity buffer (capacity = 1024).
    Constant *capConst = ConstantInt::get(i32Ty, 1024);
    ArrayType *bufArrayTy = ArrayType::get(i32Ty, 1024);
    AllocaInst *bufInst = builder.CreateAlloca(bufArrayTy, nullptr, "stackbuf");
    // Get pointer to the first element of the buffer.
    Value *zero = ConstantInt::get(i32Ty, 0);
    Value *indices[2] = { zero, zero };
    Value *bufPtr = builder.CreateInBoundsGEP(bufArrayTy, bufInst, indices, "bufPtr");
    // Store the buffer pointer into field 0 of the stack.
    SmallVector<Value*, 2> idx0;
    idx0.push_back(ConstantInt::get(i32Ty, 0));
    idx0.push_back(ConstantInt::get(i32Ty, 0));
    Value *stackField0 = builder.CreateInBoundsGEP(stackTy, stackInst, idx0, "stack_buf_ptr");    
    builder.CreateStore(bufPtr, stackField0);
    // Initialize the size (field 1) to 0.
    SmallVector<Value*, 2> idx1;
    idx1.push_back(ConstantInt::get(i32Ty, 0));
    idx1.push_back(ConstantInt::get(i32Ty, 1));
    Value *stackField1 = builder.CreateInBoundsGEP(stackTy, stackInst, idx1, "stack_size_ptr");
    builder.CreateStore(zero, stackField1);
    // Initialize the capacity (field 2) to 1024.
    SmallVector<Value*, 2> idx2;
    idx2.push_back(ConstantInt::get(i32Ty, 0));
    idx2.push_back(ConstantInt::get(i32Ty, 2));
    Value *stackField2 = builder.CreateInBoundsGEP(stackTy, stackInst, idx2, "stack_capacity_ptr");
    builder.CreateStore(capConst, stackField2);

    Constant *formatStrConst = ConstantDataArray::getString(context, "%d", true);
    GlobalVariable *formatStrVar = new GlobalVariable(*module, formatStrConst->getType(),
                                                      true, GlobalValue::PrivateLinkage,
                                                      formatStrConst, ".str");                                            
    // Get pointer to format string.
    SmallVector<Value*, 2> fmtIdx;
    fmtIdx.push_back(ConstantInt::get(i32Ty, 0));
    fmtIdx.push_back(ConstantInt::get(i32Ty, 0));
    Value *formatStrPtr = builder.CreateInBoundsGEP(formatStrVar->getValueType(), formatStrVar, fmtIdx, "formatStrPtr");

    FunctionType *scanfType = FunctionType::get(i32Ty, {i8PtrTy}, true);
    Function *scanfF = Function::Create(scanfType, Function::ExternalLinkage, "__isoc99_scanf", module);
    FunctionType *printfType = FunctionType::get(i32Ty, {i8PtrTy}, true);
    Function *printfF = Function::Create(printfType, Function::ExternalLinkage, "printf", module);
    FunctionType *InputCharType = FunctionType::get(Type::getInt32Ty(context), {}, false);
    Function *inputCharF = Function::Create(InputCharType, Function::ExternalLinkage, "getchar", module);
    FunctionType *outputCharType = FunctionType::get(Type::getInt32Ty(context), {Type::getInt32Ty(context)}, false);
    Function *outputCharF = Function::Create(outputCharType, Function::ExternalLinkage, "putchar", module);

    // Retrieve the execution graph.
    const std::vector<GraphNode>& nodes = graph.getNodes();
    if (nodes.empty()) {
        builder.CreateRet(ConstantInt::get(i32Ty, 0));
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
            // Terminal state: return 0.
            builder.CreateRet(ConstantInt::get(i32Ty, 0));
        } else if (node.transitions.size() == 1) {
            // Single transition: execute the command associated with the edge.
            Command cmd = node.transitions[0].command;
            // For arithmetic commands we simulate inline operations (this is similar to previous IR generation).
            switch (cmd) {
                case Command::Push: {
                    Value *blockSizeVal = ConstantInt::get(i32Ty, node.blockSize);
                    inlineStackPush(builder, stackInst, blockSizeVal, stackTy, i32Ty);
                    break;
                }
                case Command::Pop: {
                    inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    break;
                }
                case Command::Add: {
                    Value *a = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *b = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *sum = builder.CreateAdd(a, b, "sum");
                    inlineStackPush(builder, stackInst, sum, stackTy, i32Ty);
                    break;
                }
                case Command::Subtract: {
                    Value *a = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *b = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *diff = builder.CreateSub(b, a, "diff");
                    inlineStackPush(builder, stackInst, diff, stackTy, i32Ty);
                    break;
                }
                case Command::Multiply: {
                    Value *a = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *b = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *prod = builder.CreateMul(a, b, "prod");
                    inlineStackPush(builder, stackInst, prod, stackTy, i32Ty);
                    break;
                }
                case Command::Divide: {
                    Value *a = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *b = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *quot = builder.CreateSDiv(b, a, "quot");
                    inlineStackPush(builder, stackInst, quot, stackTy, i32Ty);
                    break;
                }
                case Command::Modulo: {
                    Value *a = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *b = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *rem = builder.CreateSRem(b, a, "rem");
                    inlineStackPush(builder, stackInst, rem, stackTy, i32Ty);
                    break;
                }
                case Command::Not: {
                    Value *a = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *cmp = builder.CreateICmpEQ(a, ConstantInt::get(i32Ty, 0), "cmp");
                    Value *result = builder.CreateSelect(cmp,
                                                         ConstantInt::get(i32Ty, 1),
                                                         ConstantInt::get(i32Ty, 0),
                                                         "not_result");
                    inlineStackPush(builder, stackInst, result, stackTy, i32Ty);
                    break;
                }
                case Command::Greater: {
                    Value *a = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *b = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *cmp = builder.CreateICmpSGT(b, a, "gt_cmp");
                    Value *result = builder.CreateSelect(cmp,
                                                         ConstantInt::get(i32Ty, 1),
                                                         ConstantInt::get(i32Ty, 0),
                                                         "gt_result");
                    inlineStackPush(builder, stackInst, result, stackTy, i32Ty);
                    break;
                }
                case Command::Duplicate: {
                    Value *val = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    inlineStackPush(builder, stackInst, val, stackTy, i32Ty);
                    inlineStackPush(builder, stackInst, val, stackTy, i32Ty);
                    break;
                }
                case Command::Roll: {
                    Value *rolls = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    Value *depth = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    inlineStackRoll(builder, stackInst, rolls, depth, stackTy, i32Ty);
                    break;
                }
                case Command::InputNum: {   // TODO: FIX THIS
                    // AllocaInst *tmp = builder.CreateAlloca(i32Ty, nullptr, "input_num_tmp");
                    // builder.CreateCall(scanfF, { formatStrPtr, tmp });
                    // Value *inputVal = builder.CreateLoad(i32Ty, tmp, "inputVal");
                    // inlineStackPush(builder, stackInst, inputVal, stackTy, i32Ty);
                    break;
                }
                case Command::InputChar: {
                    Value *inputChar = builder.CreateCall(inputCharF, {});
                    inlineStackPush(builder, stackInst, inputChar, stackTy, i32Ty);
                    break;
                }
                case Command::OutputNum: {
                    Value *val = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    builder.CreateCall(printfF, { formatStrPtr, val });
                    break;
                }
                case Command::OutputChar: {
                    Value *val = inlineStackPop(builder, stackInst, stackTy, i32Ty);
                    builder.CreateCall(outputCharF, { builder.CreateIntCast(val, Type::getInt32Ty(context), false) });                    break;
                }
                default:
                    break;
            }
            // Unconditional branch to the sole successor.
            builder.CreateBr(bbNodes[node.transitions[0].targetNode]);
        } else {
            // Multiple transitions: pop a choice value and branch via a switch.
            Value *choice = inlineStackPop(builder, stackInst, stackTy, i32Ty);
            // For safety, compute modulo (#edges) by using an unsigned remainder.
            int numEdges = node.transitions.size();
            Value *modVal = ConstantInt::get(i32Ty, numEdges);
            Value *index = builder.CreateURem(choice, modVal, "choiceIndex");

            // Create a switch instruction that branches to each target basic block.
            BasicBlock *defaultBB = bbNodes[node.transitions[0].targetNode]; // arbitrary default.
            SwitchInst *swInst = builder.CreateSwitch(index, defaultBB, numEdges);
            for (unsigned j = 0; j < node.transitions.size(); j++) {
                swInst->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, j)),
                                bbNodes[node.transitions[j].targetNode]);
            }
        }
    }
    // Verify the module.
    verifyModule(*module, &errs());
    return module;
}
