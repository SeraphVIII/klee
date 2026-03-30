// DiskCexCache.cpp

#include "klee/Solver/DiskCexCache.h"
#include "klee/Solver/ConstraintCanonicalizer.h"

#include "klee/Expr/ExprPPrinter.h"

#include <sstream>

using namespace klee;
using mapofsets::DiskMapOfSets;

DiskCexCache::DiskCexCache(const std::string &filename,
                           const std::vector<Assignment *> &assignmentTable)
    : disk_(filename), assignmentTable_(assignmentTable), builder_(createDefaultExprBuilder()) {}

std::set<std::string>
DiskCexCache::buildDiskKey(const std::set<ref<Expr>> &constraints) const {
  // Canonicalize as a set so array positions (A0, A1, ...) are assigned
  // consistently across all constraints, not independently per expression.
  std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
  CanonicalizationResult canon =
      klee::canonicalizeConstraintSet(vec, *builder_, arrayCache_);

  // Serialize each canonicalized constraint to a string key. We use
  // printSingleExpr directly (no trailing newline) so that the key format
  // is unambiguous and matches what a future write path would produce.
  std::set<std::string> key;
  for (const auto &e : canon.constraints) {
    std::string s;
    llvm::raw_string_ostream os(s);
    ExprPPrinter::printSingleExpr(os, e);
    os.flush();
    key.insert(s);
  }
  return key;
}

// Current encoding: value is either
//   "UNSAT"              -> Unsat
//   "SAT:<id>"           -> AssignmentId(id)
// You can change this later to a protobuf value.
DiskCexCache::ParsedValue
DiskCexCache::parseValue(const std::string &val) const {
  ParsedValue pv{ValueKind::Unknown, 0};
  if (val == "UNSAT") {
    pv.kind = ValueKind::Unsat;
    return pv;
  }
  const std::string prefix = "SAT:";
  if (val.compare(0, prefix.size(), prefix) == 0) {
    unsigned id = 0;
    std::istringstream iss(val.substr(prefix.size()));
    if ((iss >> id)) {
      pv.kind = ValueKind::AssignmentId;
      pv.assignmentId = id;
      return pv;
    }
  }
  return pv;
}


bool DiskCexCache::pickEntry(
    const std::vector<DiskMapOfSets::Entry> &entries,
    const std::set<ref<Expr>> &originalConstraints,
    Assignment *&outAssignment) {
  for (const auto &e : entries) {
    ParsedValue pv = parseValue(e.value);
    switch (pv.kind) {
    case ValueKind::Unsat:
      // Any UNSAT subset proves the full set is UNSAT.
      outAssignment = nullptr;
      return true;
    case ValueKind::AssignmentId:
      if (pv.assignmentId < assignmentTable_.size()) {
        Assignment *a = assignmentTable_[pv.assignmentId];
        if (a && a->satisfies(originalConstraints.begin(),
                              originalConstraints.end())) {
          outAssignment = a;
          return true;
        }
      }
      break;
    case ValueKind::Unknown:
    default:
      break;
    }
  }
  return false;
}

bool DiskCexCache::findSuperset(const std::set<ref<Expr>> &constraints,
                                Assignment *&outAssignment) {
  auto diskKey = buildDiskKey(constraints);
  auto supers = disk_.supersets(diskKey);
  return pickEntry(supers, constraints, outAssignment);
}

bool DiskCexCache::findSubset(const std::set<ref<Expr>> &constraints,
                              Assignment *&outAssignment) {
  auto diskKey = buildDiskKey(constraints);
  auto subs = disk_.subsets(diskKey);
  return pickEntry(subs, constraints, outAssignment);
}
