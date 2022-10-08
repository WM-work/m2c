#ifndef _M2C_TWIN_MESSENGER_H_
#define _M2C_TWIN_MESSENGER_H_

#include<IoData.h>
#include<SpaceVariable.h>
#include<GhostPoint.h>
#include<GlobalMeshInfo.h>

/*************************************************************
 * Class M2CTwinMessenger is responsible for communicating with
 * the M2C Twin solver in an implementation of the overset grids
 * method. Both of the twins will activate this class.
 ************************************************************/

class M2CTwinMessenger {

  IoData &iod;

  MPI_Comm &m2c_comm; //!< This is the M2C communicator
  MPI_Comm &joint_comm; //!< This is the joint communicator of M2C and M2C Twin
  int m2c_rank, m2c_size; 

  enum TwinningStatus {LEADER = 1, FOLLOWER = 2} twinning_status; 

  SpaceVariable3D *coordinates;
  std::vector<GhostPoint> *ghost_nodes_outer;
  GlobalMeshInfo *global_mesh;

  std::vector<double> temp_buffer;

  //! For both the ``leader'' and the ``follower'', one package for each remote processor
  std::vector<std::vector<Int3> > import_nodes;
  std::vector<std::vector<GhostPoint> > export_points;


public:

  M2CTwinMessenger(IoData &iod_, MPI_Comm &m2c_comm_, MPI_Comm &joint_comm_, int status_);
  ~M2CTwinMessenger();
  void Destroy();

  //! Exchange data w/ M2C Twin (called before the first time step)
  void CommunicateBeforeTimeStepping(SpaceVariable3D &coordinates_,
                                     vector<GhostPoint> &ghost_nodes_outer_,
                                     GlobalMeshInfo &global_mesh_);

  //! Exchange data w/ M2C Twin (called at the first time step)
  void FirstExchange();

  //! Exchange data w/ M2C Twin (called at every time step except first and last)
  void Exchange();

  //! Exchange data w/ M2C Twin (called at the last time step)
  void FinalExchange();

};

#endif
