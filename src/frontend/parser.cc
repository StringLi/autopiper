/*
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "frontend/ast.h"
#include "frontend/parser.h"

using namespace std;

// TODO: make this a little nicer with some parser combinator-like helpers.
// Each term function should return a unique_ptr of its parse node or failure.
// We should have an easy helper (macro) to call a term function and place the
// result in a field in the result, returning a failure if term function
// returns a failure.

namespace autopiper {
namespace frontend {

bool Parser::Parse(AST* ast) {
    // A program is a series of defs.
    if (TryConsume(Token::EOFTOKEN)) {
        return true;
    }

    Expect(Token::IDENT);

    if (CurToken().s == "type") {
        Consume();
        ASTRef<ASTTypeDef> td(new ASTTypeDef());
        if (!ParseTypeDef(td.get())) {
            return false;
        }
        ast->types.push_back(move(td));
    } else if (CurToken().s == "func") {
        Consume();
        ASTRef<ASTFunctionDef> fd(new ASTFunctionDef());
        if (!ParseFuncDef(fd.get())) {
            return false;
        }
        ast->functions.push_back(move(fd));
    } else {
        Error("Expected 'type' or 'func' keyword.");
        return false;
    }

    return true;
}

bool Parser::ParseFuncDef(ASTFunctionDef* def) {
    def->name.reset(new ASTIdent());
    if (!Expect(Token::IDENT)) {
        return false;
    }
    if (CurToken().s == "entry") {
        def->is_entry = true;
        Consume();
        if (!Expect(Token::IDENT)) {
            return false;
        }
    }

    if (!ParseIdent(def->name.get())) {
        return false;
    }

    if (!Consume(Token::LPAREN)) {
        return false;
    }
    if (!ParseFuncArgList(def)) {
        return false;
    }
    if (!Consume(Token::RPAREN)) {
        return false;
    }
    if (!Consume(Token::COLON)) {
        return false;
    }
    def->return_type.reset(new ASTType());
    if (!ParseType(def->return_type.get())) {
        return false;
    }

    def->block.reset(new ASTStmtBlock());
    if (!ParseBlock(def->block.get())) {
        return false;
    }

    return true;
}

bool Parser::ParseFuncArgList(ASTFunctionDef* def) {
    while (true) {
        if (CurToken().type == Token::RPAREN) {
            break;
        }

        ASTRef<ASTParam> param(new ASTParam());
        param->ident.reset(new ASTIdent());
        if (!ParseIdent(param->ident.get())) {
            return false;
        }
        if (!Consume(Token::COLON)) {
            return false;
        }
        param->type.reset(new ASTType());
        if (!ParseType(param->type.get())) {
            return false;
        }
    }

    return true;
}

bool Parser::ParseBlock(ASTStmtBlock* block) {
    if (!Consume(Token::LBRACE)) {
        return false;
    }

    while (true) {
        if (CurToken().type == Token::RBRACE) {
            break;
        }

        ASTRef<ASTStmt> stmt(new ASTStmt());
        if (!ParseStmt(stmt.get())) {
            return false;
        }
        block->stmts.push_back(move(stmt));
    }

    if (!Consume(Token::RBRACE)) {
        return false;
    }

    return true;
}

bool Parser::ParseTypeDef(ASTTypeDef* def) {
    def->ident.reset(new ASTIdent());
    if (!ParseIdent(def->ident.get())) {
        return false;
    }
    if (!Consume(Token::LBRACE)) {
        return false;
    }
    while (true) {
        if (TryConsume(Token::RBRACE)) {
            break;
        }
        ASTRef<ASTTypeField> field;
        field->ident.reset(new ASTIdent());
        if (!ParseIdent(field->ident.get())) {
            return false;
        }
        if (!Consume(Token::COLON)) {
            return false;
        }
        field->type.reset(new ASTType());
        if (!ParseType(field->type.get())) {
            return false;
        }
        if (!Consume(Token::SEMICOLON)) {
            return false;
        }
        def->fields.push_back(move(field));
    }
    return true;
}

bool Parser::ParseIdent(ASTIdent* id) {
    if (!Expect(Token::IDENT)) {
        return false;
    }
    id->name = CurToken().s;
    Consume();
    return true;
}

bool Parser::ParseType(ASTType* ty) {
    if (!Expect(Token::IDENT)) {
        return false;
    }
    if (CurToken().s == "port") {
        ty->is_port = true;
        Consume();
        if (!Expect(Token::IDENT)) {
            return false;
        }
    }
    ty->ident.reset(new ASTIdent());
    if (!ParseIdent(ty->ident.get())) {
        return false;
    }
    return true;
}

bool Parser::ParseStmt(ASTStmt* st) {
    if (TryExpect(Token::LBRACE)) {
        st->block.reset(new ASTStmtBlock());
        return ParseBlock(st->block.get());
    }

#define HANDLE_STMT_TYPE(str, field, name)                   \
    if (TryExpect(Token::IDENT) && CurToken().s == str) {    \
        Consume();                                           \
        st-> field .reset(new ASTStmt ## name());            \
        return ParseStmt ## name(st-> field .get());         \
    }

    HANDLE_STMT_TYPE("let", let, Let);
    HANDLE_STMT_TYPE("if", if_, If);
    HANDLE_STMT_TYPE("while", while_, While);
    HANDLE_STMT_TYPE("break", break_, Break);
    HANDLE_STMT_TYPE("continue", continue_, Continue);
    HANDLE_STMT_TYPE("write", write, Write);
    HANDLE_STMT_TYPE("spawn", spawn, Spawn);

#undef HANDLE_STMT_TYPE

    // No keywords matched, so we must be seeing the left-hand side identifier
    // in an assignment.
    st->assign.reset(new ASTStmtAssign());
    return ParseStmtAssign(st->assign.get());
}

bool Parser::ParseStmtLet(ASTStmtLet* let) {
    // parent already consumed "def" keyword.
    
    let->lhs.reset(new ASTIdent());
    if (!ParseIdent(let->lhs.get())) {
        return false;
    }

    if (TryConsume(Token::COLON)) {
        let->type.reset(new ASTType());
        if (!ParseType(let->type.get())) {
            return false;
        }
    }

    if (!Consume(Token::EQUALS)) {
        return false;
    }

    let->rhs = ParseExpr();
    if (!let->rhs) {
        return false;
    }

    return Consume(Token::SEMICOLON);
}

bool Parser::ParseStmtAssign(ASTStmtAssign* assign) {
    assign->lhs.reset(new ASTIdent());
    if (!ParseIdent(assign->lhs.get())) {
        return false;
    }

    if (!Consume(Token::EQUALS)) {
        return false;
    }

    assign->rhs = ParseExpr();
    if (!assign->rhs) {
        return false;
    }

    return Consume(Token::SEMICOLON);
}

bool Parser::ParseStmtIf(ASTStmtIf* if_) {
    if (!Consume(Token::LPAREN)) {
        return false;
    }
    if_->condition = ParseExpr();
    if (!if_->condition) {
        return false;
    }
    if (!Consume(Token::RPAREN)) {
        return false;
    }
    if_->if_body.reset(new ASTStmt());
    if (!ParseStmt(if_->if_body.get())) {
        return false;
    }
    if (TryExpect(Token::IDENT) && CurToken().s == "else") {
        Consume();
        if_->else_body.reset(new ASTStmt());
        if (!ParseStmt(if_->else_body.get())) {
            return false;
        }
    }

    return true;
}

bool Parser::ParseStmtWhile(ASTStmtWhile* while_) {
    if (!Consume(Token::LPAREN)) {
        return false;
    }
    while_->condition = ParseExpr();
    if (!while_->condition) {
        return false;
    }
    if (!Consume(Token::RPAREN)) {
        return false;
    }
    while_->body.reset(new ASTStmt());
    if (!ParseStmt(while_->body.get())) {
        return false;
    }
    return true;
}

bool Parser::ParseStmtBreak(ASTStmtBreak* break_) {
    return Consume(Token::SEMICOLON);
}

bool Parser::ParseStmtContinue(ASTStmtContinue* continue_) {
    return Consume(Token::SEMICOLON);
}

bool Parser::ParseStmtWrite(ASTStmtWrite* write) {
    write->port.reset(new ASTIdent());
    if (!ParseIdent(write->port.get())) {
        return false;
    }
    write->rhs = ParseExpr();
    if (!write->rhs) {
        return false;
    }
    return Consume(Token::SEMICOLON);
}

bool Parser::ParseStmtSpawn(ASTStmtSpawn* spawn) {
    spawn->body.reset(new ASTStmt());
    return ParseStmt(spawn->body.get());
}

ASTRef<ASTExpr> Parser::ParseExpr() {
    return ParseExprGroup1();
}

// Group 1: ternary op
ASTRef<ASTExpr> Parser::ParseExprGroup1() {
    auto expr = ParseExprGroup2();
    if (TryConsume(Token::QUESTION)) {
        auto op1 = ParseExprGroup2();
        if (!Consume(Token::COLON)) {
            return astnull<ASTExpr>();
        }
        auto op2 = ParseExprGroup1();
        ASTRef<ASTExpr> ret(new ASTExpr());
        ret->op = ASTExpr::SEL;
        ret->ops.push_back(move(expr));
        ret->ops.push_back(move(op1));
        ret->ops.push_back(move(op2));
        return ret;
    }
    return expr;
}

// This is a little hacky. We're abstracting out the left-associative
// recursive-descent logic, and we want to take a template argument for the
// next-lower nonterminal (precedence group). We actually take a member
// function pointer, but devirtualization *should* reduce this down to a direct
// function call given sufficient optimization settings.
template<
    Parser::ExprGroupParser this_level,
    Parser::ExprGroupParser next_level,
    typename ...Args>
ASTRef<ASTExpr> Parser::ParseLeftAssocBinops(Args&&... args) {
  ASTRef<ASTExpr> ret = (this->*next_level)();
  ASTRef<ASTExpr> op_node(new ASTExpr());
  op_node->ops.push_back(move(ret));
  if (ParseLeftAssocBinopsRHS<this_level, next_level>(
      op_node.get(), args...)) {
    return op_node;
  } else {
    ret = move(op_node->ops[0]);
    return ret;
  }
}

template<
    Parser::ExprGroupParser this_level,
    Parser::ExprGroupParser next_level,
    typename ...Args>
bool Parser::ParseLeftAssocBinopsRHS(ASTExpr* expr,
                                     Token::Type op_token,
                                     ASTExpr::Op op,
                                     Args&&... args) {
  if (TryExpect(op_token)) {
    Consume();
    auto rhs = (this->*this_level)();
    if (!rhs) {
      return false;
    }
    expr->op = op;
    expr->ops.push_back(move(rhs));
    return true;
  }
  return ParseLeftAssocBinopsRHS<this_level, next_level>(expr, args...);
}

template<
    Parser::ExprGroupParser this_level,
    Parser::ExprGroupParser next_level>
bool Parser::ParseLeftAssocBinopsRHS(ASTExpr* expr) {
  return false;
}

// Group 2: logical bitwise or
ASTRef<ASTExpr> Parser::ParseExprGroup2() {
  return ParseLeftAssocBinops<
      &Parser::ParseExprGroup2,
      &Parser::ParseExprGroup3>(
            Token::PIPE, ASTExpr::OR);
}

// Group 3: logical bitwise xor
ASTRef<ASTExpr> Parser::ParseExprGroup3() {
  return ParseLeftAssocBinops<
      &Parser::ParseExprGroup3,
      &Parser::ParseExprGroup4>(
            Token::CARET, ASTExpr::XOR);
}

// Group 4: logical bitwise and
ASTRef<ASTExpr> Parser::ParseExprGroup4() {
  return ParseLeftAssocBinops<
      &Parser::ParseExprGroup4,
      &Parser::ParseExprGroup5>(
            Token::AMPERSAND, ASTExpr::AND);
}

// Group 5: equality operators
ASTRef<ASTExpr> Parser::ParseExprGroup5() {
  return ParseLeftAssocBinops<
      &Parser::ParseExprGroup5,
      &Parser::ParseExprGroup6>(
            Token::DOUBLE_EQUAL, ASTExpr::EQ,
            Token::NOT_EQUAL, ASTExpr::NE);
}

// Group 6: comparison operators
ASTRef<ASTExpr> Parser::ParseExprGroup6() {
  return ParseLeftAssocBinops<
      &Parser::ParseExprGroup6,
      &Parser::ParseExprGroup7>(
            Token::LANGLE, ASTExpr::LT,
            Token::RANGLE, ASTExpr::GT,
            Token::LESS_EQUAL, ASTExpr::LE,
            Token::GREATER_EQUAL, ASTExpr::GE);
}

// Group 7: bitshift operators
ASTRef<ASTExpr> Parser::ParseExprGroup7() {
  return ParseLeftAssocBinops<
      &Parser::ParseExprGroup7,
      &Parser::ParseExprGroup8>(
            Token::LSH, ASTExpr::LSH,
            Token::RSH, ASTExpr::RSH);
}

// Group 8: add/sub
ASTRef<ASTExpr> Parser::ParseExprGroup8() {
  return ParseLeftAssocBinops<
      &Parser::ParseExprGroup8,
      &Parser::ParseExprGroup9>(
            Token::PLUS, ASTExpr::ADD,
            Token::DASH, ASTExpr::SUB);
}

// Group 9: mul/div/rem
ASTRef<ASTExpr> Parser::ParseExprGroup9() {
  return ParseLeftAssocBinops<
      &Parser::ParseExprGroup9,
      &Parser::ParseExprGroup10>(
            Token::STAR, ASTExpr::MUL,
            Token::SLASH, ASTExpr::DIV,
            Token::PERCENT, ASTExpr::REM);
}

// Group 10: unary ops (~, unary +, unary -)
ASTRef<ASTExpr> Parser::ParseExprGroup10() {
    return astnull<ASTExpr>();
}

// Group 11: array subscripting ([]), field dereferencing (.), function calls
ASTRef<ASTExpr> Parser::ParseExprGroup11() {
    return astnull<ASTExpr>();
}

// Atoms/terminals: identifiers, literals
ASTRef<ASTExpr> Parser::ParseExprAtom() {
    // Identifier: either a variable reference, a function call, or a port
    // read.
    if (TryExpect(Token::IDENT)) {
        const string& ident = CurToken().s;

        if (ident == "read") {
            ASTRef<ASTExpr> ret(new ASTExpr());
            ret->op = ASTExpr::PORTREAD;
            ret->ident.reset(new ASTIdent());
            if (!ParseIdent(ret->ident.get())) {
                return astnull<ASTExpr>();
            }
            return ret;
        }

        // Otherwise, either a variable reference or a function call.
        // Consume the identifier and check whether a parenthesis follows.
        Consume();
    }

    return astnull<ASTExpr>();
}

}  // namespace frontend
}  // namespace autopiper
