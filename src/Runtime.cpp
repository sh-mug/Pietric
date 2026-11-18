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

    // Input goes through piet.getc, which returns a character put back by
    // piet.read_number before reading a new one.
    GlobalVariable *lookahead = nullptr;
    FunctionCallee fflushF;
    Function *defineGetc();
    Function *defineReadNumber(Function *getc);
    Function *defineReadChar(Function *getc);
    Function *defineWriteNumber();
    // Flushes stdout before reading, so that a prompt is visible before the
    // program waits for input.
    void flushOutput();
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

// Rolls that move at most this many values past the others go through a buffer of this size.
constexpr int64_t kRollBufferSize = 64;

Function *RuntimeBuilder::defineRoll(Function *reverse) {
    Function *f = define("piet.roll", FunctionType::get(voidTy, { ptr, i32, i32 }, false));
    Value *base = f->getArg(0);
    Value *depth = f->getArg(1);
    Value *rolls = f->getArg(2);

    BasicBlock *normalizeBB = BasicBlock::Create(ctx, "normalize", f);
    BasicBlock *rotateBB = BasicBlock::Create(ctx, "rotate", f);
    BasicBlock *doneBB = BasicBlock::Create(ctx, "done", f);
    Value *buffer =
        builder.CreateAlloca(ArrayType::get(i32, kRollBufferSize), nullptr, "buffer");

    // Rolling fewer than two values changes nothing (and depth 0 cannot be divided by).
    builder.CreateCondBr(builder.CreateICmpSLT(depth, builder.getInt32(2)), doneBB, normalizeBB);

    // A roll of depth n by r moves every value r places towards the top, wrapping round, so
    // only r mod n (rounded towards negative infinity, in 0..n-1) matters.
    builder.SetInsertPoint(normalizeBB);
    Value *rem = builder.CreateSRem(rolls, depth);
    Value *isNegative = builder.CreateICmpSLT(rem, builder.getInt32(0));
    Value *r = builder.CreateSelect(isNegative, builder.CreateAdd(rem, depth), rem);
    builder.CreateCondBr(builder.CreateICmpEQ(r, builder.getInt32(0)), doneBB, rotateBB);

    // Rotate in place: base[n-r..n) followed by base[0..n-r). If one of the two parts is
    // short (rolls are mostly by 1 or -1), it is saved in a buffer while memmove shifts the
    // other part; otherwise reverse the whole range, then its first r and its last n-r values.
    builder.SetInsertPoint(rotateBB);
    Value *n = builder.CreateZExt(depth, i64);
    Value *k = builder.CreateZExt(r, i64);
    Value *rest = builder.CreateSub(n, k);
    Value *bufferSize = ConstantInt::get(i64, kRollBufferSize);
    BasicBlock *shortTopBB = BasicBlock::Create(ctx, "short_top", f, doneBB);
    BasicBlock *checkBottomBB = BasicBlock::Create(ctx, "check_bottom", f, doneBB);
    BasicBlock *shortBottomBB = BasicBlock::Create(ctx, "short_bottom", f, doneBB);
    BasicBlock *reverseBB = BasicBlock::Create(ctx, "reverse", f, doneBB);
    builder.CreateCondBr(builder.CreateICmpULE(k, bufferSize), shortTopBB, checkBottomBB);

    const Align align(sizeof(int32_t));
    auto bytes = [&](Value *count) {
        return builder.CreateMul(count, ConstantInt::get(i64, sizeof(int32_t)));
    };
    auto at = [&](Value *i) { return builder.CreateInBoundsGEP(i32, base, i); };

    // The top r values go to the buffer, the others r places up, then the buffer to the bottom.
    builder.SetInsertPoint(shortTopBB);
    builder.CreateMemCpy(buffer, align, at(rest), align, bytes(k));
    builder.CreateMemMove(at(k), align, base, align, bytes(rest));
    builder.CreateMemCpy(base, align, buffer, align, bytes(k));
    builder.CreateBr(doneBB);

    builder.SetInsertPoint(checkBottomBB);
    builder.CreateCondBr(builder.CreateICmpULE(rest, bufferSize), shortBottomBB, reverseBB);

    // The bottom n-r values go to the buffer, the others to the bottom, then the buffer on top.
    builder.SetInsertPoint(shortBottomBB);
    builder.CreateMemCpy(buffer, align, base, align, bytes(rest));
    builder.CreateMemMove(base, align, at(rest), align, bytes(k));
    builder.CreateMemCpy(at(k), align, buffer, align, bytes(rest));
    builder.CreateBr(doneBB);

    builder.SetInsertPoint(reverseBB);
    Value *zero = ConstantInt::get(i64, 0);
    builder.CreateCall(reverse, { base, zero, n });
    builder.CreateCall(reverse, { base, zero, k });
    builder.CreateCall(reverse, { base, k, n });
    builder.CreateBr(doneBB);

    builder.SetInsertPoint(doneBB);
    builder.CreateRetVoid();
    return f;
}

void RuntimeBuilder::flushOutput() {
    builder.CreateCall(fflushF, { ConstantPointerNull::get(ptr) });  // fflush(NULL)
}

// i32 piet.getc()
Function *RuntimeBuilder::defineGetc() {
    FunctionCallee getcharF = module.getOrInsertFunction("getchar", FunctionType::get(i32, false));
    // The character put back, or -1 (EOF) if there is none.
    lookahead = new GlobalVariable(module, i32, false, GlobalValue::InternalLinkage,
                                   builder.getInt32(-1), "piet.lookahead");

    Function *f = define("piet.getc", FunctionType::get(i32, false));
    BasicBlock *takeBB = BasicBlock::Create(ctx, "take", f);
    BasicBlock *readBB = BasicBlock::Create(ctx, "read", f);
    Value *c = builder.CreateLoad(i32, lookahead);
    builder.CreateCondBr(builder.CreateICmpSGE(c, builder.getInt32(0)), takeBB, readBB);

    builder.SetInsertPoint(takeBB);
    builder.CreateStore(builder.getInt32(-1), lookahead);
    builder.CreateRet(c);

    builder.SetInsertPoint(readBB);
    builder.CreateRet(builder.CreateCall(getcharF));
    return f;
}

Function *RuntimeBuilder::defineReadNumber(Function *getc) {
    StructType *resultTy = StructType::get(ctx, { builder.getInt1Ty(), i32 });
    Function *f = define("piet.read_number", FunctionType::get(resultTy, false));
    BasicBlock *skipBB = BasicBlock::Create(ctx, "skip", f);
    BasicBlock *signBB = BasicBlock::Create(ctx, "sign", f);
    BasicBlock *afterSignBB = BasicBlock::Create(ctx, "after_sign", f);
    BasicBlock *firstBB = BasicBlock::Create(ctx, "first", f);
    BasicBlock *failBB = BasicBlock::Create(ctx, "fail", f);
    BasicBlock *digitsBB = BasicBlock::Create(ctx, "digits", f);
    BasicBlock *doneBB = BasicBlock::Create(ctx, "done", f);
    auto isDigit = [&](Value *c) {
        return builder.CreateICmpULT(builder.CreateSub(c, builder.getInt32('0')),
                                     builder.getInt32(10));
    };
    // Puts c back; at end of input c is -1, which leaves nothing to read back.
    auto putBack = [&](Value *c) { builder.CreateStore(c, lookahead); };

    flushOutput();
    builder.CreateBr(skipBB);

    // Skip whitespace (as isspace does in the C locale: space and \t \n \v \f \r).
    builder.SetInsertPoint(skipBB);
    Value *c = builder.CreateCall(getc);
    Value *isSpace = builder.CreateOr(
        builder.CreateICmpEQ(c, builder.getInt32(' ')),
        builder.CreateICmpULT(builder.CreateSub(c, builder.getInt32('\t')), builder.getInt32(5)));
    builder.CreateCondBr(isSpace, skipBB, signBB);

    builder.SetInsertPoint(signBB);
    Value *isMinus = builder.CreateICmpEQ(c, builder.getInt32('-'));
    Value *isSign = builder.CreateOr(isMinus, builder.CreateICmpEQ(c, builder.getInt32('+')));
    builder.CreateCondBr(isSign, afterSignBB, firstBB);

    builder.SetInsertPoint(afterSignBB);
    Value *afterSign = builder.CreateCall(getc);
    builder.CreateBr(firstBB);

    builder.SetInsertPoint(firstBB);
    PHINode *first = builder.CreatePHI(i32, 2, "first");
    first->addIncoming(c, signBB);
    first->addIncoming(afterSign, afterSignBB);
    PHINode *negative = builder.CreatePHI(builder.getInt1Ty(), 2, "negative");
    negative->addIncoming(builder.getFalse(), signBB);
    negative->addIncoming(isMinus, afterSignBB);
    builder.CreateCondBr(isDigit(first), digitsBB, failBB);

    builder.SetInsertPoint(failBB);
    putBack(first);
    builder.CreateRet(ConstantStruct::get(resultTy, { builder.getFalse(), builder.getInt32(0) }));

    builder.SetInsertPoint(digitsBB);
    PHINode *magnitude = builder.CreatePHI(i32, 2, "magnitude");
    PHINode *digit = builder.CreatePHI(i32, 2, "digit");
    Value *next = builder.CreateAdd(builder.CreateMul(magnitude, builder.getInt32(10)),
                                    builder.CreateSub(digit, builder.getInt32('0')));
    Value *following = builder.CreateCall(getc);
    builder.CreateCondBr(isDigit(following), digitsBB, doneBB);
    magnitude->addIncoming(builder.getInt32(0), firstBB);
    magnitude->addIncoming(next, digitsBB);
    digit->addIncoming(first, firstBB);
    digit->addIncoming(following, digitsBB);

    builder.SetInsertPoint(doneBB);
    putBack(following);
    Value *value = builder.CreateSelect(negative, builder.CreateNeg(next), next);
    Value *result = builder.CreateInsertValue(PoisonValue::get(resultTy), builder.getTrue(), 0);
    builder.CreateRet(builder.CreateInsertValue(result, value, 1));
    return f;
}

Function *RuntimeBuilder::defineReadChar(Function *getc) {
    StructType *resultTy = StructType::get(ctx, { builder.getInt1Ty(), i32 });
    Function *f = define("piet.read_char", FunctionType::get(resultTy, false));
    flushOutput();
    Value *c = builder.CreateCall(getc);
    Value *ok = builder.CreateICmpSGE(c, builder.getInt32(0));
    Value *result = builder.CreateInsertValue(PoisonValue::get(resultTy), ok, 0);
    builder.CreateRet(builder.CreateInsertValue(result, c, 1));
    return f;
}

Function *RuntimeBuilder::defineWriteNumber() {
    FunctionCallee printfF =
        module.getOrInsertFunction("printf", FunctionType::get(i32, { ptr }, true));
    Function *f = define("piet.write_number", FunctionType::get(voidTy, { i32 }, false));
    Value *format = builder.CreateGlobalString("%d", "piet.int_format");
    builder.CreateCall(printfF, { format, f->getArg(0) });
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

    runtime.putchar = module.getOrInsertFunction("putchar", FunctionType::get(i32, { i32 }, false));
    fflushF = module.getOrInsertFunction("fflush", FunctionType::get(i32, { ptr }, false));
    Function *getc = defineGetc();
    runtime.readNumber = defineReadNumber(getc);
    runtime.readChar = defineReadChar(getc);
    runtime.writeNumber = defineWriteNumber();
    return runtime;
}

} // namespace

Runtime defineRuntime(Module &module) {
    return RuntimeBuilder(module).build();
}
