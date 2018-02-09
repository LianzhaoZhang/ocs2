/*
 * ComKinoDynamicsDerivativeBase.h
 *
 *  Created on: Nov 12, 2017
 *      Author: farbod
 */

#ifndef COMKINODYNAMICSDERIVATIVEBASE_H_
#define COMKINODYNAMICSDERIVATIVEBASE_H_

#include <array>
#include <memory>
#include <iostream>
#include <string>
#include <Eigen/Dense>

#include <ocs2_core/dynamics/DerivativesBase.h>

#include "c_switched_model_interface/core/SwitchedModel.h"
#include "c_switched_model_interface/core/KinematicsModelBase.h"
#include "c_switched_model_interface/core/ComModelBase.h"
#include "ComDynamicsDerivativeBase.h"
#include "c_switched_model_interface/logic/SwitchedModelLogicRulesBase.h"
#include "c_switched_model_interface/state_constraint/EndEffectorConstraintBase.h"
#include <c_switched_model_interface/core/Options.h>

namespace switched_model {

template <size_t JOINT_COORD_SIZE>
class ComKinoDynamicsDerivativeBase : public ocs2::DerivativesBase<12+JOINT_COORD_SIZE, 12+JOINT_COORD_SIZE, SwitchedModelLogicRulesBase<JOINT_COORD_SIZE>>
{
public:
	EIGEN_MAKE_ALIGNED_OPERATOR_NEW

	enum
	{
		STATE_DIM = 12+JOINT_COORD_SIZE,
		INPUT_DIM = 12+JOINT_COORD_SIZE
	};

	typedef SwitchedModelLogicRulesBase<JOINT_COORD_SIZE> logic_rules_t;
	typedef ocs2::LogicRulesMachine<STATE_DIM, INPUT_DIM, logic_rules_t> logic_rules_machine_t;

	typedef ocs2::DerivativesBase<STATE_DIM, INPUT_DIM, logic_rules_t> Base;

	typedef ComModelBase<JOINT_COORD_SIZE> com_model_t;
	typedef KinematicsModelBase<JOINT_COORD_SIZE> kinematic_model_t;

	typedef typename Base::scalar_t scalar_t;
	typedef typename Base::state_matrix_t state_matrix_t;
	typedef typename Base::state_vector_t state_vector_t;
	typedef typename Base::input_vector_t input_vector_t;
	typedef typename Base::control_gain_matrix_t control_gain_matrix_t;
	typedef typename Base::constraint1_state_matrix_t constraint1_state_matrix_t;
	typedef typename Base::constraint1_control_matrix_t constraint1_control_matrix_t;
	typedef typename Base::constraint2_state_matrix_t constraint2_state_matrix_t;

	typedef typename SwitchedModel<JOINT_COORD_SIZE>::base_coordinate_t  base_coordinate_t;
	typedef typename SwitchedModel<JOINT_COORD_SIZE>::joint_coordinate_t joint_coordinate_t;
	typedef Eigen::Matrix<double,6,JOINT_COORD_SIZE> base_jacobian_matrix_t;


	ComKinoDynamicsDerivativeBase(const kinematic_model_t* kinematicModelPtr, const com_model_t* comModelPtr,
			const scalar_t& gravitationalAcceleration=9.81, const Options& options = Options())

	: Base(),
	  kinematicModelPtr_(kinematicModelPtr->clone()),
	  comModelPtr_(comModelPtr->clone()),
	  o_gravityVector_(0.0, 0.0, -gravitationalAcceleration),
	  options_(options),
	  comDynamicsDerivative_(kinematicModelPtr, comModelPtr, gravitationalAcceleration, options.constrainedIntegration_)
	{
		if (gravitationalAcceleration < 0)
			throw std::runtime_error("Gravitational acceleration should be a positive value.");
	}

	/**
	 * copy construntor of ComKinoDynamicsDerivativeBase
	 */
	ComKinoDynamicsDerivativeBase(const ComKinoDynamicsDerivativeBase& rhs)

	: Base(rhs),
	  kinematicModelPtr_(rhs.kinematicModelPtr_->clone()),
	  comModelPtr_(rhs.comModelPtr_->clone()),
	  o_gravityVector_(rhs.o_gravityVector_),
	  options_(rhs.options_),
	  comDynamicsDerivative_(rhs.comDynamicsDerivative_)
	{}

	virtual ~ComKinoDynamicsDerivativeBase() {}

	/**
	 * clone ComKinoDynamicsDerivativeBase class.
	 */
	virtual ComKinoDynamicsDerivativeBase<JOINT_COORD_SIZE>* clone() const override;

	/**
	 * Initializes the system dynamics. This method should always be called at the very first call of the model.
	 *
	 * @param [in] logicRulesMachine: A class which contains and parse the logic rules e.g
	 * method findActiveSubsystemHandle returns a Lambda expression which can be used to
	 * find the ID of the current active subsystem.
	 * @param [in] partitionIndex: index of the time partition.
	 * @param [in] algorithmName: The algorithm that class this class (default not defined).
	 */
	virtual void initializeModel(const logic_rules_machine_t& logicRulesMachine,
			const size_t& partitionIndex, const char* algorithmName=NULL) override;

	/**
	 * Set the current state and contact force input
	 *
	 * @param t: current time
	 * @param x: current switched state vector (centrodial dynamics plus joints' angles)
	 * @param u: current switched input vector (contact forces plus joints' velocities)
	 */
	virtual void setCurrentStateAndControl(const scalar_t& t, const state_vector_t& x, const input_vector_t& u) override;

	/**
	 * calculate and retrieve the A matrix (i.e. the state derivative of the dynamics w.r.t. state vector).
	 *
	 * @param A: a nx-by-nx matrix
	 */
	virtual void getDerivativeState(state_matrix_t& A)  override;

	/**
	 * calculate and retrieve the B matrix (i.e. the state derivative of the dynamics w.r.t. input vector).
	 *
	 * @param B: a ny-by-nu matrix
	 */
	virtual void getDerivativesControl(control_gain_matrix_t& B)  override;

	/**
	 * set the stance legs
	 */
	void setStanceLegs (const std::array<bool,4>& stanceLegs);

	/**
	 * get the model's stance leg
	 */
	void getStanceLegs (std::array<bool,4>& stanceLegs) const;


private:

	typename kinematic_model_t::Ptr kinematicModelPtr_;
	typename com_model_t::Ptr comModelPtr_;
	Eigen::Vector3d   o_gravityVector_;
	Options options_;

	ComDynamicsDerivativeBase<JOINT_COORD_SIZE> comDynamicsDerivative_;

	const logic_rules_t* logicRulesPtr_;

	std::function<size_t(scalar_t)> findActiveSubsystemFnc_;

	std::array<bool,4> stanceLegs_;

	joint_coordinate_t qJoints_;
	joint_coordinate_t dqJoints_;
	base_coordinate_t basePose_;
	base_coordinate_t baseLocalVelocities_;

	Eigen::Matrix3d o_R_b_;

	Eigen::Vector3d com_base2CoM_;
	base_jacobian_matrix_t b_comJacobain_;
	base_jacobian_matrix_t b_comJacobainTimeDerivative_;
	std::array<Eigen::Vector3d,4> com_com2StanceFeet_;

	std::array<bool,4> feetConstraintIsActive_;
	std::array<Eigen::Vector3d,4> feetConstraintJacobains_;

	std::array<base_jacobian_matrix_t,4> b_feetJacobains_;

	std::string algorithmName_;
};

} // end of namespace switched_model

#include "implementation/ComKinoDynamicsDerivativeBase.h"

#endif /* COMKINODYNAMICSDERIVATIVEBASE_H_ */
