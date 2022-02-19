#ifndef _SPECIAL_TOOLS_DRIVER_H_
#define _SPECIAL_TOOLS_DRIVER_H_

#include<ConcurrentProgramsHandler.h>

/*****************************************
 * class SpecialToolsDriver is the driver
 * that runs special tools implemented
 * in the M2C code.
 ****************************************/

class SpecialToolsDriver 
{

  MPI_Comm& comm;
  IoData& iod;
  ConcurrentProgramsHandler& concurrent;

public:
  
  SpecialToolsDriver(IoData &iod_, MPI_Comm &comm_, ConcurrentProgramsHandler &concurrent_);
  ~SpecialToolsDriver();

  void Run();
 
};

#endif
