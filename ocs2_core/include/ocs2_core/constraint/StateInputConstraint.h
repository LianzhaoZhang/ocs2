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

#pragma once

#include <type_traits>

#include <ocs2_core/Types.h>

namespace ocs2 {

/** State-input constraint function base class */
class StateInputConstraint {
 public:
  StateInputConstraint() = default;
  virtual ~StateInputConstraint() = default;
  virtual StateInputConstraint* clone() const = 0;

  /** Set constraint activity */
  void setActivity(bool activity) { active_ = activity; }

  /** Check constraint activity */
  bool isActive() const { return active_; }

  /** Get the size of the constraint vector at given time */
  virtual size_t getNumConstraints(scalar_t time) const = 0;

  /** Get the constraint vector value */
  virtual vector_t getValue(scalar_t time, const vector_t& state, const vector_t& input) const = 0;

  /** Get the constraint linear approximation */
  virtual VectorFunctionLinearApproximation getLinearApproximation(scalar_t time, const vector_t& state, const vector_t& input) const {
    throw std::runtime_error("[StateInputConstraint] Linear approximation not implemented");
  }

  /** Get the constraint quadratic approximation */
  virtual VectorFunctionQuadraticApproximation getQuadraticApproximation(scalar_t time, const vector_t& state,
                                                                         const vector_t& input) const {
    throw std::runtime_error("[StateInputConstraint] Quadratic approximation not implemented");
  }

 protected:
  StateInputConstraint(const StateInputConstraint& rhs) = default;

 private:
  bool active_ = true;
};

// Template for conditional compilation using SFINAE
template <typename T>
using EnableIfStateInputConstraint_t = typename std::enable_if<std::is_same<T, StateInputConstraint>::value, bool>::type;

}  // namespace ocs2