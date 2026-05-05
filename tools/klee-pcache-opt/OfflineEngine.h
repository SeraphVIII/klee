//===-- OfflineEngine.h ---------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Foundation infrastructure for solver-using passes of klee-pcache-opt.
//
// Canonical-key roundtrip
//   Cache keys are sets of constraint strings produced by ExprPPrinter on
//   alpha-renamed expressions (arrays A0, A1, ...).  printSingleExpr emits
//   no array declarations, so we synthesise them from the array names that
//   appear in the constraint strings and feed a synthetic KQuery to the
//   kleaver parser to recover ref<Expr> objects.
//
//   When the log record includes a concrete-arrays section (written by
//   CexCachingSolver::appendToCacheLog), callers pass that data to parseKey,
//   which reconstructs concrete arrays faithfully instead of using symbolic
//   placeholders.  Entries loaded from a cache file (which does not store
//   concrete-array metadata) still use symbolic reconstruction.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_PCACHE_OPT_OFFLINE_ENGINE_H
#define KLEE_PCACHE_OPT_OFFLINE_ENGINE_H

#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/ExprBuilder.h"
#include "klee/Expr/Parser/Parser.h"
#include "klee/Solver/Solver.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace klee {
class Z3Builder; // private to lib/Solver, owned via unique_ptr below
}

namespace klee_pcache_opt {

/// Result of parsing a canonical key.
///
/// Owns the kleaver Parser so that the Arrays referenced by `constraints`
/// stay alive: Parser holds an ArrayCache that deletes its Arrays on
/// destruction, and the parsed Exprs hold raw Array* pointers into it.
struct ParsedKey {
  std::vector<klee::ref<klee::Expr>> constraints;
  std::vector<const klee::Array *> arrays; // sorted by canonical index
  bool ok = false;
  std::string error;

  // Keep-alive bag — callers should not depend on the contents.
  std::unique_ptr<klee::expr::Parser> _parser;
  std::vector<std::unique_ptr<klee::expr::Decl>> _decls;

  ParsedKey() = default;
  ParsedKey(ParsedKey &&) = default;
  ParsedKey &operator=(ParsedKey &&) = default;
  ParsedKey(const ParsedKey &) = delete;
  ParsedKey &operator=(const ParsedKey &) = delete;
};

class OfflineEngine {
public:
  /// \param coreSolverTimeoutSeconds  Per-query timeout passed to the core
  ///        solver. 0 disables the timeout.
  explicit OfflineEngine(unsigned coreSolverTimeoutSeconds = 30);
  ~OfflineEngine();

  /// Parse a canonical key (set of constraint strings) into a vector of
  /// ref<Expr> constraints plus the arrays they reference.
  /// \param concreteArrays  Optional concrete-array metadata from the log
  ///        record: each pair is (canonical name_index, byte_values).  Arrays
  ///        listed here are reconstructed as concrete KQuery arrays; all
  ///        others are reconstructed as fully symbolic arrays of size
  ///        kSyntheticArraySize.  Pass an empty vector (the default) when no
  ///        metadata is available (e.g. entries loaded from a cache file).
  /// On parse failure returns ParsedKey with ok=false and an error message.
  ParsedKey parseKey(
      const std::set<std::string> &key,
      const std::vector<std::pair<uint8_t, std::vector<unsigned char>>>
          &concreteArrays = {});

  /// Scan a constraint string for canonical array name references (A0, A1,
  /// ...).  Used by callers building up dependency clusters across entries.
  static void collectArrayNames(const std::string &s,
                                std::set<std::string> &out);

  /// \return true iff the constraint set is provably UNSAT.
  ///         Sets `timedOut=true` if the underlying solver timed out (in
  ///         which case the boolean return is meaningless).
  bool isUnsat(const std::vector<klee::ref<klee::Expr>> &constraints,
               bool &timedOut);

  /// Compute initial values satisfying the constraints.  Returns true on
  /// success (sets `assignment`); false on UNSAT or solver failure.
  bool getInitialValues(const std::vector<klee::ref<klee::Expr>> &constraints,
                        const std::vector<const klee::Array *> &arrays,
                        std::vector<std::vector<unsigned char>> &assignment,
                        bool &timedOut);

  struct UnsatCoreResult {
    /// Indices into the input constraint vector that participate in the core
    /// returned by Z3.  Note: not guaranteed minimal — Z3's core may still
    /// contain redundant constraints.  Callers wanting strict minimality
    /// should follow up with delta-debugging on this subset.
    std::vector<std::size_t> coreIndices;
    /// True iff the constraint set was actually UNSAT.  False here means the
    /// solver returned SAT (no core to extract) and `coreIndices` is empty.
    bool ok = false;
    /// True iff the underlying Z3 call hit the timeout.
    bool timedOut = false;
  };

  /// Single-call unsat-core extraction via a parallel Z3 context.
  /// Bypasses KLEE's solver chain entirely — uses Z3Builder + a fresh Z3
  /// solver configured with `(set-option :unsat-core true)` and
  /// `Z3_solver_assert_and_track`.
  UnsatCoreResult
  getUnsatCore(const std::vector<klee::ref<klee::Expr>> &constraints);

  struct MinimizationResult {
    /// Indices into `constraints` that form a minimal UNSAT subset.  If
    /// minimisation aborted (precondition fail / timeout), this is the
    /// original 0..N-1 list and `aborted=true`.
    std::vector<size_t> keptIndices;
    /// True iff any solver call during minimisation timed out.
    bool timedOut = false;
    /// True iff the input set was not UNSAT under symbolic-array reconstruction
    /// (e.g. it relied on concrete-array contents that the canonical key cannot
    /// preserve).  No minimisation is attempted in this case.
    bool notUnsatPrecondition = false;
    /// Number of solver calls issued.
    std::size_t solverCalls = 0;
  };

  /// Iteratively delta-debug the constraint set down to a minimal UNSAT
  /// subset.  Worst case O(N) solver calls per shrink pass; typically much
  /// less because most removals immediately yield SAT and are rejected.
  MinimizationResult
  minimizeUnsatCore(const std::vector<klee::ref<klee::Expr>> &constraints);

  struct ByteComponent {
    /// Constraint indices in this component.
    std::vector<std::size_t> constraintIndices;
    /// Canonical array names ("A0", "A3", ...) touched by this component.
    std::set<std::string> arrayNames;
    /// For each array name, the smallest size that covers all bytes touched
    /// by the component (max constant offset + 1).  std::nullopt means the
    /// array was indexed symbolically and the full original size is required.
    std::map<std::string, std::optional<std::uint32_t>> arrayCoverage;
  };

  /// Partition constraints by byte-level array dependency.
  ///
  /// Two constraints share a component iff they touch the same (array, byte)
  /// pair.  A symbolic index into array A makes that constraint depend on
  /// every byte of A — any other constraint touching any byte of A joins the
  /// same component.
  ///
  /// IndependentSolver, which sits above the persistent cache, already groups
  /// constraints by *array* dependency.  Splitting here is finer-grained: it
  /// can break apart entries whose constraints touch disjoint byte ranges
  /// within the same array.
  std::vector<ByteComponent>
  computeByteComponents(const std::vector<klee::ref<klee::Expr>> &constraints);

  klee::ArrayCache &arrayCache() { return arrayCache_; }
  klee::ExprBuilder &builder() { return *builder_; }

private:
  klee::ArrayCache arrayCache_;
  std::unique_ptr<klee::ExprBuilder> builder_;
  std::unique_ptr<klee::Solver> solver_;
  // Parallel Z3 context used for unsat-core extraction.  Lazily created on
  // the first call so tools that never minimise UNSAT pay no Z3 overhead.
  std::unique_ptr<klee::Z3Builder> z3Builder_;
  unsigned timeoutSec_;
};

} // namespace klee_pcache_opt

#endif // KLEE_PCACHE_OPT_OFFLINE_ENGINE_H
