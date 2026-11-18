#include "IRBuilder.h"
#include "Runtime.h"
#include "llvm/IR/IRBuilder.h"
#include <algorithm>
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
// Upper bounds on the number of depth-tracked and of cached node copies, which keep the code size
// of large programs in check. Copies are created breadth-first from the start, so depth-tracked
// copies of shallow states, many on paths that are never taken, would use up a shared budget
// before the loops that run with deeper stacks get theirs. Past the depth-tracked budget,
// execution continues in cached copies, of which a node has at most kWindowSize + 1; past theirs,
// in generic copies.
constexpr size_t kMaxTrackedCopies = 8192;
constexpr size_t kMaxCachedCopies = 24576;
// Generic copies live in functions of their own, this many each, which enter one another by
// tail calls. In main, the generic code of a large program forms one huge loop, which loop and
// value-range analyses take quadratic time on: opt took 322 s for 12,556 generic copies, and a
// random 100x100 program, with 54,150, did not finish in half an hour.
constexpr int kGenericGroupSize = 256;
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
// Past kMaxTrackedCopies, execution continues in cached copies, and past kMaxCachedCopies in
// generic copies, which keep every value on the heap and emit push and pop inline.
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
    // highest of them. The slots for these are allocas in main and globals in the functions
    // of generic copies; sizeSlot, capacitySlot and bufferSlot are those of the code being
    // emitted.
    Value *sizeSlot = nullptr;      // i64
    Value *capacitySlot = nullptr;  // i64: how many values the buffer has room for
    Value *bufferSlot = nullptr;    // ptr
    AllocaInst *sizeLocal = nullptr, *capacityLocal = nullptr, *bufferLocal = nullptr;
    GlobalVariable *sizeGlobal = nullptr, *capacityGlobal = nullptr, *bufferGlobal = nullptr;
    GlobalVariable *entryGlobal = nullptr;

    // The functions of generic copies, each with a switch on the number in entryGlobal that
    // leads to the copies other functions enter; and the function and entry number of each
    // generic copy (-1 until another function enters it). The entry number is passed in a
    // global, not an argument, so that interprocedural constant propagation does not try to
    // specialize the functions for every entry, which takes most of opt's time.
    std::vector<Function *> genericFunctions;
    std::vector<SwitchInst *> genericEntries;
    struct GenericCopy {
        int function;
        int entry;
    };
    std::map<int, GenericCopy> genericCopies;
    Function *currentFunction() { return builder.GetInsertBlock()->getParent(); }

    // How the code being emitted keeps the stack, and the number of values in the slots (the
    // depth of a depth-tracked stack, or the size of the window; 0 in generic copies).
    Mode mode = Mode::Tracked;
    int depth = 0;
    // The values in the slots, where known in the code being emitted (else null): values are
    // reused instead of loaded again, and a copy starts with the constants that knownTops
    // finds at the top of the stack, so that a roll by pushed constants becomes a renaming of
    // slots and branches on constants need only one side.
    std::vector<Value *> known;
    struct State {
        Mode mode;
        int depth;
        std::vector<Value *> known;
    };
    State state() const { return { mode, depth, known }; }
    void restore(State saved) {
        mode = saved.mode;
        depth = saved.depth;
        known = std::move(saved.known);
    }
    int stateKey() const;
    // Sets the state for the copy of `node` with state key `key`.
    void enter(int node, int key);

    // Node copies, keyed by (node index, state key), and the ones still to be emitted.
    std::map<std::pair<int, int>, BasicBlock *> copies;
    std::deque<std::pair<int, int>> pending;
    size_t trackedCopies = 0, cachedCopies = 0;

    // For every node, the values that are at the top of the stack whenever it is entered, top
    // first, and the analysis that finds them.
    std::vector<std::vector<int32_t>> knownTops;
    void analyzeKnownTops(const std::vector<GraphNode> &nodes);

    ConstantInt *constant(int value) { return ConstantInt::get(i32, value); }
    ConstantInt *index(int64_t value) { return ConstantInt::get(i64, value); }
    bool tracked() const { return mode == Mode::Tracked; }
    bool cached() const { return mode == Mode::Cached; }
    bool generic() const { return mode == Mode::Generic; }

    // Returns the block of the copy of `node` for the state `key`, creating it if needed.
    BasicBlock *copyOf(int node, int key);
    std::string copyName(int node, int key) const;
    // Ends the current block with a branch to the copy of `node` for the current state. If that
    // would need a depth-tracked copy beyond kMaxTrackedCopies, spills first; if it would need a
    // cached copy beyond kMaxCachedCopies, moves the window to the heap and continues
    // generically.
    void jump(int node);

    // --- Stack operations, for every kind of copy ---
    // Reads and writes slot i.
    Value *getSlot(int i);
    void setSlot(int i, Value *value);
    // The number of values on the stack, as an i64.
    Value *stackSize();
    // Pops the top value. The stack must not be empty, and in a cached copy the window must
    // not be either (emitIfStackHas sees to that).
    Value *pop();
    void push(Value *value);
    // Moves a depth-tracked stack to a new heap buffer, except for (at most) the top
    // kWindowSize - kEvictCount values, and continues with these as the window.
    void spill();
    // Moves the bottom `count` values of the slots to the top of the heap stack.
    void moveToHeap(int count);
    // Moves `count` values from the top of the heap stack, which must hold them, to the bottom
    // of the window.
    void refill(int count);
    // Makes room in the heap buffer for `count` values above `size`.
    void reserve(Value *size, int count);
    // Rolls the top rollDepth values of the heap stack (the window must be empty).
    void emitHeapRoll(Value *rollDepth, Value *rolls);
    // Emits a valid roll (0 <= rollDepth <= size) in a cached copy, followed by the branch to
    // `next`.
    void emitCachedRoll(Value *rollDepth, Value *rolls, int next);
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
    // Rolls the top `rollDepth` values (0 <= rollDepth <= depth) of the slots.
    void emitTrackedRoll(Value *rollDepth, Value *rolls);
    // Rolls within slots[first..depth) (rollDepth <= depth - first) with selects.
    void rollSlots(int first, Value *rollDepth, Value *rolls);
    // Room for the slots, for rolls done in memory.
    AllocaInst *rollScratch = nullptr;
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

std::string CodeGen::copyName(int node, int stateKey) const {
    return "node" + std::to_string(node) +
           (stateKey == kGenericKey ? ".g"
            : stateKey >= kCachedKey ? ".c" + std::to_string(stateKey - kCachedKey)
                                     : ".d" + std::to_string(stateKey));
}

BasicBlock *CodeGen::copyOf(int node, int stateKey) {
    auto key = std::make_pair(node, stateKey);
    auto it = copies.find(key);
    if (it != copies.end())
        return it->second;
    Function *function = mainFunc;
    if (stateKey == kGenericKey) {
        int group = genericCopies.size() / kGenericGroupSize;
        if (group == static_cast<int>(genericFunctions.size())) {
            // Inlining the functions into one another would build the huge function up again.
            function = Function::Create(FunctionType::get(builder.getVoidTy(), false),
                                        Function::InternalLinkage,
                                        "piet.generic" + std::to_string(group), module);
            function->addFnAttr(Attribute::NoInline);
            function->addFnAttr(Attribute::NoUnwind);
            BasicBlock *entryBB = BasicBlock::Create(ctx, "entry", function);
            BasicBlock *noEntryBB = BasicBlock::Create(ctx, "no_entry", function);
            new UnreachableInst(ctx, noEntryBB);
            Value *entry = new LoadInst(i32, entryGlobal, "entry", entryBB);
            genericEntries.push_back(SwitchInst::Create(entry, noEntryBB, 0, entryBB));
            genericFunctions.push_back(function);
        }
        function = genericFunctions[group];
        genericCopies[node] = { group, -1 };
    }
    BasicBlock *bb = BasicBlock::Create(ctx, copyName(node, stateKey), function);
    copies.emplace(key, bb);
    pending.push_back(key);
    if (stateKey >= kCachedKey)
        cachedCopies++;
    else if (stateKey != kGenericKey)
        trackedCopies++;
    return bb;
}

void CodeGen::jump(int node) {
    auto isNew = [&] { return !copies.count({ node, stateKey() }); };
    if (tracked() && isNew() && trackedCopies >= kMaxTrackedCopies)
        spill();
    if (cached() && isNew() && cachedCopies >= kMaxCachedCopies) {
        moveToHeap(depth);
        mode = Mode::Generic;
    }
    BasicBlock *target = copyOf(node, stateKey());
    if (target->getParent() == currentFunction()) {
        builder.CreateBr(target);
        return;
    }
    // A generic copy in another function. From main, hand the heap stack over in the globals
    // and return when the program ends; from another function, tail-call, so that loops through
    // several functions do not grow the call stack.
    GenericCopy &copy = genericCopies[node];
    SwitchInst *entries = genericEntries[copy.function];
    if (copy.entry < 0) {
        copy.entry = entries->getNumCases();
        entries->addCase(constant(copy.entry), target);
    }
    Function *function = genericFunctions[copy.function];
    builder.CreateStore(constant(copy.entry), entryGlobal);
    if (currentFunction() == mainFunc) {
        builder.CreateStore(builder.CreateLoad(i64, sizeLocal), sizeGlobal);
        builder.CreateStore(builder.CreateLoad(i64, capacityLocal), capacityGlobal);
        builder.CreateStore(builder.CreateLoad(ptr, bufferLocal), bufferGlobal);
        builder.CreateCall(function);
        builder.CreateRet(constant(0));
    } else {
        builder.CreateCall(function)->setTailCallKind(CallInst::TCK_MustTail);
        builder.CreateRetVoid();
    }
}

void CodeGen::enter(int node, int key) {
    if (key == kGenericKey)
        mode = Mode::Generic, depth = 0;
    else if (key >= kCachedKey)
        mode = Mode::Cached, depth = key - kCachedKey;
    else
        mode = Mode::Tracked, depth = key;
    known.assign(kMaxTrackedDepth, nullptr);
    const std::vector<int32_t> &tops = knownTops[node];
    for (int i = 0; i < depth && i < static_cast<int>(tops.size()); i++)
        known[depth - 1 - i] = constant(tops[i]);
}

// The effect of a command on the known values at the top of the stack (top first): what is
// known after it, given what is known before. A command whose effect depends on values that
// are not known, or that can fail and leave the stack in another shape, leaves nothing known.
std::vector<int32_t> knownTopsAfter(Command cmd, int blockSize, const std::vector<int32_t> &in) {
    auto rest = [&](size_t count) { return std::vector<int32_t>(in.begin() + count, in.end()); };
    auto with = [](int32_t value, std::vector<int32_t> below) {
        below.insert(below.begin(), value);
        return below;
    };
    auto wrap = [](uint32_t value) { return static_cast<int32_t>(value); };
    if (cmd == Command::Push)
        return with(blockSize, in);
    if (cmd == Command::None)
        return in;
    if (in.empty())
        return {};
    int32_t top = in[0];
    switch (cmd) {
        case Command::Pop:
        case Command::OutputNum:
        case Command::OutputChar:
            return rest(1);
        case Command::Not:
            return with(top == 0, rest(1));
        case Command::Duplicate:
            return with(top, in);
        default:
            break;
    }
    if (in.size() < 2)
        return {};
    int32_t second = in[1];
    uint32_t a = static_cast<uint32_t>(second), b = static_cast<uint32_t>(top);
    switch (cmd) {
        case Command::Add:
            return with(wrap(a + b), rest(2));
        case Command::Subtract:
            return with(wrap(a - b), rest(2));
        case Command::Multiply:
            return with(wrap(a * b), rest(2));
        case Command::Greater:
            return with(second > top, rest(2));
        case Command::Divide:
            if (top == 0)
                return {};
            return with(top == -1 ? wrap(0u - a) : second / top, rest(2));
        case Command::Modulo: {
            if (top == 0)
                return {};
            int32_t rem = top == -1 ? 0 : second % top;
            if (rem != 0 && (rem ^ top) < 0)
                rem += top;
            return with(rem, rest(2));
        }
        default:
            return {};  // roll, input
    }
}

// A forward analysis over the graph: the known values on entry to a node are those that every
// way into it leaves there. A pointer or switch whose value is known takes only one way out.
void CodeGen::analyzeKnownTops(const std::vector<GraphNode> &nodes) {
    constexpr size_t kMaxKnown = kMaxTrackedDepth;
    std::vector<bool> reached(nodes.size(), false);
    knownTops.assign(nodes.size(), {});
    std::deque<int> work;
    auto flow = [&](int target, std::vector<int32_t> tops) {
        if (tops.size() > kMaxKnown)
            tops.resize(kMaxKnown);
        std::vector<int32_t> &current = knownTops[target];
        if (!reached[target]) {
            reached[target] = true;
            current = std::move(tops);
            work.push_back(target);
            return;
        }
        size_t common = 0;
        while (common < current.size() && common < tops.size() && current[common] == tops[common])
            common++;
        if (common < current.size()) {
            current.resize(common);
            work.push_back(target);
        }
    };
    flow(0, {});
    while (!work.empty()) {
        const GraphNode &node = nodes[work.front()];
        std::vector<int32_t> in = knownTops[work.front()];
        work.pop_front();
        if (node.transitions.empty())
            continue;
        Command cmd = node.transitions[0].command;
        if (cmd != Command::Pointer && cmd != Command::Switch) {
            flow(node.transitions[0].targetNode, knownTopsAfter(cmd, node.blockSize, in));
            continue;
        }
        if (in.empty()) {
            for (const GraphEdge &edge : node.transitions)
                flow(edge.targetNode, {});
            continue;
        }
        int32_t value = in[0];
        int way = cmd == Command::Pointer ? ((value % 4) + 4) % 4 : (value % 2 != 0);
        flow(node.transitions[way].targetNode, std::vector<int32_t>(in.begin() + 1, in.end()));
    }
}

Value *CodeGen::getSlot(int i) {
    if (!known[i])
        known[i] = builder.CreateLoad(i32, slots[i]);
    return known[i];
}

void CodeGen::setSlot(int i, Value *value) {
    builder.CreateStore(value, slots[i]);
    known[i] = value;
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
        return getSlot(depth);
    }
    // The callers check the size first, so it cannot wrap.
    Value *newSize = builder.CreateNUWSub(stackSize(), index(1));
    builder.CreateStore(newSize, sizeSlot);
    return builder.CreateLoad(i32, builder.CreateInBoundsGEP(i32, stackBuffer(), newSize));
}

void CodeGen::push(Value *value) {
    if (tracked() && depth == kMaxTrackedDepth)
        spill();
    else if (cached() && depth == kWindowSize)
        moveToHeap(kEvictCount);
    if (!generic()) {
        setSlot(depth, value);
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

void CodeGen::spill() {
    assert(tracked());
    Value *buffer = builder.CreateCall(
        runtime.malloc, { index(kInitialStackCapacity * sizeof(int32_t)) }, "buffer");
    emitIf(builder.CreateIsNull(buffer), [&] { builder.CreateCall(runtime.outOfMemory); });
    builder.CreateStore(index(0), sizeSlot);
    builder.CreateStore(index(kInitialStackCapacity), capacitySlot);
    builder.CreateStore(buffer, bufferSlot);
    mode = Mode::Cached;
    moveToHeap(std::max(0, depth - (kWindowSize - kEvictCount)));
}

void CodeGen::moveToHeap(int count) {
    assert(!generic() && count <= depth);
    if (count == 0)
        return;
    Value *size = builder.CreateLoad(i64, sizeSlot, "size");
    reserve(size, count);
    Value *buffer = stackBuffer();
    for (int i = 0; i < count; i++)
        builder.CreateStore(getSlot(i), builder.CreateInBoundsGEP(
                                            i32, buffer, builder.CreateNUWAdd(size, index(i))));
    for (int i = count; i < depth; i++)
        setSlot(i - count, getSlot(i));
    builder.CreateStore(builder.CreateNUWAdd(size, index(count)), sizeSlot);
    depth -= count;
}

void CodeGen::refill(int count) {
    assert(cached() && depth + count <= kWindowSize);
    for (int i = depth - 1; i >= 0; i--)
        setSlot(i + count, getSlot(i));
    Value *size = builder.CreateNUWSub(builder.CreateLoad(i64, sizeSlot, "size"), index(count));
    Value *buffer = stackBuffer();
    for (int i = 0; i < count; i++)
        setSlot(i, builder.CreateLoad(
                       i32, builder.CreateInBoundsGEP(i32, buffer,
                                                      builder.CreateNUWAdd(size, index(i)))));
    builder.CreateStore(size, sizeSlot);
    depth += count;
}

template <typename T, typename E> void CodeGen::emitBranch(Value *cond, T then, E otherwise) {
    if (auto *fixed = dyn_cast<ConstantInt>(cond)) {
        // Known at compile time: only one side can run.
        State saved = state();
        if (fixed->isOne())
            then();
        else
            otherwise();
        restore(saved);
        return;
    }
    BasicBlock *thenBB = BasicBlock::Create(ctx, "then", currentFunction());
    BasicBlock *elseBB = BasicBlock::Create(ctx, "else", currentFunction());
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
    BasicBlock *thenBB = BasicBlock::Create(ctx, "then", currentFunction());
    BasicBlock *contBB = BasicBlock::Create(ctx, "cont", currentFunction());
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
            // A known depth that the window holds is valid whatever the heap holds.
            if (auto *knownDepth = dyn_cast<ConstantInt>(rollDepth); knownDepth && cached())
                if (knownDepth->getSExtValue() >= 0 && knownDepth->getSExtValue() <= depth)
                    valid = builder.getTrue();
            emitBranch(valid,
                [&] {
                    if (cached()) {
                        emitCachedRoll(rollDepth, rolls, next);
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

// Rolls within the window if it holds the values to roll. Else, if a full window would hold
// them and the heap has the values to fill it, fills it and rolls within it: rolls of a few
// values whose depth is only known at run time are common, and the window may hold fewer
// values than they need because pops refill it only one value at a time. Else the roll reaches
// into the heap: by 1 or -1, which programs use to reach values deep in the stack, it moves
// one value past the heap values it covers and leaves the rest of the window in place;
// otherwise it moves the window to the heap and rolls there.
void CodeGen::emitCachedRoll(Value *rollDepth, Value *rolls, int next) {
    auto inWindow = [&] {
        emitTrackedRoll(rollDepth, rolls);
        jump(next);
    };
    // Deeper than the window. By 1 or -1, one value moves between the window and the heap,
    // past the heap values that the roll covers, and the rest of the window stays; other rolls
    // move the window to the heap and roll there.
    auto onHeap = [&] {
        auto throughHeap = [&] {
            moveToHeap(depth);
            emitHeapRoll(rollDepth, rolls);
            jump(next);
        };
        if (depth == 0) {
            throughHeap();
            return;
        }
        const Align align(sizeof(int32_t));
        auto bytes = [&](Value *count) {
            return builder.CreateMul(count, index(sizeof(int32_t)));
        };
        Value *covered = builder.CreateSExt(builder.CreateSub(rollDepth, constant(depth)), i64);
        // By 1 or by 1 - rollDepth, which is the same; likewise for -1 and rollDepth - 1.
        Value *byOne = builder.CreateOr(
            builder.CreateICmpEQ(rolls, constant(1)),
            builder.CreateICmpEQ(rolls, builder.CreateSub(constant(1), rollDepth)));
        Value *byMinusOne = builder.CreateOr(
            builder.CreateICmpEQ(rolls, constant(-1)),
            builder.CreateICmpEQ(rolls, builder.CreateSub(rollDepth, constant(1))));
        emitBranch(byOne,
            [&] {
                // The top value goes below the covered heap values.
                Value *top = pop();
                Value *size = builder.CreateLoad(i64, sizeSlot, "size");
                reserve(size, 1);
                Value *buffer = stackBuffer();
                Value *start = builder.CreateSub(size, covered);
                Value *at = builder.CreateInBoundsGEP(i32, buffer, start);
                builder.CreateMemMove(builder.CreateConstInBoundsGEP1_64(i32, at, 1), align, at,
                                      align, bytes(covered));
                builder.CreateStore(top, at);
                builder.CreateStore(builder.CreateNUWAdd(size, index(1)), sizeSlot);
                jump(next);
            },
            [&] {
                emitBranch(byMinusOne,
                    [&] {
                        // The lowest covered heap value comes to the top.
                        Value *size = builder.CreateLoad(i64, sizeSlot, "size");
                        Value *buffer = stackBuffer();
                        Value *start = builder.CreateSub(size, covered);
                        Value *at = builder.CreateInBoundsGEP(i32, buffer, start);
                        Value *value = builder.CreateLoad(i32, at);
                        builder.CreateMemMove(at, align,
                                              builder.CreateConstInBoundsGEP1_64(i32, at, 1),
                                              align, bytes(builder.CreateSub(covered, index(1))));
                        builder.CreateStore(builder.CreateNUWSub(size, index(1)), sizeSlot);
                        push(value);
                        jump(next);
                    },
                    throughHeap);
            });
    };
    emitBranch(builder.CreateICmpSLE(rollDepth, constant(depth)), inWindow, [&] {
        if (depth == kWindowSize) {
            onHeap();
            return;
        }
        Value *fitsWindow = builder.CreateICmpSLE(rollDepth, constant(kWindowSize));
        if (auto *fixed = dyn_cast<ConstantInt>(fitsWindow); fixed && fixed->isZero()) {
            onHeap();
            return;
        }
        int missing = kWindowSize - depth;
        Value *heapSize = builder.CreateLoad(i64, sizeSlot, "size");
        Value *fits =
            builder.CreateAnd(fitsWindow, builder.CreateICmpSGE(heapSize, index(missing)));
        emitBranch(fits,
            [&] {
                refill(missing);
                inWindow();
            },
            onHeap);
    });
}

void CodeGen::emitHeapRoll(Value *rollDepth, Value *rolls) {
    Value *size = builder.CreateLoad(i64, sizeSlot, "size");
    Value *base = builder.CreateInBoundsGEP(
        i32, stackBuffer(), builder.CreateSub(size, builder.CreateSExt(rollDepth, i64)));
    builder.CreateCall(runtime.roll, { base, rollDepth, rolls });
}

// The values stay in SSA form. A roll by known values renames slots; otherwise every slot
// gets a select between the old values, picked by arithmetic on rollDepth and rolls, which
// folds away if they become known later. As that takes a select for every pair of slots, a
// roll that may be deeper than kWindowSize selects only within the top kWindowSize slots and
// rolls deeper stacks in memory.
void CodeGen::emitTrackedRoll(Value *rollDepth, Value *rolls) {
    if (depth < 2)
        return;
    if (isa<ConstantInt>(rollDepth) || depth <= kWindowSize) {
        rollSlots(0, rollDepth, rolls);
        return;
    }
    BasicBlock *selectBB = BasicBlock::Create(ctx, "roll.select", currentFunction());
    BasicBlock *memoryBB = BasicBlock::Create(ctx, "roll.memory", currentFunction());
    BasicBlock *doneBB = BasicBlock::Create(ctx, "roll.done", currentFunction());
    builder.CreateCondBr(builder.CreateICmpSLE(rollDepth, constant(kWindowSize)), selectBB,
                         memoryBB);
    std::vector<Value *> saved = known;

    builder.SetInsertPoint(selectBB);
    rollSlots(depth - kWindowSize, rollDepth, rolls);
    builder.CreateBr(doneBB);

    builder.SetInsertPoint(memoryBB);
    known = saved;
    for (int i = 0; i < depth; i++)
        builder.CreateStore(getSlot(i), builder.CreateConstInBoundsGEP1_64(i32, rollScratch, i));
    Value *base = builder.CreateInBoundsGEP(
        i32, rollScratch, builder.CreateSub(index(depth), builder.CreateSExt(rollDepth, i64)));
    builder.CreateCall(runtime.roll, { base, rollDepth, rolls });
    for (int i = 0; i < depth; i++)
        setSlot(i,
                builder.CreateLoad(i32, builder.CreateConstInBoundsGEP1_64(i32, rollScratch, i)));
    builder.CreateBr(doneBB);

    // The slots differ between the two ways; load them again.
    builder.SetInsertPoint(doneBB);
    std::fill(known.begin(), known.end(), nullptr);
}

void CodeGen::rollSlots(int first, Value *rollDepth, Value *rolls) {
    int width = depth - first;
    std::vector<Value *> old;
    for (int i = first; i < depth; i++)
        old.push_back(getSlot(i));

    // A roll of depth n by r moves every value r places towards the top, wrapping round, so
    // only r mod n (rounded towards negative infinity, in 0..n-1) matters. Depths 0 and 1
    // change nothing; dividing by 1 instead gives r = 0 for them.
    Value *n = builder.CreateSelect(builder.CreateICmpSGT(rollDepth, constant(1)), rollDepth,
                                    constant(1));
    Value *rem = builder.CreateSRem(rolls, n);
    Value *r = builder.CreateSelect(builder.CreateICmpSLT(rem, constant(0)),
                                    builder.CreateAdd(rem, n), rem);
    // The first rolled position, counted from `first`.
    Value *base = builder.CreateSub(constant(width), rollDepth);

    std::vector<Value *> rolled;
    for (int i = 0; i < width; i++) {
        // Position i receives the value from position base + ((i - base - r) mod n), if it is
        // among the rolled values.
        Value *offset = builder.CreateSub(constant(i), base);
        Value *from = builder.CreateSub(offset, r);
        from = builder.CreateSelect(builder.CreateICmpSLT(from, constant(0)),
                                    builder.CreateAdd(from, n), from);
        Value *source = builder.CreateSelect(builder.CreateICmpSGE(offset, constant(0)),
                                             builder.CreateAdd(base, from), constant(i));
        if (auto *knownSource = dyn_cast<ConstantInt>(source)) {
            // rollDepth and rolls are known: the roll renames slots. (A source outside the
            // stack means that rollDepth is out of range, on a path that is never taken.)
            uint64_t from = knownSource->getZExtValue();
            rolled.push_back(from < old.size() ? old[from] : old[i]);
            continue;
        }
        Value *value = old[width - 1];
        for (int j = width - 2; j >= 0; j--)
            value = builder.CreateSelect(builder.CreateICmpEQ(source, constant(j)), old[j], value);
        rolled.push_back(value);
    }
    for (int i = 0; i < width; i++)
        setSlot(first + i, rolled[i]);
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
            if (auto *knownIndex = dyn_cast<ConstantInt>(index)) {
                jump(node.transitions[knownIndex->getZExtValue()].targetNode);
                return;
            }
            // Each successor gets its own block, so that a copy whose depth-tracked budget
            // runs out can spill on the way.
            BasicBlock *switchBB = builder.GetInsertBlock();
            std::vector<BasicBlock *> targets;
            State saved = state();
            for (int j = 0; j < numEdges; j++) {
                targets.push_back(BasicBlock::Create(ctx, "case", currentFunction()));
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
        if (currentFunction() == mainFunc)
            builder.CreateRet(constant(0));
        else
            builder.CreateRetVoid();
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
    sizeLocal = builder.CreateAlloca(i64, nullptr, "size.slot");
    capacityLocal = builder.CreateAlloca(i64, nullptr, "capacity.slot");
    bufferLocal = builder.CreateAlloca(ptr, nullptr, "buffer.slot");
    // External (but hidden) linkage keeps interprocedural constant propagation from tracking
    // every store to these globals, which takes it long in large programs.
    auto global = [&](Type *type, const char *name) {
        auto *variable = new GlobalVariable(module, type, false, GlobalValue::ExternalLinkage,
                                            Constant::getNullValue(type), name);
        variable->setVisibility(GlobalValue::HiddenVisibility);
        return variable;
    };
    sizeGlobal = global(i64, "piet.size");
    capacityGlobal = global(i64, "piet.capacity");
    bufferGlobal = global(ptr, "piet.buffer");
    entryGlobal = global(i32, "piet.entry");
    rollScratch =
        builder.CreateAlloca(ArrayType::get(i32, kMaxTrackedDepth), nullptr, "roll.slots");

    analyzeKnownTops(nodes);

    builder.CreateCall(runtime.start);
    // Execution starts at node 0 with an empty stack.
    sizeSlot = sizeLocal, capacitySlot = capacityLocal, bufferSlot = bufferLocal;
    enter(0, 0);
    jump(0);

    while (!pending.empty()) {
        auto [nodeIndex, key] = pending.front();
        pending.pop_front();
        builder.SetInsertPoint(copies[{ nodeIndex, key }]);
        bool inMain = key != kGenericKey;
        sizeSlot = inMain ? static_cast<Value *>(sizeLocal) : sizeGlobal;
        capacitySlot = inMain ? static_cast<Value *>(capacityLocal) : capacityGlobal;
        bufferSlot = inMain ? static_cast<Value *>(bufferLocal) : bufferGlobal;
        enter(nodeIndex, key);
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
