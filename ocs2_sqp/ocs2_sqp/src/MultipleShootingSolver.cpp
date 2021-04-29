/******************************************************************************
Copyright (c) 2020, Farbod Farshidian. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include "ocs2_sqp/MultipleShootingSolver.h"

#include <iostream>
#include <numeric>

#include <ocs2_core/control/FeedforwardController.h>
#include <ocs2_core/control/LinearController.h>
#include <ocs2_core/misc/LinearInterpolation.h>
#include <ocs2_core/soft_constraint/penalties/RelaxedBarrierPenalty.h>

#include "ocs2_sqp/MultipleShootingTranscription.h"

namespace ocs2 {

MultipleShootingSolver::MultipleShootingSolver(Settings settings, const SystemDynamicsBase* systemDynamicsPtr,
                                               const CostFunctionBase* costFunctionPtr,
                                               const SystemOperatingTrajectoriesBase* operatingTrajectoriesPtr,
                                               const ConstraintBase* constraintPtr, const CostFunctionBase* terminalCostFunctionPtr)
    : SolverBase(), settings_(std::move(settings)), hpipmInterface_(hpipm_interface::OcpSize(), settings.hpipmSettings) {
  // Multithreading, set up threadpool for N-1 helpers, our main thread is the N-th one.
  if (settings_.nThreads > 1) {
    threadPoolPtr_.reset(new ThreadPool(settings_.nThreads - 1, settings_.threadPriority));
  }
  Eigen::setNbThreads(1);  // No multithreading within Eigen.
  Eigen::initParallel();

  // Dynamics discretization
  discretizer_ = selectDynamicsDiscretization(settings.integratorType);
  sensitivityDiscretizer_ = selectDynamicsSensitivityDiscretization(settings.integratorType);

  // Clone objects to have one for each worker
  for (int w = 0; w < settings.nThreads; w++) {
    systemDynamicsPtr_.emplace_back(systemDynamicsPtr->clone());
    costFunctionPtr_.emplace_back(costFunctionPtr->clone());
    if (constraintPtr != nullptr) {
      constraintPtr_.emplace_back(constraintPtr->clone());
    } else {
      constraintPtr_.emplace_back(nullptr);
    }
  }

  // Operating points
  operatingTrajectoriesPtr_.reset(operatingTrajectoriesPtr->clone());

  if (constraintPtr == nullptr) {
    settings_.projectStateInputEqualityConstraints = false;  // True does not make sense if there are no constraints.
  }

  if (constraintPtr != nullptr && settings_.inequalityConstraintMu > 0) {
    std::unique_ptr<RelaxedBarrierPenalty> penaltyFunction(
        new RelaxedBarrierPenalty({settings_.inequalityConstraintMu, settings_.inequalityConstraintDelta}));
    penaltyPtr_.reset(new SoftConstraintPenalty(std::move(penaltyFunction)));
  }

  if (terminalCostFunctionPtr != nullptr) {
    terminalCostFunctionPtr_.reset(terminalCostFunctionPtr->clone());
  }
}

MultipleShootingSolver::~MultipleShootingSolver() {
  if (settings_.printSolverStatistics) {
    std::cerr << getBenchmarkingInformation() << std::endl;
  }
}

void MultipleShootingSolver::reset() {
  // Clear solution
  primalSolution_ = PrimalSolution();
  performanceIndeces_.clear();

  // reset timers
  totalNumIterations_ = 0;
  linearQuadraticApproximationTimer_.reset();
  solveQpTimer_.reset();
  linesearchTimer_.reset();
  computeControllerTimer_.reset();
}

std::string MultipleShootingSolver::getBenchmarkingInformation() const {
  const auto linearQuadraticApproximationTotal = linearQuadraticApproximationTimer_.getTotalInMilliseconds();
  const auto solveQpTotal = solveQpTimer_.getTotalInMilliseconds();
  const auto linesearchTotal = linesearchTimer_.getTotalInMilliseconds();
  const auto computeControllerTotal = computeControllerTimer_.getTotalInMilliseconds();

  const auto benchmarkTotal = linearQuadraticApproximationTotal + solveQpTotal + linesearchTotal + computeControllerTotal;

  std::stringstream infoStream;
  if (benchmarkTotal > 0.0) {
    const scalar_t inPercent = 100.0;
    infoStream << "\n########################################################################\n";
    infoStream << "The benchmarking is computed over " << totalNumIterations_ << " iterations. \n";
    infoStream << "SQP Benchmarking\t   :\tAverage time [ms]   (% of total runtime)\n";
    infoStream << "\tLQ Approximation   :\t" << linearQuadraticApproximationTimer_.getAverageInMilliseconds() << " [ms] \t\t("
               << linearQuadraticApproximationTotal / benchmarkTotal * inPercent << "%)\n";
    infoStream << "\tSolve QP           :\t" << solveQpTimer_.getAverageInMilliseconds() << " [ms] \t\t("
               << solveQpTotal / benchmarkTotal * inPercent << "%)\n";
    infoStream << "\tLinesearch         :\t" << linesearchTimer_.getAverageInMilliseconds() << " [ms] \t\t("
               << linesearchTotal / benchmarkTotal * inPercent << "%)\n";
    infoStream << "\tCompute Controller :\t" << computeControllerTimer_.getAverageInMilliseconds() << " [ms] \t\t("
               << computeControllerTotal / benchmarkTotal * inPercent << "%)\n";
  }
  return infoStream.str();
}

const std::vector<PerformanceIndex>& MultipleShootingSolver::getIterationsLog() const {
  if (performanceIndeces_.empty()) {
    throw std::runtime_error("[MultipleShootingSolver]: No performance log yet, no problem solved yet?");
  } else {
    return performanceIndeces_;
  }
}

void MultipleShootingSolver::runImpl(scalar_t initTime, const vector_t& initState, scalar_t finalTime,
                                     const scalar_array_t& partitioningTimes) {
  if (settings_.printSolverStatus || settings_.printLinesearch) {
    std::cerr << "\n++++++++++++++++++++++++++++++++++++++++++++++++++++++";
    std::cerr << "\n+++++++++++++ SQP solver is initialized ++++++++++++++";
    std::cerr << "\n++++++++++++++++++++++++++++++++++++++++++++++++++++++\n";
  }

  // Determine time discretization, taking into account event times.
  const auto timeDiscretization = timeDiscretizationWithEvents(initTime, finalTime, settings_.dt, this->getModeSchedule().eventTimes);

  // Initialize the state and input
  vector_array_t x = initializeStateTrajectory(initState, timeDiscretization);
  vector_array_t u = initializeInputTrajectory(timeDiscretization, x);

  // Initialize cost
  for (auto& cost : costFunctionPtr_) {
    cost->setCostDesiredTrajectoriesPtr(&this->getCostDesiredTrajectories());
  }
  if (terminalCostFunctionPtr_) {
    terminalCostFunctionPtr_->setCostDesiredTrajectoriesPtr(&this->getCostDesiredTrajectories());
  }

  // Bookkeeping
  performanceIndeces_.clear();

  for (int iter = 0; iter < settings_.sqpIteration; iter++) {
    if (settings_.printSolverStatus || settings_.printLinesearch) {
      std::cerr << "\nSQP iteration: " << iter << "\n";
    }
    // Make QP approximation
    linearQuadraticApproximationTimer_.startTimer();
    performanceIndeces_.emplace_back(setupQuadraticSubproblem(timeDiscretization, initState, x, u));
    linearQuadraticApproximationTimer_.endTimer();

    // Solve QP
    solveQpTimer_.startTimer();
    const vector_t delta_x0 = initState - x[0];
    const auto deltaSolution = getOCPSolution(delta_x0);
    solveQpTimer_.endTimer();

    // Apply step
    linesearchTimer_.startTimer();
    bool converged = takeStep(performanceIndeces_.back(), timeDiscretization, initState, deltaSolution.first, deltaSolution.second, x, u);
    linesearchTimer_.endTimer();

    totalNumIterations_++;
    if (converged) {
      break;
    }
  }

  computeControllerTimer_.startTimer();
  setPrimalSolution(timeDiscretization, std::move(x), std::move(u));
  computeControllerTimer_.endTimer();

  if (settings_.printSolverStatus || settings_.printLinesearch) {
    std::cerr << "\n++++++++++++++++++++++++++++++++++++++++++++++++++++++";
    std::cerr << "\n+++++++++++++ SQP solver has terminated ++++++++++++++";
    std::cerr << "\n++++++++++++++++++++++++++++++++++++++++++++++++++++++\n";
  }
}

void MultipleShootingSolver::runParallel(std::function<void(int)> taskFunction) {
  // Launch tasks in helper threads
  std::vector<std::future<void>> futures;
  if (threadPoolPtr_) {
    int numHelpers = settings_.nThreads - 1;
    futures.reserve(numHelpers);
    for (int i = 0; i < numHelpers; i++) {
      futures.emplace_back(threadPoolPtr_->run(taskFunction));
    }
  }
  // Execute one instance in this thread.
  const int workerId = settings_.nThreads - 1;  // threadpool uses 0 -> n-2
  taskFunction(workerId);

  // Wait for helpers to finish.
  for (auto&& fut : futures) {
    fut.get();
  }
}

vector_array_t MultipleShootingSolver::initializeInputTrajectory(const std::vector<AnnotatedTime>& timeDiscretization,
                                                                 const vector_array_t& stateTrajectory) const {
  const int N = static_cast<int>(timeDiscretization.size()) - 1;
  const scalar_t interpolateTill = (totalNumIterations_ > 0) ? primalSolution_.timeTrajectory_.back() : timeDiscretization.front().time;

  vector_array_t u;
  u.reserve(N);
  for (int i = 0; i < N; i++) {
    const scalar_t ti = getInterpolationTime(timeDiscretization[i]);
    if (ti < interpolateTill) {
      // Interpolate previous input trajectory
      u.emplace_back(primalSolution_.controllerPtr_->computeInput(ti, stateTrajectory[i]));
    } else {
      // No previous control at this time-point -> fall back to heuristics
      // Ask for operating trajectory between t[k] and t[k+1]. Take the returned input at t[k] as our heuristic.
      const scalar_t tNext = getIntervalEnd(timeDiscretization[i + 1]);
      scalar_array_t timeArray;
      vector_array_t stateArray;
      vector_array_t inputArray;
      operatingTrajectoriesPtr_->getSystemOperatingTrajectories(stateTrajectory[i], ti, tNext, timeArray, stateArray, inputArray, false);
      u.push_back(std::move(inputArray.front()));
    }
  }

  return u;
}

vector_array_t MultipleShootingSolver::initializeStateTrajectory(const vector_t& initState,
                                                                 const std::vector<AnnotatedTime>& timeDiscretization) const {
  const int trajectoryLength = static_cast<int>(timeDiscretization.size());
  if (totalNumIterations_ == 0) {  // first iteration
    return vector_array_t(trajectoryLength, initState);
  } else {  // interpolation of previous solution
    vector_array_t x;
    x.reserve(trajectoryLength);
    x.push_back(initState);  // Force linearization of the first node around the current state
    for (int i = 1; i < trajectoryLength; i++) {
      const scalar_t ti = getInterpolationTime(timeDiscretization[i]);
      x.emplace_back(LinearInterpolation::interpolate(ti, primalSolution_.timeTrajectory_, primalSolution_.stateTrajectory_));
    }
    return x;
  }
}

std::pair<vector_array_t, vector_array_t> MultipleShootingSolver::getOCPSolution(const vector_t& delta_x0) {
  // Solve the QP
  vector_array_t deltaXSol;
  vector_array_t deltaUSol;
  hpipm_status status;
  if (constraintPtr_.front() && !settings_.projectStateInputEqualityConstraints) {
    hpipmInterface_.resize(hpipm_interface::extractSizesFromProblem(dynamics_, cost_, &constraints_));
    status = hpipmInterface_.solve(delta_x0, dynamics_, cost_, &constraints_, deltaXSol, deltaUSol, settings_.printSolverStatus);
  } else {  // without constraints, or when using projection, we have an unconstrained QP.
    hpipmInterface_.resize(hpipm_interface::extractSizesFromProblem(dynamics_, cost_, nullptr));
    status = hpipmInterface_.solve(delta_x0, dynamics_, cost_, nullptr, deltaXSol, deltaUSol, settings_.printSolverStatus);
  }

  if (status != hpipm_status::SUCCESS) {
    throw std::runtime_error("[MultipleShootingSolver] Failed to solve QP");
  }

  // remap the tilde delta u to real delta u
  if (settings_.projectStateInputEqualityConstraints) {
    vector_t tmp;  // 1 temporary for re-use.
    for (int i = 0; i < deltaUSol.size(); i++) {
      if (constraintsProjection_[i].f.size() > 0) {
        tmp.noalias() = constraintsProjection_[i].dfdu * deltaUSol[i];
        deltaUSol[i] = tmp + constraintsProjection_[i].f;
        deltaUSol[i].noalias() += constraintsProjection_[i].dfdx * deltaXSol[i];
      }
    }
  }

  return {deltaXSol, deltaUSol};
}

void MultipleShootingSolver::setPrimalSolution(const std::vector<AnnotatedTime>& time, vector_array_t&& x, vector_array_t&& u) {
  // Clear old solution
  primalSolution_ = PrimalSolution();

  // Compute feedback, before x and u are moved to primal solution
  vector_array_t uff;
  matrix_array_t controllerGain;
  if (settings_.useFeedbackPolicy) {
    // see doc/LQR_full.pdf for detailed derivation for feedback terms
    uff.reserve(time.size());
    controllerGain.reserve(time.size());
    matrix_array_t KMatrices = hpipmInterface_.getRiccatiFeedback(dynamics_[0], cost_[0]);
    for (int i = 0; (i + 1) < time.size(); i++) {
      if (time[i].event == AnnotatedTime::Event::PreEvent && !uff.empty()) {
        // Correct for missing inputs at PreEvents
        uff.push_back(uff.back());
        controllerGain.push_back(controllerGain.back());
      } else {
        // Linear controller has convention u = uff + K * x;
        // We computed u = u'(t) + K (x - x'(t));
        // >> uff = u'(t) - K x'(t)
        if (constraintsProjection_[i].f.size() > 0) {
          controllerGain.push_back(std::move(constraintsProjection_[i].dfdx));  // Steal! Don't use after this.
          controllerGain.back().noalias() += constraintsProjection_[i].dfdu * KMatrices[i];
        } else {
          controllerGain.push_back(std::move(KMatrices[i]));
        }
        uff.push_back(u[i]);
        uff.back().noalias() -= controllerGain.back() * x[i];
      }
    }
    // Copy last one to get correct length
    uff.push_back(uff.back());
    controllerGain.push_back(controllerGain.back());
  }

  // Construct nominal state and inputs
  u.push_back(u.back());  // Repeat last input to make equal length vectors
  primalSolution_.stateTrajectory_ = std::move(x);
  primalSolution_.inputTrajectory_ = std::move(u);
  for (int i = 0; i < time.size(); ++i) {
    primalSolution_.timeTrajectory_.push_back(time[i].time);
    if (time[i].event == AnnotatedTime::Event::PreEvent && i > 0) {
      // Correct for missing inputs at PreEvents
      primalSolution_.inputTrajectory_[i] = primalSolution_.inputTrajectory_[i - 1];
    }
  }
  primalSolution_.modeSchedule_ = this->getModeSchedule();

  // Assign controller
  if (settings_.useFeedbackPolicy) {
    primalSolution_.controllerPtr_.reset(new LinearController(primalSolution_.timeTrajectory_, std::move(uff), std::move(controllerGain)));
  } else {
    primalSolution_.controllerPtr_.reset(new FeedforwardController(primalSolution_.timeTrajectory_, primalSolution_.inputTrajectory_));
  }
}

PerformanceIndex MultipleShootingSolver::setupQuadraticSubproblem(const std::vector<AnnotatedTime>& time, const vector_t& initState,
                                                                  const vector_array_t& x, const vector_array_t& u) {
  // Problem horizon
  const int N = static_cast<int>(time.size()) - 1;

  std::vector<PerformanceIndex> performance(settings_.nThreads, PerformanceIndex());
  dynamics_.resize(N);
  cost_.resize(N + 1);
  constraints_.resize(N + 1);
  constraintsProjection_.resize(N);

  std::atomic_int timeIndex{0};
  auto parallelTask = [&](int workerId) {
    // Get worker specific resources
    SystemDynamicsBase& systemDynamics = *systemDynamicsPtr_[workerId];
    CostFunctionBase& costFunction = *costFunctionPtr_[workerId];
    ConstraintBase* constraintPtr = constraintPtr_[workerId].get();
    PerformanceIndex workerPerformance;  // Accumulate performance in local variable
    const bool projection = settings_.projectStateInputEqualityConstraints;

    int i = timeIndex++;
    while (i < N) {
      if (time[i].event == AnnotatedTime::Event::PreEvent) {
        // Event node
        auto result = multiple_shooting::setupEventNode(systemDynamics, nullptr, nullptr, time[i].time, x[i], x[i + 1]);
        workerPerformance += result.performance;
        dynamics_[i] = std::move(result.dynamics);
        cost_[i] = std::move(result.cost);
        constraints_[i] = std::move(result.constraints);
        constraintsProjection_[i] = VectorFunctionLinearApproximation::Zero(0, x[i].size(), 0);
      } else {
        // Normal, intermediate node
        const scalar_t ti = getIntervalStart(time[i]);
        const scalar_t dt = getIntervalDuration(time[i], time[i + 1]);
        auto result = multiple_shooting::setupIntermediateNode(systemDynamics, sensitivityDiscretizer_, costFunction, constraintPtr,
                                                               penaltyPtr_.get(), projection, ti, dt, x[i], x[i + 1], u[i]);
        workerPerformance += result.performance;
        dynamics_[i] = std::move(result.dynamics);
        cost_[i] = std::move(result.cost);
        constraints_[i] = std::move(result.constraints);
        constraintsProjection_[i] = std::move(result.constraintsProjection);
      }

      i = timeIndex++;
    }

    if (i == N) {  // Only one worker will execute this
      const scalar_t tN = getIntervalStart(time[N]);
      auto result = multiple_shooting::setupTerminalNode(terminalCostFunctionPtr_.get(), constraintPtr, tN, x[N]);
      workerPerformance += result.performance;
      cost_[i] = std::move(result.cost);
      constraints_[i] = std::move(result.constraints);
    }

    // Accumulate! Same worker might run multiple tasks
    performance[workerId] += workerPerformance;
  };
  runParallel(parallelTask);

  // Account for init state in performance
  performance.front().stateEqConstraintISE += (initState - x.front()).squaredNorm();

  // Sum performance of the threads
  PerformanceIndex totalPerformance = std::accumulate(std::next(performance.begin()), performance.end(), performance.front());
  totalPerformance.merit = totalPerformance.totalCost + totalPerformance.inequalityConstraintPenalty;
  return totalPerformance;
}

PerformanceIndex MultipleShootingSolver::computePerformance(const std::vector<AnnotatedTime>& time, const vector_t& initState,
                                                            const vector_array_t& x, const vector_array_t& u) {
  // Problem horizon
  const int N = static_cast<int>(time.size()) - 1;

  std::vector<PerformanceIndex> performance(settings_.nThreads, PerformanceIndex());
  std::atomic_int timeIndex{0};
  auto parallelTask = [&](int workerId) {
    // Get worker specific resources
    SystemDynamicsBase& systemDynamics = *systemDynamicsPtr_[workerId];
    CostFunctionBase& costFunction = *costFunctionPtr_[workerId];
    ConstraintBase* constraintPtr = constraintPtr_[workerId].get();
    PerformanceIndex workerPerformance;  // Accumulate performance in local variable

    int i = timeIndex++;
    while (i < N) {
      if (time[i].event == AnnotatedTime::Event::PreEvent) {
        // Event node
        workerPerformance += multiple_shooting::computeEventPerformance(systemDynamics, nullptr, nullptr, time[i].time, x[i], x[i + 1]);
      } else {
        // Normal, intermediate node
        const scalar_t ti = getIntervalStart(time[i]);
        const scalar_t dt = getIntervalDuration(time[i], time[i + 1]);
        workerPerformance += multiple_shooting::computeIntermediatePerformance(systemDynamics, discretizer_, costFunction, constraintPtr,
                                                                               penaltyPtr_.get(), ti, dt, x[i], x[i + 1], u[i]);
      }

      i = timeIndex++;
    }

    if (i == N) {  // Only one worker will execute this
      const scalar_t tN = getIntervalStart(time[N]);
      workerPerformance += multiple_shooting::computeTerminalPerformance(terminalCostFunctionPtr_.get(), constraintPtr, tN, x[N]);
    }

    // Accumulate! Same worker might run multiple tasks
    performance[workerId] += workerPerformance;
  };
  runParallel(parallelTask);

  // Account for init state in performance
  performance.front().stateEqConstraintISE += (initState - x.front()).squaredNorm();

  // Sum performance of the threads
  PerformanceIndex totalPerformance = std::accumulate(std::next(performance.begin()), performance.end(), performance.front());
  totalPerformance.merit = totalPerformance.totalCost + totalPerformance.inequalityConstraintPenalty;
  return totalPerformance;
}

scalar_t MultipleShootingSolver::trajectoryNorm(const vector_array_t& v) {
  scalar_t norm = 0.0;
  for (const auto& vi : v) {
    norm += vi.squaredNorm();
  }
  return std::sqrt(norm);
}

bool MultipleShootingSolver::takeStep(const PerformanceIndex& baseline, const std::vector<AnnotatedTime>& timeDiscretization,
                                      const vector_t& initState, const vector_array_t& dx, const vector_array_t& du, vector_array_t& x,
                                      vector_array_t& u) {
  /*
   * Filter linesearch based on:
   * "On the implementation of an interior-point filter line-search algorithm for large-scale nonlinear programming"
   * https://link.springer.com/article/10.1007/s10107-004-0559-y
   */
  if (settings_.printLinesearch) {
    std::cerr << std::setprecision(9) << std::fixed;
    std::cerr << "\n=== Linesearch ===\n";
    std::cerr << "Baseline:\n";
    std::cerr << "\tMerit: " << baseline.merit << "\t DynamicsISE: " << baseline.stateEqConstraintISE
              << "\t StateInputISE: " << baseline.stateInputEqConstraintISE << "\t IneqISE: " << baseline.inequalityConstraintISE
              << "\t Penalty: " << baseline.inequalityConstraintPenalty << "\n";
  }

  // Some settings
  const scalar_t alpha_decay = settings_.alpha_decay;
  const scalar_t alpha_min = settings_.alpha_min;
  const scalar_t gamma_c = settings_.gamma_c;
  const scalar_t g_max = settings_.g_max;
  const scalar_t g_min = settings_.g_min;
  const scalar_t costTol = settings_.costTol;

  // Total Constraint violation function
  auto constraintViolation = [](const PerformanceIndex& performance) -> scalar_t {
    return std::sqrt(performance.stateEqConstraintISE + performance.stateInputEqConstraintISE + performance.inequalityConstraintISE);
  };

  const scalar_t baselineConstraintViolation = constraintViolation(baseline);

  // Update norm
  const scalar_t deltaUnorm = trajectoryNorm(du);
  const scalar_t deltaXnorm = trajectoryNorm(dx);

  scalar_t alpha = 1.0;
  vector_array_t xNew(x.size());
  vector_array_t uNew(u.size());
  do {
    // Compute step
    for (int i = 0; i < u.size(); i++) {
      if (du[i].size() > 0) {  // account for absence of inputs at events.
        uNew[i] = u[i] + alpha * du[i];
      }
    }
    for (int i = 0; i < x.size(); i++) {
      xNew[i] = x[i] + alpha * dx[i];
    }

    // Compute cost and constraints
    const PerformanceIndex performanceNew = computePerformance(timeDiscretization, initState, xNew, uNew);
    const scalar_t newConstraintViolation = constraintViolation(performanceNew);

    const bool stepAccepted = [&]() {
      if (newConstraintViolation > g_max) {
        return false;
      } else if (newConstraintViolation < g_min) {
        // With low violation only care about cost, reference paper implements here armijo condition
        return (performanceNew.merit < baseline.merit);
      } else {
        // Medium violation: either merit or constraints decrease (with small gamma_c mixing of old constraints)
        return performanceNew.merit < (baseline.merit - gamma_c * baselineConstraintViolation) ||
               newConstraintViolation < ((1.0 - gamma_c) * baselineConstraintViolation);
      }
    }();

    if (settings_.printLinesearch) {
      std::cerr << "Stepsize = " << alpha << (stepAccepted ? std::string{" (Accepted)"} : std::string{" (Rejected)"}) << "\n";
      std::cerr << "|dx| = " << alpha * deltaXnorm << "\t|du| = " << alpha * deltaUnorm << "\n";
      std::cerr << "\tMerit: " << performanceNew.merit << "\t DynamicsISE: " << performanceNew.stateEqConstraintISE
                << "\t StateInputISE: " << performanceNew.stateInputEqConstraintISE
                << "\t IneqISE: " << performanceNew.inequalityConstraintISE << "\t Penalty: " << performanceNew.inequalityConstraintPenalty
                << "\n";
    }

    // Exit conditions
    const bool stepSizeBelowTol = alpha * deltaUnorm < settings_.deltaTol && alpha * deltaXnorm < settings_.deltaTol;

    if (stepAccepted) {  // Return if step accepted
      x = std::move(xNew);
      u = std::move(uNew);
      const bool improvementBelowTol = std::abs(baseline.merit - performanceNew.merit) < costTol && newConstraintViolation < g_min;
      return stepSizeBelowTol || improvementBelowTol;
    } else if (stepSizeBelowTol) {  // Return if steps get too small without being accepted
      if (settings_.printLinesearch) {
        std::cerr << "Stepsize is smaller than provided deltaTol -> converged \n";
      }
      return true;
    } else {  // Try smaller step
      alpha *= alpha_decay;
    }
  } while (alpha > alpha_min);

  return true;  // Alpha_min reached and no improvement found -> Converged
}

}  // namespace ocs2