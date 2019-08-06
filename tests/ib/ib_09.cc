//BASE
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/utilities.h>
#include <deal.II/base/index_set.h>

//NUMERICS
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>

//GRID
#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/manifold_lib.h>

//LACS
#include <deal.II/lac/trilinos_vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/precondition.h>


//DOFS
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>


// Distributed
#include <deal.II/distributed/tria.h>

#include "ibcombiner.h"
#include "iblevelsetfunctions.h"
#include "write_data.h"
#include "../tests.h"

// Mes ajouts so far
#include "nouvtriangles.h"

#include "T_integration_class.h"

using namespace dealii;

double calculate_L2_error(int refinement)

// solves the heat equation between 2 circles, T set to 2 on the ext circle and to 1 on the int circles
// calculates the L2 norm of (T_analytical - T_calculated)

{
    MPI_Comm                         mpi_communicator(MPI_COMM_WORLD);
    unsigned int n_mpi_processes (Utilities::MPI::n_mpi_processes(mpi_communicator));
    unsigned int this_mpi_process (Utilities::MPI::this_mpi_process(mpi_communicator));

    // Create triangulation and square mesh
    parallel::distributed::Triangulation<2> triangulation (mpi_communicator, typename Triangulation<2>::MeshSmoothing
                                                           (Triangulation<2>::smoothing_on_refinement | Triangulation<2>::smoothing_on_coarsening));
    GridGenerator::hyper_cube (triangulation,
                               -2,2,false);
    triangulation.refine_global(refinement);

    DoFHandler<2>                  dof_handler(triangulation);
    FESystem<2>                    fe(FE_Q<2>(1),1);
    dof_handler.distribute_dofs(fe);

    // Set-up global system of equation
    SparsityPattern      sparsity_pattern;
    SparseMatrix<double> system_matrix;

    Vector<double>       solution;
    Vector<double>       system_rhs;

    DynamicSparsityPattern dsp(dof_handler.n_dofs());
    DoFTools::make_sparsity_pattern (dof_handler, dsp);

    // Initialize vector and sparsity patterns
    sparsity_pattern.copy_from(dsp);
    system_matrix.reinit (sparsity_pattern);
    solution.reinit (dof_handler.n_dofs());
    system_rhs.reinit (dof_handler.n_dofs());

    // Quadrature formula for the element
    QGauss<2>            quadrature_formula(2);
    const unsigned int   dofs_per_cell = fe.dofs_per_cell;
    const unsigned int   n_q_points = quadrature_formula.size();

    // Matrix and RHS sides;
    FullMatrix<double>   cell_mat (dofs_per_cell, dofs_per_cell);
    Vector<double>       elem_rhs (dofs_per_cell);

    // Get the position of the support points
    const MappingQ<2>      mapping (1);

    FEValues<2> fe_values (mapping,
                           fe,
                           quadrature_formula,
                           update_values   | update_gradients |
                           update_quadrature_points | update_JxW_values);

    std::map< types::global_dof_index, Point< 2 > > support_points;
    DoFTools::map_dofs_to_support_points ( mapping, dof_handler,support_points );

    // Instantiations for the decomposition of the elements
    std::vector<int>                     corresp(9);
    std::vector<Point<2> >               decomp_elem(9);         // Array containing the points of the new elements created by decomposing the elements crossed by the boundary fluid/solid, there are up to 9 points that are stored in it
    std::vector<node_status>             No_pts_solid(4);
    int                                  nb_poly;                   // Number of sub-elements created in the fluid part for each element ( 0 if the element is entirely in the solid or the fluid)
    std::vector<Point<2> >               num_elem(6);

    // Set the values of the dof points position
    std::vector<double>                  distance(dofs_per_cell); // Array for the distances associated with the DOFS
    std::vector<Point<2> >               dofs_points(dofs_per_cell);// Array for the DOFs points
    std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell); // Global DOFs indices corresponding to cell

  // Set-up the center, velocity and angular velocity of circle

  double abscisse = 0.25; // appartient à la frontière supérieure

  // Set-up the center, velocity and angular velocity of circle
  Point<2> center(0.2356,-0.0125);
  Tensor<1,2> velocity;
  velocity[0]=1.;
  velocity[1]=0.;
  Tensor<1,3> angular;
  angular[0]=0;
  angular[1]=0;
  angular[2]=0;
  double T_scal1, T_scal2;
  T_scal1=1;
  T_scal2=2;

  double radius1 =0.76891;
  double radius2 =1.56841;
  bool inside=0;

  // IB composer
  std::vector<IBLevelSetFunctions<2> *> ib_functions;
  // Add a shape to it
  IBLevelSetCircle<2> circle1(center,velocity,angular, T_scal1,  inside, radius1);
  IBLevelSetCircle<2> circle2(center,velocity,angular, T_scal2, !inside, radius2);

  ib_functions.push_back(&circle1);
  ib_functions.push_back(&circle2);

  IBCombiner<2> ib_combiner(ib_functions);

  double            Tdirichlet;
  Point<2> center_elem;

  // Tool for the integration
  Heat_integration_circles  H_i_c;

  typename DoFHandler<2>::active_cell_iterator
  cell = dof_handler.begin_active(),
  endc = dof_handler.end();
  for (; cell!=endc; ++cell)
  {
    std::fill(elem_rhs.begin(), elem_rhs.end(), 0.);
    cell_mat = 0;

    center_elem[0]=0;
    center_elem[1]=0;

    if (cell->is_locally_owned())
    {
      fe_values.reinit(cell);
      cell->get_dof_indices (local_dof_indices);

      // we get the value of the distance function (signed distance to the boundary solid/fluid, positive in the fluid) and the position of the vertices of the element

      for (unsigned int dof_index=0 ; dof_index < local_dof_indices.size() ; ++dof_index)
      {
        dofs_points[dof_index] = support_points[local_dof_indices[dof_index]];
        distance[dof_index]=ib_combiner.value(dofs_points[dof_index]);
        center_elem[0] += dofs_points[dof_index][0]/4;
        center_elem[1] += dofs_points[dof_index][1]/4;
      }

      // center_elem is the point located at the barycenter of the square element
      Tdirichlet = ib_combiner.scalar(center_elem);

      // we apply the decomposition function to determine if the cell is crossed by the solid-fluid boundary

      decomposition(corresp, No_pts_solid, num_elem, decomp_elem, &nb_poly, dofs_points, distance);

      // we set the H_i_c object for this cell
      H_i_c.set_decomp(decomp_elem);
      H_i_c.set_nb_poly(nb_poly);
      H_i_c.set_corresp(corresp);
      H_i_c.set_pts_status(No_pts_solid);

      if (nb_poly==0) // case where there is no decomposition
      {
          if (distance[0]>0) // the cell is entirely in the fluid
              for (int i = 0; i < 4; ++i) {
                  for (int j = 0; j < 4; ++j) {
                      for (unsigned int q_index=0; q_index<n_q_points; ++q_index)
                      {
                          cell_mat[i][j] += fe_values.shape_grad(i, q_index) * fe_values.shape_grad (j, q_index) * fe_values.JxW (q_index);
                      }
                  }
                      for (unsigned int q_index=0; q_index<n_q_points; ++q_index)
                      {
                          elem_rhs[i] += 0 ;
                      }
              }
          else // the cell is entirely in the solid
          {
              //in that case we set T to be Tdirichlet in the solid, so we put zeros in the matrix, we set its diagonal to be full of ones, and we put Tdirichlet as rhs

              for (int i = 0; i < 4; ++i) {
                  for (int j = 0; j < 4; ++j) {
                        if (i==j)
                          cell_mat[i][j] = 1;
                        else {
                          cell_mat[i][j] = 0;
                        }

                  }
                  elem_rhs[i] += Tdirichlet;
             }
          }
      }

      else {

          // H_i_c.T_integrate_IB evaluates the elementary matrix and elementary rhs in the cases where we decompose the element
          H_i_c.T_integrate_IB(Tdirichlet, cell_mat, elem_rhs);
      }


    for (unsigned int i=0; i<dofs_per_cell; ++i)
      for (unsigned int j=0; j<dofs_per_cell; ++j)
        system_matrix.add (local_dof_indices[i],
                           local_dof_indices[j],
                           cell_mat[i][j]);


    for (unsigned int i=0; i<dofs_per_cell; ++i)
      system_rhs(local_dof_indices[i]) += elem_rhs[i];
    }
  }

  std::map<types::global_dof_index,double> boundary_values;

  VectorTools::interpolate_boundary_values (dof_handler,
                                            1,
                                             Functions::ConstantFunction<2>(2.),
                                            boundary_values);

  MatrixTools::apply_boundary_values (boundary_values,
                                      system_matrix,
                                      solution,
                                      system_rhs);


  SolverControl           solver_control (3000, 1e-12);
  SolverCG<>              solver (solver_control);
  solver.solve (system_matrix, solution, system_rhs,
                PreconditionIdentity());

  double err=0; // the soon-to-be L2 error
  std::vector<double> T(6); // vector in which we will store the values of the solution at each vertex plus at the boundary points if we decompose

  // We have calculated the solution, we now want to determine the L2-error on it, as we know the analytical solution
  // since we have to calculate integrals again, we will sum the values on each element

  cell = dof_handler.begin_active();
  for (; cell!=endc; ++cell)
  {
      fe_values.reinit(cell);
      cell->get_dof_indices (local_dof_indices);

      // getting the distance to the boundary and the point associated to each dof
      for (unsigned int dof_index=0 ; dof_index < local_dof_indices.size() ; ++dof_index)
      {
        dofs_points[dof_index] = support_points[local_dof_indices[dof_index]];
        distance[dof_index]= ib_combiner.value(dofs_points[dof_index]);

        // we fill the vector of the values of the solution on each vertex
        T[dof_index]=solution[local_dof_indices[dof_index]];
      }

      // we decompose

      decomposition(corresp, No_pts_solid, num_elem, decomp_elem, &nb_poly, dofs_points, distance);

      // we set the H_i_c object for this cell
      H_i_c.set_decomp(decomp_elem);
      H_i_c.set_nb_poly(nb_poly);
      H_i_c.set_corresp(corresp);
      H_i_c.set_pts_status(No_pts_solid);

      if (nb_poly==0)
      {
          if (distance[0]>0){
              for (unsigned int q=0; q<n_q_points; q++) {
                err+=std::pow(T_analytical(fe_values.quadrature_point (q), center, T_scal1, T_scal2, radius1, radius2)-T_calc_interp_quad(T, fe_values.quadrature_point (q)), 2)
                        *fe_values.JxW(q);
              }
          }

      }

      else {

        T[4]=T[No_pts_solid[0]];
        T[5]=T[No_pts_solid[0]];
        // T is set to Tdirichlet on the boundary, indices 4 and 5 refer to the boundary points calculated by "decomposition"

        // H_i_c.T_L2_norm_IB evaluates the error for a decomposed element
        err+=H_i_c.T_L2_norm_IB(center, T_scal1, T_scal2, radius1, radius2, T);
      }
  }
  std::cout << err << std::endl;
  return err;
}

void test_L2()
{
    Vector<double>      Error;
    Vector<double>      Error_theo;
    Error.reinit(5);
    Error_theo.reinit(5);

    for (int i = 4; i < 9; ++i) {
        Error(i-4)=calculate_L2_error(i);
    }

    Error_theo(0)=0.47656;
    Error_theo(1)=0.166937;
    Error_theo(2)=0.0475146;
    Error_theo(3)=0.0126869;
    Error_theo(4)=0.00325879;

    double acc = 1e-5;

    for (int i = 0; i < 5; ++i) {
        if (std::abs(Error(i)-Error_theo(i))>acc*std::pow(0.25,i)) throw std::runtime_error("Failed to calculate the L2 error");
    }
    std::cout << " Tests passed successfully" << std::endl;
}


int main(int argc, char* argv[])
{
  try
  {
    Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv, numbers::invalid_unsigned_int);
    initlog();
    test_L2();
  }
  catch (std::exception &exc)
  {
    std::cerr << std::endl << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Exception on processing: " << std::endl
              << exc.what()  << std::endl
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
