//===-- CexCachingSolver.cpp ----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Solver/Solver.h"

#include "klee/ADT/MapOfSets.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Expr/ExprVisitor.h"
#include "klee/Support/OptionCategories.h"
#include "klee/Statistics/TimerStatIncrementer.h"
#include "klee/Solver/SolverImpl.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Solver/DiskCexCache.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Solver/SolverStats.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Config/config.h"

#include "llvm/Support/CommandLine.h"

#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace klee;
using namespace llvm;

/// Returns the active core solver backend as a short lowercase string
/// suitable for storing in the cache file metadata.
static std::string currentSolverBackendName() {
  switch (CoreSolverToUse) {
  case STP_SOLVER:     return "stp";
  case Z3_SOLVER:      return "z3";
  case METASMT_SOLVER: return "metasmt";
  default:             return "unknown";
  }
}

static CacheMetadata buildCacheMetadata() {
  CacheMetadata m;
  m.solverBackend = currentSolverBackendName();
#ifdef PACKAGE_STRING
  m.kleeVersion = PACKAGE_STRING;
#endif
  return m;
}

namespace {

cl::opt<std::string> DiskCexCacheFile(
    "disk-cex-cache",
    cl::desc("Path to read-only disk cex cache file (empty=disabled)"),
    cl::init(""),
    cl::cat(SolvingCat));

cl::opt<std::string> WriteDiskCexCacheFile(
    "write-disk-cex-cache",
    cl::desc("Append new CEX cache entries to this log file during the run"),
    cl::init(""),
    cl::cat(SolvingCat));

cl::opt<std::string> PersistentCexCacheFile(
    "persistent-cex-cache",
    cl::desc("Read cache from this file on startup, append new entries to "
             "<path>.log during the run (use the offline merge tool to "
             "incorporate the log back into the cache file)"),
    cl::init(""),
    cl::cat(SolvingCat));

cl::opt<unsigned> DiskCacheMinHitRatePct(
    "disk-cex-cache-min-hit-rate",
    cl::desc("Suspend the disk CEX cache when the hit rate over the last "
             "--disk-cex-cache-hit-rate-window lookups falls below this "
             "percentage; re-enable after the same number of misses "
             "(0 = never suspend, default = 10)"),
    cl::init(10),
    cl::cat(SolvingCat));

cl::opt<unsigned> DiskCacheHitRateWindow(
    "disk-cex-cache-hit-rate-window",
    cl::desc("Sliding-window size (number of recent lookups) used by "
             "--disk-cex-cache-min-hit-rate (default = 100)."),
    cl::init(100),
    cl::cat(SolvingCat));

cl::opt<unsigned> DiskCexCacheLruSize(
    "disk-cex-cache-lru-size",
    cl::desc("Bound on decoded chunks held in memory while reading the "
             "disk CEX cache (default = 100)"),
    cl::init(100),
    cl::cat(SolvingCat));

cl::opt<bool> DebugCexCacheCheckBinding(
    "debug-cex-cache-check-binding", cl::init(false),
    cl::desc("Debug the correctness of the counterexample "
             "cache assignments (default=false)"),
    cl::cat(SolvingCat));

cl::opt<bool>
    CexCacheTryAll("cex-cache-try-all", cl::init(false),
                   cl::desc("Try substituting all counterexamples before "
                            "asking the SMT solver (default=false)"),
                   cl::cat(SolvingCat));

cl::opt<bool>
    CexCacheSuperSet("cex-cache-superset", cl::init(false),
                     cl::desc("Try substituting SAT superset counterexample "
                              "before asking the SMT solver (default=false)"),
                     cl::cat(SolvingCat));

cl::opt<bool> CanonicalizeCexKey(
    "canonicalize-cex-key", cl::init(false),
    cl::desc("Canonicalize expression trees (commutative/associative operand "
             "ordering, no array renaming) before the in-memory cex-cache "
             "lookup; for measuring the in-memory hit-rate effect of "
             "canonicalization (default=false)"),
    cl::cat(SolvingCat));

} // namespace

///

typedef std::set< ref<Expr> > KeyType;

struct AssignmentLessThan {
  bool operator()(const Assignment *a, const Assignment *b) const {
    return a->bindings < b->bindings;
  }
};


class CexCachingSolver : public SolverImpl {
  typedef std::set<Assignment*, AssignmentLessThan> assignmentsTable_ty;

  std::unique_ptr<Solver> solver;
  
  MapOfSets<ref<Expr>, Assignment*> cache;
  // memo table
  assignmentsTable_ty assignmentsTable;

  // Disk cache support
  std::unique_ptr<DiskCexCache> diskCexCache_;

  // Circular buffer of recent hit(1)/miss(0) outcomes; cache is suspended for
  // W lookups when the hit rate in a full window falls below the threshold.
  std::vector<uint8_t> diskCacheWindow_;
  unsigned diskCacheWindowPos_  = 0;
  unsigned diskCacheWindowFill_ = 0;
  unsigned diskCacheWindowHits_ = 0;
  uint64_t diskCacheSuspendedAt_ = 0;     // 0 = active
  uint64_t diskCacheLookups_    = 0;

  int logFd_ = -1;
  ArrayCache logArrayCache_;

  void appendToCacheLog(const KeyType &key, Assignment *a);

  bool searchForAssignment(KeyType &key,
                           Assignment *&result);
  
  bool lookupAssignment(const Query& query, KeyType &key, Assignment *&result);

  bool lookupAssignment(const Query& query, Assignment *&result) {
    KeyType key;
    return lookupAssignment(query, key, result);
  }

  bool getAssignment(const Query& query, Assignment *&result);
  
public:
  CexCachingSolver(std::unique_ptr<Solver> solver);
  ~CexCachingSolver();

  bool computeTruth(const Query &, bool &isValid) override;
  bool computeValidity(const Query &, Solver::Validity &result) override;
  bool computeValue(const Query &, ref<Expr> &result) override;
  bool computeInitialValues(const Query &,
                            const std::vector<const Array *> &objects,
                            std::vector<std::vector<unsigned char>> &values,
                            bool &hasSolution) override;
  SolverRunStatus getOperationStatusCode() override;
  std::string getConstraintLog(const Query &query) override;
  void setCoreSolverTimeout(time::Span timeout) override;
};

///

struct NullAssignment {
  bool operator()(Assignment *a) const { return !a; }
};

struct NonNullAssignment {
  bool operator()(Assignment *a) const { return a!=0; }
};

struct NullOrSatisfyingAssignment {
  KeyType &key;
  
  NullOrSatisfyingAssignment(KeyType &_key) : key(_key) {}

  bool operator()(Assignment *a) const { 
    return !a || a->satisfies(key.begin(), key.end()); 
  }
};

/// searchForAssignment - Look for a cached solution for a query.
///
/// \param key - The query to look up.
/// \param result [out] - The cached result, if the lookup is successful. This is
/// either a satisfying assignment (for a satisfiable query), or 0 (for an
/// unsatisfiable query).
/// \return - True if a cached result was found.
bool CexCachingSolver::searchForAssignment(KeyType &key, Assignment *&result) {
  Assignment * const *lookup = cache.lookup(key);
  if (lookup) {
    result = *lookup;
    return true;
  }

  if (CexCacheTryAll) {
    // Look for a satisfying assignment for a superset, which is trivially an
    // assignment for any subset.
    Assignment **lookup = 0;
    if (CexCacheSuperSet)
      lookup = cache.findSuperset(key, NonNullAssignment());

    // Otherwise, look for a subset which is unsatisfiable, see below.
    if (!lookup) 
      lookup = cache.findSubset(key, NullAssignment());

    // If either lookup succeeded, then we have a cached solution.
    if (lookup) {
      result = *lookup;
      return true;
    }

    // Otherwise, iterate through the set of current assignments to see if one
    // of them satisfies the query.
    for (assignmentsTable_ty::iterator it = assignmentsTable.begin(), 
           ie = assignmentsTable.end(); it != ie; ++it) {
      Assignment *a = *it;
      if (a->satisfies(key.begin(), key.end())) {
        result = a;
        return true;
      }
    }
  } else {
    // FIXME: Which order? one is sure to be better.

    // Look for a satisfying assignment for a superset, which is trivially an
    // assignment for any subset.
    Assignment **lookup = 0;
    if (CexCacheSuperSet)
      lookup = cache.findSuperset(key, NonNullAssignment());

    // Otherwise, look for a subset which is unsatisfiable -- if the subset is
    // unsatisfiable then no additional constraints can produce a valid
    // assignment. While searching subsets, we also explicitly the solutions for
    // satisfiable subsets to see if they solve the current query and return
    // them if so. This is cheap and frequently succeeds.
    if (!lookup) 
      lookup = cache.findSubset(key, NullOrSatisfyingAssignment(key));

    // If either lookup succeeded, then we have a cached solution.
    if (lookup) {
      result = *lookup;
      return true;
    }
  }

  if (diskCexCache_) {
    if (diskCacheWindow_.empty()) {
      unsigned w = std::max(DiskCacheHitRateWindow.getValue(), 1u);
      diskCacheWindow_.assign(w, 0);
    }

    const unsigned W = static_cast<unsigned>(diskCacheWindow_.size());

    ++diskCacheLookups_;

    if (diskCacheSuspendedAt_ != 0) {
      if (diskCacheLookups_ - diskCacheSuspendedAt_ >= W) {
        diskCacheSuspendedAt_ = 0;
        diskCacheWindowPos_   = 0;
        diskCacheWindowFill_  = 0;
        diskCacheWindowHits_  = 0;
        klee_message("DiskCexCache: re-enabling after %u-lookup cooldown", W);
      } else {
        return false;
      }
    }

    Assignment *diskResult = nullptr;
    const bool hit = diskCexCache_->find(key, CexCacheSuperSet, diskResult);

    if (hit) {
      ++stats::queryCexDiskCacheHits;
    } else {
      ++stats::queryCexDiskCacheMisses;
    }

    if (diskCacheWindowFill_ == W) {
      diskCacheWindowHits_ -= diskCacheWindow_[diskCacheWindowPos_];
    } else {
      ++diskCacheWindowFill_;
    }
    diskCacheWindow_[diskCacheWindowPos_] = hit ? 1u : 0u;
    diskCacheWindowHits_ += hit ? 1u : 0u;
    diskCacheWindowPos_ = (diskCacheWindowPos_ + 1) % W;

    if (hit) {
      result = diskResult;
      return true;
    }

    if (DiskCacheMinHitRatePct > 0 && diskCacheWindowFill_ == W &&
        diskCacheWindowHits_ * 100 < W * DiskCacheMinHitRatePct) {
      diskCacheSuspendedAt_ = diskCacheLookups_;
      klee_warning("DiskCexCache: hit rate %.1f%% over last %u lookups — "
                   "suspending (threshold %u%%, will retry after %u misses)",
                   diskCacheWindowHits_ * 100.0 / W, W,
                   DiskCacheMinHitRatePct.getValue(), W);
    }
  }

  return false;
}

/// lookupAssignment - Lookup a cached result for the given \arg query.
///
/// \param query - The query to lookup.
/// \param key [out] - On return, the key constructed for the query.
/// \param result [out] - The cached result, if the lookup is successful. This is
/// either a satisfying assignment (for a satisfiable query), or 0 (for an
/// unsatisfiable query).
/// \return True if a cached result was found.
bool CexCachingSolver::lookupAssignment(const Query &query, 
                                        KeyType &key,
                                        Assignment *&result) {
  key = KeyType(query.constraints.begin(), query.constraints.end());
  ref<Expr> neg = Expr::createIsZero(query.expr);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(neg)) {
    if (CE->isFalse()) {
      result = (Assignment*) 0;
      ++stats::queryCexCacheHits;
      return true;
    }
  } else {
    key.insert(neg);
  }

  // Canonicalize expression trees so commutatively/associatively equivalent
  // queries collide in the in-memory cache. Arrays are left untouched, so the
  // cached Assignment (over the original arrays) stays valid for this query and
  // findSymbolicObjects still recovers the same objects.
  if (CanonicalizeCexKey) {
    std::vector<ref<Expr>> vec(key.begin(), key.end());
    std::vector<ref<Expr>> canon = canonicalizeExprTreesOnly(vec);
    key = KeyType(canon.begin(), canon.end());
  }

  bool found = searchForAssignment(key, result);
  if (found)
    ++stats::queryCexCacheHits;
  else ++stats::queryCexCacheMisses;
    
  return found;
}

bool CexCachingSolver::getAssignment(const Query& query, Assignment *&result) {
  KeyType key;
  if (lookupAssignment(query, key, result)) {
    // Promote disk hits to in-memory; assignmentsTable owns its entries so do
    // not add disk-owned pointers to it.
    if (diskCexCache_ && !cache.lookup(key))
      cache.insert(key, result);
    return true;
  }

  std::vector<const Array*> objects;
  findSymbolicObjects(key.begin(), key.end(), objects);

  std::vector< std::vector<unsigned char> > values;
  bool hasSolution;
  
  if (!solver->impl->computeInitialValues(query, objects, values, 
                                          hasSolution))
    return false;
    
  Assignment *binding;
  if (hasSolution) {
    binding = new Assignment(objects, values);

    // Memoize the result.
    std::pair<assignmentsTable_ty::iterator, bool>
      res = assignmentsTable.insert(binding);
    if (!res.second) {
      delete binding;
      binding = *res.first;
    }
    
    if (DebugCexCacheCheckBinding)
      if (!binding->satisfies(key.begin(), key.end())) {
        query.dump();
        binding->dump();
        klee_error("Generated assignment doesn't match query");
      }
  } else {
    binding = (Assignment*) 0;
  }
  
  result = binding;
  cache.insert(key, binding);

  if (logFd_ >= 0)
    appendToCacheLog(key, binding);

  return true;
}

///
CexCachingSolver::CexCachingSolver(std::unique_ptr<Solver> solver)
    : solver(std::move(solver)) {
  const std::string readPath = !DiskCexCacheFile.empty()
                                   ? DiskCexCacheFile.getValue()
                                   : PersistentCexCacheFile.getValue();

  // --write-disk-cex-cache wins; else --persistent-cex-cache=X uses X.log.
  std::string logPath;
  if (!WriteDiskCexCacheFile.empty())
    logPath = WriteDiskCexCacheFile.getValue();
  else if (!PersistentCexCacheFile.empty())
    logPath = PersistentCexCacheFile.getValue() + ".log";

  if (!readPath.empty())
    diskCexCache_ = std::make_unique<DiskCexCache>(
        readPath, buildCacheMetadata(), DiskCexCacheLruSize.getValue());

  if (!logPath.empty()) {
    logFd_ = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logFd_ < 0) {
      klee_warning("Cannot open CEX cache log '%s': %s",
                   logPath.c_str(), strerror(errno));
    } else {
      klee_message("CEX cache log: %s", logPath.c_str());
    }
  }
}


CexCachingSolver::~CexCachingSolver() {
  if (logFd_ >= 0)
    close(logFd_);

  cache.clear();
  for (assignmentsTable_ty::iterator it = assignmentsTable.begin(),
         ie = assignmentsTable.end(); it != ie; ++it)
    delete *it;
}

// Log record:
//   [u32 num_keys]([u32 key_len][key_bytes]) × num_keys
//   [u32 val_len][val_bytes]
//   [u32 num_concrete_arrays]([u8 idx][u32 len LE][bytes]) × num_concrete_arrays
void CexCachingSolver::appendToCacheLog(const KeyType &key, Assignment *a) {
  std::vector<ref<Expr>> vec(key.begin(), key.end());
  auto [diskKey, canon] =
      buildConstraintDiskKey(vec, logArrayCache_);

  // serializeAssignment guards the SAT path; mirror the cap here so the
  // UNSAT (value="") path does not silently truncate uint8 indices past 255.
  if (canon.forwardArrayMap.size() > 255) {
    klee_warning("appendToCacheLog: too many symbolic arrays (%zu) for "
                 "binary format (max 255); skipping log entry",
                 canon.forwardArrayMap.size());
    return;
  }

  std::string value = a ? DiskCexCache::serializeAssignment(a, canon.forwardArrayMap)
                        : ""; // UNSAT sentinel

  // Concrete-array entries are emitted in canonical-index order so the
  // reader sees a deterministic stream.
  struct ConcreteEntry { uint8_t idx; std::vector<unsigned char> bytes; };
  std::vector<ConcreteEntry> concretes;
  for (const auto &[orig, canon_arr] : canon.forwardArrayMap) {
    if (!canon_arr->isConstantArray()) continue;
    uint8_t idx = static_cast<uint8_t>(
        std::stoi(canon_arr->name.substr(1)));
    std::vector<unsigned char> bytes;
    bytes.reserve(canon_arr->constantValues.size());
    for (const auto &cv : canon_arr->constantValues)
      bytes.push_back(static_cast<unsigned char>(cv->getZExtValue()));
    concretes.push_back({idx, std::move(bytes)});
  }
  std::sort(concretes.begin(), concretes.end(),
            [](const ConcreteEntry &x, const ConcreteEntry &y) {
              return x.idx < y.idx;
            });

  std::string record;
  uint32_t numKeys = static_cast<uint32_t>(diskKey.size());
  record.append(reinterpret_cast<const char *>(&numKeys), 4);
  for (const auto &k : diskKey) {
    uint32_t klen = static_cast<uint32_t>(k.size());
    record.append(reinterpret_cast<const char *>(&klen), 4);
    record.append(k);
  }
  uint32_t vlen = static_cast<uint32_t>(value.size());
  record.append(reinterpret_cast<const char *>(&vlen), 4);
  record.append(value);

  uint32_t numConcrete = static_cast<uint32_t>(concretes.size());
  record.append(reinterpret_cast<const char *>(&numConcrete), 4);
  for (const auto &ce : concretes) {
    record.push_back(static_cast<char>(ce.idx));
    uint32_t len = static_cast<uint32_t>(ce.bytes.size());
    record.append(reinterpret_cast<const char *>(&len), 4);
    record.append(reinterpret_cast<const char *>(ce.bytes.data()), len);
  }

  // O_APPEND atomicity holds only up to PIPE_BUF; warn (don't drop) so
  // oversize records still land but interleaving risk is visible.
#ifdef PIPE_BUF
  if (record.size() > PIPE_BUF)
    klee_warning_once(nullptr,
                      "CEX cache log: record size %zu exceeds PIPE_BUF (%d); "
                      "concurrent writers may interleave records",
                      record.size(), PIPE_BUF);
#endif

  ssize_t written = write(logFd_, record.data(), record.size());
  if (written < 0 || static_cast<size_t>(written) != record.size())
    klee_warning("CEX cache log: short write (%zd of %zu bytes)",
                 written, record.size());
}

bool CexCachingSolver::computeValidity(const Query& query,
                                       Solver::Validity &result) {
  TimerStatIncrementer t(stats::cexCacheTime);
  Assignment *a;
  if (!getAssignment(query.withFalse(), a))
    return false;
  assert(a && "computeValidity() must have assignment");
  ref<Expr> q = a->evaluate(query.expr);
  assert(isa<ConstantExpr>(q) && 
         "assignment evaluation did not result in constant");

  if (cast<ConstantExpr>(q)->isTrue()) {
    if (!getAssignment(query, a))
      return false;
    result = !a ? Solver::True : Solver::Unknown;
  } else {
    if (!getAssignment(query.negateExpr(), a))
      return false;
    result = !a ? Solver::False : Solver::Unknown;
  }
  
  return true;
}

bool CexCachingSolver::computeTruth(const Query& query,
                                    bool &isValid) {
  TimerStatIncrementer t(stats::cexCacheTime);

  Assignment *a;
  if (!getAssignment(query, a))
    return false;

  isValid = !a;

  return true;
}

bool CexCachingSolver::computeValue(const Query& query,
                                    ref<Expr> &result) {
  TimerStatIncrementer t(stats::cexCacheTime);

  Assignment *a;
  if (!getAssignment(query.withFalse(), a))
    return false;
  assert(a && "computeValue() must have assignment");
  result = a->evaluate(query.expr);  
  assert(isa<ConstantExpr>(result) && 
         "assignment evaluation did not result in constant");
  return true;
}

bool 
CexCachingSolver::computeInitialValues(const Query& query,
                                       const std::vector<const Array*> 
                                         &objects,
                                       std::vector< std::vector<unsigned char> >
                                         &values,
                                       bool &hasSolution) {
  TimerStatIncrementer t(stats::cexCacheTime);
  Assignment *a;
  if (!getAssignment(query, a))
    return false;
  hasSolution = !!a;
  
  if (!a)
    return true;

  // FIXME: We should use smarter assignment for result so we don't
  // need redundant copy.
  values = std::vector< std::vector<unsigned char> >(objects.size());
  for (unsigned i=0; i < objects.size(); ++i) {
    const Array *os = objects[i];
    Assignment::bindings_ty::iterator it = a->bindings.find(os);
    
    if (it == a->bindings.end()) {
      values[i] = std::vector<unsigned char>(os->size, 0);
    } else {
      values[i] = it->second;
    }
  }
  
  return true;
}

SolverImpl::SolverRunStatus CexCachingSolver::getOperationStatusCode() {
  return solver->impl->getOperationStatusCode();
}

std::string CexCachingSolver::getConstraintLog(const Query& query) {
  return solver->impl->getConstraintLog(query);
}

void CexCachingSolver::setCoreSolverTimeout(time::Span timeout) {
  solver->impl->setCoreSolverTimeout(timeout);
}

///

std::unique_ptr<Solver>
klee::createCexCachingSolver(std::unique_ptr<Solver> solver) {
  return std::make_unique<Solver>(
      std::make_unique<CexCachingSolver>(std::move(solver)));
}
