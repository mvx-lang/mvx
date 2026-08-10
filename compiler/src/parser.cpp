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

#include "parser.h"
#include "lexer.h"

#include <filesystem>
#include <unordered_map>

namespace mvx {

namespace {

class Parser {
public:
    Parser(std::vector<Token> toks, std::string item,
           const std::set<std::string> &extFuncs)
        : toks_(std::move(toks)), item_(std::move(item)) {
        // package extension functions read as user functions (sets e->call)
        for (const auto &f : extFuncs) deffuns_[f] = -1;
    }

    Program run(const std::string &sourcePath) {
        Program prog;
        prog.sourcePath = sourcePath;
        skipEols();

        if (at(Tok::KwSubroutine) || at(Tok::KwFunction)) {
            bool isFunc = cur().kind == Tok::KwFunction;
            advance();
            prog.isSubroutine = true;   // functions share the subroutine ABI
            prog.isFunction = isFunc;
            prog.name = expect(Tok::Ident,
                               isFunc ? "function name" : "subroutine name")
                            .text;
            if (isFunc) deffuns_[prog.name] = -1;  // own body may recurse
            if (at(Tok::LParen)) {
                advance();
                if (!at(Tok::RParen)) {
                    for (;;) {
                        bool isMat = false;         // MAT param: a whole array by ref
                        if (at(Tok::KwMat)) { advance(); isMat = true; }
                        prog.params.push_back(
                            expect(Tok::Ident, "parameter name").text);
                        prog.paramIsMat.push_back(isMat);
                        if (!at(Tok::Comma)) break;
                        advance();
                    }
                }
                expect(Tok::RParen, "')'");
            }
            endStatement();
        }

        while (!at(Tok::Eof)) {
            skipEols();
            if (at(Tok::Eof)) break;
            if (at(Tok::KwEnd)) {          // top-level END: end of program
                advance();
                skipEols();
                if (!at(Tok::Eof))
                    err("statements after top-level END");
                break;
            }
            prog.body.push_back(statement());
        }
        return prog;
    }

private:
    std::vector<Token> toks_;
    std::string item_;
    size_t pos_ = 0;

    // EQUATE bindings: a name expands to a clone of its expression at
    // every use site (compile-time; must be declared before use).
    std::unordered_map<std::string, ExprP> equates_;
    // DEFFUN'd user functions: name -> argument count (-1 = unspecified).
    std::unordered_map<std::string, int> deffuns_;

    static ExprP cloneExpr(const Expr &e) {
        auto c = std::make_unique<Expr>();
        c->kind = e.kind;
        c->line = e.line;
        c->call = e.call;
        c->ival = e.ival;
        c->fval = e.fval;
        c->sval = e.sval;
        c->op = e.op;
        for (const auto &a : e.args) c->args.push_back(cloneExpr(*a));
        if (e.lhs) c->lhs = cloneExpr(*e.lhs);
        if (e.rhs) c->rhs = cloneExpr(*e.rhs);
        return c;
    }

    const Token &cur() const { return toks_[pos_]; }
    const Token &peek(size_t k = 1) const {
        size_t p = pos_ + k;
        return toks_[p < toks_.size() ? p : toks_.size() - 1];
    }
    bool at(Tok k) const { return cur().kind == k; }
    Token advance() { return toks_[pos_++]; }

    [[noreturn]] void err(const std::string &msg) const {
        throw CompileError(item_, cur().line, msg);
    }
    Token expect(Tok k, const char *what) {
        if (!at(k)) err(std::string("expected ") + what);
        return advance();
    }
    void skipEols() { while (at(Tok::Eol) || at(Tok::Semi)) advance(); }
    void endStatement() {
        if (at(Tok::Eol) || at(Tok::Semi)) { advance(); return; }
        if (at(Tok::Eof)) return;
        err("unexpected token at end of statement");
    }

    // ------------------------------------------------------------ statements

    // Statements up to (not consuming) one of the given terminators.
    // Terminators are only recognised at statement start.
    std::vector<StmtP> block(std::initializer_list<Tok> terms) {
        std::vector<StmtP> out;
        for (;;) {
            skipEols();
            if (at(Tok::Eof)) err("unexpected end of file inside block");
            bool done = false;
            for (Tok t : terms)
                if (at(t)) { done = true; break; }
            if (done) return out;
            out.push_back(statement());
        }
    }

    // Statements on the current line, for single-line IF bodies.
    // Stops at EOL or ELSE.
    std::vector<StmtP> lineStmts() {
        std::vector<StmtP> out;
        for (;;) {
            if (at(Tok::Eol) || at(Tok::Eof) || at(Tok::KwElse)) return out;
            out.push_back(statement());
            if (at(Tok::Semi)) advance();
        }
    }

    StmtP statement() {
        int line = cur().line;
        StmtP s;
        switch (cur().kind) {
        case Tok::KwDim:      s = dimStmt();   break;
        case Tok::KwIf:       s = ifStmt();    break;
        case Tok::KwFor:      s = forStmt();   break;
        case Tok::KwLoop:     s = loopStmt();  break;
        case Tok::KwPrint:
        case Tok::KwCrt:      s = printStmt(); break;
        case Tok::KwBegin:    s = caseStmt();  break;
        case Tok::KwLocate:   s = locateStmt(); break;
        case Tok::KwInput:
            advance();
            s = mk(Stmt::K::Input);
            s->target = primaryRef();
            endStatementSoft();
            break;
        case Tok::KwOpen: {
            advance();
            s = mk(Stmt::K::Open);
            s->args.push_back(expression());
            if (at(Tok::Comma)) {                   // OPEN "DICT","F" form
                advance();
                s->args.push_back(expression());
            }
            expect(Tok::KwTo, "TO in OPEN");
            s->name = expect(Tok::Ident, "file variable").text;
            thenElse(*s);
            break;
        }
        case Tok::KwRead:
        case Tok::KwReadu: {
            bool lock = cur().kind == Tok::KwReadu;
            advance();
            s = mk(Stmt::K::ReadF);
            if (lock) s->name = "U";
            s->target = primaryRef();
            expect(Tok::KwFrom, "FROM in READ");
            s->args.push_back(expression());        // file
            expect(Tok::Comma, "','");
            s->args.push_back(expression());        // id
            lockedThenElse(*s);
            break;
        }
        case Tok::KwWrite:
        case Tok::KwWriteu: {
            bool keep = cur().kind == Tok::KwWriteu;
            advance();
            s = mk(Stmt::K::WriteF);
            if (keep) s->name = "U";
            s->value = expression();                // record
            if (at(Tok::KwOn) || at(Tok::KwTo)) advance();
            else err("expected ON in WRITE");
            s->args.push_back(expression());        // file
            expect(Tok::Comma, "','");
            s->args.push_back(expression());        // id
            onErrorClause(*s);
            endStatementSoft();
            break;
        }
        case Tok::KwReadv:
        case Tok::KwReadvu: {
            bool lock = cur().kind == Tok::KwReadvu;
            advance();
            s = mk(Stmt::K::ReadV);
            if (lock) s->name = "U";
            s->target = primaryRef();
            expect(Tok::KwFrom, "FROM in READV");
            s->args.push_back(expression());        // file
            expect(Tok::Comma, "','");
            s->args.push_back(expression());        // id
            expect(Tok::Comma, "',' before attribute");
            s->args.push_back(expression());        // attribute number
            lockedThenElse(*s);
            break;
        }
        case Tok::KwWritev:
        case Tok::KwWritevu: {
            bool keep = cur().kind == Tok::KwWritevu;
            advance();
            s = mk(Stmt::K::WriteV);
            if (keep) s->name = "U";
            s->value = expression();                // value
            if (at(Tok::KwOn) || at(Tok::KwTo)) advance();
            else err("expected ON in WRITEV");
            s->args.push_back(expression());        // file
            expect(Tok::Comma, "','");
            s->args.push_back(expression());        // id
            expect(Tok::Comma, "',' before attribute");
            s->args.push_back(expression());        // attribute number
            onErrorClause(*s);
            endStatementSoft();
            break;
        }
        case Tok::KwMatread:
        case Tok::KwMatreadu: {
            bool lock = cur().kind == Tok::KwMatreadu;
            advance();
            s = mk(Stmt::K::MatRead);
            if (lock) s->name2 = "U";
            s->name = expect(Tok::Ident, "array name after MATREAD").text;
            expect(Tok::KwFrom, "FROM in MATREAD");
            s->args.push_back(expression());        // file
            expect(Tok::Comma, "','");
            s->args.push_back(expression());        // id
            lockedThenElse(*s);
            break;
        }
        case Tok::KwMatwrite:
        case Tok::KwMatwriteu: {
            bool keep = cur().kind == Tok::KwMatwriteu;
            advance();
            s = mk(Stmt::K::MatWrite);
            if (keep) s->name2 = "U";
            s->name = expect(Tok::Ident, "array name after MATWRITE").text;
            if (at(Tok::KwOn) || at(Tok::KwTo)) advance();
            else err("expected ON in MATWRITE");
            s->args.push_back(expression());        // file
            expect(Tok::Comma, "','");
            s->args.push_back(expression());        // id
            onErrorClause(*s);
            endStatementSoft();
            break;
        }
        case Tok::KwMatparse: {
            // MATPARSE arr FROM strexpr {USING delim}: split a string
            // across the array's elements in memory (no file).
            advance();
            s = mk(Stmt::K::MatParse);
            s->name = expect(Tok::Ident, "array name after MATPARSE").text;
            expect(Tok::KwFrom, "FROM in MATPARSE");
            s->args.push_back(expression());        // source string
            if (at(Tok::KwUsing)) {
                advance();
                s->value = expression();            // delimiter
            }
            endStatementSoft();
            break;
        }
        case Tok::KwMatbuild: {
            // MATBUILD strvar FROM arr {USING delim}: join the array's
            // elements into a string in memory (no file).
            advance();
            s = mk(Stmt::K::MatBuild);
            s->target = primaryRef();               // string lvalue
            expect(Tok::KwFrom, "FROM in MATBUILD");
            s->name = expect(Tok::Ident, "array name in MATBUILD").text;
            if (at(Tok::KwUsing)) {
                advance();
                s->value = expression();            // delimiter
            }
            endStatementSoft();
            break;
        }
        case Tok::KwDelete: {
            advance();
            s = mk(Stmt::K::DeleteF);
            s->args.push_back(expression());
            expect(Tok::Comma, "',' in DELETE");
            s->args.push_back(expression());
            onErrorClause(*s);
            endStatementSoft();
            break;
        }
        case Tok::KwRelease: {
            advance();
            s = mk(Stmt::K::Release);
            if (!at(Tok::Eol) && !at(Tok::Semi) && !at(Tok::Eof) &&
                !at(Tok::KwElse)) {
                s->args.push_back(expression());
                expect(Tok::Comma, "',' in RELEASE");
                s->args.push_back(expression());
            }
            endStatementSoft();
            break;
        }
        case Tok::KwSelect: {
            advance();
            s = mk(Stmt::K::Select);
            s->args.push_back(expression());
            endStatementSoft();
            break;
        }
        case Tok::KwReadnext: {
            advance();
            s = mk(Stmt::K::Readnext);
            s->name = expect(Tok::Ident, "id variable").text;
            thenElse(*s);
            break;
        }
        case Tok::KwDeffun: {
            // DEFFUN name(nargs) {CALLING "cat.name"}: declare a user
            // function so name(...) is a call, not an array reference.
            advance();
            s = mk(Stmt::K::Nop);
            std::string nm = expect(Tok::Ident, "function name after DEFFUN")
                                 .text;
            int nargs = -1;
            if (at(Tok::LParen)) {
                advance();
                if (!at(Tok::RParen))
                    nargs = (int)expect(Tok::IntLit, "argument count").ival;
                expect(Tok::RParen, "')'");
            }
            if (at(Tok::KwCalling)) {   // cataloged name; recorded, not used
                advance();
                expect(Tok::StrLit, "cataloged name string");
            }
            deffuns_[nm] = nargs;
            endStatementSoft();
            break;
        }
        case Tok::KwEcho: {
            advance();
            s = mk(Stmt::K::Echo);
            if (at(Tok::KwOn)) {
                advance();
                s->name = "ON";
            } else {
                Token t = expect(Tok::Ident, "ON or OFF after ECHO");
                if (t.text != "OFF" && t.text != "ON")
                    err("expected ON or OFF after ECHO");
                s->name = t.text;
            }
            endStatementSoft();
            break;
        }
        case Tok::KwFormlist:
            advance();
            s = mk(Stmt::K::Formlist);
            s->value = expression();
            endStatementSoft();
            break;
        case Tok::KwEquate: {
            // EQUATE name TO expr  (LITERALLY/LIT are accepted for TO):
            // a compile-time constant, expanded wherever the name is used.
            advance();
            s = mk(Stmt::K::Nop);
            std::string nm = expect(Tok::Ident, "name after EQUATE").text;
            if (at(Tok::KwTo) || at(Tok::KwLit)) advance();
            else err("expected TO or LITERALLY in EQUATE");
            if (equates_.count(nm)) err("EQUATE " + nm + " redefined");
            equates_[nm] = expression();
            endStatementSoft();
            break;
        }
        case Tok::KwExecute: {
            advance();
            s = mk(Stmt::K::Execute);
            s->value = expression();
            for (;;) {
                if (at(Tok::KwCapturing)) {
                    advance();
                    s->name = expect(Tok::Ident, "CAPTURING variable").text;
                } else if (at(Tok::KwReturning)) {
                    advance();
                    s->name2 = expect(Tok::Ident, "RETURNING variable").text;
                } else break;
            }
            endStatementSoft();
            break;
        }
        case Tok::KwCommon: {
            advance();
            s = mk(Stmt::K::Common);
            if (at(Tok::Slash)) {                   // COMMON /BLOCK/ ...
                advance();
                s->name2 = expect(Tok::Ident, "common block name").text;
                expect(Tok::Slash, "'/' closing block name");
            }
            for (;;) {
                s->args.push_back(primaryRef());    // Var or name(dims)
                if (!at(Tok::Comma)) break;
                advance();
            }
            endStatementSoft();
            break;
        }
        case Tok::KwMat:
            advance();
            s = mk(Stmt::K::Mat);
            s->name = expect(Tok::Ident, "array name after MAT").text;
            expect(Tok::Eq, "'=' in MAT assignment");
            if (at(Tok::KwMat)) {
                advance();
                s->name2 = expect(Tok::Ident, "array name").text;
            } else {
                s->value = expression();
            }
            endStatementSoft();
            break;
        case Tok::KwCall:     s = callStmt();  break;
        case Tok::IntLit: {
            // A number at statement start is a classic Pick numeric
            // label.  It may stand alone or precede a statement on the
            // same line, so no end-of-statement check here.
            Token t = advance();
            s = mk(Stmt::K::Label);
            s->name = std::to_string(t.ival);
            break;
        }
        case Tok::KwGoto:
        case Tok::KwGo: {
            if (advance().kind == Tok::KwGo && at(Tok::KwTo))
                advance();                          // GO TO n
            s = mk(Stmt::K::Goto);
            s->name = labelRef("label after GOTO");
            endStatementSoft();
            break;
        }
        case Tok::KwGosub:
            advance();
            s = mk(Stmt::K::Gosub);
            s->name = labelRef("label after GOSUB");
            endStatementSoft();
            break;
        case Tok::KwOn: {                       // ON expr GOTO/GOSUB n, n, ...
            advance();
            ExprP sel = expression();
            Stmt::K k;
            if (at(Tok::KwGoto) || at(Tok::KwGo)) {
                advance();
                if (at(Tok::KwTo)) advance();
                k = Stmt::K::OnGoto;
            } else if (at(Tok::KwGosub)) {
                advance();
                k = Stmt::K::OnGosub;
            } else {
                err("expected GOTO or GOSUB after ON expr");
            }
            s = mk(k);
            s->cond = std::move(sel);
            for (;;) {
                s->labelList.push_back(labelRef("label in ON list"));
                if (!at(Tok::Comma)) break;
                advance();
            }
            endStatementSoft();
            break;
        }
        case Tok::KwReturn:
            advance();
            s = mk(Stmt::K::Return);
            if (at(Tok::LParen)) {          // RETURN(expr): function result
                advance();
                s->value = expression();
                expect(Tok::RParen, "')' after RETURN value");
            }
            endStatementSoft();
            break;
        case Tok::KwStop:
            advance();
            s = mk(Stmt::K::Stop);
            // STOP <code>: an optional numeric process exit status (default 0).
            if (!at(Tok::Eol) && !at(Tok::Semi) && !at(Tok::Eof) &&
                !at(Tok::KwElse))
                s->value = expression();
            endStatementSoft();
            break;
        case Tok::KwNull:                       // explicit no-op statement
            advance();
            s = mk(Stmt::K::Nop);
            endStatementSoft();
            break;
        case Tok::KwContinue:
            advance();
            s = mk(Stmt::K::Continue);
            endStatementSoft();
            break;
        case Tok::KwExit:
            advance();
            s = mk(Stmt::K::Exit);
            endStatementSoft();
            break;
        case Tok::Ident:
            // NAME: at statement start is an alphanumeric label (distinct from
            // ':' concatenation, which only appears in expression context).  It
            // may stand alone or precede a statement on the same line, like a
            // numeric label — so no end-of-statement check.  `NAME :=` is the
            // append-assignment operator, not a label.
            if (peek().kind == Tok::Colon && peek(2).kind != Tok::Eq) {
                s = mk(Stmt::K::Label);
                s->name = advance().text;       // the label name
                advance();                      // consume ':'
                break;
            }
            s = assignStmt();
            break;
        default:
            err("expected statement");
        }
        s->line = line;
        return s;
    }

    StmtP mk(Stmt::K k) {
        auto s = std::make_unique<Stmt>();
        s->kind = k;
        s->line = cur().line;
        return s;
    }

    // A GOTO/GOSUB/ON target: a numeric label (stringified) or an
    // alphanumeric one.  Labels live in one string-keyed namespace.
    std::string labelRef(const char *ctx) {
        if (at(Tok::IntLit)) return std::to_string(advance().ival);
        return expect(Tok::Ident, ctx).text;
    }

    // Statement terminator inside single-line IF bodies: ELSE and EOL are
    // left for the caller; ';' is consumed by the caller loop.
    void endStatementSoft() {
        if (at(Tok::Eol) || at(Tok::Semi) || at(Tok::Eof) ||
            at(Tok::KwElse) || at(Tok::KwThen) || at(Tok::KwLocked))
            return;
        err("unexpected token at end of statement");
    }

    StmtP assignStmt() {
        if (at(Tok::Ident) && equates_.count(cur().text) &&
            peek().kind != Tok::LParen)
            err("cannot assign to EQUATE name " + cur().text);
        auto s = mk(Stmt::K::Assign);
        s->target = maybeExtract(primaryRef());
        // Compound assignment: `target OP= value` desugars to
        // `target = target OP value` (`:=` appends, `+= -= *= /=`).
        BinOp cop = BinOp::Add;
        bool compound = peek().kind == Tok::Eq;
        if (compound && at(Tok::Colon))      cop = BinOp::Cat;
        else if (compound && at(Tok::Plus))  cop = BinOp::Add;
        else if (compound && at(Tok::Minus)) cop = BinOp::Sub;
        else if (compound && at(Tok::Star))  cop = BinOp::Mul;
        else if (compound && at(Tok::Slash)) cop = BinOp::Div;
        else compound = false;
        if (compound) {
            int line = advance().line;               // the OP
            advance();                               // the '='
            s->value = bin(cop, cloneExpr(*s->target), expression(), line);
        } else {
            expect(Tok::Eq, "'=' in assignment");
            s->value = expression();
        }
        endStatementSoft();
        return s;
    }

    // Dynamic-array extraction postfix: base<a[,v[,s]]>.  '<' is ambiguous
    // with less-than; attempt the extraction parse and backtrack to the
    // comparison reading if it does not close with '>'.  Subscripts parse
    // at additive precedence, so comparisons inside need parentheses —
    // which is what resolves the grammar, as in classic MV compilers.
    ExprP maybeExtract(ExprP base) {
        for (;;) {
            // Juxtaposed format mask: expr "R#10" — classic FMT operator.
            if (at(Tok::StrLit)) {
                Token m = advance();
                auto e = std::make_unique<Expr>();
                e->kind = Expr::K::Fmt;
                e->line = m.line;
                e->lhs = std::move(base);
                e->rhs = std::make_unique<Expr>();
                e->rhs->kind = Expr::K::StrLit;
                e->rhs->sval = m.text;
                e->rhs->line = m.line;
                base = std::move(e);
                continue;
            }
            // Substring: base[start, len]
            if (at(Tok::LBrack)) {
                int line = advance().line;
                auto e = std::make_unique<Expr>();
                e->kind = Expr::K::Substr;
                e->line = line;
                e->lhs = std::move(base);
                e->args.push_back(expression());
                expect(Tok::Comma, "',' in substring");
                e->args.push_back(expression());
                expect(Tok::RBrack, "']'");
                base = std::move(e);
                continue;
            }
            if (!at(Tok::Lt)) return base;
            size_t save = pos_;
            int line = cur().line;
            advance();
            std::vector<ExprP> subs;
            bool ok = true;
            try {
                for (;;) {
                    subs.push_back(addExpr());
                    if (at(Tok::Comma) && subs.size() < 3) {
                        advance();
                        continue;
                    }
                    break;
                }
                if (!at(Tok::Gt)) ok = false;
            } catch (const CompileError &) {
                ok = false;
            }
            if (!ok) {
                pos_ = save;
                break;
            }
            advance();                              // '>'
            auto e = std::make_unique<Expr>();
            e->kind = Expr::K::Extract;
            e->line = line;
            e->lhs = std::move(base);
            e->args = std::move(subs);
            base = std::move(e);
        }
        return base;
    }

    // A variable or name(subscripts) reference for assignment targets.
    ExprP primaryRef() {
        Token name = expect(Tok::Ident, "variable name");
        auto e = std::make_unique<Expr>();
        e->line = name.line;
        e->sval = name.text;
        if (at(Tok::LParen)) {
            e->kind = Expr::K::Paren;
            advance();
            if (!at(Tok::RParen)) {
                for (;;) {
                    e->args.push_back(expression());
                    if (!at(Tok::Comma)) break;
                    advance();
                }
            }
            expect(Tok::RParen, "')'");
            if (deffuns_.count(e->sval)) e->call = true;   // user function
        } else {
            e->kind = Expr::K::Var;
        }
        return e;
    }

    StmtP dimStmt() {
        advance();
        auto s = mk(Stmt::K::Dim);
        s->name = expect(Tok::Ident, "array name").text;
        expect(Tok::LParen, "'(' after DIM name");
        s->args.push_back(expression());
        if (at(Tok::Comma)) {
            advance();
            s->args.push_back(expression());
        }
        expect(Tok::RParen, "')'");
        endStatementSoft();
        return s;
    }

    // Shared THEN/ELSE clause parsing for IF and LOCATE.  Block form
    // (classic Pick): THEN <eol> ... END [ELSE <eol> ... END]; both
    // clauses also come in single-line form.  At least one is required.
    void elseClause(Stmt &s) {
        if (!at(Tok::KwElse)) return;
        advance();
        if (at(Tok::Eol) || at(Tok::Semi)) {
            advance();
            s.elseBody = block({Tok::KwEnd});
            advance();                              // END
        } else {
            s.elseBody = lineStmts();
        }
    }

    // READU/READVU/MATREADU may carry a LOCKED clause before THEN/ELSE
    // (classic Pick): it runs when another session holds the record, so
    // the read neither blocks nor takes the lock.  THEN/ELSE stay
    // optional once LOCKED is present.
    // Optional ON ERROR clause: runs on a backend fault (a WRITE the
    // driver rejects), instead of aborting the program.  "ERROR" is not
    // a reserved word — it is the identifier following the ON preposition.
    void onErrorClause(Stmt &s) {
        if (!(at(Tok::KwOn) && peek().kind == Tok::Ident &&
              peek().text == "ERROR"))
            return;
        advance();                                  // ON
        advance();                                  // ERROR
        s.hasError = true;
        if (at(Tok::Eol) || at(Tok::Semi)) {
            advance();
            s.errorBody = block({Tok::KwEnd});
            advance();                              // END
        } else {
            while (!at(Tok::KwThen) && !at(Tok::KwElse) &&
                   !at(Tok::KwLocked) && !at(Tok::Eol) && !at(Tok::Eof)) {
                s.errorBody.push_back(statement());
                if (at(Tok::Semi)) advance();
            }
        }
    }

    void lockedThenElse(Stmt &s) {
        onErrorClause(s);                           // ON ERROR before LOCKED
        if (at(Tok::KwLocked)) {
            advance();
            s.hasLocked = true;
            if (at(Tok::Eol) || at(Tok::Semi)) {
                advance();
                s.lockedBody = block({Tok::KwEnd});
                advance();                          // END
            } else {
                // single-line: statements up to THEN/ELSE/eol
                while (!at(Tok::KwThen) && !at(Tok::KwElse) &&
                       !at(Tok::Eol) && !at(Tok::Eof)) {
                    s.lockedBody.push_back(statement());
                    if (at(Tok::Semi)) advance();
                }
            }
        }
        if (at(Tok::KwThen) || at(Tok::KwElse)) thenElse(s);
        else endStatementSoft();
    }

    void thenElse(Stmt &s) {
        if (at(Tok::KwThen)) {
            advance();
            if (at(Tok::Eol) || at(Tok::Semi)) {
                advance();
                s.body = block({Tok::KwEnd});
                advance();                          // END
            } else {
                s.body = lineStmts();
            }
            elseClause(s);
            endStatementSoft();
            return;
        }
        if (!at(Tok::KwElse)) err("expected THEN or ELSE");
        elseClause(s);
        endStatementSoft();
    }

    StmtP ifStmt() {
        advance();
        auto s = mk(Stmt::K::If);
        s->cond = expression();
        thenElse(*s);
        return s;
    }

    // BEGIN CASE / CASE expr / END CASE, desugared to a nested IF chain.
    StmtP caseStmt() {
        advance();                                  // BEGIN
        expect(Tok::KwCase, "CASE after BEGIN");
        endStatement();
        struct Branch { ExprP cond; std::vector<StmtP> body; int line; };
        std::vector<Branch> brs;
        for (;;) {
            skipEols();
            if (at(Tok::KwEnd)) break;
            int line = cur().line;
            expect(Tok::KwCase, "CASE or END CASE");
            ExprP c = expression();
            endStatement();
            auto body = block({Tok::KwCase, Tok::KwEnd});
            brs.push_back({std::move(c), std::move(body), line});
        }
        advance();                                  // END
        expect(Tok::KwCase, "CASE after END");
        endStatementSoft();

        StmtP chain;
        for (auto it = brs.rbegin(); it != brs.rend(); ++it) {
            auto ifs = std::make_unique<Stmt>();
            ifs->kind = Stmt::K::If;
            ifs->line = it->line;
            ifs->cond = std::move(it->cond);
            ifs->body = std::move(it->body);
            if (chain) ifs->elseBody.push_back(std::move(chain));
            chain = std::move(ifs);
        }
        if (!chain) {                               // no branches: no-op
            chain = std::make_unique<Stmt>();
            chain->kind = Stmt::K::If;
            chain->cond = std::make_unique<Expr>();
            chain->cond->kind = Expr::K::IntLit;
            chain->cond->ival = 0;
        }
        return chain;
    }

    // Classic Pick:  LOCATE(item, dyn{, attr{, val}}; var{; order}) THEN/ELSE
    StmtP locateStmt() {
        advance();
        auto s = mk(Stmt::K::Locate);
        expect(Tok::LParen, "'(' after LOCATE");
        s->args.push_back(expression());            // item
        expect(Tok::Comma, "','");
        s->args.push_back(expression());            // dynamic array
        while (at(Tok::Comma) && s->args.size() < 4) {
            advance();
            s->args.push_back(expression());        // attr#, value#
        }
        expect(Tok::Semi, "';' before setting variable");
        s->name = expect(Tok::Ident, "setting variable").text;
        if (at(Tok::Semi)) {
            advance();
            s->value = expression();                // order (AL/AR/DL/DR)
        }
        expect(Tok::RParen, "')'");
        thenElse(*s);
        return s;
    }

    StmtP forStmt() {
        advance();
        auto s = mk(Stmt::K::For);
        s->name = expect(Tok::Ident, "FOR variable").text;
        expect(Tok::Eq, "'=' in FOR");
        s->from = expression();
        expect(Tok::KwTo, "TO");
        s->to = expression();
        if (at(Tok::KwStep)) {
            advance();
            s->step = expression();
        }
        endStatement();
        s->body = block({Tok::KwNext});
        advance();                                  // NEXT
        if (at(Tok::Ident)) {
            Token v = advance();
            if (v.text != s->name)
                throw CompileError(item_, v.line,
                    "NEXT " + v.text + " does not match FOR " + s->name);
        }
        endStatementSoft();
        return s;
    }

    StmtP loopStmt() {
        advance();
        auto s = mk(Stmt::K::Loop);
        // Classic Pick/R83 allows the test on the same line as LOOP
        // (`LOOP WHILE cond DO ... REPEAT`), so only require a statement
        // terminator when a pre-test body follows on later lines.
        if (!at(Tok::KwWhile) && !at(Tok::KwUntil) && !at(Tok::KwRepeat))
            endStatement();
        s->pre = block({Tok::KwWhile, Tok::KwUntil, Tok::KwRepeat});
        if (at(Tok::KwWhile) || at(Tok::KwUntil)) {
            s->loopCond = at(Tok::KwWhile) ? Stmt::LoopCond::While
                                           : Stmt::LoopCond::Until;
            advance();
            s->cond = expression();
            if (at(Tok::KwDo)) advance();
            endStatement();
            s->post = block({Tok::KwRepeat});
        }
        advance();                                  // REPEAT
        endStatementSoft();
        return s;
    }

    StmtP printStmt() {
        advance();
        auto s = mk(Stmt::K::Print);
        if (at(Tok::Eol) || at(Tok::Semi) || at(Tok::Eof)) {
            endStatementSoft();
            return s;                               // bare PRINT: newline
        }
        bool tab = false;
        for (;;) {
            s->args.push_back(expression());
            s->printTabs.push_back(tab);
            if (at(Tok::Comma)) { advance(); tab = true; continue; }
            if (at(Tok::Colon) &&
                (peek().kind == Tok::Eol || peek().kind == Tok::Semi ||
                 peek().kind == Tok::Eof || peek().kind == Tok::KwElse)) {
                advance();
                s->noNewline = true;
            }
            break;
        }
        endStatementSoft();
        return s;
    }

    StmtP callStmt() {
        advance();
        auto s = mk(Stmt::K::Call);
        s->name = expect(Tok::Ident, "subroutine name").text;
        if (at(Tok::LParen)) {
            advance();
            if (!at(Tok::RParen)) {
                for (;;) {
                    if (at(Tok::KwMat)) {        // CALL SUB(MAT arr): pass whole array
                        int ln = cur().line;
                        advance();
                        auto a = std::make_unique<Expr>();
                        a->kind = Expr::K::Var;
                        a->line = ln;
                        a->sval = expect(Tok::Ident, "array name after MAT").text;
                        a->matArg = true;
                        s->args.push_back(std::move(a));
                    } else {
                        s->args.push_back(expression());
                    }
                    if (!at(Tok::Comma)) break;
                    advance();
                }
            }
            expect(Tok::RParen, "')'");
        }
        endStatementSoft();
        return s;
    }

    // ----------------------------------------------------------- expressions
    // Precedence, loosest to tightest:
    //   OR < AND < relational < concat(:) < add < mul < power < unary < primary

    ExprP expression() { return orExpr(); }

    ExprP bin(BinOp op, ExprP l, ExprP r, int line) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::K::Bin;
        e->op = op;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        e->line = line;
        return e;
    }

    ExprP orExpr() {
        ExprP l = andExpr();
        while (at(Tok::KwOr)) {
            int line = advance().line;
            l = bin(BinOp::Or, std::move(l), andExpr(), line);
        }
        return l;
    }

    ExprP andExpr() {
        ExprP l = relExpr();
        while (at(Tok::KwAnd)) {
            int line = advance().line;
            l = bin(BinOp::And, std::move(l), relExpr(), line);
        }
        return l;
    }

    ExprP relExpr() {
        ExprP l = catExpr();
        for (;;) {
            BinOp op;
            switch (cur().kind) {
            case Tok::Eq: op = BinOp::Eq; break;
            case Tok::Ne: op = BinOp::Ne; break;
            case Tok::Lt: op = BinOp::Lt; break;
            case Tok::Le: op = BinOp::Le; break;
            case Tok::Gt: op = BinOp::Gt; break;
            case Tok::Ge: op = BinOp::Ge; break;
            case Tok::KwMatches: op = BinOp::Matches; break;
            default: return l;
            }
            int line = advance().line;
            l = bin(op, std::move(l), catExpr(), line);
        }
    }

    bool startsOperand(const Token &t) const {
        switch (t.kind) {
        case Tok::Ident: case Tok::IntLit: case Tok::FltLit: case Tok::StrLit:
        case Tok::LParen: case Tok::Minus: case Tok::Plus: case Tok::KwNot:
            return true;
        default:
            return false;
        }
    }

    ExprP catExpr() {
        ExprP l = addExpr();
        // ':' is concatenation only when an operand follows; otherwise it
        // belongs to the statement (PRINT trailing colon).
        while (at(Tok::Colon) && startsOperand(peek())) {
            int line = advance().line;
            l = bin(BinOp::Cat, std::move(l), addExpr(), line);
        }
        return l;
    }

    ExprP addExpr() {
        ExprP l = mulExpr();
        for (;;) {
            if (at(Tok::Plus)) {
                int line = advance().line;
                l = bin(BinOp::Add, std::move(l), mulExpr(), line);
            } else if (at(Tok::Minus)) {
                int line = advance().line;
                l = bin(BinOp::Sub, std::move(l), mulExpr(), line);
            } else return l;
        }
    }

    ExprP mulExpr() {
        ExprP l = powExpr();
        for (;;) {
            if (at(Tok::Star)) {
                int line = advance().line;
                l = bin(BinOp::Mul, std::move(l), powExpr(), line);
            } else if (at(Tok::Slash)) {
                int line = advance().line;
                l = bin(BinOp::Div, std::move(l), powExpr(), line);
            } else return l;
        }
    }

    ExprP powExpr() {
        ExprP l = unaryExpr();
        if (at(Tok::Caret)) {                       // right-associative
            int line = advance().line;
            l = bin(BinOp::Pow, std::move(l), powExpr(), line);
        }
        return l;
    }

    ExprP unaryExpr() {
        if (at(Tok::Minus)) {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::K::Neg;
            e->lhs = unaryExpr();
            e->line = line;
            return e;
        }
        if (at(Tok::Plus)) { advance(); return unaryExpr(); }
        if (at(Tok::KwNot)) {
            int line = advance().line;
            expect(Tok::LParen, "'(' after NOT");
            auto e = std::make_unique<Expr>();
            e->kind = Expr::K::Not;
            e->lhs = expression();
            e->line = line;
            expect(Tok::RParen, "')'");
            return e;
        }
        return primary();
    }

    ExprP primary() {
        int line = cur().line;
        switch (cur().kind) {
        case Tok::IntLit: {
            Token t = advance();
            auto e = std::make_unique<Expr>();
            e->kind = Expr::K::IntLit;
            e->ival = t.ival;
            e->line = line;
            return maybeExtract(std::move(e));
        }
        case Tok::FltLit: {
            Token t = advance();
            auto e = std::make_unique<Expr>();
            e->kind = Expr::K::FltLit;
            e->fval = t.fval;
            e->line = line;
            return maybeExtract(std::move(e));
        }
        case Tok::StrLit: {
            Token t = advance();
            auto e = std::make_unique<Expr>();
            e->kind = Expr::K::StrLit;
            e->sval = t.text;
            e->line = line;
            return maybeExtract(std::move(e));
        }
        case Tok::LParen: {
            advance();
            ExprP e = expression();
            expect(Tok::RParen, "')'");
            return maybeExtract(std::move(e));
        }
        case Tok::KwDelete: {
            // DELETE doubles as the dynamic-array function in expressions.
            advance();
            auto e = std::make_unique<Expr>();
            e->kind = Expr::K::Paren;
            e->line = line;
            e->sval = "DELETE";
            expect(Tok::LParen, "'(' after DELETE");
            if (!at(Tok::RParen)) {
                for (;;) {
                    e->args.push_back(expression());
                    if (!at(Tok::Comma)) break;
                    advance();
                }
            }
            expect(Tok::RParen, "')'");
            return maybeExtract(std::move(e));
        }
        case Tok::Ident: {
            // Expand an EQUATE constant in expression context (a bare
            // name, not a name( ... ) call/subscript).
            auto it = equates_.find(cur().text);
            if (it != equates_.end() && peek().kind != Tok::LParen) {
                advance();
                return maybeExtract(cloneExpr(*it->second));
            }
            return maybeExtract(primaryRef());
        }
        default:
            err("expected expression");
        }
    }
};

} // namespace

Program parse(const std::string &src, const std::string &sourcePath,
              const std::set<std::string> &extFuncs) {
    std::string item = std::filesystem::path(sourcePath).filename().string();
    Parser p(lex(src, item), item, extFuncs);
    return p.run(sourcePath);
}

} // namespace mvx
