// DiskCexCache.cpp

#include "klee/Solver/DiskCexCache.h"

#include "klee/Expr/ExprPPrinter.h"
#include "klee/Expr/ExprVisitor.h"

#include <sstream>

using namespace klee;
using mapofsets::DiskMapOfSets;

DiskCexCache::DiskCexCache(const std::string &filename,
                           const std::vector<Assignment *> &assignmentTable)
    : disk_(filename), assignmentTable_(assignmentTable) {}

// Very simple canonicalization: print the Expr in a stable textual form.
// You can tighten this later (e.g., normalize commutative ops, sort children, etc.).
std::string DiskCexCache::canonConstraint(ref<Expr> e) const {
  std::string out;
  llvm::raw_string_ostream os(out);
  ExprPPrinter::printSingleExpr(os, e);
  os.flush();
  return out;
}

std::set<std::string>
DiskCexCache::buildDiskKey(const std::set<ref<Expr>> &constraints) const {
  std::set<std::string> key;
  for (auto &c : constraints)
    key.insert(canonConstraint(c));
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
    Assignment *&outAssignment) {
  if (entries.empty())
    return false;

  // Very simple strategy: just pick the first entry.
  const auto &e = entries.front();
  ParsedValue pv = parseValue(e.value);
  switch (pv.kind) {
  case ValueKind::Unsat:
    outAssignment = nullptr;
    return true;
  case ValueKind::AssignmentId:
    if (pv.assignmentId < assignmentTable_.size()) {
      outAssignment = assignmentTable_[pv.assignmentId];
      return true;
    }
    return false;
  case ValueKind::Unknown:
  default:
    return false;
  }
}

bool DiskCexCache::findSuperset(const std::set<ref<Expr>> &constraints,
                                Assignment *&outAssignment) {
  auto diskKey = buildDiskKey(constraints);
  auto supers = disk_.supersets(diskKey);
  return pickEntry(supers, outAssignment);
}

bool DiskCexCache::findSubset(const std::set<ref<Expr>> &constraints,
                              Assignment *&outAssignment) {
  auto diskKey = buildDiskKey(constraints);
  auto subs = disk_.subsets(diskKey);
  return pickEntry(subs, outAssignment);
}
