//===----  RepoPruning.cpp - Program repository pruning  ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "pstore/core/database.hpp"
#include "pstore/core/hamt_map.hpp"
#include "pstore/core/index_types.hpp"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/RepoGlobals.h"
#include "llvm/IR/RepoHashCalculator.h"
#include "llvm/IR/RepoTicket.h"
#include "llvm/Pass.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"
#include <iostream>
#include <set>
using namespace llvm;

#define DEBUG_TYPE "prepo-pruning"

STATISTIC(NumAliases, "Number of global aliases removed");
STATISTIC(NumFunctions, "Number of functions removed");
STATISTIC(NumVariables, "Number of variables removed");

namespace {

/// RepoPruning removes the redundant global objects.
class RepoPruning : public ModulePass {
public:
  static char ID;
  RepoPruning() : ModulePass(ID) {
    initializeRepoPruningPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "RepoPruningPass"; }

  bool runOnModule(Module &M) override;

private:
  bool isObjFormatRepo(Module &M) const {
    return Triple(M.getTargetTriple()).isOSBinFormatRepo();
  }
};

} // end anonymous namespace

char RepoPruning::ID = 0;
INITIALIZE_PASS(RepoPruning, "prepo-pruning",
                "Program Repository Global Object Pruning", false, false)

ModulePass *llvm::createRepoPruningPass() { return new RepoPruning(); }

using GlobalObjectMap =
    std::map<const GlobalObject *, const ticketmd::DigestType>;
using GlobalValueMap = ticketmd::GlobalValueMap;

bool RepoPruning::runOnModule(Module &M) {
  if (skipModule(M) || !isObjFormatRepo(M))
    return false;

  bool Changed = false;
  MDBuilder MDB(M.getContext());

  pstore::database &Repository = getRepoDatabase();

  MDNode *MD = nullptr;
  pstore::index::digest_index const *const Digests =
      pstore::index::get_digest_index(Repository, false);
  if (Digests == nullptr) {
    return false;
  }

  // Erase the unchanged global objects.
  auto EraseUnchangedGlobalObect = [&](GlobalObject &GO,
                                       llvm::Statistic &NumGO) -> bool {
    if (GO.isDeclaration() || GO.hasAvailableExternallyLinkage())
      return false;
    auto const Result = ticketmd::get(&GO);
    assert(!Result.second && "The repo_ticket metadata doesn't exist!");

    auto const Key = pstore::index::digest{Result.first.high(), Result.first.low()};
    if (Digests->find(Key) != Digests->end()) {
      Changed = true;
      ++NumGO;
      GO.setComdat(nullptr);
      // Remove all metadata except fragment.
      MD = GO.getMetadata(LLVMContext::MD_repo_ticket);
      dyn_cast<TicketNode>(MD)->setPruned(true);
      GO.clearMetadata();
      GO.setMetadata(LLVMContext::MD_repo_ticket, MD);
      GO.setLinkage(GlobalValue::ExternalLinkage);
      return true;
    }
    return false;
  };

  for (GlobalVariable &GV : M.globals()) {
    if (EraseUnchangedGlobalObect(GV, NumVariables)) {
      // Removes the Global variable initializer.
      Constant *Init = GV.getInitializer();
      if (isSafeToDestroyConstant(Init))
        Init->destroyConstant();
      GV.setInitializer(nullptr);
    }
  }

  for (Function &Func : M) {
    if (EraseUnchangedGlobalObect(Func, NumFunctions)) {
      Func.deleteBody();
      Func.setMetadata(LLVMContext::MD_repo_ticket, MD);
    }
  }

  DEBUG(dbgs() << "size of module: " << M.size() << '\n');
  DEBUG(dbgs() << "size of removed functions: " << NumFunctions << '\n');
  DEBUG(dbgs() << "size of removed variables: " << NumVariables << '\n');
  DEBUG(dbgs() << "size of removed aliases: " << NumAliases << '\n');

  return Changed;
}
