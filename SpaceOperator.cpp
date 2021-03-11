#include <Utils.h>
#include <SpaceOperator.h>
#include <Vector3D.h>
#include <Vector5D.h>
#include <algorithm> //std::upper_bound
#include <cfloat> //DBL_MAX
using std::cout;
using std::endl;
using std::max;
using std::min;

//-----------------------------------------------------

SpaceOperator::SpaceOperator(MPI_Comm &comm_, DataManagers3D &dm_all_, IoData &iod_,
                             vector<VarFcnBase*> &varFcn_, FluxFcnBase &fluxFcn_,
                             ExactRiemannSolverBase &riemann_) 
  : comm(comm_), iod(iod_), varFcn(varFcn_), fluxFcn(fluxFcn_), riemann(riemann_),
    coordinates(comm_, &(dm_all_.ghosted1_3dof)),
    delta_xyz(comm_, &(dm_all_.ghosted1_3dof)),
    volume(comm_, &(dm_all_.ghosted1_1dof)),
    rec(comm_, dm_all_, iod_.schemes.ns.rec, coordinates, delta_xyz),
    Vl(comm_, &(dm_all_.ghosted1_5dof)),
    Vr(comm_, &(dm_all_.ghosted1_5dof)),
    Vb(comm_, &(dm_all_.ghosted1_5dof)),
    Vt(comm_, &(dm_all_.ghosted1_5dof)),
    Vk(comm_, &(dm_all_.ghosted1_5dof)),
    Vf(comm_, &(dm_all_.ghosted1_5dof))
{
  
  coordinates.GetCornerIndices(&i0, &j0, &k0, &imax, &jmax, &kmax);
  coordinates.GetGhostedCornerIndices(&ii0, &jj0, &kk0, &iimax, &jjmax, &kkmax);

  SetupMesh();

  rec.Setup(); //this function requires mesh info (dxyz)
  
}

//-----------------------------------------------------

SpaceOperator::~SpaceOperator()
{

}

//-----------------------------------------------------

void SpaceOperator::Destroy()
{
  rec.Destroy();
  coordinates.Destroy();
  delta_xyz.Destroy();
  volume.Destroy();
  Vl.Destroy();
  Vr.Destroy();
  Vb.Destroy();
  Vt.Destroy();
  Vk.Destroy();
  Vf.Destroy();
}

//-----------------------------------------------------

void SpaceOperator::SetupMesh()
{
  //! Setup coordinates of cell centers and dx, dy, dz
  if(true)
    SetupMeshUniformRectangularDomain();
  //! TODO: add more choices later


  //! Compute mesh information
  Vec3D***  dxyz = (Vec3D***)delta_xyz.GetDataPointer();
  double*** vol  = (double***)volume.GetDataPointer();


  /** Calculate the volume/area of node-centered control volumes ("cells")
   *  Include ghost cells. 
   */
  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {
        vol[k][j][i] /*volume of cv*/ = dxyz[k][j][i][0]*dxyz[k][j][i][1]*dxyz[k][j][i][2];
//        fprintf(stderr,"(%d,%d,%d), dx = %e, dy = %e, dz = %e, vol = %e.\n", i,j,k, dxyz[k][j][i][0], dxyz[k][j][i][1], dxyz[k][j][i][2], vol[k][j][i]);
      }

  delta_xyz.RestoreDataPointerAndInsert();
  volume.RestoreDataPointerAndInsert();
}

//-----------------------------------------------------

void SpaceOperator::SetupMeshUniformRectangularDomain()
{
  int NX, NY, NZ;
  coordinates.GetGlobalSize(&NX, &NY, &NZ);

  double dx = (iod.mesh.xmax - iod.mesh.x0)/NX;
  double dy = (iod.mesh.ymax - iod.mesh.y0)/NY;
  double dz = (iod.mesh.zmax - iod.mesh.z0)/NZ;

  //! get array to edit
  Vec3D*** coords = (Vec3D***)coordinates.GetDataPointer();
  Vec3D*** dxyz   = (Vec3D***)delta_xyz.GetDataPointer();

  //! Fill the actual subdomain, w/o ghost cells 
  for(int k=k0; k<kmax; k++)
    for(int j=j0; j<jmax; j++)
      for(int i=i0; i<imax; i++) {
        coords[k][j][i][0] = iod.mesh.x0 + 0.5*dx + i*dx; 
        coords[k][j][i][1] = iod.mesh.y0 + 0.5*dy + j*dy; 
        coords[k][j][i][2] = iod.mesh.z0 + 0.5*dz + k*dz; 
        dxyz[k][j][i][0] = dx;
        dxyz[k][j][i][1] = dy;
        dxyz[k][j][i][2] = dz;
      } 

  //! restore array
  coordinates.RestoreDataPointerAndInsert(); //update localVec and globalVec;
  delta_xyz.RestoreDataPointerAndInsert(); //update localVec and globalVec;

  //! Populate the ghost cells (coordinates, dx, dy, dz)
  PopulateGhostBoundaryCoordinates();
}

//-----------------------------------------------------
/** Populate the coordinates, dx, dy, and dz of ghost cells */
void SpaceOperator::PopulateGhostBoundaryCoordinates()
{
  Vec3D*** v    = (Vec3D***) coordinates.GetDataPointer();
  Vec3D*** dxyz = (Vec3D***) delta_xyz.GetDataPointer();

  int nnx, nny, nnz, NX, NY, NZ;
  coordinates.GetGhostedSize(&nnx, &nny, &nnz);
  coordinates.GetGlobalSize(&NX, &NY, &NZ);

  // capture the mesh info of the corners 
  double v0[3], v1[3];
  double dxyz0[3], dxyz1[3];
  for(int p=0; p<3; p++) {
    v0[p]    = v[k0][j0][i0][p] - dxyz[k0][j0][i0][p];
    v1[p]    = v[kmax-1][jmax-1][imax-1][p] + dxyz[kmax-1][jmax-1][imax-1][p];
    dxyz0[p] = dxyz[k0][j0][i0][p];
    dxyz1[p] = dxyz[kmax-1][jmax-1][imax-1][p];
  }

  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {

        if(k!=-1 && k!=NZ && j!=-1 && j!=NY && i!=-1 && i!=NX)
          continue; //not in the ghost layer of the physical domain

        Vec3D& X  = v[k][j][i];
        Vec3D& dX = dxyz[k][j][i];

        bool xdone = false, ydone = false, zdone = false;

        if(i==-1) {
          X[0]  = v0[0];
          dX[0] = dxyz0[0];
          xdone = true;
        }

        if(i==NX) {
          X[0]  = v1[0];
          dX[0] = dxyz1[0];
          xdone = true;
        }

        if(j==-1) {
          X[1]  = v0[1];
          dX[1] = dxyz0[1];
          ydone = true;
        }

        if(j==NY) {
          X[1]  = v1[1];
          dX[1] = dxyz1[1];
          ydone = true;
        }

        if(k==-1) {
          X[2]  = v0[2];
          dX[2] = dxyz0[2];
          zdone = true;
        }

        if(k==NZ) {
          X[2]  = v1[2];
          dX[2] = dxyz1[2];
          zdone = true;
        }

        if(!xdone) {
          X[0]  = v[k0][j0][i][0];    //x[i]
          dX[0] = dxyz[k0][j0][i][0]; //dx[i]
        }

        if(!ydone) {
          X[1]  = v[k0][j][i0][1];    //y[j]
          dX[1] = dxyz[k0][j][i0][1]; //dy[j]
        }

        if(!zdone) {
          X[2]  = v[k][j0][i0][2];    //z[k]
          dX[2] = dxyz[k][j0][i0][2]; //dz[k]
        }

      }
  
  coordinates.RestoreDataPointerAndInsert();
  delta_xyz.RestoreDataPointerAndInsert();
}

//-----------------------------------------------------

void SpaceOperator::ConservativeToPrimitive(SpaceVariable3D &U, SpaceVariable3D &ID, SpaceVariable3D &V,
                                            bool workOnGhost)
{
  Vec5D*** u = (Vec5D***) U.GetDataPointer();
  Vec5D*** v = (Vec5D***) V.GetDataPointer();
  double*** id = (double***) ID.GetDataPointer();

  int myi0, myj0, myk0, myimax, myjmax, mykmax;
  if(workOnGhost)
    U.GetGhostedCornerIndices(&myi0, &myj0, &myk0, &myimax, &myjmax, &mykmax);
  else
    U.GetCornerIndices(&myi0, &myj0, &myk0, &myimax, &myjmax, &mykmax);

  for(int k=myk0; k<mykmax; k++)
    for(int j=myj0; j<myjmax; j++)
      for(int i=myi0; i<myimax; i++)
        varFcn[id[k][j][i]]->ConservativeToPrimitive((double*)u[k][j][i], (double*)v[k][j][i]); 

  U.RestoreDataPointerToLocalVector(); //no changes made
  V.RestoreDataPointerAndInsert();
  ID.RestoreDataPointerToLocalVector(); //no changes made
}

//-----------------------------------------------------

void SpaceOperator::PrimitiveToConservative(SpaceVariable3D &V, SpaceVariable3D &ID, SpaceVariable3D &U, 
                                            bool workOnGhost)
{
  Vec5D*** v = (Vec5D***) V.GetDataPointer();
  Vec5D*** u = (Vec5D***) U.GetDataPointer();
  double*** id = (double***) ID.GetDataPointer();

  int myi0, myj0, myk0, myimax, myjmax, mykmax;
  if(workOnGhost)
    U.GetGhostedCornerIndices(&myi0, &myj0, &myk0, &myimax, &myjmax, &mykmax);
  else
    U.GetCornerIndices(&myi0, &myj0, &myk0, &myimax, &myjmax, &mykmax);

  for(int k=myk0; k<mykmax; k++)
    for(int j=myj0; j<myjmax; j++)
      for(int i=myi0; i<myimax; i++)
        varFcn[id[k][j][i]]->PrimitiveToConservative((double*)v[k][j][i], (double*)u[k][j][i]); 

  V.RestoreDataPointerToLocalVector(); //no changes made
  U.RestoreDataPointerAndInsert();
  ID.RestoreDataPointerToLocalVector(); //no changes made
}

//-----------------------------------------------------

int SpaceOperator::ClipDensityAndPressure(SpaceVariable3D &V, SpaceVariable3D &ID, 
                                          bool workOnGhost, bool checkState)
{

  Vec5D*** v = (Vec5D***) V.GetDataPointer();
  double*** id = (double***) ID.GetDataPointer();

  int myi0, myj0, myk0, myimax, myjmax, mykmax;
  if(workOnGhost)
    V.GetGhostedCornerIndices(&myi0, &myj0, &myk0, &myimax, &myjmax, &mykmax);
  else
    V.GetCornerIndices(&myi0, &myj0, &myk0, &myimax, &myjmax, &mykmax);

  int nClipped = 0;
  for(int k=myk0; k<mykmax; k++) {
    for(int j=myj0; j<myjmax; j++) {
      for(int i=myi0; i<myimax; i++) {

        nClipped += (int)varFcn[id[k][j][i]]->ClipDensityAndPressure(v[k][j][i]);

        if(checkState) {
          if(varFcn[id[k][j][i]]->CheckState(v[k][j][i])) {
            print_error("Error: State variables at (%d,%d,%d) violate hyperbolicity. matid = %d.\n", i,j,k, id[k][j][i]);
            fprintf(stderr,"v[%d,%d,%d] = [%e, %e, %e, %e, %e]\n", i,j,k, v[k][j][i][0], v[k][j][i][1], v[k][j][i][2], v[k][j][i][3], v[k][j][i][4]);
            exit_mpi();
          }
        }
      }
    }
  }

  MPI_Allreduce(MPI_IN_PLACE, &nClipped, 1, MPI_INT, MPI_SUM, comm);
  if(nClipped)
    print("Warning: Clipped pressure and/or density in %d cells.\n", nClipped);


  ID.RestoreDataPointerToLocalVector(); //no changes made
  V.RestoreDataPointerAndInsert();

  return nClipped;
}  

//-----------------------------------------------------

//apply IC within the real domain
void SpaceOperator::SetInitialCondition(SpaceVariable3D &V, SpaceVariable3D &ID) 
{
  Vec3D*** coords = (Vec3D***)coordinates.GetDataPointer();

  Vec5D*** v = (Vec5D***) V.GetDataPointer();
  double*** id = (double***) ID.GetDataPointer();

  //! 1. apply the inlet (i.e. farfield) state
  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {
        v[k][j][i][0] = iod.bc.inlet.density;
        v[k][j][i][1] = iod.bc.inlet.velocity_x;
        v[k][j][i][2] = iod.bc.inlet.velocity_y;
        v[k][j][i][3] = iod.bc.inlet.velocity_z;
        v[k][j][i][4] = iod.bc.inlet.pressure;
        id[k][j][i]   = iod.bc.inlet.materialid;
      }



  //! 2. apply user-specified function
  if(iod.ic.type != IcData::NONE) {

    //! Get coordinates
    Vec3D    x0(iod.ic.x0[0], iod.ic.x0[1], iod.ic.x0[2]); 

    if (iod.ic.type == IcData::PLANAR || iod.ic.type == IcData::CYLINDRICAL) {

      if(iod.ic.type == IcData::PLANAR)
        print("- Applying file-based initial condition (planar).\n");
      else
        print("- Applying file-based initial condition (with cylindrical symmetry).\n");
 
      Vec3D dir(iod.ic.dir[0], iod.ic.dir[1], iod.ic.dir[2]); 
      dir /= dir.norm();

      double x = 0.0;
      int n = iod.ic.user_data[IcData::COORDINATE].size(); //!< number of data points provided by user (in the axial dir)

      double r = 0.0;
      int nrad = iod.ic.user_data2[IcData::COORDINATE].size(); //!< number of data points in the radial dir.

      int t0, t1;    
      double a0, a1;

      for(int k=k0; k<kmax; k++)
        for(int j=j0; j<jmax; j++)
          for(int i=i0; i<imax; i++) {

            x = (coords[k][j][i] - x0)*dir; //!< projection onto the 1D axis
//            cout << "coords: " << coords[j][i][0] << ", " << coords[j][i][1] << "; x = " << x << endl;
            if(x<0 || x>iod.ic.user_data[IcData::COORDINATE][n-1])
              continue;

            if(nrad>0) {
              r = (coords[k][j][i] - x0 - x*dir).norm();
              if(r>iod.ic.user_data2[IcData::COORDINATE][nrad-1])
                continue;
            }
 
            //! Find the first 1D coordinate greater than x
            auto upper_it = std::upper_bound(iod.ic.user_data[IcData::COORDINATE].begin(),
                                             iod.ic.user_data[IcData::COORDINATE].end(),
                                             x); 
            t1 = (int)(upper_it - iod.ic.user_data[IcData::COORDINATE].begin());

            if(t1==0) // exactly the first node in 1D
              t1 = 1;

            t0 = t1 - 1;

            //! calculate interpolation weights
            a0 = (iod.ic.user_data[IcData::COORDINATE][t1] - x) /
                 (iod.ic.user_data[IcData::COORDINATE][t1] - iod.ic.user_data[IcData::COORDINATE][t0]);
            a1 = 1.0 - a0;

            //! specify i.c. on node (cell center)
            if(iod.ic.specified[IcData::DENSITY])
              v[k][j][i][0] = a0*iod.ic.user_data[IcData::DENSITY][t0]  
                            + a1*iod.ic.user_data[IcData::DENSITY][t1];
            if(iod.ic.specified[IcData::VELOCITY]) {
              v[k][j][i][1] = (a0*iod.ic.user_data[IcData::VELOCITY][t0] 
                            +  a1*iod.ic.user_data[IcData::VELOCITY][t1])*dir[0];
              v[k][j][i][2] = (a0*iod.ic.user_data[IcData::VELOCITY][t0]
                            +  a1*iod.ic.user_data[IcData::VELOCITY][t1])*dir[1];
              v[k][j][i][3] = (a0*iod.ic.user_data[IcData::VELOCITY][t0] 
                            +  a1*iod.ic.user_data[IcData::VELOCITY][t1])*dir[2];
            }
            if(iod.ic.specified[IcData::PRESSURE]) 
              v[k][j][i][4] = a0*iod.ic.user_data[IcData::PRESSURE][t0]
                            + a1*iod.ic.user_data[IcData::PRESSURE][t1];
            if(iod.ic.specified[IcData::MATERIALID])
              id[k][j][i]   = std::round(a0*iod.ic.user_data[IcData::MATERIALID][t0] 
                                       + a1*iod.ic.user_data[IcData::MATERIALID][t1]);

            //! apply radial variation (if provided by user)
            if(nrad>0) { 
 
              //! Find the first radial coordinate greater than r
              auto upper_it = std::upper_bound(iod.ic.user_data2[IcData::COORDINATE].begin(),
                                               iod.ic.user_data2[IcData::COORDINATE].end(),
                                               r); 
              t1 = (int)(upper_it - iod.ic.user_data2[IcData::COORDINATE].begin());

              if(t1==0) // exactly the first node
                t1 = 1;

              t0 = t1 - 1;

              //! calculate interpolation weights
              a0 = (iod.ic.user_data2[IcData::COORDINATE][t1] - r) /
                   (iod.ic.user_data2[IcData::COORDINATE][t1] - iod.ic.user_data2[IcData::COORDINATE][t0]);
              a1 = 1.0 - a0;

              if(iod.ic.specified[IcData::DENSITY])
                v[k][j][i][0] *= a0*iod.ic.user_data2[IcData::DENSITY][t0] 
                               + a1*iod.ic.user_data2[IcData::DENSITY][t1];
              if(iod.ic.specified[IcData::VELOCITY])
                for(int p=1; p<=3; p++)
                  v[k][j][i][p] *= a0*iod.ic.user_data2[IcData::VELOCITY][t0] 
                                 + a1*iod.ic.user_data2[IcData::VELOCITY][t1];
              if(iod.ic.specified[IcData::PRESSURE])
                v[k][j][i][4] *= a0*iod.ic.user_data2[IcData::PRESSURE][t0]
                               + a1*iod.ic.user_data2[IcData::PRESSURE][t1];
            }
          }

    } 

    else if (iod.ic.type == IcData::SPHERICAL) {

      print("- Applying file-based initial condition (with spherical symmetry).\n");
 
      double x;
      int n = iod.ic.user_data[IcData::COORDINATE].size(); //!< number of data points provided by user
      Vec3D dir;

      int t0, t1;    
      double a0, a1;

      for(int k=k0; k<kmax; k++)
        for(int j=j0; j<jmax; j++)
          for(int i=i0; i<imax; i++) {

            dir = coords[k][j][i] - x0;
            x = dir.norm();
            dir /= x;
   
//            cout << "coords: " << coords[j][i][0] << ", " << coords[j][i][1] << "; x = " << x << endl;
            if(x>iod.ic.user_data[IcData::COORDINATE][n-1])
              continue;
 
            //! Find the first 1D coordinate greater than x
            auto upper_it = std::upper_bound(iod.ic.user_data[IcData::COORDINATE].begin(),
                                             iod.ic.user_data[IcData::COORDINATE].end(),
                                             x); 
            t1 = (int)(upper_it - iod.ic.user_data[IcData::COORDINATE].begin());

            if(t1==0) // exactly the first node in 1D
              t1 = 1;

            t0 = t1 - 1;

            //! calculate interpolation weights
            a0 = (iod.ic.user_data[IcData::COORDINATE][t1] - x) /
                 (iod.ic.user_data[IcData::COORDINATE][t1] - iod.ic.user_data[IcData::COORDINATE][t0]);
            a1 = 1.0 - a0;

            //! specify i.c. on node (cell center)
            if(iod.ic.specified[IcData::DENSITY])
              v[k][j][i][0] = a0*iod.ic.user_data[IcData::DENSITY][t0]
                            + a1*iod.ic.user_data[IcData::DENSITY][t1];
            if(iod.ic.specified[IcData::VELOCITY]) {
              v[k][j][i][1] = (a0*iod.ic.user_data[IcData::VELOCITY][t0]
                            +  a1*iod.ic.user_data[IcData::VELOCITY][t1])*dir[0];
              v[k][j][i][2] = (a0*iod.ic.user_data[IcData::VELOCITY][t0] 
                            +  a1*iod.ic.user_data[IcData::VELOCITY][t1])*dir[1];
              v[k][j][i][3] = (a0*iod.ic.user_data[IcData::VELOCITY][t0]
                            +  a1*iod.ic.user_data[IcData::VELOCITY][t1])*dir[2];
            }
            if(iod.ic.specified[IcData::PRESSURE])
              v[k][j][i][4] = a0*iod.ic.user_data[IcData::PRESSURE][t0]
                            + a1*iod.ic.user_data[IcData::PRESSURE][t1];
            if(iod.ic.specified[IcData::MATERIALID])
              id[k][j][i]   = std::round(a0*iod.ic.user_data[IcData::MATERIALID][t0] 
                                       + a1*iod.ic.user_data[IcData::MATERIALID][t1]);
          }

    }
  }



  //! 3. apply i.c. based on geometric objects (planes, cylinder-cones, spheres)
  MultiInitialConditionsData &ic(iod.ic.multiInitialConditions);

  // planes
  for(auto it=ic.planeMap.dataMap.begin(); it!=ic.planeMap.dataMap.end(); it++) {

    print("- Applying initial condition on one side of a plane (material id: %d).\n", 
          it->second->initialConditions.materialid);
    Vec3D x0(it->second->cen_x, it->second->cen_y, it->second->cen_z);
    Vec3D dir(it->second->nx, it->second->ny, it->second->nz);
    dir /= dir.norm();
    double dist;

    for(int k=k0; k<kmax; k++)
      for(int j=j0; j<jmax; j++)
        for(int i=i0; i<imax; i++) {
          dist = (coords[k][j][i]-x0)*dir;
          if(dist>0) {
            v[k][j][i][0] = it->second->initialConditions.density;
            v[k][j][i][1] = it->second->initialConditions.velocity_x;
            v[k][j][i][2] = it->second->initialConditions.velocity_y;
            v[k][j][i][3] = it->second->initialConditions.velocity_z;
            v[k][j][i][4] = it->second->initialConditions.pressure;
            id[k][j][i]   = it->second->initialConditions.materialid;
          }
        }
  }

  // cylinder-cone
  for(auto it=ic.cylinderconeMap.dataMap.begin(); it!=ic.cylinderconeMap.dataMap.end(); it++) {

    print("- Applying initial condition within a cylinder-cone (material id: %d).\n",
          it->second->initialConditions.materialid);
    Vec3D x0(it->second->cen_x, it->second->cen_y, it->second->cen_z);
    Vec3D dir(it->second->nx, it->second->ny, it->second->nz);
    dir /= dir.norm();

    double L = it->second->L; //cylinder height
    double R = it->second->r; //cylinder radius
    double tan_alpha = tan(it->second->opening_angle_degrees/180.0*acos(-1.0));//opening angle
    double Hmax = R/tan_alpha;
    double H = min(it->second->cone_height, Hmax); //cone's height

    double x, r;
    for(int k=k0; k<kmax; k++)
      for(int j=j0; j<jmax; j++)
        for(int i=i0; i<imax; i++) {
          x = (coords[k][j][i]-x0)*dir;
          r = (coords[k][j][i] - x0 - x*dir).norm();
          if( (x>0 && x<L && r<R) || (x>=L && x<L+H && r<(L+Hmax-x)*tan_alpha) ) {//inside
            v[k][j][i][0] = it->second->initialConditions.density;
            v[k][j][i][1] = it->second->initialConditions.velocity_x;
            v[k][j][i][2] = it->second->initialConditions.velocity_y;
            v[k][j][i][3] = it->second->initialConditions.velocity_z;
            v[k][j][i][4] = it->second->initialConditions.pressure;
            id[k][j][i]   = it->second->initialConditions.materialid;
          }
        }
  }


  // spheres
  for(auto it=ic.sphereMap.dataMap.begin(); it!=ic.sphereMap.dataMap.end(); it++) {

    print("- Applying initial condition within a sphere (material id: %d).\n",
          it->second->initialConditions.materialid);
    Vec3D x0(it->second->cen_x, it->second->cen_y, it->second->cen_z);
    double dist;
    for(int k=k0; k<kmax; k++)
      for(int j=j0; j<jmax; j++)
        for(int i=i0; i<imax; i++) {
          dist = (coords[k][j][i]-x0).norm() - it->second->radius;
          if (dist<0) {
            v[k][j][i][0] = it->second->initialConditions.density;
            v[k][j][i][1] = it->second->initialConditions.velocity_x;
            v[k][j][i][2] = it->second->initialConditions.velocity_y;
            v[k][j][i][3] = it->second->initialConditions.velocity_z;
            v[k][j][i][4] = it->second->initialConditions.pressure;
            id[k][j][i]   = it->second->initialConditions.materialid;
          }
        }
  }



  V.RestoreDataPointerAndInsert();
  ID.RestoreDataPointerAndInsert();
  coordinates.RestoreDataPointerToLocalVector(); //!< data was not changed.

  //! Apply boundary condition to populate ghost nodes (no need to do this for ID)
  ApplyBoundaryConditions(V);   

}

//-----------------------------------------------------
//! Apply boundary conditions by populating the ghost cells.
void SpaceOperator::ApplyBoundaryConditions(SpaceVariable3D &V)
{
  Vec5D*** v = (Vec5D***) V.GetDataPointer();

  int NX, NY, NZ;
  V.GetGlobalSize(&NX, &NY, &NZ);
//  cout << "NX = " << NX << ", NY = " << NY << endl;

  //! Left boundary
  if(ii0==-1) { 
    switch (iod.mesh.bc_x0) {
      case MeshData::INLET :
        for(int k=k0; k<kmax; k++)
          for(int j=j0; j<jmax; j++) {
            v[k][j][ii0][0] = iod.bc.inlet.density;
            v[k][j][ii0][1] = iod.bc.inlet.velocity_x;
            v[k][j][ii0][2] = iod.bc.inlet.velocity_y;
            v[k][j][ii0][3] = iod.bc.inlet.velocity_z;
            v[k][j][ii0][4] = iod.bc.inlet.pressure;
          }
        break;
      case MeshData::OUTLET :
        for(int k=k0; k<kmax; k++)
          for(int j=j0; j<jmax; j++) {
            v[k][j][ii0][0] = iod.bc.outlet.density;
            v[k][j][ii0][1] = iod.bc.outlet.velocity_x;
            v[k][j][ii0][2] = iod.bc.outlet.velocity_y;
            v[k][j][ii0][3] = iod.bc.outlet.velocity_z;
            v[k][j][ii0][4] = iod.bc.outlet.pressure;
          }
        break; 
      case MeshData::WALL :
      case MeshData::SYMMETRY :
        for(int k=k0; k<kmax; k++)
          for(int j=j0; j<jmax; j++) {
            v[k][j][ii0][0] =      v[k][j][ii0+1][0];
            v[k][j][ii0][1] = -1.0*v[k][j][ii0+1][1];
            v[k][j][ii0][2] =      v[k][j][ii0+1][2];
            v[k][j][ii0][3] =      v[k][j][ii0+1][3]; 
            v[k][j][ii0][4] =      v[k][j][ii0+1][4];
          }
        break;
      default :
        print_error("Error: Boundary condition at x=x0 cannot be specified!\n");
        exit_mpi();
    }
  }

  //! Right boundary
  if(iimax==NX+1) { 
    switch (iod.mesh.bc_xmax) {
      case MeshData::INLET :
        for(int k=k0; k<kmax; k++)
          for(int j=j0; j<jmax; j++) {
            v[k][j][iimax-1][0] = iod.bc.inlet.density;
            v[k][j][iimax-1][1] = iod.bc.inlet.velocity_x;
            v[k][j][iimax-1][2] = iod.bc.inlet.velocity_y;
            v[k][j][iimax-1][3] = iod.bc.inlet.velocity_z;
            v[k][j][iimax-1][4] = iod.bc.inlet.pressure;
          }
        break;
      case MeshData::OUTLET :
        for(int k=k0; k<kmax; k++)
          for(int j=j0; j<jmax; j++) {
            v[k][j][iimax-1][0] = iod.bc.outlet.density;
            v[k][j][iimax-1][1] = iod.bc.outlet.velocity_x;
            v[k][j][iimax-1][2] = iod.bc.outlet.velocity_y;
            v[k][j][iimax-1][3] = iod.bc.outlet.velocity_z;
            v[k][j][iimax-1][4] = iod.bc.outlet.pressure;
          }
        break; 
      case MeshData::WALL :
      case MeshData::SYMMETRY :
        for(int k=k0; k<kmax; k++)
          for(int j=j0; j<jmax; j++) {
            v[k][j][iimax-1][0] =      v[k][j][iimax-2][0];
            v[k][j][iimax-1][1] = -1.0*v[k][j][iimax-2][1];
            v[k][j][iimax-1][2] =      v[k][j][iimax-2][2];
            v[k][j][iimax-1][3] =      v[k][j][iimax-2][3]; 
            v[k][j][iimax-1][4] =      v[k][j][iimax-2][4];
          }
        break;
      default :
        print_error("Error: Boundary condition at x=xmax cannot be specified!\n");
        exit_mpi();
    }
  }

  //! Bottom boundary
  if(jj0==-1) { 
    switch (iod.mesh.bc_y0) {
      case MeshData::INLET :
        for(int k=k0; k<kmax; k++)
          for(int i=i0; i<imax; i++) {
            v[k][jj0][i][0] = iod.bc.inlet.density;
            v[k][jj0][i][1] = iod.bc.inlet.velocity_x;
            v[k][jj0][i][2] = iod.bc.inlet.velocity_y;
            v[k][jj0][i][3] = iod.bc.inlet.velocity_z;
            v[k][jj0][i][4] = iod.bc.inlet.pressure;
          }
        break;
      case MeshData::OUTLET :
        for(int k=k0; k<kmax; k++)
          for(int i=i0; i<imax; i++) {
            v[k][jj0][i][0] = iod.bc.outlet.density;
            v[k][jj0][i][1] = iod.bc.outlet.velocity_x;
            v[k][jj0][i][2] = iod.bc.outlet.velocity_y;
            v[k][jj0][i][3] = iod.bc.outlet.velocity_z;
            v[k][jj0][i][4] = iod.bc.outlet.pressure;
          }
        break; 
      case MeshData::WALL :
      case MeshData::SYMMETRY :
        for(int k=k0; k<kmax; k++)
          for(int i=i0; i<imax; i++) {
            v[k][jj0][i][0] =      v[k][jj0+1][i][0];
            v[k][jj0][i][1] =      v[k][jj0+1][i][1];
            v[k][jj0][i][2] = -1.0*v[k][jj0+1][i][2];
            v[k][jj0][i][3] =      v[k][jj0+1][i][3]; 
            v[k][jj0][i][4] =      v[k][jj0+1][i][4];
          }
        break;
      default :
        print_error("Error: Boundary condition at y=y0 cannot be specified!\n");
        exit_mpi();
    }
  }

  //! Top boundary
  if(jjmax==NY+1) { 
    switch (iod.mesh.bc_ymax) {
      case MeshData::INLET :
        for(int k=k0; k<kmax; k++)
          for(int i=i0; i<imax; i++) {
            v[k][jjmax-1][i][0] = iod.bc.inlet.density;
            v[k][jjmax-1][i][1] = iod.bc.inlet.velocity_x;
            v[k][jjmax-1][i][2] = iod.bc.inlet.velocity_y;
            v[k][jjmax-1][i][3] = iod.bc.inlet.velocity_z;
            v[k][jjmax-1][i][4] = iod.bc.inlet.pressure;
          }
        break;
      case MeshData::OUTLET :
        for(int k=k0; k<kmax; k++)
          for(int i=i0; i<imax; i++) {
            v[k][jjmax-1][i][0] = iod.bc.outlet.density;
            v[k][jjmax-1][i][1] = iod.bc.outlet.velocity_x;
            v[k][jjmax-1][i][2] = iod.bc.outlet.velocity_y;
            v[k][jjmax-1][i][3] = iod.bc.outlet.velocity_z;
            v[k][jjmax-1][i][4] = iod.bc.outlet.pressure;
          }
        break; 
      case MeshData::WALL :
      case MeshData::SYMMETRY :
        for(int k=k0; k<kmax; k++)
          for(int i=i0; i<imax; i++) {
            v[k][jjmax-1][i][0] =      v[k][jjmax-2][i][0];
            v[k][jjmax-1][i][1] =      v[k][jjmax-2][i][1];
            v[k][jjmax-1][i][2] = -1.0*v[k][jjmax-2][i][2];
            v[k][jjmax-1][i][3] =      v[k][jjmax-2][i][3]; 
            v[k][jjmax-1][i][4] =      v[k][jjmax-2][i][4];
          }
        break;
      default :
        print_error("Error: Boundary condition at y=ymax cannot be specified!\n");
        exit_mpi();
    }
  }

  //! Back boundary (z min)
  if(kk0==-1) { 
    switch (iod.mesh.bc_z0) {
      case MeshData::INLET :
        for(int j=j0; j<jmax; j++)
          for(int i=i0; i<imax; i++) {
            v[kk0][j][i][0] = iod.bc.inlet.density;
            v[kk0][j][i][1] = iod.bc.inlet.velocity_x;
            v[kk0][j][i][2] = iod.bc.inlet.velocity_y;
            v[kk0][j][i][3] = iod.bc.inlet.velocity_z;
            v[kk0][j][i][4] = iod.bc.inlet.pressure;
          }
        break;
      case MeshData::OUTLET :
        for(int j=j0; j<jmax; j++)
          for(int i=i0; i<imax; i++) {
            v[kk0][j][i][0] = iod.bc.outlet.density;
            v[kk0][j][i][1] = iod.bc.outlet.velocity_x;
            v[kk0][j][i][2] = iod.bc.outlet.velocity_y;
            v[kk0][j][i][3] = iod.bc.outlet.velocity_z;
            v[kk0][j][i][4] = iod.bc.outlet.pressure;
          }
        break; 
      case MeshData::WALL :
      case MeshData::SYMMETRY :
        for(int j=j0; j<jmax; j++)
          for(int i=i0; i<imax; i++) {
            v[kk0][j][i][0] =      v[kk0+1][j][i][0];
            v[kk0][j][i][1] =      v[kk0+1][j][i][1];
            v[kk0][j][i][2] =      v[kk0+1][j][i][2];
            v[kk0][j][i][3] = -1.0*v[kk0+1][j][i][3]; 
            v[kk0][j][i][4] =      v[kk0+1][j][i][4];
          }
        break;
      default :
        print_error("Error: Boundary condition at z=z0 cannot be specified!\n");
        exit_mpi();
    }
  }

  //! Front boundary (z max)
  if(kkmax==NZ+1) { 
    switch (iod.mesh.bc_zmax) {
      case MeshData::INLET :
        for(int j=j0; j<jmax; j++)
          for(int i=i0; i<imax; i++) {
            v[kkmax-1][j][i][0] = iod.bc.inlet.density;
            v[kkmax-1][j][i][1] = iod.bc.inlet.velocity_x;
            v[kkmax-1][j][i][2] = iod.bc.inlet.velocity_y;
            v[kkmax-1][j][i][3] = iod.bc.inlet.velocity_z;
            v[kkmax-1][j][i][4] = iod.bc.inlet.pressure;
          }
        break;
      case MeshData::OUTLET :
        for(int j=j0; j<jmax; j++)
          for(int i=i0; i<imax; i++) {
            v[kkmax-1][j][i][0] = iod.bc.outlet.density;
            v[kkmax-1][j][i][1] = iod.bc.outlet.velocity_x;
            v[kkmax-1][j][i][2] = iod.bc.outlet.velocity_y;
            v[kkmax-1][j][i][3] = iod.bc.outlet.velocity_z;
            v[kkmax-1][j][i][4] = iod.bc.outlet.pressure;
          }
        break; 
      case MeshData::WALL :
      case MeshData::SYMMETRY :
        for(int j=j0; j<jmax; j++)
          for(int i=i0; i<imax; i++) {
            v[kkmax-1][j][i][0] =      v[kkmax-2][j][i][0];
            v[kkmax-1][j][i][1] =      v[kkmax-2][j][i][1];
            v[kkmax-1][j][i][2] =      v[kkmax-2][j][i][2];
            v[kkmax-1][j][i][3] = -1.0*v[kkmax-2][j][i][3]; 
            v[kkmax-1][j][i][4] =      v[kkmax-2][j][i][4];
          }
        break;
      default :
        print_error("Error: Boundary condition at z=zmax cannot be specified!\n");
        exit_mpi();
    }
  }

  V.RestoreDataPointerAndInsert();
}

//-----------------------------------------------------

void SpaceOperator::FindExtremeValuesOfFlowVariables(SpaceVariable3D &V, SpaceVariable3D &ID,
                        double *Vmin, double *Vmax, double &cmin, double &cmax,
                        double &Machmax, double &char_speed_max,
                        double &dx_over_char_speed_min)
{
  Vec5D*** v    = (Vec5D***)V.GetDataPointer();
  Vec3D*** dxyz = (Vec3D***)delta_xyz.GetDataPointer();
  double*** id = (double***) ID.GetDataPointer();

  for(int i=0; i<5; i++) {
    Vmin[i] = DBL_MAX; //max. double precision number
    Vmax[i] = -DBL_MAX;
  }
  cmin = DBL_MAX;
  cmax = Machmax = char_speed_max = -DBL_MAX;
  dx_over_char_speed_min = DBL_MAX;

  // Loop through the real domain (excluding the ghost layer)
  double c, mach, lam_f, lam_g, lam_h;
  int myid;
  for(int k=k0; k<kmax; k++) {
    for(int j=j0; j<jmax; j++) {
      for(int i=i0; i<imax; i++) {

        for(int p=0; p<5; p++) {
          Vmin[p] = min(Vmin[p], v[k][j][i][p]);
          Vmax[p] = max(Vmax[p], v[k][j][i][p]);
        } 

        myid = id[k][j][i];

        c = varFcn[myid]->ComputeSoundSpeed(v[k][j][i][0]/*rho*/, 
                            varFcn[myid]->GetInternalEnergyPerUnitMass(v[k][j][i][0],v[k][j][i][4])/*e*/);

        cmin = min(cmin, c);
        cmax = max(cmax, c);
        mach = varFcn[myid]->ComputeMachNumber(v[k][j][i]); 
        Machmax = max(Machmax, mach); 

        fluxFcn.EvaluateMaxEigenvalues(v[k][j][i], myid, lam_f, lam_g, lam_h);
        char_speed_max = max(max(max(char_speed_max, lam_f), lam_g), lam_h);

        dx_over_char_speed_min = min(dx_over_char_speed_min, 
                                     min(dxyz[k][j][i][0]/lam_f, 
                                         min(dxyz[k][j][i][1]/lam_g, dxyz[k][j][i][2]/lam_h) ) );
      }
    }
  }

  MPI_Allreduce(MPI_IN_PLACE, Vmin, 5, MPI_DOUBLE, MPI_MIN, comm);
  MPI_Allreduce(MPI_IN_PLACE, Vmax, 5, MPI_DOUBLE, MPI_MAX, comm);
  MPI_Allreduce(MPI_IN_PLACE, &cmin, 1, MPI_DOUBLE, MPI_MIN, comm);
  MPI_Allreduce(MPI_IN_PLACE, &cmax, 1, MPI_DOUBLE, MPI_MAX, comm);
  MPI_Allreduce(MPI_IN_PLACE, &Machmax, 1, MPI_DOUBLE, MPI_MAX, comm);
  MPI_Allreduce(MPI_IN_PLACE, &char_speed_max, 1, MPI_DOUBLE, MPI_MAX, comm);
  MPI_Allreduce(MPI_IN_PLACE, &dx_over_char_speed_min, 1, MPI_DOUBLE, MPI_MIN, comm);

  V.RestoreDataPointerToLocalVector(); 
  delta_xyz.RestoreDataPointerToLocalVector(); 
  ID.RestoreDataPointerToLocalVector();
}

//-----------------------------------------------------

void SpaceOperator::ComputeTimeStepSize(SpaceVariable3D &V, SpaceVariable3D &ID, double &dt, double &cfl)
{
  double Vmin[5], Vmax[5], cmin, cmax, Machmax, char_speed_max, dx_over_char_speed_min; 
  FindExtremeValuesOfFlowVariables(V, ID, Vmin, Vmax, cmin, cmax, Machmax, char_speed_max, dx_over_char_speed_min);

  if(iod.output.verbose == OutputData::ON)
    print("  - Maximum values: rho = %e, p = %e, c = %e, Mach = %e, char. speed = %e.\n", 
          Vmax[0], Vmax[4], cmax, Machmax, char_speed_max);

  if(iod.ts.timestep > 0) {
    dt = iod.ts.timestep;
    cfl = dt/dx_over_char_speed_min;
  } else {//apply the CFL number
    cfl = iod.ts.cfl;      
    dt = cfl*dx_over_char_speed_min;
  }

}

//-----------------------------------------------------

void SpaceOperator::ComputeAdvectionFluxes(SpaceVariable3D &V, SpaceVariable3D &ID, SpaceVariable3D &F)
{
  //------------------------------------
  // Reconstruction w/ slope limiters.
  //------------------------------------
  rec.Reconstruct(V, Vl, Vr, Vb, Vt, Vk, Vf);

  Vec5D*** v  = (Vec5D***) V.GetDataPointer();
  Vec5D*** vl = (Vec5D***) Vl.GetDataPointer();
  Vec5D*** vr = (Vec5D***) Vr.GetDataPointer();
  Vec5D*** vb = (Vec5D***) Vb.GetDataPointer();
  Vec5D*** vt = (Vec5D***) Vt.GetDataPointer();
  Vec5D*** vk = (Vec5D***) Vk.GetDataPointer();
  Vec5D*** vf = (Vec5D***) Vf.GetDataPointer();
  Vec5D*** f  = (Vec5D***) F.GetDataPointer();

  double*** id = (double***) ID.GetDataPointer();

  Vec3D*** coords = (Vec3D***)coordinates.GetDataPointer(); //for debugging

  //------------------------------------
  // Clip pressure and density for the reconstructed state
  // Verify hyperbolicity (i.e. c^2 > 0).
  //------------------------------------
  int nClipped = 0;
  bool error = false;
  int corner;
  int myid;
  for(int k=kk0; k<kkmax; k++) {
    for(int j=jj0; j<jjmax; j++) {
      for(int i=ii0; i<iimax; i++) {

        corner = 0;
        if(k==kk0 || k==kkmax-1) corner++;
        if(j==jj0 || j==jjmax-1) corner++;
        if(i==ii0 || i==iimax-1) corner++;
        if(corner>=2) //not needed
          continue;

        myid = id[k][j][i];

        nClipped += (int)varFcn[myid]->ClipDensityAndPressure(vl[k][j][i]);
        nClipped += (int)varFcn[myid]->ClipDensityAndPressure(vr[k][j][i]);
        nClipped += (int)varFcn[myid]->ClipDensityAndPressure(vb[k][j][i]);
        nClipped += (int)varFcn[myid]->ClipDensityAndPressure(vt[k][j][i]);
        nClipped += (int)varFcn[myid]->ClipDensityAndPressure(vk[k][j][i]);
        nClipped += (int)varFcn[myid]->ClipDensityAndPressure(vf[k][j][i]);
         
        error = varFcn[myid]->CheckState(vl[k][j][i]) || varFcn[myid]->CheckState(vr[k][j][i]) || 
                varFcn[myid]->CheckState(vb[k][j][i]) || varFcn[myid]->CheckState(vt[k][j][i]) ||
                varFcn[myid]->CheckState(vk[k][j][i]) || varFcn[myid]->CheckState(vf[k][j][i]);

        if(error) {
          print_error("Error: Reconstructed state at (%d,%d,%d) violates hyperbolicity. matid = %d.\n", i,j,k, myid);
          fprintf(stderr, "v[%d,%d,%d]  = [%e, %e, %e, %e, %e]\n", i,j,k, v[k][j][i][0], v[k][j][i][1], v[k][j][i][2], v[k][j][i][3], v[k][j][i][4]);
          fprintf(stderr, "vl[%d,%d,%d] = [%e, %e, %e, %e, %e]\n", i,j,k, vl[k][j][i][0], vl[k][j][i][1], vl[k][j][i][2], vl[k][j][i][3], vl[k][j][i][4]);
          fprintf(stderr, "vr[%d,%d,%d] = [%e, %e, %e, %e, %e]\n", i,j,k, vr[k][j][i][0], vr[k][j][i][1], vr[k][j][i][2], vr[k][j][i][3], vr[k][j][i][4]);
          fprintf(stderr, "vb[%d,%d,%d] = [%e, %e, %e, %e, %e]\n", i,j,k, vb[k][j][i][0], vb[k][j][i][1], vb[k][j][i][2], vb[k][j][i][3], vb[k][j][i][4]);
          fprintf(stderr, "vt[%d,%d,%d] = [%e, %e, %e, %e, %e]\n", i,j,k, vt[k][j][i][0], vt[k][j][i][1], vt[k][j][i][2], vt[k][j][i][3], vt[k][j][i][4]);
          fprintf(stderr, "vk[%d,%d,%d] = [%e, %e, %e, %e, %e]\n", i,j,k, vk[k][j][i][0], vk[k][j][i][1], vk[k][j][i][2], vk[k][j][i][3], vk[k][j][i][4]);
          fprintf(stderr, "vf[%d,%d,%d] = [%e, %e, %e, %e, %e]\n", i,j,k, vf[k][j][i][0], vf[k][j][i][1], vf[k][j][i][2], vf[k][j][i][3], vf[k][j][i][4]);
          exit_mpi();
        } 
      }
    }
  }
  MPI_Allreduce(MPI_IN_PLACE, &nClipped, 1, MPI_INT, MPI_SUM, comm);
  if(nClipped)
    print("Warning: Clipped pressure and/or density in %d reconstructed states.\n", nClipped);
  
  //------------------------------------
  // Compute fluxes
  //------------------------------------
  Vec5D localflux;
  Vec3D*** dxyz = (Vec3D***)delta_xyz.GetDataPointer();

  // Initialize F to 0
  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++) 
      for(int i=ii0; i<iimax; i++)
          f[k][j][i] = 0.0; //setting f[k][j][i][0] = ... = f[k][j][i][4] = 0.0;


  int neighborid = -1;
  int midid = -1;
  double Vmid[5];
  // Loop through the domain interior, and the right, top, and front ghost layers. For each cell, calculate the
  // numerical flux across the left, lower, and back cell boundaries/interfaces
  for(int k=k0; k<kkmax; k++) {
    for(int j=j0; j<jjmax; j++) {
      for(int i=i0; i<iimax; i++) {

        myid = id[k][j][i];

        //calculate flux function F_{i-1/2,j,k}
        if(k!=kkmax-1 && j!=jjmax-1) {
 
          neighborid = id[k][j][i-1];
          if(neighborid==myid) {
            fluxFcn.ComputeNumericalFluxAtCellInterface(0/*F*/, vr[k][j][i-1]/*Vm*/, vl[k][j][i]/*Vp*/, myid, localflux);
          } else {//material interface
            riemann.ComputeRiemannSolution(0/*F*/, vr[k][j][i-1], neighborid, vl[k][j][i], myid, Vmid, midid);
            //Godunov flux
            fluxFcn.EvaluateFluxFunction_F(Vmid, midid, localflux);
          }

          localflux *= dxyz[k][j][i][1]*dxyz[k][j][i][2];
          f[k][j][i-1] += localflux;
          f[k][j][i]   -= localflux;  // the scheme is conservative 

        }

        //calculate flux function G_{i,j-1/2,k}
        if(k!=kkmax-1 && i!=iimax-1) {

          neighborid = id[k][j-1][i];
          if(neighborid==myid) {
            fluxFcn.ComputeNumericalFluxAtCellInterface(1/*G*/, vt[k][j-1][i]/*Vm*/, vb[k][j][i]/*Vp*/, myid, localflux);
          } else {//material interface
//            fprintf(stderr,"coords[%d,%d,%d] = %e %e %e, id = %d.\n", k, j-1, i, coords[k][j-1][i][0], coords[k][j-1][i][1], coords[k][j-1][i][2], (int)id[k][j-1][i]);
//            fprintf(stderr,"coords[%d,%d,%d] = %e %e %e, id = %d.\n", k, j, i, coords[k][j][i][0], coords[k][j][i][1], coords[k][j][i][2], (int)id[k][j][i]);
            riemann.ComputeRiemannSolution(1/*G*/, vt[k][j-1][i], neighborid, vb[k][j][i], myid, Vmid, midid);
            //Godunov flux
            fluxFcn.EvaluateFluxFunction_G(Vmid, midid, localflux);
          }

          localflux *= dxyz[k][j][i][0]*dxyz[k][j][i][2];
          f[k][j-1][i] += localflux;
          f[k][j][i]   -= localflux;  // the scheme is conservative 
        }

        //calculate flux function H_{i,j,k-1/2}
        if(j!=jjmax-1 && i!=iimax-1) {

          neighborid = id[k-1][j][i];
          if(neighborid==myid) {
            fluxFcn.ComputeNumericalFluxAtCellInterface(2/*H*/, vf[k-1][j][i]/*Vm*/, vk[k][j][i]/*Vp*/, myid, localflux);
          } else {//material interface
            riemann.ComputeRiemannSolution(2/*H*/, vf[k-1][j][i], neighborid, vk[k][j][i], myid, Vmid, midid);
            //Godunov flux
            fluxFcn.EvaluateFluxFunction_H(Vmid, midid, localflux);
          }

          localflux *= dxyz[k][j][i][0]*dxyz[k][j][i][1];
          f[k-1][j][i] += localflux;
          f[k][j][i]   -= localflux;  // the scheme is conservative 
        }

      }
    }
  }
        
  //------------------------------------
  // Restore Spatial Variables
  //------------------------------------
  delta_xyz.RestoreDataPointerToLocalVector(); //no changes
  ID.RestoreDataPointerToLocalVector(); //no changes
  coordinates.RestoreDataPointerToLocalVector(); //no changes

  V.RestoreDataPointerToLocalVector(); 
  Vl.RestoreDataPointerToLocalVector(); 
  Vr.RestoreDataPointerToLocalVector(); 
  Vb.RestoreDataPointerToLocalVector(); 
  Vt.RestoreDataPointerToLocalVector(); 
  Vk.RestoreDataPointerToLocalVector(); 
  Vf.RestoreDataPointerToLocalVector(); 

  F.RestoreDataPointerToLocalVector(); //NOTE: although F has been updated, there is no need of 
                                       //      cross-subdomain communications. So, no need to 
                                       //      update the global vec.
}

//-----------------------------------------------------

void SpaceOperator::ComputeResidual(SpaceVariable3D &V, SpaceVariable3D &ID, SpaceVariable3D &R)
{
  ComputeAdvectionFluxes(V, ID, R);

  // -------------------------------------------------
  // multiply flux by -1, and divide by cell volume (for cells within the actual domain)
  // -------------------------------------------------
  Vec5D***    r = (Vec5D***) R.GetDataPointer();
  double*** vol = (double***)volume.GetDataPointer();

  for(int k=k0; k<kmax; k++)
    for(int j=j0; j<jmax; j++) 
      for(int i=i0; i<imax; i++)
        r[k][j][i] /= -vol[k][j][i];

  // restore spatial variables
  R.RestoreDataPointerToLocalVector(); //NOTE: although R has been updated, there is no need of 
                                       //      cross-subdomain communications. So, no need to 
                                       //      update the global vec.
  volume.RestoreDataPointerToLocalVector();
}


//-----------------------------------------------------









