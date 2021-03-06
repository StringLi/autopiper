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

#include "frontend/codegen.h"
#include "common/util.h"

#include <sstream>

using namespace std;

namespace autopiper {
namespace frontend {

// -------------------------- CodeGenContext ----------------------------------

CodeGenContext::CodeGenContext(AST* ast) {
    prog_.reset(new IRProgram());
    gensym_ = 1;
    curbb_ = nullptr;
    ast_ = ast;
    prog_->crosslinked_args_bbs = true;
}

std::string CodeGenContext::GenSym(const char* prefix) {
    ostringstream os;
    if (!prefix) {
        os << "__codegen_gensym__";
    } else {
        os << prefix << "_";
    }
    os << gensym_;
    gensym_++;
    return os.str();
}

IRBB* CodeGenContext::AddBB(const char* label_prefix) {
    unique_ptr<IRBB> bb(new IRBB());
    bb->label = GenSym(label_prefix);
    IRBB* ret = bb.get();
    prog_->bbs.push_back(move(bb));
    return ret;
}

IRStmt* CodeGenContext::AddIRStmt(
        IRBB* bb, std::unique_ptr<IRStmt> stmt,
        const ASTExpr* expr) {
    if (expr) {
        expr_to_ir_map_[expr] = stmt.get();
    }
    if (stmt->valnum >= prog_->next_valnum) {
        prog_->next_valnum = stmt->valnum + 1;
    }
    IRStmt* ret = stmt.get();
    bb->stmts.push_back(move(stmt));
    return ret;
}

// -------------------------- CodeGenPass ----------------------------------

CodeGenPass::Result
CodeGenPass::ModifyASTFunctionDefPre(ASTRef<ASTFunctionDef>& node) {
    // Skip non-entry functions completely.
    if (!node->is_entry) {
        // Don't recurse.
        return VISIT_TERMINAL;
    }

    // Start a new BB and mark it as an etry point. Name it after the function.
    IRBB* bb = ctx_->AddBB();
    bb->label = node->name->name;
    bb->is_entry = true;
    ctx_->AddEntry(bb);
    ctx_->SetCurBB(bb);

    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTFunctionDefPost(ASTRef<ASTFunctionDef>& node) {
    // Add a 'done' at the end in case the function body did not.
    if (node->is_entry) {
        unique_ptr<IRStmt> done_stmt(new IRStmt());
        done_stmt->valnum = ctx_->Valnum();
        done_stmt->type = IRStmtDone;
        ctx_->AddIRStmt(ctx_->CurBB(), move(done_stmt));
    }
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtLetPost(ASTRef<ASTStmtLet>& node) {
    ctx_->Bindings().Set(node.get(), node->rhs.get());
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtAssignPre(ASTRef<ASTStmtAssign>& node) {
    // Never codegen the LHS, because it may contain array or reg references
    // (for example) that are actually writes, not reads. We manually invoke
    // codegen on only the RHS subtree.
    ASTVisitor visitor;
    if (!visitor.ModifyASTExpr(node->rhs, this)) {
        return VISIT_END;
    }

    // Don't recurse.
    return VISIT_TERMINAL;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtAssignPost(ASTRef<ASTStmtAssign>& node) {
    if (node->lhs->op == ASTExpr::VAR) {
        // Simple variable assignment.  Associate the binding (let) with the
        // new ASTExpr (the RHS).
        ctx_->Bindings().Set(node->lhs->def, node->rhs.get());
    } else if (node->lhs->op == ASTExpr::REG_REF) {
        // Reg write.
        const ASTExpr* regdef = FindEntityDef(
                node->lhs->ops[0].get(), ASTExpr::REG_INIT, node->lhs.get());
        if (!regdef) {
            return VISIT_END;
        }

        unique_ptr<IRStmt> reg_write(new IRStmt());
        reg_write->valnum = ctx_->Valnum();
        reg_write->type = IRStmtRegWrite;
        reg_write->port_name = regdef->ident->name;
        reg_write->width = regdef->inferred_type.width;

        IRStmt* value = ctx_->GetIRStmt(node->rhs.get());
        reg_write->args.push_back(value);
        reg_write->arg_nums.push_back(value->valnum);

        ctx_->AddIRStmt(ctx_->CurBB(), move(reg_write));
    } else if (node->lhs->op == ASTExpr::ARRAY_REF) {
        // Generate an array-write IR stmt.
        // The first op must be a direct var reference to an array -- arrays
        // nested in other lvalues (other arrays or fields of aggregate types,
        // for example) are not supported.
        const ASTExpr* arraydef = FindEntityDef(
                node->lhs->ops[0].get(),
                ASTExpr::ARRAY_INIT, node->lhs.get());
        if (!arraydef) {
            return VISIT_END;
        }

        unique_ptr<IRStmt> array_write(new IRStmt());
        array_write->valnum = ctx_->Valnum();
        array_write->type = IRStmtArrayWrite;
        array_write->width = node->rhs->inferred_type.width;
        array_write->port_name = arraydef->ident->name;

        // Manually codegen the index arg, because we disable traversal over
        // the LHS subtree (to avoid turning the whole ARRAY_REF into a read).
        ASTVisitor visitor;
        if (!visitor.ModifyASTExpr(node->lhs->ops[1], this)) {
            return VISIT_END;
        }

        IRStmt* index_arg = ctx_->GetIRStmt(node->lhs->ops[1].get());
        IRStmt* value = ctx_->GetIRStmt(node->rhs.get());
        array_write->args.push_back(index_arg);
        array_write->arg_nums.push_back(index_arg->valnum);
        array_write->args.push_back(value);
        array_write->arg_nums.push_back(value->valnum);

        ctx_->AddIRStmt(ctx_->CurBB(), move(array_write));
    } else if (node->lhs->op == ASTExpr::FIELD_REF) {
        // This should have been desugared by the type-lower pass to (i)
        // constructing the new aggregate value in a temporary and (ii)
        // assigning to the whole aggregate.
        assert(false);
    } else {
        Error(node.get(),
                "Cannot assign to non-variable, non-array-slot, "
                "non-field-slot lvalue.");
        return VISIT_END;
    }

    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtWritePost(ASTRef<ASTStmtWrite>& node) {
    unique_ptr<IRStmt> write_stmt(new IRStmt());
    write_stmt->valnum = ctx_->Valnum();
    const ASTExpr* portdef = FindEntityDef(
            node->port.get(), ASTExpr::PORTDEF, node->port.get());
    if (!portdef) {
        return VISIT_END;
    }
    if (portdef->inferred_type.is_port) {
        write_stmt->type = IRStmtPortWrite;
    } else if (portdef->inferred_type.is_chan) {
        write_stmt->type = IRStmtChanWrite;
    } else {
        Error(node.get(), "Write to something not a port or chan");
    }
    write_stmt->port_name = portdef->ident->name;
    IRStmt* val = ctx_->GetIRStmt(node->rhs.get());
    write_stmt->args.push_back(val);
    write_stmt->arg_nums.push_back(val->valnum);
    write_stmt->width = node->rhs->inferred_type.width;
    write_stmt->port_default = portdef->constant;
    write_stmt->port_has_default = portdef->has_constant;
    ctx_->AddIRStmt(ctx_->CurBB(), move(write_stmt));
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtKillPost(ASTRef<ASTStmtKill>& node) {
    unique_ptr<IRStmt> stmt(new IRStmt());
    stmt->valnum = ctx_->Valnum();
    stmt->type = IRStmtKill;
    ctx_->AddIRStmt(ctx_->CurBB(), move(stmt));
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtKillYoungerPost(
                ASTRef<ASTStmtKillYounger>& node) {
    unique_ptr<IRStmt> stmt(new IRStmt());
    stmt->valnum = ctx_->Valnum();
    stmt->type = IRStmtKillYounger;
    ctx_->AddIRStmt(ctx_->CurBB(), move(stmt));

    // Codegen any OnKillYounger blocks.
    ASTVisitor visitor;
    for (auto& onkillyounger : c_.back().onkillyoungers) {
        if (!visitor.ModifyASTStmtBlock(onkillyounger->body, this)) {
            return VISIT_TERMINAL;
        }
    }

    return VISIT_CONTINUE;
}

static bool VerifyNoSideEffects(ASTExpr* expr, ErrorCollector* coll) {
    // The only way for an expr to perform a side-effect is for it to include a
    // stmt block (which in turn may contain any statement) or for it to be an
    // array reference.
    if (expr->op == ASTExpr::STMTBLOCK ||
        expr->op == ASTExpr::ARRAY_REF) {
        coll->ReportError(expr->loc, ErrorCollector::ERROR,
                "Expression contains a potential side-effect "
                "(possibly a function call or a statement-block expression "
                "or an array read), which is not allowed in a kill-if "
                "condition. Such conditions may only contain simple "
                "port/chan reads, variable references, and computations "
                "on those values.");
        return false;
    }
    for (auto& op : expr->ops) {
        if (!VerifyNoSideEffects(op.get(), coll)) {
            return false;
        }
    }
    return true;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtKillIfPost(ASTRef<ASTStmtKillIf>& node) {
    // Verify that condition contains only side-effect-free operations.
    if (!VerifyNoSideEffects(node->condition.get(), Errors())) {
        return VISIT_END;
    }
    unique_ptr<IRStmt> stmt(new IRStmt());
    stmt->valnum = ctx_->Valnum();
    stmt->type = IRStmtKillIf;
    IRStmt* cond = ctx_->GetIRStmt(node->condition.get());
    stmt->args.push_back(cond);
    stmt->arg_nums.push_back(cond->valnum);
    ctx_->AddIRStmt(ctx_->CurBB(), move(stmt));
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtTimingPre(ASTRef<ASTStmtTiming>& node) {
    unique_ptr<IRTimeVar> timevar(new IRTimeVar());
    timevar->name = ctx_->GenSym("timing");
    ctx_->ir()->timevar_map[timevar->name] = timevar.get();
    c_.back().timing_stack.push_back(timevar.get());
    c_.back().timing_last_stage.push_back(0);

    // Create an implicit barrier with offset 0.
    unique_ptr<IRStmt> timing_barrier(new IRStmt());
    timing_barrier->valnum = ctx_->Valnum();
    timing_barrier->type = IRStmtTimingBarrier;
    timing_barrier->timevar = timevar.get();
    timevar->uses.push_back(timing_barrier.get());
    timing_barrier->time_offset = 0;
    ctx_->AddIRStmt(ctx_->CurBB(), move(timing_barrier));

    ctx_->ir()->timevars.push_back(move(timevar));
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtTimingPost(ASTRef<ASTStmtTiming>& node) {
    // Create an implicit barrier with offset equal to the last stage statement
    // to ensure the last stage can't leak into later stages.
    unique_ptr<IRStmt> timing_barrier(new IRStmt());
    timing_barrier->valnum = ctx_->Valnum();
    timing_barrier->type = IRStmtTimingBarrier;
    timing_barrier->timevar = c_.back().timing_stack.back();
    c_.back().timing_stack.back()->uses.push_back(timing_barrier.get());
    timing_barrier->time_offset = c_.back().timing_last_stage.back();
    ctx_->AddIRStmt(ctx_->CurBB(), move(timing_barrier));

    c_.back().timing_stack.pop_back();
    c_.back().timing_last_stage.pop_back();
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtStagePost(ASTRef<ASTStmtStage>& node) {
    if (c_.back().timing_stack.empty()) {
        Error(node.get(),
                "'stage' statement appears outside of a timing {} block. "
                "Staging barriers can occur only inside the context of a "
                "timing {} block.");
        return VISIT_END;
    }
    IRTimeVar* timevar = c_.back().timing_stack.back();
    int& last_stage = c_.back().timing_last_stage.back();

    // We place (i) a barrier anchored to the *last* stage statement's offset,
    // to late-constrain all statements before this stage start; and (ii) a
    // barrier anchored to *this* stage statement's offset, to early-constrain
    // all statements in this stage.

    unique_ptr<IRStmt> late_timing_barrier(new IRStmt());
    late_timing_barrier->valnum = ctx_->Valnum();
    late_timing_barrier->type = IRStmtTimingBarrier;
    late_timing_barrier->timevar = timevar;
    timevar->uses.push_back(late_timing_barrier.get());
    late_timing_barrier->time_offset = last_stage;

    ctx_->AddIRStmt(ctx_->CurBB(), move(late_timing_barrier));

    unique_ptr<IRStmt> early_timing_barrier(new IRStmt());
    early_timing_barrier->valnum = ctx_->Valnum();
    early_timing_barrier->type = IRStmtTimingBarrier;
    early_timing_barrier->timevar = timevar;
    timevar->uses.push_back(early_timing_barrier.get());
    early_timing_barrier->time_offset = node->offset;
    last_stage = node->offset;

    ctx_->AddIRStmt(ctx_->CurBB(), move(early_timing_barrier));

    return VISIT_CONTINUE;
}

static IRStmtOp ExprTypeToOpType(ASTExpr::Op op) {
    switch (op) {
#define T(ast, ir) \
        case ASTExpr:: ast : return IRStmtOp ## ir
        T(ADD, Add);
        T(SUB, Sub);
        T(MUL, Mul);
        T(DIV, Div);
        T(REM, Rem);
        T(AND, And);
        T(OR, Or);
        T(NOT, Not);
        T(XOR, Xor);
        T(LSH, Lsh);
        T(RSH, Rsh);
        T(SEL, Select);
        T(BITSLICE, Bitslice);
        T(CONCAT,Concat);
        T(EQ, CmpEQ);
        T(NE, CmpNE);
        T(LE, CmpLE);
        T(LT, CmpLT);
        T(GE, CmpGE);
        T(GT, CmpGT);
#undef T
        default:
            return IRStmtOpNone;
    }
}

const ASTExpr* CodeGenPass::FindEntityDef(
        const ASTExpr* node,
        ASTExpr::Op def_type,
        const ASTExpr* orig) {
    if (node->op == def_type) {
        return node;
    } else if (node->op == ASTExpr::VAR) {
        const ASTExpr* binding = ctx_->Bindings()[node->def];
        return FindEntityDef(binding, def_type, orig);
    } else {
        Error(orig,
                "Port/array/reg value expected but cannot trace back to "
                "def statically.");
        return nullptr;
    }
}

CodeGenPass::Result
CodeGenPass::ModifyASTExprPost(ASTRef<ASTExpr>& node) {
    IRStmtOp op = ExprTypeToOpType(node->op);
    // Is this a 'normal' case where we have a 1-to-1 mapping with an IR stmt?
    if (op != IRStmtOpNone) {
        unique_ptr<IRStmt> stmt(new IRStmt());
        stmt->valnum = ctx_->Valnum();
        stmt->type = IRStmtExpr;
        stmt->op = op;
        stmt->width = node->inferred_type.width;
        for (auto& op : node->ops) {
            IRStmt* op_stmt = ctx_->GetIRStmt(op.get());
            stmt->args.push_back(op_stmt);
            stmt->arg_nums.push_back(op_stmt->valnum);
        }
        ctx_->AddIRStmt(ctx_->CurBB(), move(stmt), node.get());
    } else {
        // Handle a few other special cases.
        switch (node->op) {
            case ASTExpr::CONST: {
                unique_ptr<IRStmt> stmt(new IRStmt());
                stmt->type = IRStmtExpr;
                stmt->op = IRStmtOpConst;
                stmt->valnum = ctx_->Valnum();
                stmt->constant = node->constant;
                stmt->has_constant = true;
                stmt->width = node->inferred_type.width;
                ctx_->AddIRStmt(ctx_->CurBB(), move(stmt), node.get());
                break;
            }
            case ASTExpr::VAR: {
                // Simply pass through the current binding.
                const ASTExpr* expr = ctx_->Bindings()[node->def];
                IRStmt* ir = ctx_->GetIRStmt(expr);
                if (ir) {
                    ctx_->AddIRStmt(ir, node.get());
                }
                break;
            }
            case ASTExpr::PORTDEF: {
                // If the portdef has a user-specified name, it's exported.
                // Else we come up with a name at this point.
                if (!node->ident->name.empty()) {
                    if (node->inferred_type.is_chan) {
                        Error(node.get(),
                                "Cannot use a defined name on a chan: "
                                "chans must be anonymous.");
                        return VISIT_END;
                    }
                    unique_ptr<IRStmt> export_stmt(new IRStmt());
                    export_stmt->valnum = ctx_->Valnum();
                    export_stmt->type = IRStmtPortExport;
                    export_stmt->port_name = node->ident->name;
                    export_stmt->width = node->inferred_type.width;
                    ctx_->AddIRStmt(ctx_->CurBB(), move(export_stmt));
                } else {
                    // Anonymous port: give it a name, but don't mark it as
                    // exported.
                    node->ident->name = ctx_->GenSym();
                }
                break;
            }
            case ASTExpr::PORTREAD: {
                // Get the eventual port def. We trace through VAR->lets but
                // don't support any other static analysis here.
                const ASTExpr* portdef = FindEntityDef(
                        node->ops[0].get(), ASTExpr::PORTDEF, node.get());
                if (!portdef) {
                    return VISIT_END;
                }
                unique_ptr<IRStmt> read_stmt(new IRStmt());
                read_stmt->valnum = ctx_->Valnum();
                if (portdef->inferred_type.is_port) {
                    read_stmt->type = IRStmtPortRead;
                } else if (portdef->inferred_type.is_chan) {
                    read_stmt->type = IRStmtChanRead;
                } else {
                    // Typecheck should catch this before we get this far, but
                    // just to be safe...
                    Error(node.get(),
                            "Read from something not a port or chan");
                }
                read_stmt->port_name = portdef->ident->name;
                read_stmt->width = portdef->inferred_type.width;
                ctx_->AddIRStmt(ctx_->CurBB(), move(read_stmt), node.get());
                break;
            }
            case ASTExpr::ARRAY_INIT: {
                // Generate a name for this array and produce an IR statement
                // to define its size.
                node->ident.reset(new ASTIdent());
                node->ident->name = ctx_->GenSym("array");

                unique_ptr<IRStmt> array_def(new IRStmt());
                array_def->valnum = ctx_->Valnum();
                array_def->type = IRStmtArraySize;
                array_def->port_name = node->ident->name;
                array_def->constant = node->inferred_type.array_size;
                ctx_->AddIRStmt(ctx_->CurBB(), move(array_def));
                break;
            }
            case ASTExpr::ARRAY_REF: {
                const ASTExpr* arraydef = FindEntityDef(
                        node->ops[0].get(), ASTExpr::ARRAY_INIT, node.get());
                if (!arraydef) {
                    return VISIT_END;
                }

                unique_ptr<IRStmt> array_read(new IRStmt());
                array_read->valnum = ctx_->Valnum();
                array_read->type = IRStmtArrayRead;
                array_read->width = node->inferred_type.width;

                IRStmt* index_arg = ctx_->GetIRStmt(node->ops[1].get());
                array_read->args.push_back(index_arg);
                array_read->arg_nums.push_back(index_arg->valnum);
                array_read->port_name = arraydef->ident->name;

                ctx_->AddIRStmt(ctx_->CurBB(), move(array_read), node.get());
                break;
            }
            case ASTExpr::REG_INIT: {
                // Generate a name for this reg.
                node->ident.reset(new ASTIdent());
                node->ident->name = ctx_->GenSym("reg");
                break;
            }
            case ASTExpr::REG_REF: {
                const ASTExpr* regdef = FindEntityDef(
                        node->ops[0].get(), ASTExpr::REG_INIT, node.get());
                if (!regdef) {
                    return VISIT_END;
                }

                unique_ptr<IRStmt> reg_read(new IRStmt());
                reg_read->valnum = ctx_->Valnum();
                reg_read->type = IRStmtRegRead;
                reg_read->width = node->inferred_type.width;
                reg_read->port_name = regdef->ident->name;

                ctx_->AddIRStmt(ctx_->CurBB(), move(reg_read), node.get());
                break;
            }
            case ASTExpr::BYPASSDEF:
                // Generate a name for this bypass network.
                node->ident.reset(new ASTIdent());
                node->ident->name = ctx_->GenSym("bypass");
                break;

            case ASTExpr::BYPASSPRESENT:
            case ASTExpr::BYPASSREADY:
            case ASTExpr::BYPASSREAD: {
                const ASTExpr* bypassdef = FindEntityDef(
                        node->ops[0].get(), ASTExpr::BYPASSDEF, node.get());
                if (!bypassdef) {
                    return VISIT_END;
                }

                unique_ptr<IRStmt> bypassop(new IRStmt());
                switch (node->op) {
                    case ASTExpr::BYPASSPRESENT:
                        bypassop->type = IRStmtBypassPresent;
                        break;
                    case ASTExpr::BYPASSREADY:
                        bypassop->type = IRStmtBypassReady;
                        break;
                    case ASTExpr::BYPASSREAD:
                        bypassop->type = IRStmtBypassRead;
                        break;
                    default:
                        assert(false);
                        break;
                }
                bypassop->valnum = ctx_->Valnum();
                bypassop->width = node->inferred_type.width;
                bypassop->port_name = bypassdef->ident->name;

                IRStmt* index_arg = ctx_->GetIRStmt(node->ops[1].get());
                bypassop->args.push_back(index_arg);
                bypassop->arg_nums.push_back(index_arg->valnum);

                ctx_->AddIRStmt(ctx_->CurBB(), move(bypassop), node.get());
                break;
            }

            case ASTExpr::STMTBLOCK: {
                // the block will have already been codegen'd during visit,
                // since we're in a post-hook, so we merely have to find the
                // right expression statement and use its value as the result.
                const ASTStmtExpr* expr_value = nullptr;
                if (node->stmt->stmts.size() < 1 ||
                    !node->stmt->stmts.back()->expr) {
                    Error(node.get(),
                            "Statement-block expr where last stmt is "
                            "not an expression statement.");
                    return VISIT_END;
                }
                expr_value = node->stmt->stmts.back()->expr.get();

                // Associate the final expression statement's value with this
                // whole expression's value.
                ctx_->AddIRStmt(
                        ctx_->GetIRStmt(expr_value->expr.get()),
                        node.get());
                break;
            }
            case ASTExpr::CAST:
                // Nothing -- just associate the IR value of the casted-from
                // expression to the casted-to expression as well. Widths must
                // always match during casting so we never need any computation
                // to implement a cast.
                ctx_->AddIRStmt(
                        ctx_->GetIRStmt(node->ops[0].get()),
                        node.get());
                break;
            default:
                Error(node.get(),
                        strprintf("Unsupported node type: %d", node->op));
                assert(false);
                return VISIT_END;
        }
    }
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtExprPost(ASTRef<ASTStmtExpr>& node) {
    // No further codegen necessary -- we will have already gen'd the IR to
    // compute the underlying expression.
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtNestedFuncPre(ASTRef<ASTStmtNestedFunc>& node) {
    // Push a new function context and save the code-generation point (current
    // BB) of the current function to restore later.
    c_.push_back({});
    c_.back().last_curbb = ctx_->CurBB();

    // Start a new function with a new entry BB.
    IRBB* entry = ctx_->AddBB("anon_func");
    entry->is_entry = true;
    ctx_->ir()->entries.push_back(entry);
    ctx_->SetCurBB(entry);

    // manually codegen the body.
    ASTVisitor visitor;
    if (!visitor.ModifyASTStmtBlock(node->body, this)) {
        return VISIT_END;
    }

    // End with a 'done'.
    unique_ptr<IRStmt> done_stmt(new IRStmt());
    done_stmt->valnum = ctx_->Valnum();
    done_stmt->type = IRStmtDone;
    ctx_->AddIRStmt(ctx_->CurBB(), move(done_stmt));

    // restore the old context.
    ctx_->SetCurBB(c_.back().last_curbb);
    c_.pop_back();

    // We already codegen'd the body -- don't recurse into it on this visit pass.
    return VISIT_TERMINAL;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtOnKillYoungerPre(ASTRef<ASTStmtOnKillYounger>& node) {
    // Clone the code block to the onkillyounger block list, but don't codegen
    // yet.
    auto clone = CloneAST(node.get());
    c_.back().onkillyoungers.push_back(move(clone));
    return VISIT_TERMINAL;  // Don't recurse.
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtBypassStartPost(ASTRef<ASTStmtBypassStart>& node) {
    unique_ptr<IRStmt> bypass_stmt(new IRStmt());
    bypass_stmt->valnum = ctx_->Valnum();
    const ASTExpr* bypassdef = FindEntityDef(
            node->bypass.get(), ASTExpr::BYPASSDEF, node->bypass.get());
    if (!bypassdef) {
        return VISIT_END;
    }
    bypass_stmt->type = IRStmtBypassStart;
    bypass_stmt->port_name = bypassdef->ident->name;
    IRStmt* index_arg = ctx_->GetIRStmt(node->index.get());
    bypass_stmt->args.push_back(index_arg);
    bypass_stmt->arg_nums.push_back(index_arg->valnum);
    ctx_->AddIRStmt(ctx_->CurBB(), move(bypass_stmt));
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtBypassEndPost(ASTRef<ASTStmtBypassEnd>& node) {
    unique_ptr<IRStmt> bypass_stmt(new IRStmt());
    bypass_stmt->valnum = ctx_->Valnum();
    const ASTExpr* bypassdef = FindEntityDef(
            node->bypass.get(), ASTExpr::BYPASSDEF, node->bypass.get());
    if (!bypassdef) {
        return VISIT_END;
    }
    bypass_stmt->type = IRStmtBypassEnd;
    bypass_stmt->port_name = bypassdef->ident->name;
    ctx_->AddIRStmt(ctx_->CurBB(), move(bypass_stmt));
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtBypassWritePost(ASTRef<ASTStmtBypassWrite>& node) {
    unique_ptr<IRStmt> bypass_stmt(new IRStmt());
    bypass_stmt->valnum = ctx_->Valnum();
    const ASTExpr* bypassdef = FindEntityDef(
            node->bypass.get(), ASTExpr::BYPASSDEF, node->bypass.get());
    if (!bypassdef) {
        return VISIT_END;
    }
    bypass_stmt->type = IRStmtBypassWrite;
    bypass_stmt->port_name = bypassdef->ident->name;
    IRStmt* value_arg = ctx_->GetIRStmt(node->value.get());
    bypass_stmt->args.push_back(value_arg);
    bypass_stmt->arg_nums.push_back(value_arg->valnum);
    bypass_stmt->width = value_arg->width;
    ctx_->AddIRStmt(ctx_->CurBB(), move(bypass_stmt));
    return VISIT_CONTINUE;
}

// If-statement support.

CodeGenPass::Result
CodeGenPass::ModifyASTStmtIfPre(ASTRef<ASTStmtIf>& node) {
    ASTVisitor visitor;

    // Create two BBs for the if- and else-bodies respectively.
    IRBB* if_body = ctx_->AddBB("if_body");
    IRBB* else_body = ctx_->AddBB("else_body");

    // Generate the conditional explicitly -- since we're a pre-hook (so that
    // we can control if-/else-body generation), we don't get this for free as
    // the post-hooks do.
    if (!visitor.ModifyASTExpr(node->condition, this)) {
        return VISIT_END;
    }
    IRStmt* conditional = ctx_->GetIRStmt(node->condition.get());

    // Terminate CurBB() with the conditional branch to the if- or else-path.
    unique_ptr<IRStmt> cond_br(new IRStmt());
    cond_br->valnum = ctx_->Valnum();
    cond_br->type = IRStmtIf;
    cond_br->args.push_back(conditional);
    cond_br->arg_nums.push_back(conditional->valnum);
    cond_br->targets.push_back(if_body);
    cond_br->target_names.push_back(if_body->label);
    cond_br->targets.push_back(else_body);
    cond_br->target_names.push_back(else_body->label);
    ctx_->AddIRStmt(ctx_->CurBB(), move(cond_br));

    // Generate each of the if- and else-bodies, pushing a control-flow-path
    // binding context and saving the overwritten bindings so that we can merge
    // with phis at the join point.
    
    int level = ctx_->Bindings().Push();
    ctx_->SetCurBB(if_body);
    if (!visitor.ModifyASTStmt(node->if_body, this)) {
        return VISIT_END;
    }
    auto if_bindings = ctx_->Bindings().Overlay(level);
    ctx_->Bindings().PopTo(level);
    // current BB at end of if-body may be different (if body was not
    // straightline code) -- save to build the merge below.
    IRBB* if_end = ctx_->CurBB();

    level = ctx_->Bindings().Push();
    ctx_->SetCurBB(else_body);
    if (node->else_body) {
        if (!visitor.ModifyASTStmt(node->else_body, this)) {
            return VISIT_END;
        }
    }
    auto else_bindings = ctx_->Bindings().Overlay(level);
    ctx_->Bindings().PopTo(level);
    IRBB* else_end = ctx_->CurBB();

    // Generate the merge point. We (i) create the new BB and set it as the
    // current BB (so that at exit, we leave the current BB set to our single
    // exit point), (ii) generate the jumps from each side of the diamond to
    // the merge point, and (iii) insert phis for all overwritten bindings.
    IRBB* merge_bb = ctx_->AddBB("if_else_merge");
    ctx_->SetCurBB(merge_bb);

    unique_ptr<IRStmt> if_end_jmp(new IRStmt());
    if_end_jmp->valnum = ctx_->Valnum();
    if_end_jmp->type = IRStmtJmp;
    if_end_jmp->targets.push_back(merge_bb);
    if_end_jmp->target_names.push_back(merge_bb->label);
    ctx_->AddIRStmt(if_end, move(if_end_jmp));

    unique_ptr<IRStmt> else_end_jmp(new IRStmt());
    else_end_jmp->valnum = ctx_->Valnum();
    else_end_jmp->type = IRStmtJmp;
    else_end_jmp->targets.push_back(merge_bb);
    else_end_jmp->target_names.push_back(merge_bb->label);
    ctx_->AddIRStmt(else_end, move(else_end_jmp));

    auto phi_map = ctx_->Bindings().JoinOverlays(
            vector<map<ASTStmtLet*, const ASTExpr*>> {
                if_bindings, else_bindings });

    for (auto& p : phi_map) {
        ASTStmtLet* let = p.first;
        vector<const ASTExpr*>& sub_bindings = p.second;
        IRStmt* if_val = ctx_->GetIRStmt(sub_bindings[0]);
        IRStmt* else_val = ctx_->GetIRStmt(sub_bindings[1]);

        if (!if_val || !else_val) {
            Error(node.get(),
                    "If/else reassigns value without underlying IR "
                    "representation. This usually occurs when attempting "
                    "to reassign port variables.");
            return VISIT_END;
        }

        unique_ptr<IRStmt> phi_node(new IRStmt());
        phi_node->type = IRStmtPhi;
        phi_node->valnum = ctx_->Valnum();
        phi_node->width = if_val->width;
        phi_node->args.push_back(if_val);
        phi_node->arg_nums.push_back(if_val->valnum);
        phi_node->args.push_back(else_val);
        phi_node->arg_nums.push_back(else_val->valnum);
        phi_node->targets.push_back(if_end);
        phi_node->target_names.push_back(if_end->label);
        phi_node->targets.push_back(else_end);
        phi_node->target_names.push_back(else_end->label);

        IRStmt* new_node = ctx_->AddIRStmt(merge_bb, move(phi_node));

        // Create a new dummy ASTExpr to refer to the phi node.
        unique_ptr<ASTExpr> phi_expr(new ASTExpr());
        phi_expr->op = ASTExpr::NOP;
        phi_expr->inferred_type = sub_bindings[0]->inferred_type;
        ctx_->AddIRStmt(new_node, phi_expr.get());
        ctx_->Bindings().Set(let, phi_expr.get());
        ctx_->ast()->ir_exprs.push_back(move(phi_expr));
    }

    // Don't recurse.
    return VISIT_TERMINAL;
}

// While-loop support: while, break, continue

// The key idea of the transform is: each break or continue 'forks' the binding scope
// (pushes a new binding level), and creates an edge to either the top-of-loop
// (header block) or the exit block. At the end of the 'while' scope, we pop
// all the binding scopes, write out a jump to the top-of-loop (header block),
// and write out phis in the header and exit blocks.
//
// Nested whiles with labeled breaks/continues necessitate some slightly more
// complex bookkeeping: in the CodeGenContext itself, we keep a map from open
// 'while' scope to its data. Its data includes the current set of "up-exits"
// (continues) and "down-exits" (breaks). These lists may be amended while
// within nested whiles, for example, so it's important that all be available
// for update up the lexical stack. Each exit edge carries a copy of the
// binding map from the top of the associated loop to that point. Such a map
// may be constructed by merging maps down the lexical scope stack: each
// 'while'-loop record should also record a copy of the binding map at its own
// start point relative to the next enclosing loop.
//
// The code layout is:
//
// .---------------------------------------------------------.
// | header                                                  | <-.
// | (phi joins: all continue edges and fallthrough in-edge) |   |
// | (loop condition check with conditional out to footer)   |   |
// `---------------------------------------------------------'   |
//     |                                   ___(continue edges)___/
//     v                                  /                      |
//  (loop body region: arbitrarily complex)___(fallthrough edge)_/
//     |
//   (break edges)
//     |
// .-------------------------------------.
// | footer (phi joins: all break edges) |
// `-------------------------------------'
//
// N.B. that this structure is our generic low-level loop primitive and the
// *only* way to build a backedge. Other loop types may be desugared to this.
// It's also one of only two ways to build a forward edge the skips over code
// (the other being if-else with one side of the diamond empty); this
// convenient structure is used to inline functions by desugaring 'return' to a
// break out of a loop body.

bool CodeGenPass::AddWhileLoopPhiNodeInputs(
        const ASTBase* node,
        CodeGenContext* ctx,
        // Expect either |binding_phis| to be non-null, in which case phis
        // exist and we're adding to their input sets (e.g. for header), or
        // |binding_phi_bb| to be non-null, in which case we're creating new
        // phis (e.g. for footer).
        map<ASTStmtLet*, IRStmt*>* binding_phis,
        IRBB* binding_phi_bb,
        map<IRBB*, SubBindingMap>& in_edges) {
    vector<IRBB*> in_bbs;
    vector<SubBindingMap> in_maps;
    for (auto& p : in_edges) {
        in_bbs.push_back(p.first);
        in_maps.push_back(p.second);
    }

    map<ASTStmtLet*, vector<const ASTExpr*>> join =
        ctx->Bindings().JoinOverlays(in_maps);

    for (auto& p : join) {
        ASTStmtLet* let = p.first;
        auto& exprs = p.second;
        IRStmt* phi_node = nullptr;
        if (binding_phis) {
            phi_node = (*binding_phis)[let];
        } else {
            unique_ptr<IRStmt> new_phi_node(new IRStmt());
            phi_node = new_phi_node.get();
            new_phi_node->valnum = ctx->Valnum();
            new_phi_node->type = IRStmtPhi;
            unique_ptr<ASTExpr> phi_node_expr(new ASTExpr());
            phi_node_expr->inferred_type = let->inferred_type;
            phi_node_expr->op = ASTExpr::NOP;
            ctx->Bindings().Set(let, phi_node_expr.get());
            ctx->AddIRStmt(binding_phi_bb, move(new_phi_node), phi_node_expr.get());
            ctx->ast()->ir_exprs.push_back(move(phi_node_expr));
        }
        if (!phi_node) {
            Error(node,
                    "Attempt to reassign a value without an IR "
                    "representation inside a while loop. This usually "
                    "occurs when attempting to reassign port variables.");
            return false;
        }
        for (unsigned i = 0; i < exprs.size(); i++) {
            IRStmt* in_val = ctx->GetIRStmt(exprs[i]);
            IRBB* in_bb = in_bbs[i];
            phi_node->args.push_back(in_val);
            phi_node->arg_nums.push_back(in_val->valnum);
            phi_node->targets.push_back(in_bb);
            phi_node->target_names.push_back(in_bb->label);
            // Propagate this as we see values so that new phi nodes are
            // properly set up.
            phi_node->width = in_val->width;
        }
    }
    return true;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtWhilePre(ASTRef<ASTStmtWhile>& node) {
    ASTVisitor visitor;

    c_.back().loop_frames.push_back({});
    LoopFrame* frame = &c_.back().loop_frames.back();
    frame->while_block = node.get();
    frame->overlay_depth = ctx_->Bindings().Push();

    // Create header and footer.
    string bb_name_prefix = "while_";
    if (node->label) {
        bb_name_prefix = node->label->name + "_";
    }
    frame->header = ctx_->AddBB((bb_name_prefix + "header").c_str());
    frame->footer = ctx_->AddBB((bb_name_prefix + "footer").c_str());

    // Add a jump from current block to header.
    frame->in_bb = ctx_->CurBB();
    unique_ptr<IRStmt> in_jmp(new IRStmt());
    in_jmp->valnum = ctx_->Valnum();
    in_jmp->type = IRStmtJmp;
    in_jmp->targets.push_back(frame->header);
    in_jmp->target_names.push_back(frame->header->label);
    ctx_->AddIRStmt(ctx_->CurBB(), move(in_jmp));

    ctx_->SetCurBB(frame->header);

    // Generate phis for all bindings in the current scope. Note that we do
    // this in order to have a single-pass algorithm without fixups -- if we
    // wanted to have phis only for bindings that were overwritten in the loop
    // body, then we would either need a pre-pass over the loop to find
    // assignment statements, or a post-pass to insert phis for overwritten
    // bindings and fix up all references. This approach we take here will
    // generate redundant select operations in the output Verilog (e.g.,
    // condition ? x : x) but these will be optimized away in synthesis, and we
    // could also do a simple optimization pass to remove these if we really
    // cared about clean output.
    //
    // We'll add inputs to the phis later, after codegen'ing the body, when we
    // know about all 'continue' edges up to the restart point.
    set<ASTStmtLet*> bindings = ctx_->Bindings().Keys();
    map<ASTStmtLet*, IRStmt*> binding_phis;
    for (auto& let : bindings) {
        if (!ctx_->Bindings().Has(let)) {
            continue;
        }
        const ASTExpr* binding = ctx_->Bindings()[let];
        IRStmt* binding_ir = ctx_->GetIRStmt(binding);
        if (!binding_ir) {
            // Skip vars without direct IR representation, i.e., ports --
            // trying to rebind these within a loop body is an error anyway.
            continue;
        }

        unique_ptr<IRStmt> phi_node(new IRStmt());
        binding_phis[let] = phi_node.get();
        phi_node->type = IRStmtPhi;
        phi_node->valnum = ctx_->Valnum();
        phi_node->width = binding_ir->width;
        phi_node->targets.push_back(frame->in_bb);
        phi_node->target_names.push_back(frame->in_bb->label);
        phi_node->args.push_back(binding_ir);
        phi_node->arg_nums.push_back(binding_ir->valnum);

        // Create a new ASTExpr to refer to phi.
        ASTRef<ASTExpr> ast_expr(new ASTExpr());
        ast_expr->op = ASTExpr::NOP;
        ctx_->AddIRStmt(frame->header, move(phi_node), ast_expr.get());
        ctx_->Bindings().Set(let, ast_expr.get());
        ctx_->ast()->ir_exprs.push_back(move(ast_expr));
    }

    // Generate the loop condition in the current BB (which is the header
    // block).
    if (!visitor.ModifyASTExpr(node->condition, this)) {
        return VISIT_END;
    }
    IRStmt* loop_cond_br_arg = ctx_->GetIRStmt(node->condition.get());

    // Re-establish |frame| -- recursive visit may have modified loop-frame
    // stack and re-allocated the stack storage.
    frame = &c_.back().loop_frames.back();

    // Create the first actual body BB.
    IRBB* body_bb = ctx_->AddBB((bb_name_prefix + "body").c_str());

    // Generate the loop conditional jmp.
    unique_ptr<IRStmt> loop_cond_br(new IRStmt());
    loop_cond_br->valnum = ctx_->Valnum();
    loop_cond_br->type = IRStmtIf;
    loop_cond_br->args.push_back(loop_cond_br_arg);
    loop_cond_br->arg_nums.push_back(loop_cond_br_arg->valnum);
    loop_cond_br->targets.push_back(body_bb);
    loop_cond_br->target_names.push_back(body_bb->label);
    loop_cond_br->targets.push_back(frame->footer);
    loop_cond_br->target_names.push_back(frame->footer->label);
    ctx_->AddIRStmt(frame->header, move(loop_cond_br));

    // Add the implicit 'break' edge from the header to the footer due to the
    // exit condition.
    frame->break_edges.insert(make_pair(
                frame->header,
                ctx_->Bindings().Overlay(frame->overlay_depth)));

    // Generate loop body starting at the body BB.
    ctx_->SetCurBB(body_bb);
    if (!visitor.ModifyASTStmt(node->body, this)) {
        return VISIT_END;
    }

    // Re-establish |frame| -- recursive visit may have modified loop-frame
    // stack and re-allocated the stack storage.
    frame = &c_.back().loop_frames.back();

    // Add the implicit 'continue' jmp at the end of the body.
    IRBB* body_end_bb = ctx_->CurBB();
    unique_ptr<IRStmt> final_continue_jmp(new IRStmt());
    final_continue_jmp->valnum = ctx_->Valnum();
    final_continue_jmp->type = IRStmtJmp;
    final_continue_jmp->targets.push_back(frame->header);
    final_continue_jmp->target_names.push_back(frame->header->label);
    ctx_->AddIRStmt(body_end_bb, move(final_continue_jmp));

    // Add the implicit 'continue' edge with bindings so that the phis will be
    // updated.
    frame->continue_edges.insert(make_pair(
                body_end_bb,
                ctx_->Bindings().Overlay(frame->overlay_depth)));

    // Restore the binding stack to its earlier level.
    ctx_->Bindings().PopTo(frame->overlay_depth);

    // Add in-edges to each phi in the loop header for each continue edge (edge
    // into the header), including the implicit 'end-of-body continue' we added
    // above, and for each break edge (edge into the footer), including the
    // implicit 'break on loop condition false' edge we added above.

    if (!AddWhileLoopPhiNodeInputs(
                node.get(), ctx_,
                &binding_phis, nullptr,
                frame->continue_edges)) {
        return VISIT_END;
    }
    if (!AddWhileLoopPhiNodeInputs(
                node.get(), ctx_,
                nullptr, frame->footer,
                frame->break_edges)) {
        return VISIT_END;
    }

    // Set the footer as current -- this is our single exit point.
    ctx_->SetCurBB(frame->footer);

    // Pop the loop frame.
    c_.back().loop_frames.pop_back();

    // Indicate to traversal that we should not recurse, since we've already
    // manually done codegen for the body.
    return VISIT_TERMINAL;
}

CodeGenPass::LoopFrame* CodeGenPass::FindLoopFrame(
        const ASTBase* node, const ASTIdent* label) {
    if (label) {
        for (int i = c_.back().loop_frames.size() - 1; i >= 0; --i) {
            LoopFrame* frame = &c_.back().loop_frames[i];
            if (frame->while_block->label &&
                frame->while_block->label->name == label->name) {
                return frame;
            }
        }
        Error(node,
                strprintf("Break/continue with unknown label '%s'",
                    label->name.c_str()));
        return nullptr;
    } else {
        if (c_.back().loop_frames.empty()) {
            Error(node, "Break/continue not in loop");
            return nullptr;
        }
        return &c_.back().loop_frames.back();
    }
}

void CodeGenPass::HandleBreakContinue(LoopFrame* frame,
        map<IRBB*, SubBindingMap>& edge_map, IRBB* target) {
    // Capture the bindings up to this point.
    SubBindingMap bindings = ctx_->Bindings().Overlay(frame->overlay_depth);
    // Create a new binding scope for all bindings created after this 'break'.
    ctx_->Bindings().Push();

    // Add the break/continue edge to the frame so that when the loop is
    // closed, this set of bindings is added to the header's or footer's (for
    // continue or break, respectively) phi nodes.
    edge_map.insert(make_pair(ctx_->CurBB(), bindings));

    // Generate the jump to the loop's header (continue) or footer (break)
    // block.
    unique_ptr<IRStmt> jmp(new IRStmt());
    jmp->valnum = ctx_->Valnum();
    jmp->type = IRStmtJmp;
    jmp->targets.push_back(target);
    jmp->target_names.push_back(target->label);
    ctx_->AddIRStmt(ctx_->CurBB(), move(jmp));
    // Start a new CurBB. This is unreachable but it must be non-null to
    // maintain our invariant.
    ctx_->SetCurBB(ctx_->AddBB("unreachable"));
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtBreakPost(ASTRef<ASTStmtBreak>& node) {
    // Find the loop frame that matches this node's label, if any, or the
    // topmost one if no label was provided.
    LoopFrame* frame = FindLoopFrame(node.get(), node->label.get());
    if (!frame) {
        return VISIT_END;
    }
    HandleBreakContinue(frame, frame->break_edges, frame->footer);
    return VISIT_CONTINUE;
}

CodeGenPass::Result
CodeGenPass::ModifyASTStmtContinuePost(ASTRef<ASTStmtContinue>& node) {
    // Find the loop frame that matches this node's label, if any, or the
    // topmost one if no label was provided.
    LoopFrame* frame = FindLoopFrame(node.get(), node->label.get());
    if (!frame) {
        return VISIT_END;
    }
    HandleBreakContinue(frame, frame->continue_edges, frame->header);
    return VISIT_CONTINUE;
}

// Spawn support.

CodeGenPass::Result
CodeGenPass::ModifyASTStmtSpawnPre(ASTRef<ASTStmtSpawn>& node) {
    ASTVisitor visitor;

    IRBB* cur_bb = ctx_->CurBB();

    // Start a new BB for the spawned path.
    IRBB* spawn_bb = ctx_->AddBB("spawn");

    // Produce the spawn statement itself.
    unique_ptr<IRStmt> spawn_stmt(new IRStmt());
    spawn_stmt->valnum = ctx_->Valnum();
    spawn_stmt->type = IRStmtSpawn;
    spawn_stmt->width = kIRStmtWidthTxnID;
    spawn_stmt->targets.push_back(spawn_bb);
    spawn_stmt->target_names.push_back(spawn_bb->label);
    ctx_->AddIRStmt(ctx_->CurBB(), move(spawn_stmt));

    // Generate the spawn-path's code.
    ctx_->SetCurBB(spawn_bb);
    // Start a new binding context for this path.
    int level = ctx_->Bindings().Push();
    if (!visitor.ModifyASTStmt(node->body, this)) {
        return VISIT_END;
    }
    ctx_->Bindings().PopTo(level);
    // Ensure that the spawn-path ends with a 'kill'.
    unique_ptr<IRStmt> kill_stmt(new IRStmt());
    kill_stmt->valnum = ctx_->Valnum();
    kill_stmt->type = IRStmtKill;
    ctx_->AddIRStmt(ctx_->CurBB(), move(kill_stmt));

    // Re-set the output path (current BB) to the BB to which we added the
    // 'spawn' -- codegen continues on the fallthrough path.
    ctx_->SetCurBB(cur_bb);

    // Don't recurse.
    return VISIT_TERMINAL;
}

CodeGenPass::Result
CodeGenPass::ModifyASTPragmaPost(ASTRef<ASTPragma>& node) {
    if (node->key == "timing_model") {
        ctx_->ir()->timing_model = node->value;
    }
    return VISIT_CONTINUE;
}

static void MarkSuccs(set<IRBB*>& bb_set, IRBB* root) {
    if (bb_set.find(root) != bb_set.end()) {
        return;
    }
    bb_set.insert(root);
    for (auto* bb : root->Succs()) {
        MarkSuccs(bb_set, bb);
    }
}

static void FilterPhiInputs(IRStmt* phi, set<IRBB*>& reachable) {
    vector<IRBB*> new_targets;
    vector<string> new_target_names;
    vector<IRStmt*> new_args;
    vector<int> new_arg_nums;
    for (unsigned i = 0; i < phi->args.size(); i++) {
        if (reachable.find(phi->targets[i]) != reachable.end()) {
            new_targets.push_back(phi->targets[i]);
            new_target_names.push_back(phi->target_names[i]);
            new_args.push_back(phi->args[i]);
            new_arg_nums.push_back(phi->arg_nums[i]);
        }
    }
    phi->targets.swap(new_targets);
    phi->target_names.swap(new_target_names);
    phi->args.swap(new_args);
    phi->arg_nums.swap(new_arg_nums);
}

void CodeGenPass::RemoveUnreachableBBsAndPhis() {
    // Find the set of all reachable BBs by starting with entry points and
    // marking all unmarked successors.
    set<IRBB*> reachable;
    for (auto& bb : ctx_->ir()->bbs) {
        if (bb->is_entry) {
            MarkSuccs(reachable, bb.get());
        }
        for (auto& stmt : bb->stmts) {
            if (stmt->type == IRStmtSpawn) {
                MarkSuccs(reachable, stmt->targets[0]);
            }
        }
    }

    // Remove all phi-node args coming from unreachable BBs.
    for (auto& bb : ctx_->ir()->bbs) {
        for (auto& stmt : bb->stmts) {
            if (stmt->type != IRStmtPhi) {
                continue;
            }
            FilterPhiInputs(stmt.get(), reachable);
        }
    }


    // Remove all unreachable BBs.
    vector<unique_ptr<IRBB>> filtered_bbs;
    for (auto& bb : ctx_->ir()->bbs) {
        if (reachable.find(bb.get()) != reachable.end()) {
            filtered_bbs.push_back(move(bb));
        }
    }
    ctx_->ir()->bbs.swap(filtered_bbs);
    filtered_bbs.clear();
}


}  // namespace frontend
}  // namespace autopiper
