//
// Copyright (c) 2018, University of Edinburgh
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of  nor the names of its contributors may be used to
//    endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

#include <exotica_core/problems/bounded_end_pose_problem.h>
#include <exotica_core/setup.h>

#include <exotica_core/task_initializer.h>

REGISTER_PROBLEM_TYPE("BoundedEndPoseProblem", exotica::BoundedEndPoseProblem)

namespace exotica
{
BoundedEndPoseProblem::BoundedEndPoseProblem()
{
    flags_ = KIN_FK | KIN_J;
}

BoundedEndPoseProblem::~BoundedEndPoseProblem() = default;

Eigen::MatrixXd BoundedEndPoseProblem::GetBounds() const
{
    return scene_->GetKinematicTree().GetJointLimits();
}

void BoundedEndPoseProblem::Instantiate(BoundedEndPoseProblemInitializer& init)
{
    num_tasks = tasks_.size();
    length_Phi = 0;
    length_jacobian = 0;
    for (int i = 0; i < num_tasks; ++i)
    {
        AppendVector(Phi.map, tasks_[i]->GetLieGroupIndices());
        length_Phi += tasks_[i]->length;
        length_jacobian += tasks_[i]->length_jacobian;
    }
    Phi.SetZero(length_Phi);
    W = Eigen::MatrixXd::Identity(N, N);
    if (init.W.rows() > 0)
    {
        if (init.W.rows() == N)
        {
            W.diagonal() = init.W;
        }
        else
        {
            ThrowNamed("W dimension mismatch! Expected " << N << ", got " << init.W.rows());
        }
    }
    if (flags_ & KIN_J) jacobian = Eigen::MatrixXd(length_jacobian, N);
    if (flags_ & KIN_J_DOT) hessian.setConstant(length_jacobian, Eigen::MatrixXd::Zero(N, N));

    if (init.LowerBound.rows() == N)
    {
        scene_->GetKinematicTree().SetJointLimitsLower(init.LowerBound);
    }
    else if (init.LowerBound.rows() != 0)
    {
        ThrowNamed("Lower bound size incorrect! Expected " << N << " got " << init.LowerBound.rows());
    }
    if (init.UpperBound.rows() == N)
    {
        scene_->GetKinematicTree().SetJointLimitsUpper(init.UpperBound);
    }
    else if (init.UpperBound.rows() != 0)
    {
        ThrowNamed("Lower bound size incorrect! Expected " << N << " got " << init.UpperBound.rows());
    }

    TaskSpaceVector dummy;
    cost.Initialize(init.Cost, shared_from_this(), dummy);
    ApplyStartState(false);
    PreUpdate();
}

void BoundedEndPoseProblem::PreUpdate()
{
    PlanningProblem::PreUpdate();
    for (int i = 0; i < tasks_.size(); ++i) tasks_[i]->is_used = false;
    cost.UpdateS();
}

double BoundedEndPoseProblem::GetScalarCost()
{
    return cost.ydiff.transpose() * cost.S * cost.ydiff;
}

Eigen::VectorXd BoundedEndPoseProblem::GetScalarJacobian()
{
    return cost.jacobian.transpose() * cost.S * cost.ydiff * 2.0;
}

double BoundedEndPoseProblem::GetScalarTaskCost(const std::string& task_name)
{
    for (int i = 0; i < cost.indexing.size(); ++i)
    {
        if (cost.tasks[i]->GetObjectName() == task_name)
        {
            return cost.ydiff.segment(cost.indexing[i].start, cost.indexing[i].length).transpose() * cost.rho(cost.indexing[i].id) * cost.ydiff.segment(cost.indexing[i].start, cost.indexing[i].length);
        }
    }
    ThrowPretty("Cannot get scalar task cost. Task map '" << task_name << "' does not exist.");
}

void BoundedEndPoseProblem::Update(Eigen::VectorXdRefConst x)
{
    scene_->Update(x, t_start);
    Phi.SetZero(length_Phi);
    if (flags_ & KIN_J) jacobian.setZero();
    if (flags_ & KIN_J_DOT)
        for (int i = 0; i < length_jacobian; ++i) hessian(i).setZero();
    for (int i = 0; i < tasks_.size(); ++i)
    {
        if (tasks_[i]->is_used)
        {
            if (flags_ & KIN_J_DOT)
            {
                tasks_[i]->Update(x, Phi.data.segment(tasks_[i]->start, tasks_[i]->length), jacobian.middleRows(tasks_[i]->start_jacobian, tasks_[i]->length_jacobian), hessian.segment(tasks_[i]->start, tasks_[i]->length));
            }
            else if (flags_ & KIN_J)
            {
                tasks_[i]->Update(x, Phi.data.segment(tasks_[i]->start, tasks_[i]->length), jacobian.middleRows(tasks_[i]->start_jacobian, tasks_[i]->length_jacobian));
            }
            else
            {
                tasks_[i]->Update(x, Phi.data.segment(tasks_[i]->start, tasks_[i]->length));
            }
        }
    }
    if (flags_ & KIN_J_DOT)
    {
        cost.Update(Phi, jacobian, hessian);
    }
    else if (flags_ & KIN_J)
    {
        cost.Update(Phi, jacobian);
    }
    else
    {
        cost.Update(Phi);
    }
    ++number_of_problem_updates_;
}

void BoundedEndPoseProblem::SetGoal(const std::string& task_name, Eigen::VectorXdRefConst goal)
{
    for (int i = 0; i < cost.indexing.size(); ++i)
    {
        if (cost.tasks[i]->GetObjectName() == task_name)
        {
            if (goal.rows() != cost.indexing[i].length) ThrowPretty("Expected length of " << cost.indexing[i].length << " and got " << goal.rows());
            cost.y.data.segment(cost.indexing[i].start, cost.indexing[i].length) = goal;
            return;
        }
    }
    ThrowPretty("Cannot set Goal. Task map '" << task_name << "' does not exist.");
}

void BoundedEndPoseProblem::SetRho(const std::string& task_name, const double& rho)
{
    for (int i = 0; i < cost.indexing.size(); ++i)
    {
        if (cost.tasks[i]->GetObjectName() == task_name)
        {
            cost.rho(cost.indexing[i].id) = rho;
            PreUpdate();
            return;
        }
    }
    ThrowPretty("Cannot set rho. Task map '" << task_name << "' does not exist.");
}

Eigen::VectorXd BoundedEndPoseProblem::GetGoal(const std::string& task_name)
{
    for (int i = 0; i < cost.indexing.size(); ++i)
    {
        if (cost.tasks[i]->GetObjectName() == task_name)
        {
            return cost.y.data.segment(cost.indexing[i].start, cost.indexing[i].length);
        }
    }
    ThrowPretty("Cannot get Goal. Task map '" << task_name << "' does not exist.");
}

double BoundedEndPoseProblem::GetRho(const std::string& task_name)
{
    for (int i = 0; i < cost.indexing.size(); ++i)
    {
        if (cost.tasks[i]->GetObjectName() == task_name)
        {
            return cost.rho(cost.indexing[i].id);
        }
    }
    ThrowPretty("Cannot get rho. Task map '" << task_name << "' does not exist.");
}

bool BoundedEndPoseProblem::IsValid()
{
    Eigen::VectorXd x = scene_->GetKinematicTree().GetControlledState();
    Eigen::MatrixXd bounds = scene_->GetKinematicTree().GetJointLimits();
    for (unsigned int i = 0; i < N; ++i)
    {
        if (x(i) < bounds(i, 0) || x(i) > bounds(i, 1)) return false;
    }
    return true;
}
}
