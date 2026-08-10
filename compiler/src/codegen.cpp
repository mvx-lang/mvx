/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.  There is NO WARRANTY, to
 * the extent permitted by law; see the LICENSE file for details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

// AST -> LLVM IR -> object file.
//
// All values live in 32-byte mv_value slots and mutation goes through
// runtime calls; see DECISIONS.md.  The evalInto/evalPtr split is the seam
// where compiler type specialisation (ARCHITECTURE.md 3.3 option 3) will
// plug in: a provably-numeric variable's slot can become a bare i64 alloca
// behind these helpers without touching the parser.

#include "codegen.h"
#include "parser.h"     // CompileError

#include "llvm/Config/llvm-config.h"   // LLVM_VERSION_MAJOR
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>

using namespace llvm;

namespace mvx {

namespace {

// LLVM version shims: the codebase tracks the newest LLVM, but the
// containers and packaged distributions build against the latest
// *released* one.  These smooth over the API renames between them.
inline Function *intrinsicDecl(Module *m, Intrinsic::ID id,
                               ArrayRef<Type *> tys) {
#if LLVM_VERSION_MAJOR >= 20
    return Intrinsic::getOrInsertDeclaration(m, id, tys);
#else
    return Intrinsic::getDeclaration(m, id, tys);
#endif
}

const std::set<std::string> kIntrinsics = {
    "TIME", "SYSTEM", "INT", "SQRT", "ABS", "MOD",
    "PWR", "LN", "EXP", "SIN", "COS", "TAN", "ATAN",
};

// String-valued dynamic-array intrinsics (boxed path only).
const std::set<std::string> kStrIntrinsics = {
    "EXTRACT", "REPLACE", "INSERT", "DELETE",
};

// Integer-valued intrinsics whose arguments are strings.
const std::set<std::string> kIntIntrinsics = {
    "LEN", "COUNT", "DCOUNT", "SEQ", "INDEX", "NUM", "STATUS", "ALPHA", "CATALOGED",
    "CREATEFILE", "DELETEFILE", "COMPILE", "DATE",
    "INDEXBUILD", "INDEXDROP", "INDEXSELECT", "RND", "MAPBUILD", "MAPDROP",
    "MAPCHECK", "QUERYSELECT", "TRANSSELECT", "QUERYCOUNT", "ORDERSELECT",
    "MULTISELECT", "TRANSORDERSELECT",
    "OSWRITE", "OSDELETE", "OSEXEC", "MKDIR", "RMTREE", "UNTAR",
    "EDITFILE", "SETCRED", "SETCONN",
};

// String-valued intrinsics (boxed results).
const std::set<std::string> kStrFns = {
    "CHAR", "STR", "SPACE", "TRIM", "FIELD",
};

// System mark constants: @AM/@FM, @VM, @SM/@SVM.
inline int sysConstChar(const std::string &n) {
    if (n == "@AM" || n == "@FM") return 0xFE;
    if (n == "@VM") return 0xFD;
    if (n == "@SM" || n == "@SVM") return 0xFC;
    return -1;
}

// --------------------------------------------------------------------------
// Numeric specialisation analysis (ARCHITECTURE.md 3.3, option 3).
//
// A scalar or DIM'd array is "numeric" when every value stored into it is a
// provably numeric expression and it never escapes by reference (CALL
// argument, subroutine parameter).  Numeric scalars compile to a bare
// double alloca and numeric arrays to a flat double buffer; expressions
// over them lower to native FP instructions with no runtime calls.
// Demotion iterates to a fixed point because expression numericity depends
// on variable numericity.
//
// Two specialised tiers over the boxed representation:
//   Int — provably integral: bare i64 storage, native integer ops.
//         Deviation: i64 arithmetic wraps on overflow where boxed
//         arithmetic promotes to double; accepted and documented.
//   Dbl — provably numeric: bare double storage, native FP ops.  Boxed
//         arithmetic already promotes through double and compares
//         numerically via double, so this tier is exact to 2^53.
// Division always yields Dbl because MV division is fractional.
// Arrays additionally specialise to i8 storage when every value stored
// is an integer literal in 0..255 (flag arrays — the sieve's case).
enum class NK { Int, Dbl, NotNum };          // lattice: Int < Dbl < NotNum

inline NK joinNK(NK a, NK b) { return a > b ? a : b; }

class NumericAnalysis {
public:
    enum class ArrClass { Boxed, F64, I64, I8 };

    void run(const Program &prog) {
        for (const auto &p : prog.params) varK_[p] = NK::NotNum;
        collect(prog.body);
        bool changed = true;
        while (changed) {
            changed = false;
            scan(prog.body, changed);
        }
    }

    NK varKind(const std::string &n) const {
        if (arrays_.count(n)) return NK::NotNum;
        if (!n.empty() && n[0] == '@') return NK::NotNum;  // system vars
        auto it = varK_.find(n);
        return it == varK_.end() ? NK::Int : it->second;
    }
    bool numericVar(const std::string &n) const {
        return varKind(n) != NK::NotNum;
    }

    ArrClass arrClass(const std::string &n) const {
        auto it = arrK_.find(n);
        NK k = it == arrK_.end() ? NK::Int : it->second;
        if (!arrays_.count(n) || k == NK::NotNum) return ArrClass::Boxed;
        if (k == NK::Dbl) return ArrClass::F64;
        return byteOnly_.count(n) ? ArrClass::I8 : ArrClass::I64;
    }
    bool numericArray(const std::string &n) const {
        return arrClass(n) != ArrClass::Boxed;
    }

    bool numericExpr(const Expr &e) const {
        return kindOf(e) != NK::NotNum;
    }

    NK kindOf(const Expr &e) const {
        switch (e.kind) {
        case Expr::K::IntLit: return NK::Int;
        case Expr::K::FltLit: return NK::Dbl;
        case Expr::K::StrLit: return NK::NotNum;
        case Expr::K::Var:    return varKind(e.sval);
        case Expr::K::Neg:    return kindOf(*e.lhs);
        case Expr::K::Not:
            return numericExpr(*e.lhs) ? NK::Int : NK::NotNum;
        case Expr::K::Extract: return NK::NotNum;
        case Expr::K::Substr:  return NK::NotNum;
        case Expr::K::Fmt:     return NK::NotNum;
        case Expr::K::Paren: {
            if (arrays_.count(e.sval))
                switch (arrClass(e.sval)) {
                case ArrClass::I8:
                case ArrClass::I64: return NK::Int;
                case ArrClass::F64: return NK::Dbl;
                default:            return NK::NotNum;
                }
            if (kIntIntrinsics.count(e.sval)) return NK::Int;
            if (!kIntrinsics.count(e.sval)) return NK::NotNum;
            for (const auto &a : e.args)
                if (!numericExpr(*a)) return NK::NotNum;
            const std::string &f = e.sval;
            if (f == "TIME" || f == "SYSTEM" || f == "INT") return NK::Int;
            if (f == "SQRT" || f == "PWR" || f == "LN" || f == "EXP" ||
                f == "SIN" || f == "COS" || f == "TAN" || f == "ATAN")
                return NK::Dbl;
            if (f == "ABS")  return kindOf(*e.args[0]);
            /* MOD */
            return joinNK(kindOf(*e.args[0]), kindOf(*e.args[1]));
        }
        case Expr::K::Bin: {
            if (e.op == BinOp::Cat) return NK::NotNum;
            NK l = kindOf(*e.lhs), r = kindOf(*e.rhs);
            if (l == NK::NotNum || r == NK::NotNum) return NK::NotNum;
            switch (e.op) {
            case BinOp::Add: case BinOp::Sub: case BinOp::Mul:
                return joinNK(l, r);
            case BinOp::Div: case BinOp::Pow:
                return NK::Dbl;
            default:                       // comparisons, AND, OR: 0 / 1
                return NK::Int;
            }
        }
        }
        return NK::NotNum;
    }

private:
    std::set<std::string> arrays_, byteUnsafe_, byteOnly_;
    std::map<std::string, NK> varK_, arrK_;

    static bool isByteLit(const Expr &e) {
        return e.kind == Expr::K::IntLit && e.ival >= 0 && e.ival <= 255;
    }

    void collect(const std::vector<StmtP> &stmts) {
        for (const auto &sp : stmts) {
            const Stmt &s = *sp;
            if (s.kind == Stmt::K::Dim) arrays_.insert(s.name);
            if (s.kind == Stmt::K::Call) {
                // By-reference arguments escape: the callee may store
                // anything into them.
                for (const auto &a : s.args) {
                    if (a->kind == Expr::K::Var)
                        varK_[a->sval] = NK::NotNum;
                    if (a->kind == Expr::K::Paren)
                        arrK_[a->sval] = NK::NotNum;
                }
            }
            if (s.kind == Stmt::K::Assign &&
                s.target->kind == Expr::K::Paren &&
                !isByteLit(*s.value))
                byteUnsafe_.insert(s.target->sval);
            if (s.kind == Stmt::K::Mat) {
                if (s.name2.empty() ? !isByteLit(*s.value) : true)
                    byteUnsafe_.insert(s.name);
                if (!s.name2.empty()) byteUnsafe_.insert(s.name2);
            }
            if (s.kind == Stmt::K::MatRead || s.kind == Stmt::K::MatWrite ||
                s.kind == Stmt::K::MatParse || s.kind == Stmt::K::MatBuild)
                byteUnsafe_.insert(s.name);
            if (s.kind == Stmt::K::Common) {
                // COMMON storage is shared across programs: always boxed.
                for (const auto &item : s.args) {
                    if (item->kind == Expr::K::Var)
                        varK_[item->sval] = NK::NotNum;
                    else {
                        arrays_.insert(item->sval);
                        arrK_[item->sval] = NK::NotNum;
                    }
                }
            }
            collect(s.body); collect(s.elseBody);
            collect(s.lockedBody); collect(s.errorBody);
            collect(s.pre);  collect(s.post);
        }
    }

    void joinVar(const std::string &n, NK k, bool &changed) {
        NK cur = varK_.count(n) ? varK_[n] : NK::Int;
        NK nw = joinNK(cur, k);
        if (nw != cur) { varK_[n] = nw; changed = true; }
    }
    void joinArr(const std::string &n, NK k, bool &changed) {
        NK cur = arrK_.count(n) ? arrK_[n] : NK::Int;
        NK nw = joinNK(cur, k);
        if (nw != cur) { arrK_[n] = nw; changed = true; }
    }

    void scan(const std::vector<StmtP> &stmts, bool &changed) {
        for (const auto &sp : stmts) {
            const Stmt &s = *sp;
            switch (s.kind) {
            case Stmt::K::Assign: {
                const Expr &t = *s.target;
                NK k = kindOf(*s.value);
                if (t.kind == Expr::K::Var) joinVar(t.sval, k, changed);
                if (t.kind == Expr::K::Paren && arrays_.count(t.sval))
                    joinArr(t.sval, k, changed);
                if (t.kind == Expr::K::Extract) {
                    // Dynamic-array replace stores marks into the base.
                    if (t.lhs->kind == Expr::K::Var)
                        joinVar(t.lhs->sval, NK::NotNum, changed);
                    if (t.lhs->kind == Expr::K::Paren)
                        joinArr(t.lhs->sval, NK::NotNum, changed);
                }
                break;
            }
            case Stmt::K::For: {
                NK k = joinNK(kindOf(*s.from), kindOf(*s.to));
                if (s.step) k = joinNK(k, kindOf(*s.step));
                joinVar(s.name, k, changed);
                break;
            }
            case Stmt::K::Input:
                // Reads an arbitrary line into the target.
                if (s.target->kind == Expr::K::Var)
                    joinVar(s.target->sval, NK::NotNum, changed);
                else
                    joinArr(s.target->sval, NK::NotNum, changed);
                break;
            case Stmt::K::Mat:
                if (!s.name2.empty()) {
                    // MAT copy runs through the boxed representation.
                    joinArr(s.name, NK::NotNum, changed);
                    joinArr(s.name2, NK::NotNum, changed);
                } else {
                    joinArr(s.name, kindOf(*s.value), changed);
                }
                break;
            case Stmt::K::MatRead:
            case Stmt::K::MatWrite:
            case Stmt::K::MatParse:
                // Record I/O runs through the boxed representation.
                joinArr(s.name, NK::NotNum, changed);
                break;
            case Stmt::K::MatBuild:
                joinArr(s.name, NK::NotNum, changed);
                if (s.target->kind == Expr::K::Var)
                    joinVar(s.target->sval, NK::NotNum, changed);
                else
                    joinArr(s.target->sval, NK::NotNum, changed);
                break;
            case Stmt::K::Open:
            case Stmt::K::Readnext:
                joinVar(s.name, NK::NotNum, changed);
                break;
            case Stmt::K::ReadF:
            case Stmt::K::ReadV:
                if (s.target->kind == Expr::K::Var)
                    joinVar(s.target->sval, NK::NotNum, changed);
                else
                    joinArr(s.target->sval, NK::NotNum, changed);
                break;
            case Stmt::K::Execute:
                if (!s.name.empty()) joinVar(s.name, NK::NotNum, changed);
                if (!s.name2.empty()) joinVar(s.name2, NK::NotNum, changed);
                break;
            default:
                break;
            }
            scan(s.body, changed); scan(s.elseBody, changed);
            scan(s.lockedBody, changed); scan(s.errorBody, changed);
            scan(s.pre, changed);  scan(s.post, changed);
        }
        // byteOnly_ is derived, not part of the fixed point
        byteOnly_.clear();
        for (const auto &a : arrays_)
            if (!byteUnsafe_.count(a)) byteOnly_.insert(a);
    }
};

class CodeGen {
public:
    CodeGen(const Program &prog, const CodegenOptions &opts)
        : prog_(prog), opts_(opts),
          item_(std::filesystem::path(prog.sourcePath).filename().string()),
          mod_(item_, llctx_), b_(llctx_), eb_(llctx_), dib_(mod_) {}

    void run(const std::string &outPath);

private:
    const Program &prog_;
    const CodegenOptions &opts_;
    std::string item_;

    LLVMContext llctx_;
    Module mod_;
    IRBuilder<> b_;         // statement stream
    IRBuilder<> eb_;        // entry block: allocas + init calls
    DIBuilder dib_;

    Function *fn_ = nullptr;
    Value *ctxArg_ = nullptr;
    Value *funcResult_ = nullptr;   // FUNCTION result slot (argv[0])
    BasicBlock *retBB_ = nullptr;

    StructType *valTy_ = nullptr;
    PointerType *ptrTy_ = nullptr;
    Type *i64Ty_ = nullptr, *i32Ty_ = nullptr, *dblTy_ = nullptr,
         *voidTy_ = nullptr;

    DISubprogram *sp_ = nullptr;
    DIFile *diFile_ = nullptr;
    DICompositeType *diValTy_ = nullptr;

    std::map<std::string, Value *> scalars_;   // name -> ptr to mv_value
    std::map<std::string, Value *> arrays_;    // name -> alloca of mv_array*
    std::set<std::string> arrayNames_;         // every DIM'd name
    std::map<std::string, Constant *> strings_;

    NumericAnalysis num_;
    std::map<std::string, Value *> numVars_;   // name -> i64/double alloca
    struct NumArr { Value *ptr, *d1, *d2; Type *elemTy; };
    std::map<std::string, NumArr> numArrs_;

    std::vector<Value *> tempPool_;
    size_t tempUsed_ = 0;

    std::set<std::string> commonArrays_;

    // Labels and GOSUB.
    std::map<std::string, BasicBlock *> labelBBs_;
    bool hasGosub_ = false;
    Value *gsSp_ = nullptr;                 // i64 alloca, stack pointer
    Value *gsStack_ = nullptr;              // [kGosubDepth x i32] alloca
    std::vector<BasicBlock *> gosubConts_;  // continuation per GOSUB site
    BasicBlock *gosubRetBB_ = nullptr;
    // Enclosing loops: {CONTINUE target, EXIT target}, innermost last.
    std::vector<std::pair<BasicBlock *, BasicBlock *>> loops_;
    static constexpr uint64_t kGosubDepth = 1024;

    // ---------------------------------------------------------------- utils

    [[noreturn]] void err(int line, const std::string &msg) {
        throw CompileError(item_, line, msg);
    }

    FunctionCallee rt(const char *name, Type *ret, ArrayRef<Type *> params) {
        return mod_.getOrInsertFunction(
            name, FunctionType::get(ret, params, false));
    }

    Value *callRt(const char *name, Type *ret, ArrayRef<Type *> paramTys,
                  ArrayRef<Value *> args) {
        return b_.CreateCall(rt(name, ret, paramTys), args);
    }

    void call1(const char *name, Value *a) {
        callRt(name, voidTy_, {ptrTy_}, {a});
    }
    void call2(const char *name, Value *a, Value *b) {
        callRt(name, voidTy_, {ptrTy_, ptrTy_}, {a, b});
    }
    void call3(const char *name, Value *a, Value *b, Value *c) {
        callRt(name, voidTy_, {ptrTy_, ptrTy_, ptrTy_}, {a, b, c});
    }

    Constant *stringConst(const std::string &s) {
        auto it = strings_.find(s);
        if (it != strings_.end()) return it->second;
        Constant *g = b_.CreateGlobalString(s, ".str");
        strings_[s] = g;
        return g;
    }

    // Allocate one mv_value slot in the entry block, initialised once.
    Value *newSlot(const std::string &dbgName = "") {
        Value *slot = eb_.CreateAlloca(valTy_, nullptr, dbgName);
        eb_.CreateCall(rt("mv_init", voidTy_, {ptrTy_}), {slot});
        return slot;
    }

    Value *acquireTemp() {
        if (tempUsed_ == tempPool_.size())
            tempPool_.push_back(newSlot());
        return tempPool_[tempUsed_++];
    }

    Value *getScalar(const std::string &name, int line) {
        if (arrayNames_.count(name))
            err(line, "array " + name + " used without subscripts");
        auto it = scalars_.find(name);
        if (it != scalars_.end()) return it->second;
        Value *slot = newSlot(name);
        scalars_[name] = slot;
        declareVarDebug(name, slot, line);
        return slot;
    }

    Value *getArraySlot(const std::string &name) {
        auto it = arrays_.find(name);
        if (it != arrays_.end()) return it->second;
        Value *slot = eb_.CreateAlloca(ptrTy_, nullptr, name + ".arr");
        eb_.CreateStore(ConstantPointerNull::get(ptrTy_), slot);
        arrays_[name] = slot;
        return slot;
    }

    DebugLoc loc(int line) {
        const auto &dl = opts_.dwarfLines;
        int src = (line >= 1 && line <= (int)dl.size()) ? dl[line - 1] : line;
        return DILocation::get(llctx_, src, 1, sp_);
    }

    void declareVarDebug(const std::string &name, Value *slot, int line) {
        if (!sp_) return;
        DILocalVariable *dv = dib_.createAutoVariable(
            sp_, name, diFile_, (unsigned)line, diValTy_);
        dib_.insertDeclare(slot, dv, dib_.createExpression(),
                           DILocation::get(llctx_, (unsigned)line, 1, sp_),
                           b_.GetInsertBlock());
    }

    // ------------------------------------------------- numeric fast path

    bool intVar(const std::string &n) const {
        return num_.varKind(n) == NK::Int;
    }

    Value *numVarSlot(const std::string &name) {
        auto it = numVars_.find(name);
        if (it != numVars_.end()) return it->second;
        Type *ty = intVar(name) ? i64Ty_ : dblTy_;
        Value *slot = eb_.CreateAlloca(ty, nullptr, name);
        eb_.CreateStore(Constant::getNullValue(ty), slot);
        numVars_[name] = slot;
        return slot;
    }

    Type *arrElemTy(const std::string &name) {
        switch (num_.arrClass(name)) {
        case NumericAnalysis::ArrClass::I8:  return b_.getInt8Ty();
        case NumericAnalysis::ArrClass::I64: return i64Ty_;
        default:                             return dblTy_;
        }
    }

    NumArr &numArrSlots(const std::string &name) {
        auto it = numArrs_.find(name);
        if (it != numArrs_.end()) return it->second;
        NumArr a;
        a.ptr = eb_.CreateAlloca(ptrTy_, nullptr, name + ".nptr");
        a.d1  = eb_.CreateAlloca(i64Ty_, nullptr, name + ".d1");
        a.d2  = eb_.CreateAlloca(i64Ty_, nullptr, name + ".d2");
        a.elemTy = arrElemTy(name);
        eb_.CreateStore(ConstantPointerNull::get(ptrTy_), a.ptr);
        eb_.CreateStore(ConstantInt::get(i64Ty_, 0), a.d1);
        eb_.CreateStore(ConstantInt::get(i64Ty_, 0), a.d2);
        return numArrs_[name] = a;
    }

    // Saturating double -> i64; plain fptosi is UB out of range.
    Value *dblToI64(Value *v) {
        Function *f = intrinsicDecl(
            &mod_, Intrinsic::fptosi_sat, {i64Ty_, dblTy_});
        return b_.CreateCall(f, {v});
    }
    Value *asI64(const Expr &e) {
        Value *v = evalNum(e);
        return num_.kindOf(e) == NK::Int ? v : dblToI64(v);
    }
    Value *asDbl(const Expr &e) {
        Value *v = evalNum(e);
        return num_.kindOf(e) == NK::Int ? b_.CreateSIToFP(v, dblTy_) : v;
    }

    // i64 subscript from any expression.
    Value *numIndex(const Expr &e) {
        if (e.kind == Expr::K::IntLit)
            return ConstantInt::get(i64Ty_, e.ival);
        if (num_.numericExpr(e))
            return asI64(e);
        return callRt("mv_get_int", i64Ty_, {ptrTy_}, {evalPtr(e)});
    }

    // Bounds-checked pointer to a numeric array element.
    Value *numElemPtr(const Expr &e) {
        if (e.args.empty() || e.args.size() > 2)
            err(e.line, "array " + e.sval + " takes 1 or 2 subscripts");
        NumArr &a = numArrSlots(e.sval);
        Value *d1 = b_.CreateLoad(i64Ty_, a.d1);
        Value *d2 = b_.CreateLoad(i64Ty_, a.d2);
        Value *i = numIndex(*e.args[0]);
        Value *zero = ConstantInt::get(i64Ty_, 0);
        Value *one = ConstantInt::get(i64Ty_, 1);
        Value *j = e.args.size() == 2 ? numIndex(*e.args[1]) : zero;

        Value *bad;
        Value *idx;
        if (e.args.size() == 1) {
            bad = b_.CreateOr(b_.CreateICmpNE(d2, zero),
                              b_.CreateOr(b_.CreateICmpSLT(i, one),
                                          b_.CreateICmpSGT(i, d1)));
            idx = b_.CreateSub(i, one);
        } else {
            Value *badI = b_.CreateOr(b_.CreateICmpSLT(i, one),
                                      b_.CreateICmpSGT(i, d1));
            Value *badJ = b_.CreateOr(b_.CreateICmpSLT(j, one),
                                      b_.CreateICmpSGT(j, d2));
            bad = b_.CreateOr(b_.CreateICmpEQ(d2, zero),
                              b_.CreateOr(badI, badJ));
            idx = b_.CreateAdd(
                b_.CreateMul(b_.CreateSub(i, one), d2),
                b_.CreateSub(j, one));
        }
        BasicBlock *failBB = newBB("idx.fail");
        BasicBlock *okBB = newBB("idx.ok");
        b_.CreateCondBr(bad, failBB, okBB,
                        MDBuilder(llctx_).createUnlikelyBranchWeights());
        b_.SetInsertPoint(failBB);
        callRt("mvx_narr_fail", voidTy_, {i64Ty_, i64Ty_, i64Ty_, i64Ty_},
               {i, j, d1, d2});
        b_.CreateUnreachable();
        b_.SetInsertPoint(okBB);
        Value *base = b_.CreateLoad(ptrTy_, a.ptr);
        return b_.CreateGEP(a.elemTy, base, idx);
    }

    Value *fpIntrinsic(Intrinsic::ID id, ArrayRef<Value *> args) {
        Function *f =
            intrinsicDecl(&mod_, id, {dblTy_});
        return b_.CreateCall(f, args);
    }

    // Native value of a provably numeric expression: i64 when
    // num_.kindOf(e) == NK::Int, double when NK::Dbl.
    Value *evalNum(const Expr &e) {
        switch (e.kind) {
        case Expr::K::IntLit:
            return ConstantInt::get(i64Ty_, e.ival);
        case Expr::K::FltLit:
            return ConstantFP::get(dblTy_, e.fval);
        case Expr::K::Var: {
            Value *slot = numVarSlot(e.sval);
            return b_.CreateLoad(intVar(e.sval) ? i64Ty_ : dblTy_, slot);
        }
        case Expr::K::Paren: {
            if (arrayNames_.count(e.sval)) {
                Value *v = b_.CreateLoad(arrElemTy(e.sval), numElemPtr(e));
                if (v->getType() == b_.getInt8Ty())
                    v = b_.CreateZExt(v, i64Ty_);
                return v;
            }
            const std::string &f = e.sval;
            if (f == "LEN" && e.args.size() == 1)
                return callRt("mv_len_fn", i64Ty_, {ptrTy_},
                              {evalPtr(*e.args[0])});
            if (f == "CATALOGED" && e.args.size() == 1)
                return callRt("mv_cataloged_fn", i64Ty_, {ptrTy_},
                              {evalPtr(*e.args[0])});
            if (f == "COUNT" && e.args.size() == 2)
                return callRt("mv_count_fn", i64Ty_, {ptrTy_, ptrTy_},
                              {evalPtr(*e.args[0]), evalPtr(*e.args[1])});
            if (f == "DCOUNT" && e.args.size() == 2)
                return callRt("mv_dcount_fn", i64Ty_, {ptrTy_, ptrTy_},
                              {evalPtr(*e.args[0]), evalPtr(*e.args[1])});
            if (f == "SEQ" && e.args.size() == 1)
                return callRt("mv_seq_fn", i64Ty_, {ptrTy_},
                              {evalPtr(*e.args[0])});
            if (f == "NUM" && e.args.size() == 1)
                return callRt("mv_num_fn", i64Ty_, {ptrTy_},
                              {evalPtr(*e.args[0])});
            if (f == "ALPHA" && e.args.size() == 1)
                return callRt("mv_alpha_fn", i64Ty_, {ptrTy_},
                              {evalPtr(*e.args[0])});
            if (f == "INDEX" && e.args.size() == 3)
                return callRt("mv_index_fn", i64Ty_,
                              {ptrTy_, ptrTy_, i64Ty_},
                              {evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                               numIndex(*e.args[2])});
            if (f == "STATUS" && e.args.empty())
                return callRt("mvx_status", i64Ty_, {ptrTy_}, {ctxArg_});
            if (f == "DATE" && e.args.empty())
                return callRt("mv_date_fn", i64Ty_, {}, {});
            if (f == "RND" && e.args.size() == 1)
                return callRt("mv_rnd_fn", i64Ty_, {i64Ty_},
                              {numIndex(*e.args[0])});
            if (f == "OSWRITE" && e.args.size() == 2)
                return callRt("mv_oswrite", i64Ty_, {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1])});
            if (f == "OSDELETE" && e.args.size() == 1)
                return callRt("mv_osdelete", i64Ty_, {ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0])});
            if (f == "OSEXEC" && (e.args.size() == 1 || e.args.size() == 2)) {
                Value *cap = e.args.size() == 2
                                 ? evalPtr(*e.args[1])
                                 : (Value *)ConstantPointerNull::get(ptrTy_);
                return callRt("mvx_run", i64Ty_, {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]), cap});
            }
            if (f == "MKDIR" && e.args.size() == 1)
                return callRt("mvx_mkdir", i64Ty_, {ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0])});
            if (f == "RMTREE" && e.args.size() == 1)
                return callRt("mvx_rmtree", i64Ty_, {ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0])});
            if (f == "UNTAR" && e.args.size() == 2)
                return callRt("mvx_untar", i64Ty_, {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]), evalPtr(*e.args[1])});
            if (f == "EDITFILE" && e.args.size() == 1)
                return callRt("mvx_editfile", i64Ty_, {ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0])});
            if (f == "CREATEFILE" &&
                (e.args.size() == 1 || e.args.size() == 2)) {
                Value *type = e.args.size() == 2
                                  ? evalPtr(*e.args[1])
                                  : (Value *)ConstantPointerNull::get(ptrTy_);
                return callRt("mvx_createfile", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]), type});
            }
            if (f == "DELETEFILE" && e.args.size() == 1)
                return callRt("mvx_deletefile", i64Ty_, {ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0])});
            if (f == "INDEXBUILD" && e.args.size() == 2)
                return callRt("mvx_index_build", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1])});
            if (f == "INDEXDROP" && e.args.size() == 2)
                return callRt("mvx_index_drop", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1])});
            if (f == "INDEXSELECT" && e.args.size() == 3)
                return callRt("mvx_index_select", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1]), evalPtr(*e.args[2])});
            if (f == "MULTISELECT" && e.args.size() == 2)
                return callRt("mvx_multiselect", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1])});
            if (f == "QUERYSELECT" && e.args.size() == 5)
                return callRt("mvx_query_select", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1]), evalPtr(*e.args[2]),
                               evalPtr(*e.args[3]), evalPtr(*e.args[4])});
            if (f == "TRANSSELECT" && e.args.size() == 4)
                return callRt("mvx_transselect", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1]), evalPtr(*e.args[2]),
                               evalPtr(*e.args[3])});
            if (f == "TRANSORDERSELECT" && e.args.size() == 4)
                return callRt("mvx_transorderselect", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1]), evalPtr(*e.args[2]),
                               evalPtr(*e.args[3])});
            if (f == "QUERYCOUNT" && e.args.size() == 5)
                return callRt("mvx_querycount", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1]), evalPtr(*e.args[2]),
                               evalPtr(*e.args[3]), evalPtr(*e.args[4])});
            if (f == "ORDERSELECT" && e.args.size() == 8)
                return callRt("mvx_orderselect", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_,
                               ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                               evalPtr(*e.args[2]), evalPtr(*e.args[3]),
                               evalPtr(*e.args[4]), evalPtr(*e.args[5]),
                               evalPtr(*e.args[6]), evalPtr(*e.args[7])});
            if (f == "MAPBUILD" &&
                (e.args.size() == 2 || e.args.size() == 3))
                return callRt("mvx_mapbuild", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, i64Ty_},
                              {ctxArg_, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                               e.args.size() == 3 ? numIndex(*e.args[2])
                                   : ConstantInt::get(i64Ty_, 0)});
            if (f == "MAPDROP" && e.args.size() == 2)
                return callRt("mvx_mapdrop", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1])});
            if (f == "MAPCHECK" && e.args.size() == 2)
                return callRt("mvx_mapcheck", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1])});
            if (f == "COMPILE" && e.args.size() == 3)
                return callRt("mvx_compile", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1]), evalPtr(*e.args[2])});
            if (f == "SETCRED" && e.args.size() == 4)
                return callRt("mvx_setcred", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1]), evalPtr(*e.args[2]),
                               evalPtr(*e.args[3])});
            if (f == "SETCONN" && e.args.size() == 2)
                return callRt("mvx_setconn", i64Ty_,
                              {ptrTy_, ptrTy_, ptrTy_},
                              {ctxArg_, evalPtr(*e.args[0]),
                               evalPtr(*e.args[1])});
            if (kIntIntrinsics.count(f))
                err(e.line, f + "() given wrong number of arguments");
            if (f == "TIME")
                return dblToI64(callRt("mvx_num_time", dblTy_, {}, {}));
            if (f == "SYSTEM")
                return dblToI64(callRt("mvx_num_system", dblTy_,
                                       {ptrTy_, dblTy_},
                                       {ctxArg_, asDbl(*e.args[0])}));
            if (f == "INT")
                return asI64(*e.args[0]);   // fptosi_sat truncates to zero
            if (f == "SQRT")
                return fpIntrinsic(Intrinsic::sqrt, {asDbl(*e.args[0])});
            {
                struct { const char *nm, *rt; int arity; } maths[] = {
                    {"PWR", "mvx_num_pow", 2}, {"LN", "mvx_num_ln", 1},
                    {"EXP", "mvx_num_exp", 1}, {"SIN", "mvx_num_sin", 1},
                    {"COS", "mvx_num_cos", 1}, {"TAN", "mvx_num_tan", 1},
                    {"ATAN", "mvx_num_atan", 1},
                };
                for (auto &m : maths) {
                    if (f != m.nm) continue;
                    if ((int)e.args.size() != m.arity)
                        err(e.line, f + "() takes " +
                                        std::to_string(m.arity) + " argument(s)");
                    if (m.arity == 2)
                        return callRt(m.rt, dblTy_, {dblTy_, dblTy_},
                                      {asDbl(*e.args[0]), asDbl(*e.args[1])});
                    return callRt(m.rt, dblTy_, {dblTy_},
                                  {asDbl(*e.args[0])});
                }
            }
            if (f == "ABS") {
                const Expr &a = *e.args[0];
                if (num_.kindOf(a) == NK::Int) {
                    Function *fn = intrinsicDecl(
                        &mod_, Intrinsic::abs, {i64Ty_});
                    return b_.CreateCall(fn, {evalNum(a), b_.getFalse()});
                }
                return fpIntrinsic(Intrinsic::fabs, {evalNum(a)});
            }
            if (f == "MOD") {
                if (num_.kindOf(e) == NK::Int)
                    return callRt("mvx_num_imod", i64Ty_, {i64Ty_, i64Ty_},
                                  {evalNum(*e.args[0]), evalNum(*e.args[1])});
                return callRt("mvx_num_mod", dblTy_, {dblTy_, dblTy_},
                              {asDbl(*e.args[0]), asDbl(*e.args[1])});
            }
            err(e.line, f + " is not an intrinsic function or DIM'd array");
        }
        case Expr::K::Neg: {
            Value *v = evalNum(*e.lhs);
            return num_.kindOf(*e.lhs) == NK::Int ? b_.CreateNeg(v)
                                                  : b_.CreateFNeg(v);
        }
        case Expr::K::Not:
            return b_.CreateZExt(b_.CreateNot(evalCond(*e.lhs)), i64Ty_);
        case Expr::K::Bin: {
            bool bothInt = num_.kindOf(*e.lhs) == NK::Int &&
                           num_.kindOf(*e.rhs) == NK::Int;
            switch (e.op) {
            case BinOp::Add:
                return bothInt
                    ? b_.CreateAdd(evalNum(*e.lhs), evalNum(*e.rhs))
                    : b_.CreateFAdd(asDbl(*e.lhs), asDbl(*e.rhs));
            case BinOp::Sub:
                return bothInt
                    ? b_.CreateSub(evalNum(*e.lhs), evalNum(*e.rhs))
                    : b_.CreateFSub(asDbl(*e.lhs), asDbl(*e.rhs));
            case BinOp::Mul:
                return bothInt
                    ? b_.CreateMul(evalNum(*e.lhs), evalNum(*e.rhs))
                    : b_.CreateFMul(asDbl(*e.lhs), asDbl(*e.rhs));
            case BinOp::Div:
                return b_.CreateFDiv(asDbl(*e.lhs), asDbl(*e.rhs));
            case BinOp::Pow: {
                Function *f = intrinsicDecl(
                    &mod_, Intrinsic::pow, {dblTy_});
                return b_.CreateCall(f, {asDbl(*e.lhs), asDbl(*e.rhs)});
            }
            default:            // comparison / AND / OR as 0-1 value
                return b_.CreateZExt(evalCond(e), i64Ty_);
            }
        }
        case Expr::K::StrLit:
            break;
        }
        err(e.line, "internal error: evalNum on non-numeric expression");
    }

    // ----------------------------------------------------------- expressions

    Value *arrayElemPtr(const Expr &e) {
        Value *arr = b_.CreateLoad(ptrTy_, getArraySlot(e.sval));
        if (e.args.empty() || e.args.size() > 2)
            err(e.line, "array " + e.sval + " takes 1 or 2 subscripts");
        Value *i = numIndex(*e.args[0]);
        Value *j = e.args.size() == 2 ? numIndex(*e.args[1])
                                      : ConstantInt::get(i64Ty_, 0);
        return callRt("mv_arr_elem", ptrTy_, {ptrTy_, i64Ty_, i64Ty_},
                      {arr, i, j});
    }

    // Pointer to an mv_value holding the expression's value.  Lvalues are
    // returned in place (no copy); other expressions land in a temp.
    // Numeric expressions are boxed into a temp here — the bridge from the
    // fast path into string-land.
    // Box a numeric expression into the given slot, preserving intness.
    void boxNum(const Expr &e, Value *dest) {
        if (num_.kindOf(e) == NK::Int)
            callRt("mv_set_int", voidTy_, {ptrTy_, i64Ty_},
                   {dest, evalNum(e)});
        else
            callRt("mv_set_dbl", voidTy_, {ptrTy_, dblTy_},
                   {dest, evalNum(e)});
    }

    Value *evalPtr(const Expr &e) {
        if (num_.numericExpr(e)) {
            Value *t = acquireTemp();
            boxNum(e, t);
            return t;
        }
        if (e.kind == Expr::K::Var && sysConstChar(e.sval) < 0 &&
            e.sval != "@USER.TYPE")
            return getScalar(e.sval, e.line);
        if (e.kind == Expr::K::Paren && arrayNames_.count(e.sval))
            return arrayElemPtr(e);
        Value *t = acquireTemp();
        evalInto(e, t);
        return t;
    }

    void evalInto(const Expr &e, Value *dest) {
        if (num_.numericExpr(e)) {
            boxNum(e, dest);
            return;
        }
        switch (e.kind) {
        case Expr::K::IntLit:
            callRt("mv_set_int", voidTy_, {ptrTy_, i64Ty_},
                   {dest, ConstantInt::get(i64Ty_, e.ival)});
            return;
        case Expr::K::FltLit:
            callRt("mv_set_dbl", voidTy_, {ptrTy_, dblTy_},
                   {dest, ConstantFP::get(dblTy_, e.fval)});
            return;
        case Expr::K::StrLit:
            callRt("mv_set_str", voidTy_, {ptrTy_, ptrTy_, i64Ty_},
                   {dest, stringConst(e.sval),
                    ConstantInt::get(i64Ty_, (int64_t)e.sval.size())});
            return;
        case Expr::K::Var: {
            if (e.sval == "@USER.TYPE") {          // session type (0 = interactive)
                callRt("mv_user_type", voidTy_, {ptrTy_, ptrTy_},
                       {ctxArg_, dest});
                return;
            }
            int mc = sysConstChar(e.sval);
            if (mc >= 0) {
                callRt("mv_set_str", voidTy_, {ptrTy_, ptrTy_, i64Ty_},
                       {dest, stringConst(std::string(1, (char)mc)),
                        ConstantInt::get(i64Ty_, 1)});
                return;
            }
            call2("mv_copy", dest, getScalar(e.sval, e.line));
            return;
        }
        case Expr::K::Paren:
            evalParenInto(e, dest);
            return;
        case Expr::K::Extract: {
            Value *base = evalPtr(*e.lhs);
            callRt("mv_extract_fn", voidTy_,
                   {ptrTy_, ptrTy_, i64Ty_, i64Ty_, i64Ty_},
                   {dest, base, subIdx(e, 0), subIdx(e, 1), subIdx(e, 2)});
            return;
        }
        case Expr::K::Substr:
            callRt("mv_substr", voidTy_, {ptrTy_, ptrTy_, i64Ty_, i64Ty_},
                   {dest, evalPtr(*e.lhs), numIndex(*e.args[0]),
                    numIndex(*e.args[1])});
            return;
        case Expr::K::Fmt:
            call3("mv_fmt", dest, evalPtr(*e.lhs), evalPtr(*e.rhs));
            return;
        case Expr::K::Neg:
            call2("mv_neg", dest, evalPtr(*e.lhs));
            return;
        case Expr::K::Not: {
            Value *t = evalCond(*e.lhs);
            Value *inv = b_.CreateZExt(b_.CreateNot(t), i64Ty_);
            callRt("mv_set_int", voidTy_, {ptrTy_, i64Ty_}, {dest, inv});
            return;
        }
        case Expr::K::Bin:
            evalBinInto(e, dest);
            return;
        }
    }

    // Subscript k of an Extract-shaped node (0 when absent).
    Value *subIdx(const Expr &e, size_t k) {
        if (k >= e.args.size()) return ConstantInt::get(i64Ty_, 0);
        return numIndex(*e.args[k]);
    }

    void evalParenInto(const Expr &e, Value *dest) {
        if (e.call) { evalFuncCall(e, dest); return; }   // user function
        if (arrayNames_.count(e.sval)) {
            call2("mv_copy", dest, arrayElemPtr(e));
            return;
        }
        const std::string &f = e.sval;
        if (kStrIntrinsics.count(f)) {
            evalStrIntrinsic(e, dest);
            return;
        }
        auto need = [&](size_t n) {
            if (e.args.size() != n)
                err(e.line, f + "() takes " + std::to_string(n) +
                                " argument(s)");
        };
        if (f == "CHAR") { need(1);
            callRt("mv_char_fn", voidTy_, {ptrTy_, i64Ty_},
                   {dest, numIndex(*e.args[0])});
            return; }
        if (f == "SPACE") { need(1);
            callRt("mv_space_fn", voidTy_, {ptrTy_, i64Ty_},
                   {dest, numIndex(*e.args[0])});
            return; }
        if (f == "STR") { need(2);
            callRt("mv_str_fn", voidTy_, {ptrTy_, ptrTy_, i64Ty_},
                   {dest, evalPtr(*e.args[0]), numIndex(*e.args[1])});
            return; }
        if (f == "TRIM") {
            if (e.args.size() == 1) {
                call2("mv_trim_fn", dest, evalPtr(*e.args[0]));
            } else if (e.args.size() == 2 || e.args.size() == 3) {
                Value *opt = e.args.size() == 3
                                 ? evalPtr(*e.args[2])
                                 : (Value *)ConstantPointerNull::get(ptrTy_);
                callRt("mv_trim_opt", voidTy_,
                       {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                       {dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]), opt});
            } else {
                err(e.line, "TRIM() takes 1 to 3 arguments");
            }
            return; }
        if (f == "TRIMB") { need(1);
            call2("mv_trimb_fn", dest, evalPtr(*e.args[0]));
            return; }
        if (f == "TRIMF") { need(1);
            call2("mv_trimf_fn", dest, evalPtr(*e.args[0]));
            return; }
        if (f == "CONVERT") { need(3);
            callRt("mv_convert_fn", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                    evalPtr(*e.args[2])});
            return; }
        if (f == "EREPLACE" || f == "SWAP") { need(3);
            callRt("mv_change_fn", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                    evalPtr(*e.args[2])});
            return; }
        if (f == "QUOTE" || f == "DQUOTE" || f == "SQUOTE") { need(1);
            callRt("mv_quote_fn", voidTy_, {ptrTy_, ptrTy_, i64Ty_},
                   {dest, evalPtr(*e.args[0]),
                    ConstantInt::get(i64Ty_, f == "SQUOTE" ? '\'' : '"')});
            return; }
        if (f == "ENV") { need(1);
            call2("mv_env", dest, evalPtr(*e.args[0]));
            return; }
        if (f == "FILELIST") { need(0);
            callRt("mvx_filelist", voidTy_, {ptrTy_, ptrTy_},
                   {ctxArg_, dest});
            return; }
        if (f == "TRANS" || f == "XLATE") { need(4);
            callRt("mvx_trans", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {ctxArg_, dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                    evalPtr(*e.args[2]), evalPtr(*e.args[3])});
            return; }
        if (f == "IEVAL") { need(2);
            callRt("mvx_ieval", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {ctxArg_, dest, evalPtr(*e.args[0]), evalPtr(*e.args[1])});
            return; }
        if (f == "QUERYSUM") { need(6);
            callRt("mvx_querysum", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_,
                    ptrTy_},
                   {ctxArg_, dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                    evalPtr(*e.args[2]), evalPtr(*e.args[3]),
                    evalPtr(*e.args[4]), evalPtr(*e.args[5])});
            return; }
        if (f == "DESCRIBE") { need(3);
            callRt("mvx_describe", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {ctxArg_, dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                    evalPtr(*e.args[2])});
            return; }
        if (f == "MAPSPEC") { need(1);
            callRt("mvx_mapspec", voidTy_, {ptrTy_, ptrTy_, ptrTy_},
                   {ctxArg_, dest, evalPtr(*e.args[0])});
            return; }
        if (f == "MAPFIELD") {
            if (e.args.size() < 2 || e.args.size() > 5)
                err(e.line, "MAPFIELD() takes 2 to 5 arguments");
            auto opt = [&](size_t k) -> Value * {
                return e.args.size() > k ? evalPtr(*e.args[k])
                                         : (Value *)ConstantPointerNull::get(ptrTy_);
            };
            callRt("mvx_map_field", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]), opt(2),
                    opt(3), opt(4)});
            return; }
        if (f == "@") {
            if (e.args.size() != 1 && e.args.size() != 2)
                err(e.line, "@() takes 1 or 2 arguments");
            Value *a = numIndex(*e.args[0]);
            Value *b = e.args.size() == 2 ? numIndex(*e.args[1])
                                          : ConstantInt::get(i64Ty_, 0);
            callRt("mv_at_fn", voidTy_, {ptrTy_, i64Ty_, i64Ty_, i64Ty_},
                   {dest, a, b,
                    ConstantInt::get(i64Ty_, e.args.size() == 2 ? 1 : 0)});
            return;
        }
        if (f == "COLOR") {
            if (e.args.size() != 1 && e.args.size() != 2)
                err(e.line, "COLOR() takes 1 or 2 arguments");
            Value *bg = e.args.size() == 2
                            ? evalPtr(*e.args[1])
                            : (Value *)ConstantPointerNull::get(ptrTy_);
            callRt("mv_color_fn", voidTy_, {ptrTy_, ptrTy_, ptrTy_},
                   {dest, evalPtr(*e.args[0]), bg});
            return;
        }
        if (f == "KEYIN") {
            if (e.args.size() > 1)
                err(e.line, "KEYIN() takes at most one argument");
            Value *t = e.args.empty() ? ConstantInt::get(i64Ty_, -1)
                                      : numIndex(*e.args[0]);
            callRt("mv_keyin", voidTy_, {ptrTy_, ptrTy_, i64Ty_},
                   {ctxArg_, dest, t});
            return;
        }
        if (f == "MOUSE") { need(0);
            callRt("mv_mouse", voidTy_, {ptrTy_, ptrTy_},
                   {ctxArg_, dest});
            return; }
        if (f == "SENTENCE") { need(0);
            callRt("mv_sentence", voidTy_, {ptrTy_, ptrTy_},
                   {ctxArg_, dest});
            return; }
        if (f == "LISTCRED") { need(0);
            callRt("mvx_listcred", voidTy_, {ptrTy_, ptrTy_},
                   {ctxArg_, dest});
            return; }
        if (f == "LISTCONN") { need(0);
            callRt("mvx_listconn", voidTy_, {ptrTy_, ptrTy_},
                   {ctxArg_, dest});
            return; }
        if (f == "OSREAD") { need(1);
            callRt("mv_osread", voidTy_, {ptrTy_, ptrTy_, ptrTy_},
                   {ctxArg_, dest, evalPtr(*e.args[0])});
            return; }
        if (f == "UNAME") { need(1);
            callRt("mvx_uname", voidTy_, {ptrTy_, ptrTy_, ptrTy_},
                   {ctxArg_, dest, evalPtr(*e.args[0])});
            return; }
        if (f == "TMPNAM") { need(0);
            callRt("mvx_tmpnam", voidTy_, {ptrTy_}, {dest});
            return; }
        if (f == "CHANGE") { need(3);
            callRt("mv_change_fn", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                    evalPtr(*e.args[2])});
            return; }
        if (f == "OCONV" || f == "ICONV") { need(2);
            callRt(f == "OCONV" ? "mv_oconv" : "mv_iconv", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {ctxArg_, dest, evalPtr(*e.args[0]),
                    evalPtr(*e.args[1])});
            return; }
        if (f == "FMT") { need(2);
            call3("mv_fmt", dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]));
            return; }
        if (f == "FIELD") {
            if (e.args.size() != 3 && e.args.size() != 4)
                err(e.line, "FIELD() takes 3 or 4 arguments");
            Value *cnt = e.args.size() == 4 ? numIndex(*e.args[3])
                                            : ConstantInt::get(i64Ty_, 1);
            callRt("mv_field_fn", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, i64Ty_, i64Ty_},
                   {dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                    numIndex(*e.args[2]), cnt});
            return; }
        if (f == "MATCHFIELD") {
            need(3);
            callRt("mv_matchfield_fn", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, i64Ty_},
                   {dest, evalPtr(*e.args[0]), evalPtr(*e.args[1]),
                    numIndex(*e.args[2])});
            return; }
        if (f == "SUM")     { need(1); call2("mv_sum",    dest,
                                             evalPtr(*e.args[0])); return; }
        if (f == "MAXIMUM") { need(1); call2("mv_max_fn", dest,
                                             evalPtr(*e.args[0])); return; }
        if (f == "MINIMUM") { need(1); call2("mv_min_fn", dest,
                                             evalPtr(*e.args[0])); return; }
        if (f == "TIME")   { need(0); call1("mv_time", dest); return; }
        if (f == "SYSTEM") { need(1);
            call3("mv_system_fn", ctxArg_, dest, evalPtr(*e.args[0]));
            return; }
        if (f == "INT")    { need(1);
            call2("mv_int_fn", dest, evalPtr(*e.args[0])); return; }
        if (f == "SQRT")   { need(1);
            call2("mv_sqrt_fn", dest, evalPtr(*e.args[0])); return; }
        if (f == "ABS")    { need(1);
            call2("mv_abs_fn", dest, evalPtr(*e.args[0])); return; }
        if (f == "MOD")    { need(2);
            call3("mv_mod_fn", dest, evalPtr(*e.args[0]),
                  evalPtr(*e.args[1])); return; }
        err(e.line, f + " is not an intrinsic function or DIM'd array");
    }

    // EXTRACT(A,a[,v[,s]]) / DELETE(A,a[,v[,s]]) /
    // REPLACE(A,a[,v[,s]],X) / INSERT(A,a[,v[,s]],X)
    void evalStrIntrinsic(const Expr &e, Value *dest) {
        const std::string &f = e.sval;
        bool hasVal = (f == "REPLACE" || f == "INSERT");
        size_t nIdx = e.args.size() - 1 - (hasVal ? 1 : 0);
        if (e.args.size() < (hasVal ? 3u : 2u) || nIdx > 3)
            err(e.line, f + "() given " + std::to_string(e.args.size()) +
                            " argument(s)");
        Value *base = evalPtr(*e.args[0]);
        Value *idx[3];
        for (size_t k = 0; k < 3; k++)
            idx[k] = k < nIdx ? numIndex(*e.args[k + 1])
                              : ConstantInt::get(i64Ty_, 0);
        if (f == "EXTRACT") {
            callRt("mv_extract_fn", voidTy_,
                   {ptrTy_, ptrTy_, i64Ty_, i64Ty_, i64Ty_},
                   {dest, base, idx[0], idx[1], idx[2]});
        } else if (f == "DELETE") {
            callRt("mv_delete_fn", voidTy_,
                   {ptrTy_, ptrTy_, i64Ty_, i64Ty_, i64Ty_},
                   {dest, base, idx[0], idx[1], idx[2]});
        } else {
            Value *val = evalPtr(*e.args.back());
            callRt(f == "REPLACE" ? "mv_replace_fn" : "mv_insert_fn",
                   voidTy_,
                   {ptrTy_, ptrTy_, i64Ty_, i64Ty_, i64Ty_, ptrTy_},
                   {dest, base, idx[0], idx[1], idx[2], val});
        }
    }

    void evalBinInto(const Expr &e, Value *dest) {
        switch (e.op) {
        case BinOp::Add: case BinOp::Sub: case BinOp::Mul:
        case BinOp::Div: case BinOp::Pow: case BinOp::Cat: {
            static const std::map<BinOp, const char *> fns = {
                {BinOp::Add, "mv_add"}, {BinOp::Sub, "mv_sub"},
                {BinOp::Mul, "mv_mul"}, {BinOp::Div, "mv_div"},
                {BinOp::Pow, "mv_pow"}, {BinOp::Cat, "mv_cat"},
            };
            Value *pa = evalPtr(*e.lhs);
            Value *pb = evalPtr(*e.rhs);
            call3(fns.at(e.op), dest, pa, pb);
            return;
        }
        default: {
            Value *c = evalCond(e);
            callRt("mv_set_int", voidTy_, {ptrTy_, i64Ty_},
                   {dest, b_.CreateZExt(c, i64Ty_)});
            return;
        }
        }
    }

    // Boolean contexts skip boxing entirely.
    Value *evalCond(const Expr &e) {
        if (e.kind == Expr::K::Bin) {
            switch (e.op) {
            case BinOp::Eq: case BinOp::Ne: case BinOp::Lt:
            case BinOp::Le: case BinOp::Gt: case BinOp::Ge: {
                if (num_.numericExpr(*e.lhs) && num_.numericExpr(*e.rhs)) {
                    if (num_.kindOf(*e.lhs) == NK::Int &&
                        num_.kindOf(*e.rhs) == NK::Int) {
                        Value *l = evalNum(*e.lhs);
                        Value *r = evalNum(*e.rhs);
                        switch (e.op) {
                        case BinOp::Eq: return b_.CreateICmpEQ(l, r);
                        case BinOp::Ne: return b_.CreateICmpNE(l, r);
                        case BinOp::Lt: return b_.CreateICmpSLT(l, r);
                        case BinOp::Le: return b_.CreateICmpSLE(l, r);
                        case BinOp::Gt: return b_.CreateICmpSGT(l, r);
                        default:        return b_.CreateICmpSGE(l, r);
                        }
                    }
                    Value *l = asDbl(*e.lhs);
                    Value *r = asDbl(*e.rhs);
                    switch (e.op) {
                    case BinOp::Eq: return b_.CreateFCmpOEQ(l, r);
                    case BinOp::Ne: return b_.CreateFCmpONE(l, r);
                    case BinOp::Lt: return b_.CreateFCmpOLT(l, r);
                    case BinOp::Le: return b_.CreateFCmpOLE(l, r);
                    case BinOp::Gt: return b_.CreateFCmpOGT(l, r);
                    default:        return b_.CreateFCmpOGE(l, r);
                    }
                }
                Value *pa = evalPtr(*e.lhs);
                Value *pb = evalPtr(*e.rhs);
                Value *c = callRt("mv_compare", i64Ty_, {ptrTy_, ptrTy_},
                                  {pa, pb});
                Value *zero = ConstantInt::get(i64Ty_, 0);
                switch (e.op) {
                case BinOp::Eq: return b_.CreateICmpEQ(c, zero);
                case BinOp::Ne: return b_.CreateICmpNE(c, zero);
                case BinOp::Lt: return b_.CreateICmpSLT(c, zero);
                case BinOp::Le: return b_.CreateICmpSLE(c, zero);
                case BinOp::Gt: return b_.CreateICmpSGT(c, zero);
                default:        return b_.CreateICmpSGE(c, zero);
                }
            }
            case BinOp::And:
                return b_.CreateAnd(evalCond(*e.lhs), evalCond(*e.rhs));
            case BinOp::Or:
                return b_.CreateOr(evalCond(*e.lhs), evalCond(*e.rhs));
            case BinOp::Matches: {
                Value *m = callRt("mv_matches", i64Ty_, {ptrTy_, ptrTy_},
                                  {evalPtr(*e.lhs), evalPtr(*e.rhs)});
                return b_.CreateICmpNE(m, ConstantInt::get(i64Ty_, 0));
            }
            default:
                break;
            }
        }
        if (e.kind == Expr::K::Not)
            return b_.CreateNot(evalCond(*e.lhs));
        if (num_.numericExpr(e)) {
            Value *v = evalNum(e);
            return num_.kindOf(e) == NK::Int
                ? b_.CreateICmpNE(v, ConstantInt::get(i64Ty_, 0))
                : b_.CreateFCmpONE(v, ConstantFP::get(dblTy_, 0.0));
        }
        Value *t = callRt("mv_truth", i64Ty_, {ptrTy_}, {evalPtr(e)});
        return b_.CreateICmpNE(t, ConstantInt::get(i64Ty_, 0));
    }

    // ------------------------------------------------------------ statements

    BasicBlock *newBB(const char *name) {
        return BasicBlock::Create(llctx_, name, fn_);
    }

    void emitBlock(const std::vector<StmtP> &stmts) {
        for (const auto &s : stmts) emitStmt(*s);
    }

    void emitStmt(const Stmt &s) {
        tempUsed_ = 0;
        b_.SetCurrentDebugLocation(loc(s.line));
        switch (s.kind) {
        case Stmt::K::Nop:    break;         // EQUATE: compile-time only
        case Stmt::K::Assign: emitAssign(s); break;
        case Stmt::K::Dim:    emitDim(s);    break;
        case Stmt::K::If:     emitIf(s);     break;
        case Stmt::K::For:    emitFor(s);    break;
        case Stmt::K::Loop:   emitLoop(s);   break;
        case Stmt::K::Print:  emitPrint(s);  break;
        case Stmt::K::Call:   emitCall(s);   break;
        case Stmt::K::Label: {
            BasicBlock *bb = labelBBs_.at(s.name);
            bb->insertInto(fn_);
            b_.CreateBr(bb);                        // fall through
            b_.SetInsertPoint(bb);
            break;
        }
        case Stmt::K::Goto:
            b_.CreateBr(labelBBs_.at(s.name));
            b_.SetInsertPoint(newBB("dead"));
            break;
        case Stmt::K::Continue:
        case Stmt::K::Exit:
            if (loops_.empty())
                err(s.line, (s.kind == Stmt::K::Continue ? "CONTINUE"
                                                         : "EXIT")
                                + std::string(" outside a loop"));
            b_.CreateBr(s.kind == Stmt::K::Continue ? loops_.back().first
                                                    : loops_.back().second);
            b_.SetInsertPoint(newBB("dead"));
            break;
        case Stmt::K::Gosub:
            emitGosub(s);
            break;
        case Stmt::K::OnGoto:
            emitOnGoto(s);
            break;
        case Stmt::K::OnGosub:
            emitOnGosub(s);
            break;
        case Stmt::K::Locate:
            emitLocate(s);
            break;
        case Stmt::K::Input:
            emitInput(s);
            break;
        case Stmt::K::Mat:
            emitMat(s);
            break;
        case Stmt::K::Common:
            break;          // bound at function entry
        case Stmt::K::Echo:
            callRt("mv_echo", voidTy_, {ptrTy_, i64Ty_},
                   {ctxArg_,
                    ConstantInt::get(i64Ty_, s.name == "ON" ? 1 : 0)});
            break;
        case Stmt::K::Open:     emitOpen(s);     break;
        case Stmt::K::ReadF:    emitReadF(s);    break;
        case Stmt::K::WriteF:   emitWriteF(s);   break;
        case Stmt::K::ReadV:    emitReadV(s);    break;
        case Stmt::K::WriteV:   emitWriteV(s);   break;
        case Stmt::K::MatParse: emitMatParse(s); break;
        case Stmt::K::MatBuild: emitMatBuild(s); break;
        case Stmt::K::MatRead:  emitMatRead(s);  break;
        case Stmt::K::MatWrite: emitMatWrite(s); break;
        case Stmt::K::DeleteF: {
            Value *st =
                callRt("mvx_delete_rec", i64Ty_, {ptrTy_, ptrTy_, ptrTy_},
                       {ctxArg_, evalPtr(*s.args[0]), evalPtr(*s.args[1])});
            emitErrorClause(st, s, "delete");
            break;
        }
        case Stmt::K::Release:
            if (s.args.empty())
                callRt("mvx_release", voidTy_, {ptrTy_, ptrTy_, ptrTy_},
                       {ctxArg_, ConstantPointerNull::get(ptrTy_),
                        ConstantPointerNull::get(ptrTy_)});
            else
                callRt("mvx_release", voidTy_, {ptrTy_, ptrTy_, ptrTy_},
                       {ctxArg_, evalPtr(*s.args[0]), evalPtr(*s.args[1])});
            break;
        case Stmt::K::Select:
            callRt("mvx_select", voidTy_, {ptrTy_, ptrTy_},
                   {ctxArg_, evalPtr(*s.args[0])});
            break;
        case Stmt::K::Readnext: {
            Value *found = callRt("mvx_readnext", i64Ty_, {ptrTy_, ptrTy_},
                                  {ctxArg_, getScalar(s.name, s.line)});
            emitThenElse(found, s, "rn");
            break;
        }
        case Stmt::K::Formlist:
            callRt("mvx_formlist", voidTy_, {ptrTy_, ptrTy_},
                   {ctxArg_, evalPtr(*s.value)});
            break;
        case Stmt::K::Execute: {
            Value *cap = s.name.empty()
                             ? (Value *)ConstantPointerNull::get(ptrTy_)
                             : getScalar(s.name, s.line);
            Value *ret = s.name2.empty()
                             ? (Value *)ConstantPointerNull::get(ptrTy_)
                             : getScalar(s.name2, s.line);
            callRt("mvx_execute", i64Ty_,
                   {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                   {ctxArg_, evalPtr(*s.value), cap, ret});
            break;
        }

        case Stmt::K::Return:
            if (s.value) {   // RETURN(expr): a function result, returns now
                if (!funcResult_)
                    err(s.line, "RETURN(value) outside a FUNCTION");
                evalInto(*s.value, funcResult_);
                b_.CreateBr(retBB_);
            } else {
                // With GOSUBs present, RETURN pops the return stack; with an
                // empty stack it ends the program / returns to the caller.
                b_.CreateBr(hasGosub_ ? gosubRetBB() : retBB_);
            }
            b_.SetInsertPoint(newBB("dead"));
            break;
        case Stmt::K::Stop:
            if (s.value) {
                // STOP <code>: end the whole program with a process exit status.
                Value *code = b_.CreateTrunc(asI64(*s.value), i32Ty_);
                callRt("mvx_exit", voidTy_, {i32Ty_}, {code});
                b_.CreateUnreachable();
            } else if (prog_.isSubroutine) {
                // STOP ends the whole program, not just the subroutine.
                callRt("mvx_stop", voidTy_, {}, {});
                b_.CreateUnreachable();
            } else {
                b_.CreateBr(retBB_);
            }
            b_.SetInsertPoint(newBB("dead"));
            break;
        }
    }

    // Bind every COMMON item to its context-owned slot, positionally per
    // block, before any body statement runs.
    void emitCommonBindings(const std::vector<StmtP> &stmts,
                            std::map<std::string, int64_t> &counters) {
        for (const auto &sp : stmts) {
            const Stmt &s = *sp;
            if (s.kind == Stmt::K::Common) {
                for (const auto &item : s.args) {
                    int64_t idx = counters[s.name2]++;
                    Value *block = stringConst(s.name2);
                    Value *iv = ConstantInt::get(i64Ty_, idx);
                    if (item->kind == Expr::K::Var) {
                        scalars_[item->sval] = callRt(
                            "mvx_common_scalar", ptrTy_,
                            {ptrTy_, ptrTy_, i64Ty_}, {ctxArg_, block, iv});
                    } else {
                        if (item->args.empty() || item->args.size() > 2)
                            err(item->line, "COMMON array " + item->sval +
                                                " needs 1 or 2 dimensions");
                        Value *d1 = numIndex(*item->args[0]);
                        Value *d2 = item->args.size() == 2
                                        ? numIndex(*item->args[1])
                                        : ConstantInt::get(i64Ty_, 0);
                        Value *arr = callRt(
                            "mvx_common_arr", ptrTy_,
                            {ptrTy_, ptrTy_, i64Ty_, i64Ty_, i64Ty_},
                            {ctxArg_, block, iv, d1, d2});
                        b_.CreateStore(arr, getArraySlot(item->sval));
                    }
                }
            }
            emitCommonBindings(s.body, counters);
            emitCommonBindings(s.elseBody, counters);
            emitCommonBindings(s.lockedBody, counters);
            emitCommonBindings(s.errorBody, counters);
            emitCommonBindings(s.pre, counters);
            emitCommonBindings(s.post, counters);
        }
    }

    // Branch on an i64 result into the statement's THEN/ELSE bodies.
    void emitThenElse(Value *result, const Stmt &s, const char *tag) {
        Value *c = b_.CreateICmpNE(result, ConstantInt::get(i64Ty_, 0));
        BasicBlock *thenBB = newBB((std::string(tag) + ".then").c_str());
        BasicBlock *elseBB = newBB((std::string(tag) + ".else").c_str());
        BasicBlock *doneBB = newBB((std::string(tag) + ".done").c_str());
        b_.CreateCondBr(c, thenBB, elseBB);
        b_.SetInsertPoint(thenBB);
        emitBlock(s.body);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(elseBB);
        emitBlock(s.elseBody);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(doneBB);
    }

    // Lock mode passed to the runtime read: 2 = try (LOCKED clause,
    // non-blocking), 1 = blocking READU, 0 = no lock.
    Value *lockMode(const Stmt &s, bool isU) {
        return ConstantInt::get(i64Ty_, s.hasLocked ? 2 : (isU ? 1 : 0));
    }

    // Branch a read/write result across its optional clauses.  The
    // runtime encodes: -2 = backend error (ON ERROR), -1 = held by
    // another session (LOCKED), 0 = not found (ELSE), >0 = found (THEN).
    // Only the sentinels whose clause is present can occur, so absent
    // clauses fold away.
    void emitReadResult(Value *result, const Stmt &s, const char *tag) {
        if (!s.hasError && !s.hasLocked) { emitThenElse(result, s, tag); return; }
        Value *zero = ConstantInt::get(i64Ty_, 0);
        BasicBlock *doneBB = newBB((std::string(tag) + ".done").c_str());
        if (s.hasError) {
            BasicBlock *errBB = newBB((std::string(tag) + ".err").c_str());
            BasicBlock *nBB = newBB((std::string(tag) + ".nerr").c_str());
            b_.CreateCondBr(
                b_.CreateICmpEQ(result, ConstantInt::get(i64Ty_, -2)),
                errBB, nBB);
            b_.SetInsertPoint(errBB);
            emitBlock(s.errorBody);
            b_.CreateBr(doneBB);
            b_.SetInsertPoint(nBB);
        }
        if (s.hasLocked) {
            BasicBlock *lkBB = newBB((std::string(tag) + ".locked").c_str());
            BasicBlock *nBB = newBB((std::string(tag) + ".nlock").c_str());
            b_.CreateCondBr(b_.CreateICmpSLT(result, zero), lkBB, nBB);
            b_.SetInsertPoint(lkBB);
            emitBlock(s.lockedBody);
            b_.CreateBr(doneBB);
            b_.SetInsertPoint(nBB);
        }
        BasicBlock *thenBB = newBB((std::string(tag) + ".then").c_str());
        BasicBlock *elseBB = newBB((std::string(tag) + ".else").c_str());
        b_.CreateCondBr(b_.CreateICmpNE(result, zero), thenBB, elseBB);
        b_.SetInsertPoint(thenBB);
        emitBlock(s.body);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(elseBB);
        emitBlock(s.elseBody);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(doneBB);
    }

    // A write/delete carrying only ON ERROR: run errorBody on -2.
    void emitErrorClause(Value *result, const Stmt &s, const char *tag) {
        if (!s.hasError) return;
        BasicBlock *errBB = newBB((std::string(tag) + ".err").c_str());
        BasicBlock *doneBB = newBB((std::string(tag) + ".done").c_str());
        b_.CreateCondBr(
            b_.CreateICmpEQ(result, ConstantInt::get(i64Ty_, -2)),
            errBB, doneBB);
        b_.SetInsertPoint(errBB);
        emitBlock(s.errorBody);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(doneBB);
    }

    // ON ERROR flag passed to a write runtime call.
    Value *errFlag(const Stmt &s) {
        return ConstantInt::get(i64Ty_, s.hasError ? 1 : 0);
    }

    void emitOpen(const Stmt &s) {
        bool twoPart = s.args.size() == 2;
        Value *dict = twoPart ? evalPtr(*s.args[0])
                              : (Value *)ConstantPointerNull::get(ptrTy_);
        Value *spec = evalPtr(*s.args[twoPart ? 1 : 0]);
        Value *fvar = getScalar(s.name, s.line);
        Value *ok = callRt("mvx_open", i64Ty_,
                           {ptrTy_, ptrTy_, ptrTy_, ptrTy_},
                           {ctxArg_, dict, spec, fvar});
        emitThenElse(ok, s, "open");
    }

    void emitReadF(const Stmt &s) {
        const Expr &t = *s.target;
        Value *dst;
        if (t.kind == Expr::K::Var)
            dst = getScalar(t.sval, t.line);
        else if (arrayNames_.count(t.sval) && !num_.numericArray(t.sval))
            dst = arrayElemPtr(t);
        else
            err(t.line, "READ target must be a variable or array element");
        Value *lock = lockMode(s, s.name == "U");
        Value *found = callRt(
            "mvx_read", i64Ty_, {ptrTy_, ptrTy_, ptrTy_, ptrTy_, i64Ty_},
            {ctxArg_, dst, evalPtr(*s.args[0]), evalPtr(*s.args[1]), lock});
        emitReadResult(found, s, "read");
    }

    void emitWriteF(const Stmt &s) {
        Value *keep = ConstantInt::get(i64Ty_, s.name == "U" ? 1 : 0);
        Value *st = callRt("mvx_write", i64Ty_,
               {ptrTy_, ptrTy_, ptrTy_, ptrTy_, i64Ty_, i64Ty_},
               {ctxArg_, evalPtr(*s.value), evalPtr(*s.args[0]),
                evalPtr(*s.args[1]), keep, errFlag(s)});
        emitErrorClause(st, s, "write");
    }

    void emitReadV(const Stmt &s) {
        const Expr &t = *s.target;
        Value *dst;
        if (t.kind == Expr::K::Var)
            dst = getScalar(t.sval, t.line);
        else if (arrayNames_.count(t.sval) && !num_.numericArray(t.sval))
            dst = arrayElemPtr(t);
        else
            err(t.line, "READV target must be a variable or array element");
        Value *attr = numIndex(*s.args[2]);
        Value *lock = lockMode(s, s.name == "U");
        Value *found = callRt(
            "mvx_readv", i64Ty_,
            {ptrTy_, ptrTy_, ptrTy_, ptrTy_, i64Ty_, i64Ty_},
            {ctxArg_, dst, evalPtr(*s.args[0]), evalPtr(*s.args[1]), attr,
             lock});
        emitReadResult(found, s, "readv");
    }

    void emitWriteV(const Stmt &s) {
        Value *attr = numIndex(*s.args[2]);
        Value *keep = ConstantInt::get(i64Ty_, s.name == "U" ? 1 : 0);
        Value *st = callRt("mvx_writev", i64Ty_,
               {ptrTy_, ptrTy_, ptrTy_, ptrTy_, i64Ty_, i64Ty_, i64Ty_},
               {ctxArg_, evalPtr(*s.value), evalPtr(*s.args[0]),
                evalPtr(*s.args[1]), attr, keep, errFlag(s)});
        emitErrorClause(st, s, "writev");
    }

    void emitMatParse(const Stmt &s) {
        if (!arrayNames_.count(s.name))
            err(s.line, "MATPARSE target " + s.name + " is not DIM'd");
        Value *arr = b_.CreateLoad(ptrTy_, getArraySlot(s.name));
        Value *src = evalPtr(*s.args[0]);
        Value *delim = s.value ? evalPtr(*s.value)
                               : (Value *)ConstantPointerNull::get(ptrTy_);
        callRt("mv_mat_parse", voidTy_, {ptrTy_, ptrTy_, ptrTy_},
               {arr, src, delim});
    }

    void emitMatBuild(const Stmt &s) {
        if (!arrayNames_.count(s.name))
            err(s.line, "MATBUILD source " + s.name + " is not DIM'd");
        const Expr &t = *s.target;
        Value *dst;
        if (t.kind == Expr::K::Var)
            dst = getScalar(t.sval, t.line);
        else if (arrayNames_.count(t.sval) && !num_.numericArray(t.sval))
            dst = arrayElemPtr(t);
        else
            err(t.line, "MATBUILD target must be a variable or array element");
        Value *arr = b_.CreateLoad(ptrTy_, getArraySlot(s.name));
        Value *delim = s.value ? evalPtr(*s.value)
                               : (Value *)ConstantPointerNull::get(ptrTy_);
        callRt("mv_mat_build", voidTy_, {ptrTy_, ptrTy_, ptrTy_},
               {arr, dst, delim});
    }

    void emitMatRead(const Stmt &s) {
        if (!arrayNames_.count(s.name))
            err(s.line, "MATREAD target " + s.name + " is not DIM'd");
        // Analysis demotes MATREAD arrays to the boxed representation.
        Value *arr = b_.CreateLoad(ptrTy_, getArraySlot(s.name));
        Value *lock = lockMode(s, s.name2 == "U");
        Value *found = callRt(
            "mvx_matread", i64Ty_, {ptrTy_, ptrTy_, ptrTy_, ptrTy_, i64Ty_},
            {ctxArg_, arr, evalPtr(*s.args[0]), evalPtr(*s.args[1]), lock});
        emitReadResult(found, s, "matread");
    }

    void emitMatWrite(const Stmt &s) {
        if (!arrayNames_.count(s.name))
            err(s.line, "MATWRITE source " + s.name + " is not DIM'd");
        Value *arr = b_.CreateLoad(ptrTy_, getArraySlot(s.name));
        Value *keep = ConstantInt::get(i64Ty_, s.name2 == "U" ? 1 : 0);
        Value *st = callRt("mvx_matwrite", i64Ty_,
               {ptrTy_, ptrTy_, ptrTy_, ptrTy_, i64Ty_, i64Ty_},
               {ctxArg_, arr, evalPtr(*s.args[0]), evalPtr(*s.args[1]), keep,
                errFlag(s)});
        emitErrorClause(st, s, "matwrite");
    }

    void emitInput(const Stmt &s) {
        const Expr &t = *s.target;
        Value *dst;
        if (t.kind == Expr::K::Var) {
            if (!t.sval.empty() && t.sval[0] == '@')
                err(t.line, "cannot INPUT into system variable " + t.sval);
            dst = getScalar(t.sval, t.line);
        } else if (arrayNames_.count(t.sval)) {
            if (num_.numericArray(t.sval))
                err(t.line, "internal error: INPUT target not demoted");
            dst = arrayElemPtr(t);
        } else {
            err(t.line, "INPUT target must be a variable or array element");
        }
        callRt("mv_input", voidTy_, {ptrTy_, ptrTy_}, {ctxArg_, dst});
    }

    void emitMat(const Stmt &s) {
        if (!arrayNames_.count(s.name))
            err(s.line, "MAT target " + s.name + " is not DIM'd");
        if (!s.name2.empty() && !arrayNames_.count(s.name2))
            err(s.line, "MAT source " + s.name2 + " is not DIM'd");

        if (num_.numericArray(s.name)) {
            // Fill only: MAT copies are demoted to boxed by analysis.
            NumArr &a = numArrSlots(s.name);
            Value *base = b_.CreateLoad(ptrTy_, a.ptr);
            Value *d1 = b_.CreateLoad(i64Ty_, a.d1);
            Value *d2 = b_.CreateLoad(i64Ty_, a.d2);
            Value *cols = b_.CreateSelect(
                b_.CreateICmpEQ(d2, ConstantInt::get(i64Ty_, 0)),
                ConstantInt::get(i64Ty_, 1), d2);
            Value *n = b_.CreateMul(d1, cols);
            if (a.elemTy == b_.getInt8Ty()) {
                Value *byte = b_.CreateTrunc(evalNum(*s.value),
                                             b_.getInt8Ty());
                b_.CreateMemSet(base, byte, n, MaybeAlign(1));
                return;
            }
            Value *v = a.elemTy == dblTy_ ? asDbl(*s.value)
                                          : evalNum(*s.value);
            Value *idxSlot = eb_.CreateAlloca(i64Ty_, nullptr, "mat.i");
            b_.CreateStore(ConstantInt::get(i64Ty_, 0), idxSlot);
            BasicBlock *testBB = newBB("mat.test");
            BasicBlock *bodyBB = newBB("mat.body");
            BasicBlock *doneBB = newBB("mat.done");
            b_.CreateBr(testBB);
            b_.SetInsertPoint(testBB);
            Value *i = b_.CreateLoad(i64Ty_, idxSlot);
            b_.CreateCondBr(b_.CreateICmpSLT(i, n), bodyBB, doneBB);
            b_.SetInsertPoint(bodyBB);
            b_.CreateStore(v, b_.CreateGEP(a.elemTy, base, i));
            b_.CreateStore(b_.CreateAdd(i, ConstantInt::get(i64Ty_, 1)),
                           idxSlot);
            b_.CreateBr(testBB);
            b_.SetInsertPoint(doneBB);
            return;
        }

        Value *arr = b_.CreateLoad(ptrTy_, getArraySlot(s.name));
        if (!s.name2.empty()) {
            Value *src = b_.CreateLoad(ptrTy_, getArraySlot(s.name2));
            call2("mv_arr_copy", arr, src);
        } else {
            call2("mv_arr_fill", arr, evalPtr(*s.value));
        }
    }

    void emitLocate(const Stmt &s) {
        Value *item = evalPtr(*s.args[0]);
        Value *dyn = evalPtr(*s.args[1]);
        Value *zero = ConstantInt::get(i64Ty_, 0);
        Value *a = s.args.size() > 2 ? numIndex(*s.args[2]) : zero;
        Value *v = s.args.size() > 3 ? numIndex(*s.args[3]) : zero;
        Value *order = s.value ? evalPtr(*s.value)
                               : (Value *)ConstantPointerNull::get(ptrTy_);
        Value *posSlot = eb_.CreateAlloca(i64Ty_, nullptr, "locate.pos");
        Value *found = callRt(
            "mv_locate_fn", i64Ty_,
            {ptrTy_, ptrTy_, i64Ty_, i64Ty_, ptrTy_, ptrTy_},
            {item, dyn, a, v, order, posSlot});
        Value *pos = b_.CreateLoad(i64Ty_, posSlot);

        if (num_.numericVar(s.name)) {
            Value *pv = intVar(s.name)
                            ? pos
                            : b_.CreateSIToFP(pos, dblTy_);
            b_.CreateStore(pv, numVarSlot(s.name));
        } else {
            callRt("mv_set_int", voidTy_, {ptrTy_, i64Ty_},
                   {getScalar(s.name, s.line), pos});
        }

        Value *c = b_.CreateICmpNE(found, ConstantInt::get(i64Ty_, 0));
        BasicBlock *thenBB = newBB("loc.then");
        BasicBlock *elseBB = newBB("loc.else");
        BasicBlock *doneBB = newBB("loc.done");
        b_.CreateCondBr(c, thenBB, elseBB);
        b_.SetInsertPoint(thenBB);
        emitBlock(s.body);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(elseBB);
        emitBlock(s.elseBody);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(doneBB);
    }

    // ------------------------------------------------------ GOSUB support

    void ensureGosubState() {
        if (gsSp_) return;
        gsSp_ = eb_.CreateAlloca(i64Ty_, nullptr, "gosub.sp");
        gsStack_ = eb_.CreateAlloca(
            ArrayType::get(i32Ty_, kGosubDepth), nullptr, "gosub.stack");
        eb_.CreateStore(ConstantInt::get(i64Ty_, 0), gsSp_);
    }

    BasicBlock *gosubRetBB() {
        if (!gosubRetBB_)
            gosubRetBB_ = BasicBlock::Create(llctx_, "gosub.ret");
        return gosubRetBB_;
    }

    void emitGosub(const Stmt &s) {
        ensureGosubState();
        Value *sp = b_.CreateLoad(i64Ty_, gsSp_);

        BasicBlock *failBB = newBB("gosub.deep");
        BasicBlock *pushBB = newBB("gosub.push");
        b_.CreateCondBr(
            b_.CreateICmpUGE(sp, ConstantInt::get(i64Ty_, kGosubDepth)),
            failBB, pushBB,
            MDBuilder(llctx_).createUnlikelyBranchWeights());
        b_.SetInsertPoint(failBB);
        callRt("mvx_fatal", voidTy_, {ptrTy_},
               {stringConst("GOSUB nesting deeper than 1024")});
        b_.CreateUnreachable();

        b_.SetInsertPoint(pushBB);
        uint32_t id = (uint32_t)gosubConts_.size();
        Value *cell = b_.CreateGEP(ArrayType::get(i32Ty_, kGosubDepth),
                                   gsStack_,
                                   {ConstantInt::get(i64Ty_, 0), sp});
        b_.CreateStore(ConstantInt::get(i32Ty_, id), cell);
        b_.CreateStore(
            b_.CreateAdd(sp, ConstantInt::get(i64Ty_, 1)), gsSp_);
        b_.CreateBr(labelBBs_.at(s.name));

        BasicBlock *cont = newBB("gosub.cont");
        gosubConts_.push_back(cont);
        b_.SetInsertPoint(cont);
    }

    // ON n GOTO l1, l2, ...: jump to the n-th label (1-based); out of
    // range falls through to the next statement.
    void emitOnGoto(const Stmt &s) {
        Value *idx = numIndex(*s.cond);
        BasicBlock *next = newBB("ongoto.next");
        SwitchInst *sw = b_.CreateSwitch(idx, next,
                                         (unsigned)s.labelList.size());
        for (size_t i = 0; i < s.labelList.size(); i++)
            sw->addCase(cast<ConstantInt>(ConstantInt::get(i64Ty_, (int64_t)i + 1)),
                        labelBBs_.at(s.labelList[i]));
        b_.SetInsertPoint(next);
    }

    // ON n GOSUB l1, l2, ...: GOSUB the n-th label; out of range falls
    // through.  All targets share one continuation.
    void emitOnGosub(const Stmt &s) {
        ensureGosubState();
        Value *idx = numIndex(*s.cond);
        BasicBlock *cont = newBB("ongosub.cont");
        uint32_t id = (uint32_t)gosubConts_.size();
        gosubConts_.push_back(cont);

        Value *sp = b_.CreateLoad(i64Ty_, gsSp_);
        BasicBlock *failBB = newBB("gosub.deep");
        BasicBlock *okBB = newBB("ongosub.ok");
        b_.CreateCondBr(
            b_.CreateICmpUGE(sp, ConstantInt::get(i64Ty_, kGosubDepth)),
            failBB, okBB,
            MDBuilder(llctx_).createUnlikelyBranchWeights());
        b_.SetInsertPoint(failBB);
        callRt("mvx_fatal", voidTy_, {ptrTy_},
               {stringConst("GOSUB nesting deeper than 1024")});
        b_.CreateUnreachable();

        b_.SetInsertPoint(okBB);
        SwitchInst *sw = b_.CreateSwitch(idx, cont,
                                         (unsigned)s.labelList.size());
        for (size_t i = 0; i < s.labelList.size(); i++) {
            BasicBlock *push = newBB("ongosub.push");
            sw->addCase(cast<ConstantInt>(ConstantInt::get(i64Ty_, (int64_t)i + 1)), push);
            b_.SetInsertPoint(push);
            Value *cell = b_.CreateGEP(ArrayType::get(i32Ty_, kGosubDepth),
                                       gsStack_,
                                       {ConstantInt::get(i64Ty_, 0), sp});
            b_.CreateStore(ConstantInt::get(i32Ty_, id), cell);
            b_.CreateStore(b_.CreateAdd(sp, ConstantInt::get(i64Ty_, 1)),
                           gsSp_);
            b_.CreateBr(labelBBs_.at(s.labelList[i]));
        }
        b_.SetInsertPoint(cont);
    }

    // Built once at the end: pop the stack and dispatch to the site
    // after the matching GOSUB; empty stack falls out of the program.
    void finishGosubRet() {
        if (!gosubRetBB_) return;
        gosubRetBB_->insertInto(fn_);
        b_.SetInsertPoint(gosubRetBB_);
        if (!hasGosub_ || gosubConts_.empty()) {
            b_.CreateBr(retBB_);
            return;
        }
        Value *sp = b_.CreateLoad(i64Ty_, gsSp_);
        BasicBlock *popBB = newBB("gosub.pop");
        b_.CreateCondBr(
            b_.CreateICmpEQ(sp, ConstantInt::get(i64Ty_, 0)),
            retBB_, popBB);
        b_.SetInsertPoint(popBB);
        Value *nsp = b_.CreateSub(sp, ConstantInt::get(i64Ty_, 1));
        b_.CreateStore(nsp, gsSp_);
        Value *cell = b_.CreateGEP(ArrayType::get(i32Ty_, kGosubDepth),
                                   gsStack_,
                                   {ConstantInt::get(i64Ty_, 0), nsp});
        Value *id = b_.CreateLoad(i32Ty_, cell);
        BasicBlock *deadBB = newBB("gosub.baddisp");
        SwitchInst *sw = b_.CreateSwitch(id, deadBB,
                                         (unsigned)gosubConts_.size());
        for (uint32_t k = 0; k < gosubConts_.size(); k++)
            sw->addCase(ConstantInt::get(cast<IntegerType>(i32Ty_), k),
                        gosubConts_[k]);
        b_.SetInsertPoint(deadBB);
        b_.CreateUnreachable();
    }

    void emitAssign(const Stmt &s) {
        const Expr &t = *s.target;
        if (t.kind == Expr::K::Extract) {
            const Expr &base = *t.lhs;
            Value *bp;
            if (base.kind == Expr::K::Var && sysConstChar(base.sval) < 0 &&
                !arrayNames_.count(base.sval))
                bp = getScalar(base.sval, base.line);
            else if (base.kind == Expr::K::Paren &&
                     arrayNames_.count(base.sval) &&
                     !num_.numericArray(base.sval))
                bp = arrayElemPtr(base);
            else
                err(t.line, "dynamic-array assignment target must be a "
                            "variable or array element");
            Value *val = evalPtr(*s.value);
            callRt("mv_replace_fn", voidTy_,
                   {ptrTy_, ptrTy_, i64Ty_, i64Ty_, i64Ty_, ptrTy_},
                   {bp, bp, subIdx(t, 0), subIdx(t, 1), subIdx(t, 2), val});
            return;
        }
        if (t.kind == Expr::K::Var) {
            if (sysConstChar(t.sval) >= 0 ||
                (!t.sval.empty() && t.sval[0] == '@'))
                err(t.line, "cannot assign to system variable " + t.sval);
            if (num_.numericVar(t.sval)) {
                Value *v = intVar(t.sval) ? evalNum(*s.value)
                                          : asDbl(*s.value);
                b_.CreateStore(v, numVarSlot(t.sval));
                return;
            }
            evalInto(*s.value, getScalar(t.sval, t.line));
            return;
        }
        // Paren target: must be a DIM'd array element.
        if (!arrayNames_.count(t.sval))
            err(t.line, "cannot assign to " + t.sval + "()");
        if (num_.numericArray(t.sval)) {
            Type *ety = arrElemTy(t.sval);
            Value *v;
            if (ety == dblTy_)          v = asDbl(*s.value);
            else if (ety == i64Ty_)     v = evalNum(*s.value);
            else /* i8, byte literal */ v = b_.CreateTrunc(
                                            evalNum(*s.value), ety);
            b_.CreateStore(v, numElemPtr(t));
            return;
        }
        Value *elem = arrayElemPtr(t);
        evalInto(*s.value, elem);
    }

    void emitDim(const Stmt &s) {
        if (commonArrays_.count(s.name))
            err(s.line, "cannot DIM COMMON array " + s.name);
        Value *d1 = numIndex(*s.args[0]);
        Value *d2 = s.args.size() == 2 ? numIndex(*s.args[1])
                                       : ConstantInt::get(i64Ty_, 0);
        if (num_.numericArray(s.name)) {
            NumArr &a = numArrSlots(s.name);
            call1("mvx_buf_destroy", b_.CreateLoad(ptrTy_, a.ptr));
            Value *cols = b_.CreateSelect(
                b_.CreateICmpEQ(d2, ConstantInt::get(i64Ty_, 0)),
                ConstantInt::get(i64Ty_, 1), d2);
            Value *n = b_.CreateMul(d1, cols);
            uint64_t esz = a.elemTy == b_.getInt8Ty() ? 1 : 8;
            Value *nbytes =
                b_.CreateMul(n, ConstantInt::get(i64Ty_, esz));
            Value *p = callRt("mvx_buf_create", ptrTy_, {i64Ty_}, {nbytes});
            b_.CreateStore(p, a.ptr);
            b_.CreateStore(d1, a.d1);
            b_.CreateStore(d2, a.d2);
            return;
        }
        Value *slot = getArraySlot(s.name);
        Value *old = b_.CreateLoad(ptrTy_, slot);
        call1("mv_arr_destroy", old);
        Value *arr = callRt("mv_arr_create", ptrTy_, {i64Ty_, i64Ty_},
                            {d1, d2});
        b_.CreateStore(arr, slot);
    }

    void emitIf(const Stmt &s) {
        Value *c = evalCond(*s.cond);
        BasicBlock *thenBB = newBB("if.then");
        BasicBlock *elseBB = newBB("if.else");
        BasicBlock *doneBB = newBB("if.done");
        b_.CreateCondBr(c, thenBB, elseBB);
        b_.SetInsertPoint(thenBB);
        emitBlock(s.body);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(elseBB);
        emitBlock(s.elseBody);
        b_.CreateBr(doneBB);
        b_.SetInsertPoint(doneBB);
    }

    void emitFor(const Stmt &s) {
        if (num_.numericVar(s.name)) { emitForNum(s); return; }
        Value *var = getScalar(s.name, s.line);
        Value *limit = newSlot();
        Value *step = newSlot();
        evalInto(*s.from, var);
        evalInto(*s.to, limit);
        if (s.step) evalInto(*s.step, step);
        else callRt("mv_set_int", voidTy_, {ptrTy_, i64Ty_},
                    {step, ConstantInt::get(i64Ty_, 1)});
        Value *stepD = callRt("mv_get_dbl", dblTy_, {ptrTy_}, {step});
        // Loop state lives in allocas, not SSA values: a label inside the
        // body admits jumps that would break SSA dominance otherwise.
        Value *ascSlot = eb_.CreateAlloca(b_.getInt1Ty(), nullptr, "for.asc");
        b_.CreateStore(
            b_.CreateFCmpOGE(stepD, ConstantFP::get(dblTy_, 0.0)), ascSlot);

        BasicBlock *testBB = newBB("for.test");
        BasicBlock *bodyBB = newBB("for.body");
        BasicBlock *incBB = newBB("for.inc");
        BasicBlock *doneBB = newBB("for.done");
        b_.CreateBr(testBB);

        b_.SetInsertPoint(testBB);
        Value *c = callRt("mv_compare", i64Ty_, {ptrTy_, ptrTy_},
                          {var, limit});
        Value *zero = ConstantInt::get(i64Ty_, 0);
        Value *cont = b_.CreateSelect(b_.CreateLoad(b_.getInt1Ty(), ascSlot),
                                      b_.CreateICmpSLE(c, zero),
                                      b_.CreateICmpSGE(c, zero));
        b_.CreateCondBr(cont, bodyBB, doneBB);

        b_.SetInsertPoint(bodyBB);
        loops_.push_back({incBB, doneBB});
        emitBlock(s.body);
        loops_.pop_back();
        b_.CreateBr(incBB);

        b_.SetInsertPoint(incBB);
        b_.SetCurrentDebugLocation(loc(s.line));
        call3("mv_add", var, var, step);
        b_.CreateBr(testBB);

        b_.SetInsertPoint(doneBB);
    }

    // Native FOR loop for a numeric loop variable (i64 or double).
    void emitForNum(const Stmt &s) {
        bool isInt = intVar(s.name);
        Type *ty = isInt ? i64Ty_ : dblTy_;
        Value *var = numVarSlot(s.name);
        b_.CreateStore(isInt ? evalNum(*s.from) : asDbl(*s.from), var);
        // Loop state lives in allocas, not SSA values: a label inside the
        // body admits jumps that would break SSA dominance otherwise.
        // mem2reg promotes these back when the loop is label-free.
        Value *limitSlot = eb_.CreateAlloca(ty, nullptr, "for.lim");
        Value *stepSlot = eb_.CreateAlloca(ty, nullptr, "for.step");
        Value *ascSlot = eb_.CreateAlloca(b_.getInt1Ty(), nullptr, "for.asc");
        b_.CreateStore(isInt ? evalNum(*s.to) : asDbl(*s.to), limitSlot);
        Value *step;
        if (s.step) step = isInt ? evalNum(*s.step) : asDbl(*s.step);
        else step = isInt ? (Value *)ConstantInt::get(i64Ty_, 1)
                          : (Value *)ConstantFP::get(dblTy_, 1.0);
        b_.CreateStore(step, stepSlot);
        b_.CreateStore(
            isInt ? b_.CreateICmpSGE(step, ConstantInt::get(i64Ty_, 0))
                  : b_.CreateFCmpOGE(step, ConstantFP::get(dblTy_, 0.0)),
            ascSlot);

        BasicBlock *testBB = newBB("for.test");
        BasicBlock *bodyBB = newBB("for.body");
        BasicBlock *incBB = newBB("for.inc");
        BasicBlock *doneBB = newBB("for.done");
        b_.CreateBr(testBB);

        b_.SetInsertPoint(testBB);
        Value *v = b_.CreateLoad(ty, var);
        Value *limit = b_.CreateLoad(ty, limitSlot);
        Value *asc = b_.CreateLoad(b_.getInt1Ty(), ascSlot);
        Value *cont = isInt
            ? b_.CreateSelect(asc, b_.CreateICmpSLE(v, limit),
                              b_.CreateICmpSGE(v, limit))
            : b_.CreateSelect(asc, b_.CreateFCmpOLE(v, limit),
                              b_.CreateFCmpOGE(v, limit));
        b_.CreateCondBr(cont, bodyBB, doneBB);

        b_.SetInsertPoint(bodyBB);
        loops_.push_back({incBB, doneBB});
        emitBlock(s.body);
        loops_.pop_back();
        b_.CreateBr(incBB);

        b_.SetInsertPoint(incBB);
        b_.SetCurrentDebugLocation(loc(s.line));
        Value *cur = b_.CreateLoad(ty, var);
        Value *st = b_.CreateLoad(ty, stepSlot);
        b_.CreateStore(isInt ? b_.CreateAdd(cur, st)
                             : b_.CreateFAdd(cur, st), var);
        b_.CreateBr(testBB);

        b_.SetInsertPoint(doneBB);
    }

    void emitLoop(const Stmt &s) {
        BasicBlock *preBB = newBB("loop.pre");
        BasicBlock *doneBB = newBB("loop.done");
        b_.CreateBr(preBB);
        b_.SetInsertPoint(preBB);
        loops_.push_back({preBB, doneBB});
        emitBlock(s.pre);

        if (s.loopCond == Stmt::LoopCond::None) {
            loops_.pop_back();
            b_.CreateBr(preBB);
            b_.SetInsertPoint(doneBB);
            return;
        }
        b_.SetCurrentDebugLocation(loc(s.cond->line));
        tempUsed_ = 0;
        Value *c = evalCond(*s.cond);
        BasicBlock *postBB = newBB("loop.post");
        if (s.loopCond == Stmt::LoopCond::While)
            b_.CreateCondBr(c, postBB, doneBB);
        else
            b_.CreateCondBr(c, doneBB, postBB);
        b_.SetInsertPoint(postBB);
        emitBlock(s.post);
        loops_.pop_back();
        b_.CreateBr(preBB);
        b_.SetInsertPoint(doneBB);
    }

    void emitPrint(const Stmt &s) {
        for (size_t k = 0; k < s.args.size(); k++) {
            if (s.printTabs[k])
                callRt("mv_print_tab", voidTy_, {ptrTy_}, {ctxArg_});
            Value *p = evalPtr(*s.args[k]);
            callRt("mv_print", voidTy_, {ptrTy_, ptrTy_}, {ctxArg_, p});
        }
        if (!s.noNewline)
            callRt("mv_print_nl", voidTy_, {ptrTy_}, {ctxArg_});
    }

    // CALL binds at runtime through mvx_call (the jBASE catalog model):
    // subroutines may live in this program, in cataloged LIB/ shared
    // libraries, in linked packages, or in the system account.
    // CALL @VAR takes the target name from the variable.
    // X = NAME(args): a DEFFUN'd user function.  Shares the subroutine
    // dispatch (mvx_call) with the result slot passed as argv[0]; args go
    // by value into fresh temps so the callee cannot alter the caller.
    void evalFuncCall(const Expr &e, Value *dest) {
        size_t n = e.args.size();
        // value-less RETURN leaves the result empty
        callRt("mv_set_str", voidTy_, {ptrTy_, ptrTy_, i64Ty_},
               {dest, stringConst(""), ConstantInt::get(i64Ty_, 0)});
        // Package extension function: result slot is passed separately, argv
        // holds only the inputs, dispatched through the extension registry.
        if (opts_.extFuncs.count(e.sval)) {
            Value *eargv = eb_.CreateAlloca(ptrTy_,
                               ConstantInt::get(i64Ty_, n ? n : 1), "eargv");
            for (size_t k = 0; k < n; k++) {
                Value *a = acquireTemp();
                evalInto(*e.args[k], a);
                b_.CreateStore(a, b_.CreateGEP(ptrTy_, eargv,
                                               ConstantInt::get(i64Ty_, k)));
            }
            callRt("mvx_ext_invoke", voidTy_,
                   {ptrTy_, ptrTy_, ptrTy_, i32Ty_, ptrTy_},
                   {ctxArg_, stringConst(e.sval), dest,
                    ConstantInt::get(i32Ty_, (int)n), eargv});
            return;
        }
        Value *argv = eb_.CreateAlloca(ptrTy_,
                                       ConstantInt::get(i64Ty_, n + 1), "fargv");
        b_.CreateStore(dest, b_.CreateGEP(ptrTy_, argv,
                                          ConstantInt::get(i64Ty_, 0)));
        for (size_t k = 0; k < n; k++) {
            Value *a = acquireTemp();
            evalInto(*e.args[k], a);
            b_.CreateStore(a, b_.CreateGEP(ptrTy_, argv,
                                           ConstantInt::get(i64Ty_, k + 1)));
        }
        callRt("mvx_call", voidTy_, {ptrTy_, ptrTy_, i32Ty_, ptrTy_},
               {ctxArg_, stringConst(e.sval),
                ConstantInt::get(i32Ty_, (int)n + 1), argv});
    }

    void emitCall(const Stmt &s) {
        size_t n = s.args.size();
        Value *argv = eb_.CreateAlloca(ptrTy_, ConstantInt::get(i64Ty_, n ? n : 1),
                                       "argv");
        for (size_t k = 0; k < n; k++) {
            const Expr &a = *s.args[k];
            Value *p;
            if (a.kind == Expr::K::Var && sysConstChar(a.sval) < 0 &&
                !arrayNames_.count(a.sval))
                p = getScalar(a.sval, a.line);
            else if (a.kind == Expr::K::Paren && arrayNames_.count(a.sval))
                p = arrayElemPtr(a);
            else
                p = evalPtr(a);
            Value *cell = b_.CreateGEP(ptrTy_, argv,
                                       ConstantInt::get(i64Ty_, k));
            b_.CreateStore(p, cell);
        }
        Value *argc = ConstantInt::get(i32Ty_, (int)n);
        if (!s.name.empty() && s.name[0] == '@') {
            Value *namev = getScalar(s.name.substr(1), s.line);
            callRt("mvx_call_var", voidTy_,
                   {ptrTy_, ptrTy_, i32Ty_, ptrTy_},
                   {ctxArg_, namev, argc, argv});
        } else {
            callRt("mvx_call", voidTy_,
                   {ptrTy_, ptrTy_, i32Ty_, ptrTy_},
                   {ctxArg_, stringConst(s.name), argc, argv});
        }
    }

    // -------------------------------------------------------------- function

    void collectArrayNames(const std::vector<StmtP> &stmts) {
        for (const auto &sp : stmts) {
            const Stmt &s = *sp;
            if (s.kind == Stmt::K::Dim) arrayNames_.insert(s.name);
            if (s.kind == Stmt::K::Common)
                for (const auto &item : s.args)
                    if (item->kind == Expr::K::Paren) {
                        arrayNames_.insert(item->sval);
                        commonArrays_.insert(item->sval);
                    }
            collectArrayNames(s.body);
            collectArrayNames(s.elseBody);
            collectArrayNames(s.lockedBody);
            collectArrayNames(s.errorBody);
            collectArrayNames(s.pre);
            collectArrayNames(s.post);
        }
    }

    void collectLabels(const std::vector<StmtP> &stmts) {
        for (const auto &sp : stmts) {
            const Stmt &s = *sp;
            if (s.kind == Stmt::K::Label) {
                if (labelBBs_.count(s.name))
                    err(s.line, "duplicate label " + s.name);
                labelBBs_[s.name] =
                    BasicBlock::Create(llctx_, "L" + s.name);
            }
            if (s.kind == Stmt::K::Gosub || s.kind == Stmt::K::OnGosub)
                hasGosub_ = true;
            collectLabels(s.body);
            collectLabels(s.elseBody);
            collectLabels(s.lockedBody);
            collectLabels(s.errorBody);
            collectLabels(s.pre);
            collectLabels(s.post);
        }
    }

    void checkLabelRefs(const std::vector<StmtP> &stmts) {
        for (const auto &sp : stmts) {
            const Stmt &s = *sp;
            if ((s.kind == Stmt::K::Goto || s.kind == Stmt::K::Gosub) &&
                !labelBBs_.count(s.name))
                err(s.line, "label " + s.name + " is not defined");
            if (s.kind == Stmt::K::OnGoto || s.kind == Stmt::K::OnGosub)
                for (const auto &lbl : s.labelList)
                    if (!labelBBs_.count(lbl))
                        err(s.line, "label " + lbl + " is not defined");
            checkLabelRefs(s.body);
            checkLabelRefs(s.elseBody);
            checkLabelRefs(s.lockedBody);
            checkLabelRefs(s.errorBody);
            checkLabelRefs(s.pre);
            checkLabelRefs(s.post);
        }
    }

    void setupDebug(const std::string &fnName) {
        namespace fs = std::filesystem;
        fs::path p = fs::absolute(prog_.sourcePath);
        diFile_ = dib_.createFile(p.filename().string(),
                                  p.parent_path().string());
        // DWARF has no standard language code for BASIC; use the
        // vendor-reserved range.  Debuggers step by line table regardless.
#if LLVM_VERSION_MAJOR >= 22
        DICompileUnit *cu = dib_.createCompileUnit(
            DISourceLanguageName(dwarf::DW_LANG_lo_user), diFile_,
            "mvx-basic 0.1.0", opts_.optLevel > 0, "", 0);
#else
        DICompileUnit *cu = dib_.createCompileUnit(
            dwarf::DW_LANG_lo_user, diFile_,
            "mvx-basic 0.1.0", opts_.optLevel > 0, "", 0);
#endif
        (void)cu;

        DIType *i64d = dib_.createBasicType("INT64", 64,
                                            dwarf::DW_ATE_signed);
        DIType *dbld = dib_.createBasicType("FLOAT64", 64,
                                            dwarf::DW_ATE_float);
        DIType *chard = dib_.createBasicType("CHAR", 8,
                                             dwarf::DW_ATE_signed_char);
        DIType *strp = dib_.createPointerType(chard, 64);
        SmallVector<Metadata *, 4> members = {
            dib_.createMemberType(nullptr, "TAG", diFile_, 0, 64, 64, 0,
                                  DINode::FlagZero, i64d),
            dib_.createMemberType(nullptr, "I", diFile_, 0, 64, 64, 64,
                                  DINode::FlagZero, i64d),
            dib_.createMemberType(nullptr, "D", diFile_, 0, 64, 64, 128,
                                  DINode::FlagZero, dbld),
            dib_.createMemberType(nullptr, "S", diFile_, 0, 64, 64, 192,
                                  DINode::FlagZero, strp),
        };
        diValTy_ = dib_.createStructType(
            nullptr, "MVVALUE", diFile_, 0, 256, 64, DINode::FlagZero,
            nullptr, dib_.getOrCreateArray(members));

        DISubroutineType *spTy = dib_.createSubroutineType(
            dib_.getOrCreateTypeArray({nullptr}));
        int line = prog_.body.empty() ? 1 : prog_.body.front()->line;
        sp_ = dib_.createFunction(
            diFile_, prog_.isSubroutine ? prog_.name : "PROGRAM", fnName,
            diFile_, (unsigned)line, spTy, (unsigned)line,
            DINode::FlagPrototyped, DISubprogram::SPFlagDefinition);
        fn_->setSubprogram(sp_);
    }

    void buildFunction() {
        collectArrayNames(prog_.body);
        collectLabels(prog_.body);
        checkLabelRefs(prog_.body);
        num_.run(prog_);

        std::string fnName;
        if (prog_.isSubroutine) {
            fnName = "mvx_sub_" + prog_.name;
            FunctionType *ft = FunctionType::get(
                voidTy_, {ptrTy_, i32Ty_, ptrTy_}, false);
            fn_ = Function::Create(ft, Function::ExternalLinkage, fnName,
                                   mod_);
        } else {
            fnName = "mvx_main";
            FunctionType *ft = FunctionType::get(voidTy_, {ptrTy_}, false);
            fn_ = Function::Create(ft, Function::ExternalLinkage, fnName,
                                   mod_);
        }
        ctxArg_ = fn_->getArg(0);
        ctxArg_->setName("ctx");

        setupDebug(fnName);

        BasicBlock *entry = newBB("entry");
        BasicBlock *start = newBB("start");
        retBB_ = BasicBlock::Create(llctx_, "ret", fn_);

        // Entry block: allocas and one-time init, then fall through.
        eb_.SetInsertPoint(entry);
        BranchInst *entryBr = eb_.CreateBr(start);
        eb_.SetInsertPoint(entryBr);   // subsequent allocas go before the br
        eb_.SetCurrentDebugLocation(loc(1));

        b_.SetInsertPoint(start);
        b_.SetCurrentDebugLocation(loc(1));

        if (prog_.isSubroutine) {
            Value *argc = fn_->getArg(1);
            Value *argv = fn_->getArg(2);
            argc->setName("argc");
            argv->setName("argv");
            // A FUNCTION reserves argv[0] for its result; user parameters
            // follow, so its ABI arity is one more than the declared count.
            int off = prog_.isFunction ? 1 : 0;
            callRt("mvx_arity_check", voidTy_,
                   {ptrTy_, i32Ty_, i32Ty_},
                   {stringConst(prog_.name),
                    ConstantInt::get(i32Ty_, (int)prog_.params.size() + off),
                    argc});
            if (prog_.isFunction) {
                Value *rcell = b_.CreateGEP(ptrTy_, argv,
                                            ConstantInt::get(i64Ty_, 0));
                funcResult_ = b_.CreateLoad(ptrTy_, rcell, "result");
            }
            for (size_t k = 0; k < prog_.params.size(); k++) {
                Value *cell = b_.CreateGEP(
                    ptrTy_, argv, ConstantInt::get(i64Ty_, (int)k + off));
                Value *p = b_.CreateLoad(ptrTy_, cell, prog_.params[k]);
                if (arrayNames_.count(prog_.params[k]))
                    err(1, "parameter " + prog_.params[k] +
                               " conflicts with DIM");
                scalars_[prog_.params[k]] = p;
                declareVarDebug(prog_.params[k], p,
                                prog_.body.empty() ? 1
                                    : prog_.body.front()->line);
            }
        }

        {
            std::map<std::string, int64_t> commonCounters;
            emitCommonBindings(prog_.body, commonCounters);
        }

        emitBlock(prog_.body);
        b_.CreateBr(retBB_);

        b_.SetInsertPoint(retBB_);
        b_.SetCurrentDebugLocation(
            loc(prog_.body.empty() ? 1 : prog_.body.back()->line));
        b_.CreateRetVoid();

        finishGosubRet();

        dib_.finalize();
    }
};

void CodeGen::run(const std::string &outPath) {
    i64Ty_ = Type::getInt64Ty(llctx_);
    i32Ty_ = Type::getInt32Ty(llctx_);
    dblTy_ = Type::getDoubleTy(llctx_);
    voidTy_ = Type::getVoidTy(llctx_);
    ptrTy_ = PointerType::get(llctx_, 0);
    valTy_ = StructType::create(llctx_, {i64Ty_, i64Ty_, dblTy_, ptrTy_},
                                "mv_value");

    mod_.addModuleFlag(Module::Warning, "Debug Info Version",
                       DEBUG_METADATA_VERSION);
    mod_.addModuleFlag(Module::Warning, "Dwarf Version", 4);

    buildFunction();

    std::string verifyErr;
    raw_string_ostream vos(verifyErr);
    if (verifyModule(mod_, &vos))
        report_fatal_error(Twine("internal error: invalid IR generated:\n") +
                           vos.str());

    // Target setup.
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    Triple triple(sys::getDefaultTargetTriple());
    std::string lookupErr;
    TargetOptions topts;
    // The TargetRegistry / TargetMachine / Module APIs took a Triple in
    // place of a triple string in LLVM 21.
#if LLVM_VERSION_MAJOR >= 21
    const Target *target = TargetRegistry::lookupTarget(triple, lookupErr);
    if (!target) report_fatal_error(Twine(lookupErr));
    TargetMachine *tm = target->createTargetMachine(
        triple, sys::getHostCPUName(), "", topts, Reloc::PIC_);
    mod_.setDataLayout(tm->createDataLayout());
    mod_.setTargetTriple(triple);
#else
    const Target *target = TargetRegistry::lookupTarget(triple.str(), lookupErr);
    if (!target) report_fatal_error(Twine(lookupErr));
    TargetMachine *tm = target->createTargetMachine(
        triple.str(), sys::getHostCPUName(), "", topts, Reloc::PIC_);
    mod_.setDataLayout(tm->createDataLayout());
    mod_.setTargetTriple(triple.str());
#endif

    // Optimisation.
    if (opts_.optLevel > 0) {
        LoopAnalysisManager lam;
        FunctionAnalysisManager fam;
        CGSCCAnalysisManager cgam;
        ModuleAnalysisManager mam;
        PassBuilder pb(tm);
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(
            opts_.optLevel >= 2 ? OptimizationLevel::O2
                                : OptimizationLevel::O1);
        mpm.run(mod_, mam);
    }

    if (opts_.emitLLVM) {
        std::error_code ec;
        raw_fd_ostream os(outPath + ".ll", ec, sys::fs::OF_Text);
        if (!ec) mod_.print(os, nullptr);
    }

    std::error_code ec;
    raw_fd_ostream dest(outPath, ec, sys::fs::OF_None);
    if (ec)
        report_fatal_error(Twine("cannot write ") + outPath + ": " +
                           ec.message());
    legacy::PassManager emit;
    if (tm->addPassesToEmitFile(emit, dest, nullptr,
                                CodeGenFileType::ObjectFile))
        report_fatal_error("target cannot emit object files");
    emit.run(mod_);
    dest.flush();
}

} // namespace

void compileToObject(const Program &prog, const std::string &outPath,
                     const CodegenOptions &opts) {
    CodeGen cg(prog, opts);
    cg.run(outPath);
}

} // namespace mvx
