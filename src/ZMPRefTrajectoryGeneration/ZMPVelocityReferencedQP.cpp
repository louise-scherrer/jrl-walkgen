/*
 * Copyright 2010,
 *
 * Andrei Herdt
 * Francois Keith
 * Olivier Stasse
 *
 * JRL, CNRS/AIST
 *
 * This file is part of walkGenJrl.
 * walkGenJrl is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * walkGenJrl is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Lesser Public License for more details.
 * You should have received a copy of the GNU Lesser General Public License
 * along with walkGenJrl.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Research carried out within the scope of the
 *  Joint Japanese-French Robotics Laboratory (JRL)
 */

/*! This object generate all the values for the foot trajectories,
  and the desired ZMP based on a sequence of steps following a QP
  formulation and a new QP solver as proposed by Herdt Advanced Robotics 2010.

  Andrei Herdt,
  Olivier Stasse,
*/

#include "portability/gettimeofday.hh"

#ifdef WIN32
# include <Windows.h>
#endif /* WIN32 */

#include <time.h>

#include <iostream>
#include <fstream>

#include <ZMPRefTrajectoryGeneration/ZMPVelocityReferencedQP.h>

#include <Debug.h>
using namespace std;
using namespace PatternGeneratorJRL;



ZMPVelocityReferencedQP::ZMPVelocityReferencedQP(SimplePluginManager *lSPM,
						 string DataFile,
						 CjrlHumanoidDynamicRobot *aHS) :
  ZMPRefTrajectoryGeneration(lSPM)
{

  m_Pu = 0;
  m_FullDebug = 1;
  m_FastFormulationMode = QLD;

  m_QP_T = 0.1;
  m_QP_N = 16;

  m_SamplingPeriod = 0.005;

  m_ComHeight = 0.814;

  //Gains
  m_Alpha = 0.00001;//Jerk
  m_Beta = 1.0; //Velocity
  m_Gamma = 0.000001; //ZMP

  /*! Getting the ZMP reference from Kajita's heuristic. */
  m_ZMPD = new ZMPDiscretization(lSPM,DataFile,aHS);

//  /*! For simulating the linearized inverted pendulum in 2D. */
//  m_CoM = new LinearizedInvertedPendulum2D();


  /*! For computing the stability constraints from the feet positions. */
  m_ConstraintOnX = 0.04;
  m_ConstraintOnY = 0.04;
  m_fCALS = new FootConstraintsAsLinearSystemForVelRef(lSPM,aHS,m_ConstraintOnX,m_ConstraintOnY);

  //m_StartTime = 0.0;
  m_UpperTimeLimitToUpdate = 0.0;
  m_TimeBuffer = 0.040;

  m_FTGS = new FootTrajectoryGenerationStandard(lSPM,aHS->leftFoot());
  m_FTGS->InitializeInternalDataStructures();

  //TODO: Change the name of the class
  /* Initialize the FSM */
  m_SupportFSM = new SupportFSM(m_QP_T);

  //TODO: The initialization has to be exported.
  //Initialize the support state
  m_CurrentSupport.Phase = 0;
  m_CurrentSupport.Foot = 1;
  m_CurrentSupport.TimeLimit = 1000000000;
  m_CurrentSupport.StepsLeft = 1;
  m_CurrentSupport.SSSS = false;
  m_CurrentSupport.StateChanged = false;


  /* Orientations preview algorithm*/
  m_OP = new OrientationsPreview(m_QP_T, m_QP_N, m_SupportFSM->m_SSPeriod, aHS->rootJoint());

  m_RobotMass = aHS->mass();
  m_TrunkState.yaw[0]=m_TrunkState.yaw[1]=m_TrunkState.yaw[2]=0.0;

  InitConstants();

  m_PLDPSolverHerdt = 0;

  /* Initialize  the 2D LIPM */
  m_CoM.SetSimulationControlPeriod(m_QP_T);
  m_CoM.SetRobotControlPeriod(m_SamplingPeriod);
  m_CoM.SetComHeight(m_ComHeight);
  m_CoM.InitializeSystem();

  initFeet();

  // Register method to handle
  string aMethodName[2] =
    {":previewcontroltime",
     ":numberstepsbeforestop"};

  for(int i=0;i<2;i++)
    {
      if (!RegisterMethod(aMethodName[i]))
	{
	  std::cerr << "Unable to register " << aMethodName << std::endl;
	}
    }
  
  // Debug point.
  debugConstructor();

  //Feet distance in the DS phase
  m_FeetDistanceDS = 0.2;

  m_PerturbationOccured = false;


  m_GenVR = new GeneratorVelRef(lSPM, DataFile, aHS);

  m_GenVR->setNbPrwSamplings(16);
  m_GenVR->setSamplingPeriodPreview(0.1);

  m_GenVR->setNbVariables(32);

  //m_Pb.setNbVariables(m_GenVR->getNbVariables());

  m_GenVR->initialize(m_Matrices);
  m_GenVR->setPonderation(m_Matrices, m_Beta, IntermedQPMat::INSTANT_VELOCITY);
  m_GenVR->setPonderation(m_Matrices, m_Gamma, IntermedQPMat::COP_CENTERING);
  m_GenVR->setPonderation(m_Matrices, m_Alpha, IntermedQPMat::JERK);

  m_Matrices.dumpObjective("/tmp/InstantVelocity.dat", IntermedQPMat::INSTANT_VELOCITY);
  m_Matrices.dumpObjective("/tmp/COPCentering.dat", IntermedQPMat::COP_CENTERING);
  m_Matrices.dumpObjective("/tmp/Jerk.dat", IntermedQPMat::JERK);

//  m_Pb.setDimensions(2*m_QP_N,
//                     0,
//                     0);
//
//  m_GenVR->buildInvariantPart(m_Pb, m_Matrices);
//
//  m_Pb.dumpProblem("/tmp/PB.dat");
}

ZMPVelocityReferencedQP::~ZMPVelocityReferencedQP()
{

  if (m_GenVR!=0)
    delete m_GenVR;

  if (m_ZMPD!=0)
    delete m_ZMPD;

  if (m_SupportFSM!=0)
    delete m_SupportFSM;

  if (m_fCALS!=0)
    delete m_fCALS;

  if (m_FTGS!=0)
    delete m_FTGS;

  if (m_OP!=0)
    delete m_OP;

  if (m_PLDPSolverHerdt!=0)
    delete m_PLDPSolverHerdt;

  if (m_Pu!=0)
    delete [] m_Pu ;

}


void ZMPVelocityReferencedQP::setVelReference(istringstream &strm)
{
  strm >> m_VelRef.local.x;
  strm >> m_VelRef.local.y;
  strm >> m_VelRef.local.yaw;
}

void ZMPVelocityReferencedQP::setVelReference(double dx,
					      double dy,
					      double dyaw)
{
  m_VelRef.local.x = dx;
  m_VelRef.local.y = dy;
  m_VelRef.local.yaw = dyaw;
}

void ZMPVelocityReferencedQP::setCoMPerturbationForce(istringstream &strm)
{
  MAL_VECTOR_RESIZE(m_PerturbationAcceleration,6);

  strm >> m_PerturbationAcceleration(2);
  strm >> m_PerturbationAcceleration(5);
  m_PerturbationAcceleration(2) = m_PerturbationAcceleration(2)/m_RobotMass;
  m_PerturbationAcceleration(5) = m_PerturbationAcceleration(5)/m_RobotMass;
  m_PerturbationOccured = true;
}

void ZMPVelocityReferencedQP::setCoMPerturbationForce(double x,double y)
{
  MAL_VECTOR_RESIZE(m_PerturbationAcceleration,6);

  m_PerturbationAcceleration(2) = x/m_RobotMass;
  m_PerturbationAcceleration(5) = y/m_RobotMass;
  m_PerturbationOccured = true;

}

void ZMPVelocityReferencedQP::interpolateFeet(deque<FootAbsolutePosition> &,
					      deque<FootAbsolutePosition> &)
{

  printf("To be implemented \n");
}

int ZMPVelocityReferencedQP::InitializeMatrixPbConstants()
{
  MAL_MATRIX_RESIZE(m_PPu,2*m_QP_N,2*m_QP_N);
  MAL_MATRIX_RESIZE(m_PZu,m_QP_N,m_QP_N);
  MAL_MATRIX_RESIZE(m_VPu,2*m_QP_N,2*m_QP_N);
  MAL_MATRIX_RESIZE(m_PPx,2*m_QP_N,6);
  MAL_MATRIX_RESIZE(m_PZx,m_QP_N,3);
  MAL_MATRIX_RESIZE(m_VPx,2*m_QP_N,6);

  for( int i=0;i<m_QP_N;i++)
    {
      // Compute VPx and PPx
      m_VPx(i,0)   = 0.0;   m_VPx(i,1) =     1.0; m_VPx(i,2)   = (i+1)*m_QP_T;
      m_VPx(i,3)   = 0.0;   m_VPx(i,4) =     0.0; m_VPx(i,5)   = 0.0;
      m_VPx(i+m_QP_N,0) = 0.0;   m_VPx(i+m_QP_N,1) =   0.0; m_VPx(i+m_QP_N,2) = 0.0;
      m_VPx(i+m_QP_N,3) = 0.0;   m_VPx(i+m_QP_N,4) =   1.0; m_VPx(i+m_QP_N,5) = (i+1)*m_QP_T;

      m_PPx(i,0) = 1.0; m_PPx(i,1)     = (i+1)*m_QP_T; m_PPx(i,2) = (i+1)*(i+1)*m_QP_T*m_QP_T*0.5;
      m_PPx(i,3) = 0.0; m_PPx(i,4)     =       0; m_PPx(i,5) = 0.;
      m_PPx(i+m_QP_N,0) = 0.0; m_PPx(i+m_QP_N,1) =     0.0; m_PPx(i+m_QP_N,2) = 0.0;
      m_PPx(i+m_QP_N,3) = 1.0; m_PPx(i+m_QP_N,4) = (i+1)*m_QP_T; m_PPx(i+m_QP_N,5) = (i+1)*(i+1)*m_QP_T*m_QP_T*0.5;

      //TODO: + or - m_ComHeight/9.81
      m_PZx(i,0) = 1.0; m_PZx(i,1)     = (i+1)*m_QP_T; m_PZx(i,2) = (i+1)*(i+1)*m_QP_T*m_QP_T*0.5-m_ComHeight/9.81;
      //m_PZx(i,3) = 0.0; m_PZx(i,4)     =       0; m_PZx(i,5) = 0.;
      //		m_PZx(i+m_QP_N,0) = 0.0; m_PZx(i+m_QP_N,1) =     0.0; m_PZx(i+m_QP_N,2) = 0.0;
      //		m_PZx(i+m_QP_N,3) = 1.0; m_PZx(i+m_QP_N,4) = (i+1)*m_QP_T; m_PZx(i+m_QP_N,5) = (i+1)*(i+1)*m_QP_T*m_QP_T*0.5+m_ComHeight/9.81;


      for( int j=0;j<m_QP_N;j++)
	{
	  m_PPu(i,j)=0;
	  m_PZu(i,j)=0;

	  if (j<=i)
	    {

	      m_VPu(i,j)= (2*(i-j)+1)*m_QP_T*m_QP_T*0.5 ;
	      m_VPu(i+m_QP_N,j+m_QP_N)= (2*(i-j)+1)*m_QP_T*m_QP_T*0.5 ;
	      m_VPu(i,j+m_QP_N)=0.0;
	      m_VPu(i+m_QP_N,j)=0.0;


	      m_PPu(i,j)= (1 + 3*(i-j) + 3*(i-j)*(i-j)) * m_QP_T*m_QP_T*m_QP_T/6.0;
	      m_PPu(i+m_QP_N,j+m_QP_N)= (1 + 3*(i-j) + 3*(i-j)*(i-j)) * m_QP_T*m_QP_T*m_QP_T/6.0;
	      m_PPu(i,j+m_QP_N)=0.0;
	      m_PPu(i+m_QP_N,j)=0.0;

	      m_PZu(i,j)= (1 + 3*(i-j) + 3*(i-j)*(i-j)) * m_QP_T*m_QP_T*m_QP_T/6.0 - m_QP_T*m_ComHeight/9.81;
	      //m_PZu(i+m_QP_N,j+m_QP_N)= (1 + 3*(i-j) + 3*(i-j)*(i-j)) * m_QP_T*m_QP_T*m_QP_T/6.0 + m_QP_T*m_ComHeight/9.81;
	      //m_PZu(i,j+m_QP_N)=0.0;
	      //m_PZu(i+m_QP_N,j)=0.0;
	    }
	  else
	    {

	      m_VPu(i,j) = 0.0;
	      m_VPu(i+m_QP_N,j+m_QP_N)=0.0;
	      m_VPu(i,j+m_QP_N)=0.0;
	      m_VPu(i+m_QP_N,j)=0.0;

	      m_PPu(i,j) = 0.0;
	      m_PPu(i+m_QP_N,j+m_QP_N)=0.0;
	      m_PPu(i,j+m_QP_N)=0.0;
	      m_PPu(i+m_QP_N,j)=0.0;

	      m_PZu(i,j) = 0.0;
	      //m_PZu(i+m_QP_N,j+m_QP_N)=0.0;
	      //m_PZu(i,j+m_QP_N)=0.0;
	      //m_PZu(i+m_QP_N,j)=0.0;
	    }

	}
    }

  // Build m_Px.
  MAL_MATRIX_RESIZE(m_Px,m_QP_N,3);

  for( int li=0;li<m_QP_N;li++)
    {
      m_Px(li,0) = 1.0;
      m_Px(li,1) = (double)(1.0+li)*m_QP_T;
      m_Px(li,2) = (li+1.0)*(li+1.0)*m_QP_T*m_QP_T*0.5-m_ComHeight/9.81;
    }
  if (m_FullDebug>2)
    {
      ofstream aof;
      aof.open("/tmp/VPx.dat");
      aof << m_VPx;
      aof.close();

      aof.open("/tmp/m_PPx.dat");
      aof << m_PPx;
      aof.close();

      aof.open("/tmp/VPu.dat");
      aof << m_VPu;
      aof.close();

      aof.open("/tmp/PPu.dat");
      aof << m_PPu;
      aof.close();

      aof.open("/tmp/PZu.dat");
      aof << m_PZu;
      aof.close();
    }

  return 0;
}


int ZMPVelocityReferencedQP::BuildingConstantPartOfConstraintMatrices()
{
  if (m_Pu==0)
    m_Pu = new double[m_QP_N*m_QP_N];

  double * lInterPu=0;
  double * ptPu=0;

  if ((m_FastFormulationMode==QLDANDLQ)||
      (m_FastFormulationMode==PLDP))
    {
      lInterPu = new double[m_QP_N*m_QP_N];
      memset(lInterPu,0,m_QP_N*m_QP_N*sizeof(double));
      ptPu = lInterPu;
    }
  else
    ptPu = m_Pu;

  memset(m_Pu,0,m_QP_N*m_QP_N*sizeof(double));

  // Recursive multiplication of the system is applied.
  // we keep the transpose form, i.e. Pu'.
  for(int i=0;i<m_QP_N;i++)
    {

      for(int k=0;k<=i;k++)
	{
	  ptPu[k*m_QP_N+i] =
	    ((1+3*(i-k)+3*(i-k)*(i-k))*m_QP_T*m_QP_T*m_QP_T/6.0 - m_QP_T * m_ComHeight/9.81);
	}
    }

  // Consider QLDANDLQ formulation.
  if ((m_FastFormulationMode==QLDANDLQ) ||
      (m_FastFormulationMode==PLDPHerdt))
    {
      // Premultiplication by LQ-1
      // Indeed we have to provide qld transpose matrix,
      // therefore instead of providing D*Pu*iLQ (IROS 2008 p 28)
      // we provide its transpose:
      // (D*Pu*iLQ')' = iLQ*Pu'*D'
      // So here we compute iLQ*Pu'
      // Be careful with the two stages resolution.
      for(int i=0;i<m_QP_N;i++)
	{
	  for(int j=0;j<m_QP_N;j++)
	    {
	      m_Pu[i*m_QP_N+j] = 0;
	      for(int k=0;k<m_QP_N;k++)
		{
		  m_Pu[i*m_QP_N+j] += m_iLQ(i,k) * ptPu[k*m_QP_N+j];
		}
	    }
	}

      if (m_FastFormulationMode==PLDPHerdt)
	{
	  MAL_MATRIX_DIM(m_mal_Pu,double,m_QP_N,m_QP_N);
	  for(int j=0;j<m_QP_N;j++)
	    for(int k=0;k<m_QP_N;k++)
	      m_mal_Pu(j,k) = m_Pu[j*m_QP_N+k];
	  MAL_INVERSE(m_mal_Pu, m_iPu, double);
	}
    }

  if (m_FullDebug>0)
    {
      ofstream aof;
      char Buffer[1024];
      sprintf(Buffer,"/tmp/PuCst.dat");
      aof.open(Buffer,ofstream::out);
      for( int i=0;i<m_QP_N;i++)
	{
	  for( int j=0;j<m_QP_N;j++)
	    aof << m_Pu[j+i*m_QP_N] << " " ;
	  aof << endl;
	}
      aof.close();

      sprintf(Buffer,"/tmp/tmpPuCst.dat");
      aof.open(Buffer,ofstream::out);
      for( int i=0;i<m_QP_N;i++)
	{
	  for( int j=0;j<m_QP_N;j++)
	    aof << ptPu[j+i*m_QP_N] << " " ;
	  aof << endl;
	}
      aof.close();

      if ((m_FastFormulationMode==QLDANDLQ) ||
	  (m_FastFormulationMode==PLDP))
	{
	  sprintf(Buffer,"/tmp/tmpiLQ.dat");
	  aof.open(Buffer,ofstream::out);
	  for( int i=0;i<m_QP_N;i++)
	    {
	      for( int j=0;j<m_QP_N;j++)
		aof << m_iLQ(i,j) << " " ;
	      aof << endl;
	    }
	  aof.close();
	}

    }

  delete [] lInterPu;
  return 0;
}


int ZMPVelocityReferencedQP::buildConstraintMatricesPLDPHerdt()
{
  m_Pu = new double[2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)];

  double * lInterPu=0;
  double * ptPu=0;

  if ((m_FastFormulationMode==QLDANDLQ)||
      (m_FastFormulationMode==PLDPHerdt))
    {
      lInterPu = new double[2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)];
      memset(lInterPu,0,2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)*sizeof(double));
      ptPu = lInterPu;
    }
  else
    ptPu = m_Pu;


  memset(m_Pu,0,2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)*sizeof(double));


  // Recursive multiplication of the system is applied.
  // we keep the transpose form, i.e. Pu'.
  for(int i=0;i<m_QP_N;i++)
    {
      for(int k=0;k<=i;k++)
	{
	  ptPu[k*2*(m_QP_N+m_PrwSupport.StepNumber)+i] =
	    ((1+3*(i-k)+3*(i-k)*(i-k))*m_QP_T*m_QP_T*m_QP_T/6.0 - m_QP_T * m_ComHeight/9.81);
	  ptPu[(k+m_QP_N)*2*(m_QP_N+m_PrwSupport.StepNumber)+m_QP_N+i] =
	    ((1+3*(i-k)+3*(i-k)*(i-k))*m_QP_T*m_QP_T*m_QP_T/6.0 - m_QP_T * m_ComHeight/9.81);
	}
    }
  for(int i=0;i<m_PrwSupport.StepNumber;i++)
    {
      ptPu[(2*m_QP_N+i)*2*(m_QP_N+m_PrwSupport.StepNumber)+2*m_QP_N+i] = 1.0;
      ptPu[(2*m_QP_N+m_PrwSupport.StepNumber+i)*2*(m_QP_N+m_PrwSupport.StepNumber)+2*m_QP_N+m_PrwSupport.StepNumber+i] = 1.0;
    }

  if ((m_FastFormulationMode==QLDANDLQ) ||
      (m_FastFormulationMode==PLDPHerdt))
    {
      // Premultiplication by LQ-1
      // Indeed we have to provide qld transpose matrix,
      // therefore instead of providing D*Pu*iLQ (IROS 2008 p 28)
      // we provide its transpose:
      // (D*Pu*iLQ')' = iLQ*Pu'*D'
      // So here we compute iLQ*Pu'
      // Be careful with the two stages resolution.
      for(int i=0;i<(2*m_QP_N+2*m_PrwSupport.StepNumber);i++)
	{
	  for(int j=0;j<(2*m_QP_N+2*m_PrwSupport.StepNumber);j++)
	    {
	      m_Pu[i*2*(m_QP_N+m_PrwSupport.StepNumber)+j] = 0;
	      for(int k=0;k<2*(m_QP_N+m_PrwSupport.StepNumber);k++)
		{
		  m_Pu[i*2*(m_QP_N+m_PrwSupport.StepNumber)+j] += m_iLQ(i,k) * ptPu[k*2*(m_QP_N+m_PrwSupport.StepNumber)+j];
		}
	    }
	}

      if (m_FastFormulationMode==PLDPHerdt)
	{
	  MAL_MATRIX_DIM(m_mal_Pu,double,2*(m_QP_N+m_PrwSupport.StepNumber),2*(m_QP_N+m_PrwSupport.StepNumber));
	  for(int j=0;j<2*(m_QP_N+m_PrwSupport.StepNumber);j++)
	    for(int k=0;k<2*(m_QP_N+m_PrwSupport.StepNumber);k++)
	      m_mal_Pu(j,k) = m_Pu[j*2*(m_QP_N+m_PrwSupport.StepNumber)+k];
	  MAL_INVERSE(m_mal_Pu, m_iPu, double);
	}
    }

  if (m_FullDebug>0)
    {
      ofstream aof;
      char Buffer[1024];
      sprintf(Buffer,"/tmp/PuVar.dat");
      aof.open(Buffer,ofstream::out);
      for( int i=0;i<(2*m_QP_N+2*m_PrwSupport.StepNumber);i++)
	{
	  for( int j=0;j<(2*m_QP_N+2*m_PrwSupport.StepNumber);j++)
	    aof << m_Pu[j+i*(2*m_QP_N+2*m_PrwSupport.StepNumber)] << " " ;
	  aof << endl;
	}
      aof.close();

      sprintf(Buffer,"/tmp/tmpPuVar.dat");
      aof.open(Buffer,ofstream::out);
      for(int i=0;i<(2*m_QP_N+2*m_PrwSupport.StepNumber);i++)
	{
	  for(int j=0;j<(2*m_QP_N+2*m_PrwSupport.StepNumber);j++)
	    aof << ptPu[j+i*(2*m_QP_N+2*m_PrwSupport.StepNumber)] << " " ;
	  aof << endl;
	}
      aof.close();

      sprintf(Buffer,"/tmp/tmpPuVar.dat");
      aof.open(Buffer,ofstream::out);
      for(int i=0;i<(2*m_QP_N+2*m_PrwSupport.StepNumber);i++)
	{
	  for(int j=0;j<(2*m_QP_N+2*m_PrwSupport.StepNumber);j++)
	    aof << ptPu[j+i*(2*m_QP_N+2*m_PrwSupport.StepNumber)] << " " ;
	  aof << endl;
	}
      aof.close();

      if ((m_FastFormulationMode==QLDANDLQ) ||
	  (m_FastFormulationMode==PLDPHerdt))
	{
	  sprintf(Buffer,"/tmp/tmpiLQVar.dat");
	  aof.open(Buffer,ofstream::out);
	  for( int i=0;i<(2*m_QP_N+2*m_PrwSupport.StepNumber);i++)
	    {
	      for( int j=0;j<(2*m_QP_N+2*m_PrwSupport.StepNumber);j++)
		aof << m_iLQ(i,j) << " " ;
	      aof << endl;
	    }
	  aof.close();
	}

    }

  delete [] lInterPu;
  return 0;
}

void ZMPVelocityReferencedQP::initFeet()
{

  //Define the initial coordinates of the feet
  //This might be done when creating SupportState
  supportfoot_t aSFLeft;
  supportfoot_t aSFRight;
  aSFLeft.x = 0.0;
  aSFLeft.y = 0.1;//TODO:
  aSFLeft.theta = 0.0;
  aSFLeft.StartTime = 0.0;
  aSFLeft.SupportFoot = 1;
  aSFRight.x = 0.0;
  aSFRight.y = -0.1;//TODO:
  aSFRight.theta = 0.0;
  aSFRight.StartTime = 0.0;
  aSFRight.SupportFoot = -1;

  QueueOfSupportFeet.push_back(aSFLeft);
  QueueOfSupportFeet.push_back(aSFRight);

}


int ZMPVelocityReferencedQP::InitConstants()
{
  int r;
  if ((r=InitializeMatrixPbConstants())<0)
    return r;
  if(m_FastFormulationMode != PLDPHerdt)
      if ((r=BuildingConstantPartOfConstraintMatrices())<0)
	return r;

  return 0;
}

void ZMPVelocityReferencedQP::SetAlpha(const double &anAlpha)
{
  m_Alpha = anAlpha;
}

const double & ZMPVelocityReferencedQP::GetAlpha() const
{
  return m_Alpha;
}

void ZMPVelocityReferencedQP::SetBeta(const double &anAlpha)
{
  m_Beta = anAlpha;
}

const double & ZMPVelocityReferencedQP::GetBeta() const
{
  return m_Beta;
}



//------------------new functions---
//
//
//----------------------------------

int ZMPVelocityReferencedQP::dumpProblem(MAL_VECTOR(& xk,double),
					 double Time)
{
  ofstream aof;

  char Buffer[1024];
  sprintf(Buffer,"/tmp/ProblemFF_%f.dat",Time);
  aof.open(Buffer,ofstream::out);

  // Dumping D.
  m_Pb.dumpProblem(aof);
  
  return 0;
}

int ZMPVelocityReferencedQP::buildConstraintMatrices(double * &,
						     double * &DU,
						     double , 
						     double StartingTime,
						     deque<LinearConstraintInequalityFreeFeet_t> &
						     QueueOfLConstraintInequalitiesFreeFeet,
						     deque<LinearConstraintInequalityFreeFeet_t> &
						     QueueOfFeetPosInequalities,
						     deque<supportfoot_t> &
						     QueueOfSupportFeet,
						     double ,
						     int NbOfConstraints,
						     MAL_VECTOR(& xk,double))
{

  m_Pb.initialize(DU,NbOfConstraints*2*(m_QP_N+m_PrwSupport.StepNumber),0.0);
  m_Pb.initialize(m_Pb.DS,2*(m_QP_N+m_PrwSupport.StepNumber),0.0);

  //Current support foot
  deque<LinearConstraintInequalityFreeFeet_t>::iterator LCIFF_it;
  deque<supportfoot_t>::iterator CurSF_it;
  CurSF_it = QueueOfSupportFeet.end();
  CurSF_it--;
  while(CurSF_it->SupportFoot!=m_CurrentSupport.Foot)
    CurSF_it--;

  LCIFF_it = QueueOfLConstraintInequalitiesFreeFeet.begin();


  double FFPx, FFPy;

  int IndexConstraint = 0;

  //ZMP constraints
  for( int i=0;i<m_QP_N;i++)
    {
      if(LCIFF_it->StepNumber==0)
	{//c'est pas bon ca
	  FFPx = CurSF_it->x;
	  FFPy = CurSF_it->y;
	}
      else
	{
	  FFPx = 0.0;
	  FFPy = 0.0;
	}

      // For each constraint.
      for(int j=0;j<MAL_MATRIX_NB_ROWS(LCIFF_it->D);j++)
	{
	  m_Pb.DS[IndexConstraint] =
	    // X Axis * A
	    (FFPx-xk[0] * m_Px(i,0)-
	     xk[1] * m_Px(i,1)-
	     xk[2] * m_Px(i,2))
	    * LCIFF_it->D(j,0)
	    +
	    // Y Axis * A
	     (FFPy-xk[3] * m_Px(i,0)-
	      xk[4] * m_Px(i,1)-
	      xk[5] * m_Px(i,2))
	    * LCIFF_it->D(j,1)
	    // Constante part of the constraint
	    + LCIFF_it->Dc(j,0);

	  if (m_FastFormulationMode==QLD)
	    {
	      // In this case, Pu is triangular.
	      // so we can speed up the computation.

	      for(int k=0;k<=i;k++)
		{
		  // X axis
		  DU[IndexConstraint+k*(NbOfConstraints+1)] =
		    -(LCIFF_it)->D(j,0)*m_Pu[k*m_QP_N+i];

		  // Y axis
		  DU[IndexConstraint+(k+m_QP_N)*(NbOfConstraints+1)] =
		    -(LCIFF_it)->D(j,1)*m_Pu[k*m_QP_N+i];
		}

	    }
	  else if ((m_FastFormulationMode==QLDANDLQ)||
		   (m_FastFormulationMode==PLDPHerdt))
	    {
	      // In this case, Pu is *NOT* triangular.
	      for(int k=0;k<m_QP_N;k++)
		{
		  // X axis
		  DU[IndexConstraint+k*(NbOfConstraints+1)] =
		    -(LCIFF_it)->D(j,0)*m_Pu[k*2*(m_QP_N+m_PrwSupport.StepNumber)+i];
		  // Y axis
		  DU[IndexConstraint+(k+m_QP_N)*(NbOfConstraints+1)] =
		    -(LCIFF_it)->D(j,1)*m_Pu[k*2*(m_QP_N+m_PrwSupport.StepNumber)+i];
		}
	    }

	  //Feet variables after jerk: [dddX,dddY,FPx,FPy]
	  if(LCIFF_it->StepNumber>0)
	    {
	      DU[IndexConstraint+(2*m_QP_N+(LCIFF_it)->StepNumber-1)*(NbOfConstraints+1)] = LCIFF_it->D(j,0);
	      DU[IndexConstraint+(2*m_QP_N+m_PrwSupport.StepNumber+(LCIFF_it)->StepNumber-1)*(NbOfConstraints+1)] = LCIFF_it->D(j,1);
	    }
	  IndexConstraint++;
	}
      LCIFF_it++;
    }


  //Feet position constraints
  LCIFF_it = QueueOfFeetPosInequalities.begin();
  for( int i=0;i<m_PrwSupport.StepNumber;i++)
    {
      if(LCIFF_it->StepNumber==1)
	{
	  FFPx = CurSF_it->x;
	  FFPy = CurSF_it->y;
	}
      else
	{
	  FFPx = 0.0;
	  FFPy = 0.0;
	}


      // For each constraint.
      for(unsigned int j=0;j<MAL_MATRIX_NB_ROWS(LCIFF_it->D);j++)
	{
	  m_Pb.DS[IndexConstraint] =
	    // X Axis * A
	    FFPx * LCIFF_it->D(j,0)
	    +
	    // Y Axis * A
	    FFPy * LCIFF_it->D(j,1)
	    // Constante part of the constraint
	    + LCIFF_it->Dc(j,0);


	  //Foot variables after jerk: [dddX,dddY,FPx,FPy]
	  if((LCIFF_it)->StepNumber==1)
	    {
	      m_Pb.DU[IndexConstraint+(2*m_QP_N+(LCIFF_it)->StepNumber-1)*(NbOfConstraints+1)] =
		-LCIFF_it->D(j,0);
	      m_Pb.DU[IndexConstraint+(2*m_QP_N+m_PrwSupport.StepNumber+LCIFF_it->StepNumber-1)*(NbOfConstraints+1)] =
		-LCIFF_it->D(j,1);
	    }
	  if((LCIFF_it)->StepNumber>1)
	    {
	      m_Pb.DU[IndexConstraint+(2*m_QP_N+(LCIFF_it)->StepNumber-1)*(NbOfConstraints+1)] =
		-(LCIFF_it)->D(j,0);
	      m_Pb.DU[IndexConstraint+(2*m_QP_N+(LCIFF_it)->StepNumber-2)*(NbOfConstraints+1)] =
		(LCIFF_it)->D(j,0);
	      m_Pb.DU[IndexConstraint+(2*m_QP_N+m_PrwSupport.StepNumber+(LCIFF_it)->StepNumber-1)*(NbOfConstraints+1)] =
		-(LCIFF_it)->D(j,1);
	      m_Pb.DU[IndexConstraint+(2*m_QP_N+m_PrwSupport.StepNumber+(LCIFF_it)->StepNumber-2)*(NbOfConstraints+1)] =
		(LCIFF_it)->D(j,1);
	    }
	  IndexConstraint++;
	}

      LCIFF_it++;
    }

  static double localtime = -m_QP_T;
  localtime+=m_QP_T;

  return 0;
}



//--------------------------------------
//
//
//-----------new functions--------------
void ZMPVelocityReferencedQP::CallMethod(std::string & Method, std::istringstream &strm)
{
  if (Method==":previewcontroltime")
    {
      strm >> m_PreviewControlTime;
    }
  if (Method==":numberstepsbeforestop")
    {
      strm >> m_SupportFSM->m_NbOfStepsSSDS;
    }
  if (Method==":comheight")
    {
      strm >> m_ComHeight;
      m_CoM.SetComHeight(m_ComHeight);
    }

  ZMPRefTrajectoryGeneration::CallMethod(Method,strm);
}

int ZMPVelocityReferencedQP::InitOnLine(deque<ZMPPosition> & FinalZMPPositions,
					deque<COMState> & FinalCoMPositions,
					deque<FootAbsolutePosition> & FinalLeftFootAbsolutePositions,
					deque<FootAbsolutePosition> & FinalRightFootAbsolutePositions,
					FootAbsolutePosition & InitLeftFootAbsolutePosition,
					FootAbsolutePosition & InitRightFootAbsolutePosition,
					deque<RelativeFootPosition> &, // RelativeFootPositions,
					COMState & lStartingCOMState,
					MAL_S3_VECTOR_TYPE(double) & lStartingZMPPosition)
{

  FootAbsolutePosition CurrentLeftFootAbsPos, CurrentRightFootAbsPos;

  // Set the internal state of the ZMPRefTrajectory object.
  m_OnLineMode = true;
  m_EndingPhase = false;
  m_TimeToStopOnLineMode = -1.0;

  // Initialize position of the feet.
  CurrentLeftFootAbsPos = InitLeftFootAbsolutePosition;
  CurrentLeftFootAbsPos.z = 0.0;//m_FTGS->m_AnklePositionLeft[2];
  CurrentLeftFootAbsPos.time = 0.0;
  CurrentLeftFootAbsPos.theta = 0.0;


  CurrentRightFootAbsPos = InitRightFootAbsolutePosition;
  CurrentRightFootAbsPos.z = 0.0;//m_FTGS->m_AnklePositionRight[2];
  CurrentRightFootAbsPos.time = 0.0;
  CurrentRightFootAbsPos.theta = 0.0;

  // V pre is the difference between
  // the current m_SupportFSM position and the precedent.


  int AddArraySize;
  {
    assert(m_SamplingPeriod > 0);
    double ldAddArraySize = m_TimeBuffer/m_SamplingPeriod;
    AddArraySize = (int) ldAddArraySize;
  }

  FinalZMPPositions.resize(AddArraySize);
  FinalCoMPositions.resize(AddArraySize);
  FinalLeftFootAbsolutePositions.resize(AddArraySize);
  FinalRightFootAbsolutePositions.resize(AddArraySize);
  int CurrentZMPindex=0;


  for( unsigned int i=0;i<FinalZMPPositions.size();i++)
    {

      // Smooth ramp
      FinalZMPPositions[CurrentZMPindex].px = lStartingZMPPosition(0);
      FinalZMPPositions[CurrentZMPindex].py = lStartingZMPPosition(1);
      FinalZMPPositions[CurrentZMPindex].pz = lStartingZMPPosition(2);
      FinalZMPPositions[CurrentZMPindex].theta = 0.0;
      FinalZMPPositions[CurrentZMPindex].time = m_CurrentTime;
      FinalZMPPositions[CurrentZMPindex].stepType = 0;

      // Set CoM positions.
      FinalCoMPositions[CurrentZMPindex] = lStartingCOMState;
      // Set Left Foot positions.
      FinalLeftFootAbsolutePositions[CurrentZMPindex] = CurrentLeftFootAbsPos;
      FinalRightFootAbsolutePositions[CurrentZMPindex] = CurrentRightFootAbsPos;

      FinalLeftFootAbsolutePositions[CurrentZMPindex].time =
        FinalRightFootAbsolutePositions[CurrentZMPindex].time = m_CurrentTime;

      FinalLeftFootAbsolutePositions[CurrentZMPindex].stepType =
        FinalRightFootAbsolutePositions[CurrentZMPindex].stepType = 10;



      if(m_FullDebug>0)
	{
	  //Feet coordinates for plot in scilab
	  ofstream aoffeet;
	  aoffeet.open("/tmp/Feet.dat",ios::app);
	  aoffeet<<FinalLeftFootAbsolutePositions[CurrentZMPindex].time<<"    "
		 <<FinalLeftFootAbsolutePositions[CurrentZMPindex].x<<"    "
		 <<FinalLeftFootAbsolutePositions[CurrentZMPindex].y<<"    "
		 <<FinalLeftFootAbsolutePositions[CurrentZMPindex].z<<"    "
		 <<FinalLeftFootAbsolutePositions[CurrentZMPindex].stepType<<"    "
		 <<FinalRightFootAbsolutePositions[CurrentZMPindex].x<<"    "
		 <<FinalRightFootAbsolutePositions[CurrentZMPindex].y<<"    "
		 <<FinalRightFootAbsolutePositions[CurrentZMPindex].z<<"    "
		 <<FinalRightFootAbsolutePositions[CurrentZMPindex].stepType<<"    "<<endl;
	  aoffeet.close();
	}

      m_CurrentTime += m_SamplingPeriod;
      CurrentZMPindex++;

    }

  com_t CoM;

  CoM.x[0] = lStartingCOMState.x[0];
  CoM.x[1] = lStartingCOMState.x[1];
  CoM.x[2] = lStartingCOMState.x[2];
  CoM.y[0] = lStartingCOMState.y[0];
  CoM.y[1] = lStartingCOMState.y[1];
  CoM.y[2] = lStartingCOMState.y[2];

  m_CoM(CoM);

  return 0;
}



void ZMPVelocityReferencedQP::computeCholeskyOfQ(double * OptA)
{

  /*! Build cholesky matrix of the optimum
    We copy only the upper corner of the OptA matrix
    because we know its specific structure.
  */
  double *localQ=new double[2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)];
  for( int i=0;i<2*(m_QP_N+m_PrwSupport.StepNumber);i++)
    for( int j=0;j<2*(m_QP_N+m_PrwSupport.StepNumber);j++)
      localQ[i*2*(m_QP_N+m_PrwSupport.StepNumber)+j] = OptA[i*2*(m_QP_N+m_PrwSupport.StepNumber)+j];

  double *localLQ=new double[2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)];
  double *localiLQ=new double[2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)];

  memset(localLQ,0,2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)*sizeof(double));
  memset(localiLQ,0,2*(m_QP_N+m_PrwSupport.StepNumber)*2*(m_QP_N+m_PrwSupport.StepNumber)*sizeof(double));

  OptCholesky anOCD(2*(m_QP_N+m_PrwSupport.StepNumber),2*(m_QP_N+m_PrwSupport.StepNumber),OptCholesky::MODE_NORMAL);
  anOCD.SetA(localQ,2*(m_QP_N+m_PrwSupport.StepNumber));
  anOCD.SetL(localLQ);
  anOCD.SetiL(localiLQ);

  anOCD.ComputeNormalCholeskyOnANormal();
  anOCD.ComputeInverseCholeskyNormal(1);


  MAL_MATRIX_RESIZE(m_LQ,2*(m_QP_N+m_PrwSupport.StepNumber),2*(m_QP_N+m_PrwSupport.StepNumber));
  MAL_MATRIX_RESIZE(m_iLQ,2*(m_QP_N+m_PrwSupport.StepNumber),2*(m_QP_N+m_PrwSupport.StepNumber));


  for( int i=0;i<2*(m_QP_N+m_PrwSupport.StepNumber);i++)
    {
      for( int j=0;j<2*(m_QP_N+m_PrwSupport.StepNumber);j++)
	{
	  m_LQ(i,j) = localLQ[i*2*(m_QP_N+m_PrwSupport.StepNumber)+j];
	  // 	  m_LQ(i+2*(m_QP_N+m_PrwSupport.StepNumber),j+2*(m_QP_N+m_PrwSupport.StepNumber)) = localLQ[i*2*(m_QP_N+m_PrwSupport.StepNumber)+j];
	  // 	  m_LQ(i,j+(m_QP_N+m_PrwSupport.StepNumber)) = 0.0;
	  // 	  m_LQ(i+(m_QP_N+m_PrwSupport.StepNumber),j) = 0.0;

	  m_iLQ(i,j) = localiLQ[i*2*(m_QP_N+m_PrwSupport.StepNumber)+j];
	  // 	  m_iLQ(i+(m_QP_N+m_PrwSupport.StepNumber),j+(m_QP_N+m_PrwSupport.StepNumber)) = localiLQ[i*(m_QP_N+m_PrwSupport.StepNumber)+j];
	  // 	  m_iLQ(i,j+(m_QP_N+m_PrwSupport.StepNumber)) = 0.0;
	  // 	  m_iLQ(i+(m_QP_N+m_PrwSupport.StepNumber),j) = 0.0;

	}
    }


  delete [] localQ;
  delete [] localLQ;
  delete [] localiLQ;

}


void ZMPVelocityReferencedQP::interpolateTrunkState(double time, int CurrentIndex,
						    deque<COMState> & FinalCOMStates)
{
  if(m_CurrentSupport.Phase == 1 && time+m_TimeBuffer+3.0/2.0*m_QP_T < m_CurrentSupport.TimeLimit)
    {
      //Set parameters for trunk interpolation
      m_c = 3.0*(m_TrunkStateT.yaw[1]-m_TrunkState.yaw[1])/(m_QP_T*m_QP_T);
      m_d = -2.0*m_c/(3.0*m_QP_T);
      m_a =  m_TrunkState.yaw[1];

      double tT;
      double Theta = m_TrunkState.yaw[0];
      //double dTheta = m_TrunkState.yaw[1];
      //double ddTheta = m_TrunkState.yaw[2];

      FinalCOMStates[CurrentIndex].yaw[0] = m_TrunkState.yaw[0];
      //Interpolate the
      for(int k = 1; k<=(int)(m_QP_T/m_SamplingPeriod);k++)
	{
	  tT = (double)k*m_SamplingPeriod;
	  //interpolate the orientation of the trunk
	  if(fabs(m_TrunkStateT.yaw[1]-m_TrunkState.yaw[1])-0.000001 > 0)
	    {
	      m_TrunkState.yaw[0] = (((1.0/4.0*m_d*tT+1.0/3.0*m_c)*
				      tT)*tT+m_a)*tT+Theta;
	      m_TrunkState.yaw[1] = ((m_d*tT+m_c)*tT)*tT+m_a;
	      m_TrunkState.yaw[2] = (3.0*m_d*tT+2.0*m_c)*tT;

	      m_QueueOfTrunkStates.push_back(m_TrunkState);
	    }
	  else
	    {
	      m_TrunkState.yaw[0] += m_SamplingPeriod*m_TrunkStateT.yaw[1];

	      m_QueueOfTrunkStates.push_back(m_TrunkState);
	    }
	  FinalCOMStates[CurrentIndex+k].yaw[0] = m_TrunkState.yaw[0];
	  if(m_FullDebug>2)
	    {
	      ofstream aof;
	      aof.open("/tmp/Trunk.dat",ofstream::app);
	      aof<<time+k*m_SamplingPeriod<<" "<<m_TrunkState.yaw[0]<<" "<<m_TrunkState.yaw[1]<<" "<<m_TrunkState.yaw[2]<<endl;
	      aof.close();
	    }
	}
    }
  else if (m_CurrentSupport.Phase == 0 || time+m_TimeBuffer+3.0/2.0*m_QP_T > m_CurrentSupport.TimeLimit)
    {
      for(int k = 0; k<=(int)(m_QP_T/m_SamplingPeriod);k++)
	{
	  FinalCOMStates[CurrentIndex+k].yaw[0] = m_TrunkState.yaw[0];
	}
    }
  

}


void ZMPVelocityReferencedQP::interpolateFeetPositions(double time, int CurrentIndex,
						       deque<FootAbsolutePosition> &FinalLeftFootAbsolutePositions,
						       deque<FootAbsolutePosition> &FinalRightFootAbsolutePositions)
{
  double LocalInterpolationTime = (time+m_TimeBuffer)-(m_CurrentSupport.TimeLimit-m_SupportFSM->m_SSPeriod);

  double StepHeight = 0.05;
  int StepType = 1;

  if(m_CurrentSupport.Phase == 1 && time+m_TimeBuffer+3.0/2.0*m_QP_T < m_CurrentSupport.TimeLimit)
    {
      //determine coefficients of interpolation polynom
      double ModulationSupportCoefficient = 0.9;
      double ModulatedSingleSupportTime = (m_SupportFSM->m_SSPeriod-m_QP_T) * ModulationSupportCoefficient;
      double EndOfLiftOff = ((m_SupportFSM->m_SSPeriod-m_QP_T)-ModulatedSingleSupportTime)*0.5;
      double InterpolationTimePassed = 0.0;
      if(LocalInterpolationTime>EndOfLiftOff)
	InterpolationTimePassed = LocalInterpolationTime-EndOfLiftOff;

      FootAbsolutePosition LastSwingFootPosition;

      if(m_CurrentSupport.Foot==1)
	{
	  LastSwingFootPosition = FinalRightFootAbsolutePositions[CurrentIndex];
	}
      else
	{
	  LastSwingFootPosition = FinalLeftFootAbsolutePositions[CurrentIndex];
	}
      //Set parameters for current polynomial
      m_FTGS->SetParametersWithInitPosInitSpeed(FootTrajectoryGenerationStandard::X_AXIS,
						ModulatedSingleSupportTime-InterpolationTimePassed,m_FPx,
						LastSwingFootPosition.x,
						LastSwingFootPosition.dx);
      m_FTGS->SetParametersWithInitPosInitSpeed(FootTrajectoryGenerationStandard::Y_AXIS,
						ModulatedSingleSupportTime-InterpolationTimePassed,m_FPy,
						LastSwingFootPosition.y,
						LastSwingFootPosition.dy);

      if(m_CurrentSupport.StateChanged==true)
	m_FTGS->SetParameters(FootTrajectoryGenerationStandard::Z_AXIS, m_SupportFSM->m_SSPeriod-m_QP_T,StepHeight);

      m_FTGS->SetParametersWithInitPosInitSpeed(FootTrajectoryGenerationStandard::THETA_AXIS,
						ModulatedSingleSupportTime-InterpolationTimePassed,
						m_PreviewedSupportAngles[0]*180.0/M_PI,
						LastSwingFootPosition.theta,
						LastSwingFootPosition.dtheta);
      m_FTGS->SetParametersWithInitPosInitSpeed(FootTrajectoryGenerationStandard::OMEGA_AXIS,
						ModulatedSingleSupportTime-InterpolationTimePassed,0.0*180.0/M_PI,
						LastSwingFootPosition.omega,
						LastSwingFootPosition.domega);
      m_FTGS->SetParametersWithInitPosInitSpeed(FootTrajectoryGenerationStandard::OMEGA2_AXIS,
						ModulatedSingleSupportTime-InterpolationTimePassed,2*0.0*180.0/M_PI,
						LastSwingFootPosition.omega2,
						LastSwingFootPosition.domega2);

      for(int k = 1; k<=(int)(m_QP_T/m_SamplingPeriod);k++)
	{
	  if (m_CurrentSupport.Foot==1)
	    {
	      m_FTGS->UpdateFootPosition(FinalLeftFootAbsolutePositions,
					 FinalRightFootAbsolutePositions,
					 CurrentIndex,k,
					 LocalInterpolationTime,
					 ModulatedSingleSupportTime,
					 StepType, -1);
	    }
	  else
	    {
	      m_FTGS->UpdateFootPosition(FinalRightFootAbsolutePositions,
					 FinalLeftFootAbsolutePositions,
					 CurrentIndex,k,
					 LocalInterpolationTime,
					 ModulatedSingleSupportTime,
					 StepType, 1);
	    }
	  FinalLeftFootAbsolutePositions[CurrentIndex+k].time =
	    FinalRightFootAbsolutePositions[CurrentIndex+k].time = time+m_TimeBuffer+k*m_SamplingPeriod;


	  if(m_FullDebug>0)
	    {
	      ofstream aoffeet;
	      aoffeet.open("/tmp/Feet.dat",ios::app);
	      aoffeet<<time+m_TimeBuffer+k*m_SamplingPeriod<<"    "
		     <<FinalLeftFootAbsolutePositions[CurrentIndex+k].x<<"    "
		     <<FinalLeftFootAbsolutePositions[CurrentIndex+k].y<<"    "
		     <<FinalLeftFootAbsolutePositions[CurrentIndex+k].z<<"    "
		     <<FinalLeftFootAbsolutePositions[CurrentIndex+k].stepType<<"    "
		     <<FinalRightFootAbsolutePositions[CurrentIndex+k].x<<"    "
		     <<FinalRightFootAbsolutePositions[CurrentIndex+k].y<<"    "
		     <<FinalRightFootAbsolutePositions[CurrentIndex+k].z<<"    "
		     <<FinalRightFootAbsolutePositions[CurrentIndex+k].stepType<<"    "
		     <<endl;
	      aoffeet.close();
	    }

	}
    }
  else if (m_CurrentSupport.Phase == 0 || time+m_TimeBuffer+3.0/2.0*m_QP_T > m_CurrentSupport.TimeLimit)
    {
      for(int k = 0; k<=(int)(m_QP_T/m_SamplingPeriod);k++)
	{
	  FinalRightFootAbsolutePositions[CurrentIndex+k]=FinalRightFootAbsolutePositions[CurrentIndex+k-1];
	  FinalLeftFootAbsolutePositions[CurrentIndex+k]=FinalLeftFootAbsolutePositions[CurrentIndex+k-1];
	  FinalLeftFootAbsolutePositions[CurrentIndex+k].time =
	    FinalRightFootAbsolutePositions[CurrentIndex+k].time = time+m_TimeBuffer+k*m_SamplingPeriod;
	  FinalLeftFootAbsolutePositions[CurrentIndex+k].stepType =
	    FinalRightFootAbsolutePositions[CurrentIndex+k].stepType = 10;

	  if(m_FullDebug>0)
	    {
	      ofstream aoffeet;
	      aoffeet.open("/tmp/Feet.dat",ios::app);
	      aoffeet<<time+m_TimeBuffer+k*m_SamplingPeriod<<"    "
		     <<FinalLeftFootAbsolutePositions[CurrentIndex+k].x<<"    "
		     <<FinalLeftFootAbsolutePositions[CurrentIndex+k].y<<"    "
		     <<FinalLeftFootAbsolutePositions[CurrentIndex+k].z<<"    "
		     <<FinalLeftFootAbsolutePositions[CurrentIndex+k].stepType<<"    "
		     <<FinalRightFootAbsolutePositions[CurrentIndex+k].x<<"    "
		     <<FinalRightFootAbsolutePositions[CurrentIndex+k].y<<"    "
		     <<FinalRightFootAbsolutePositions[CurrentIndex+k].z<<"    "
		     <<FinalRightFootAbsolutePositions[CurrentIndex+k].stepType<<"    "
		     <<endl;
	      aoffeet.close();
	    }
	}
    }
}


void ZMPVelocityReferencedQP::OnLine(double time,
				     deque<ZMPPosition> & FinalZMPPositions,
				     deque<COMState> & FinalCOMStates,
				     deque<FootAbsolutePosition> &FinalLeftFootAbsolutePositions,
				     deque<FootAbsolutePosition> &FinalRightFootAbsolutePositions)
{

  // If on-line mode not activated we go out.
  if (!m_OnLineMode)
    return;

  // Testing if we are reaching the end of the online mode.
  if ((m_EndingPhase) &&
      (time>=m_TimeToStopOnLineMode))
    m_OnLineMode = false;


  if(time + 0.00001 > m_UpperTimeLimitToUpdate)
    {

      int NbOfConstraints=0; // Nb of constraints are not known in advance

      MAL_VECTOR_DIM(xk,double,6);

      int CriteriaToMaximize=1;


      deque<LinearConstraintInequality_t> QueueOfLConstraintInequalities;
      deque<LinearConstraintInequalityFreeFeet_t> QueueOfLConstraintInequalitiesFreeFeet;
      deque<LinearConstraintInequalityFreeFeet_t> QueueOfFeetPosInequalities;

      // pre compute the matrices needed for the optimization.
      double TotalAmountOfCPUTime=0.0,CurrentCPUTime=0.0;
      struct timeval start,end;

      bool StartingSequence = true;


      gettimeofday(&start,0);


      m_OP->verifyAccelerationOfHipJoint(m_VelRef, m_TrunkState,
					 m_TrunkStateT, m_CurrentSupport);


      m_OP->previewOrientations(time+m_TimeBuffer,
				m_PreviewedSupportAngles,
				m_TrunkState,
				m_TrunkStateT,
				m_SupportFSM, m_CurrentSupport,
				FinalLeftFootAbsolutePositions,
				FinalRightFootAbsolutePositions);


      // Read the current state of the 2D Linearized Inverted Pendulum.
      m_CoM.GetState(xk);

      m_GenVR->computeGlobalReference(m_Matrices, m_TrunkStateT);

      //Apply external forces
      if(m_PerturbationOccured == true)
	{
	  xk(2) = xk(2)+m_PerturbationAcceleration(2);
	  xk(5) = xk(5)+m_PerturbationAcceleration(5);
	  m_PerturbationOccured = false;
	}
      m_CoM.setState(xk);

      m_SupportFSM->setSupportState(time+m_TimeBuffer, 0, m_CurrentSupport, m_VelRef);


      //Add a new support foot to the support feet history deque
      if(m_CurrentSupport.StateChanged == true)
	{
	  deque<FootAbsolutePosition>::iterator FAP_it;
	  supportfoot_t newSF;
	  if(m_CurrentSupport.Foot==1)
	    {
	      FAP_it = FinalLeftFootAbsolutePositions.end();
	      FAP_it--;
	    }
	  else
	    {
	      FAP_it = FinalRightFootAbsolutePositions.end();
	      FAP_it--;
	    }

	  newSF.x = FAP_it->x;
	  newSF.y = FAP_it->y;
	  newSF.theta = FAP_it->theta*M_PI/180.0;
	  newSF.StartTime = time+m_TimeBuffer;
	  newSF.SupportFoot = m_CurrentSupport.Foot;

	  QueueOfSupportFeet.push_back(newSF);
	}

      deque<support_state_t> deqPrwSupportStates;
      deqPrwSupportStates.push_back(m_CurrentSupport);

      m_GenVR->setCurrentTime(time+m_TimeBuffer);
      m_Matrices.Reference(m_VelRef);

      m_GenVR->computeGlobalReference(m_Matrices, m_TrunkStateT);

      m_GenVR->preview(m_Matrices, m_SupportFSM, deqPrwSupportStates);

      m_GenVR->generateSelectionMatrices(m_Matrices, deqPrwSupportStates);

      m_fCALS->buildLinearConstraintInequalities(FinalLeftFootAbsolutePositions,
						 FinalRightFootAbsolutePositions,
						 QueueOfLConstraintInequalitiesFreeFeet,
						 QueueOfFeetPosInequalities,
						 m_VelRef,
						 time+m_TimeBuffer,
						 m_QP_N,
						 m_SupportFSM, m_CurrentSupport, m_PrwSupport, m_PreviewedSupportAngles,
						 NbOfConstraints);

      deque<supportfoot_t>::iterator SF_it;//, storeFF_it, VFF_it;
      SF_it = QueueOfSupportFeet.end();
      SF_it--;
      m_Matrices.SupportFoot(*SF_it);

      m_Pb.setDimensions(2*(m_QP_N + m_PrwSupport.StepNumber),
                         NbOfConstraints,
                         0);

      if (m_FastFormulationMode==QLDANDLQ)
        m_Pb.iwar[0]=0;
      else
        m_Pb.iwar[0]=1;

      m_Pb.initialize(m_Pb.Q,2*(m_QP_N + m_PrwSupport.StepNumber)*2*(m_QP_N + m_PrwSupport.StepNumber),0.0);
      m_GenVR->buildInvariantPart(m_Pb, m_Matrices);

      m_Pb.initialize(m_Pb.D,2*(m_QP_N + m_PrwSupport.StepNumber),0.0);
      m_GenVR->updateProblem(m_Pb, m_Matrices, deqPrwSupportStates);


      if(m_FastFormulationMode == PLDPHerdt)
	{
	  computeCholeskyOfQ(m_Pb.Q);
	  buildConstraintMatricesPLDPHerdt();
	}


      buildConstraintMatrices(m_Pb.DS,m_Pb.DU,
			      m_QP_T,
			      time+m_TimeBuffer,
			      QueueOfLConstraintInequalitiesFreeFeet,
			      QueueOfFeetPosInequalities,
			      QueueOfSupportFeet,
			      m_ComHeight,
			      NbOfConstraints,
			      xk);


      double ldt = 0.0;
      if ((m_FastFormulationMode==QLDANDLQ)||
	  (m_FastFormulationMode==QLD))
	{
	  struct timeval lbegin,lend;
	  gettimeofday(&lbegin,0);

          m_Pb.dumpProblem("/tmp/Problem.dat");

	  m_Pb.solve( QPProblem_s::QLD );

	  gettimeofday(&lend,0);

	  ldt = lend.tv_sec - lbegin.tv_sec +
	    0.000001 * (lend.tv_usec - lbegin.tv_usec);

	  int NbOfActivatedConstraints = 0;
	  for(int lk=0;lk<m_Pb.m;lk++)
	    {
	      if (m_Pb.U[lk]>0.0)
		{
		  NbOfActivatedConstraints++;
		}
	    }
	}
      else if (m_FastFormulationMode==PLDPHerdt)
	{
	  struct timeval lbegin,lend;
	  gettimeofday(&lbegin,0);


	  if(m_PLDPSolverHerdt==0)
	    m_PLDPSolverHerdt = new Optimization::Solver::PLDPSolverHerdt((unsigned int)m_QP_N,
									  MAL_RET_MATRIX_DATABLOCK(m_iPu),
									  MAL_RET_MATRIX_DATABLOCK(m_Px),
									  m_Pu,
									  MAL_RET_MATRIX_DATABLOCK(m_iLQ));


	  unsigned int NumberOfRemovedConstraints = 4; unsigned int NbRemovedFootCstr = 5;

	  m_Pb.ifail=m_PLDPSolverHerdt->SolveProblem(QueueOfLConstraintInequalitiesFreeFeet, QueueOfSupportFeet,
						     m_Pb.D,
						     (unsigned int)m_Pb.m,
						     m_Pb.DU,
						     m_Pb.DS,
						     MAL_RET_VECTOR_DATABLOCK(xk),m_Pb.X,
						     NumberOfRemovedConstraints, NbRemovedFootCstr,
						     StartingSequence,
						     (unsigned int)m_PrwSupport.StepNumber,
						     m_CurrentSupport.StateChanged, time);
	  StartingSequence = false;
	  gettimeofday(&lend,0);
	}
      if (m_Pb.ifail!=0)
	{
	  cout << "IFAIL: " << m_Pb.ifail << " at time: " << time << endl;
	  //return -1;
	}

      double *ptX=0;
      if ((m_FastFormulationMode==QLDANDLQ)||
	  (m_FastFormulationMode==PLDPHerdt))
	{
	  /* Multiply the solution by the transpose of iLQ
      	     because it is a triangular matrix we do a specific
      	     multiplication.
	  */
	  memset(m_Pb.NewX,0,2*(m_QP_N+m_PrwSupport.StepNumber)*sizeof(double));

	  double *pm_iLQ = MAL_RET_MATRIX_DATABLOCK(m_iLQ);
	  double *pNewX = m_Pb.NewX;

	  for( int i=0;i<2*(m_QP_N+m_PrwSupport.StepNumber);i++)
	    {
	      double *pX= m_Pb.X+i;
	      double *piLQ = pm_iLQ+i*2*(m_QP_N+m_PrwSupport.StepNumber)+i;
	      *pNewX = 0.0;
	      for(int j=i;j<2*(m_QP_N+m_PrwSupport.StepNumber);j++)
		{
		  *pNewX+= (*piLQ) * (*pX++);
		  piLQ+=2*(m_QP_N+m_PrwSupport.StepNumber);
		}
	      pNewX++;
	    }
	  ptX=m_Pb.NewX;
	}
      else
	ptX=m_Pb.X;


      FinalCOMStates.resize((int)((m_QP_T+m_TimeBuffer)/m_SamplingPeriod));
      FinalZMPPositions.resize((int)((m_QP_T+m_TimeBuffer)/m_SamplingPeriod));
      FinalLeftFootAbsolutePositions.resize((int)((m_QP_T+m_TimeBuffer)/m_SamplingPeriod));
      FinalRightFootAbsolutePositions.resize((int)((m_QP_T+m_TimeBuffer)/m_SamplingPeriod));

      //TODO: The variable CurrentIndex might be obsolete as it introduces a buffer which might be unnecessary.
      int CurrentIndex = (int)(m_TimeBuffer/m_SamplingPeriod)-1;

      m_CoM.Interpolation(FinalCOMStates,
			      FinalZMPPositions,
			      CurrentIndex,
			      ptX[0],ptX[m_QP_N]);
      m_Matrices.CoM(m_CoM.OneIteration(ptX[0],ptX[m_QP_N]));

      //The robot is supposed to stop always with the feet aligned in the lateral plane.
      if(m_CurrentSupport.StepsLeft>0)
	{
	  if(fabs(ptX[2*m_QP_N])-0.00001<0.0)
	    {
	      cout<<"Previewed foot position zero at time: "<<time<<endl;
	    }
	  else if (m_CurrentSupport.TimeLimit-time-m_QP_T/2.0>0)
	    {//The landing position is yet determined by the solver because the robot finds himself still in the single support phase
	      m_FPx = ptX[2*m_QP_N];
	      m_FPy = ptX[2*m_QP_N+m_PrwSupport.StepNumber];
	    }
	}
      else
	{//The solver isn't responsible for the feet positions anymore
	  deque<supportfoot_t>::iterator CurSF_it;
	  CurSF_it = QueueOfSupportFeet.end();
	  CurSF_it--;
	  while(CurSF_it->SupportFoot!=m_CurrentSupport.Foot)
	    CurSF_it--;
	  m_FPx = CurSF_it->x + double(CurSF_it->SupportFoot)*sin(CurSF_it->theta)*m_FeetDistanceDS;
	  m_FPy = CurSF_it->y - double(CurSF_it->SupportFoot)*cos(CurSF_it->theta)*m_FeetDistanceDS;

	  // Specify that we are in the ending phase.
	  if (m_EndingPhase==false)
	    {
	      // This should be done only during the transition EndingPhase=false -> EndingPhase=true
	      m_TimeToStopOnLineMode = m_UpperTimeLimitToUpdate+m_QP_T * m_QP_N;
	      // Set the ZMP reference as very important.
	      // It suppose to work because Gamma appears only during the non-constant 
	    }
	  m_EndingPhase = true;
	  
	}


      interpolateTrunkState(time, CurrentIndex,
			    FinalCOMStates);
      interpolateFeetPositions(time, CurrentIndex,
			       FinalLeftFootAbsolutePositions,
			       FinalRightFootAbsolutePositions);

      m_UpperTimeLimitToUpdate = m_UpperTimeLimitToUpdate+m_QP_T;
      ODEBUG6("uk:" << uk,"/tmp/DebugPBW.dat");
      ODEBUG6("xk:" << xk,"/tmp/DebugPBW.dat");


      // Compute CPU consumption time.
      gettimeofday(&end,0);
      CurrentCPUTime = end.tv_sec - start.tv_sec +
        0.000001 * (end.tv_usec - start.tv_usec);
      TotalAmountOfCPUTime += CurrentCPUTime;


      ODEBUG("Current Time : " <<time << " " <<
	     " Virtual time to simulate: " << QueueOfLConstraintInequalities.back()->EndingTime - time <<
	     "Computation Time " << CurrentCPUTime << " " << TotalAmountOfCPUTime);

      QueueOfLConstraintInequalitiesFreeFeet.clear();
      QueueOfFeetPosInequalities.clear();

    }

}


void ZMPVelocityReferencedQP::GetZMPDiscretization(deque<ZMPPosition> & ,
						   deque<COMState> & ,
						   deque<RelativeFootPosition> &,
						   deque<FootAbsolutePosition> &,
						   deque<FootAbsolutePosition> &,
						   double ,
						   COMState &,
						   MAL_S3_VECTOR(&,double),
						   FootAbsolutePosition & ,
						   FootAbsolutePosition & )
{
}


void ZMPVelocityReferencedQP::OnLineAddFoot(RelativeFootPosition & ,
					    deque<ZMPPosition> & ,
					    deque<COMState> & ,
					    deque<FootAbsolutePosition> &,
					    deque<FootAbsolutePosition> &,
					    bool)
{
  cout << "To be implemented" << endl;
}

int ZMPVelocityReferencedQP::OnLineFootChange(double ,
					      FootAbsolutePosition &,
					      deque<ZMPPosition> & ,
					      deque<COMState> & ,
					      deque<FootAbsolutePosition> &,
					      deque<FootAbsolutePosition> &,
					      StepStackHandler  *)
{
  cout << "To be implemented" << endl;
  return -1;
}

void ZMPVelocityReferencedQP::EndPhaseOfTheWalking(deque<ZMPPosition> &, 
						   deque<COMState> &,
						   deque<FootAbsolutePosition> &,
						   deque<FootAbsolutePosition> &)
{
  cout << "To be implemented" << endl;
}

int ZMPVelocityReferencedQP::ReturnOptimalTimeToRegenerateAStep()
{
  int r = (int)(m_PreviewControlTime/m_SamplingPeriod);
  return 2*r;
}


