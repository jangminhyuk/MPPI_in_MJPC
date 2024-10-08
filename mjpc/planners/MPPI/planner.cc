// Copyright 2022 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mjpc/planners/MPPI/planner.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <cmath>
#include <fstream>

#include <absl/random/random.h>
#include <mujoco/mujoco.h>
#include "mjpc/array_safety.h"
#include "mjpc/planners/MPPI/policy.h"
#include "mjpc/states/state.h"
#include "mjpc/trajectory.h"
#include "mjpc/utilities.h"
#include <filesystem>
namespace mjpc {

namespace mju = ::mujoco::util_mjpc;

// initialize data and settings
void MPPIPlanner::Initialize(mjModel* model, const Task& task) {
  // delete mjData instances since model might have changed.
  data_.clear();
  // allocate one mjData for nominal.
  ResizeMjData(model, 1);

  // model
  this->model = model;

  // task
  this->task = &task;

  // rollout parameters
  timestep_power = 1.0;

  // sampling noise
  noise_exploration = GetNumberOrDefault(0.1, model, "sampling_exploration");

  // set number of trajectories to rollout
  num_trajectory_ = GetNumberOrDefault(10, model, "sampling_trajectories");

  // set splines num
  num_spline_points_ = GetNumberOrDefault(10, model, "sampling_spline_points");

  lambda = GetNumberOrDefault(0.1, model, "MPPI lambda");

  gamma = GetNumberOrDefault(1, model, "MPPI gamma");

  alpha_mu = GetNumberOrDefault(1, model, "MPPI alpha_mu");

  alpha_sig = GetNumberOrDefault(0, model, "MPPI alpha_sig");

  lam_coeff = GetNumberOrDefault(5, model, "lambda coefficient");
  lam_power = GetNumberOrDefault(-2, model, "lambda power");
  cov_weight = GetNumberOrDefault(1, model, "Lambda objective tuning");

  if (num_trajectory_ > kMaxTrajectory) {
    mju_error_i("Too many trajectories, %d is the maximum allowed.",
                kMaxTrajectory);
  }

  //std::cout<<"hi!!"<<std::endl;

  previous_avg_return = 0.1;
  current_avg_return = 0.1;
  lambda_choice = 1;
  moving_avg = 0.1;
  moving_variance = 0.1;
  lambda_objective = 0.1;

  winner = 0;

}

// allocate memory
void MPPIPlanner::Allocate() {
  // initial state
  int num_state = model->nq + model->nv + model->na;

  // state
  state.resize(num_state);
  mocap.resize(7 * model->nmocap);
  userdata.resize(model->nuserdata);

  // policy
  int num_max_parameter = model->nu * kMaxTrajectoryHorizon;
  policy.Allocate(model, *task, kMaxTrajectoryHorizon);
  previous_policy.Allocate(model, *task, kMaxTrajectoryHorizon);

  // scratch
  parameters_scratch.resize(num_max_parameter);
  cov_parameters_scratch.resize(num_max_parameter);
  times_scratch.resize(kMaxTrajectoryHorizon);
  tmp_params_scratch.resize(num_max_parameter);
  tmp_times_scratch.resize(kMaxTrajectoryHorizon);
  importance_weight.resize(kMaxTrajectory);
  // noise
  noise.resize(kMaxTrajectory * (model->nu * kMaxTrajectoryHorizon));

  // trajectory and parameters
  winner = -1;
  for (int i = 0; i < kMaxTrajectory; i++) {
    trajectory[i].Initialize(num_state, model->nu, task->num_residual,
                             task->num_trace, kMaxTrajectoryHorizon);
    trajectory[i].Allocate(kMaxTrajectoryHorizon);
    candidate_policy[i].Allocate(model, *task, kMaxTrajectoryHorizon);
    candidate_policy[i].num_spline_points = num_spline_points_; // added by me
    trajectory[i].MPPIinitialize(gamma);
  }
  for (int i = 0 ; i < lambda_num ; i++){
    MPPI_trajectory[i].Initialize(num_state, model->nu, task->num_residual,
                             task->num_trace, kMaxTrajectoryHorizon);
    MPPI_trajectory[i].Allocate(kMaxTrajectoryHorizon);
    MPPI_candidate_policy[i].Allocate(model, *task, kMaxTrajectoryHorizon);
    MPPI_candidate_policy[i].num_spline_points = num_spline_points_; // added by me
  }

  // noise gradient
  noise_gradient.resize(num_max_parameter);
}

// reset memory to zeros
void MPPIPlanner::Reset(int horizon) {
  
  // state
  std::fill(state.begin(), state.end(), 0.0);
  std::fill(mocap.begin(), mocap.end(), 0.0);
  std::fill(userdata.begin(), userdata.end(), 0.0);
  time = 0.0;

  // policy parameters
  policy.Reset(horizon);
  previous_policy.Reset(horizon);

  // scratch
  std::fill(parameters_scratch.begin(), parameters_scratch.end(), 0.0);
  std::fill(cov_parameters_scratch.begin(), cov_parameters_scratch.end(), noise_exploration);
  std::fill(times_scratch.begin(), times_scratch.end(), 0.0);
  std::fill(tmp_params_scratch.begin(), tmp_params_scratch.end(), 0.0);
  std::fill(tmp_times_scratch.begin(), tmp_times_scratch.end(), 0.0);
  std::fill(importance_weight.begin(), importance_weight.end(), 0.0);
  // noise
  std::fill(noise.begin(), noise.end(), 0.0);

  // trajectory samples
  for (int i = 0; i < kMaxTrajectory; i++) {
    trajectory[i].Reset(kMaxTrajectoryHorizon);
    candidate_policy[i].Reset(horizon);
  }

  for (const auto& d : data_) {
    mju_zero(d->ctrl, model->nu);
  }
  for (int i = 0; i <lambda_num; ++i) {
        lambda_list[i] = 0.001;
    }
  // noise gradient
  std::fill(noise_gradient.begin(), noise_gradient.end(), 0.0);

  // improvement
  improvement = 0.0;

  // winner
  winner = 0;
}

// set state
void MPPIPlanner::SetState(const State& state) {
  state.CopyTo(this->state.data(), this->mocap.data(), this->userdata.data(),
               &this->time);
}
double clamp(double value, double min, double max) {
    return std::min(std::max(value, min), max);
}
void MPPIPlanner::Cal_MPPI_candidate(double lambda_candidate, double min_return, int num_trajectory, int ith){
  etha = 0;
  for(int i = 0 ; i < num_trajectory; i++){
    //numerator of weight (need to be divided by etha later)
    importance_weight[i] = std::exp(-trajectory[i].total_return/lambda_candidate);
    etha += importance_weight[i];
  }
  for(int i = 0 ; i< num_trajectory; i++){
    importance_weight[i] /= etha;
  }

  for(int t = 0 ; t < num_spline_points_; t++ ){
    for(int u = 0 ; u < model->nu ; u++){
      //for each input
      double tmp_mean = (1-alpha_mu)*candidate_policy[0].parameters.at(t*model->nu + u);//from nominal policy
      //double tmp_var = (1-alpha_sig)*cov_parameters_scratch.at(t*model->nu + u);//from previous cov
      for(int i = 0 ; i< num_trajectory; i++){//mean calculate
        tmp_mean += alpha_mu*importance_weight[i]*candidate_policy[i].parameters.at(t*model->nu + u);
      }
      if(ith==-1){
        candidate_policy[0].parameters.at(t*model->nu + u) = tmp_mean;
      }
      else{
        MPPI_candidate_policy[ith].parameters.at(t*model->nu + u) = tmp_mean;
      }
      
    }
  }
}
void saveArrayToFile(const std::vector<double>& array, const std::string& filename) {
    // Create an output file stream
    std::ofstream outFile(filename);

    // Check if the file is open
    if (!outFile) {
        std::cerr << "Unable to open file for writing: " << filename << std::endl;
        return;
    }

    // Write array elements to the file
    for (const double& value : array) {
        outFile << value << "\n";
    }
    std::cout << "Current working directory: " << std::filesystem::current_path() << std::endl;
    std::cout <<"Lambda SAVED"<<std::endl;
    // Close the file
    outFile.close();
}

int MPPIPlanner::OptimizePolicyCandidates(int ncandidates, int horizon,
                                              ThreadPool& pool) {

  // if num_spline_points_ has changed, use it in this new iteration
  // num_spline_points_ might change while this function runs. Keep it constant
  // for the duration of this function.
  policy.num_spline_points = num_spline_points_; // added by me ! 
  previous_policy.num_spline_points = num_spline_points_; // added by me!
  for (int i = 0; i < kMaxTrajectory; i++) {
    candidate_policy[i].num_spline_points = num_spline_points_; // added by me
  }

  // if num_trajectory_ has changed, use it in this new iteration.
  // num_trajectory_ might change while this function runs. Keep it constant
  // for the duration of this function.
  int num_trajectory = num_trajectory_;
  ncandidates = std::min(ncandidates, num_trajectory);
  ResizeMjData(model, pool.NumThreads());

  // ----- rollout noisy policies ----- //
  // start timer
  auto rollouts_start = std::chrono::steady_clock::now();

  //std::cout<< "candiate policy[0] scratch begin "<< candidate_policy[0].parameters.at(0) << std::endl;
  // simulate noisy policies
  this->Rollouts(num_trajectory, horizon, pool);

  //std::cout<< "candiate policy[0] scratch middle "<< candidate_policy[0].parameters.at(0) << std::endl;
  double min_return = trajectory[0].total_return;

  for(int i = 0 ; i < num_trajectory; i++){
    if(trajectory[i].total_return < min_return){
      min_return = trajectory[i].total_return;
    }
  }
  
  // now, min_return have the lowest cost
  // Now try calculate different MPPI results with different lambdas
  for(int i = 0 ; i < num_trajectory; i++){
    trajectory[i].total_return -= min_return;
  }
  
  std::random_device rd;
  std::mt19937 gen(rd());
  double sigma = 1.0*lambda;
  std::lognormal_distribution<> distribution(std::log(lambda) + sigma * sigma, sigma);

  if(time> 10 && time<50){
    //absl::BitGen gen_;
    for(int i = 0 ; i<lambda_num ; i++){
      // Log Normal distribution
      //tmp = absl::Gaussian<double>(gen_, lambda, 0.05*lambda);
      lambda_list[i] = distribution(gen);
      if(lambda_list[i]<=0){
        std::cout <<"Negative Lambda"<<std::endl;
        lambda_list[i]=lambda;
      }
    }
    
    //Calculate desired MPPI control law using different Lambda's
    for(int i = 0 ; i< lambda_num ; i++){
      Cal_MPPI_candidate(lambda_list[i], min_return, num_trajectory, i);//Calculate MPPI Candidate policy
    }
    //MPPI_candidate_policy[ith].parameters.at(t*model->nu + u) = tmp_mean;
    
    for(int i = 0 ; i < lambda_num ; i++){
      MPPI_NominalTrajectory(horizon, pool, i); //total_return will be calculated using this new trajectory
    }
    //std::cout <<"HERE!!!!" <<std::endl;
    //Now, MPPI_NominalTrajectory contains the calculated costs
    //Compute the best MPPI policy(lambda)
    best_lambda = Optimize_Lambda(horizon); // best_lambda is the index of the best policy
    lambda = lambda_list[best_lambda];
    //optimal_lambda_save[cnt] = lambda;
    optimal_lambda_save.push_back(lambda);
    cnt++;
    //candidate_policy[0] = MPPI_candidate_policy[best_lambda];
    for(int t = 0 ; t < num_spline_points_; t++ ){
      for(int u = 0 ; u < model->nu ; u++){
        //for each input
        candidate_policy[0].parameters.at(t*model->nu + u) = MPPI_candidate_policy[best_lambda].parameters.at(t*model->nu + u);
      }
    }
  }
  else{
    Cal_MPPI_candidate(lambda, min_return, num_trajectory, -1);
    if(time>50 && cnt<10000){
      std::string filename = "optimal_lambda.txt";
      saveArrayToFile(optimal_lambda_save, filename);
      cnt = 10001;
    }
  }
    
  //std::cout<< "cov par scratch end "<< cov_parameters_scratch.at(0) << std::endl;
  // compute trajectory again using the new MPPI policy
  NominalTrajectory(horizon, pool); //total_return will be calculated using this new trajectory

  // stop timer
  rollouts_compute_time = GetDuration(rollouts_start);

  return ncandidates;
}
int MPPIPlanner::Optimize_Lambda(int horizon){
  double tmp_mean;
  double tmp_var;
  double min_obj= 9999999;
  int min_idx = 0;
  for(int i = 0 ; i < lambda_num ; i++){
    tmp_mean = tmp_var = 0;
    for(int t = 0 ; t< horizon ; t++){
      tmp_mean += MPPI_trajectory[i].costs[t];
    }
    tmp_mean /= horizon;
    for(int t = 0 ; t< horizon ; t++){
      tmp_var += MPPI_trajectory[i].costs[t]*MPPI_trajectory[i].costs[t];
    }
    tmp_var /= horizon;
    tmp_var = tmp_var - tmp_mean*tmp_mean;
    if(tmp_mean + tmp_var*cov_weight < min_obj){
      min_obj = tmp_mean + tmp_var*cov_weight;
      min_idx = i;
    }
  }
  return min_idx;
}

// optimize nominal policy using random sampling
void MPPIPlanner::OptimizePolicy(int horizon, ThreadPool& pool) {
  // resample nominal policy to current time
  this->UpdateNominalPolicy(horizon);
  OptimizePolicyCandidates(1, horizon, pool);
  // ----- update policy ----- //
  // start timer
  auto policy_update_start = std::chrono::steady_clock::now();

  CopyCandidateToPolicy(0);
  
  // improvement: compare nominal to winner
  double best_return = trajectory[0].total_return;
  improvement = mju_max(best_return - trajectory[winner].total_return, 0.0);

  // stop timer
  policy_update_compute_time = GetDuration(policy_update_start);
}
// compute trajectory using MPPI policy
void MPPIPlanner::MPPI_NominalTrajectory(int horizon, ThreadPool& pool, int ith) {
  // set policy
  
  auto nominal_policy = [&cp = MPPI_candidate_policy[ith]](
                            double* action, const double* state, double time) {
    cp.Action(action, state, time);
  };
  //std::cout <<"HERE!!!!" <<std::endl;
  // rollout policy
  MPPI_trajectory[ith].Rollout(nominal_policy, task, model, data_[0].get(),
                        state.data(), time, mocap.data(), userdata.data(),
                        horizon);
}
// compute trajectory using nominal policy
void MPPIPlanner::NominalTrajectory(int horizon, ThreadPool& pool) {
  // set policy
  //std::cout<< "candiate policy[0] scratch here "<< candidate_policy[0].parameters.at(0) << std::endl;
  auto nominal_policy = [&cp = candidate_policy[0]](
                            double* action, const double* state, double time) {
    cp.Action(action, state, time);
  };

  // rollout nominal policy
  trajectory[0].Rollout(nominal_policy, task, model, data_[0].get(),
                        state.data(), time, mocap.data(), userdata.data(),
                        horizon);
}

// set action from policy
void MPPIPlanner::ActionFromPolicy(double* action, const double* state,
                                       double time, bool use_previous) {
  const std::shared_lock<std::shared_mutex> lock(mtx_);
  if (use_previous) {
    previous_policy.Action(action, state, time);
  } else {
    policy.Action(action, state, time);
  }
}

// update policy via resampling
void MPPIPlanner::UpdateNominalPolicy(int horizon) {
  // dimensions
  int num_spline_points = candidate_policy[winner].num_spline_points;

  // set time
  double nominal_time = time;
  double time_shift = mju_max(
      (horizon - 1) * model->opt.timestep / (num_spline_points - 1), 1.0e-5);

  // get spline points
  for (int t = 0; t < num_spline_points; t++) {
    times_scratch[t] = nominal_time;
    candidate_policy[winner].Action(DataAt(parameters_scratch, t * model->nu),
                               nullptr, nominal_time);
    nominal_time += time_shift;
  }
  //std::cout<<"params scractch2 : "<<parameters_scratch.at(0) << std::endl;
  // update 
  {
    const std::shared_lock<std::shared_mutex> lock(mtx_);
    // parameters
    policy.CopyParametersFrom(parameters_scratch, times_scratch);

    // time power transformation
    PowerSequence(policy.times.data(), time_shift,
                  policy.times[0],
                  policy.times[num_spline_points - 1],
                  timestep_power, num_spline_points);
  }
}

// add random noise to nominal policy
void MPPIPlanner::AddNoiseToPolicy(int i, int horizon) {
  // start timer
  auto noise_start = std::chrono::steady_clock::now();

  // dimensions
  int num_spline_points = candidate_policy[i].num_spline_points;
  int num_parameters = candidate_policy[i].num_parameters;
  //int num_parameters = model->nu * horizon;

  // sampling token
  absl::BitGen gen_;

  // shift index
  int shift = i * (model->nu * kMaxTrajectoryHorizon);

  //std::cout <<" num param : " <<num_parameters << std::endl;
  // sample noise
  for (int k = 0; k < num_parameters; k++) {
    //use nominal covariance to generate noise
    noise[k + shift] = absl::Gaussian<double>(gen_, 0.0, cov_parameters_scratch.at(k));
    //noise[k + shift] = absl::Gaussian<double>(gen_, 0.0, noise_exploration);
  }

  // add noise
  mju_addTo(candidate_policy[i].parameters.data(), DataAt(noise, shift),
            num_parameters);

  // clamp parameters
  for (int t = 0; t < num_spline_points; t++) {
    Clamp(DataAt(candidate_policy[i].parameters, t * model->nu),
          model->actuator_ctrlrange, model->nu);
  }

  // end timer
  IncrementAtomic(noise_compute_time, GetDuration(noise_start));
}

// compute candidate trajectories
void MPPIPlanner::Rollouts(int num_trajectory, int horizon,
                               ThreadPool& pool) {
  // reset noise compute time
  noise_compute_time = 0.0;

  policy.num_parameters = model->nu * policy.num_spline_points;
  // random search
  int count_before = pool.GetCount();
  for (int i = 0; i < num_trajectory; i++) {
    pool.Schedule([&s = *this, &model = this->model, &task = this->task,
                   &state = this->state, &time = this->time,
                   &mocap = this->mocap, &userdata = this->userdata, horizon,
                   i]() {
      // copy nominal policy
      {
        const std::shared_lock<std::shared_mutex> lock(s.mtx_);
        s.candidate_policy[i].CopyFrom(s.policy, s.policy.num_spline_points);
        s.candidate_policy[i].representation = s.policy.representation;
      }

      // sample noise policy
      if (i != 0) s.AddNoiseToPolicy(i, horizon);
      //s.AddNoiseToPolicy(i);
      // ----- rollout sample policy ----- //

      // policy
      auto sample_policy_i = [&candidate_policy = s.candidate_policy, &i](
                                 double* action, const double* state,
                                 double time) {
        candidate_policy[i].Action(action, state, time);
      };

      // policy rollout
      s.trajectory[i].Rollout(
          sample_policy_i, task, model, s.data_[ThreadPool::WorkerId()].get(),
          state.data(), time, mocap.data(), userdata.data(), horizon);
    });
  }
  pool.WaitCount(count_before + num_trajectory);
  pool.ResetCount();
}

// return trajectory with best total return
const Trajectory* MPPIPlanner::BestTrajectory() {
  return winner >= 0 ? &trajectory[winner] : nullptr;
}

// visualize planner-specific traces
void MPPIPlanner::Traces(mjvScene* scn) {
  // sample color
  float color[4];
  color[0] = 1.0;
  color[1] = 1.0;
  color[2] = 1.0;
  color[3] = 1.0;

  // width of a sample trace, in pixels
  double width = GetNumberOrDefault(3, model, "agent_sample_width");

  // scratch
  double zero3[3] = {0};
  double zero9[9] = {0};

  // best
  auto best = this->BestTrajectory();

  // sample traces
  for (int k = 0; k < num_trajectory_; k++) {
    // skip winner
    if (k == winner) continue;

    // plot sample
    for (int i = 0; i < best->horizon - 1; i++) {
      if (scn->ngeom + task->num_trace > scn->maxgeom) break;
      for (int j = 0; j < task->num_trace; j++) {
        // initialize geometry
        mjv_initGeom(&scn->geoms[scn->ngeom], mjGEOM_LINE, zero3, zero3, zero9,
                     color);

        // make geometry
        mjv_makeConnector(
            &scn->geoms[scn->ngeom], mjGEOM_LINE, width,
            trajectory[k].trace[3 * task->num_trace * i + 3 * j],
            trajectory[k].trace[3 * task->num_trace * i + 1 + 3 * j],
            trajectory[k].trace[3 * task->num_trace * i + 2 + 3 * j],
            trajectory[k].trace[3 * task->num_trace * (i + 1) + 3 * j],
            trajectory[k].trace[3 * task->num_trace * (i + 1) + 1 + 3 * j],
            trajectory[k].trace[3 * task->num_trace * (i + 1) + 2 + 3 * j]);

        // increment number of geometries
        scn->ngeom += 1;
      }
    }
  }
}

// planner-specific GUI elements
void MPPIPlanner::GUI(mjUI& ui) {
  mjuiDef defSampling[] = {
      {mjITEM_SLIDERINT, "Rollouts", 2, &num_trajectory_, "0 1"},
      {mjITEM_SELECT, "Spline", 2, &policy.representation,
       "Zero\nLinear\nCubic"},
      //{mjITEM_SLIDERINT, "Spline Pts", 2, &policy.num_spline_points, "0 1"},
      {mjITEM_SLIDERINT, "Spline Pts", 2, &num_spline_points_, "0 1"},
      // {mjITEM_SLIDERNUM, "Spline Pow. ", 2, &timestep_power, "0 10"},
      // {mjITEM_SELECT, "Noise type", 2, &noise_type, "Gaussian\nUniform"},
      {mjITEM_SLIDERNUM, "Noise Std", 2, &noise_exploration, "0 1"},
      {mjITEM_SLIDERNUM, "Inverse temperature", 2, &lambda, "0 1"},
      {mjITEM_SLIDERNUM, "Discount Factor", 2, &gamma, "0 1"},
      {mjITEM_SLIDERNUM, "Alpha mu", 2, &alpha_mu, "0 1"},
      {mjITEM_SLIDERNUM, "Alpha sig", 2, &alpha_sig, "0 1"},
      {mjITEM_SLIDERNUM, "Lambda coeff ", 2, &lam_coeff, "0 1"},
      {mjITEM_SLIDERNUM, "Lambda power", 2, &lam_power, "0 1"},
      {mjITEM_SLIDERNUM, "Lambda obj tuning", 2, &cov_weight, "0 1"},
      {mjITEM_END}};

  // set number of trajectory slider limits
  mju::sprintf_arr(defSampling[0].other, "%i %i", 1, kMaxTrajectory);

  // set spline point limits
  mju::sprintf_arr(defSampling[2].other, "%i %i", MinMPPISamplingSplinePoints,
                   MaxMPPISamplingSplinePoints);

  // set noise standard deviation limits
  mju::sprintf_arr(defSampling[3].other, "%f %f", MinMPPINoiseStdDev,
                   MaxMPPINoiseStdDev);

  // set inverse temperature limits
  mju::sprintf_arr(defSampling[4].other, "%f %f", MinMPPIinvTemp,
                   MaxMPPIinvTemp);

  // set Discount factor limits
  mju::sprintf_arr(defSampling[5].other, "%f %f", MinMPPIGamma,
                   MaxMPPIGamma);

  // set alpha _mu from Storm Paper
  mju::sprintf_arr(defSampling[6].other, "%f %f", MinMPPIAlphamu,
                   MaxMPPIAlphamu);

  // set alpha _sig from Storm Paper
  mju::sprintf_arr(defSampling[7].other, "%f %f", MinMPPIAlphasig,
                   MaxMPPIAlphasig);

  mju::sprintf_arr(defSampling[8].other, "%f %f", 0,
                   9.999);

  mju::sprintf_arr(defSampling[9].other, "%d %d", -50,
                   10);

  mju::sprintf_arr(defSampling[10].other, "%f %f", 0.01,
                   99.9);

  // add sampling planner
  mjui_add(&ui, defSampling);
}

// planner-specific plots
void MPPIPlanner::Plots(mjvFigure* fig_planner, mjvFigure* fig_timer,
                            int planner_shift, int timer_shift, int planning,
                            int* shift) {
  // ----- planner ----- //
  double planner_bounds[2] = {-6.0, 6.0};

  // improvement
  mjpc::PlotUpdateData(fig_planner, planner_bounds,
                       fig_planner->linedata[0 + planner_shift][0] + 1,
                       mju_log10(mju_max(improvement, 1.0e-6)), 100,
                       0 + planner_shift, 0, 1, -100);

  // legend
  mju::strcpy_arr(fig_planner->linename[0 + planner_shift], "Improvement");

  fig_planner->range[1][0] = planner_bounds[0];
  fig_planner->range[1][1] = planner_bounds[1];

  // bounds
  double timer_bounds[2] = {0.0, 1.0};

  // ----- timer ----- //

  PlotUpdateData(
      fig_timer, timer_bounds, fig_timer->linedata[0 + timer_shift][0] + 1,
      1.0e-3 * noise_compute_time * planning, 100, 0 + timer_shift, 0, 1, -100);

  PlotUpdateData(fig_timer, timer_bounds,
                 fig_timer->linedata[1 + timer_shift][0] + 1,
                 1.0e-3 * rollouts_compute_time * planning, 100,
                 1 + timer_shift, 0, 1, -100);

  PlotUpdateData(fig_timer, timer_bounds,
                 fig_timer->linedata[2 + timer_shift][0] + 1,
                 1.0e-3 * policy_update_compute_time * planning, 100,
                 2 + timer_shift, 0, 1, -100);

  // legend
  mju::strcpy_arr(fig_timer->linename[0 + timer_shift], "Noise");
  mju::strcpy_arr(fig_timer->linename[1 + timer_shift], "Rollout");
  mju::strcpy_arr(fig_timer->linename[2 + timer_shift], "Policy Update");

  // planner shift
  shift[0] += 1;

  // timer shift
  shift[1] += 3;
}

double MPPIPlanner::CandidateScore(int candidate) const {
  return trajectory[trajectory_order[candidate]].total_return;
}

// set action from candidate policy
void MPPIPlanner::ActionFromCandidatePolicy(double* action, int candidate,
                                                const double* state,
                                                double time) {
  candidate_policy[trajectory_order[candidate]].Action(action, state, time);
}

void MPPIPlanner::CopyCandidateToPolicy(int candidate) {
  // set winner
  //winner = trajectory_order[candidate];
  winner = 0;
  {
    const std::shared_lock<std::shared_mutex> lock(mtx_);
    previous_policy = policy;
    policy = candidate_policy[winner];
  }
}
}  // namespace mjpc
