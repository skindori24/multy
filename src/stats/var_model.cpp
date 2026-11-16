#include "mcb/stats/var_model.hpp"

#include <cmath>

#include "mcb/core/error.hpp"

namespace mcb {

double spectralRadius(const Eigen::MatrixXd& A) {
  if (A.rows() == 1) return std::fabs(A(0, 0));
  const Eigen::EigenSolver<Eigen::MatrixXd> es(A, false);
  return es.eigenvalues().cwiseAbs().maxCoeff();
}

namespace {

// OLS of Y_t on (1, Y_{t-1}); returns mu and Phi.
void estimate(const Eigen::MatrixXd& Y, Eigen::VectorXd& mu, Eigen::MatrixXd& Phi) {
  const Eigen::Index T = Y.rows(), k = Y.cols();
  Eigen::MatrixXd X(T - 1, k + 1);
  X.col(0).setOnes();
  X.rightCols(k) = Y.topRows(T - 1);
  const Eigen::MatrixXd B = (X.transpose() * X).ldlt().solve(X.transpose() * Y.bottomRows(T - 1));  // (k+1) x k
  mu = B.row(0).transpose();
  Phi = B.bottomRows(k).transpose();
}

Eigen::MatrixXd residualsOf(const Eigen::MatrixXd& Y, const Eigen::VectorXd& mu, const Eigen::MatrixXd& Phi) {
  const Eigen::Index T = Y.rows();
  Eigen::MatrixXd E = Y.bottomRows(T - 1) - Y.topRows(T - 1) * Phi.transpose();
  E.rowwise() -= mu.transpose();
  return E;
}

}  // namespace

Var1 fitVar1(const Eigen::MatrixXd& Y) {
  MCB_REQUIRE(Y.rows() > Y.cols() + 3, "VAR(1): not enough observations");
  Var1 m;
  estimate(Y, m.mu, m.Phi);
  m.residuals = residualsOf(Y, m.mu, m.Phi);
  m.maxEigenvalue = spectralRadius(m.Phi);
  return m;
}

Var1 fitVar1BiasCorrected(const Eigen::MatrixXd& Y, int reps, std::mt19937_64& rng) {
  Var1 m = fitVar1(Y);
  if (reps <= 0 || m.maxEigenvalue >= 1.0) return m;  // leave explosive estimates alone

  const Eigen::Index T = Y.rows(), k = Y.cols();
  // Mean bootstrap estimate of Phi when the truth is the OLS estimate.
  Eigen::MatrixXd centred = m.residuals;
  centred.rowwise() -= centred.colwise().mean();
  std::uniform_int_distribution<Eigen::Index> pick(0, T - 2);
  Eigen::MatrixXd sumPhi = Eigen::MatrixXd::Zero(k, k);
  Eigen::MatrixXd sim(T, k);
  Eigen::VectorXd mu;
  Eigen::MatrixXd Phi;
  for (int b = 0; b < reps; ++b) {
    sim.row(0) = Y.row(0);
    for (Eigen::Index t = 1; t < T; ++t)
      sim.row(t) = (m.mu + m.Phi * sim.row(t - 1).transpose() + centred.row(pick(rng)).transpose()).transpose();
    estimate(sim, mu, Phi);
    sumPhi += Phi;
  }
  const Eigen::MatrixXd bias = sumPhi / reps - m.Phi;

  // Subtract the bias, backing off until the corrected system is stationary.
  Eigen::MatrixXd corrected = m.Phi - bias;
  for (double delta = 1.0; spectralRadius(corrected) >= 1.0 && delta > 0.0; delta -= 0.01)
    corrected = m.Phi - delta * bias;
  if (spectralRadius(corrected) >= 1.0) return m;

  m.Phi = corrected;
  // Keep the unconditional mean at the sample mean.
  const Eigen::VectorXd ybar = Y.colwise().mean().transpose();
  m.mu = (Eigen::MatrixXd::Identity(k, k) - m.Phi) * ybar;
  m.residuals = residualsOf(Y, m.mu, m.Phi);
  m.maxEigenvalue = spectralRadius(m.Phi);
  m.biasCorrected = true;
  return m;
}

}  // namespace mcb
