#ifndef _PROBE_OUTPUT_H_
#define _PROBE_OUTPUT_H_
#include <IoData.h>
#include <SpaceVariable.h>

/** This class is responsible for interpolating solutions at probe locations and outputing
 *  the interpolated solutions to files. It is owned by class Output
 *  It is also responsible for line output, in which case the "probe" nodes are sampled
 *  uniformly along the line. For explicitly specified probe nodes, each solution variable 
 *  (e.g., density, pressure) is written to a separate file. For line output, all the solution
 *  variables along a line are written to one file at each time of output.
 */
class ProbeOutput {

  MPI_Comm &comm;
  OutputData &iod_output;

  int numNodes;
  int frequency;

  std::vector<Vec3D> locations;
  FILE *file[Probes::SIZE]; //!< one file per solution variable

  int line_number; //!< only used if the probes are along a line

  //! For each probe node, ijk are the lower nodal indices of the element that contains the node
  std::vector<Int3> ijk;
  //! For each probe node, trilinear_coords contains the local x,y,z coordinates w/i the element
  std::vector<Vec3D> trilinear_coords;

public:
  //! Constructor 1: write probe info to file. 
  ProbeOutput(MPI_Comm &comm_, OutputData &iod_output_);
  //! Constructor 2: Probe is part of line_plot 
  ProbeOutput(MPI_Comm &comm_, OutputData &iod_output_, int line_number); 

  ~ProbeOutput();

  void SetupInterpolation(SpaceVariable3D &coordinates);

  void WriteSolutionAtProbes(double time, int time_step, SpaceVariable3D &V, SpaceVariable3D &ID,
           std::vector<SpaceVariable3D*> &Phi, bool must_write = false); //!< write probe solution to file

  void WriteAllSolutionsAlongLine(double time, int time_step, SpaceVariable3D &V, SpaceVariable3D &ID,
           std::vector<SpaceVariable3D*> &Phi, bool must_write = false);

private:
  double InterpolateSolutionAtProbe(Int3& ijk, Vec3D &trilinear_coords, double ***v, int dim, int p);

};




#endif