#pragma once
// The Crump-Gospodinov ("CG") bootstrap for yield curves.
//
// NOTE: this is *statistical* bootstrapping (resampling history to measure
// uncertainty), not the *curve* bootstrapping in mcb/curves (solving today's
// discount factors from quotes).  The curve engine produces yield curves;
// this module takes a history of such curves and manufactures thousands of
// alternative histories that are statistically alike.
//
// Why not just resample yields?  Yields are extremely persistent and
// mechanically linked across maturities (an n-period yield is the average of
// n forwards), so naive resampling produces curves that violate these
// identities and regressions with meaningless p-values.  Instead:
//
//   1. convert yields to forwards and to difference returns
//        dr(n)_{t+1} = f(n)_t - f(n-1)_{t+1}
//      which are close to serially uncorrelated "news";
//   2. keep one persistent series, the longest forward f(N), and pre-whiten
//      it with a bias-corrected AR(1) (jointly with any external predictors
//      in a VAR(1));
//   3. resample whole rows (all maturities together, preserving the
//      cross-section) in blocks of M = (T N)^(2/5) consecutive periods
//      (preserving remaining serial dependence);
//   4. rebuild f(N) from its autoregression and every other forward from
//        f(n-1)_{t+1} = f(n)_t - dr(n)_{t+1},
//      starting from the observed first curve.  Yields and returns follow
//      from identities, so every bootstrap history is internally consistent.
#include <Eigen/Dense>

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "mcb/stats/linear_model.hpp"
#include "mcb/stats/var_model.hpp"

namespace mcb {

struct CgOptions {
  int blockLength = 0;            // 0 = rule of thumb (T N)^(2/5)
  bool prewhiten = true;          // AR/VAR(1) pre-whitening of f(N) and external series
  bool biasCorrect = true;        // Kilian (1998) bias correction of the autoregression
  int biasCorrectionReps = 1000;
  std::uint64_t seed = 884;       // for the bias-correction simulation
};

struct CgSample {
  Eigen::MatrixXd forwards;  // T x N per-period forwards
  Eigen::MatrixXd yields;    // T x N per-period yields
  Eigen::MatrixXd external;  // T x K external series (0 columns if none)
};

class CgBootstrap {
 public:
  /// yields: T x N per-period yields (see YieldPanel). external: T x K series
  /// resampled jointly with the curve (pass an empty matrix for none).
  CgBootstrap(const Eigen::MatrixXd& yields, const Eigen::MatrixXd& external, const CgOptions& options = {});
  explicit CgBootstrap(const Eigen::MatrixXd& yields, const CgOptions& options = {})
      : CgBootstrap(yields, Eigen::MatrixXd(), options) {}

  Eigen::Index T() const { return observed_.yields.rows(); }
  Eigen::Index N() const { return observed_.yields.cols(); }
  Eigen::Index K() const { return observed_.external.cols(); }
  int blockLength() const { return blockLength_; }
  static int ruleOfThumbBlockLength(Eigen::Index T, Eigen::Index N);

  const CgSample& observed() const { return observed_; }
  const Eigen::MatrixXd& differenceReturns() const { return dr_; }
  /// Autoregression of the persistent block x_t = (f(N)_t, external_t).
  const Var1& persistentModel() const { return model_; }

  /// One bootstrap history of the same length as the data, conditional on the
  /// observed first forward curve.
  CgSample draw(std::mt19937_64& rng) const;

  /// `horizon` future periods starting from a given current forward curve
  /// (and external state): row 0 of the result is the current state, rows
  /// 1..horizon are simulated.
  CgSample simulate(const Eigen::VectorXd& currentForwards, const Eigen::VectorXd& currentExternal, int horizon,
                    std::mt19937_64& rng) const;

 private:
  std::vector<Eigen::Index> resampleRows(Eigen::Index count, std::mt19937_64& rng) const;

  CgOptions options_;
  CgSample observed_;
  Eigen::MatrixXd dr_;        // (T-1) x (N-1)
  Eigen::MatrixXd x_;         // T x (1+K): f(N) and external series
  Var1 model_;
  Eigen::MatrixXd shocks_;    // (T-1) x (1+K): centred residuals (or level changes without pre-whitening)
  int blockLength_ = 1;
};

// ---- regression inference -----------------------------------------------------

struct RegressionData {
  Eigen::MatrixXd X;  // regressors without the intercept
  Eigen::VectorXd y;
};
/// Builds a regression from a (real or bootstrapped) sample. Return false to skip a draw.
using RegressionBuilder = std::function<bool(const CgSample& sample, RegressionData& out)>;

struct BootstrapRegression {
  OlsResult sample;                 // estimates on the observed data
  Eigen::VectorXd tStats;           // (beta - null) / se
  Eigen::VectorXd pBootstrap;       // symmetric percentile-t bootstrap p-values (equation (17) of the paper)
  Eigen::VectorXd pAsymptotic;      // conventional normal-approximation p-values, for comparison
  Eigen::MatrixXd betaDraws;        // replications x coefficients
  int replications = 0;
};

/// Bootstrap inference for an OLS regression whose variables are built from
/// the yield curve.  nullValues holds the hypothesised coefficient values
/// (including the intercept as element 0).
BootstrapRegression bootstrapRegression(const CgBootstrap& cg, const RegressionBuilder& build,
                                        const Eigen::VectorXd& nullValues, int replications,
                                        CovarianceType covariance, int nwLags, std::uint64_t seed);

/// Two-sided p-value of a standard normal statistic.
double normalPValue(double t);

}  // namespace mcb
