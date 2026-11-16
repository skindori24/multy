#include "mcb/curves/bootstrap.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "mcb/core/error.hpp"
#include "mcb/math/solver.hpp"

namespace mcb {
namespace {

// Bounds on ln DF at time t: zero rates between roughly -100% and +400%.
double lowerBound(double t) { return -4.0 * t - 0.5; }
double upperBound(double t) { return 1.0 * t + 0.5; }

}  // namespace

std::shared_ptr<YieldCurve> bootstrapCurve(const std::string& name, Date referenceDate,
                                           const std::vector<const RateHelper*>& helpers,
                                           const std::vector<double>& quotes,
                                           const DiscountCurve* discountCurve, const BootstrapOptions& options,
                                           BootstrapReport* report, const YieldCurve* warmStart,
                                           std::size_t firstDirtyNode) {
  const std::size_t n = helpers.size();
  MCB_REQUIRE(n > 0, "curve '" << name << "': no instruments to bootstrap from");
  MCB_REQUIRE(quotes.size() == n, "curve '" << name << "': " << n << " instruments but " << quotes.size() << " quotes");

  // Sort by pillar date, remembering the caller's order for the error report.
  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return helpers[a]->pillarDate() < helpers[b]->pillarDate(); });
  std::vector<const RateHelper*> sorted(n);
  std::vector<double> q(n);  // quotes in pillar order
  std::vector<Date> pillars(n);
  for (std::size_t i = 0; i < n; ++i) {
    sorted[i] = helpers[order[i]];
    q[i] = quotes[order[i]];
    MCB_REQUIRE(std::isfinite(q[i]), "curve '" << name << "': missing quote for '" << sorted[i]->label << "'");
    pillars[i] = sorted[i]->pillarDate();
    MCB_REQUIRE(pillars[i] > referenceDate, "curve '" << name << "': instrument '" << sorted[i]->label
                                                      << "' has already matured (pillar " << pillars[i].iso() << ")");
    MCB_REQUIRE(i == 0 || pillars[i] > pillars[i - 1],
                "curve '" << name << "': instruments '" << sorted[i - 1]->label << "' and '" << sorted[i]->label
                          << "' share the pillar date " << pillars[i].iso());
  }

  // Initial guess: the warm start if it matches, otherwise a flat 2% curve.
  const bool warm = warmStart != nullptr && warmStart->pillarDates() == pillars;
  std::vector<double> dfs(n);
  for (std::size_t i = 0; i < n; ++i)
    dfs[i] = warm ? std::exp(warmStart->nodeLogDiscounts()[i + 1]) : std::exp(-0.02 * (pillars[i] - referenceDate) / 365.0);
  auto curve = std::make_shared<YieldCurve>(name, referenceDate, pillars, dfs, options.interpolation);
  const DiscountCurve& disc = discountCurve ? *discountCurve : static_cast<const DiscountCurve&>(*curve);
  const std::vector<double>& t = curve->nodeTimes();

  BootstrapReport rep;
  const bool local = isLocal(options.interpolation);

  // Solves node i (1-based) so that its instrument reprices, with the
  // interpolation fitted through the first `active` nodes.
  auto solveNode = [&](std::size_t i, std::size_t active, double guess) {
    const RateHelper& h = *sorted[i - 1];
    auto f = [&](double x) {
      curve->setNodeLogDiscount(i, x);
      curve->refit(active);
      ++rep.evaluations;
      return h.impliedQuote(*curve, disc) - q[i - 1];
    };
    const double step = 1e-4 * std::max(t[i], 1.0 / 365.0);
    const RootResult r = findRoot(f, guess, step, lowerBound(t[i]), upperBound(t[i]), 1e-15, 1e-16);
    MCB_REQUIRE(r.converged, "curve '" << name << "': cannot solve for instrument '" << h.label << "' (pillar "
                                       << h.pillarDate().iso() << ", quote " << q[i - 1] << ")");
    curve->setNodeLogDiscount(i, r.x);
    curve->refit(active);
    return r.x;
  };

  // ---- first sweep: one node at a time, curve defined up to the current node
  // (skipped for a warm-started non-local curve: its nodes are already close,
  //  so the full-curve sweeps below converge directly)
  if (local || !warm) {
    const std::size_t start = warm ? std::max<std::size_t>(firstDirtyNode, 1) : 1;
    for (std::size_t i = start; i <= n; ++i) {
      double guess = curve->nodeLogDiscounts()[i];
      if (!warm && i > 1) guess = curve->nodeLogDiscounts()[i - 1] * t[i] / t[i - 1];  // carry the zero rate forward
      solveNode(i, i + 1, guess);
    }
  }
  curve->refit();
  rep.passes = 1;
  rep.method = "single pass";
  rep.converged = true;

  // ---- non-local interpolation: repeat until the nodes stop moving
  if (!local) {
    rep.method = "iterative";
    rep.converged = false;
    const int gaussSeidelPasses = options.globalNewton ? 1 : options.maxPasses;
    for (int pass = 0; pass < gaussSeidelPasses; ++pass) {
      double maxMove = 0.0;
      for (std::size_t i = 1; i <= n; ++i) {
        const double before = curve->nodeLogDiscounts()[i];
        const double after = solveNode(i, n + 1, before);
        maxMove = std::max(maxMove, std::fabs(after - before));
      }
      ++rep.passes;
      if (maxMove < options.tolerance) {
        rep.converged = true;
        break;
      }
    }
    if (!rep.converged) {
      // Simultaneous Newton solve: F(y) = implied(y) - quotes = 0, with the
      // Jacobian obtained by bumping each node.
      rep.method = "global Newton";
      Eigen::VectorXd F(static_cast<Eigen::Index>(n));
      auto residuals = [&](Eigen::VectorXd& out) {
        double worst = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
          out[static_cast<Eigen::Index>(k)] = sorted[k]->impliedQuote(*curve, disc) - q[k];
          ++rep.evaluations;
          worst = std::max(worst, std::fabs(out[static_cast<Eigen::Index>(k)]));
        }
        return worst;
      };
      double err = residuals(F);
      for (int it = 0; it < 50 && err > 1e-15; ++it) {
        const std::vector<double> jac = quoteJacobian(*curve, sorted, nullptr, discountCurve);
        rep.evaluations += static_cast<long>(2 * n * n);
        Eigen::MatrixXd J(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        for (std::size_t k = 0; k < n; ++k)
          for (std::size_t i = 0; i < n; ++i) J(static_cast<Eigen::Index>(k), static_cast<Eigen::Index>(i)) = jac[k * n + i];
        const Eigen::VectorXd delta = J.partialPivLu().solve(-F);
        const std::vector<double> y0 = curve->nodeLogDiscounts();
        double lambda = 1.0, newErr = err;
        Eigen::VectorXd Fnew(static_cast<Eigen::Index>(n));
        for (int ls = 0; ls < 30; ++ls) {  // backtracking line search
          for (std::size_t i = 1; i <= n; ++i)
            curve->setNodeLogDiscount(i, y0[i] + lambda * delta[static_cast<Eigen::Index>(i - 1)]);
          curve->refit();
          newErr = residuals(Fnew);
          if (newErr < err || newErr < 1e-15) break;
          lambda *= 0.5;
        }
        ++rep.passes;
        const double move = lambda * delta.cwiseAbs().maxCoeff();
        F = Fnew;
        const bool stalled = !(newErr < err);
        err = newErr;
        if (move < options.tolerance || stalled) break;
      }
      rep.converged = err < 1e-11;
    }
  }

  // ---- final repricing errors, reported in the caller's order
  rep.errors.assign(n, 0.0);
  rep.maxAbsError = 0.0;
  for (std::size_t k = 0; k < n; ++k) {
    const double e = sorted[k]->impliedQuote(*curve, disc) - q[k];
    rep.errors[order[k]] = e;
    rep.maxAbsError = std::max(rep.maxAbsError, std::fabs(e));
  }
  if (report) *report = rep;
  return curve;
}

std::vector<double> quoteJacobian(YieldCurve& curve, const std::vector<const RateHelper*>& helpers,
                                  const DiscountCurve* forwardingCurve, const DiscountCurve* discountCurve) {
  const std::size_t m = helpers.size(), n = curve.size();
  const DiscountCurve& fwd = forwardingCurve ? *forwardingCurve : static_cast<const DiscountCurve&>(curve);
  const DiscountCurve& disc = discountCurve ? *discountCurve : fwd;
  std::vector<double> jac(m * n, 0.0);
  const double h = 1e-6;
  for (std::size_t i = 1; i <= n; ++i) {
    const double y = curve.nodeLogDiscounts()[i];
    curve.setNodeLogDiscount(i, y + h);
    curve.refit();
    std::vector<double> up(m);
    for (std::size_t k = 0; k < m; ++k) up[k] = helpers[k]->impliedQuote(fwd, disc);
    curve.setNodeLogDiscount(i, y - h);
    curve.refit();
    for (std::size_t k = 0; k < m; ++k) jac[k * n + (i - 1)] = (up[k] - helpers[k]->impliedQuote(fwd, disc)) / (2.0 * h);
    curve.setNodeLogDiscount(i, y);
  }
  curve.refit();
  return jac;
}

}  // namespace mcb
