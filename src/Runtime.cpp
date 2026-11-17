#include "Runtime.h"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

namespace {

class RuntimeBuilder {
public:
    explicit RuntimeBuilder(Module &module);
    Runtime build();

private:
    Module &module;
    LLVMContext &ctx;
    IRBuilder<> builder;
    Type *voidTy;
    IntegerType *i32, *i64;
    PointerType *ptr;

    Function *define(const char *name, FunctionType *type);
    Function *defineOutOfMemory();
    Function *defineGrow(Function *outOfMemory);
    Function *defineReverse();
    Function *defineRoll(Function *reverse);
};

RuntimeBuilder::RuntimeBuilder(Module &module)
    : module(module), ctx(module.getContext()), builder(ctx), voidTy(builder.getVoidTy()),
      i32(builder.getInt32Ty()), i64(builder.getInt64Ty()), ptr(builder.getPtrTy()) { }

Function *RuntimeBuilder::define(const char *name, FunctionType *type) {
    Function *f = Function::Create(type, Function::InternalLinkage, name, module);
    f->addFnAttr(Attribute::NoUnwind);
    builder.SetInsertPoint(BasicBlock::Create(ctx, "entry", f));
    return f;
}

Function *RuntimeBuilder::defineOutOfMemory() {
    FunctionCallee perrorF =
        module.getOrInsertFunction("perror", FunctionType::get(voidTy, { ptr }, false));
    FunctionCallee exitF =
        module.getOrInsertFunction("exit", FunctionType::get(voidTy, { i32 }, false));

    Function *f = define("piet.out_of_memory", FunctionType::get(voidTy, false));
    f->addFnAttr(Attribute::NoReturn);
    f->addFnAttr(Attribute::Cold);
    builder.CreateCall(perrorF, { builder.CreateGlobalString("piet", "piet.name") });
    builder.CreateCall(exitF, { builder.getInt32(1) });
    builder.CreateUnreachable();
    return f;
}

Function *RuntimeBuilder::defineGrow(Function *outOfMemory) {
    FunctionCallee reallocF =
        module.getOrInsertFunction("realloc", FunctionType::get(ptr, { ptr, i64 }, false));

    Function *f = define("piet.grow", FunctionType::get(ptr, { ptr, i64 }, false));
    // Growing is rare; keep it out of line so that every push stays small.
    f->addFnAttr(Attribute::Cold);
    f->addFnAttr(Attribute::NoInline);
    Value *buffer = f->getArg(0);
    Value *capacity = f->getArg(1);

    Value *bytes = builder.CreateMul(capacity, ConstantInt::get(i64, sizeof(int32_t)));
    Value *grown = builder.CreateCall(reallocF, { buffer, bytes });
    BasicBlock *failBB = BasicBlock::Create(ctx, "fail", f);
    BasicBlock *doneBB = BasicBlock::Create(ctx, "done", f);
    builder.CreateCondBr(builder.CreateIsNull(grown), failBB, doneBB);

    builder.SetInsertPoint(failBB);
    builder.CreateCall(outOfMemory);
    builder.CreateUnreachable();

    builder.SetInsertPoint(doneBB);
    builder.CreateRet(grown);
    return f;
}

// void piet.reverse(ptr base, i64 begin, i64 end): reverses base[begin..end).
Function *RuntimeBuilder::defineReverse() {
    Function *f = define("piet.reverse", FunctionType::get(voidTy, { ptr, i64, i64 }, false));
    Value *base = f->getArg(0);
    Value *begin = f->getArg(1);
    Value *end = f->getArg(2);
    BasicBlock *entryBB = builder.GetInsertBlock();
    Value *last = builder.CreateSub(end, ConstantInt::get(i64, 1));

    BasicBlock *loopBB = BasicBlock::Create(ctx, "loop", f);
    BasicBlock *bodyBB = BasicBlock::Create(ctx, "body", f);
    BasicBlock *doneBB = BasicBlock::Create(ctx, "done", f);
    builder.CreateBr(loopBB);

    builder.SetInsertPoint(loopBB);
    PHINode *i = builder.CreatePHI(i64, 2, "i");
    PHINode *j = builder.CreatePHI(i64, 2, "j");
    builder.CreateCondBr(builder.CreateICmpSLT(i, j), bodyBB, doneBB);

    builder.SetInsertPoint(bodyBB);
    Value *pi = builder.CreateInBoundsGEP(i32, base, i);
    Value *pj = builder.CreateInBoundsGEP(i32, base, j);
    Value *vi = builder.CreateLoad(i32, pi);
    Value *vj = builder.CreateLoad(i32, pj);
    builder.CreateStore(vj, pi);
    builder.CreateStore(vi, pj);
    Value *nextI = builder.CreateAdd(i, ConstantInt::get(i64, 1));
    Value *nextJ = builder.CreateSub(j, ConstantInt::get(i64, 1));
    builder.CreateBr(loopBB);

    i->addIncoming(begin, entryBB);
    i->addIncoming(nextI, bodyBB);
    j->addIncoming(last, entryBB);
    j->addIncoming(nextJ, bodyBB);

    builder.SetInsertPoint(doneBB);
    builder.CreateRetVoid();
    return f;
}

Function *RuntimeBuilder::defineRoll(Function *reverse) {
    Function *f = define("piet.roll", FunctionType::get(voidTy, { ptr, i32, i32 }, false));
    Value *base = f->getArg(0);
    Value *depth = f->getArg(1);
    Value *rolls = f->getArg(2);

    BasicBlock *normalizeBB = BasicBlock::Create(ctx, "normalize", f);
    BasicBlock *rotateBB = BasicBlock::Create(ctx, "rotate", f);
    BasicBlock *doneBB = BasicBlock::Create(ctx, "done", f);

    // Rolling fewer than two values changes nothing (and depth 0 cannot be divided by).
    builder.CreateCondBr(builder.CreateICmpSLT(depth, builder.getInt32(2)), doneBB, normalizeBB);

    // A roll of depth n by r moves every value r places towards the top, wrapping round, so
    // only r mod n (rounded towards negative infinity, in 0..n-1) matters.
    builder.SetInsertPoint(normalizeBB);
    Value *rem = builder.CreateSRem(rolls, depth);
    Value *isNegative = builder.CreateICmpSLT(rem, builder.getInt32(0));
    Value *r = builder.CreateSelect(isNegative, builder.CreateAdd(rem, depth), rem);
    builder.CreateCondBr(builder.CreateICmpEQ(r, builder.getInt32(0)), doneBB, rotateBB);

    // Rotate in place: base[n-r..n) followed by base[0..n-r) is what reversing the whole range,
    // then its first r and its last n-r values gives.
    builder.SetInsertPoint(rotateBB);
    Value *n = builder.CreateZExt(depth, i64);
    Value *k = builder.CreateZExt(r, i64);
    Value *zero = ConstantInt::get(i64, 0);
    builder.CreateCall(reverse, { base, zero, n });
    builder.CreateCall(reverse, { base, zero, k });
    builder.CreateCall(reverse, { base, k, n });
    builder.CreateBr(doneBB);

    builder.SetInsertPoint(doneBB);
    builder.CreateRetVoid();
    return f;
}

Runtime RuntimeBuilder::build() {
    Runtime runtime;
    runtime.malloc = module.getOrInsertFunction("malloc", FunctionType::get(ptr, { i64 }, false));
    runtime.free = module.getOrInsertFunction("free", FunctionType::get(voidTy, { ptr }, false));
    runtime.outOfMemory = defineOutOfMemory();
    runtime.grow = defineGrow(runtime.outOfMemory);
    runtime.roll = defineRoll(defineReverse());
    return runtime;
}

} // namespace

Runtime defineRuntime(Module &module) {
    return RuntimeBuilder(module).build();
}
