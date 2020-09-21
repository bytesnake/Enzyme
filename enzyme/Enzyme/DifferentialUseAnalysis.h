#include "GradientUtils.h"

// Determine if a value is needed in the reverse pass. We only use this logic in
// the top level function right now.
bool is_value_needed_in_reverse(
    TypeResults &TR, const GradientUtils *gutils, const Value *inst,
    bool topLevel, std::map<std::pair<const Value *, bool>, bool> seen = {}) {
  auto idx = std::make_pair(inst, topLevel);
  if (seen.find(idx) != seen.end())
    return seen[idx];
  if (auto ainst = dyn_cast<Instruction>(inst)) {
    assert(ainst->getParent()->getParent() == gutils->oldFunc);
  }

  // Inductively claim we aren't needed (and try to find contradiction)
  seen[idx] = false;

  // Consider all users of this value, do any of them need this in the reverse?
  for (auto use : inst->users()) {
    if (use == inst)
      continue;

    const Instruction *user = dyn_cast<Instruction>(use);

    // One may need to this value in the computation of loop
    // bounds/comparisons/etc (which even though not active -- will be used for
    // the reverse pass)
    //   We only need this if we're not doing the combined forward/reverse since
    //   otherwise it will use the local cache (rather than save for a separate
    //   backwards cache)
    if (!topLevel) {
      // Proving that none of the uses (or uses' uses) are used in control flow
      // allows us to safely not do this load

      // TODO save loop bounds for dynamic loop

      // TODO make this more aggressive and dont need to save loop latch
      if (isa<BranchInst>(use) || isa<SwitchInst>(use)) {
        // llvm::errs() << " had to use in reverse since used in branch/switch "
        // << *inst << " use: " << *use << "\n";
        return seen[idx] = true;
      }

      if (is_value_needed_in_reverse(TR, gutils, user, topLevel, seen)) {
        // llvm::errs() << " had to use in reverse since used in " << *inst << "
        // use: " << *use << "\n";
        return seen[idx] = true;
      }
    }
    // llvm::errs() << " considering use : " << *user << " of " <<  *inst <<
    // "\n";

    // The following are types we know we don't need to compute adjoints

    // A pointer is only needed in the reverse pass if its non-store uses are
    // needed in the reverse pass
    //   Moreover, we only need this pointer in the reverse pass if all of its
    //   non-store users are not already cached for the reverse pass
    if (!inst->getType()->isFPOrFPVectorTy() &&
        TR.query(const_cast<Value *>(inst)).Data0()[{}].isPossiblePointer()) {
      // continue;
      bool unknown = true;
      for (auto zu : inst->users()) {
        // Stores to a pointer are not needed for the reverse pass
        if (auto si = dyn_cast<StoreInst>(zu)) {
          if (si->getPointerOperand() == inst) {
            continue;
          }
        }

        if (isa<LoadInst>(zu) || isa<CastInst>(zu) || isa<PHINode>(zu)) {
          if (is_value_needed_in_reverse(TR, gutils, zu, topLevel, seen)) {
            // llvm::errs() << " had to use in reverse since sub use " << *zu <<
            // " of " << *inst << "\n";
            return seen[idx] = true;
          }
          continue;
        }

        if (auto II = dyn_cast<IntrinsicInst>(zu)) {
          if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
              II->getIntrinsicID() == Intrinsic::lifetime_end ||
              II->getIntrinsicID() == Intrinsic::stacksave ||
              II->getIntrinsicID() == Intrinsic::stackrestore) {
            continue;
          }
        }

        if (auto ci = dyn_cast<CallInst>(zu)) {
          // If this instruction isn't constant (and thus we need the argument
          // to propagate to its adjoint)
          //   it may write memory and is topLevel (and thus we need to do the
          //   write in reverse) or we need this value for the reverse pass (we
          //   conservatively assume that if legal it is recomputed and not
          //   stored)
          if (!gutils->isConstantInstruction(ci) ||
              (ci->mayWriteToMemory() && topLevel) ||
              (gutils->legalRecompute(ci, ValueToValueMapTy()) &&
               is_value_needed_in_reverse(TR, gutils, ci, topLevel, seen))) {
            return seen[idx] = true;
          }
          continue;
        }

        /*
        if (auto gep = dyn_cast<GetElementPtrInst>(zu)) {
            for(auto &idx : gep->indices()) {
                if (idx == inst) {
                    return seen[inst] = true;
                }
            }
            if (gep->getPointerOperand() == inst &&
        is_value_needed_in_reverse(gutils, gep, topLevel, seen)) {
                //llvm::errs() << " had to use in reverse since sub gep use " <<
        *zu << " of " << *inst << "\n"; return seen[inst] = true;
            }
            continue;
        }
        */

        // TODO add handling of call and allow interprocedural
        // llvm::errs() << " unknown pointer use " << *zu << " of " << *inst <<
        // "\n";
        unknown = true;
      }
      if (!unknown)
        continue;
      // return seen[inst] = false;
    }

    if (isa<LoadInst>(user) || isa<CastInst>(user) || isa<PHINode>(user)) {
      if (!is_value_needed_in_reverse(TR, gutils, user, topLevel, seen)) {
        continue;
      }
    }

    if (auto II = dyn_cast<IntrinsicInst>(user)) {
      if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
          II->getIntrinsicID() == Intrinsic::lifetime_end ||
          II->getIntrinsicID() == Intrinsic::stacksave ||
          II->getIntrinsicID() == Intrinsic::stackrestore) {
        continue;
      }
    }

    if (auto op = dyn_cast<BinaryOperator>(user)) {
      if (op->getOpcode() == Instruction::FAdd ||
          op->getOpcode() == Instruction::FSub) {
        continue;
      } else if (op->getOpcode() == Instruction::FMul) {
        bool needed = false;
        if (op->getOperand(0) == inst &&
            !gutils->isConstantValue(op->getOperand(1)))
          needed = true;
        if (op->getOperand(1) == inst &&
            !gutils->isConstantValue(op->getOperand(0)))
          needed = true;
        // llvm::errs() << "needed " << *inst << " in mul " << *op << " -
        // needed:" << needed << "\n";
        if (!needed)
          continue;
      } else if (op->getOpcode() == Instruction::FDiv) {
        bool needed = false;
        if (op->getOperand(1) == inst &&
            !gutils->isConstantValue(op->getOperand(1)))
          needed = true;
        if (op->getOperand(1) == inst &&
            !gutils->isConstantValue(op->getOperand(0)))
          needed = true;
        if (op->getOperand(0) == inst &&
            !gutils->isConstantValue(op->getOperand(1)))
          needed = true;
        // llvm::errs() << "needed " << *inst << " in div " << *op << " -
        // needed:" << needed << "\n";
        if (!needed)
          continue;
      } else
        continue;
    }

    // We don't need only the indices of a GEP to compute the adjoint of a GEP
    if (auto gep = dyn_cast<GetElementPtrInst>(user)) {
      bool indexuse = false;
      for (auto &idx : gep->indices()) {
        if (idx == inst) {
          indexuse = true;
        }
      }
      if (!indexuse)
        continue;
    }

    if (auto si = dyn_cast<SelectInst>(use)) {
      // only need the condition if select is active
      if (gutils->isConstantValue(const_cast<SelectInst *>(si)))
        continue;
      //   none of the other operands are needed otherwise
      if (si->getCondition() != inst) {
        continue;
      }
    }

    // We don't need any of the input operands to compute the adjoint of a store
    // instance
    if (isa<StoreInst>(use)) {
      continue;
    }

    if (isa<CmpInst>(use) || isa<BranchInst>(use) || isa<CastInst>(use) ||
        isa<PHINode>(use) || isa<ReturnInst>(use) || isa<FPExtInst>(use) ||
        (isa<InsertElementInst>(use) &&
         cast<InsertElementInst>(use)->getOperand(2) != inst) ||
        (isa<ExtractElementInst>(use) &&
         cast<ExtractElementInst>(use)->getIndexOperand() != inst)
        // isa<LoadInst>(use) || (isa<SelectInst>(use) &&
        // cast<SelectInst>(use)->getCondition() != inst) //TODO remove load?
        //|| isa<SwitchInst>(use) || isa<ExtractElement>(use) ||
        //isa<InsertElementInst>(use) || isa<ShuffleVectorInst>(use) ||
        // isa<ExtractValueInst>(use) || isa<AllocaInst>(use)
        /*|| isa<StoreInst>(use)*/) {
      continue;
    }

    //! Note it is important that return check comes before this as it may not
    //! have a new instruction

    if (auto ci = dyn_cast<CallInst>(use)) {
      // If this instruction isn't constant (and thus we need the argument to
      // propagate to its adjoint)
      //   it may write memory and is topLevel (and thus we need to do the write
      //   in reverse) or we need this value for the reverse pass (we
      //   conservatively assume that if legal it is recomputed and not stored)
      if (!gutils->isConstantInstruction(ci) ||
          (ci->mayWriteToMemory() && topLevel) ||
          (gutils->legalRecompute(ci, ValueToValueMapTy()) &&
           is_value_needed_in_reverse(TR, gutils, ci, topLevel, seen))) {
        return seen[idx] = true;
      }
      continue;
    }

    if (auto inst = dyn_cast<Instruction>(use))
      if (gutils->isConstantInstruction(const_cast<Instruction *>(inst)))
        continue;

    // llvm::errs() << " + must have in reverse from considering use : " <<
    // *user << " of " <<  *inst << "\n";
    return seen[idx] = true;
  }
  return false;
}