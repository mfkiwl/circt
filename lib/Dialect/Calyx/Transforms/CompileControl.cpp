//===- CompileControl.cpp - Compile Control Pass ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Contains the definitions of the Compile Control pass.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/Calyx/CalyxOps.h"
#include "circt/Dialect/Calyx/CalyxPasses.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OperationSupport.h"

using namespace circt;
using namespace calyx;
using namespace mlir;

/// A helper function to create constants in the HW dialect.
static hw::ConstantOp createConstant(OpBuilder &builder, Location loc,
                                     size_t bitWidth, size_t value) {
  return builder.create<hw::ConstantOp>(
      loc, APInt(bitWidth, value, /*unsigned=*/true));
}

/// Given some number of states, returns the necessary bit width
/// TODO(Calyx): Probably a better built-in operation?
static size_t getNecessaryBitWidth(size_t numStates) {
  APInt apNumStates(64, numStates);
  size_t log2 = apNumStates.ceilLogBase2();
  return log2 > 1 ? log2 : 1;
}

/// Creates a component interface for a Register, and returns a Cell instance
/// referring to it.
/// TODO(Calyx): Need to introduce a standard register.
static CellOp buildRegisterLikeComponent(OpBuilder &builder,
                                         ComponentOp &component, size_t width,
                                         StringRef name) {
  auto *context = builder.getContext();
  // Create Component to represent the internal state of a register.
  auto program = component->getParentOfType<ProgramOp>();

  auto regWidthType = builder.getIntegerType(width);

  SmallVector<calyx::ComponentPortInfo, 3> ports{
      {.name = StringAttr::get(context, "in"),
       .type = regWidthType,
       .direction = PortDirection::INPUT},
      {.name = StringAttr::get(context, "write_en"),
       .type = builder.getI1Type(),
       .direction = PortDirection::INPUT},
      {.name = StringAttr::get(context, "out"),
       .type = regWidthType,
       .direction = PortDirection::OUTPUT}};

  builder.setInsertionPointToStart(program.getBody());
  auto newRegister = builder.create<ComponentOp>(
      program->getLoc(), StringAttr::get(context, name), ports);

  // Create the cell operation.
  TypeRange types = {regWidthType, builder.getI1Type(), regWidthType};
  builder.setInsertionPointToStart(component.getBody());
  return builder.create<CellOp>(
      component->getLoc(), types, StringAttr::get(context, "fsm"),
      FlatSymbolRefAttr::get(context, SymbolTable::getSymbolName(newRegister)));
}

// TODO(Calyx): Document.
static void visitSeqOp(SeqOp &seq, ComponentOp &component) {
  auto wires = component.getWiresOp();
  Block *wiresBody = wires.getBody();

  size_t numEnables = seq.getBody()->getOperations().size();
  // This should be the number of enable statements + 1 since this is the
  // maximum value the FSM register will reach.
  size_t fsmBitWidth = getNecessaryBitWidth(numEnables + 1);

  OpBuilder builder(component->getRegion(0));
  auto fsmCell =
      buildRegisterLikeComponent(builder, component, fsmBitWidth, "register");
  auto fsmIn = fsmCell.getResult(0);
  auto fsmWriteEn = fsmCell.getResult(1);
  auto fsmOut = fsmCell.getResult(2);

  builder.setInsertionPointToStart(wiresBody);
  auto oneConstant = createConstant(builder, wires->getLoc(), 1, 1);

  builder.setInsertionPointToEnd(wiresBody);
  auto seqGroup =
      builder.create<GroupOp>(wires->getLoc(), builder.getStringAttr("seq"));
  Block *seqGroupBody = new Block();
  seqGroup->getRegion(0).push_back(seqGroupBody);

  // Guarantees a unique SymbolName for the group.
  SymbolTable symTable(wires);
  symTable.insert(seqGroup);

  size_t fsmIndex = 0;
  SmallVector<Attribute, 8> groupNames;
  seq.walk([&](EnableOp enable) {
    StringRef groupName = enable.groupName();
    groupNames.push_back(SymbolRefAttr::get(builder.getContext(), groupName));
    auto groupOp = cast<GroupOp>(symTable.lookup(groupName));

    builder.setInsertionPoint(groupOp);
    auto fsmCurrentState =
        createConstant(builder, wires->getLoc(), fsmBitWidth, fsmIndex);

    // Currently, we assume that there is no guard, i.e. the source is
    // used in the predicate whenever `GroupDoneOp` is needed.
    // TODO(Calyx): Need to canonicalize the GroupDoneOp's guard and source
    // from: group_done %src : i1
    // =>
    // %c1 = hw.constant 1 : i1
    // group_done %c1, %src ? : i1
    auto doneOpSource = groupOp.getDoneOp().src();

    // Build the Guard for the `go` signal of the current group being walked.
    // The group should begin when:
    // (1) the current step in the fsm is reached, and
    // (2) the done signal of this group is not high.
    auto eqCmp = builder.create<comb::ICmpOp>(
        wires->getLoc(), comb::ICmpPredicate::eq, fsmOut, fsmCurrentState);
    auto notDone =
        builder.create<comb::XorOp>(wires->getLoc(), doneOpSource, oneConstant);
    auto groupGoGuard =
        builder.create<comb::AndOp>(wires->getLoc(), eqCmp, notDone);

    // Guard for the `in` and `write_en` signal of the fsm register. These are
    // driven when the group has completed.
    builder.setInsertionPoint(seqGroup);
    auto groupDoneGuard =
        builder.create<comb::AndOp>(wires->getLoc(), eqCmp, doneOpSource);

    // Directly update the GroupGoOp of the current group being walked.
    auto goOp = groupOp.getGoOp();
    assert(goOp && "The Go Insertion pass should be run before this.");

    goOp->setOperand(0, oneConstant);
    goOp->insertOperands(1, {groupGoGuard});

    // Add guarded assignments to the fsm register `in` and `write_en` ports.
    auto fsmNextState =
        createConstant(builder, wires->getLoc(), fsmBitWidth, fsmIndex + 1);
    builder.setInsertionPointToEnd(seqGroupBody);
    builder.create<AssignOp>(wires->getLoc(), fsmIn, fsmNextState,
                             groupDoneGuard);
    builder.create<AssignOp>(wires->getLoc(), fsmWriteEn, oneConstant,
                             groupDoneGuard);

    ++fsmIndex;
  });

  // Build the final guard for the new Seq group's GroupDoneOp. This is defined
  // by the fsm's final state.
  builder.setInsertionPoint(seqGroup);
  auto fsmFinalState =
      createConstant(builder, wires->getLoc(), fsmBitWidth, fsmIndex);
  auto eqCmp = builder.create<comb::ICmpOp>(
      wires->getLoc(), comb::ICmpPredicate::eq, fsmOut, fsmFinalState);

  // Insert the respective GroupDoneOp.
  builder.setInsertionPointToEnd(seqGroupBody);
  builder.create<GroupDoneOp>(seqGroup->getLoc(), oneConstant, eqCmp);

  // Replace the SeqOp with an EnableOp.
  builder.setInsertionPoint(seq);
  auto newEnableOp =
      builder.create<EnableOp>(seq->getLoc(), seqGroup.sym_name());
  seq->erase();

  // Keep a list of compiled groups associated with the new enable.
  newEnableOp->setAttr("groups",
                       ArrayAttr::get(builder.getContext(), groupNames));
}

namespace {

struct CompileControlPass : public CompileControlBase<CompileControlPass> {
  void runOnOperation() override;
};

} // end anonymous namespace

void CompileControlPass::runOnOperation() {
  ComponentOp component = getOperation();
  component.getControlOp().walk([&](SeqOp seq) { visitSeqOp(seq, component); });

  // A post-condition of this pass is that all undefined GroupGoOps, created in
  // the Go Insertion pass, are now defined.
  component.getWiresOp().walk([&](UndefinedOp op) { op->erase(); });
}

std::unique_ptr<mlir::Pass> circt::calyx::createCompileControlPass() {
  return std::make_unique<CompileControlPass>();
}
