//===-- OfflineEngine.h ---------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Solver-using infrastructure for klee-pcache-opt: canonical-key parsing,
// UNSAT checking and core extraction, byte-level dependency analysis.
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
class Z3Builder;
}

namespace klee_pcache_opt {

/// Owns the Parser keeping its ArrayCache alive, since `constraints` and
/// `arrays` hold raw pointers into it.
struct ParsedKey {
  std::vector<klee::ref<klee::Expr>> constraints;
  std::vector<const klee::Array *> arrays; // sorted by canonical index
  bool ok = false;
  std::string error;

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
  /// 0 disables the per-query timeout.
  explicit OfflineEngine(unsigned coreSolverTimeoutSeconds = 30);
  ~OfflineEngine();

  /// Parse a canonical key into ref<Expr> constraints + their arrays.
  /// `concreteArrays` (canonical name_index → byte values) reconstructs
  /// concrete arrays from log metadata; arrays not listed here are
  /// synthesised as symbolic of size kSyntheticArraySize.  On failure
  /// returns ParsedKey with ok=false and an error message.
  ParsedKey parseKey(
      const std::set<std::string> &key,
      const std::vector<std::pair<uint8_t, std::vector<unsigned char>>>
          &concreteArrays = {});

  static void collectArrayNames(const std::string &s,
                                std::set<std::string> &out);

  /// Sets timedOut=true on solver timeout (return value meaningless then).
  bool isUnsat(const std::vector<klee::ref<klee::Expr>> &constraints,
               bool &timedOut);

  bool getInitialValues(const std::vector<klee::ref<klee::Expr>> &constraints,
                        const std::vector<const klee::Array *> &arrays,
                        std::vector<std::vector<unsigned char>> &assignment,
                        bool &timedOut);

  struct UnsatCoreResult {
    std::vector<std::size_t> coreIndices; // not guaranteed minimal
    bool ok = false;       // false ⇒ SAT and coreIndices empty
    bool timedOut = false;
  };

  /// Bypasses KLEE's solver chain; uses Z3Builder + Z3_solver_assert_and_track.
  UnsatCoreResult
  getUnsatCore(const std::vector<klee::ref<klee::Expr>> &constraints);

  struct MinimizationResult {
    std::vector<size_t> keptIndices;
    bool timedOut = false;
    /// SAT under symbolic-array reconstruction — caller should skip the entry.
    bool notUnsatPrecondition = false;
    std::size_t solverCalls = 0;
  };

  /// Worst case O(N) solver calls per shrink pass; typically far less.
  MinimizationResult
  minimizeUnsatCore(const std::vector<klee::ref<klee::Expr>> &constraints);

  struct ByteComponent {
    std::vector<std::size_t> constraintIndices;
    std::set<std::string> arrayNames;
    /// Per-array smallest covering size (max constant offset + 1).
    /// nullopt = symbolic index, requires full original size.
    std::map<std::string, std::optional<std::uint32_t>> arrayCoverage;
  };

  /// Partition by byte-level array dependency.  Two constraints share a
  /// component iff they touch the same (array, byte) pair; a symbolic index
  /// into A unites with every prior toucher of A.  Finer than KLEE's
  /// IndependentSolver, which groups by whole-array dependence.
  std::vector<ByteComponent>
  computeByteComponents(const std::vector<klee::ref<klee::Expr>> &constraints);

  klee::ArrayCache &arrayCache() { return arrayCache_; }
  klee::ExprBuilder &builder() { return *builder_; }

private:
  klee::ArrayCache arrayCache_;
  std::unique_ptr<klee::ExprBuilder> builder_;
  std::unique_ptr<klee::Solver> solver_;
  // Lazily created on first use so solver-free runs pay no Z3 startup cost.
  std::unique_ptr<klee::Z3Builder> z3Builder_;
  unsigned timeoutSec_;
};

} // namespace klee_pcache_opt

#endif // KLEE_PCACHE_OPT_OFFLINE_ENGINE_H
