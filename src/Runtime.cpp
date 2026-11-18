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
    Function *defineOutOfMemory(Function *flush);
    Function *defineGrow(Function *outOfMemory);
    Function *defineReverse();
    Function *defineRoll(Function *reverse);

    // Output is collected in piet.out (piet.out_length bytes so far).
    // piet.out_lines is set if stdout is a terminal: then each newline is
    // written out at once, as stdio does.
    GlobalVariable *outBuffer = nullptr, *outLength = nullptr, *outLines = nullptr;
    Function *flushF = nullptr;
    Function *defineFlush();
    Function *defineStart();
    Function *defineWriteChar();
    Function *defineWriteNumber();

    // Input goes through piet.getc, which returns a character put back by
    // piet.read_number before reading a new one.
    GlobalVariable *lookahead = nullptr;
    Function *defineGetc();
    Function *defineReadNumber(Function *getc);
    Function *defineReadChar(Function *getc);
    // Writes out the buffered output before reading, so that a prompt is
    // visible before the program waits for input.
    void flushOutput();
};

// The size of the output buffer, and the longest decimal number (-2147483648).
constexpr int64_t kOutputBufferSize = 1 << 16;
constexpr int64_t kMaxNumberLength = 11;

RuntimeBuilder::RuntimeBuilder(Module &module)
    : module(module), ctx(module.getContext()), builder(ctx), voidTy(builder.getVoidTy()),
      i32(builder.getInt32Ty()), i64(builder.getInt64Ty()), ptr(builder.getPtrTy()) { }

Function *RuntimeBuilder::define(const char *name, FunctionType *type) {
    Function *f = Function::Create(type, Function::InternalLinkage, name, module);
    f->addFnAttr(Attribute::NoUnwind);
    builder.SetInsertPoint(BasicBlock::Create(ctx, "entry", f));
    return f;
}

Function *RuntimeBuilder::defineOutOfMemory(Function *flush) {
    FunctionCallee perrorF =
        module.getOrInsertFunction("perror", FunctionType::get(voidTy, { ptr }, false));
    FunctionCallee exitF =
        module.getOrInsertFunction("exit", FunctionType::get(voidTy, { i32 }, false));

    Function *f = define("piet.out_of_memory", FunctionType::get(voidTy, false));
    f->addFnAttr(Attribute::NoReturn);
    f->addFnAttr(Attribute::Cold);
    builder.CreateCall(flush);  // what the program printed before running out
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

    BasicBlock *byOneBB = BasicBlock::Create(ctx, "by_one", f, normalizeBB);
    BasicBlock *downBB = BasicBlock::Create(ctx, "top_down", f, normalizeBB);
    BasicBlock *byMinusOneBB = BasicBlock::Create(ctx, "by_minus_one", f, normalizeBB);
    BasicBlock *upBB = BasicBlock::Create(ctx, "bottom_up", f, normalizeBB);
    // Rolling fewer than two values changes nothing (and depth 0 cannot be divided by).
    builder.CreateCondBr(builder.CreateICmpSLT(depth, builder.getInt32(2)), doneBB, byOneBB);

    // Rolls by 1 and -1 are the most common (also written as by 1 - n and n - 1). They move one
    // value past the others, which needs neither the division below nor the buffer.
    builder.SetInsertPoint(byOneBB);
    const Align align(sizeof(int32_t));
    Value *last = builder.CreateSub(builder.CreateZExt(depth, i64), ConstantInt::get(i64, 1));
    Value *lastAt = builder.CreateInBoundsGEP(i32, base, last);
    Value *secondAt = builder.CreateConstInBoundsGEP1_64(i32, base, 1);
    Value *othersBytes = builder.CreateMul(last, ConstantInt::get(i64, sizeof(int32_t)));
    Value *byOne = builder.CreateOr(
        builder.CreateICmpEQ(rolls, builder.getInt32(1)),
        builder.CreateICmpEQ(rolls, builder.CreateSub(builder.getInt32(1), depth)));
    builder.CreateCondBr(byOne, downBB, byMinusOneBB);

    // By 1: the top value goes to the bottom.
    builder.SetInsertPoint(downBB);
    Value *top = builder.CreateLoad(i32, lastAt);
    builder.CreateMemMove(secondAt, align, base, align, othersBytes);
    builder.CreateStore(top, base);
    builder.CreateBr(doneBB);

    // By -1: the bottom value goes to the top.
    builder.SetInsertPoint(byMinusOneBB);
    Value *byMinusOne = builder.CreateOr(
        builder.CreateICmpEQ(rolls, builder.getInt32(-1)),
        builder.CreateICmpEQ(rolls, builder.CreateSub(depth, builder.getInt32(1))));
    builder.CreateCondBr(byMinusOne, upBB, normalizeBB);
    builder.SetInsertPoint(upBB);
    Value *bottom = builder.CreateLoad(i32, base);
    builder.CreateMemMove(base, align, secondAt, align, othersBytes);
    builder.CreateStore(bottom, lastAt);
    builder.CreateBr(doneBB);

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
    builder.CreateCall(flushF);
}

// void piet.flush(): writes out piet.out[0..piet.out_length). Write errors (say, a closed
// pipe) drop the output, as they would with stdio.
Function *RuntimeBuilder::defineFlush() {
    FunctionCallee writeF =
        module.getOrInsertFunction("write", FunctionType::get(i64, { i32, ptr, i64 }, false));
    Function *f = define("piet.flush", FunctionType::get(voidTy, false));
    f->addFnAttr(Attribute::NoInline);
    BasicBlock *entryBB = builder.GetInsertBlock();
    BasicBlock *writeBB = BasicBlock::Create(ctx, "write", f);
    BasicBlock *nextBB = BasicBlock::Create(ctx, "next", f);
    BasicBlock *doneBB = BasicBlock::Create(ctx, "done", f);
    Value *length = builder.CreateLoad(i64, outLength);
    builder.CreateCondBr(builder.CreateICmpEQ(length, ConstantInt::get(i64, 0)), doneBB, writeBB);

    // write() may write less than asked; continue with the rest.
    builder.SetInsertPoint(writeBB);
    PHINode *offset = builder.CreatePHI(i64, 2, "offset");
    offset->addIncoming(ConstantInt::get(i64, 0), entryBB);
    Value *from = builder.CreateInBoundsGEP(builder.getInt8Ty(), outBuffer, offset);
    Value *left = builder.CreateSub(length, offset);
    Value *written = builder.CreateCall(writeF, { builder.getInt32(1), from, left });
    builder.CreateCondBr(builder.CreateICmpSGT(written, ConstantInt::get(i64, 0)), nextBB, doneBB);

    builder.SetInsertPoint(nextBB);
    Value *nextOffset = builder.CreateAdd(offset, written);
    offset->addIncoming(nextOffset, nextBB);
    builder.CreateCondBr(builder.CreateICmpSLT(nextOffset, length), writeBB, doneBB);

    builder.SetInsertPoint(doneBB);
    builder.CreateStore(ConstantInt::get(i64, 0), outLength);
    builder.CreateRetVoid();
    return f;
}

// void piet.start(): buffers stdout by lines if it is a terminal.
Function *RuntimeBuilder::defineStart() {
    FunctionCallee isattyF =
        module.getOrInsertFunction("isatty", FunctionType::get(i32, { i32 }, false));
    Function *f = define("piet.start", FunctionType::get(voidTy, false));
    Value *terminal = builder.CreateCall(isattyF, { builder.getInt32(1) });
    builder.CreateStore(builder.CreateICmpNE(terminal, builder.getInt32(0)), outLines);
    builder.CreateRetVoid();
    return f;
}

// void piet.write_char(i32 c)
Function *RuntimeBuilder::defineWriteChar() {
    Function *f = define("piet.write_char", FunctionType::get(voidTy, { i32 }, false));
    BasicBlock *entryBB = builder.GetInsertBlock();
    BasicBlock *fullBB = BasicBlock::Create(ctx, "full", f);
    BasicBlock *storeBB = BasicBlock::Create(ctx, "store", f);
    BasicBlock *lineBB = BasicBlock::Create(ctx, "line", f);
    BasicBlock *doneBB = BasicBlock::Create(ctx, "done", f);
    Value *length = builder.CreateLoad(i64, outLength);
    Value *isFull = builder.CreateICmpEQ(length, ConstantInt::get(i64, kOutputBufferSize));
    builder.CreateCondBr(isFull, fullBB, storeBB);

    builder.SetInsertPoint(fullBB);
    flushOutput();
    builder.CreateBr(storeBB);

    builder.SetInsertPoint(storeBB);
    PHINode *at = builder.CreatePHI(i64, 2, "at");
    at->addIncoming(length, entryBB);
    at->addIncoming(ConstantInt::get(i64, 0), fullBB);
    Value *byte = builder.CreateTrunc(f->getArg(0), builder.getInt8Ty());
    builder.CreateStore(byte, builder.CreateInBoundsGEP(builder.getInt8Ty(), outBuffer, at));
    builder.CreateStore(builder.CreateAdd(at, ConstantInt::get(i64, 1)), outLength);
    Value *isNewline = builder.CreateICmpEQ(byte, builder.getInt8('\n'));
    builder.CreateCondBr(isNewline, lineBB, doneBB);

    builder.SetInsertPoint(lineBB);
    BasicBlock *flushBB = BasicBlock::Create(ctx, "flush", f, doneBB);
    builder.CreateCondBr(builder.CreateLoad(builder.getInt1Ty(), outLines), flushBB, doneBB);
    builder.SetInsertPoint(flushBB);
    flushOutput();
    builder.CreateBr(doneBB);

    builder.SetInsertPoint(doneBB);
    builder.CreateRetVoid();
    return f;
}

// void piet.write_number(i32 v): writes v in decimal (without a newline).
Function *RuntimeBuilder::defineWriteNumber() {
    Type *i8 = builder.getInt8Ty();
    Function *f = define("piet.write_number", FunctionType::get(voidTy, { i32 }, false));
    Value *value = f->getArg(0);
    BasicBlock *entryBB = builder.GetInsertBlock();
    BasicBlock *fullBB = BasicBlock::Create(ctx, "full", f);
    BasicBlock *convertBB = BasicBlock::Create(ctx, "convert", f);
    BasicBlock *digitsBB = BasicBlock::Create(ctx, "digits", f);
    BasicBlock *copyBB = BasicBlock::Create(ctx, "copy", f);
    BasicBlock *doneBB = BasicBlock::Create(ctx, "done", f);
    // The digits, last one first.
    Value *digits = builder.CreateAlloca(ArrayType::get(i8, kMaxNumberLength), nullptr, "digits");
    Value *length = builder.CreateLoad(i64, outLength);
    Value *isFull = builder.CreateICmpSGT(
        length, ConstantInt::get(i64, kOutputBufferSize - kMaxNumberLength));
    builder.CreateCondBr(isFull, fullBB, convertBB);

    builder.SetInsertPoint(fullBB);
    flushOutput();
    builder.CreateBr(convertBB);

    // The magnitude, as an unsigned number (so that -2147483648 has one too).
    builder.SetInsertPoint(convertBB);
    PHINode *start = builder.CreatePHI(i64, 2, "start");
    start->addIncoming(length, entryBB);
    start->addIncoming(ConstantInt::get(i64, 0), fullBB);
    Value *negative = builder.CreateICmpSLT(value, builder.getInt32(0));
    Value *magnitude =
        builder.CreateSelect(negative, builder.CreateSub(builder.getInt32(0), value), value);
    builder.CreateBr(digitsBB);

    builder.SetInsertPoint(digitsBB);
    PHINode *count = builder.CreatePHI(i64, 2, "count");
    PHINode *rest = builder.CreatePHI(i32, 2, "rest");
    Value *lastDigit = builder.CreateURem(rest, builder.getInt32(10));
    Value *digit = builder.CreateAdd(builder.CreateTrunc(lastDigit, i8), builder.getInt8('0'));
    builder.CreateStore(digit, builder.CreateInBoundsGEP(i8, digits, count));
    Value *nextCount = builder.CreateAdd(count, ConstantInt::get(i64, 1));
    Value *nextRest = builder.CreateUDiv(rest, builder.getInt32(10));
    count->addIncoming(ConstantInt::get(i64, 0), convertBB);
    count->addIncoming(nextCount, digitsBB);
    rest->addIncoming(magnitude, convertBB);
    rest->addIncoming(nextRest, digitsBB);
    builder.CreateCondBr(builder.CreateICmpNE(nextRest, builder.getInt32(0)), digitsBB, copyBB);

    // The sign, then the digits in order.
    builder.SetInsertPoint(copyBB);
    BasicBlock *signBB = builder.GetInsertBlock();
    Value *signAt = builder.CreateInBoundsGEP(i8, outBuffer, start);
    builder.CreateStore(builder.getInt8('-'), signAt);  // overwritten below if not negative
    Value *first = builder.CreateAdd(start, builder.CreateZExt(negative, i64));
    BasicBlock *loopBB = BasicBlock::Create(ctx, "copy_digit", f, doneBB);
    builder.CreateBr(loopBB);
    builder.SetInsertPoint(loopBB);
    PHINode *i = builder.CreatePHI(i64, 2, "i");
    i->addIncoming(ConstantInt::get(i64, 0), signBB);
    Value *from = builder.CreateSub(builder.CreateSub(nextCount, i), ConstantInt::get(i64, 1));
    Value *c = builder.CreateLoad(i8, builder.CreateInBoundsGEP(i8, digits, from));
    builder.CreateStore(c, builder.CreateInBoundsGEP(i8, outBuffer, builder.CreateAdd(first, i)));
    Value *nextI = builder.CreateAdd(i, ConstantInt::get(i64, 1));
    i->addIncoming(nextI, loopBB);
    builder.CreateCondBr(builder.CreateICmpULT(nextI, nextCount), loopBB, doneBB);

    builder.SetInsertPoint(doneBB);
    builder.CreateStore(builder.CreateAdd(first, nextCount), outLength);
    builder.CreateRetVoid();
    return f;
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

Runtime RuntimeBuilder::build() {
    Runtime runtime;
    runtime.malloc = module.getOrInsertFunction("malloc", FunctionType::get(ptr, { i64 }, false));
    runtime.free = module.getOrInsertFunction("free", FunctionType::get(voidTy, { ptr }, false));

    ArrayType *outTy = ArrayType::get(builder.getInt8Ty(), kOutputBufferSize);
    outBuffer = new GlobalVariable(module, outTy, false, GlobalValue::InternalLinkage,
                                   ConstantAggregateZero::get(outTy), "piet.out");
    outLength = new GlobalVariable(module, i64, false, GlobalValue::InternalLinkage,
                                   ConstantInt::get(i64, 0), "piet.out_length");
    outLines = new GlobalVariable(module, builder.getInt1Ty(), false, GlobalValue::InternalLinkage,
                                  builder.getFalse(), "piet.out_lines");
    runtime.flush = flushF = defineFlush();
    runtime.start = defineStart();
    runtime.writeChar = defineWriteChar();
    runtime.writeNumber = defineWriteNumber();

    runtime.outOfMemory = defineOutOfMemory(flushF);
    runtime.grow = defineGrow(runtime.outOfMemory);
    runtime.roll = defineRoll(defineReverse());

    Function *getc = defineGetc();
    runtime.readNumber = defineReadNumber(getc);
    runtime.readChar = defineReadChar(getc);
    return runtime;
}

} // namespace

Runtime defineRuntime(Module &module) {
    return RuntimeBuilder(module).build();
}
