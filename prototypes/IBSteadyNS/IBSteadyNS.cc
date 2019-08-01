﻿/* ---------------------------------------------------------------------
 *
 * Copyright (C) 2000 - 2016 by the deal.II authors
 *
 * This file is part of the deal.II library.
 *
 * The deal.II library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE at
 * the top level of the deal.II distribution.
 *
 * ---------------------------------------------------------------------

 *
 * Author: Wolfgang Bangerth, University of Heidelberg, 2000
 */


// @sect3{Include files}

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/logstream.h>

#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/block_sparse_matrix.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/solver_bicgstab.h>
#include <deal.II/lac/solver_gmres.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/sparse_ilu.h>
#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/sparse_direct.h>

#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/grid/manifold_lib.h>
#include <deal.II/grid/grid_in.h>
#include <deal.II/grid/grid_out.h>
#include <deal.II/grid/grid_refinement.h>


#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_q.h>

#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/numerics/solution_transfer.h>

#include "exactsolutions.h"
#include "forcingfunctions.h"
#include "boundaryconditions.h"

#include <fstream>
#include <iostream>

#include "GLS_residual.h"
#include "iblevelsetfunctions.h"
#include "ibcombiner.h"
#include "ib_node_status.h"
#include "nouvtriangles.h"

// Finally, this is as in previous programs:
using namespace dealii;

enum SimulationCases{MMS, CouetteX, CouetteY, TaylorCouette};

template <int dim>
class DirectSteadyNavierStokes
{

public:
    DirectSteadyNavierStokes(const unsigned int degreeVelocity, const unsigned int degreePressure);
    ~DirectSteadyNavierStokes();
    void runCouetteX();
    void runCouetteIBX();
    void runCouetteXPerturbedMesh();
    void runIBTaylorCouette();
    void runMMS();
    void runTaylorCouette();

    Function<dim> *exact_solution;
    Function<dim> *forcing_function;

private:
    void make_cube_grid(int refinementLevel);
    void refine_grid();
    void refine_mesh();
    void refine_mesh_uniform();
    void setup_dofs();
    void initialize_system();

    void assemble(const bool initial_step,
                   const bool assemble_matrix);
    void assemble_system(const bool initial_step);
    void assemble_rhs(const bool initial_step);
    void solve(bool initial_step);
    void calculateL2Error();
    void output_results(std::string case_name, const unsigned int cycle) const;
    void newton_iteration(const double tolerance,
                          const unsigned int max_iteration,
                          const bool is_initial_step,
                          const bool output_result);


    std::vector<types::global_dof_index> dofs_per_block;

    double viscosity_;
    const unsigned int           degreeIntegration_;
    Triangulation<dim> triangulation;
    FESystem<dim> fe;
    DoFHandler<dim> dof_handler;


    AffineConstraints<double>    zero_constraints;
    AffineConstraints<double>    nonzero_constraints;

    BlockSparsityPattern         sparsity_pattern;
    BlockSparseMatrix<double>    system_matrix;

    BlockVector<double>          present_solution;
    BlockVector<double>          newton_update;
    BlockVector<double>          system_rhs;
    BlockVector<double>          evaluation_point;

    IBCombiner<dim>              ib_combiner;


    SimulationCases simulationCase_;
    const bool stabilized_=false;
    const bool iterative_=false;
    std::vector<double> L2ErrorU_;
    const int initialSize_=3;
};


// Constructor
template<int dim>
DirectSteadyNavierStokes<dim>::DirectSteadyNavierStokes(const unsigned int degreeVelocity, const unsigned int degreePressure):
    viscosity_(1), degreeIntegration_(degreeVelocity),
    fe(FE_Q<dim>(degreeVelocity), dim, FE_Q<dim>(degreePressure), 1),
    dof_handler(triangulation)
{}


template <int dim>
DirectSteadyNavierStokes<dim>::~DirectSteadyNavierStokes ()
{
  triangulation.clear ();
}



template <int dim>
void DirectSteadyNavierStokes<dim>::make_cube_grid (int refinementLevel)
{
  GridGenerator::hyper_cube (triangulation, -1, 1);
  triangulation.refine_global (refinementLevel);
}

template <int dim>
void DirectSteadyNavierStokes<dim>::refine_grid()
{
    triangulation.refine_global(1);
}


template <int dim>
void DirectSteadyNavierStokes<dim>::setup_dofs ()
{
    system_matrix.clear();

    dof_handler.distribute_dofs(fe);

    std::vector<unsigned int> block_component(dim+1, 0);
    block_component[dim] = 1;
    DoFRenumbering::component_wise (dof_handler, block_component);
    dofs_per_block.resize (2);
    DoFTools::count_dofs_per_block (dof_handler, dofs_per_block, block_component);
    unsigned int dof_u = dofs_per_block[0];
    unsigned int dof_p = dofs_per_block[1];

    FEValuesExtractors::Vector velocities(0);
    {
      nonzero_constraints.clear();

      DoFTools::make_hanging_node_constraints(dof_handler, nonzero_constraints);
      VectorTools::interpolate_boundary_values(dof_handler, 0, ZeroFunction<dim>(dim+1), nonzero_constraints,
                                               fe.component_mask(velocities));

      if (simulationCase_==TaylorCouette)
      {
          VectorTools::interpolate_boundary_values(dof_handler,
                                                   1,
                                                   RotatingWall<dim>(),
                                                   nonzero_constraints,
                                                   fe.component_mask(velocities));
      }

      if (simulationCase_==CouetteX)
      {
          VectorTools::interpolate_boundary_values(dof_handler,
                                                   1,
                                                   ConstantYMotion<dim>(),
                                                   nonzero_constraints,
                                                   fe.component_mask(velocities));
      }
    }
    nonzero_constraints.close();

    {
      zero_constraints.clear();
      DoFTools::make_hanging_node_constraints(dof_handler, zero_constraints);
      VectorTools::interpolate_boundary_values(dof_handler,
                                               0,
                                               ZeroFunction<dim>(dim+1),
                                               zero_constraints,
                                               fe.component_mask(velocities));


      if (simulationCase_==TaylorCouette || simulationCase_==CouetteX )
      {
          VectorTools::interpolate_boundary_values(dof_handler,
                                               1,
                                               ZeroFunction<dim>(dim+1),
                                               zero_constraints,
                                               fe.component_mask(velocities));
      }

    }
    zero_constraints.close();
    std::cout << "   Number of active cells: "
              << triangulation.n_active_cells()
              << std::endl
              << "   Number of degrees of freedom: "
              << dof_handler.n_dofs()
              << " (" << dof_u << '+' << dof_p << ')'
              << std::endl;

}

template <int dim>
void DirectSteadyNavierStokes<dim>::initialize_system()
{
  {
    BlockDynamicSparsityPattern dsp (dofs_per_block, dofs_per_block);
    DoFTools::make_sparsity_pattern (dof_handler, dsp, nonzero_constraints);
    sparsity_pattern.copy_from (dsp);
  }
  system_matrix.reinit (sparsity_pattern);
  present_solution.reinit (dofs_per_block);
  newton_update.reinit (dofs_per_block);
  system_rhs.reinit (dofs_per_block);
}

template <int dim>
void DirectSteadyNavierStokes<dim>::assemble(const bool initial_step,
                                           const bool assemble_matrix)
{



    if (assemble_matrix) system_matrix    = 0;
    system_rhs       = 0;
    QGauss<dim>   quadrature_formula(degreeIntegration_+2);
    const MappingQ<dim>      mapping (1);
    FEValues<dim> fe_values (mapping,
                             fe,
                             quadrature_formula,
                             update_values |
                             update_quadrature_points |
                             update_JxW_values |
                             update_gradients |
                             update_hessians);
    const unsigned int   dofs_per_cell = fe.dofs_per_cell;
    const unsigned int   n_q_points    = quadrature_formula.size();
    const FEValuesExtractors::Vector velocities (0);
    const FEValuesExtractors::Scalar pressure (dim);
    FullMatrix<double>   local_matrix (dofs_per_cell, dofs_per_cell);
    Vector<double>       local_rhs    (dofs_per_cell);
    std::vector<Vector<double> >      rhs_force (n_q_points, Vector<double>(dim+1));
    std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell);
    std::vector<Tensor<1, dim> >  present_velocity_values    (n_q_points);
    std::vector<Tensor<2, dim> >  present_velocity_gradients (n_q_points);
    std::vector<double>           present_pressure_values    (n_q_points);
    std::vector<Tensor<1, dim> >  present_pressure_gradients   (n_q_points);
    std::vector<Tensor<1, dim> >  present_velocity_laplacians  (n_q_points);
    std::vector<Tensor<2, dim> >  present_velocity_hess        (n_q_points);

    Tensor<1, dim>  force;


    std::vector<double>           div_phi_u                 (dofs_per_cell);
    std::vector<Tensor<1, dim> >  phi_u                     (dofs_per_cell);
    std::vector<Tensor<3, dim> >  hess_phi_u                (dofs_per_cell);
    std::vector<Tensor<1, dim> >  laplacian_phi_u           (dofs_per_cell);
    std::vector<Tensor<2, dim> >  grad_phi_u                (dofs_per_cell);
    std::vector<double>           phi_p                     (dofs_per_cell);
    std::vector<Tensor<1, dim> >  grad_phi_p                (dofs_per_cell);

    std::map< types::global_dof_index, Point< 2 > > support_points;
    std::vector<double>                  distance(dofs_per_cell); // Array for the distances associated with the DOFS
    std::vector<Point<2> >               dofs_points(dofs_per_cell);// Array for the DOFs points

    // Instantiations for the decomposition of the elements
    std::vector<int>                     corresp(9);
    std::vector<Point<2> >               decomp_elem(9);         // Array containing the points of the new elements created by decomposing the elements crossed by the boundary fluid/solid, there are up to 9 points that are stored in it
    std::vector<node_status>             No_pts_solid(4);
    int                                  nb_poly=0;                   // Number of sub-elements created in the fluid part for each element ( 0 if the element is entirely in the solid or the fluid)
    std::vector<Point<2> >               num_elem(6);

    std::vector<Point<2> >               coor(4);
    std::vector<double>                  dist(4);
    // The previous part is only implemented for 2D NS, and the function nouvtriangles as well

    typename DoFHandler<dim>::active_cell_iterator
    cell = dof_handler.begin_active(),
    endc = dof_handler.end();

    // Element size
    double h ;

    for (; cell!=endc; ++cell)
      {
        fe_values.reinit(cell);
        cell->get_dof_indices (local_dof_indices);

        if (ib_combiner.size()>0)
        {
          for (unsigned int dof_index=0 ; dof_index < local_dof_indices.size() ; ++dof_index)
          {
            dofs_points[dof_index] = support_points[local_dof_indices[dof_index]];
            distance[dof_index]    = ib_combiner.value(dofs_points[dof_index]);
          }

          // We get the coordinates and the distance associated to the vertices of the element
          for (unsigned int i = 0; i < dofs_per_cell/(dim+1); ++i) {
            coor[i] = dofs_points[(dim+1)*i];
            dist[i] = distance[(dim+1)*i];
          }

          nouvtriangles(corresp, No_pts_solid, num_elem, decomp_elem, &nb_poly, coor, dist);
        }
        else
          nb_poly=0;

        //nb_poly=0;
        local_matrix = 0;
        local_rhs    = 0;

        if (ib_combiner.size()<1 || (nb_poly==0 && (distance[0]>0)) )
        {
          if (dim==2) h = std::sqrt(4.* cell->measure() / M_PI) ;
          else if (dim==3) h = pow(6*cell->measure()/M_PI,1./3.) ;

          fe_values[velocities].get_function_values(evaluation_point, present_velocity_values);
          fe_values[velocities].get_function_gradients(evaluation_point, present_velocity_gradients);
          fe_values[pressure].get_function_values(evaluation_point,present_pressure_values);
          fe_values[pressure].get_function_gradients(evaluation_point,present_pressure_gradients);
          fe_values[velocities].get_function_laplacians(evaluation_point,present_velocity_laplacians);
            forcing_function->vector_value_list(fe_values.get_quadrature_points(),
                                                       rhs_force);

            for (unsigned int q=0; q<n_q_points; ++q)
            {
              const double u_mag= std::max(present_velocity_values[q].norm(),1e-3*1.);
              double tau = 1./ std::sqrt(std::pow(2.*u_mag/h,2)+9*std::pow(4*viscosity_/(h*h),2));
              for (unsigned int k=0; k<dofs_per_cell; ++k)
              {
                div_phi_u[k]  =  fe_values[velocities].divergence (k, q);
                grad_phi_u[k] =  fe_values[velocities].gradient(k, q);
                phi_u[k]      =  fe_values[velocities].value(k, q);
                hess_phi_u[k] =  fe_values[velocities].hessian(k, q);
                phi_p[k]      =  fe_values[pressure]  .value(k, q);
                grad_phi_p[k] =  fe_values[pressure]  .gradient(k, q);

                for( int d=0; d<dim; ++d )
                  laplacian_phi_u[k][d] = trace( hess_phi_u[k][d] );
              }

              // Establish the force vector
              for( int i=0; i<dim; ++i )
              {
                const unsigned int component_i = fe.system_to_component_index(i).first;
                force[i] = rhs_force[q](component_i);
              }

              auto strong_residual= present_velocity_gradients[q]*present_velocity_values[q]
                                    + present_pressure_gradients[q]
                                    - viscosity_* present_velocity_laplacians[q]
                                    - force ;

              for (unsigned int j=0; j<dofs_per_cell; ++j)
              {
                if (assemble_matrix)
                {
                  auto strong_jac = (  present_velocity_gradients[q]*phi_u[j]
                                       + grad_phi_u[j]*present_velocity_values[q]
                                       + grad_phi_p[j]
                                       - viscosity_* laplacian_phi_u[j]
                                       );

                  for (unsigned int i=0; i<dofs_per_cell; ++i)
                  {
                    local_matrix(i, j) += (  viscosity_*scalar_product(grad_phi_u[j], grad_phi_u[i])
                                             + present_velocity_gradients[q]*phi_u[j]*phi_u[i]
                                             + grad_phi_u[j]*present_velocity_values[q]*phi_u[i]
                                             - div_phi_u[i]*phi_p[j]
                                             + phi_p[i]*div_phi_u[j]
                                             )
                                          * fe_values.JxW(q);
                    //PSPG GLS term
                    local_matrix(i, j) += tau*
                                          strong_jac* grad_phi_p[i]
                                          * fe_values.JxW(q);

                    // SUPG GLS term
                    local_matrix(i, j) +=
                        tau*
                        (
                          strong_jac*(grad_phi_u[i]*present_velocity_values[q])
                          +
                          strong_residual* (grad_phi_u[i]*phi_u[j])
                          )
                        * fe_values.JxW(q)
                        ;
                  }
                }
                }
                for (unsigned int i=0; i<dofs_per_cell; ++i)
                {
                  const unsigned int component_i = fe.system_to_component_index(i).first;
                  double present_velocity_divergence =  trace(present_velocity_gradients[q]);
                  local_rhs(i) += ( - viscosity_*scalar_product(present_velocity_gradients[q],grad_phi_u[i])
                                    - present_velocity_gradients[q]*present_velocity_values[q]*phi_u[i]
                                    + present_pressure_values[q]*div_phi_u[i]
                                    - present_velocity_divergence*phi_p[i]
                                    + force * phi_u[i]
                                    )
                          * fe_values.JxW(q);

                  // PSPG GLS term
                  local_rhs(i) +=  - tau
                                   * (strong_residual*grad_phi_p[i])
                                   * fe_values.JxW(q);

                  //SUPG GLS term
                      local_rhs(i) += - tau
                                      *(strong_residual*(grad_phi_u[i]*present_velocity_values[q]))
                                      * fe_values.JxW(q);
                }
            }
        }

        // Pure solid case
        else if ((nb_poly==0 && (distance[0]<0)) )
        {

        }

        // Quadrilateral case
        else if (nb_poly==-1)
        {

        }

        else if (nb_poly>0) {

            std::vector<Point<dim> >      coor_trg(3);

            FullMatrix<double>            cell_mat(18, 18);
            Vector<double>                cell_rhs(18);

            cell_mat = 0;
            cell_rhs = 0;

            // these are the cell matrix and rhs before we condensate them
            // we store the contributions created by the boundary points in it as well as the other contributions

            for (int n = 0; n < nb_poly; ++n) {
                coor_trg[0] = decomp_elem[(3*n)];
                coor_trg[(1)] = decomp_elem[(3*n+1)];
                coor_trg[2] = decomp_elem[(3*n)+2];

                // récuperer vitesse pression et grad, ensuite on appelle glsres

                //condenser ensuite
            }
        }


      cell->get_dof_indices (local_dof_indices);
      const AffineConstraints<double> &constraints_used = initial_step ? nonzero_constraints : zero_constraints;
      if (assemble_matrix)
        {
          constraints_used.distribute_local_to_global(local_matrix,
                                                      local_rhs,
                                                      local_dof_indices,
                                                      system_matrix,
                                                      system_rhs);
        }
      else
        {
          constraints_used.distribute_local_to_global(local_rhs,
                                                      local_dof_indices,
                                                      system_rhs);
        }
    }
}

template <int dim>
void DirectSteadyNavierStokes<dim>::assemble_system(const bool initial_step)
{
  assemble(initial_step, true);
}
template <int dim>
void DirectSteadyNavierStokes<dim>::assemble_rhs(const bool initial_step)
{
  assemble(initial_step, false);
}

template <int dim>
void DirectSteadyNavierStokes<dim>::solve (const bool initial_step)
{
  const AffineConstraints<double> &constraints_used = initial_step ? nonzero_constraints : zero_constraints;
  SparseDirectUMFPACK direct;
  direct.initialize(system_matrix);
  direct.vmult(newton_update,system_rhs);
  constraints_used.distribute(newton_update);
}

template <int dim>
void DirectSteadyNavierStokes<dim>::refine_mesh ()
{
    Vector<float> estimated_error_per_cell (triangulation.n_active_cells());
    FEValuesExtractors::Vector velocity(0);
    KellyErrorEstimator<dim>::estimate (dof_handler,
                                        QGauss<dim-1>(degreeIntegration_+1),
                                        typename std::map<types::boundary_id, const Function<dim, double> *>(),
                                        present_solution,
                                        estimated_error_per_cell,
                                        fe.component_mask(velocity));
    GridRefinement::refine_and_coarsen_fixed_number (triangulation,
                                                     estimated_error_per_cell,
                                                     0.15, 0.0);
    triangulation.prepare_coarsening_and_refinement();
    SolutionTransfer<dim, BlockVector<double> > solution_transfer(dof_handler);
    solution_transfer.prepare_for_coarsening_and_refinement(present_solution);
    triangulation.execute_coarsening_and_refinement ();
    setup_dofs();
    BlockVector<double> tmp (dofs_per_block);
    solution_transfer.interpolate(present_solution, tmp);
    nonzero_constraints.distribute(tmp);
    initialize_system();
    present_solution = tmp;
}

template <int dim>
void DirectSteadyNavierStokes<dim>::refine_mesh_uniform ()
{
    SolutionTransfer<dim, BlockVector<double> > solution_transfer(dof_handler);
    solution_transfer.prepare_for_coarsening_and_refinement(present_solution);
    triangulation.refine_global(1);
    setup_dofs();
    BlockVector<double> tmp (dofs_per_block);
    solution_transfer.interpolate(present_solution, tmp);
    nonzero_constraints.distribute(tmp);
    initialize_system();
    present_solution = tmp;
}


template <int dim>
void DirectSteadyNavierStokes<dim>::newton_iteration(const double tolerance,
                                                   const unsigned int max_iteration,
                                                   const bool  is_initial_step,
                                                   const bool  /*output_result*/)
{
  double current_res;
  double last_res;
  bool   first_step = is_initial_step;
    {
      unsigned int outer_iteration = 0;
      last_res = 1.0;
      current_res = 1.0;
      while ((first_step || (current_res > tolerance)) && outer_iteration < max_iteration)
        {
          if (first_step)
            {
              initialize_system();
              evaluation_point = present_solution;
              assemble_system(first_step);
              current_res = system_rhs.l2_norm();
              std::cout  << "Newton iteration: " << outer_iteration << "  - Residual:  " << current_res << std::endl;
              solve(first_step);
              present_solution = newton_update;
              nonzero_constraints.distribute(present_solution);
              first_step = false;
              evaluation_point = present_solution;
              assemble_rhs(first_step);
              current_res = system_rhs.l2_norm();
              last_res = current_res;
            }
          else
            {
              std::cout  << "Newton iteration: " << outer_iteration << "  - Residual:  " << current_res << std::endl;
              evaluation_point = present_solution;
              assemble_system(first_step);
              solve(first_step);
              for (double alpha = 1.0; alpha > 1e-3; alpha *= 0.5)
                {
                  evaluation_point = present_solution;
                  evaluation_point.add(alpha, newton_update);
                  nonzero_constraints.distribute(evaluation_point);
                  assemble_rhs(first_step);
                  current_res = system_rhs.l2_norm();
                  std::cout << "\t\talpha = " << std::setw(6) << alpha << std::setw(0)
                            << " res = " << current_res << std::endl;
                  if (current_res < last_res)
                    break;
                }
              {
                present_solution = evaluation_point;
                last_res = current_res;
              }
            }
          ++outer_iteration;

        }
    }
}

template <int dim>
void DirectSteadyNavierStokes<dim>::output_results (std::string case_name, const unsigned int cycle) const
{
    std::vector<std::string> solution_names (dim, "velocity");
    solution_names.push_back ("pressure");

    std::vector<DataComponentInterpretation::DataComponentInterpretation>
    data_component_interpretation
    (dim, DataComponentInterpretation::component_is_part_of_vector);
    data_component_interpretation
    .push_back (DataComponentInterpretation::component_is_scalar);

    DataOut<dim> data_out;
    data_out.attach_dof_handler (dof_handler);
    data_out.add_data_vector (present_solution, solution_names, DataOut<dim>::type_dof_data, data_component_interpretation);
    data_out.build_patches (1);

    std::string filenamesolution = case_name;
    filenamesolution += ('0' + cycle);
    filenamesolution += ".vtk";

    std::cout << "Writing file : " << filenamesolution << std::endl;
    std::ofstream outputSolution (filenamesolution.c_str());

    data_out.write_vtk (outputSolution);
}

//Find the l2 norm of the error between the finite element sol'n and the exact sol'n
template <int dim>
void DirectSteadyNavierStokes<dim>::calculateL2Error()
{

    QGauss<dim>  quadrature_formula(fe.degree+2);
    FEValues<dim> fe_values (fe, quadrature_formula,
                             update_values   | update_gradients |
                             update_quadrature_points | update_JxW_values);

    const FEValuesExtractors::Vector velocities (0);
    const FEValuesExtractors::Scalar pressure (dim);


    const unsigned int   			dofs_per_cell = fe.dofs_per_cell;         // This gives you dofs per cell
    std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell); //  Local connectivity

    const unsigned int   n_q_points    = quadrature_formula.size();
    double l2errorU=0.;

    std::vector<Vector<double> > q_exactSol (n_q_points, Vector<double>(dim+1));


    std::vector<Tensor<1,dim> > local_velocity_values (n_q_points);
    std::vector<double > local_pressure_values (n_q_points);

    double maxPressure=-DBL_MAX;
    // Get the maximal value of the pressure
    for (auto icell=dof_handler.begin_active(); icell!=dof_handler.end(); ++icell)
    {
        fe_values.reinit (icell);
        fe_values[pressure].get_function_values(present_solution,
                                                local_pressure_values);

        for (unsigned int i=0 ; i<local_pressure_values.size() ; ++i)
        {
            maxPressure=std::max(local_pressure_values[i],maxPressure);
        }

    }

    //loop over elements
    typename DoFHandler<dim>::active_cell_iterator
            cell = dof_handler.begin_active(),
            endc = dof_handler.end();
    for (; cell!=endc; ++cell)
    {
        fe_values.reinit (cell);
        fe_values[velocities].get_function_values (present_solution,
                                                   local_velocity_values);
        fe_values[pressure].get_function_values(present_solution,
                                               local_pressure_values);

        //Retrieve the effective "connectivity matrix" for this element
        cell->get_dof_indices (local_dof_indices);

        // Get the exact solution at all gauss points
        exact_solution->vector_value_list(fe_values.get_quadrature_points(),
                                            q_exactSol);

        for(unsigned int q=0; q<n_q_points; q++)
        {
            //Find the values of x and u_h (the finite element solution) at the quadrature points
            double ux_sim=local_velocity_values[q][0];
            double ux_exact=q_exactSol[q][0];

            double uy_sim=local_velocity_values[q][1];
            double uy_exact=q_exactSol[q][1];

            l2errorU += (ux_sim-ux_exact)*(ux_sim-ux_exact) * fe_values.JxW(q);
            l2errorU += (uy_sim-uy_exact)*(uy_sim-uy_exact) * fe_values.JxW(q);
        }
    }
    std::cout << "L2Error is : " << std::sqrt(l2errorU) << std::endl;
    L2ErrorU_.push_back(std::sqrt(l2errorU));
}


template<int dim>
void DirectSteadyNavierStokes<dim>::runMMS()
{
  std::cout << "**********************************************" << std::endl;
  std::cout << "* Method of Manufactured Solutions           *" << std::endl;
  std::cout << "**********************************************" << std::endl;
  simulationCase_=MMS;
  make_cube_grid(initialSize_);
  exact_solution = new ExactSolutionMMS<dim>;
  forcing_function = new MMSSineForcingFunction<dim>;
  viscosity_=1.;
  setup_dofs();

  for (unsigned int cycle =0; cycle < 5 ; cycle++)
  {
    if (cycle !=0) refine_mesh_uniform();
    newton_iteration(1.e-6, 5, true, true);
    output_results ("MMS-",cycle);
    calculateL2Error();

  }
  std::ofstream output_file("./L2Error.dat");
  for (unsigned int i=0 ; i < L2ErrorU_.size() ; ++i)
  {
    output_file << i+initialSize_ << " " << L2ErrorU_[i] << std::endl;
  }
  output_file.close();
}

template<int dim>
void DirectSteadyNavierStokes<dim>::runCouetteX()
{
  std::cout << "**********************************************" << std::endl;
  std::cout << "* Couette X                                  *" << std::endl;
  std::cout << "**********************************************" << std::endl;
  simulationCase_=CouetteX;
  GridGenerator::hyper_cube (triangulation, 0, 1,true);
  forcing_function = new NoForce<dim>;
  triangulation.refine_global (3);
  exact_solution = new ExactSolutionCouetteX<dim>;
  viscosity_=1.;
  setup_dofs();

  newton_iteration(1.e-6, 5, true, true);
  output_results ("Couette-X-",0);
  calculateL2Error();
}

template<int dim>
void DirectSteadyNavierStokes<dim>::runCouetteXPerturbedMesh()
{
  std::cout << "**********************************************" << std::endl;
  std::cout << "* Couette X  - Perturbed Mesh                *" << std::endl;
  std::cout << "**********************************************" << std::endl;
  simulationCase_=CouetteX;
  GridGenerator::hyper_cube (triangulation, 0, 1,true);
  forcing_function = new NoForce<dim>;
  triangulation.refine_global (3);
  GridTools::distort_random(0.3,triangulation);
  exact_solution = new ExactSolutionCouetteX<dim>;
  viscosity_=1.;
  setup_dofs();

  newton_iteration(1.e-6, 5, true, true);
  output_results ("Couette-X-Perturbed-",0);
  calculateL2Error();
}


template<int dim>
void DirectSteadyNavierStokes<dim>::runTaylorCouette()
{
    viscosity_=10;
    GridIn<dim> grid_in;
    grid_in.attach_triangulation (triangulation);
    std::ifstream input_file("taylorcouette.msh");

    grid_in.read_msh(input_file);


    static const SphericalManifold<dim> boundary;

    triangulation.set_all_manifold_ids_on_boundary(0);
    triangulation.set_manifold (0, boundary);

    forcing_function = new NoForce<dim>;
    exact_solution = new ExactSolutionTaylorCouette<dim>;
    setup_dofs();




    for (int cycle=0 ; cycle < 4 ; cycle++)
    {
        if (cycle !=0)  refine_mesh();
        newton_iteration(1.e-10, 50, true, true);
        output_results (cycle);
        calculateL2Error();
    }

    std::ofstream output_file("./L2Error.dat");
    for (unsigned int i=0 ; i < L2ErrorU_.size() ; ++i)
    {
        output_file << i+initialSize_ << " " << L2ErrorU_[i] << std::endl;
    }
    output_file.close();
}

template<int dim>
void DirectSteadyNavierStokes<dim>::runIBTaylorCouette()
{
    viscosity_=10;
    GridIn<dim> grid_in;
    grid_in.attach_triangulation (triangulation);
    std::ifstream input_file("taylorcouette.msh");

    grid_in.read_msh(input_file);

    // Set-up the center, velocity and angular velocity of circle
    Point<2> center(0.2356,-0.0125);
    Tensor<1,2> velocity;
    velocity[0]=1.;
    velocity[1]=0.;
    Tensor<1,3> angular;
    angular[0]=0;
    angular[1]=0;
    angular[2]=0;
    double T_scal;
    T_scal=1;
    double radius1 =0.76891/2.;
    double radius2 =1.56841/2.;
    bool inside=0;
    // IB composer
    std::vector<IBLevelSetFunctions<2> *> ib_functions;
    // Add a shape to it
    IBLevelSetCircle<2> circle1(center,velocity,angular, T_scal, inside, radius1);
    IBLevelSetCircle<2> circle2(center,velocity,angular, T_scal, !inside, radius2);

    ib_functions.push_back(&circle1);
    ib_functions.push_back(&circle2);

    IBCombiner<dim>              ib_combiner(ib_functions);



    static const SphericalManifold<dim> boundary;

    triangulation.set_all_manifold_ids_on_boundary(0);
    triangulation.set_manifold (0, boundary);

    forcing_function = new NoForce<dim>;
    exact_solution = new ExactSolutionTaylorCouette<dim>;
    setup_dofs();




    for (int cycle=0 ; cycle < 4 ; cycle++)
    {
        if (cycle !=0)  refine_mesh();
        newton_iteration(1.e-10, 50, true, true);
        output_results (cycle);
        calculateL2Error();
    }

    std::ofstream output_file("./L2Error.dat");
    for (unsigned int i=0 ; i < L2ErrorU_.size() ; ++i)
    {
        output_file << i+initialSize_ << " " << L2ErrorU_[i] << std::endl;
    }
    output_file.close();
}

template<int dim>
void DirectSteadyNavierStokes<dim>::runCouetteIBX()
{
  std::cout << "**********************************************" << std::endl;
  std::cout << "* Couette IB X                                  *" << std::endl;
  std::cout << "**********************************************" << std::endl;
  simulationCase_=CouetteX;
  GridGenerator::hyper_cube (triangulation, 0, 1,true);
  forcing_function = new NoForce<dim>;
  triangulation.refine_global (3);
  exact_solution = new ExactSolutionCouetteX<dim>;
  viscosity_=1.;
  setup_dofs();

  newton_iteration(1.e-6, 5, true, true);
  output_results ("Couette-X-",0);
  calculateL2Error();
}



int main ()
{
    try
    {

      {
        DirectSteadyNavierStokes<2> problem_2d(1,1);
        problem_2d.runCouetteX();
      }
      {
        DirectSteadyNavierStokes<2> problem_2d(1,1);
        problem_2d.runCouetteXPerturbedMesh();
      }
      {
        DirectSteadyNavierStokes<2> problem_2d(1,1);
        problem_2d.runCouetteIBX();
      }
      {
      DirectSteadyNavierStokes<2> problem_2d(1,1);
      problem_2d.runMMS();
      }
    }
    catch (std::exception &exc)
    {
        std::cerr << std::endl << std::endl
                  << "----------------------------------------------------"
                  << std::endl;
        std::cerr << "Exception on processing: " << std::endl
                  << exc.what() << std::endl
                  << "Aborting!" << std::endl
                  << "----------------------------------------------------"
                  << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << std::endl << std::endl
                  << "----------------------------------------------------"
                  << std::endl;
        std::cerr << "Unknown exception!" << std::endl
                  << "Aborting!" << std::endl
                  << "----------------------------------------------------"
                  << std::endl;
        return 1;
    }
    return 0;
}