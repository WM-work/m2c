#include <LevelSetReinitializer.h>
#include <cfloat> //DBL_MAX

extern int verbose;

//--------------------------------------------------------------------------

LevelSetReinitializer::LevelSetReinitializer(MPI_Comm &comm_, DataManagers3D &dm_all_, 
                           LevelSetSchemeData &iod_ls_, SpaceVariable3D &coordinates_, 
                           SpaceVariable3D &delta_xyz_, vector<GhostPoint> &ghost_nodes_inner_,
                           vector<GhostPoint> &ghost_nodes_outer_)
                     : comm(comm_), iod_ls(iod_ls_), coordinates(coordinates_),
                       delta_xyz(delta_xyz_), ghost_nodes_inner(ghost_nodes_inner_),
                       ghost_nodes_outer(ghost_nodes_outer_),
                       interp(NULL), grad(NULL),
                       Tag(comm_, &(dm_all_.ghosted1_1dof)),
                       R(comm_, &(dm_all_.ghosted1_1dof)),
                       Phi1(comm_, &(dm_all_.ghosted1_1dof)),
                       Sign(comm_, &(dm_all_.ghosted1_1dof)),
                       PhiG2(comm_, &(dm_all_.ghosted2_1dof)),
                       phi_max(-DBL_MAX), phi_min(DBL_MAX)
{
  coordinates.GetCornerIndices(&i0, &j0, &k0, &imax, &jmax, &kmax);
  coordinates.GetGhostedCornerIndices(&ii0, &jj0, &kk0, &iimax, &jjmax, &kkmax);

  Tag.SetConstantValue(0, true);

  if(true) //can add other interpolation methods later
    interp = new InterpolatorLinear(comm_, dm_all_, coordinates_, delta_xyz_);
  if(true) //can add other differentiation methods later
    grad = new GradientCalculatorCentral(comm_, dm_all_, coordinates_, delta_xyz_, *interp);

  cfl = iod_ls.reinit.cfl;
}


//--------------------------------------------------------------------------

LevelSetReinitializer::~LevelSetReinitializer()
{
  if(interp) delete interp;
  if(grad)   delete grad;
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::Destroy()
{
  if(interp)
    interp->Destroy();
  if(grad)
    grad->Destroy();

  Tag.Destroy();
  R.Destroy();
  Phi1.Destroy();
  Sign.Destroy();
  PhiG2.Destroy();
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::Reinitialize(SpaceVariable3D &Phi)
{
  // Step 1: Prep: Tag first layer nodes & store the sign function
  vector<FirstLayerNode> firstLayer;
  TagFirstLayerNodes(Phi, firstLayer); //also calculates the associated coefficients
  EvaluateSignFunction(Phi, 1.0/*smoothing coefficient*/);

  // Step 2: Reinitialize first layer nodes (no iterations needed)
  if(iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::UNCONSTRAINED||
     iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED1 ||
     iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED2) {
    Phi1.AXPlusBY(0.0, 1.0, Phi, true); //Phi1 = Phi
    ReinitializeFirstLayerNodes(Phi1, Phi, firstLayer); //Phi is updated
    ApplyBoundaryConditions(Phi);
  }

  // Step 3: Main loop -- 3rd-order Runge-Kutta w/ spatially varying dt
  double residual = 0.0;
  int iter;
  for(iter = 0; iter < iod_ls.reinit.maxIts; iter++) {

    //************** Step 1 of RK3 *****************
    residual = ComputeResidual(Phi, R, cfl);  //R = R(Phi)
    if(verbose>=1)
      print("  o Iter. %d: Residual = %e, Tol = %e.\n", iter, residual, 
            iod_ls.reinit.convergence_tolerance);
    if(residual < iod_ls.reinit.convergence_tolerance) //residual itself is nondimensional
      break;

    Phi1.AXPlusBY(0.0, 1.0, Phi); //Phi1 = Phi
    Phi1.AXPlusBY(1.0, 1.0, R);   //Phi1 = Phi + R(Phi)
    ApplyBoundaryConditions(Phi1);
    ApplyCorrectionToFirstLayerNodes(Phi1, firstLayer, cfl); //HCR-1 or HCR-2 (also apply b.c.)
    //*********************************************


    //************** Step 2 of RK3 *****************
    ComputeResidual(Phi1, R, cfl);
    Phi1.AXPlusBY(0.25, 0.75, Phi);
    Phi1.AXPlusBY(1.0, 0.25, R);
    ApplyBoundaryConditions(Phi1);
    ApplyCorrectionToFirstLayerNodes(Phi1, firstLayer, cfl); //HCR-1 or HCR-2 (also apply b.c.)
    //*********************************************

    //************** Step 3 of RK3 *****************
    ComputeResidual(Phi1, R, cfl);
    Phi.AXPlusBY(1.0/3.0, 2.0/3.0, Phi1);
    Phi.AXPlusBY(1.0, 2.0/3.0, R);
    ApplyBoundaryConditions(Phi);
    ApplyCorrectionToFirstLayerNodes(Phi, firstLayer, cfl); //HCR-1 or HCR-2 (also apply b.c.)
    //*********************************************
    
  }

  if(iter==iod_ls.reinit.maxIts)
    print("  o Warning: Failed to converge. Residual = %e, Tol = %e.\n", 
          residual, iod_ls.reinit.convergence_tolerance);
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::ReinitializeInBand(SpaceVariable3D &Phi, SpaceVariable3D &Level, 
                           SpaceVariable3D &Useful,SpaceVariable3D &Active, 
                           vector<Int3> &useful_nodes, vector<Int3> &active_nodes)
{

  // update phi_max and phi_min (only for use in updating new useful nodes)
  UpdatePhiMaxAndPhiMinInBand(Phi, useful_nodes);

  // Step 1: Prep: Tag first layer nodes & store the sign function, and update the narrow band
  vector<FirstLayerNode> firstLayer;
  vector<Int3> firstLayerIncGhost;
  TagFirstLayerNodesInBand(Phi, useful_nodes, firstLayer, firstLayerIncGhost);
  UpdateNarrowBand(Phi, firstLayerIncGhost, Level, Useful, Active, useful_nodes, active_nodes);
  EvaluateSignFunctionInBand(Phi, useful_nodes, 1.0/*smoothing coefficient*/);

  // Step 2: Reinitialize first layer nodes (no iterations needed)
  if(iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::UNCONSTRAINED||
     iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED1 ||
     iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED2) {
    Phi1.AXPlusBY(0.0, 1.0, Phi, true); //Phi1 = Phi
    ReinitializeFirstLayerNodes(Phi1, Phi, firstLayer); //Phi is updated
    ApplyBoundaryConditions(Phi, &Useful);
  }

  // Step 3: Main loop -- 3rd-order Runge-Kutta w/ spatially varying dt
  double residual = 0.0;
  int iter;
  for(iter = 0; iter < iod_ls.reinit.maxIts; iter++) {

    //************** Step 1 of RK3 *****************
    residual = ComputeResidualInBand(Phi, Useful, useful_nodes, R, cfl);  //R = R(Phi)
    if(verbose>=1)
      print("  o Iter. %d: Residual = %e, Tol = %e.\n", iter, residual, 
            iod_ls.reinit.convergence_tolerance);
    if(residual < iod_ls.reinit.convergence_tolerance) //residual itself is nondimensional
      break;

    Phi1.AXPlusBY(0.0, 1.0, Phi); //Phi1 = Phi
    Phi1.AXPlusBY(1.0, 1.0, R);   //Phi1 = Phi + R(Phi)
    ApplyBoundaryConditions(Phi1, &Useful);
    ApplyCorrectionToFirstLayerNodes(Phi1, firstLayer, cfl); //HCR-1 or HCR-2 (also apply b.c.)
    //*********************************************


    //************** Step 2 of RK3 *****************
    ComputeResidualInBand(Phi1, Useful, useful_nodes, R, cfl);
    Phi1.AXPlusBY(0.25, 0.75, Phi);
    Phi1.AXPlusBY(1.0, 0.25, R);
    ApplyBoundaryConditions(Phi1, &Useful);
    ApplyCorrectionToFirstLayerNodes(Phi1, firstLayer, cfl); //HCR-1 or HCR-2 (also apply b.c.)
    //*********************************************

    //************** Step 3 of RK3 *****************
    ComputeResidualInBand(Phi1, Useful, useful_nodes, R, cfl);
    Phi.AXPlusBY(1.0/3.0, 2.0/3.0, Phi1);
    Phi.AXPlusBY(1.0, 2.0/3.0, R);
    ApplyBoundaryConditions(Phi, &Useful);
    ApplyCorrectionToFirstLayerNodes(Phi, firstLayer, cfl); //HCR-1 or HCR-2 (also apply b.c.)
    //*********************************************
    
  }

  if(iter==iod_ls.reinit.maxIts)
    print("  o Warning: Failed to converge. Residual = %e, Tol = %e.\n", 
          residual, iod_ls.reinit.convergence_tolerance);

}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::TagFirstLayerNodes(SpaceVariable3D &Phi, vector<FirstLayerNode> &firstLayer)
{
  //*****************************************************************************
  // "Tag" is populated within both domain interior and the ghost boundary outside 
  // the physical domain (except corners).
  // "firstLayer" stores all the first-layer nodes within the physical domain (i.e.
  // Gamma in Hartmann et al. (2010))
  // For each node in Gamma, S is constructed to contain nodes on the opposite side
  // of the interface (excluding nodes with phi == 0!). For each node on the opposite
  // side, r_alpha, and \tilde{r} are computed.
  //*****************************************************************************
  firstLayer.clear();

  double*** phi   = (double***)Phi.GetDataPointer();
  double*** tag   = (double***)Tag.GetDataPointer();

  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {

        tag[k][j][i] = 0; //default

        if(Phi.OutsidePhysicalDomainAndUnpopulated(i,j,k))
          continue;

        if(i-1>=ii0 && phi[k][j][i]*phi[k][j][i-1]<=0) {
          tag[k][j][i] = 1;
          if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
            firstLayer.push_back(FirstLayerNode(i,j,k));
          continue;
        }

        if(i+1<iimax && phi[k][j][i]*phi[k][j][i+1]<=0) {
          tag[k][j][i] = 1;
          if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
            firstLayer.push_back(FirstLayerNode(i,j,k));
          continue;
        }

        if(j-1>=jj0 && phi[k][j][i]*phi[k][j-1][i]<=0) {
          tag[k][j][i] = 1;
          if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
            firstLayer.push_back(FirstLayerNode(i,j,k));
          continue;
        }

        if(j+1<jjmax && phi[k][j][i]*phi[k][j+1][i]<=0) {
          tag[k][j][i] = 1;
          if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
            firstLayer.push_back(FirstLayerNode(i,j,k));
          continue;
        }

        if(k-1>=kk0 && phi[k][j][i]*phi[k-1][j][i]<=0) {
          tag[k][j][i] = 1;
          if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
            firstLayer.push_back(FirstLayerNode(i,j,k));
          continue;
        }
        
        if(k+1<kkmax && phi[k][j][i]*phi[k+1][j][i]<=0) {
          tag[k][j][i] = 1;
          if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
            firstLayer.push_back(FirstLayerNode(i,j,k));
          continue;
        }
      }


  //Now, calculate s and r for each node in firstLayer (Gamma)
  for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {

    double sum(0.0);
    int i(it->i), j(it->j), k(it->k);
    if(phi[k][j][i]*phi[k][j][i-1]<0) { //left neighbor
      it->s[0] = true;
      it->r[0] = phi[k][j][i]/phi[k][j][i-1]; 
      it->ns++;
      sum += phi[k][j][i-1];
    }
    if(phi[k][j][i]*phi[k][j][i+1]<0) { //right neighbor
      it->s[1] = true;
      it->r[1] = phi[k][j][i]/phi[k][j][i+1]; 
      it->ns++;
      sum += phi[k][j][i+1];
    }
    if(phi[k][j][i]*phi[k][j-1][i]<0) { //bottom neighbor
      it->s[2] = true;
      it->r[2] = phi[k][j][i]/phi[k][j-1][i];
      it->ns++;
      sum += phi[k][j-1][i];
    }
    if(phi[k][j][i]*phi[k][j+1][i]<0) { //top neighbor
      it->s[3] = true;
      it->r[3] = phi[k][j][i]/phi[k][j+1][i];
      it->ns++;
      sum += phi[k][j+1][i];
    }
    if(phi[k][j][i]*phi[k-1][j][i]<0) { //back neighbor
      it->s[4] = true;
      it->r[4] = phi[k][j][i]/phi[k-1][j][i];
      it->ns++;
      sum += phi[k-1][j][i];
    }
    if(phi[k][j][i]*phi[k+1][j][i]<0) { //front neighbor
      it->s[5] = true;
      it->r[5] = phi[k][j][i]/phi[k+1][j][i];
      it->ns++;
      sum += phi[k+1][j][i];
    }
    it->r0 = (sum!=0) ? phi[k][j][i]/sum : 0.0;

  }


  Tag.RestoreDataPointerAndInsert();

  Phi.RestoreDataPointerToLocalVector(); //no changes made
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::TagFirstLayerNodesInBand(SpaceVariable3D &Phi, vector<Int3> &useful_nodes,
                                                vector<FirstLayerNode> &firstLayer,
                                                vector<Int3> &firstLayerIncGhost)
{
  //*****************************************************************************
  // "Tag" is populated within both domain interior and the ghost boundary outside 
  // the physical domain (except corners).
  // "firstLayer" stores all the first-layer nodes within the physical domain (i.e.
  // Gamma in Hartmann et al. (2010))
  // For each node in Gamma, S is constructed to contain nodes on the opposite side
  // of the interface (excluding nodes with phi == 0!). For each node on the opposite
  // side, r_alpha, and \tilde{r} are computed.
  //*****************************************************************************
  firstLayer.clear();
  firstLayerIncGhost.clear();

  double*** phi   = (double***)Phi.GetDataPointer();
  double*** tag   = (double***)Tag.GetDataPointer();

  for(auto it = useful_nodes.begin(); it != useful_nodes.end(); it++) {

    int i((*it)[0]), j((*it)[1]), k((*it)[2]);

    tag[k][j][i] = 0; //default

    if(Phi.OutsidePhysicalDomainAndUnpopulated(i,j,k))
      continue;

    if(i-1>=ii0 && phi[k][j][i]*phi[k][j][i-1]<=0) {
      tag[k][j][i] = 1;
      firstLayerIncGhost.push_back(Int3(i,j,k));
      if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
        firstLayer.push_back(FirstLayerNode(i,j,k));
      continue;
    }

    if(i+1<iimax && phi[k][j][i]*phi[k][j][i+1]<=0) {
      tag[k][j][i] = 1;
      firstLayerIncGhost.push_back(Int3(i,j,k));
      if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
        firstLayer.push_back(FirstLayerNode(i,j,k));
      continue;
    }

    if(j-1>=jj0 && phi[k][j][i]*phi[k][j-1][i]<=0) {
      tag[k][j][i] = 1;
      firstLayerIncGhost.push_back(Int3(i,j,k));
      if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
        firstLayer.push_back(FirstLayerNode(i,j,k));
      continue;
    }

    if(j+1<jjmax && phi[k][j][i]*phi[k][j+1][i]<=0) {
      tag[k][j][i] = 1;
      firstLayerIncGhost.push_back(Int3(i,j,k));
      if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
        firstLayer.push_back(FirstLayerNode(i,j,k));
      continue;
    }

    if(k-1>=kk0 && phi[k][j][i]*phi[k-1][j][i]<=0) {
      tag[k][j][i] = 1;
      firstLayerIncGhost.push_back(Int3(i,j,k));
      if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
        firstLayer.push_back(FirstLayerNode(i,j,k));
      continue;
    }
        
    if(k+1<kkmax && phi[k][j][i]*phi[k+1][j][i]<=0) {
      tag[k][j][i] = 1;
      firstLayerIncGhost.push_back(Int3(i,j,k));
      if(Phi.IsHere(i,j,k,false)) //only push nodes in the subdomain interior into firstLayer
        firstLayer.push_back(FirstLayerNode(i,j,k));
      continue;
    }
  }

  Tag.RestoreDataPointerAndInsert();


  //Update firstLayerIncGhost to account for the exchange between subdomains
  tag = (double***)Tag.GetDataPointer();
  for(auto it = ghost_nodes_inner.begin(); it != ghost_nodes_inner.end(); it++) {
    int i(it->ijk[0]), j(it->ijk[1]), k(it->ijk[2]);
    if(tag[k][j][i]==1 && 
       std::find(firstLayerIncGhost.begin(), firstLayerIncGhost.end(), it->ijk) == firstLayerIncGhost.end())
      firstLayerIncGhost.push_back(it->ijk);
  } 
  for(auto it = ghost_nodes_outer.begin(); it != ghost_nodes_outer.end(); it++) {
    int i(it->ijk[0]), j(it->ijk[1]), k(it->ijk[2]);
    if(tag[k][j][i]==1 && 
       std::find(firstLayerIncGhost.begin(), firstLayerIncGhost.end(), it->ijk) == firstLayerIncGhost.end())
      firstLayerIncGhost.push_back(it->ijk);
  }
  Tag.RestoreDataPointerToLocalVector();


  //Now, calculate s and r for each node in firstLayer (Gamma)
  for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {

    double sum(0.0);
    int i(it->i), j(it->j), k(it->k);
    if(phi[k][j][i]*phi[k][j][i-1]<0) { //left neighbor
      it->s[0] = true;
      it->r[0] = phi[k][j][i]/phi[k][j][i-1]; 
      it->ns++;
      sum += phi[k][j][i-1];
    }
    if(phi[k][j][i]*phi[k][j][i+1]<0) { //right neighbor
      it->s[1] = true;
      it->r[1] = phi[k][j][i]/phi[k][j][i+1]; 
      it->ns++;
      sum += phi[k][j][i+1];
    }
    if(phi[k][j][i]*phi[k][j-1][i]<0) { //bottom neighbor
      it->s[2] = true;
      it->r[2] = phi[k][j][i]/phi[k][j-1][i];
      it->ns++;
      sum += phi[k][j-1][i];
    }
    if(phi[k][j][i]*phi[k][j+1][i]<0) { //top neighbor
      it->s[3] = true;
      it->r[3] = phi[k][j][i]/phi[k][j+1][i];
      it->ns++;
      sum += phi[k][j+1][i];
    }
    if(phi[k][j][i]*phi[k-1][j][i]<0) { //back neighbor
      it->s[4] = true;
      it->r[4] = phi[k][j][i]/phi[k-1][j][i];
      it->ns++;
      sum += phi[k-1][j][i];
    }
    if(phi[k][j][i]*phi[k+1][j][i]<0) { //front neighbor
      it->s[5] = true;
      it->r[5] = phi[k][j][i]/phi[k+1][j][i];
      it->ns++;
      sum += phi[k+1][j][i];
    }
    it->r0 = (sum!=0) ? phi[k][j][i]/sum : 0.0;

  }


  Phi.RestoreDataPointerToLocalVector(); //no changes made
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::EvaluateSignFunction(SpaceVariable3D &Phi, double eps)
{
  double*** phi   = (double***)Phi.GetDataPointer();
  Vec3D*** dxyz   = (Vec3D***)delta_xyz.GetDataPointer();
  double*** sign  = (double***)Sign.GetDataPointer();

  double factor;
  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {

        factor = eps*std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1], dxyz[k][j][i][2]));

        sign[k][j][i] = phi[k][j][i] / sqrt(phi[k][j][i]*phi[k][j][i] + factor*factor);

      }

  Phi.RestoreDataPointerToLocalVector();
  delta_xyz.RestoreDataPointerToLocalVector();

  Sign.RestoreDataPointerAndInsert(); 
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::EvaluateSignFunctionInBand(SpaceVariable3D &Phi, vector<Int3> &useful_nodes,
                                                  double eps)
{
  double*** phi   = (double***)Phi.GetDataPointer();
  Vec3D*** dxyz   = (Vec3D***)delta_xyz.GetDataPointer();
  double*** sign  = (double***)Sign.GetDataPointer();

  double factor;
  for(auto it = useful_nodes.begin(); it != useful_nodes.end(); it++) {
    int i((*it)[0]), j((*it)[1]), k((*it)[2]);
    factor = eps*std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1], dxyz[k][j][i][2]));
    sign[k][j][i] = phi[k][j][i] / sqrt(phi[k][j][i]*phi[k][j][i] + factor*factor);
  }

  Phi.RestoreDataPointerToLocalVector();
  delta_xyz.RestoreDataPointerToLocalVector();

  Sign.RestoreDataPointerAndInsert(); 
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::ReinitializeFirstLayerNodes(SpaceVariable3D &Phi0, SpaceVariable3D &Phi, 
                                                   vector<FirstLayerNode> &firstLayer)
{
  //****************************************
  // Implementing the RSU, CR-1 and CR-2 algos
  // in Hartmann et al. (2008)
  //****************************************

  PopulatePhiG2(Phi0);

  int NX, NY, NZ;
  Phi.GetGlobalSize(&NX, &NY, &NZ);

  double*** phi   = (double***)Phi.GetDataPointer();
  double*** phig  = (double***)PhiG2.GetDataPointer();
  double*** tag   = (double***)Tag.GetDataPointer();
  Vec3D*** dxyz   = (Vec3D***)delta_xyz.GetDataPointer();
  Vec3D*** coords = (Vec3D***)coordinates.GetDataPointer();

  int i,j,k;
  Vec3D gradphi;
  double gradphi_norm;
  double epsx, epsy, epsz;
  for(int n=0; n<firstLayer.size(); n++) {

    // This must be a node in the interior of the subdomain (see how firstLayer is populated)
    i = firstLayer[n].i; 
    j = firstLayer[n].j; 
    k = firstLayer[n].k; 

    // set phi = phi / |grad(phi)|, using tagged nodes to calculate the derivatives 
    // ref: Hartmann et al. 2008, Eqs.(20)(21) (which cites another paper)
    
    //Eq.(21a) of Hartmann et al., 2008
    epsx = 1.0e-3*dxyz[k][j][i][0];
    epsy = 1.0e-3*dxyz[k][j][i][1];
    epsz = 1.0e-3*dxyz[k][j][i][2];
    gradphi[0] = DifferentiateInFirstLayer(coords[k][j][i-1][0], coords[k][j][i][0], coords[k][j][i+1][0],
                                              tag[k][j][i-1],       tag[k][j][i],       tag[k][j][i+1],
                                             phig[k][j][i-1],      phig[k][j][i],      phig[k][j][i+1], 
                                           (i-2>-1) ? phig[k][j][i-2] : phig[k][j][i-1],
                                           (i+2<=NX) ? phig[k][j][i+2] : phig[k][j][i+1], epsx);
    gradphi[1] = DifferentiateInFirstLayer(coords[k][j-1][i][1], coords[k][j][i][1], coords[k][j+1][i][1],
                                              tag[k][j-1][i],       tag[k][j][i],       tag[k][j+1][i],
                                             phig[k][j-1][i],      phig[k][j][i],      phig[k][j+1][i],
                                           (j-2>-1) ? phig[k][j-2][i] : phig[k][j-1][i],
                                           (j+2<=NY) ? phig[k][j+2][i] : phig[k][j+2][i], epsy);
    gradphi[2] = DifferentiateInFirstLayer(coords[k-1][j][i][2], coords[k][j][i][2], coords[k+1][j][i][2],
                                              tag[k-1][j][i],       tag[k][j][i],       tag[k+1][j][i],
                                             phig[k-1][j][i],      phig[k][j][i],      phig[k+1][j][i],
                                           (k-2>-1) ? phig[k-2][j][i] : phig[k-1][j][i],
                                           (k+2<=NZ) ? phig[k+2][j][i] : phig[k+1][j][i], epsz);
    gradphi_norm = gradphi.norm();

    if(gradphi_norm == 0) {
      fprintf(stderr,"Warning: (%d,%d,%d)(%e,%e,%e): Updating first layer node led to zero gradient.\n",
              i,j,k,coords[k][j][i][0],coords[k][j][i][1],coords[k][j][i][2]);
      phi[k][j][i] = phig[k][j][i];
    } else
      phi[k][j][i] = phig[k][j][i]/gradphi_norm;
  }


  // Applying the averaging algorithm (CR-1 or CR-2)
  if(iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED1) {

    for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {
      i = it->i; 
      j = it->j; 
      k = it->k; 
      it->f = 0.0; //reset to 0 
      double sum(0.0);
      if(it->s[0])  sum += phi[k][j][i-1]/phig[k][j][i-1];
      if(it->s[1])  sum += phi[k][j][i+1]/phig[k][j][i+1];
      if(it->s[2])  sum += phi[k][j-1][i]/phig[k][j-1][i];
      if(it->s[3])  sum += phi[k][j+1][i]/phig[k][j+1][i];
      if(it->s[4])  sum += phi[k-1][j][i]/phig[k-1][j][i];
      if(it->s[5])  sum += phi[k+1][j][i]/phig[k+1][j][i];

      if(it->ns != 0)
        it->f = phig[k][j][i]*sum/(double)(it->ns);
      else
        it->f = 0.0;
    }

    for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {
      i = it->i; 
      j = it->j; 
      k = it->k;
      phi[k][j][i] = it->f;
      it->f = 0.0; //reset
    }

  }
  else if(iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED2) {

    for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {
      i = it->i; 
      j = it->j; 
      k = it->k; 
      it->f = 0.0; //reset to 0 
      double sum1(0.0), sum2(0.0);
      if(it->s[0]) {sum1 += phi[k][j][i-1];  sum2 += phig[k][j][i-1];}
      if(it->s[1]) {sum1 += phi[k][j][i+1];  sum2 += phig[k][j][i+1];}
      if(it->s[2]) {sum1 += phi[k][j-1][i];  sum2 += phig[k][j-1][i];}
      if(it->s[3]) {sum1 += phi[k][j+1][i];  sum2 += phig[k][j+1][i];}
      if(it->s[4]) {sum1 += phi[k-1][j][i];  sum2 += phig[k-1][j][i];}
      if(it->s[5]) {sum1 += phi[k+1][j][i];  sum2 += phig[k+1][j][i];}

      if(sum2 != 0)
        it->f = phig[k][j][i]*sum1/sum2;
      else //this would happen only when s[0] = ... = s[5] = false, meaning phig[k][j][i]=0.
        it->f = 0.0;
    }

    for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {
      i = it->i; 
      j = it->j; 
      k = it->k;
      phi[k][j][i] = it->f;
      it->f = 0.0; //reset
    }
  }


  Phi.RestoreDataPointerAndInsert();

  PhiG2.RestoreDataPointerToLocalVector();
  Tag.RestoreDataPointerToLocalVector();
  coordinates.RestoreDataPointerToLocalVector();
  delta_xyz.RestoreDataPointerToLocalVector();
  
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::PopulatePhiG2(SpaceVariable3D &Phi0)
{
  PhiG2.SetConstantValue(0.0, true);

  double*** phig2 = (double***)PhiG2.GetDataPointer();
  double*** phi0  = (double***)Phi0.GetDataPointer();

  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {
        phig2[k][j][i] = phi0[k][j][i]; 
      }

  Phi0.RestoreDataPointerToLocalVector();
  PhiG2.RestoreDataPointerAndInsert();
}

//--------------------------------------------------------------------------

//Eq.(21a) of Hartmann et al., 2008, simplified
double
LevelSetReinitializer::DifferentiateInFirstLayer(double x0, double x1, double x2, 
                                                 double tag0, double tag1, double tag2,
                                                 double phi0, double phi1, double phi2, 
                                                 double phi00, double phi3, double eps)
{
  bool phi0_useful = true, phi2_useful = true;

  //identify cases in which phi0 is not useful
  if(tag0==0) phi0_useful = false;
  if(tag2==0) phi2_useful = false;
  if(!phi0_useful && !phi2_useful)
    return 0.0;

  double dphi0 = phi1 - phi0;
  double dphi1 = phi2 - phi1;

  bool condB = dphi0*dphi1<0 || phi0*phi00<0 || phi2*phi3<0;
  
  if(condB) {
    if(phi0_useful) {
      bool condA = (phi0*phi2<0) && fabs(dphi0 + eps)<fabs(dphi1);
      if(condA)
        phi0_useful = false;
    }
    if(phi2_useful) {
      bool condA = (phi0*phi2<0) && fabs(dphi1 + eps)<fabs(dphi0);
      if(condA)
        phi2_useful = false;
    }
  }

  if(!phi0_useful && !phi2_useful)
    return 0.0;


  if(phi0_useful) {
    if(phi2_useful) { //central differencing
      double c0 = -(x2-x1)/((x1-x0)*(x2-x0));
      double c1 = 1.0/(x1-x0) - 1.0/(x2-x1);
      double c2 = (x1-x0)/((x2-x0)*(x2-x1));
      return c0*phi0 + c1*phi1 + c2*phi2;
    }
    else  //backward difference
      return (phi1-phi0)/(x1-x0);
  }
  else if(phi2_useful) { //forward difference
    return (phi2-phi1)/(x2-x1);
  }
  else //neigher phi0 nor phi2 useful
    return 0.0;    

  return 0.0;
}

//--------------------------------------------------------------------------

double
LevelSetReinitializer::ComputeResidual(SpaceVariable3D &Phi, SpaceVariable3D &R, double cfl)
{

  int NX, NY, NZ;
  Phi.GetGlobalSize(&NX, &NY, &NZ);

  // get data
  double*** tag    = (double***)Tag.GetDataPointer();
  double*** sign   = (double***)Sign.GetDataPointer();
  double*** phi    = (double***)Phi.GetDataPointer();
  Vec3D***  dxyz   = (Vec3D***)delta_xyz.GetDataPointer();
  Vec3D***  coords = (Vec3D***)coordinates.GetDataPointer();
  double*** res    = (double***)R.GetDataPointer();

  // fix first layer nodes?
  bool fix_first_layer = (iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::FIXED ||
                          iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::UNCONSTRAINED||
                          iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED1 ||
                          iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED2);

  // loop through the interior of the subdomain
  double max_residual = 0.0, dt, dx, local_res;
  double a,b,c,d,e,f, ap,am, bp,bm, cp,cm, dp,dm, ep,em, fp,fm;
  for(int k=k0; k<kmax; k++)
    for(int j=j0; j<jmax; j++)
      for(int i=i0; i<imax; i++) {
        
        if(fix_first_layer) {
          if(tag[k][j][i]!=0) {//fixed node (first layer)
            res[k][j][i] = 0.0;
            continue;
          }
        }

        dx = std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1], dxyz[k][j][i][2]));
        dt = cfl*dx;
        
        a = (phi[k][j][i]-phi[k][j][i-1])/(coords[k][j][i][0]-coords[k][j][i-1][0]);
        b = (phi[k][j][i+1]-phi[k][j][i])/(coords[k][j][i+1][0]-coords[k][j][i][0]);
        c = (phi[k][j][i]-phi[k][j-1][i])/(coords[k][j][i][1]-coords[k][j-1][i][1]);
        d = (phi[k][j+1][i]-phi[k][j][i])/(coords[k][j+1][i][1]-coords[k][j][i][1]);
        e = (phi[k][j][i]-phi[k-1][j][i])/(coords[k][j][i][2]-coords[k-1][j][i][2]);
        f = (phi[k+1][j][i]-phi[k][j][i])/(coords[k+1][j][i][2]-coords[k][j][i][2]);
        ap = std::max(a,0.0), am = std::min(a,0.0);
        bp = std::max(b,0.0), bm = std::min(b,0.0);
        cp = std::max(c,0.0), cm = std::min(c,0.0);
        dp = std::max(d,0.0), dm = std::min(d,0.0);
        ep = std::max(e,0.0), em = std::min(e,0.0);
        fp = std::max(f,0.0), fm = std::min(f,0.0);

        if(phi[k][j][i]>=0) {
          local_res = sqrt(std::max(ap*ap, bm*bm) + std::max(cp*cp, dm*dm) + std::max(ep*ep, fm*fm)) - 1.0;
        } else {
          local_res = sqrt(std::max(am*am, bp*bp) + std::max(cm*cm, dp*dp) + std::max(em*em, fp*fp)) - 1.0;
        }

        res[k][j][i] = -dt*sign[k][j][i]*local_res;

        max_residual = std::max(max_residual, fabs(local_res));
      }

  // get max residual over all the processors
  MPI_Allreduce(MPI_IN_PLACE, &max_residual, 1, MPI_DOUBLE, MPI_MAX, comm);

  // restore data
  Tag.RestoreDataPointerToLocalVector();
  Sign.RestoreDataPointerToLocalVector();
  Phi.RestoreDataPointerToLocalVector();
  delta_xyz.RestoreDataPointerToLocalVector();
  coordinates.RestoreDataPointerToLocalVector();

  R.RestoreDataPointerAndInsert();

  return max_residual;
}

//--------------------------------------------------------------------------

double
LevelSetReinitializer::ComputeResidualInBand(SpaceVariable3D &Phi, SpaceVariable3D &Useful,
                                             vector<Int3> &useful_nodes, SpaceVariable3D &R, double cfl)
{

  int NX, NY, NZ;
  Phi.GetGlobalSize(&NX, &NY, &NZ);

  // get data
  double*** tag    = (double***)Tag.GetDataPointer();
  double*** useful = (double***)Useful.GetDataPointer();
  double*** sign   = (double***)Sign.GetDataPointer();
  double*** phi    = (double***)Phi.GetDataPointer();
  Vec3D***  dxyz   = (Vec3D***)delta_xyz.GetDataPointer();
  Vec3D***  coords = (Vec3D***)coordinates.GetDataPointer();
  double*** res    = (double***)R.GetDataPointer();

  // fix first layer nodes?
  bool fix_first_layer = (iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::FIXED ||
                          iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::UNCONSTRAINED||
                          iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED1 ||
                          iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::CONSTRAINED2);

  // loop through the interior of the subdomain
  double max_residual = 0.0, dt, dx, local_res;
  double a,b,c,d,e,f, ap,am, bp,bm, cp,cm, dp,dm, ep,em, fp,fm;
  for(auto it = useful_nodes.begin(); it != useful_nodes.end(); it++) {
        
    int i((*it)[0]), j((*it)[1]), k((*it)[2]);

    if(!Phi.IsHere(i,j,k,false)) //only calculate residual within the subdomain interior
      continue;

    if(fix_first_layer) {
      if(tag[k][j][i]!=0) {//fixed node (first layer)
        res[k][j][i] = 0.0;
        continue;
      }
    }

    dx = std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1], dxyz[k][j][i][2]));
    dt = cfl*dx;

    a = useful[k][j][i-1] ? (phi[k][j][i]-phi[k][j][i-1])/(coords[k][j][i][0]-coords[k][j][i-1][0]) : 0.0;
    b = useful[k][j][i+1] ? (phi[k][j][i+1]-phi[k][j][i])/(coords[k][j][i+1][0]-coords[k][j][i][0]) : 0.0;
    c = useful[k][j-1][i] ? (phi[k][j][i]-phi[k][j-1][i])/(coords[k][j][i][1]-coords[k][j-1][i][1]) : 0.0;
    d = useful[k][j+1][i] ? (phi[k][j+1][i]-phi[k][j][i])/(coords[k][j+1][i][1]-coords[k][j][i][1]) : 0.0;
    e = useful[k-1][j][i] ? (phi[k][j][i]-phi[k-1][j][i])/(coords[k][j][i][2]-coords[k-1][j][i][2]) : 0.0;
    f = useful[k+1][j][i] ? (phi[k+1][j][i]-phi[k][j][i])/(coords[k+1][j][i][2]-coords[k][j][i][2]) : 0.0;

    ap = std::max(a,0.0), am = std::min(a,0.0);
    bp = std::max(b,0.0), bm = std::min(b,0.0);
    cp = std::max(c,0.0), cm = std::min(c,0.0);
    dp = std::max(d,0.0), dm = std::min(d,0.0);
    ep = std::max(e,0.0), em = std::min(e,0.0);
    fp = std::max(f,0.0), fm = std::min(f,0.0);

    if(phi[k][j][i]>=0) {
      local_res = sqrt(std::max(ap*ap, bm*bm) + std::max(cp*cp, dm*dm) + std::max(ep*ep, fm*fm)) - 1.0;
    } else {
      local_res = sqrt(std::max(am*am, bp*bp) + std::max(cm*cm, dp*dp) + std::max(em*em, fp*fp)) - 1.0;
    }

    res[k][j][i] = -dt*sign[k][j][i]*local_res;

    max_residual = std::max(max_residual, fabs(local_res));
  }

  // get max residual over all the processors
  MPI_Allreduce(MPI_IN_PLACE, &max_residual, 1, MPI_DOUBLE, MPI_MAX, comm);

  // restore data
  Tag.RestoreDataPointerToLocalVector();
  Sign.RestoreDataPointerToLocalVector();
  Phi.RestoreDataPointerToLocalVector();
  delta_xyz.RestoreDataPointerToLocalVector();
  coordinates.RestoreDataPointerToLocalVector();

  R.RestoreDataPointerAndInsert();

  return max_residual;
}

//--------------------------------------------------------------------------

// Apply boundary conditions by populating ghost cells of Phi
void
LevelSetReinitializer::ApplyBoundaryConditions(SpaceVariable3D &Phi, SpaceVariable3D *Useful)
{
  double*** phi = (double***) Phi.GetDataPointer();
  Vec3D*** coords = (Vec3D***)coordinates.GetDataPointer();

  double*** useful = Useful ? Useful->GetDataPointer() : NULL;


  int NX, NY, NZ;
  Phi.GetGlobalSize(&NX, &NY, &NZ);

  double r, r1, r2, f1, f2;

  for(auto it = ghost_nodes_outer.begin(); it != ghost_nodes_outer.end();  it++) {

    if(it->type_projection != GhostPoint::FACE)
      continue; //corner (i.e. edge or vertex) nodes are not populated

    int i(it->ijk[0]), j(it->ijk[1]), k(it->ijk[2]);

    if(useful && !useful[k][j][i])
      continue;

    int im_i(it->image_ijk[0]), im_j(it->image_ijk[1]), im_k(it->image_ijk[2]);

    switch (it->bcType) {

      case (int)LevelSetSchemeData::ZERO_NEUMANN :
        phi[k][j][i] = phi[im_k][im_j][im_i];
        break;

      case (int)LevelSetSchemeData::LINEAR_EXTRAPOLATION :
        //make sure the width of the subdomain is big enough for linear extrapolation
        if(it->side == GhostPoint::LEFT) {
          if(i+2<NX) {
            r  = coords[k][j][i][0];
            r1 = coords[k][j][i+1][0];  f1 = phi[k][j][i+1];
            r2 = coords[k][j][i+2][0];  f2 = phi[k][j][i+2];
            phi[k][j][i] = f1 + (f2-f1)/(r2-r1)*(r-r1);
          } else
            phi[k][j][i] = phi[im_k][im_j][im_i];
        }
        else if(it->side == GhostPoint::RIGHT) {
          if(i-2>=0) {
            r  = coords[k][j][i][0];
            r1 = coords[k][j][i-1][0];  f1 = phi[k][j][i-1];
            r2 = coords[k][j][i-2][0];  f2 = phi[k][j][i-2];
            phi[k][j][i] = f1 + (f2-f1)/(r2-r1)*(r-r1);
          } else
            phi[k][j][i] = phi[im_k][im_j][im_i];
        }
        else if(it->side == GhostPoint::BOTTOM) {
          if(j+2<NY) {
            r  = coords[k][j][i][1];
            r1 = coords[k][j+1][i][1];  f1 = phi[k][j+1][i];
            r2 = coords[k][j+2][i][1];  f2 = phi[k][j+2][i];
            phi[k][j][i] = f1 + (f2-f1)/(r2-r1)*(r-r1);
          } else
            phi[k][j][i] = phi[im_k][im_j][im_i];
        }
        else if(it->side == GhostPoint::TOP) {
          if(j-2>=0) {
            r  = coords[k][j][i][1];
            r1 = coords[k][j-1][i][1];  f1 = phi[k][j-1][i];
            r2 = coords[k][j-2][i][1];  f2 = phi[k][j-2][i];
            phi[k][j][i] = f1 + (f2-f1)/(r2-r1)*(r-r1);
          } else
            phi[k][j][i] = phi[im_k][im_j][im_i];
        }
        else if(it->side == GhostPoint::BACK) {
          if(k+2<NZ) { 
            r  = coords[k][j][i][2];
            r1 = coords[k+1][j][i][2];  f1 = phi[k+1][j][i];
            r2 = coords[k+2][j][i][2];  f2 = phi[k+2][j][i];
            phi[k][j][i] = f1 + (f2-f1)/(r2-r1)*(r-r1);
          } else
            phi[k][j][i] = phi[im_k][im_j][im_i];
        }
        else if(it->side == GhostPoint::FRONT) {
          if(k-2>=0) {
            r  = coords[k][j][i][2];
            r1 = coords[k-1][j][i][2];  f1 = phi[k-1][j][i];
            r2 = coords[k-2][j][i][2];  f2 = phi[k-2][j][i];
            phi[k][j][i] = f1 + (f2-f1)/(r2-r1)*(r-r1);
          } else
            phi[k][j][i] = phi[im_k][im_j][im_i];
        }
        break;
    }

  }

  Phi.RestoreDataPointerAndInsert();

  coordinates.RestoreDataPointerToLocalVector();
}
//--------------------------------------------------------------------------

void
LevelSetReinitializer::ApplyCorrectionToFirstLayerNodes(SpaceVariable3D &Phi, vector<FirstLayerNode> &firstLayer, 
                                                        double cfl)
{

  if(iod_ls.reinit.firstLayerTreatment != LevelSetReinitializationData::ITERATIVE_CONSTRAINED1 &&
     iod_ls.reinit.firstLayerTreatment != LevelSetReinitializationData::ITERATIVE_CONSTRAINED2)
    return; //nothing to do
  
  //Step 1: Calculate correction F 
  double*** phi = (double***) Phi.GetDataPointer();
  Vec3D*** dxyz = (Vec3D***) delta_xyz.GetDataPointer();

  if(iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::ITERATIVE_CONSTRAINED1) {//HCR-1
    for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {

      int i(it->i), j(it->j), k(it->k);

      it->f = 0.0;

      if(phi[k][j][i]*phi[k][j][i-1]>=0 || phi[k][j][i]*phi[k][j][i+1]>=0 ||
         phi[k][j][i]*phi[k][j-1][i]>=0 || phi[k][j][i]*phi[k][j+1][i]>=0 ||
         phi[k][j][i]*phi[k-1][j][i]>=0 || phi[k][j][i]*phi[k+1][j][i]>=0) 
        continue; //This node is in Gamma but not in C^{\nu}

      double sum = 0.0;
      if(it->s[0])  sum += it->r[0]*phi[k][j][i-1];
      if(it->s[1])  sum += it->r[1]*phi[k][j][i+1];
      if(it->s[2])  sum += it->r[2]*phi[k][j-1][i];
      if(it->s[3])  sum += it->r[3]*phi[k][j+1][i];
      if(it->s[4])  sum += it->r[4]*phi[k-1][j][i];
      if(it->s[5])  sum += it->r[5]*phi[k+1][j][i];

      if(it->ns!=0)
        it->f = sum/(double)(it->ns) - phi[k][j][i];
      else
        it->f = 0.0 - phi[k][j][i];

      it->f /= std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1], dxyz[k][j][i][2]));

    }
  }
  else if(iod_ls.reinit.firstLayerTreatment == LevelSetReinitializationData::ITERATIVE_CONSTRAINED2) {
    for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {

      int i(it->i), j(it->j), k(it->k);

      it->f = 0.0;

      if(phi[k][j][i]*phi[k][j][i-1]>=0 || phi[k][j][i]*phi[k][j][i+1]>=0 ||
         phi[k][j][i]*phi[k][j-1][i]>=0 || phi[k][j][i]*phi[k][j+1][i]>=0 ||
         phi[k][j][i]*phi[k-1][j][i]>=0 || phi[k][j][i]*phi[k+1][j][i]>=0) 
        continue; //This node is in Gamma but not in C^{\nu}

      double sum = 0.0;
      if(it->s[0])  sum += phi[k][j][i-1];
      if(it->s[1])  sum += phi[k][j][i+1];
      if(it->s[2])  sum += phi[k][j-1][i];
      if(it->s[3])  sum += phi[k][j+1][i];
      if(it->s[4])  sum += phi[k-1][j][i];
      if(it->s[5])  sum += phi[k+1][j][i];

      it->f = it->r0*sum - phi[k][j][i];
      it->f /= std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1], dxyz[k][j][i][2]));

    }
  }


  // Step 2: Add forcing term to phi
  double beta = 0.5;
  for(auto it = firstLayer.begin(); it != firstLayer.end(); it++) {
    int i(it->i), j(it->j), k(it->k);
    double dt = cfl*std::min(dxyz[k][j][i][0], std::min(dxyz[k][j][i][1], dxyz[k][j][i][2]));
    phi[k][j][i] += dt*beta*it->f;
  }

  // Restore
  Phi.RestoreDataPointerAndInsert();
  delta_xyz.RestoreDataPointerToLocalVector();

  // Step 3: Apply Boundary conditions
  ApplyBoundaryConditions(Phi);

}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::UpdatePhiMaxAndPhiMinInBand(SpaceVariable3D &Phi, vector<Int3> &useful_nodes)
{
  double*** phi = (double***)Phi.GetDataPointer();

  for(auto it = useful_nodes.begin(); it != useful_nodes.end(); it++) {
    int i((*it)[0]), j((*it)[1]), k((*it)[2]);
    phi_max = std::max(phi_max, phi[k][j][i]);
    phi_min = std::min(phi_min, phi[k][j][i]);
  }
  MPI_Allreduce(MPI_IN_PLACE, &phi_max, 1, MPI_DOUBLE, MPI_MAX, comm);
  MPI_Allreduce(MPI_IN_PLACE, &phi_min, 1, MPI_DOUBLE, MPI_MIN, comm);

  Phi.RestoreDataPointerToLocalVector();
}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::ConstructNarrowBand(SpaceVariable3D &Phi, 
                           SpaceVariable3D &Level, SpaceVariable3D &Useful, SpaceVariable3D &Active,
                           vector<Int3> &useful_nodes, vector<Int3> &active_nodes)
{
  //*****************************************************
  // Useful/useful_nodes ~ all nodes in the narrow band
  // Active/active_nodes ~ nodes in the interior of the narrow band
  // Phi ~ a large constant (+/-) number will be specified outside band
  //*****************************************************
  double*** phi = (double***)Phi.GetDataPointer();

  useful_nodes.clear();
  active_nodes.clear();

  // -------------------------------------------------- 
  // Step 1: find band level 0 (phi==0) and 1
  // -------------------------------------------------- 
  double*** level  = (double***)Level.GetDataPointer();
  double*** useful = (double***)Useful.GetDataPointer();
  double*** active = (double***)Active.GetDataPointer();
  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {

        // reset
        level[k][j][i] = INT_MAX;
        useful[k][j][i] = 0;
        active[k][j][i] = 0;

        if(Phi.OutsidePhysicalDomainAndUnpopulated(i,j,k))
          continue;

        if(phi[k][j][i]==0) {
          level[k][j][i] = 0;
          useful[k][j][i] = 1;
          active[k][j][i] = 1;
          useful_nodes.push_back(Int3(i,j,k));
          active_nodes.push_back(Int3(i,j,k));
        } 
        else if ( (i-1>=i0   && phi[k][j][i]*phi[k][j][i-1]<=0) ||
                  (i+1<iimax && phi[k][j][i]*phi[k][j][i+1]<=0) ||
                  (j-1>=j0   && phi[k][j][i]*phi[k][j-1][i]<=0) ||
                  (j+1<jjmax && phi[k][j][i]*phi[k][j+1][i]<=0) ||
                  (k-1>=k0   && phi[k][j][i]*phi[k-1][j][i]<=0) ||
                  (k+1<kkmax && phi[k][j][i]*phi[k+1][j][i]<=0) ) {


          level[k][j][i] = 1;
          useful[k][j][i] = 1;
          active[k][j][i] = 1;
          useful_nodes.push_back(Int3(i,j,k));
          active_nodes.push_back(Int3(i,j,k));

        } 

      }
  Level.RestoreDataPointerAndInsert();

  // Update useful_nodes and active_nodes to get the changes at boundary
  level = (double***)Level.GetDataPointer();
  for(auto it = ghost_nodes_inner.begin(); it != ghost_nodes_inner.end(); it++) {
    int i(it->ijk[0]), j(it->ijk[1]), k(it->ijk[2]);
    if(level[k][j][i]<INT_MAX && useful[k][j][i]==0) {
      useful[k][j][i] = 1;
      active[k][j][i] = 1;
      useful_nodes.push_back(it->ijk);
      active_nodes.push_back(it->ijk);
    }
  } 
  for(auto it = ghost_nodes_outer.begin(); it != ghost_nodes_outer.end(); it++) {
    int i(it->ijk[0]), j(it->ijk[1]), k(it->ijk[2]);
    //no need to skip corner nodes --- they won't satisfy the if statement anyway
    if(level[k][j][i]<INT_MAX && useful[k][j][i]==0) {
      useful[k][j][i] = 1;
      active[k][j][i] = 1;
      useful_nodes.push_back(it->ijk);
      active_nodes.push_back(it->ijk);
    }
  }
  Level.RestoreDataPointerToLocalVector();

  // Restore variables
  Active.RestoreDataPointerToLocalVector(); //should have already been updated w/ neighbors
  Useful.RestoreDataPointerToLocalVector(); //should have already been updated w/ neighbors

  Phi.RestoreDataPointerToLocalVector(); //no exchange needed


  // -------------------------------------------------- 
  // Step 2: find band level 2, 3, ..., bandwidth
  //         IMPORTANT: To be called after level 0 and 1 completed
  // -------------------------------------------------- 
  PropagateNarrowBand(Level, Useful, Active, useful_nodes, active_nodes);

  // -------------------------------------------------- 
  // Step 3: Cutoff Phi outside the band
  // -------------------------------------------------- 
  CutOffPhiOutsideBand(Phi, Useful, useful_nodes);

}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::PropagateNarrowBand(SpaceVariable3D &Level, SpaceVariable3D &Useful, 
                                           SpaceVariable3D &Active, vector<Int3> &useful_nodes, 
                                           vector<Int3> &active_nodes)
{ 
  // -------------------------------------------------- 
  // Find band level 2, 3, ..., bandwidth
  //  IMPORTANT: To be called after level 0 and 1 completed
  // -------------------------------------------------- 
  
  double*** useful = (double***)Useful.GetDataPointer();
  double*** active = (double***)Active.GetDataPointer();
 
  int bandwidth = iod_ls.bandwidth;
  double*** level;

  for(int band = 2; band <= bandwidth; band++) {
    
    level = (double***)Level.GetDataPointer();

    int size = useful_nodes.size();
    for(int n = 0; n < size; n++) {

      int i(useful_nodes[n][0]), j(useful_nodes[n][1]), k(useful_nodes[n][2]);

      //check its neighbors
      if(i-1>=ii0 && !Useful.OutsidePhysicalDomainAndUnpopulated(i-1,j,k)) { //left
        if(level[k][j][i-1]==INT_MAX) {
          level[k][j][i-1] = band;
          useful[k][j][i-1] = 1;
          useful_nodes.push_back(Int3(i-1,j,k));
          if(band<bandwidth) {
            active[k][j][i-1] = 1;
            active_nodes.push_back(Int3(i-1,j,k));
          } 
        }
      }

      if(i+1<iimax && !Useful.OutsidePhysicalDomainAndUnpopulated(i+1,j,k)) { //right
        if(level[k][j][i+1]==INT_MAX) {
          level[k][j][i+1] = band;
          useful[k][j][i+1] = 1;
          useful_nodes.push_back(Int3(i+1,j,k));
          if(band<bandwidth) {
            active[k][j][i+1] = 1;
            active_nodes.push_back(Int3(i+1,j,k));
          } 
        }
      }

      if(j-1>=jj0 && !Useful.OutsidePhysicalDomainAndUnpopulated(i,j-1,k)) { //bottom
        if(level[k][j-1][i]==INT_MAX) {
          level[k][j-1][i] = band;
          useful[k][j-1][i] = 1;
          useful_nodes.push_back(Int3(i,j-1,k));
          if(band<bandwidth) {
            active[k][j-1][i] = 1;
            active_nodes.push_back(Int3(i,j-1,k));
          } 
        }
      }

      if(j+1<jjmax && !Useful.OutsidePhysicalDomainAndUnpopulated(i,j+1,k)) { //top
        if(level[k][j+1][i]==INT_MAX) {
          level[k][j+1][i] = band;
          useful[k][j+1][i] = 1;
          useful_nodes.push_back(Int3(i,j+1,k));
          if(band<bandwidth) {
            active[k][j+1][i] = 1;
            active_nodes.push_back(Int3(i,j+1,k));
          } 
        }
      }

      if(k-1>=kk0 && !Useful.OutsidePhysicalDomainAndUnpopulated(i,j,k-1)) { //back
        if(level[k-1][j][i]==INT_MAX) {
          level[k-1][j][i] = band;
          useful[k-1][j][i] = 1;
          useful_nodes.push_back(Int3(i,j,k-1));
          if(band<bandwidth) {
            active[k-1][j][i] = 1;
            active_nodes.push_back(Int3(i,j,k-1));
          } 
        }
      }

      if(k+1<kkmax && !Useful.OutsidePhysicalDomainAndUnpopulated(i,j,k+1)) { //front
        if(level[k+1][j][i]==INT_MAX) {
          level[k+1][j][i] = band;
          useful[k+1][j][i] = 1;
          useful_nodes.push_back(Int3(i,j,k+1));
          if(band<bandwidth) {
            active[k+1][j][i] = 1;
            active_nodes.push_back(Int3(i,j,k+1));
          } 
        }
      }
    }

    Level.RestoreDataPointerAndInsert();

    // Update useful_nodes and active_nodes to get the changes at boundary
    level = (double***)Level.GetDataPointer();
    for(auto it = ghost_nodes_inner.begin(); it != ghost_nodes_inner.end(); it++) {
      int i(it->ijk[0]), j(it->ijk[1]), k(it->ijk[2]);
      if(level[k][j][i]<INT_MAX && useful[k][j][i]==0) {
        useful[k][j][i] = 1;
        useful_nodes.push_back(it->ijk);
        if(band<bandwidth) {
          active[k][j][i] = 1;
          active_nodes.push_back(it->ijk);
        }
      }
    } 
    for(auto it = ghost_nodes_outer.begin(); it != ghost_nodes_outer.end(); it++) {
      int i(it->ijk[0]), j(it->ijk[1]), k(it->ijk[2]);
      if(level[k][j][i]<INT_MAX && useful[k][j][i]==0) {
        useful[k][j][i] = 1;
        useful_nodes.push_back(it->ijk);
        if(band<bandwidth) {
          active[k][j][i] = 1;
          active_nodes.push_back(it->ijk);
        }
      }
    }
    Level.RestoreDataPointerToLocalVector();

  }
  // Restore variables
  Active.RestoreDataPointerToLocalVector(); //should have already been updated w/ neighbors
  Useful.RestoreDataPointerToLocalVector(); //should have already been updated w/ neighbors

}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::CutOffPhiOutsideBand(SpaceVariable3D &Phi, SpaceVariable3D &Useful,
                                            vector<Int3> &useful_nodes)
{
  // -------------------------------------------------- 
  // Cutoff Phi outside the band
  // -------------------------------------------------- 
  double*** phi = (double***)Phi.GetDataPointer();
  double*** useful = (double***)Useful.GetDataPointer();

  // calculate phi_max and phi_min
  phi_max = -DBL_MAX;
  phi_min = DBL_MAX;
  for (auto it = useful_nodes.begin(); it != useful_nodes.end(); it++) {
    int i((*it)[0]), j((*it)[1]), k((*it)[2]);
    phi_max = std::max(phi[k][j][i], phi_max);
    phi_min = std::min(phi[k][j][i], phi_min);
  }
  MPI_Allreduce(MPI_IN_PLACE, &phi_max, 1, MPI_DOUBLE, MPI_MAX, comm);
  MPI_Allreduce(MPI_IN_PLACE, &phi_min, 1, MPI_DOUBLE, MPI_MIN, comm);

  phi_out_pos = phi_max*10.0;
  phi_out_neg = phi_min*10.0;

  for(int k=kk0; k<kkmax; k++)
    for(int j=jj0; j<jjmax; j++)
      for(int i=ii0; i<iimax; i++) {
        if(!useful[k][j][i])
          phi[k][j][i] = phi[k][j][i]>=0 ? phi_out_pos : phi_out_neg;
      }

  // Restore variables
  Useful.RestoreDataPointerToLocalVector(); //no changes made

  Phi.RestoreDataPointerAndInsert(); 

}

//--------------------------------------------------------------------------

void
LevelSetReinitializer::UpdateNarrowBand(SpaceVariable3D &Phi, vector<Int3> &firstLayerIncGhost,
                           SpaceVariable3D &Level, SpaceVariable3D &Useful, SpaceVariable3D &Active,
                           vector<Int3> &useful_nodes, vector<Int3> &active_nodes)
{
  //*******************************************************
  // Assuming firstLayerIncGhost (level = 0, 1) is already computed
  //*******************************************************
  double*** phi    = (double***)Phi.GetDataPointer();
  double*** level  = (double***)Level.GetDataPointer();
  double*** useful = (double***)Useful.GetDataPointer();
  double*** active = (double***)Active.GetDataPointer();
 
  //--------------------------------------------------
  // Step 1: Clean up old data
  //--------------------------------------------------
  vector<Int3> useful_nodes_backup = useful_nodes;
  for(auto it = useful_nodes.begin(); it != useful_nodes.end(); it++) {
    int i((*it)[0]), j((*it)[1]), k((*it)[2]);
    level[k][j][i] = INT_MAX;
    useful[k][j][i] = 0;
    active[k][j][i] = 0;
  }
  useful_nodes.clear();
  active_nodes.clear();

  //--------------------------------------------------
  // Step 2: Update level 0 and 1 based on "firstLayerIncGhost"
  // Note: firstLayerIncGhost should also contain ghosts 
  //       outside the physical domain, but not the corner
  //       nodes (see the function that constructs it).
  //--------------------------------------------------
  for(auto it = firstLayerIncGhost.begin(); it != firstLayerIncGhost.end(); it++) {

    int i((*it)[0]), j((*it)[1]), k((*it)[2]);
    useful[k][j][i] = 1;
    useful_nodes.push_back(Int3(i,j,k));
    active[k][j][i] = 1;
    active_nodes.push_back(Int3(i,j,k));
    level[k][j][i] = (phi[k][j][i]==0) ? 0 : 1;

  }

  Useful.RestoreDataPointerToLocalVector(); 
  Active.RestoreDataPointerToLocalVector();
  Level.RestoreDataPointerToLocalVector();
 
  // -------------------------------------------------- 
  // Step 3: find band level 2, 3, ..., bandwidth
  //         IMPORTANT: To be called after level 0 and 1 completed
  // -------------------------------------------------- 
  PropagateNarrowBand(Level, Useful, Active, useful_nodes, active_nodes);

  // -------------------------------------------------- 
  // Step 4: Cutoff Phi and Residual outside the band (will not call
  //         the CutOffPhiOutsideBand function, which
  //         goes over the entire domain)
  // -------------------------------------------------- 
  useful = (double***)Useful.GetDataPointer();
  double*** res = (double***)R.GetDataPointer();
  for(auto it = useful_nodes_backup.begin(); it != useful_nodes_backup.end(); it++) {
    int i((*it)[0]), j((*it)[1]), k((*it)[2]);
    if(!useful[k][j][i]) {
      phi[k][j][i] = (phi[k][j][i]>=0) ? phi_out_pos : phi_out_neg;
      res[k][j][i] = 0.0;
    }
  }

  // -------------------------------------------------- 
  // Step 5: For nodes that just become useful, set their
  //         phi to be phi_max or phi_min. (Otherwise they
  //         carry a garbage phi that can slow down
  //         convergence.)
  // -------------------------------------------------- 
  for(auto it = useful_nodes.begin(); it != useful_nodes.end(); it++) {
    int i((*it)[0]), j((*it)[1]), k((*it)[2]);
    if(phi[k][j][i] > 0.9*phi_out_pos)
      phi[k][j][i] = phi_max*1.1;
    else if(phi[k][j][i] < 0.9*phi_out_neg)
      phi[k][j][i] = phi_min*1.1;
  }


  Useful.RestoreDataPointerToLocalVector(); 
  Phi.RestoreDataPointerToLocalVector();
  R.RestoreDataPointerToLocalVector();

}

