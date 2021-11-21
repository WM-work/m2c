#include<MultiPhaseOperator.h>
#include<SpaceOperator.h>
#include<LevelSetOperator.h>
#include<Vector5D.h>
#include<RiemannSolutions.h>
#include<algorithm>//find
#include<set>
using std::cout;
using std::endl;
using std::set;
using std::tuple;
using std::get;
extern int verbose;
//-----------------------------------------------------

MultiPhaseOperator::MultiPhaseOperator(MPI_Comm &comm_, DataManagers3D &dm_all_, IoData &iod_,
                                       vector<VarFcnBase*> &varFcn_, SpaceOperator &spo, vector<LevelSetOperator*> &lso)
                  : comm(comm_), iod(iod_), varFcn(varFcn_),
                    coordinates(spo.GetMeshCoordinates()),
                    delta_xyz(spo.GetMeshDeltaXYZ()),
                    Tag(comm_, &(dm_all_.ghosted1_1dof)),
                    Lambda(comm_, &(dm_all_.ghosted1_1dof))
{

  coordinates.GetCornerIndices(&i0, &j0, &k0, &imax, &jmax, &kmax);
  coordinates.GetGhostedCornerIndices(&ii0, &jj0, &kk0, &iimax, &jjmax, &kkmax);

  for(int i=0; i<lso.size(); i++)
    ls2matid[i] = lso[i]->GetMaterialID();

  // Initialize phase/material transition functions (if specified by user)
  if(iod.eqs.transitions.dataMap.size()) {

    // start with creating an empty vector for each material ID
    int numMaterials = varFcn.size();
    trans.resize(numMaterials);

    // fill in the user-specified transitions
    for(auto it = iod.eqs.transitions.dataMap.begin(); it != iod.eqs.transitions.dataMap.end(); it++) {
      int i = it->second->from_id;
      int j = it->second->to_id;
      if(i<0 || i>=varFcn.size() || j<0 || j>=varFcn.size() || i==j) {
        print_error("*** Error: Detected input error in Material/Phase Transition [%d] (%d -> %d).\n", it->first,
                    i, j); 
        exit_mpi();
      }
      if(true) //no other choices at the moment
        trans[i].push_back(new PhaseTransitionBase(*it->second, *varFcn[i], *varFcn[j]));
      else {
        print_error("*** Error: Unknown phase transition type (%d).\n");
        exit_mpi();
      }
    }

    // make sure the needed level set functions are available
    for(auto it = iod.eqs.transitions.dataMap.begin(); it != iod.eqs.transitions.dataMap.end(); it++) {
      int i = it->second->from_id;
      int j = it->second->to_id;
      if(i!=0) {
        bool found = false;
        for(int ls = 0; ls < lso.size(); ls++) {
          if(ls2matid[ls] == i) {
            found = true;
            break;
          }
        }
        if(!found) {
          print_error("*** Error: Phase transitions involve material ID %d, but a level set solver is not specified.\n",
                      i);
          exit_mpi();
        }
      }
      if(j!=0) {
        bool found = false;
        for(int ls = 0; ls < lso.size(); ls++) {
          if(ls2matid[ls] == j) {
            found = true;
            break;
          }
        }
        if(!found) {
          print_error("*** Error: Phase transitions involve material ID %d, but a level set solver is not specified.\n",
                      j);
          exit_mpi();
        }
      }
    }

  } else
    trans.resize(0);

}

//-----------------------------------------------------

MultiPhaseOperator::~MultiPhaseOperator()
{ }

//-----------------------------------------------------

void
MultiPhaseOperator::Destroy()
{
  Tag.Destroy();

  Lambda.Destroy();

  for(int i=0; i<trans.size(); i++)
    for(auto it = trans[i].begin(); it != trans[i].end(); it++)
      delete *it;
}

//-----------------------------------------------------

void 
MultiPhaseOperator::UpdateMaterialID(vector<SpaceVariable3D*> &Phi, SpaceVariable3D &ID)
{

#ifdef LEVELSET_TEST
  return; //testing the level set solver w/o solving the N-S / Euler equations
#endif

  // reset tag to 0
  Tag.SetConstantValue(0, true/*workOnGhost*/);
  ID.SetConstantValue(0, true/*workOnGhost*/);
  int overlap = 0;

  double*** tag = (double***)Tag.GetDataPointer();
  double*** id  = (double***)ID.GetDataPointer();

  int ls_size = Phi.size();
  vector<double***> phi(ls_size, NULL);
  for(int ls=0; ls<ls_size; ls++) 
    phi[ls] = Phi[ls]->GetDataPointer();
  

  for(int ls = 0; ls<ls_size; ls++) {//loop through all the level set functions
  
    int matid = ls2matid[ls];

    for(int k=kk0; k<kkmax; k++)
      for(int j=jj0; j<jjmax; j++)
        for(int i=ii0; i<iimax; i++) {

          if(phi[ls][k][j][i]<0) {
            if(id[k][j][i] != 0) {
              overlap++;
              tag[k][j][i] = 1;
            } 
            id[k][j][i] = matid; 
          }
          //check if this node (i,j,k) is EXACTLY at the interface of two subdomains. If yes, give it the ID
          //of one of the two materials that has a smaller ID. (Without this check, it would get ID = 0.)
          else if(ls_size>1 && phi[ls][k][j][i]==0) {
            for(int other_ls=ls+1; other_ls<ls_size; other_ls++) {
              if(phi[other_ls][k][j][i]==0) {
                id[k][j][i] = matid;
                break;
              }
            }
          }

        }
  }


  for(int ls=0; ls<ls_size; ls++) 
    Phi[ls]->RestoreDataPointerToLocalVector(); //no changes made

  MPI_Allreduce(MPI_IN_PLACE, &overlap, 1, MPI_INT, MPI_SUM, comm);


  if(overlap) {
    print_error("*** Error: Found overlapping material interfaces. Number of overlapped cells: %d.\n", overlap);
    exit_mpi();
  } 


  if(overlap) 
    Tag.RestoreDataPointerAndInsert();
  else
    Tag.RestoreDataPointerToLocalVector();

  ID.RestoreDataPointerAndInsert();

}

//-----------------------------------------------------
// Section 4.2.4 of Arthur Rallu's thesis
void
MultiPhaseOperator::UpdateStateVariablesAfterInterfaceMotion(SpaceVariable3D &IDn, 
                        SpaceVariable3D &ID, SpaceVariable3D &V, RiemannSolutions &riemann_solutions)
{
  switch (iod.multiphase.phasechange_type) {

    case MultiPhaseData::RIEMANN_SOLUTION :
      UpdateStateVariablesByRiemannSolutions(IDn, ID, V, riemann_solutions);
      break;

    case MultiPhaseData::EXTRAPOLATION :
      UpdateStateVariablesByExtrapolation(IDn, ID, V);
      break;

    default :
      print_error("*** Error: Specified method for phase-change update (%d) has not been implemented.\n", 
                  (int)iod.multiphase.phasechange_type);
  }


  if(trans.size()!=0 && iod.multiphase.latent_heat_transfer==MultiPhaseData::On) 
    AddLambdaToEnthalpyAfterInterfaceMotion(IDn, ID, V); // Add stored latent_heat (Lambda) to cells that 
                                                         // changed phase due to interface motion.
}

//-----------------------------------------------------

void
MultiPhaseOperator::UpdateStateVariablesByRiemannSolutions(SpaceVariable3D &IDn, 
                        SpaceVariable3D &ID, SpaceVariable3D &V, RiemannSolutions &riemann_solutions)
{
  // extract info
  double*** idn = (double***)IDn.GetDataPointer();
  double*** id  = (double***)ID.GetDataPointer();
  Vec5D***  v   = (Vec5D***) V.GetDataPointer();

  // create a vector that temporarily stores unresolved nodes (which will be resolved separately)
  vector<Int3> unresolved;

  // work inside the real domain
  int counter = 0;
  for(int k=k0; k<kmax; k++)
    for(int j=j0; j<jmax; j++)
      for(int i=i0; i<imax; i++) {

        if(id[k][j][i] == idn[k][j][i]) //id remains the same. Skip
          continue;

        counter = LocalUpdateByRiemannSolutions(i, j, k, id[k][j][i], v[k][j][i-1], v[k][j][i+1], 
                      v[k][j-1][i], v[k][j+1][i], v[k-1][j][i], v[k+1][j][i], riemann_solutions,
                      v[k][j][i], true);
        if(counter==0)
          counter = LocalUpdateByRiemannSolutions(i, j, k, id[k][j][i], v[k][j][i-1], v[k][j][i+1], 
              v[k][j-1][i], v[k][j+1][i], v[k-1][j][i], v[k+1][j][i], riemann_solutions,
              v[k][j][i], false);

        if(counter==0) //add it to unresolved nodes...
          unresolved.push_back(Int3(k,j,i)); //note the order: k,j,i

      }  


  V.RestoreDataPointerAndInsert(); //insert data & communicate with neighbor subd's
  ID.RestoreDataPointerToLocalVector();
  IDn.RestoreDataPointerToLocalVector();

  // Fix the unresolved nodes (if any)
  int nUnresolved = unresolved.size();
  MPI_Allreduce(MPI_IN_PLACE, &nUnresolved, 1, MPI_INT, MPI_SUM, comm);
  if(nUnresolved) //some of the subdomains have unresolved nodes
    FixUnresolvedNodes(unresolved, IDn, ID, V); 

} 

//-----------------------------------------------------

int
MultiPhaseOperator::LocalUpdateByRiemannSolutions(int i, int j, int k, int id, Vec5D &vl, Vec5D &vr, 
                        Vec5D &vb, Vec5D &vt, Vec5D &vk, Vec5D &vf, RiemannSolutions &riemann_solutions, 
                        Vec5D &v, bool upwind)
{
  int counter = 0;
  double weight = 0, sum_weight = 0;
  Int3 ind(k,j,i);

  // left
  auto it = riemann_solutions.left.find(ind);
  if(it != riemann_solutions.left.end()) {
    if(it->second.second/*ID*/ == id && (!upwind || vl[1] > 0)) {
      Vec3D v1(vl[1], vl[2], vl[3]);
      weight = upwind ? vl[1]/v1.norm() : 1.0;
      sum_weight += weight;
      if(counter==0) 
        v = weight*it->second.first; /*riemann solution*/
      else 
        v += weight*it->second.first; /*riemann solution*/
      counter++;
    }
  }

  // right
  it = riemann_solutions.right.find(ind);
  if(it != riemann_solutions.right.end()) {
    if(it->second.second/*ID*/ == id && (!upwind || vr[1] < 0)) {
      Vec3D v1(vr[1], vr[2], vr[3]);
      weight = upwind ? -vr[1]/v1.norm() : 1.0;
      sum_weight += weight;
      if(counter==0)
        v = weight*it->second.first; /*riemann solution*/
      else
        v += weight*it->second.first; /*riemann solution*/
      counter++;
    }
  }

  // bottom
  it = riemann_solutions.bottom.find(ind);
  if(it != riemann_solutions.bottom.end()) {
    if(it->second.second/*ID*/ == id && (!upwind || vb[2] > 0)) {
      Vec3D v1(vb[1], vb[2], vb[3]);
      weight = upwind ? vb[2]/v1.norm() : 1.0;
      sum_weight += weight;
      if(counter==0)
        v = weight*it->second.first; /*riemann solution*/
      else
        v += weight*it->second.first; /*riemann solution*/
      counter++;
    }
  }

  // top
  it = riemann_solutions.top.find(ind);
  if(it != riemann_solutions.top.end()) {
    if(it->second.second/*ID*/ == id && (!upwind || vt[2] < 0)) {
      Vec3D v1(vt[1], vt[2], vt[3]);
      weight = upwind ? -vt[2]/v1.norm() : 1.0;
      sum_weight += weight;
      if(counter==0)
        v = weight*it->second.first; /*riemann solution*/
      else
        v += weight*it->second.first; /*riemann solution*/
      counter++;
    }
  }

  // back
  it = riemann_solutions.back.find(ind);
  if(it != riemann_solutions.back.end()) {
    if(it->second.second/*ID*/ == id && (!upwind || vk[3] > 0)) {
      Vec3D v1(vk[1], vk[2], vk[3]);
      weight = upwind ? vk[3]/v1.norm() : 1.0;
      sum_weight += weight;
      if(counter==0)
        v = weight*it->second.first; /*riemann solution*/
      else
        v += weight*it->second.first; /*riemann solution*/
      counter++;
    }
  }

  // front
  it = riemann_solutions.front.find(ind);
  if(it != riemann_solutions.front.end()) {
    if(it->second.second/*ID*/ == id && (!upwind || vf[3] < 0)) {
      Vec3D v1(vf[1], vf[2], vf[3]);
      weight = upwind ? -vf[3]/v1.norm() : 1.0;
      sum_weight += weight;
      if(counter==0)
        v = weight*it->second.first; /*riemann solution*/
      else
        v += weight*it->second.first; /*riemann solution*/
      counter++;
    }
  }

  if(sum_weight > 0.0)
    v /= sum_weight;
  else if(upwind) {
    if(verbose>1) 
      fprintf(stderr,"Warning: Unable to update phase change at (%d,%d,%d) by Riemann solutions w/ upwinding. Retrying.\n", i,j,k);
  } else {
    if(verbose>1) 
      fprintf(stderr,"Warning: Unable to update phase change at (%d,%d,%d) by Riemann solutions. Retrying.\n", i,j,k);
  }

  return counter;
}

//-----------------------------------------------------

void
MultiPhaseOperator::UpdateStateVariablesByExtrapolation(SpaceVariable3D &IDn, 
                        SpaceVariable3D &ID, SpaceVariable3D &V)
{
  // extract info
  double*** idn = (double***)IDn.GetDataPointer();
  double*** id  = (double***)ID.GetDataPointer();
  Vec5D***  v   = (Vec5D***) V.GetDataPointer();

  Vec3D*** coords = (Vec3D***)coordinates.GetDataPointer();

  double weight, sum_weight;
  Vec3D v1, x1x0;
  double v1norm;

  // create a vector that temporarily stores unresolved nodes (which will be resolved separately)
  vector<Int3> unresolved;

  // work inside the real domain
  for(int k=k0; k<kmax; k++)
    for(int j=j0; j<jmax; j++)
      for(int i=i0; i<imax; i++) {

        if(id[k][j][i] == idn[k][j][i]) //id remains the same. Skip
          continue;

        // coordinates of this node
        Vec3D& x0(coords[k][j][i]);

        sum_weight = 0.0;

        bool reset = false; //whether v[k][j][i] has been reset to 0

        //go over the neighboring nodes 
        for(int neighk = k-1; neighk <= k+1; neighk++)         
          for(int neighj = j-1; neighj <= j+1; neighj++)
            for(int neighi = i-1; neighi <= i+1; neighi++) {

              if(id[neighk][neighj][neighi] != id[k][j][i])
                continue; //this neighbor has a different ID. Skip it.

              if(id[neighk][neighj][neighi] != idn[neighk][neighj][neighi])
                continue; //this neighbor also changed ID. Skip it. (Also skipping node [k][j][i])

              if(ID.OutsidePhysicalDomain(neighi, neighj, neighk))
                continue; //this neighbor is outside the physical domain. Skip.

              // coordinates and velocity at the neighbor node
              Vec3D& x1(coords[neighk][neighj][neighi]);
              v1[0] = v[neighk][neighj][neighi][1];
              v1[1] = v[neighk][neighj][neighi][2];
              v1[2] = v[neighk][neighj][neighi][3];

              // compute weight
              v1norm = v1.norm();
              if(v1norm != 0)
                v1 /= v1norm;
              x1x0 = x0 - x1; 
              x1x0 /= x1x0.norm();

              weight = max(0.0, x1x0*v1);

              // add weighted s.v. at neighbor node
              if(weight>0) {
                sum_weight += weight;
                if(reset)
                  v[k][j][i] += weight*v[neighk][neighj][neighi];
                else {
                  v[k][j][i] = weight*v[neighk][neighj][neighi];
                  reset = true;
                }
              }
            }

        if(sum_weight==0) {
          if(verbose>1) 
            fprintf(stderr,"Warning: Unable to update phase change at (%d,%d,%d)(%e,%e,%e) "
                    "by extrapolation w/ upwinding.\n", i,j,k, x0[0],x0[1],x0[2]);
          unresolved.push_back(Int3(k,j,i)); //note the order: k,j,i          
        } else
          v[k][j][i] /= sum_weight; 
      }

  V.RestoreDataPointerAndInsert(); //insert data & communicate with neighbor subd's
  ID.RestoreDataPointerToLocalVector();
  IDn.RestoreDataPointerToLocalVector();
  coordinates.RestoreDataPointerToLocalVector();

  // Fix the unresolved nodes (if any)
  int nUnresolved = unresolved.size();
  MPI_Allreduce(MPI_IN_PLACE, &nUnresolved, 1, MPI_INT, MPI_SUM, comm);
  if(nUnresolved) //some of the subdomains have unresolved nodes
    FixUnresolvedNodes(unresolved, IDn, ID, V); 

}

//-----------------------------------------------------

void MultiPhaseOperator::FixUnresolvedNodes(vector<Int3> &unresolved, SpaceVariable3D &IDn, SpaceVariable3D &ID,
                                            SpaceVariable3D &V)
{
  // Note: all the processor cores will enter this function even if only one or a few have unresolved nodes

  // extract info
  double*** idn = (double***)IDn.GetDataPointer();
  double*** id  = (double***)ID.GetDataPointer();
  Vec5D***  v   = (Vec5D***) V.GetDataPointer();

  Vec3D*** coords = (Vec3D***)coordinates.GetDataPointer();


  // loop through unresolved nodes
  int i,j,k;
  double weight, sum_weight = 0.0; 
  double sum_weight2 = 0.0; //similar, but regardless of "upwinding"
  Vec5D vtmp;
  Vec3D v1, x1x0;
  double v1norm, x1x0norm;

  bool reset = false; //whether v[k][j][i] has been reset

  int failure = 0;

  for(auto it = unresolved.begin(); it != unresolved.end(); it++) { 

    k = (*it)[0];
    j = (*it)[1];
    i = (*it)[2];

    Vec3D& x0(coords[k][j][i]);

    sum_weight = 0.0;
    sum_weight2 = 0.0;
    vtmp = 0.0;

    //go over the neighboring nodes 
    for(int neighk = k-1; neighk <= k+1; neighk++)         
      for(int neighj = j-1; neighj <= j+1; neighj++)
        for(int neighi = i-1; neighi <= i+1; neighi++) {

          if(ID.OutsidePhysicalDomain(neighi, neighj, neighk))
            continue; //this neighbor is outside the physical domain. Skip.

          if(id[neighk][neighj][neighi] != id[k][j][i])
            continue; //this neighbor has a different ID. Skip it.

          if(neighk==k && neighj==j && neighi==i) 
            continue; //the same node. Skip

          bool neighbor_unresolved = false;
          for(auto it2 = unresolved.begin(); it2 != unresolved.end(); it2++) {
            if(neighk==(*it2)[0] && neighj==(*it2)[1] && neighi==(*it2)[2]) {
              neighbor_unresolved = true; 
              break; 
            }
          }
          if(neighbor_unresolved)
            continue; //this neighbor is also unresolved. Skip

          // coordinates and velocity at the neighbor node
          Vec3D& x1(coords[neighk][neighj][neighi]);
          v1[0] = v[neighk][neighj][neighi][1];
          v1[1] = v[neighk][neighj][neighi][2];
          v1[2] = v[neighk][neighj][neighi][3];

          // compute weight
          v1norm = v1.norm();
          if(v1norm != 0)
            v1 /= v1norm;

          x1x0 = x0 - x1; 
          x1x0norm = x1x0.norm();
          x1x0 /= x1x0norm;

          weight = max(0.0, x1x0*v1);

          // add weighted s.v. at neighbor node
          if(weight>0) {
            sum_weight += weight;
            if(reset)
              v[k][j][i] += weight*v[neighk][neighj][neighi];
            else {
              v[k][j][i] = weight*v[neighk][neighj][neighi];
              reset = true;
            }
          }

          // add to sum_weight2, regardless of upwinding
          vtmp += x1x0norm*v[neighk][neighj][neighi];
          sum_weight2 += x1x0norm;

        }


    if(sum_weight>0) {
      v[k][j][i] /= sum_weight; //Done!
      if(verbose>1) fprintf(stderr,"*** (%d,%d,%d): Updated state variables by extrapolation w/ upwinding. (2nd attempt)\n",
                          i,j,k);
      continue;
    }


    // if still unresolved, try to apply an averaging w/o     
    if(sum_weight2>0) {
      v[k][j][i] = vtmp/sum_weight2; //Done!
      if(verbose>1) fprintf(stderr,"*** (%d,%d,%d): Updated state variables by extrapolation w/o enforcing upwinding. (2nd attempt)\n",
                          i,j,k);
      continue;
    }

    // Our last resort: keep the pressure and velocity (both normal & TANGENTIAL) at the current node, 
    // find a valid density nearby. (In this case, the solution may be different for different domain
    // partitions --- but this should rarely happen.)
          
    //go over the neighboring nodes & interpolate velocity and pressure
    int max_layer = 10;
    double density = 0.0;
    for(int layer = 1; layer <= max_layer; layer++) {

      for(int neighk = k-layer; neighk <= k+layer; neighk++)         
        for(int neighj = j-layer; neighj <= j+layer; neighj++)
          for(int neighi = i-layer; neighi <= i+layer; neighi++) {

            if(ID.OutsidePhysicalDomain(neighi, neighj, neighk))
              continue; //this neighbor is outside the physical domain. Skip.
  
            if(!ID.IsHere(neighi,neighj,neighk,true/*include_ghost*/))
              continue; //this neighbor is outside the current subdomain (TODO: Hence, different subdomain partitions
                        //may affect the results. This can be fixed in future)

            if(id[neighk][neighj][neighi] != id[k][j][i])
              continue; //this neighbor has a different ID. Skip it.

            if(neighk==k && neighj==j && neighi==i) 
              continue; //the same node. Skip

            bool neighbor_unresolved = false;
            for(auto it2 = unresolved.begin(); it2 != unresolved.end(); it2++) {
              if(neighk==(*it2)[0] && neighj==(*it2)[1] && neighi==(*it2)[2]) {
                neighbor_unresolved = true; 
                break; 
              }
            }
            if(neighbor_unresolved)
              continue; //this neighbor is also unresolved. Skip


            Vec3D& x1(coords[neighk][neighj][neighi]);
            double dist = (x1-x0).norm();

            sum_weight += dist;
            density += dist*v[neighk][neighj][neighi][0];
          }

      if(sum_weight>0) {
        v[k][j][i][0] = density/sum_weight;
        if(verbose>1) fprintf(stderr,"*** (%d,%d,%d): Updated density by interpolation w/ stencil width = %d: %e %e %e %e %e\n",
                            i,j,k, layer, v[k][j][i][0], v[k][j][i][1], v[k][j][i][2], v[k][j][i][3], v[k][j][i][4]);
        break; //done with this node
      }

    }

    //Very unlikely. This means there is no neighbors within max_layer that have the same ID!!
    if(sum_weight==0) { 
      fprintf(stderr,"\033[0;35mWarning: Updating phase change at (%d,%d,%d)(%e,%e,%e) with pre-specified density (%e). "
                     "Id:%d->%d. No valid neighbors within %d layers.\033[0m\n", 
                     i,j,k, coords[k][j][i][0], coords[k][j][i][1],
                     coords[k][j][i][2], varFcn[id[k][j][i]]->failsafe_density, (int)idn[k][j][i], (int)id[k][j][i], max_layer);
      v[k][j][i][0] = varFcn[id[k][j][i]]->failsafe_density;
      failure++;
    }
  }

  MPI_Allreduce(MPI_IN_PLACE, &failure, 1, MPI_INT, MPI_SUM, comm);

  if(failure>0) {
    ID.RestoreDataPointerAndInsert();
    ID.WriteToVTRFile("ID.vtr");
    IDn.RestoreDataPointerAndInsert();
    IDn.WriteToVTRFile("IDn.vtr");
    exit_mpi();
  }

  V.RestoreDataPointerAndInsert(); //insert data & communicate with neighbor subd's

  ID.RestoreDataPointerToLocalVector();
  IDn.RestoreDataPointerToLocalVector();
  coordinates.RestoreDataPointerToLocalVector();


}

//-------------------------------------------------------------------------
// This function checks for physical phase transitions based on varFcn. If
// found, the levelset (phi), the material ID, and possibly also the state
// variables V will be updated. The function returns the total number of
// nodes undergoing phase transitions. If it is non-zero, all the level set
// functions should be reinitialized (done outside of MultiPhaseOperator)
int 
MultiPhaseOperator::UpdatePhaseTransitions(vector<SpaceVariable3D*> &Phi, SpaceVariable3D &ID, 
                                           SpaceVariable3D &V, vector<int> &phi_updated, 
                                           vector<Int3> *new_useful_nodes)
{
  if(trans.size()==0)
    return 0; //nothing to do


  int NX, NY, NZ;
  coordinates.GetGlobalSize(&NX, &NY, &NZ);

  //---------------------------------------------
  // Step 1: Check for phase transitions; Update
  //         ID and V
  //---------------------------------------------
  double*** id  = ID.GetDataPointer();
  Vec5D***  v   = (Vec5D***) V.GetDataPointer();
  double*** lam = Lambda.GetDataPointer();

  int counter = 0;
  vector<tuple<Int3,int,int> >  changed; //(i,j,k), old id, new id -- incl. ghosts inside physical domain

  set<int> affected_ids;

  //DEBUG!!
/*
  Vec3D*** coords = (Vec3D***)coordinates.GetDataPointer();
  for(int k=k0; k<kmax; k++)
    for(int j=j0; j<jmax; j++)
      for(int i=i0; i<imax; i++) {
        if(coords[k][j][i].norm()<0.05) {
          //v[k][j][i][0] = 8.9e-4;
          //v[k][j][i][4] = 1.2e10;
          int myid = id[k][j][i]; 
          double p0 = v[k][j][i][4];
          double e0 = varFcn[myid]->GetInternalEnergyPerUnitMass(v[k][j][i][0], p0);
          double T0 = varFcn[myid]->GetTemperature(v[k][j][i][0], e0);
          double T  = T0 + 0.4;
          double e  = varFcn[myid]->GetInternalEnergyPerUnitMassFromTemperature(v[k][j][i][0], T);
          double p  = varFcn[myid]->GetPressure(v[k][j][i][0], e);
          v[k][j][i][4] = p;
          if(coords[k][j][i].norm()<0.015)
            fprintf(stderr,"Changing state at (%d,%d,%d) p: %e->%e, T: %e->%e, h: %e->%e\n", i,j,k, 
                    p0, p, T0, T, e0 + p0/v[k][j][i][0], e + p/v[k][j][i][0]);
        }
      }
  coordinates.RestoreDataPointerToLocalVector();
  V.RestoreDataPointerAndInsert();
  v = (Vec5D***) V.GetDataPointer();
*/

  int myid;
  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {

        myid = (int)id[k][j][i];
        // skip nodes outside the physical domain
        if(coordinates.OutsidePhysicalDomain(i,j,k))
          continue;

//        if(lam[k][j][i]>0)
//          fprintf(stderr,"lam[%d][%d][%d] = %e.\n", k,j,i, lam[k][j][i]);

        for(auto it = trans[myid].begin(); it != trans[myid].end(); it++) {

          //for debug only
          double rho0 = v[k][j][i][0];
          double p0 = v[k][j][i][4]; 
          double e0 = varFcn[myid]->GetInternalEnergyPerUnitMass(rho0, p0);
          double T0 = varFcn[myid]->GetTemperature(rho0, e0);

          if((*it)->Transition(v[k][j][i], lam[k][j][i])) { //NOTE: v[k][j][i][4] (p) and lam may be modified even if the 
                                                            //      return value is FALSE
            // detected phase transition

            // register the node
            changed.push_back(std::make_tuple(Int3(i,j,k), myid, (*it)->toID));

            // register the involved material ids
            affected_ids.insert(myid);
            affected_ids.insert((*it)->toID);

            // update id
            id[k][j][i] = (*it)->toID;

            // ------------------------------------------------------------------------
            // print to screen (for debug only)
            double rho1 = v[k][j][i][0];
            double p1 = v[k][j][i][4];
            double e1 = varFcn[(*it)->toID]->GetInternalEnergyPerUnitMass(rho1, p1);
            double T1 = varFcn[(*it)->toID]->GetTemperature(rho1, e1);
            fprintf(stderr,"Detected phase transition at (%d,%d,%d)(%d->%d). rho: %e->%e, p: %e->%e, T: %e->%e, h: %e->%e.\n", 
                    i,j,k, myid, (*it)->toID, rho0, rho1, p0, p1, T0, T1, e0+p0/rho0, e1+p1/rho1);
            // ------------------------------------------------------------------------


            // if node is next to a symmetry or wall boundary, update the ID of the ghost node (V will be updated by spo)
            if(i==0 && (iod.mesh.bc_x0==MeshData::WALL || iod.mesh.bc_x0==MeshData::SYMMETRY))  
              id[k][j][i-1] = id[k][j][i];
            if(i==NX-1 && (iod.mesh.bc_xmax==MeshData::WALL || iod.mesh.bc_xmax==MeshData::SYMMETRY))
              id[k][j][i+1] = id[k][j][i];
         
            if(j==0 && (iod.mesh.bc_y0==MeshData::WALL || iod.mesh.bc_y0==MeshData::SYMMETRY))  
              id[k][j-1][i] = id[k][j][i];
            if(j==NY-1 && (iod.mesh.bc_ymax==MeshData::WALL || iod.mesh.bc_ymax==MeshData::SYMMETRY))
              id[k][j+1][i] = id[k][j][i];
         
            if(k==0 && (iod.mesh.bc_z0==MeshData::WALL || iod.mesh.bc_z0==MeshData::SYMMETRY))  
              id[k-1][j][i] = id[k][j][i];
            if(k==NZ-1 && (iod.mesh.bc_zmax==MeshData::WALL || iod.mesh.bc_zmax==MeshData::SYMMETRY))
              id[k+1][j][i] = id[k][j][i];
         

            counter++;
            break;
          }
        }
      }

  MPI_Allreduce(MPI_IN_PLACE, &counter, 1, MPI_INT, MPI_SUM, comm);

  Lambda.RestoreDataPointerAndInsert();

  if(counter>0) {
    ID.RestoreDataPointerAndInsert();
    V.RestoreDataPointerAndInsert();
  } else {
    ID.RestoreDataPointerToLocalVector();
    V.RestoreDataPointerToLocalVector();
    return 0;
  }


  //---------------------------------------------
  // Step 2: Figure out which level set functions
  //         need to be updated
  //---------------------------------------------
  for(int ls = 0; ls < Phi.size(); ls++)
    phi_updated[ls] = (affected_ids.find(ls2matid[ls]) != affected_ids.end());
  MPI_Allreduce(MPI_IN_PLACE, (int*)phi_updated.data(), Phi.size(), MPI_INT, MPI_MAX, comm);

    
  //---------------------------------------------
  // Step 3: Update Phi
  //---------------------------------------------
  UpdatePhiAfterPhaseTransitions(Phi, ID, changed, phi_updated, new_useful_nodes);


  if(verbose>=1)
    print("- Detected phase/material transitions at %d node(s).\n", counter);

  return counter;

}


//-------------------------------------------------------------------------

int
MultiPhaseOperator::ResolveConflictsInLevelSets(int time_step, vector<SpaceVariable3D*> &Phi)
{

  int ls_size = Phi.size();

  if(ls_size==0)
    return 0; //nothing to do

  int resolved_conflicts = 0;

  vector<double***> phi(ls_size, NULL);

  for(int ls=0; ls<ls_size; ls++)
    phi[ls] = Phi[ls]->GetDataPointer();


  // ------------------------------------------
  // PART I: Find & resolve cells that are
  //         covered by more than one subdomain
  // ------------------------------------------

  if(ls_size>=2) {
    // loop through all the nodes
    vector<int> boundaries;
    vector<int> owner, inter;
    for(int k=kk0; k<kkmax; k++)
      for(int j=jj0; j<jjmax; j++)
        for(int i=ii0; i<iimax; i++) {

          boundaries.clear();

          //----------------------------------------------------------
          // Find subdomains that have this node next to its boundary
          //----------------------------------------------------------
          for(int ls=0; ls<ls_size; ls++) {
            if((i-1>=ii0  && phi[ls][k][j][i]*phi[ls][k][j][i-1]<=0) ||
               (i+1<iimax && phi[ls][k][j][i]*phi[ls][k][j][i+1]<=0) ||
               (j-1>=jj0  && phi[ls][k][j][i]*phi[ls][k][j-1][i]<=0) ||
               (j+1<jjmax && phi[ls][k][j][i]*phi[ls][k][j+1][i]<=0) ||
               (k-1>=kk0  && phi[ls][k][j][i]*phi[ls][k-1][j][i]<=0) ||
               (k+1<kkmax && phi[ls][k][j][i]*phi[ls][k+1][j][i]<=0))
              boundaries.push_back(ls);
          }

          if(boundaries.size()<=1) //nothing to worry about
            continue;

          owner.clear();
          inter.clear();
          for(auto it = boundaries.begin(); it != boundaries.end(); it++) {
            if(phi[*it][k][j][i]<0) 
              owner.push_back(*it);
            else if(phi[*it][k][j][i]==0)
              inter.push_back(*it);
          }

          if(owner.size()==0 && inter.size()==0)
            continue; //this node does not belong to any of the subdomains.

          if(owner.size()==1) {//great
            continue; // do nothing
/*
            double new_phi = 0.0;
            for(auto it = boundaries.begin(); it != boundaries.end(); it++)
              new_phi += fabs(phi[*it][k][j][i]); 
            new_phi /= boundaries.size();
            for(auto it = boundaries.begin(); it != boundaries.end(); it++) {
              if(phi[*it][k][j][i]<0)
                phi[*it][k][j][i] = -new_phi;
              else
                phi[*it][k][j][i] = new_phi;

            }
*/
          }
          else if(owner.size()==0) {// inter.size() is not 0
            continue; // do nothing
/*
            double new_phi = 0.0;
            for(auto it = boundaries.begin(); it != boundaries.end(); it++)
              new_phi += fabs(phi[*it][k][j][i]); 
            new_phi /= boundaries.size();
            for(auto it = boundaries.begin(); it != boundaries.end(); it++) {
              if(*it==inter[0]) //you are the owner
                phi[*it][k][j][i] = -new_phi;
              else
                phi[*it][k][j][i] = new_phi;

            }
*/
          }
          else {//owner.size()>1

            //1. find a unique owner
            int new_owner = owner[0];
            double max_phi = fabs(phi[owner[0]][k][j][i]);
            for(int ind=1; ind<owner.size(); ind++) {
              if(fabs(phi[owner[ind]][k][j][i])>max_phi) {
                new_owner = owner[ind];
                max_phi = fabs(phi[owner[ind]][k][j][i]);
              }
            }

            //2. get a new phi (abs. value)
            double new_phi = 0.0;
            for(auto it = owner.begin(); it != owner.end(); it++)
              new_phi += fabs(phi[*it][k][j][i]); 
            new_phi /= owner.size();

            //3. update all the involved level set functions
            for(auto it = owner.begin(); it != owner.end(); it++) {
              if(*it==new_owner) //you are the owner
                phi[*it][k][j][i] = -new_phi;
              else
                phi[*it][k][j][i] = new_phi;

            }

            resolved_conflicts++;
          }
        }
  }


  // ------------------------------------------
  // PART II: (Optional) Find & resolve cells 
  //          that are trapped between material
  //          interfaces
  // ------------------------------------------

  if(iod.multiphase.resolve_isolated_cells_frequency>0 &&
     time_step % iod.multiphase.resolve_isolated_cells_frequency == 0) {

    int NX, NY, NZ;
    coordinates.GetGlobalSize(&NX, &NY, &NZ);
    
    // loop through the domain interior
    for(int k=k0; k<kmax; k++)
      for(int j=j0; j<jmax; j++)
        for(int i=i0; i<imax; i++) {

          bool background_cell = true; 
          for(int ls=0; ls<ls_size; ls++) {
            if(phi[ls][k][j][i]<0) {
              background_cell = false; 
              break; //not a background cell (ID != 0)
            }
          }
          if(!background_cell)
            continue;

          int qi = 0; //like "Qi" in Weiqi/"Go"
          // check neighbors to see if this is an isolated cell
          // left neighbor
          if(i-1>=0) {
            bool connected = true;
            for(int ls=0; ls<ls_size; ls++)
              if(phi[ls][k][j][i-1]<0) {
                connected = false;
                break;
              } 
            if(connected) {
              qi++;
              if(qi>=2)
                continue;
            }
          }
  
          // right neighbor
          if(i+1<NX) {
            bool connected = true;
            for(int ls=0; ls<ls_size; ls++)
              if(phi[ls][k][j][i+1]<0) {
                connected = false;
                break;
              } 
            if(connected) {
              qi++;
              if(qi>=2)
                continue;
            }
          }
 
          // bottom neighbor
          if(j-1>=0) {
            bool connected = true;
            for(int ls=0; ls<ls_size; ls++)
              if(phi[ls][k][j-1][i]<0) {
                connected = false;
                break;
              } 
            if(connected) {
              qi++;
              if(qi>=2)
                continue;
            }
          }
  
          // top neighbor
          if(j+1<NY) {
            bool connected = true;
            for(int ls=0; ls<ls_size; ls++)
              if(phi[ls][k][j+1][i]<0) {
                connected = false;
                break;
              } 
            if(connected) {
              qi++;
              if(qi>=2)
                continue;
            }
          }
 
          // back neighbor
          if(k-1>=0) {
            bool connected = true;
            for(int ls=0; ls<ls_size; ls++)
              if(phi[ls][k-1][j][i]<0) {
                connected = false;
                break;
              } 
            if(connected) {
              qi++;
              if(qi>=2)
                continue;
            }
          }
  
          // front neighbor
          if(k+1<NZ) {
            bool connected = true;
            for(int ls=0; ls<ls_size; ls++)
              if(phi[ls][k+1][j][i]<0) {
                connected = false;
                break;
              } 
            if(connected) {
              qi++;
              if(qi>=2)
                continue;
            }
          }
 
          // qi has to be 0 or 1 now
 
          if(qi==1 && (time_step % 2*iod.multiphase.resolve_isolated_cells_frequency != 0))
            continue;

          // ---------------------------------------
          // This is an isolated background cell.
          // ---------------------------------------
          double min_phi = DBL_MAX; 
          int new_owner = -1;
          for(int ls=0; ls<ls_size; ls++) {
            if(phi[ls][k][j][i]<min_phi) {
              new_owner = ls;
              min_phi   = phi[ls][k][j][i];
            }
          }
          assert(min_phi>=0);
          phi[new_owner][k][j][i] = -min_phi;
            
          resolved_conflicts++;
        }

  }


  MPI_Allreduce(MPI_IN_PLACE, &resolved_conflicts, 1, MPI_INT, MPI_SUM, comm);


  for(int ls=0; ls<Phi.size(); ls++) {
    if(resolved_conflicts>0)
      Phi[ls]->RestoreDataPointerAndInsert();
    else
      Phi[ls]->RestoreDataPointerToLocalVector();
  }


  return resolved_conflicts;

}

//-------------------------------------------------------------------------

void
MultiPhaseOperator::AddLambdaToEnthalpyAfterInterfaceMotion(SpaceVariable3D &IDn, SpaceVariable3D &ID, 
                                                            SpaceVariable3D &V)
{

  if(trans.size()==0)
    return; //nothing to do

  double*** idn = IDn.GetDataPointer();
  double*** id  = ID.GetDataPointer();
  Vec5D***  v   = (Vec5D***) V.GetDataPointer();
  double*** lam = Lambda.GetDataPointer();

  int myidn, myid;
  int counter = 0;
  for(int k=k0; k<kmax; k++)
    for(int j=j0; j<jmax; j++)
      for(int i=i0; i<imax; i++) {

        myidn = (int)idn[k][j][i];
        myid  = (int)id[k][j][i];

        if(myidn == myid) //id remains the same. Skip
          continue;

        if(lam[k][j][i]<=0.0) //no latent heat here
          continue;

        for(auto it = trans[myidn].begin(); it != trans[myidn].end(); it++) {

          // check if this is the phase transition from "myidn" to "myid"
          if((*it)->ToID() != myid)
            continue;

          //---------------------------------------------------------------------
          // Now, do the actual work: Add lam to enthalpy
          double rho = v[k][j][i][0];
          double p   = v[k][j][i][4];
          double e   = varFcn[myid]->GetInternalEnergyPerUnitMass(v[k][j][i][0], v[k][j][i][4]);
          double h   = e + p/rho + lam[k][j][i]; //adding lam
          lam[k][j][i] = 0.0;
          e = varFcn[myid]->GetInternalEnergyPerUnitMassFromEnthalpy(rho, h);
          // update p to account for the increase of enthalpy (rho is fixed)
          v[k][j][i][4] = varFcn[myid]->GetPressure(rho, e);

          counter++;
          //---------------------------------------------------------------------
          
        }
      }

  MPI_Allreduce(MPI_IN_PLACE, &counter, 1, MPI_INT, MPI_SUM, comm);

  IDn.RestoreDataPointerToLocalVector();
  ID.RestoreDataPointerToLocalVector();
  if(counter>0) {  
    Lambda.RestoreDataPointerAndInsert();
    V.RestoreDataPointerAndInsert();
  } else { //nothing is changed...
    Lambda.RestoreDataPointerToLocalVector();
    V.RestoreDataPointerToLocalVector();
  }

}

//-----------------------------------------------------

void
MultiPhaseOperator::UpdatePhiAfterPhaseTransitions(vector<SpaceVariable3D*> &Phi, SpaceVariable3D &ID, 
                                                   vector<tuple<Int3,int,int> > &changed, 
                                                   vector<int> &phi_updated, vector<Int3> *new_useful_nodes)
{
  // This function will provide correct value of phi (up to dx error) ONLY for first layer nodes.
  // Reinitialization is needed to find the value of phi elsewhere
  // Note that "changed" should include ghost nodes inside the physical domain

  int NX, NY, NZ;
  coordinates.GetGlobalSize(&NX, &NY, &NZ);

  Vec3D*** dxyz = (Vec3D***)delta_xyz.GetDataPointer();
  double*** id  = ID.GetDataPointer();

  for(int ls = 0; ls<Phi.size(); ls++) {//loop through all the level set functions
  
    if(phi_updated[ls] == 0)
      continue; //this level set function is not involved

    double*** phi = Phi[ls]->GetDataPointer();
    int matid = ls2matid[ls];

    int i,j,k;
    for(auto it = changed.begin(); it != changed.end(); it++) {
 
      if(matid != get<1>(*it) && matid != get<2>(*it))
        continue;

      i = get<0>(*it)[0];
      j = get<0>(*it)[1];
      k = get<0>(*it)[2];

      //first, push new nodes into the vector of new_useful_nodes
      new_useful_nodes[ls].push_back(get<0>(*it));
      if(i-1>=ii0)  new_useful_nodes[ls].push_back(Int3(i-1,j,k));
      if(i+1<iimax) new_useful_nodes[ls].push_back(Int3(i+1,j,k));
      if(j-1>=jj0)  new_useful_nodes[ls].push_back(Int3(i,j-1,k));
      if(j+1<jjmax) new_useful_nodes[ls].push_back(Int3(i,j+1,k));
      if(k-1>=kk0)  new_useful_nodes[ls].push_back(Int3(i,j,k-1));
      if(k+1<kkmax) new_useful_nodes[ls].push_back(Int3(i,j,k+1));


      if(matid == get<1>(*it)) { //this node is moving outside of the subdomain

        // update phi at this node
        phi[k][j][i] = 0.5*std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1],dxyz[k][j][i][2]));        

        // update phi at neighbors that are in the physical domain, and have opposite sign 
        if(i-1>=ii0  && i-1>=0 && phi[k][j][i-1]<=0)
          phi[k][j][i-1] = std::max(phi[k][j][i-1], -0.5*dxyz[k][j][i-1][0]);
        if(i+1<iimax && i+1<NX && phi[k][j][i+1]<=0)
          phi[k][j][i+1] = std::max(phi[k][j][i+1], -0.5*dxyz[k][j][i+1][0]);
        if(j-1>=jj0  && j-1>=0 && phi[k][j-1][i]<=0) 
          phi[k][j-1][i] = std::max(phi[k][j-1][i], -0.5*dxyz[k][j-1][i][1]);
        if(j+1<jjmax && j+1<NY && phi[k][j+1][i]<=0)
          phi[k][j+1][i] = std::max(phi[k][j+1][i], -0.5*dxyz[k][j+1][i][1]);
        if(k-1>=kk0  && k-1>=0 && phi[k-1][j][i]<=0) 
          phi[k-1][j][i] = std::max(phi[k-1][j][i], -0.5*dxyz[k-1][j][i][2]);
        if(k+1<kkmax && k+1<NZ && phi[k+1][j][i]<=0)
          phi[k+1][j][i] = std::max(phi[k+1][j][i], -0.5*dxyz[k+1][j][i][2]);
      }
      else if(matid == get<2>(*it)) { //this node is moving inside the subdomain

        // update phi at this node
        phi[k][j][i] = -0.5*std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1],dxyz[k][j][i][2]));        

        // update phi at neighbors that are in the physical domain, and have opposite sign 
        if(i-1>=ii0  && i-1>=0 && phi[k][j][i-1]>=0)
          phi[k][j][i-1] = std::min(phi[k][j][i-1], 0.5*dxyz[k][j][i-1][0]);
        if(i+1<iimax && i+1<NX && phi[k][j][i+1]>=0)
          phi[k][j][i+1] = std::min(phi[k][j][i+1], 0.5*dxyz[k][j][i+1][0]);
        if(j-1>=jj0  && j-1>=0 && phi[k][j-1][i]>=0) 
          phi[k][j-1][i] = std::min(phi[k][j-1][i], 0.5*dxyz[k][j-1][i][1]);
        if(j+1<jjmax && j+1<NY && phi[k][j+1][i]>=0)
          phi[k][j+1][i] = std::min(phi[k][j+1][i], 0.5*dxyz[k][j+1][i][1]);
        if(k-1>=kk0  && k-1>=0 && phi[k-1][j][i]>=0) 
          phi[k-1][j][i] = std::min(phi[k-1][j][i], 0.5*dxyz[k-1][j][i][2]);
        if(k+1<kkmax && k+1<NZ && phi[k+1][j][i]>=0)
          phi[k+1][j][i] = std::min(phi[k+1][j][i], 0.5*dxyz[k+1][j][i][2]);
      }

    }

    Phi[ls]->RestoreDataPointerAndInsert();
  }

  // For debug only
  for(int ls = 0; ls<Phi.size(); ls++) {//loop through all the level set functions
  
    if(phi_updated[ls] == 0)
      continue; //this level set function is not involved

    double*** phi = Phi[ls]->GetDataPointer();
    int matid = ls2matid[ls];

    int i,j,k;
    for(auto it = changed.begin(); it != changed.end(); it++) {
      if(matid == get<1>(*it) || matid == get<2>(*it)) { 
 
        i = get<0>(*it)[0];
        j = get<0>(*it)[1];
        k = get<0>(*it)[2];

        if(phi[k][j][i]<0)
          assert(id[k][j][i] == matid);
        else
          assert(id[k][j][i] != matid);

        if(i-1>=ii0 && i-1>=0) {
          if(phi[k][j][i-1]<0)
            assert(id[k][j][i-1] == matid);
          else
            assert(id[k][j][i-1] != matid);
        }

        if(i+1<iimax && i+1<NX) {
          if(phi[k][j][i+1]<0)
            assert(id[k][j][i+1] == matid);
          else
            assert(id[k][j][i+1] != matid);
        }

        if(j-1>=jj0 && j-1>=0) {
          if(phi[k][j-1][i]<0)
            assert(id[k][j-1][i] == matid);
          else
            assert(id[k][j-1][i] != matid);
        }

        if(j+1<jjmax && j+1<NY) {
          if(phi[k][j+1][i]<0)
            assert(id[k][j+1][i] == matid);
          else
            assert(id[k][j+1][i] != matid);
        }

        if(k-1>=kk0 && k-1>=0) {
          if(phi[k-1][j][i]<0)
            assert(id[k-1][j][i] == matid);
          else
            assert(id[k-1][j][i] != matid);
        }

        if(k+1<kkmax && k+1<NZ) {
          if(phi[k+1][j][i]<0) 
            assert(id[k+1][j][i] == matid);
          else
            assert(id[k+1][j][i] != matid);
        }

      }
    }
    Phi[ls]->RestoreDataPointerToLocalVector();
  }

  delta_xyz.RestoreDataPointerToLocalVector();
  ID.RestoreDataPointerToLocalVector();

}

//-----------------------------------------------------

