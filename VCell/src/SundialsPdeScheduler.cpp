/*
 * (C) Copyright University of Connecticut Health Center 2001.
 * All rights reserved.
 */
#include <VCELL/SundialsPdeScheduler.h>
#include <VCELL/SimTypes.h>
#include <VCELL/Solver.h>
#include <VCELL/VolumeVariable.h>
#include <VCELL/MembraneVariable.h>
#include <VCELL/VolumeRegionVariable.h>
#include <VCELL/MembraneRegionVariable.h>
#include <VCELL/Mesh.h>
#include <VCELL/Feature.h>
#include <VCELL/Element.h>
#include <VCELL/VolumeVarContext.h>
#include <VCELL/VolumeRegionVarContext.h>
#include <VCELL/MembraneVarContext.h>
#include <VCELL/MembraneRegionVarContext.h>
#include <VCELL/SimulationExpression.h>
#include <VCELL/FVUtils.h>
#include <VCELL/SimTool.h>
#include <VCELL/CartesianMesh.h>
#include <VCELL/VolumeRegion.h>
#include <VCELL/MembraneRegion.h>
#include <VCELL/VCellModel.h>
#include <Expression.h>
#include <SimpleSymbolTable.h>
#include <assert.h>

#include <cvode/cvode.h>             /* prototypes for CVODE fcts. and consts. */
#include <nvector/nvector_serial.h>  /* serial N_Vector types, fcts., and macros */
#include <cvode/cvode_spgmr.h>       /* prototype for CVSPGMR */
#include <sundials/sundials_dense.h> /* definitions DenseMat DENSE_ELEM */
#include <sundials/sundials_types.h> /* definition of type realtype */

#include <sstream>
using namespace std;

#define ToleranceType CV_SS
#define epsilon  1e-12

//#define SUNDIALS_USE_PCNONE
//#define INCLUDE_ADVECTION_IN_PC
//#define INCLUDE_BOUNDARY_IN_PC
//#define COMPARE_WITH_OLD

static double interp_coeff[3] = {3.0/2, -1.0/2, 0};
static int dirichletExpIndexes[3] = {BOUNDARY_XP_EXP, BOUNDARY_YP_EXP, BOUNDARY_ZP_EXP};
static int velocityExpIndexes[3] = {VELOCITY_X_EXP, VELOCITY_Y_EXP, VELOCITY_Z_EXP};

SundialsPdeScheduler::SundialsPdeScheduler(Simulation *sim, double rtol, double atol, int numDisTimes, double* disTimes, bool bDefaultOuptput) : Scheduler(sim)
{
	simulation = (SimulationExpression*)sim;
	sundialsSolverMemory = 0;
	mesh = (CartesianMesh*)simulation->getMesh();
	relTol = rtol;
	absTol = atol;
	currDiscontinuityTimeCount = 0;
	numDiscontinuityTimes = numDisTimes;
	discontinuityTimes = disTimes;

	M = 0;
	pcg_workspace = 0;

	bPcReinit = true;
	bLUcomputed = false;

	bSundialsOneStepOutput = bDefaultOuptput;
	currentTime = 0;

	statePointValues = 0;
	neighborStatePointValues = 0;
}


SundialsPdeScheduler::~SundialsPdeScheduler()
{
	N_VDestroy_Serial(y);
	CVodeFree(&sundialsSolverMemory);

	delete[] statePointValues;

	if (bHasVariableDiffusionAdvection) {
		for (int n = 0; n < 3; n ++) {
			delete[] neighborStatePointValues[n];
		}
		delete neighborStatePointValues;
	}

	delete[] global2Local;
	delete[] local2Global;
	delete[] regionSizes;
	delete[] regionOffsets;
	delete[] volVectorOffsets;

	delete M;
	delete[] pcg_workspace;

	if (simulation->getNumVolVariables() > 0) {
		int numVolRegions = mesh->getNumVolumeRegions();
		delete[] regionDefinedVolVariableSizes;
		for (int r = 0; r < numVolRegions; r ++) {
			delete[] regionDefinedVolVariableIndexes[r];
		}
		delete[] regionDefinedVolVariableIndexes;
	}
}

void SundialsPdeScheduler::iterate() {
	if (bFirstTime) {
		setupOrderMaps();
		initSundialsSolver();
	}
	solve();
	bFirstTime = false;
}

void SundialsPdeScheduler::setupOrderMaps() {
	if (!bFirstTime) {
		return;
	}

	dimension =  mesh->getDimension();
	double Dx = mesh->getXScale_um();
	double Dy = mesh->getYScale_um();
	double Dz = mesh->getZScale_um();
	Nx = mesh->getNumVolumeX();
	int Ny = mesh->getNumVolumeY();
	int Nz = mesh->getNumVolumeZ();
	Nxy = Nx * Ny;
	int Nxyz = Nxy * Nz;

	VOLUME = mesh->getVolume_cu();

	oneOverH[0] = 1/Dx;
	oneOverH[1] = 1/Dy;
	oneOverH[2] = 1/Dz;

	pVolumeElement = mesh->getVolumeElements();
	pMembraneElement = mesh->getMembraneElements();

	int numVolRegions = mesh->getNumVolumeRegions();
	global2Local = new int[Nxyz];
	local2Global = new int[Nxyz];
	regionSizes = new int[numVolRegions];
	regionOffsets = new int[numVolRegions];
	regionDefinedVolVariableSizes = 0;
	regionDefinedVolVariableIndexes = 0;
	bRegionHasConstantDiffusionAdvection = 0;
	bHasAdvection = false;
	bHasVariableDiffusionAdvection = false;

	int numActivePoints = 0;
	cout << endl << "numVolRegions=" << numVolRegions << endl;
	for (int r = 0; r < numVolRegions; r ++) {
		regionOffsets[r] = numActivePoints;
		for (int k = 0; k < Nz; k ++) {
			for (int j = 0; j < Ny; j ++) {
				for (int i = 0; i < Nx; i ++) {
					int volIndex = k * Nxy + j * Nx + i;
					int regionID = pVolumeElement[volIndex].region->getId();
					if (regionID == r) {
						//global to local
						global2Local[volIndex] = numActivePoints;
						local2Global[numActivePoints] = volIndex;
						numActivePoints ++;
					}
				}
			}
		}
		regionSizes[r] = numActivePoints - regionOffsets[r];
		cout << "Region " << r << ": size=" << regionSizes[r] << ", offset=" << regionOffsets[r] << endl;
	}
	cout << "# of active points = " << numActivePoints << endl;

	// if local to global mapping is not always in increasing order
	// then in the rhs operator, we need a separate loop for reaction and scale volume
	for (int r = 0; r < numVolRegions; r ++) {
		int localIndex0 = regionOffsets[r];
		for (int localIndex = localIndex0 ; localIndex < localIndex0 + regionSizes[r]-1; localIndex ++) {
			int volIndex = local2Global[localIndex];
			int volIndexp = local2Global[localIndex+1];
			if (volIndexp < volIndex) {
				throw " local to global map not in increasing order";
			}
		}
	}

	volVectorOffsets = new int[numVolRegions];
	numUnknowns  = 0;
	if (simulation->getNumVolVariables() > 0) {
		bRegionHasConstantDiffusionAdvection = new bool[numVolRegions];
		regionDefinedVolVariableSizes = new int[numVolRegions];
		regionDefinedVolVariableIndexes = new int*[numVolRegions];

		// Volume PDE/ODE first
		for (int r = 0; r < numVolRegions; r ++) {
			volVectorOffsets[r] = numUnknowns;
			regionDefinedVolVariableSizes[r] = 0;
			regionDefinedVolVariableIndexes[r] = 0;
			bRegionHasConstantDiffusionAdvection[r] = true;

			int firstPointVolIndex = local2Global[regionOffsets[r]];
			Feature* feature = pVolumeElement[firstPointVolIndex].feature;
			for (int v = 0; v < simulation->getNumVolVariables(); v ++) {
				if (simulation->isVolumeVariableDefinedInRegion(v, r)) {
					numUnknowns += regionSizes[r];
					regionDefinedVolVariableSizes[r] ++;

					VolumeVariable* var = (VolumeVariable*)simulation->getVolVariable(v);
 					VolumeVarContext* varContext = feature->getVolumeVarContext(var);
					if (var->isPde() && !varContext->hasConstantDiffusionAdvection(dimension)) {
						bRegionHasConstantDiffusionAdvection[r] = false;
						bHasVariableDiffusionAdvection = true;
					}
					if (var->isAdvecting()) {
						bHasAdvection = true;
					}
				}
			}
			if (regionDefinedVolVariableSizes[r] > 0) {
				cout << (bRegionHasConstantDiffusionAdvection[r] ? "Constant" : "Variable") << " diffusion/advection in region " << mesh->getVolumeRegion(r)->getName() << endl;
				regionDefinedVolVariableIndexes[r] = new int[regionDefinedVolVariableSizes[r]];
				int activeVarCount = 0;
				for (int v = 0; v < simulation->getNumVolVariables(); v ++) {
					if (simulation->isVolumeVariableDefinedInRegion(v, r)) {
						regionDefinedVolVariableIndexes[r][activeVarCount ++] = v;
					}
				}
			}
		}
	}
	numVolUnknowns = numUnknowns;

	// Membrane ODE/PDE
	memVectorOffset = numUnknowns;
	numUnknowns += mesh->getNumMembraneElements() * simulation->getNumMemVariables();
	// Volume Region
	volRegionVectorOffset = numUnknowns;
	numUnknowns += numVolRegions * simulation->getNumVolRegionVariables();
	// Membrane Region
	memRegionVectorOffset = numUnknowns;
	numUnknowns += mesh->getNumMembraneRegions() * simulation->getNumMemRegionVariables();

	cout << "numUnknowns = " << numUnknowns << endl;
}

void SundialsPdeScheduler::checkCVodeReturnCode(int returnCode) {
	if (returnCode == CV_SUCCESS){
		return;
	}

	switch (returnCode){
		case CV_SUCCESS: {
			throw "CV_SUCCESS: CVode succeeded and no roots were found.";
		}
		case CV_ROOT_RETURN: {
			throw "CV_ROOT_RETURN: CVode succeeded, and found one or more roots. If nrtfn > 1, call CVodeGetRootInfo to see which g_i were found to have a root at (*tret).";
		}
		case CV_TSTOP_RETURN: {
			throw "CV_TSTOP_RETURN: CVode succeeded and returned at tstop.";
		}
		case CV_MEM_NULL:{
			throw "CV_MEM_NULL: mem argument was null";
		}
		case CV_ILL_INPUT:{
			throw "CV_ILL_INPUT: one of the inputs to CVode is illegal";
		}
		case CV_TOO_MUCH_WORK:{
			throw "CV_TOO_MUCH_WORK: took mxstep internal steps but could not reach tout";
		}
		case CV_TOO_MUCH_ACC:{
			throw "CV_TOO_MUCH_ACC: could not satisfy the accuracy demanded by the user for some internal step";
		}
		case CV_ERR_FAILURE:{
			throw "CV_ERR_FAILURE: error test failures occurred too many times during one internal step";
		}
		case CV_CONV_FAILURE:{
			throw "CV_CONV_FAILURE: convergence test failures occurred too many times during one internal step";
		}
		case CV_LINIT_FAIL:{
			throw "CV_LINIT_FAIL: the linear sundialsSolverMemory's initialization function failed.";
		}
		case CV_LSETUP_FAIL:{
			throw "CV_LSETUP_FAIL: the linear sundialsSolverMemory's setup routine failed in an unrecoverable manner.";
		}
		case CV_LSOLVE_FAIL:{
			throw "CV_LSOLVE_FAIL: the linear sundialsSolverMemory's solve routine failed in an unrecoverable manner";
		}
		case CV_REPTD_RHSFUNC_ERR: {
			throw "repeated recoverable right-hand side function errors : ";
		}
		case CV_UNREC_RHSFUNC_ERR:{
			throw "the right-hand side failed in a recoverable manner, but no recovery is possible";
		}
		default:
			throw CVodeGetReturnFlagName(returnCode);
	}
}

int SundialsPdeScheduler::RHS_callback(realtype t, N_Vector yinput, N_Vector rhs, void *fdata) {
	SundialsPdeScheduler* solver = (SundialsPdeScheduler*)fdata;
	double* y_data = NV_DATA_S(yinput);
	double* r_data = NV_DATA_S(rhs);
	return solver->CVodeRHS(t, y_data, r_data);
}

int SundialsPdeScheduler::CVodeRHS(double t, double* yinput, double* rhs) {
#ifdef COMPARE_WITH_OLD
	double* r1 = new double[numUnknowns];
	double* r2 = new double[numUnknowns];
	memset(r1, 0, numUnknowns * sizeof(double));
	memset(r2, 0, numUnknowns * sizeof(double));
	applyVolumeOperatorOld(t, yinput, r1);
	applyVolumeOperator(t, yinput, r2);

	cout << "--comparing with old code at " << t << endl;
	for (int i = 0; i < numUnknowns; i ++) {
		double relerror = r2[i] == 0? fabs(r1[i] - r2[i]) : fabs((r1[i] - r2[i])/r2[i]);
		if (relerror > 1e-5) {
			cout << i << ":" << "r1=" << r1[i] << ", r2=" << r2[i] << endl;
		}
	}
	cout << "--compared with old code" << endl;
	delete[] r1;
	delete[] r2;
#endif
	memset(rhs, 0, numUnknowns * sizeof(double));
	applyVolumeOperator(t, yinput, rhs);
	applyMembraneDiffusionReactionOperator(t, yinput, rhs);
	applyVolumeRegionReactionOperator(t, yinput, rhs);
	applyMembraneRegionReactionOperator(t, yinput, rhs);
	applyMembraneFluxOperator(t, yinput, rhs);

	return 0;
}

void SundialsPdeScheduler::initSundialsSolver() {
	if (!bFirstTime) {
		return;
	}

	int numVariables = simulation->getNumVariables();
	int numVolVar = simulation->getNumVolVariables();
	valueArraySize = 4 + numVolVar * 3 + (numVariables - numVolVar); // t, x, y, z, (U, U_INSIDE, U_OUTSIDE), (M), (VR), (MR)

	volSymbolOffset = 4;
	memSymbolOffset = volSymbolOffset + numVolVar * 3;
	volRegionSymbolOffset = memSymbolOffset + simulation->getNumMemVariables();
	memRegionSymbolOffset = volRegionSymbolOffset + simulation->getNumVolRegionVariables();

	statePointValues = new double[valueArraySize];
	memset(statePointValues, 0, valueArraySize * sizeof(double));

	if (bHasVariableDiffusionAdvection) {
		neighborStatePointValues = new double*[3];
		for (int n = 0; n < 3; n ++) {
			neighborStatePointValues[n] = new double[valueArraySize];
			memset(neighborStatePointValues[n], 0, valueArraySize * sizeof(double));
		}
	}

	currDiscontinuityTimeCount = 0;
	while (numDiscontinuityTimes > 0 && discontinuityTimes[currDiscontinuityTimeCount] < currentTime) {
		currDiscontinuityTimeCount ++;
	}

	int returnCode = 0;
	if (sundialsSolverMemory != 0) {
		return;
	}

	y = N_VNew_Serial(numUnknowns);
	if (y == 0) {
		throw "SundialsPDESolver:: Out of Memory : y ";
	}

	// initialize y with initial conditions
	int count = 0;
	double *y_data = NV_DATA_S(y);
	if (numVolVar > 0) {
		for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
			for (int ri = 0; ri < regionSizes[r]; ri ++) {
				int volIndex = local2Global[ri + regionOffsets[r]];
				for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[r]; activeVarCount ++) {
					int varIndex = regionDefinedVolVariableIndexes[r][activeVarCount];
					y_data[count ++] = simulation->getVolVariable(varIndex)->getCurr(volIndex);
				}
			}
		}
	}
	if (simulation->getNumMemVariables() > 0) {
		for (int mi = 0; mi < mesh->getNumMembraneElements(); mi ++) {
			for (int v = 0; v < simulation->getNumMemVariables(); v ++) {
				y_data[count ++] = simulation->getMemVariable(v)->getCurr(mi);
			}
		}
	}

	if (simulation->getNumVolRegionVariables() > 0) {
		for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
			for (int v = 0; v < simulation->getNumVolRegionVariables(); v ++) {
				y_data[count ++] = simulation->getVolRegionVariable(v)->getCurr(r);
			}
		}
	}

	if (simulation->getNumMemRegionVariables() > 0) {
		for (int r = 0; r < mesh->getNumMembraneRegions(); r ++) {
			for (int v = 0; v < simulation->getNumMemRegionVariables(); v ++) {
				y_data[count ++] = simulation->getMemRegionVariable(v)->getCurr(r);
			}
		}
	}

	preallocateM();
	if (!simulation->hasTimeDependentDiffusionAdvection()) {
		oldGamma = 1.0;
		buildM_Volume(0, 0, oldGamma);
		buildM_Membrane(0, 0, oldGamma);
	}

	sundialsSolverMemory = CVodeCreate(CV_BDF, CV_NEWTON);
	if (sundialsSolverMemory == 0) {
		throw "SundialsPDESolver:: Out of memory : sundialsSolverMemory";
	}

	cout << endl << "----------------------------------" << endl;
	cout << "sundials pde solver is starting from time " << currentTime << endl;
	cout << "----------------------------------" << endl;

	returnCode = CVodeMalloc(sundialsSolverMemory, RHS_callback, currentTime, y, ToleranceType, relTol, &absTol);
	checkCVodeReturnCode(returnCode);

	returnCode = CVodeSetFdata(sundialsSolverMemory, this);
	checkCVodeReturnCode(returnCode);

	// set linear solver
#ifdef SUNDIALS_USE_PCNONE
	cout << endl << "******using Sundials CVode with PREC_NONE, relTol=" << relTol << ", absTol=" << absTol << endl;
	returnCode = CVSpgmr(sundialsSolverMemory, PREC_NONE, 0);
#else
	if (simulation->getNumMemPde() == 0 && simulation->getNumVolPde() == 0) {
		cout << endl << "******ODE only, using Sundials CVode with PREC_NONE, relTol=" << relTol << ", absTol=" << absTol << endl;
		returnCode = CVSpgmr(sundialsSolverMemory, PREC_NONE, 0);
	} else {
		cout << endl << "****** using Sundials CVode with PREC_LEFT, relTol=" << relTol << ", absTol=" << absTol << endl;
		returnCode = CVSpgmr(sundialsSolverMemory, PREC_LEFT, 0);
	}
#endif
	checkCVodeReturnCode(returnCode);

	// set max of number of steps
	returnCode = CVodeSetMaxNumSteps(sundialsSolverMemory, LONG_MAX);
	checkCVodeReturnCode(returnCode);

	// set preconditioner.
	returnCode = CVSpilsSetPreconditioner (sundialsSolverMemory, pcSetup_callback, pcSolve_callback, this);
	checkCVodeReturnCode(returnCode);
}

void SundialsPdeScheduler::printCVodeStats()
{
	long int lenrw, leniw ;
	long int lenrwLS, leniwLS;
	long int nst, nfe, nsetups, nni, ncfn, netf;
	long int nli, npe, nps, ncfl, nfeLS;
	int flag;

	flag = CVodeGetWorkSpace(sundialsSolverMemory, &lenrw, &leniw);
	checkCVodeReturnCode(flag);
	flag = CVodeGetNumSteps(sundialsSolverMemory, &nst);
	checkCVodeReturnCode(flag);
	flag = CVodeGetNumRhsEvals(sundialsSolverMemory, &nfe);
	checkCVodeReturnCode(flag);
	flag = CVodeGetNumLinSolvSetups(sundialsSolverMemory, &nsetups);
	checkCVodeReturnCode(flag);
	flag = CVodeGetNumErrTestFails(sundialsSolverMemory, &netf);
	checkCVodeReturnCode(flag);
	flag = CVodeGetNumNonlinSolvIters(sundialsSolverMemory, &nni);
	checkCVodeReturnCode(flag);
	flag = CVodeGetNumNonlinSolvConvFails(sundialsSolverMemory, &ncfn);
	checkCVodeReturnCode(flag);

	flag = CVSpilsGetWorkSpace(sundialsSolverMemory, &lenrwLS, &leniwLS);
	checkCVodeReturnCode(flag);
	flag = CVSpilsGetNumLinIters(sundialsSolverMemory, &nli);
	checkCVodeReturnCode(flag);
	flag = CVSpilsGetNumPrecEvals(sundialsSolverMemory, &npe);
	checkCVodeReturnCode(flag);
	flag = CVSpilsGetNumPrecSolves(sundialsSolverMemory, &nps);
	checkCVodeReturnCode(flag);
	flag = CVSpilsGetNumConvFails(sundialsSolverMemory, &ncfl);
	checkCVodeReturnCode(flag);
	flag = CVSpilsGetNumRhsEvals(sundialsSolverMemory, &nfeLS);
	checkCVodeReturnCode(flag);

	printf("\nFinal Statistics.. \n\n");
	printf("lenrw   = %5ld     leniw   = %5ld\n", lenrw, leniw);
	printf("lenrwLS = %5ld     leniwLS = %5ld\n", lenrwLS, leniwLS);
	printf("nst     = %5ld\n"                  , nst);
	printf("nfe     = %5ld     nfeLS   = %5ld\n"  , nfe, nfeLS);
	printf("nni     = %5ld     nli     = %5ld\n"  , nni, nli);
	printf("nsetups = %5ld     netf    = %5ld\n"  , nsetups, netf);
	printf("npe     = %5ld     nps     = %5ld\n"  , npe, nps);
	printf("ncfn    = %5ld     ncfl    = %5ld\n\n", ncfn, ncfl);
}

void SundialsPdeScheduler::solve() {
	double endTime = 0;
	if (bSundialsOneStepOutput) {
		endTime = SimTool::getInstance()->getEndTime();
	} else {
		endTime = currentTime + simulation->getDT_sec();
	}

	bool bStop = false;
	double stopTime = endTime;
	// if next stop time between (currTime, endTime]
	if (currDiscontinuityTimeCount < numDiscontinuityTimes && currentTime < discontinuityTimes[currDiscontinuityTimeCount] && (endTime >  discontinuityTimes[currDiscontinuityTimeCount] || fabs(endTime - discontinuityTimes[currDiscontinuityTimeCount]) < epsilon)) {
		stopTime = discontinuityTimes[currDiscontinuityTimeCount];
		bStop = true;
	}

	while (true) {
		if (currentTime > endTime - epsilon) {
			break;
		}
		CVodeSetStopTime(sundialsSolverMemory, stopTime);
		int returnCode = CVode(sundialsSolverMemory, stopTime, y, &currentTime,  bSundialsOneStepOutput ? CV_ONE_STEP_TSTOP : CV_NORMAL_TSTOP);
		if (returnCode != CV_SUCCESS && returnCode != CV_TSTOP_RETURN) {
			checkCVodeReturnCode(returnCode);
		}

		if (bStop && currentTime > stopTime - epsilon) {
			currDiscontinuityTimeCount ++;
			if (currentTime <= SimTool::getInstance()->getEndTime() - epsilon) { // if this is the last solve, we don't have to reinit
				cout << endl << "SundialsPdeScheduler::solve() : cvode reinit at time " << currentTime << endl;
				returnCode = CVodeReInit(sundialsSolverMemory, RHS_callback, currentTime, y, ToleranceType, relTol, &absTol);
				checkCVodeReturnCode(returnCode);
			}

			if (bSundialsOneStepOutput) {
				break;
			}
			// find out next stop time
			bStop = false;
			stopTime = endTime;
			if (currDiscontinuityTimeCount < numDiscontinuityTimes
				&& currentTime < discontinuityTimes[currDiscontinuityTimeCount]
				&& (endTime >  discontinuityTimes[currDiscontinuityTimeCount]
						|| fabs(endTime - discontinuityTimes[currDiscontinuityTimeCount]) < epsilon)) {
				stopTime = discontinuityTimes[currDiscontinuityTimeCount];
				bStop = true;
			}
		}
		if (bSundialsOneStepOutput) {
			break;
		}
	}

	if (currentTime > SimTool::getInstance()->getEndTime() - epsilon) {
		printCVodeStats();
	}
	updateSolutions();
}

void SundialsPdeScheduler::updateSolutions() {
	double *y_data = NV_DATA_S(y);
	int count = 0;

	for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
		bool bDirichletBoundary = mesh->getVolumeRegion(r)->isBoundaryDirichlet();
		for (int ri = 0; ri < regionSizes[r]; ri ++) {
			int volIndex = local2Global[ri + regionOffsets[r]];
			int mask = pVolumeElement[volIndex].neighborMask;

			for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[r]; activeVarCount ++) {
				int varIndex = regionDefinedVolVariableIndexes[r][activeVarCount];
				VolumeVariable *var = simulation->getVolVariable(varIndex);

				double sol = y_data[count ++];
				if (var->isPde() && bDirichletBoundary && (mask & BOUNDARY_TYPE_DIRICHLET)) {
					updateVolumeStatePointValues(volIndex, currentTime, y_data, statePointValues);
					Feature* feature = pVolumeElement[volIndex].feature;
					VolumeVarContext* varContext = feature->getVolumeVarContext(var);
					if ((mask & NEIGHBOR_XM_BOUNDARY) && (feature->getXmBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_XP_BOUNDARY) && (feature->getXpBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_XP_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_YM_BOUNDARY) && (feature->getYmBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_YP_BOUNDARY) && (feature->getYpBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_YP_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_ZM_BOUNDARY) && (feature->getZmBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_ZM_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_ZP_BOUNDARY) && (feature->getZpBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_ZP_EXP, statePointValues);
					}
				}
				var->getCurr()[volIndex] = sol;
			}
		}
	}

	// update membrane
	if (simulation->getNumMemVariables() > 0) {
		for (int m = 0; m < mesh->getNumMembraneElements(); m ++) {
			for (int v = 0; v < simulation->getNumMemVariables(); v ++) {
				MembraneVariable *var = (MembraneVariable*)simulation->getMemVariable(v);
				int mask = mesh->getMembraneNeighborMask(m);
				double sol = y_data[count ++];
				if (var->isPde() && (mask & BOUNDARY_TYPE_DIRICHLET)) {
					updateMembraneStatePointValues(pMembraneElement[m], currentTime, y_data, statePointValues);

					Feature* feature = pMembraneElement[m].feature;
					MembraneVarContext *varContext = feature->getMembraneVarContext(var);
					if ((mask & NEIGHBOR_XM_BOUNDARY) && (feature->getXmBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_XP_BOUNDARY) && (feature->getXpBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_XP_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_YM_BOUNDARY) && (feature->getYmBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_YP_BOUNDARY) && (feature->getYpBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_YP_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_ZM_BOUNDARY) && (feature->getZmBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_ZM_EXP, statePointValues);
					} else if ((mask & NEIGHBOR_ZP_BOUNDARY) && (feature->getZpBoundaryType() == BOUNDARY_VALUE)){
						sol = varContext->evalExpression(BOUNDARY_ZP_EXP, statePointValues);
					}
				}
				var->getCurr()[m] = sol;
			}
		}
	}

	// update volume region variable
	if (simulation->getNumVolRegionVariables() > 0) {
		for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
			for (int v = 0; v < simulation->getNumVolRegionVariables(); v ++) {
				VolumeRegionVariable *var = simulation->getVolRegionVariable(v);
				double sol = y_data[count ++];
				var->getCurr()[r] = sol;
			}
		}
	}
	// update membrane region variable
	if (simulation->getNumMemRegionVariables() > 0) {
		for (int r = 0; r < mesh->getNumMembraneRegions(); r ++) {
			for (int v = 0; v < simulation->getNumMemRegionVariables(); v ++) {
				MembraneRegionVariable *var = simulation->getMemRegionVariable(v);
				double sol = y_data[count ++];
				var->getCurr()[r] = sol;
			}
		}
	}
}

void SundialsPdeScheduler::applyVolumeOperatorOld(double t, double* yinput, double* rhs) {
	short advectDirs[6] = {1, 1, 1, -1, -1, -1};
	int dirichletExpIndexes[6] = {BOUNDARY_ZM_EXP, BOUNDARY_YM_EXP, BOUNDARY_XM_EXP, BOUNDARY_XP_EXP, BOUNDARY_YP_EXP, BOUNDARY_ZP_EXP};
	int velocityExpIndexes[6] = {VELOCITY_Z_EXP, VELOCITY_Y_EXP, VELOCITY_X_EXP, VELOCITY_X_EXP, VELOCITY_Y_EXP, VELOCITY_Z_EXP};

	for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
		int activeVarCount = -1;
		for (int v = 0; v < simulation->getNumVolVariables(); v ++) {
			VolumeVariable* var = simulation->getVolVariable(v);

			if (!simulation->isVolumeVariableDefinedInRegion(v, r)) {
				continue;
			}
			activeVarCount ++;
			int firstPointVolIndex = local2Global[regionOffsets[r]];
			Feature* feature = pVolumeElement[firstPointVolIndex].feature;
			VolumeVarContext* varContext = feature->getVolumeVarContext(var);
			bool bHasConstantDiffusion = false;
			double Di = 0;
			if (varContext->hasConstantDiffusion()) {
				bHasConstantDiffusion = true;
				Di = varContext->getExpressionConstantValue(DIFF_RATE_EXP);
			}

			for (int ri = 0; ri < regionSizes[r]; ri ++) {
				int localIndex = ri + regionOffsets[r];
				int volIndex = local2Global[localIndex];
				int mask = pVolumeElement[volIndex].neighborMask;
				int vectorIndex = getVolumeElementVectorOffset(volIndex, r) + activeVarCount;

				// update values
				if (!var->isPde() || !(mask & BOUNDARY_TYPE_DIRICHLET)){
					updateVolumeStatePointValues(volIndex, t, yinput, statePointValues);
					double reactionRate = varContext->evalExpression(REACT_RATE_EXP, statePointValues);
					// add reaction
					rhs[vectorIndex] += reactionRate;
				}

				if (!var->isPde()) {
					//validateNumber(var->getName(), volIndex, "RHS", rhs[vectorIndex]);
					continue;
				}

				if (mask & BOUNDARY_TYPE_DIRICHLET) {// dirichlet
					rhs[vectorIndex] = 0;
					continue;
				}

				double lambdaX = oneOverH[0]*oneOverH[0];
				double lambdaY = oneOverH[1]*oneOverH[1];
				double lambdaZ = oneOverH[2]*oneOverH[2];
				double lambdaAreaX = oneOverH[0];
				double lambdaAreaY = oneOverH[1];
				double lambdaAreaZ = oneOverH[2];

				if (mask & NEIGHBOR_BOUNDARY_MASK){   // boundary
					if (mask & NEIGHBOR_X_BOUNDARY_MASK){
						lambdaX *= 2.0;
						lambdaAreaX *= 2.0;
					}
					if (mask & NEIGHBOR_Y_BOUNDARY_MASK){
						lambdaY *= 2.0;
						lambdaAreaY *= 2.0;
					}
					if (mask & NEIGHBOR_Z_BOUNDARY_MASK){
						lambdaZ *= 2.0;
						lambdaAreaZ *= 2.0;
					}

					// add neumann condition
					// boundary flux equals -D*DU/DX at the xm and xp walls, -D*DU/DY at the ym and yp walls
					if ((mask & BOUNDARY_TYPE_MASK) == BOUNDARY_TYPE_NEUMANN) {
						// updateVolumeStatePointValues(volIndex, t, yinput);  // this is needed, but update already above, if you move this block, uncomment this statement
						if (mask & NEIGHBOR_XM_BOUNDARY && feature->getXmBoundaryType() == BOUNDARY_FLUX){
							rhs[vectorIndex] += varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues) * lambdaAreaX;
						}
						if (mask & NEIGHBOR_XP_BOUNDARY && feature->getXpBoundaryType() == BOUNDARY_FLUX){
							rhs[vectorIndex] -= varContext->evalExpression(BOUNDARY_XP_EXP, statePointValues) * lambdaAreaX;
						}
						if (dimension > 1) {
							if (mask & NEIGHBOR_YM_BOUNDARY && feature->getYmBoundaryType() == BOUNDARY_FLUX){
								rhs[vectorIndex] += varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues) * lambdaAreaY;
							}
							if (mask & NEIGHBOR_YP_BOUNDARY && feature->getYpBoundaryType() == BOUNDARY_FLUX){
								rhs[vectorIndex] -= varContext->evalExpression(BOUNDARY_YP_EXP, statePointValues) * lambdaAreaY;
							}

							if (dimension > 2) {
								if (mask & NEIGHBOR_ZM_BOUNDARY && feature->getZmBoundaryType() == BOUNDARY_FLUX){
									rhs[vectorIndex] += varContext->evalExpression(BOUNDARY_ZM_EXP, statePointValues) * lambdaAreaZ;
								}
								if (mask & NEIGHBOR_ZP_BOUNDARY && feature->getZpBoundaryType() == BOUNDARY_FLUX){
									rhs[vectorIndex] -= varContext->evalExpression(BOUNDARY_ZP_EXP, statePointValues) * lambdaAreaZ;
								}
							}
						}
					}
				}

				// compute Di, Vi
				if (!bHasConstantDiffusion) {
					Di = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);
				}

				double Vix=0, Viy=0, Viz=0;
				if (var->isAdvecting()) {
					// updateVolumeStatePointValues(volIndex, t, yinput);  // this is needed, but update already above, if you move this block, uncomment this statement
					Vix = varContext->evalExpression(VELOCITY_X_EXP, statePointValues);
					if (dimension > 1) {
						Viy = varContext->evalExpression(VELOCITY_Y_EXP, statePointValues);
					}
					if (dimension > 2) {
						Viz = varContext->evalExpression(VELOCITY_Z_EXP, statePointValues);
					}
				}

				// order is ZM, YM, XM, XP, YP, ZP
				int volumeNeighbors[6] = {
					(dimension < 3 || mask & NEIGHBOR_ZM_MASK) ? -1 : volIndex - Nxy,
					(dimension < 2 || mask & NEIGHBOR_YM_MASK) ? -1 : volIndex - Nx,
					(mask & NEIGHBOR_XM_MASK) ? -1 : volIndex - 1,
					(mask & NEIGHBOR_XP_MASK) ? -1 : volIndex + 1,
					(dimension < 2 || mask & NEIGHBOR_YP_MASK) ? -1 : volIndex + Nx,
					(dimension < 3 || mask & NEIGHBOR_ZP_MASK) ? -1 : volIndex + Nxy
				};

				double neighborLambdas[6] = {lambdaZ, lambdaY, lambdaX, lambdaX, lambdaY, lambdaZ};
				double neighborLambdaAreas[6] = {lambdaAreaZ, lambdaAreaY, lambdaAreaX, lambdaAreaX, lambdaAreaY, lambdaAreaZ};
				double Vis[6] = {Viz, Viy, Vix, Vix, Viy, Viz};

				double Aii = 0;
				// add diffusion and convection
				for (int n = 0; n < 6; n ++) {
					int neighborIndex = volumeNeighbors[n];
					if (neighborIndex < 0) {
						continue;
					}
					int neighborVectorIndex = getVolumeElementVectorOffset(neighborIndex, r) + activeVarCount;
					double lambda = neighborLambdas[n];

					bool bNeighborStatePointValuesComputed = false;
					double D = Di;
					if (!bHasConstantDiffusion) {
						updateVolumeStatePointValues(neighborIndex, t, yinput, statePointValues);
						bNeighborStatePointValuesComputed = true;
						double Dj = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);
						D = (Di + Dj < epsilon) ? (0.0) : (2 * Di * Dj/(Di + Dj));
					}
					double yneighbor = yinput[neighborVectorIndex];
					int neighborMask = pVolumeElement[neighborIndex].neighborMask;
					if (neighborMask & BOUNDARY_TYPE_DIRICHLET) {
						if (!bNeighborStatePointValuesComputed) {
							updateVolumeStatePointValues(neighborIndex, t, yinput, statePointValues);
							bNeighborStatePointValuesComputed = true;
						}
						yneighbor = varContext->evalExpression(dirichletExpIndexes[n], statePointValues);
					}

					double Aij = 0;
					if (var->isAdvecting()) {
						if (!bNeighborStatePointValuesComputed) {
							updateVolumeStatePointValues(neighborIndex, t, yinput, statePointValues);
							bNeighborStatePointValuesComputed = true;
						}
						int advectDir = advectDirs[n];
						double Vi = Vis[n];
						double Vj = varContext->evalExpression(velocityExpIndexes[n], statePointValues);
						double V = 0.5 * (Vi + Vj);
						double lambdaArea = neighborLambdaAreas[n];
						Aij = max(D * lambda + advectDir * 0.5 * V * lambdaArea, max(advectDir * V * lambdaArea, 0));;
						Aii += max(D * lambda - advectDir * 0.5 * V * lambdaArea, max(- advectDir * V * lambdaArea, 0));
					} else {
						Aij = D * lambda;
						Aii += Aij;
					}
					rhs[vectorIndex] += yneighbor * Aij;
				} // end for n
				rhs[vectorIndex] -=  yinput[vectorIndex] * Aii;
				//validateNumber(var->getName(), volIndex, "RHS", rhs[vectorIndex]);
			} // end for ri
		} // end for v
	} // end for r
}

static void applyAdvectionHybridScheme(double diffTerm, double advectTerm, double& Aii, double& Aij) {
	//Aij = max(diffTerm + 0.5 advectTerm, max(advectTerm, 0));
	//Aii = max(diffTerm - 0.5 advectTerm, max(- advectTerm, 0));

	if (diffTerm < 0.5 * fabs(advectTerm)){ // upwind scheme
		Aij = max(advectTerm, 0);
		Aii = max(- advectTerm, 0);
	} else { // central difference scheme
		Aij = diffTerm + 0.5 * advectTerm;
		Aii = diffTerm - 0.5 * advectTerm;
	}
}

void SundialsPdeScheduler::applyVolumeOperator(double t, double* yinput, double* rhs) {
	if (simulation->getNumVolVariables() == 0) {
		return;
	}	
	for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
		if (bRegionHasConstantDiffusionAdvection[r]) {
			regionApplyVolumeOperatorConstant(r, t, yinput, rhs);
		} else {
			regionApplyVolumeOperatorVariable(r, t, yinput, rhs);
		}
	}
}

// for dirichlet point, boundary condition has to be used.
void SundialsPdeScheduler::dirichletPointSetup(int volIndex, Feature* feature, VarContext* varContext, int mask, int* volumeNeighbors, double& ypoint) {
	volumeNeighbors[0] = -1;
	volumeNeighbors[1] = -1;
	volumeNeighbors[2] = -1;
	if (dimension > 2 && (mask & NEIGHBOR_ZM_MASK))  {
		if (!(mask & NEIGHBOR_ZP_MASK)) {
			volumeNeighbors[2] = volIndex + Nxy;
		}
		if (feature->getZmBoundaryType() == BOUNDARY_VALUE) {
			ypoint = varContext->evalExpression(BOUNDARY_ZM_EXP,  statePointValues);
		}
	}
	if (dimension > 1 && (mask & NEIGHBOR_YM_MASK))  {
		if (!(mask & NEIGHBOR_YP_MASK)) {
			volumeNeighbors[1] = volIndex + Nx;
		}
		if (feature->getYmBoundaryType() == BOUNDARY_VALUE) {
			ypoint = varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues);
		}
	}
	if (mask & NEIGHBOR_XM_MASK)  {
		// make sure we take care of mixed bc (e.g. neumann/dirichlet)
		// then only xp is a neighbor
		if (!(mask & NEIGHBOR_XP_MASK)) {
			volumeNeighbors[0] = volIndex + 1;
		}
		if (feature->getXmBoundaryType() == BOUNDARY_VALUE) {
			ypoint = varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues);
		}
	}
}

// boundary flux equals -D*DU/DX at the xm and xp walls, -D*DU/DY at the ym and yp walls
double SundialsPdeScheduler::computeNeumannCondition(Feature* feature, VarContext* varContext, int mask, double* scaleSs) {
	double boundaryCondition = 0;
	if (mask & NEIGHBOR_XM_BOUNDARY && feature->getXmBoundaryType() == BOUNDARY_FLUX){
		boundaryCondition += varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues) * oneOverH[0] * scaleSs[0];
	}
	if (mask & NEIGHBOR_XP_BOUNDARY && feature->getXpBoundaryType() == BOUNDARY_FLUX){
		boundaryCondition -= varContext->evalExpression(BOUNDARY_XP_EXP, statePointValues) * oneOverH[0] * scaleSs[0];
	}
	if (dimension > 1) {
		if (mask & NEIGHBOR_YM_BOUNDARY && feature->getYmBoundaryType() == BOUNDARY_FLUX){
			boundaryCondition += varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues) * oneOverH[1] * scaleSs[1];
		}
		if (mask & NEIGHBOR_YP_BOUNDARY && feature->getYpBoundaryType() == BOUNDARY_FLUX){
			boundaryCondition -= varContext->evalExpression(BOUNDARY_YP_EXP, statePointValues) * oneOverH[1] * scaleSs[1];
		}

		if (dimension > 2) {
			if (mask & NEIGHBOR_ZM_BOUNDARY && feature->getZmBoundaryType() == BOUNDARY_FLUX){
				boundaryCondition += varContext->evalExpression(BOUNDARY_ZM_EXP, statePointValues) * oneOverH[2] * scaleSs[2];
			}
			if (mask & NEIGHBOR_ZP_BOUNDARY && feature->getZpBoundaryType() == BOUNDARY_FLUX){
				boundaryCondition -= varContext->evalExpression(BOUNDARY_ZP_EXP, statePointValues) * oneOverH[2] * scaleSs[2];
			}
		}
	}
	return boundaryCondition;
}

// scale cross sectional area scale
void computeScaleS(int mask, double* scaleS) {
	if (mask & NEIGHBOR_X_BOUNDARY_MASK){
		scaleS[1] /= 2; // Y
		scaleS[2] /= 2; // Z
	}
	if (mask & NEIGHBOR_Y_BOUNDARY_MASK){
		scaleS[0] /= 2; // X
		scaleS[2] /= 2; // Z
	}
	if (mask & NEIGHBOR_Z_BOUNDARY_MASK){
		scaleS[0] /= 2; // X
		scaleS[1] /= 2; // Y
	}
}

// order is XP, YP, ZP
#define defineVolumeNeighbors(volIndex, mask) int volumeNeighbors[3] = { \
									(mask & NEIGHBOR_XP_MASK) ? -1 : volIndex + 1, \
									(dimension < 2 || mask & NEIGHBOR_YP_MASK) ? -1 : volIndex + Nx, \
									(dimension < 3 || mask & NEIGHBOR_ZP_MASK) ? -1 : volIndex + Nxy \
								};


// now we update volume flux symmetrically because D*(Uj - Ui)*S/d is the same (opposite sign) for point i and j.
// but what we computed is D*(Uj - Ui)*S/(d * dV) which will be the same for both too, but at the end, we have to
// scale it with volume scale. So now we only have to process neighbor points whose indexes are bigger.
// because we have to scale the flux, we have to make sure
// 1. we process points from small global index to big global index (we checked this)
// 2. we always add reaction at the end because reaction term doesn't have dV (otherwise we need another loop to process reaction).

void SundialsPdeScheduler::regionApplyVolumeOperatorConstant(int regionID, double t, double* yinput, double* rhs) {
	if (regionDefinedVolVariableSizes[regionID] == 0) {
		return;
	}

	int firstPointVolIndex = local2Global[regionOffsets[regionID]];
	Feature* feature = pVolumeElement[firstPointVolIndex].feature;
	double (*advectTerms)[6] = 0;
	if (bHasAdvection) {
		advectTerms = new double[regionDefinedVolVariableSizes[regionID]][6];
		memset(advectTerms, 0, regionDefinedVolVariableSizes[regionID] * 6 * sizeof(double));

		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[regionID]; activeVarCount ++) {
			int varIndex = regionDefinedVolVariableIndexes[regionID][activeVarCount];
			VolumeVariable* var = simulation->getVolVariable(varIndex);

			if (var->isAdvecting()) {

				VolumeVarContext* varContext = feature->getVolumeVarContext(var);
				double D = varContext->getExpressionConstantValue(DIFF_RATE_EXP);

				double Vi_XYZ[3] = {0, 0, 0};
				Vi_XYZ[0] = varContext->getExpressionConstantValue(VELOCITY_X_EXP);
				if (dimension > 1) {
					Vi_XYZ[1] = varContext->getExpressionConstantValue(VELOCITY_Y_EXP);
				}
				if (dimension > 2) {
					Vi_XYZ[2] = varContext->getExpressionConstantValue(VELOCITY_Z_EXP);
				}
				
				for (int n = 0; n < 3; n ++) {
					double diffTerm = D * oneOverH[n];
					double diffAdvectTerm = 0;
					double V = Vi_XYZ[n];
					double advectTerm = -V;

					double Aii = 0, Aij = 0;
					applyAdvectionHybridScheme(diffTerm, advectTerm, Aii, Aij);
					advectTerms[activeVarCount][n*2] = Aii;
					advectTerms[activeVarCount][n*2 + 1] = Aij;
				} // end for n
			}
		}
	}

	// loop through points
	for (int regionPointIndex = 0; regionPointIndex < regionSizes[regionID]; regionPointIndex ++) {
		int localIndex = regionPointIndex + regionOffsets[regionID];
		int volIndex = local2Global[localIndex];
		int mask = pVolumeElement[volIndex].neighborMask;

		bool bDirichlet = false;
		double scaleS[3] = {1, 1, 1};
		if (mask & NEIGHBOR_BOUNDARY_MASK) {
			if (mask & BOUNDARY_TYPE_DIRICHLET) {// dirichlet
				bDirichlet = true;
			}

			computeScaleS(mask, scaleS);
		}

		// Macro
		defineVolumeNeighbors(volIndex, mask)

		// update values for this point
		updateVolumeStatePointValues(volIndex, t, yinput, statePointValues);

		int vectorIndexOffset = getVolumeElementVectorOffset(volIndex, regionID);

		// loop through defined variables
		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[regionID]; activeVarCount ++) {
			// I can also increment this, but
			int vectorIndex = vectorIndexOffset + activeVarCount;

			int varIndex = regionDefinedVolVariableIndexes[regionID][activeVarCount];
			VolumeVariable* var = (VolumeVariable*)simulation->getVolVariable(varIndex);
			VolumeVarContext* varContext = feature->getVolumeVarContext(var);

			double reactionRate = 0;
			if (bDirichlet && var->isPde()) {// pde dirichlet
				rhs[vectorIndex] = 0;
				if (mask & (NEIGHBOR_XP_BOUNDARY || NEIGHBOR_YP_BOUNDARY || NEIGHBOR_ZP_BOUNDARY)) {
					continue;
				}
			} else {
				reactionRate = varContext->evalExpression(REACT_RATE_EXP, statePointValues);
				if (!var->isPde()) {
					rhs[vectorIndex] += reactionRate;
					continue;
				}
			}

			double boundaryCondition = 0;
			if (!bDirichlet && (mask & NEIGHBOR_BOUNDARY_MASK)) {   // neumann boundary condition				
				if ((mask & BOUNDARY_TYPE_MASK) == BOUNDARY_TYPE_NEUMANN) {
					boundaryCondition = computeNeumannCondition(feature, varContext, mask, scaleS);
				}
			}

			double D = varContext->getExpressionConstantValue(DIFF_RATE_EXP);

			double ypoint = yinput[vectorIndex];
			if (bDirichlet) {
				dirichletPointSetup(volIndex, feature, varContext, mask, volumeNeighbors, ypoint);
			}

			// add diffusion and convection
			for (int n = 0; n < 3; n ++) {
				int neighborIndex = volumeNeighbors[n];
				if (neighborIndex < 0) {
					continue;
				}

				int neighborVectorIndex = getVolumeElementVectorOffset(neighborIndex, regionID) + activeVarCount;
				double yneighbor = yinput[neighborVectorIndex];

				//use the real value if the neighbor is dirichlet point
				int neighborMask = pVolumeElement[neighborIndex].neighborMask;
				bool bNeighborDirichlet = false;
				if (neighborMask & BOUNDARY_TYPE_DIRICHLET) {
					bNeighborDirichlet = true;
					updateVolumeStatePointValues(neighborIndex, t, 0, txyzValues);
					yneighbor = varContext->evalExpression(dirichletExpIndexes[n], txyzValues);
				}

				double diffAdvectTerm = 0;
				if (var->isAdvecting()) {
					diffAdvectTerm = yneighbor * advectTerms[activeVarCount][n * 2 + 1] - ypoint * advectTerms[activeVarCount][n * 2];
				} else {
					diffAdvectTerm = (yneighbor - ypoint) * D * oneOverH[n];
				}
				diffAdvectTerm *= scaleS[n] * oneOverH[n];
				if (!bDirichlet) {
					rhs[vectorIndex] += diffAdvectTerm;
				}
				if (!bNeighborDirichlet) {
					rhs[neighborVectorIndex] -= diffAdvectTerm;
				}
			} // end for n
			if (bDirichlet) {
				rhs[vectorIndex] = 0;
			} else {
				// add boundary conditon
				rhs[vectorIndex] += boundaryCondition;				
				if (mask & NEIGHBOR_BOUNDARY_MASK){
					// apply volume scale
					rhs[vectorIndex] *= (mask&VOLUME_MASK);
				}
				// add reaction
				rhs[vectorIndex] += reactionRate;
			}
		} // end for v
	} // end for ri
	delete[] advectTerms;
}

void SundialsPdeScheduler::regionApplyVolumeOperatorVariable(int regionID, double t, double* yinput, double* rhs) {
	if (regionDefinedVolVariableSizes[regionID] == 0) {
		return;
	}

	int firstPointVolIndex = local2Global[regionOffsets[regionID]];
	Feature* feature = pVolumeElement[firstPointVolIndex].feature;

	// loop through points
	for (int regionPointIndex = 0; regionPointIndex < regionSizes[regionID]; regionPointIndex ++) {
		int localIndex = regionPointIndex + regionOffsets[regionID];
		int volIndex = local2Global[localIndex];
		int mask = pVolumeElement[volIndex].neighborMask;

		bool bDirichlet = false;
		double scaleS[3] = {1, 1, 1};
		if (mask & NEIGHBOR_BOUNDARY_MASK) {
			if (mask & BOUNDARY_TYPE_DIRICHLET) {// dirichlet
				bDirichlet = true;
			}

			computeScaleS(mask, scaleS);
		}

		// Macro
		defineVolumeNeighbors(volIndex, mask)

		// update values for this point
		updateVolumeStatePointValues(volIndex, t, yinput, statePointValues);
		
		// update values for neighbor points
		for (int n = 0; n < 3; n ++) {
			int neighborIndex = volumeNeighbors[n];
			if (neighborIndex < 0) {
				continue;
			}
			updateVolumeStatePointValues(neighborIndex, t, yinput, neighborStatePointValues[n]);
		}

		int vectorIndexOffset = getVolumeElementVectorOffset(volIndex, regionID);

		// loop through defined variables
		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[regionID]; activeVarCount ++) {
			// I can also increment this, but
			int vectorIndex = vectorIndexOffset + activeVarCount;

			int varIndex = regionDefinedVolVariableIndexes[regionID][activeVarCount];
			VolumeVariable* var = simulation->getVolVariable(varIndex);
			VolumeVarContext* varContext = feature->getVolumeVarContext(var);

			double reactionRate = 0;
			if (bDirichlet && var->isPde()) {// pde dirichlet
				rhs[vectorIndex] = 0;
				if (mask & (NEIGHBOR_XP_BOUNDARY || NEIGHBOR_YP_BOUNDARY || NEIGHBOR_ZP_BOUNDARY)) {
					continue;
				}
			} else {
				reactionRate = varContext->evalExpression(REACT_RATE_EXP, statePointValues);
				if (!var->isPde()) {
					rhs[vectorIndex] += reactionRate;
					continue;
				}
			}

			double boundaryCondition = 0;
			if (!bDirichlet && (mask & NEIGHBOR_BOUNDARY_MASK)) {   // neumann boundary condition				
				if ((mask & BOUNDARY_TYPE_MASK) == BOUNDARY_TYPE_NEUMANN) {
					boundaryCondition = computeNeumannCondition(feature, varContext, mask, scaleS);
				}
			}

			bool bHasConstantDiffusion = false;
			double Di = 0;
			if (varContext->hasConstantDiffusion()) {
				bHasConstantDiffusion = true;
				Di = varContext->getExpressionConstantValue(DIFF_RATE_EXP);
			} else {
				Di = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);
			}

			double Vi_XYZ[3] = {0, 0, 0};
			if (var->isAdvecting()) {
				Vi_XYZ[0] = varContext->evalExpression(VELOCITY_X_EXP, statePointValues);
				if (dimension > 1) {
					Vi_XYZ[1] = varContext->evalExpression(VELOCITY_Y_EXP, statePointValues);
				}
				if (dimension > 2) {
					Vi_XYZ[2] = varContext->evalExpression(VELOCITY_Z_EXP, statePointValues);
				}
			}			

			double ypoint = yinput[vectorIndex];
			if (bDirichlet) {
				dirichletPointSetup(volIndex, feature, varContext, mask, volumeNeighbors, ypoint);
			}

			// add diffusion and convection
			for (int n = 0; n < 3; n ++) {
				int neighborIndex = volumeNeighbors[n];
				if (neighborIndex < 0) {
					continue;
				}

				double D = Di;
				if (!bHasConstantDiffusion) {
					double Dj = varContext->evalExpression(DIFF_RATE_EXP, neighborStatePointValues[n]);
					D = (Di + Dj < epsilon) ? (0.0) : (2 * Di * Dj/(Di + Dj));
				}

				int neighborVectorIndex = getVolumeElementVectorOffset(neighborIndex, regionID) + activeVarCount;
				double yneighbor = yinput[neighborVectorIndex];

				//use the real value if the neighbor is dirichlet point
				int neighborMask = pVolumeElement[neighborIndex].neighborMask;
				bool bNeighborDirichlet = false;
				if (neighborMask & BOUNDARY_TYPE_DIRICHLET) {
					bNeighborDirichlet = true;
					yneighbor = varContext->evalExpression(dirichletExpIndexes[n], neighborStatePointValues[n]);
				}

				double diffTerm = D * oneOverH[n];
				double diffAdvectTerm = 0;
				if (var->isAdvecting()) {
					double Vi = Vi_XYZ[n];
					double Vj = varContext->evalExpression(velocityExpIndexes[n], neighborStatePointValues[n]);
					double V = 0.5 * (Vi + Vj);
					double advectTerm = -V;

					double Aii = 0, Aij = 0;
					applyAdvectionHybridScheme(diffTerm, advectTerm, Aii, Aij);
					diffAdvectTerm = yneighbor * Aij - ypoint * Aii;
				} else {
					diffAdvectTerm = (yneighbor - ypoint) * diffTerm;
				}
				diffAdvectTerm *= scaleS[n] * oneOverH[n];
				if (!bDirichlet) {
					rhs[vectorIndex] += diffAdvectTerm;
				}
				if (!bNeighborDirichlet) {
					rhs[neighborVectorIndex] -= diffAdvectTerm;
				}
			} // end for n
			if (bDirichlet) {
				rhs[vectorIndex] = 0;
			} else {
				// add boundary conditon
				rhs[vectorIndex] += boundaryCondition;				
				if (mask & NEIGHBOR_BOUNDARY_MASK){
					// apply volume scale
					rhs[vectorIndex] *= (mask&VOLUME_MASK);
				}
				// add reaction
				rhs[vectorIndex] += reactionRate;
			}
		} // end for v
	} // end for ri
}

void SundialsPdeScheduler::applyMembraneDiffusionReactionOperator(double t, double* yinput, double* rhs) {
	if (simulation->getNumMemVariables() == 0) {
		return;
	}

	SparseMatrixPCG* membraneElementCoupling = mesh->getMembraneCoupling();
	for (int mi = 0; mi < mesh->getNumMembraneElements(); mi ++) {
		for (int v = 0; v < simulation->getNumMemVariables(); v ++) {
			int vectorIndex = getMembraneElementVectorOffset(mi) + v;

			MembraneVariable* var = (MembraneVariable*)simulation->getMemVariable(v);
			Feature* feature = pMembraneElement[mi].feature;
			MembraneVarContext *varContext = feature->getMembraneVarContext(var);
			int mask = mesh->getMembraneNeighborMask(mi);

			if (!var->isPde() || !(mask & BOUNDARY_TYPE_DIRICHLET)) {   // boundary and dirichlet
				// update values
				updateMembraneStatePointValues(pMembraneElement[mi], t, yinput, statePointValues);
				double reactionRate = varContext->evalExpression(REACT_RATE_EXP, statePointValues);
				// add reaction
				rhs[vectorIndex] += reactionRate;
			}

			if (!var->isPde()) {
				//validateNumber(var->getName(), volIndex, "RHS", rhs[vectorIndex]);
				continue;
			}

			if (mask & BOUNDARY_TYPE_DIRICHLET) {   // boundary and dirichlet
				rhs[vectorIndex] = 0;
				continue;
			}

			double Di = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);
			double volume = membraneElementCoupling->getValue(mi, mi);
			if (mask & BOUNDARY_TYPE_NEUMANN) { // boundary and neumann
				if (dimension == 2) { // all the flux area is 1
					if (mask & NEIGHBOR_XM_BOUNDARY){
						rhs[vectorIndex] += varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues)/volume;
					}
					if (mask & NEIGHBOR_XP_BOUNDARY){
						rhs[vectorIndex] -= varContext->evalExpression(BOUNDARY_XP_EXP, statePointValues)/volume;
					}
					if (mask & NEIGHBOR_YM_BOUNDARY){
						rhs[vectorIndex] += varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues)/volume;
					}
					if (mask & NEIGHBOR_YP_BOUNDARY){
						rhs[vectorIndex] -= varContext->evalExpression(BOUNDARY_YP_EXP, statePointValues)/volume;
					}
				} else if (dimension == 3) {
					double* boundaryFluxArea = mesh->getMembraneFluxArea(mi);

					if (mask & NEIGHBOR_XM_BOUNDARY){
						rhs[vectorIndex] += varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues) * boundaryFluxArea[BL_Xm]/volume;
					}
					if (mask & NEIGHBOR_XP_BOUNDARY){
						rhs[vectorIndex] -= varContext->evalExpression(BOUNDARY_XP_EXP, statePointValues) * boundaryFluxArea[BL_Xp]/volume;
					}
					if (mask & NEIGHBOR_YM_BOUNDARY){
						rhs[vectorIndex] += varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues) * boundaryFluxArea[BL_Ym]/volume;
					}
					if (mask & NEIGHBOR_YP_BOUNDARY){
						rhs[vectorIndex] -= varContext->evalExpression(BOUNDARY_YP_EXP, statePointValues) * boundaryFluxArea[BL_Yp]/volume;
					}
					if (mask & NEIGHBOR_ZM_BOUNDARY){
						rhs[vectorIndex] += varContext->evalExpression(BOUNDARY_ZM_EXP, statePointValues) * boundaryFluxArea[BL_Zm]/volume;
					}
					if (mask & NEIGHBOR_ZP_BOUNDARY){
						rhs[vectorIndex] -= varContext->evalExpression(BOUNDARY_ZP_EXP, statePointValues) * boundaryFluxArea[BL_Zp]/volume;
					}
				}
			}

			int32* membraneNeighbors;
			double* s_over_d;
			int numMembraneNeighbors = membraneElementCoupling->getColumns(mi, membraneNeighbors, s_over_d);

			double Aii = 0;
			for (long j = 0; j < numMembraneNeighbors; j ++) {
				int32 neighborIndex = membraneNeighbors[j];
				int neighborVectorIndex = getMembraneElementVectorOffset(neighborIndex) + v;

				updateMembraneStatePointValues(pMembraneElement[neighborIndex], t, yinput, statePointValues);
				double Dj = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);
				double D = (Di + Dj < epsilon)?(0.0):(2 * Di * Dj/(Di + Dj));

				double yneighbor = yinput[neighborVectorIndex];
				int neighborMask = mesh->getMembraneNeighborMask(neighborIndex);
				if (neighborMask & BOUNDARY_TYPE_DIRICHLET) {
					if ((neighborMask & NEIGHBOR_XM_BOUNDARY) && (feature->getXmBoundaryType() == BOUNDARY_VALUE)){
						yneighbor = varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues);
					}else if ((neighborMask & NEIGHBOR_XP_BOUNDARY) && (feature->getXpBoundaryType() == BOUNDARY_VALUE)){
						yneighbor = varContext->evalExpression(BOUNDARY_XP_EXP, statePointValues);
					} else if ((neighborMask & NEIGHBOR_YM_BOUNDARY) && (feature->getYmBoundaryType() == BOUNDARY_VALUE)){
						yneighbor = varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues);
					}else if ((neighborMask & NEIGHBOR_YP_BOUNDARY) && (feature->getYpBoundaryType() == BOUNDARY_VALUE)){
						yneighbor = varContext->evalExpression(BOUNDARY_YP_EXP, statePointValues);
					}else if ((neighborMask & NEIGHBOR_ZM_BOUNDARY) && (feature->getZmBoundaryType() == BOUNDARY_VALUE)){
						yneighbor = varContext->evalExpression(BOUNDARY_ZM_EXP, statePointValues);
					}else if ((neighborMask & NEIGHBOR_ZP_BOUNDARY) && (feature->getZpBoundaryType() == BOUNDARY_VALUE)){
						yneighbor = varContext->evalExpression(BOUNDARY_ZP_EXP, statePointValues);
					}
				}
				double Aij = D * s_over_d[j] / volume;
				Aii += Aij;
				rhs[vectorIndex] += yneighbor * Aij;
			}
			rhs[vectorIndex] -=  yinput[vectorIndex] * Aii;
		}
	}
}

void SundialsPdeScheduler::applyVolumeRegionReactionOperator(double t, double* yinput, double* rhs) {
	if (simulation->getNumVolRegionVariables() == 0) {
		return;
	}

	for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
		VolumeRegion* volRegion = mesh->getVolumeRegion(r);
		Feature* feature = volRegion->getFeature();
		for (int v = 0; v < simulation->getNumVolRegionVariables(); v ++) {
			VolumeRegionVariable* var = simulation->getVolRegionVariable(v);
			VolumeRegionVarContext* volRegionVarContext = feature->getVolumeRegionVarContext(var);
			int vectorIndex = getVolumeRegionVectorOffset(r) + v;

			updateRegionStatePointValues(r, t, yinput, true, statePointValues);
			rhs[vectorIndex] = volRegionVarContext->evalExpression(UNIFORM_RATE_EXP, statePointValues);

			double volume = volRegion->getVolume();
			int numElements = volRegion->getNumElements();
			double volumeIntegral = 0.0;
			for(int j = 0; j < numElements; j ++){
				int volIndex = volRegion->getIndex(j);
				updateVolumeStatePointValues(volIndex, t, yinput, statePointValues);
				volumeIntegral += volRegionVarContext->evalExpression(REACT_RATE_EXP, statePointValues) * mesh->getVolumeOfElement_cu(volIndex);
			}
			rhs[vectorIndex] += volumeIntegral/volume;

			int numMembraneRegions = volRegion->getNumMembraneRegions();
			double surfaceIntegral = 0.0;
			for(int k = 0; k < numMembraneRegions; k ++){
				MembraneRegion *memRegion = volRegion->getMembraneRegion(k);
				numElements = memRegion->getNumElements();
				for(int j = 0; j < numElements; j ++){
					int memIndex = memRegion->getIndex(j);
					updateMembraneStatePointValues(pMembraneElement[memIndex], t, yinput, statePointValues);
					surfaceIntegral += volRegionVarContext->evalExpression(IN_FLUX_EXP, statePointValues) * pMembraneElement[memIndex].area;
				}
			}
			rhs[vectorIndex] += surfaceIntegral/volume;
		}
	}
}

void SundialsPdeScheduler::applyMembraneRegionReactionOperator(double t, double* yinput, double* rhs) {
	if (simulation->getNumMemRegionVariables() == 0) {
		return;
	}
	for (int r = 0; r < mesh->getNumMembraneRegions(); r ++) {
		MembraneRegion *memRegion = mesh->getMembraneRegion(r);
		Feature* feature = memRegion->getRegionInside()->getFeature();
		for (int v = 0; v < simulation->getNumMemRegionVariables(); v ++) {
			MembraneRegionVariable* var = simulation->getMemRegionVariable(v);
			MembraneRegionVarContext * memRegionvarContext = feature->getMembraneRegionVarContext(var);
			int vectorIndex = getMembraneRegionVectorOffset(r) + v;

			if (memRegion->getRegionInside()->isClosed()) {
				updateRegionStatePointValues(r, t, yinput, false, statePointValues);
				rhs[vectorIndex] = memRegionvarContext->evalExpression(UNIFORM_RATE_EXP, statePointValues);

				double surface = memRegion->getSurface();
				long numElements = memRegion->getNumElements();
				double surfaceIntegral = 0.0;
				for(int j = 0; j < numElements; j ++) {
					int memIndex = memRegion->getIndex(j);
					updateMembraneStatePointValues(pMembraneElement[memIndex], t, yinput, statePointValues);
					surfaceIntegral += memRegionvarContext->evalExpression(REACT_RATE_EXP, statePointValues) * pMembraneElement[memIndex].area;
				}
				rhs[vectorIndex] += surfaceIntegral/surface;
			} else {
				throw "MembraneRegionEqnBuilder::buildEquation(), only implemented for closed surface, please check geometry";
			}
		}
	}
}

void SundialsPdeScheduler::applyMembraneFluxOperator(double t, double* yinput, double* rhs) {
	if (simulation->getNumVolPde() == 0) {
		return;
	}

	for (int m = 0; m < mesh->getNumMembraneElements(); m ++) {
		MembraneElement& me = pMembraneElement[m];

		int insideRegionID = pVolumeElement[me.insideIndexNear].region->getId();
		int outsideRegionID = pVolumeElement[me.outsideIndexNear].region->getId();

		double insideVolume = mesh->getVolumeOfElement_cu(me.insideIndexNear);
		double outsideVolume = mesh->getVolumeOfElement_cu(me.outsideIndexNear);

		// vector index of inside near and outside near
		int vi2 = getVolumeElementVectorOffset(me.insideIndexNear, insideRegionID);
		int vi3 = getVolumeElementVectorOffset(me.outsideIndexNear, outsideRegionID);

		updateMembraneStatePointValues(me, t, yinput, statePointValues);

		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[insideRegionID]; activeVarCount ++) {
			int varIndex = regionDefinedVolVariableIndexes[insideRegionID][activeVarCount];
			VolumeVariable* var = simulation->getVolVariable(varIndex);
			if (!var->isPde()) {
				continue;
			}

			VolumeVarContext* anotherVarContext = me.feature->getVolumeVarContext(var);
			double inFlux = anotherVarContext->evalExpression(IN_FLUX_EXP, statePointValues);
			rhs[vi2 + activeVarCount] += inFlux * me.area / insideVolume;
			//validateNumber(var->getName(), me.insideIndexNear, "Membrane Flux", rhs[vi2 + activeVarCountInside]);
		}
		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[outsideRegionID]; activeVarCount ++) {
			int varIndex = regionDefinedVolVariableIndexes[outsideRegionID][activeVarCount];
			VolumeVariable* var = simulation->getVolVariable(varIndex);
			if (!var->isPde()) {
				continue;
			}

			VolumeVarContext* anotherVarContext = me.feature->getVolumeVarContext(var);
			double outFlux = anotherVarContext->evalExpression(OUT_FLUX_EXP, statePointValues);
			rhs[vi3 + activeVarCount] += outFlux * me.area / outsideVolume;
			//validateNumber(var->getName(), me.outsideIndexNear, "Membrane Flux", rhs[vi3 + activeVarCountOutside]);
		}
	}
}

int SundialsPdeScheduler::pcSetup_callback(realtype t, N_Vector y, N_Vector fy, booleantype jok, booleantype *jcurPtr, realtype gamma,
                   void *P_data, N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
	SundialsPdeScheduler* solver = (SundialsPdeScheduler*)P_data;
	return solver->pcSetup(t, y, fy, jok, jcurPtr, gamma);
}

int SundialsPdeScheduler::pcSolve_callback(realtype t, N_Vector y, N_Vector fy, N_Vector r, N_Vector z, realtype gamma, realtype delta,
                  int lr, void *P_data, N_Vector tmp) {
	SundialsPdeScheduler* solver = (SundialsPdeScheduler*)P_data;
	return solver->pcSolve(t, y, fy, r, z, gamma, delta, lr);
}

// M approximates I - gamma*J
void SundialsPdeScheduler::preallocateM() {
	int GENERAL_MAX_NONZERO_PERROW[4] = {0, 3, 5, 7};

	// initialize a sparse matrix for M
	int numNonZeros = GENERAL_MAX_NONZERO_PERROW[dimension] * numUnknowns;
	if (simulation->getNumMemPde() > 0 && dimension == 3) {
		numNonZeros = GENERAL_MAX_NONZERO_PERROW[dimension] * numVolUnknowns   // volume variables 
			+ 20 * mesh->getNumMembraneElements() * simulation->getNumMemVariables()  // membrane variable
			+ mesh->getNumVolumeRegions() * simulation->getNumVolRegionVariables() // vol region variable
			+ mesh->getNumMembraneRegions() * simulation->getNumMemRegionVariables(); // mem region variable
	}
	if (dimension == 1) {
		nsp = numNonZeros * 20;
	} else {
		nsp = numNonZeros * 8;
	}
	try {
		delete[] pcg_workspace;
		pcg_workspace = new double[nsp];
	} catch (...) {
		throw "SundialsPDESolver:: Out of Memory : pcg_workspace";
	}
	memset(pcg_workspace, 0, nsp * sizeof(double));

	M = new SparseMatrixPCG(numUnknowns, numNonZeros, MATRIX_GENERAL); // only store upper triangle

	int colCount;
	int32 columns[20];

	if (simulation->getNumVolVariables() > 0) {
		for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
			if (regionDefinedVolVariableSizes[r] == 0) {
				continue;
			}

			for (int ri = 0; ri < regionSizes[r]; ri ++) {
				int localIndex = ri + regionOffsets[r];
				int volIndex = local2Global[localIndex];
				int mask = pVolumeElement[volIndex].neighborMask;

				int volumeNeighbors[6] = {
					(dimension < 3 || mask & NEIGHBOR_ZM_MASK) ? -1 : volIndex - Nxy,
					(dimension < 2 || mask & NEIGHBOR_YM_MASK) ? -1 : volIndex - Nx,
					(mask & NEIGHBOR_XM_MASK) ? -1 : volIndex - 1,
					(mask & NEIGHBOR_XP_MASK) ? -1 : volIndex + 1,
					(dimension < 2 || mask & NEIGHBOR_YP_MASK) ? -1 : volIndex + Nx,
					(dimension < 3 || mask & NEIGHBOR_ZP_MASK) ? -1 : volIndex + Nxy
				};

				for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[r]; activeVarCount ++) {
					int varIndex = regionDefinedVolVariableIndexes[r][activeVarCount];
					VolumeVariable* var = (VolumeVariable*)simulation->getVolVariable(varIndex);

					M->addNewRow();

					if (!var->isPde()) {
						M->setRow(1.0, 0, 0, 0);
						//validateNumber(var->getName(), volIndex, "RHS", rhs[vectorIndex]);
						continue;
					}

					if (mask & BOUNDARY_TYPE_DIRICHLET) {// dirichlet
						M->setRow(1.0, 0, 0, 0);
						continue;
					}

					colCount = 0;
					// add diffusion and convection
					for (int n = 0; n < 6; n ++) {
						int neighborIndex = volumeNeighbors[n];
						if (neighborIndex < 0) {
							continue;
						}
						int neighborRi= global2Local[neighborIndex] - regionOffsets[r];
						int neighborVectorIndex = getVolumeElementVectorOffset(neighborIndex, r) + activeVarCount;
						columns[colCount] = neighborVectorIndex;
						colCount ++;
					} // end for n
					M->setRow(1.0, colCount, columns, 0);
				} // end for vindex
			} // end for ri
		} // end for r
	}

	// membrane variable
	if (simulation->getNumMemVariables() > 0) {
		SparseMatrixPCG* membraneElementCoupling = mesh->getMembraneCoupling();

		int columns[20];
		for (int mi = 0; mi < mesh->getNumMembraneElements(); mi ++) {
			for (int v = 0; v < simulation->getNumMemVariables(); v ++) {
				M->addNewRow();

				if (!simulation->getMemVariable(v)->isPde()) {
					M->setRow(1.0, 0, 0, 0);
					continue;
				}

				int32* membraneNeighbors;
				double* s_over_d;
				int numMembraneNeighbors = membraneElementCoupling->getColumns(mi, membraneNeighbors, s_over_d);
				for (long j = 0; j < numMembraneNeighbors; j ++) {
					int32 neighborIndex = membraneNeighbors[j];
					int neighborVectorIndex = getMembraneElementVectorOffset(neighborIndex) + v;
					columns[j] = neighborVectorIndex;
				}
				M->setRow(1.0, numMembraneNeighbors, columns, 0);
			}
		}
	}
	// volume region variable
	if (simulation->getNumVolRegionVariables() != 0) {
		for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
			for (int v = 0; v < simulation->getNumVolRegionVariables(); v ++) {
				M->addNewRow();
				M->setRow(1.0, 0, 0, 0);
			}
		}
	}
	// membrane region variable
	if (simulation->getNumMemRegionVariables() != 0) {
		for (int r = 0; r < mesh->getNumMembraneRegions(); r ++) {
			for (int v = 0; v < simulation->getNumMemRegionVariables(); v ++) {
				M->addNewRow();
				M->setRow(1.0, 0, 0, 0);
			}
		}
	}
	M->close();
}

int SundialsPdeScheduler::pcSetup(realtype t, N_Vector y, N_Vector fy, booleantype jok, booleantype *jcurPtr, realtype gamma) {
	if (simulation->hasTimeDependentDiffusionAdvection()) { // has time dependent diffusion
		bPcReinit = true;

		double* yinput = NV_DATA_S(y);
		buildM_Volume(t, yinput, gamma);
		buildM_Membrane(t, yinput, gamma);
	} else if (jok) { // can reuse Jacobian data
		bPcReinit = false;
	} else {
		bPcReinit = true;

		// scale M with gamma/oldGamma
		double ratio = gamma/oldGamma;
		M->scaleOffDiagonals(ratio);
		M->shiftDiagonals(ratio);
		oldGamma = gamma;
	}
	*jcurPtr = bPcReinit;

	return 0;
}

int SundialsPdeScheduler::pcSolve(realtype t, N_Vector y, N_Vector fy, N_Vector r, N_Vector z, realtype gamma, realtype delta, int lr) {
	double* sa = M->getsa();
	int32 *ija = M->getFortranIJA();
	double *r_data = NV_DATA_S(r);
	double *z_data = NV_DATA_S(z);

	if (!bLUcomputed || bPcReinit) {
		int symmetricflg = M->getSymmetricFlag();  // general or symmetric storage format

		int IParm[75];
		memset(IParm, 0, 75 * sizeof(int));

		double RParm[25];
		memset(RParm, 0, 25 * sizeof(double));

		IParm[4] = 1; // max number of iteration
		IParm[13] = 0; // don't reuse all incomplete factorization.
		IParm[14] = 0; // fill-in parameter

		RParm[0] = 0.0;
		RParm[1] = 1.0;

		double pcgTol = 0.1;
		double RHSscale = 1;

		// zero guss
		memset(z_data, 0, numUnknowns * sizeof(double));
		PCGWRAPPER(&numUnknowns, &nsp, &symmetricflg, ija, sa, r_data, z_data, &pcgTol, IParm, RParm, pcg_workspace, pcg_workspace, &RHSscale);
		bPcReinit = false;
		if (IParm[50] != 0) {
			switch (IParm[50]) {
				case 1:	// maximum iterations reached without satisfying stopping criterion
				case 8: // stagnant
					// ignore these two errors.
					break;
				default:
					handlePCGExceptions(IParm[50], IParm[53]);
					break;
			}
		}
		if (IParm[54] == 1) {
			bLUcomputed = true;
		} else {
			bLUcomputed = false;
		}
	} else {
		int icode = -3;
		memcpy(z_data, r_data, numUnknowns * sizeof(double));
		PCILU(&icode, &numUnknowns, ija, sa, z_data, pcg_workspace, pcg_workspace);
	}

	return 0;
}

// M approximates I - gamma*J
void SundialsPdeScheduler::buildM_Volume(double t, double* yinput, double gamma) {
	if (simulation->getNumVolVariables() == 0) {
		return;
	}

#ifdef INCLUDE_ADVECTION_IN_PC
	short advectDirs[6] = {1, 1, 1, -1, -1, -1};
	int velocityExpIndexes[6] = {VELOCITY_Z_EXP, VELOCITY_Y_EXP, VELOCITY_X_EXP, VELOCITY_X_EXP, VELOCITY_Y_EXP, VELOCITY_Z_EXP};
#endif

#ifdef INCLUDE_BOUNDARY_IN_PC
	int numVariables = simulation->getNumVariables();
	int numVolVar = simulation->getNumVolVariables();
	int numSymbols = 4 + numVolVar * 3 + (numVariables - numVolVar); // t, x, y, z, U, U_INSIDE, U_OUTSIDE
	double* epsStatePointValues = new double[numSymbols];
	double eps = 1e-5;
#endif

	int colCount;
	int32* columns;
	double* values;

	for (int r = 0; r < mesh->getNumVolumeRegions(); r ++) {
		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[r]; activeVarCount ++) {
			int varIndex = regionDefinedVolVariableIndexes[r][activeVarCount];
			VolumeVariable* var = (VolumeVariable*)simulation->getVolVariable(varIndex);

			if (!var->isPde()) {
				continue;
			}
			int firstPointVolIndex = local2Global[regionOffsets[r]];
			Feature* feature = pVolumeElement[firstPointVolIndex].feature;
			VolumeVarContext* varContext = feature->getVolumeVarContext(var);
			bool bHasConstantDiffusion = false;
			double Di = 0;
			if (varContext->hasConstantDiffusion()) {
				bHasConstantDiffusion = true;
				Di = varContext->getExpressionConstantValue(DIFF_RATE_EXP);
			}
			for (int ri = 0; ri < regionSizes[r]; ri ++) {
				int localIndex = ri + regionOffsets[r];
				int volIndex = local2Global[localIndex];
				int mask = pVolumeElement[volIndex].neighborMask;
				if (mask & BOUNDARY_TYPE_DIRICHLET) {// dirichlet
					continue;
				}

				int vectorIndex = getVolumeElementVectorOffset(volIndex, r) + activeVarCount;
				assert(varContext);

				double lambdaX = oneOverH[0] * oneOverH[0];
				double lambdaY = oneOverH[1] * oneOverH[1];
				double lambdaZ = oneOverH[2] * oneOverH[2];
				double lambdaAreaX = oneOverH[0];
				double lambdaAreaY = oneOverH[1];
				double lambdaAreaZ = oneOverH[2];

				// update values
				if (!bHasConstantDiffusion) {
					updateVolumeStatePointValues(volIndex, t, yinput, statePointValues);
					Di = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);
				}

				double Aii = 0;
				if (mask & NEIGHBOR_BOUNDARY_MASK){   // boundary
					if (mask & NEIGHBOR_X_BOUNDARY_MASK){
						lambdaX *= 2.0;
						lambdaAreaX *= 2.0;
					}
					if (mask & NEIGHBOR_Y_BOUNDARY_MASK){
						lambdaY *= 2.0;
						lambdaAreaY *= 2.0;
					}
					if (mask & NEIGHBOR_Z_BOUNDARY_MASK){
						lambdaZ *= 2.0;
						lambdaAreaZ *= 2.0;
					}

#ifdef INCLUDE_BOUNDARY_IN_PC
					if ((mask & BOUNDARY_TYPE_MASK) == BOUNDARY_TYPE_NEUMANN) {
						memcpy(epsStatePointValues, statePointValues, numSymbols * sizeof(double));
						epsStatePointValues[volSymbolOffset + v * 3] += eps;

						if (mask & NEIGHBOR_XM_BOUNDARY && feature->getXmBoundaryType() == BOUNDARY_FLUX){
							double u1 = varContext->evalExpression(BOUNDARY_XM_EXP, epsStatePointValues);
							double u2 = varContext->evalExpression(BOUNDARY_XM_EXP, statePointValues);
							Aii -= (u1 - u2) * lambdaAreaX / eps;
						}
						if (mask & NEIGHBOR_XP_BOUNDARY && feature->getXpBoundaryType() == BOUNDARY_FLUX){
							double u1 = varContext->evalExpression(BOUNDARY_XP_EXP, epsStatePointValues);
							double u2 = varContext->evalExpression(BOUNDARY_XP_EXP, statePointValues);
							Aii += (u1 - u2) * lambdaAreaX / eps;
						}
						if (dimension > 1) {
							if (mask & NEIGHBOR_YM_BOUNDARY && feature->getYmBoundaryType() == BOUNDARY_FLUX){
								double u1 = varContext->evalExpression(BOUNDARY_YM_EXP, epsStatePointValues);
								double u2 = varContext->evalExpression(BOUNDARY_YM_EXP, statePointValues);
								Aii -= (u1 - u2) * lambdaAreaY / eps;
							}
							if (mask & NEIGHBOR_YP_BOUNDARY && feature->getYpBoundaryType() == BOUNDARY_FLUX){
								double u1 = varContext->evalExpression(BOUNDARY_YP_EXP, epsStatePointValues);
								double u2 = varContext->evalExpression(BOUNDARY_YP_EXP, statePointValues);
								Aii += (u1 - u2) * lambdaAreaY / eps;
							}

							if (dimension > 2) {
								if (mask & NEIGHBOR_ZM_BOUNDARY && feature->getZmBoundaryType() == BOUNDARY_FLUX){
									double u1 = varContext->evalExpression(BOUNDARY_ZM_EXP, epsStatePointValues);
									double u2 = varContext->evalExpression(BOUNDARY_ZM_EXP, statePointValues);
									Aii -= (u1 - u2) * lambdaAreaZ / eps;
								}
								if (mask & NEIGHBOR_ZP_BOUNDARY && feature->getZpBoundaryType() == BOUNDARY_FLUX){
									double u1 = varContext->evalExpression(BOUNDARY_ZP_EXP, epsStatePointValues);
									double u2 = varContext->evalExpression(BOUNDARY_ZP_EXP, statePointValues);
									Aii += (u1 - u2) * lambdaAreaZ / eps;
								}
							}
						}
					}
#endif
				}

				int volumeNeighbors[6] = {
					(dimension < 3 || mask & NEIGHBOR_ZM_MASK) ? -1 : volIndex - Nxy,
					(dimension < 2 || mask & NEIGHBOR_YM_MASK) ? -1 : volIndex - Nx,
					(mask & NEIGHBOR_XM_MASK) ? -1 : volIndex - 1,
					(mask & NEIGHBOR_XP_MASK) ? -1 : volIndex + 1,
					(dimension < 2 || mask & NEIGHBOR_YP_MASK) ? -1 : volIndex + Nx,
					(dimension < 3 || mask & NEIGHBOR_ZP_MASK) ? -1 : volIndex + Nxy
				};

#ifdef INCLUDE_ADVECTION_IN_PC
				double Vix=0, Viy=0, Viz=0;
				if (var->isAdvecting()) {
					if (hasConstantDiffusion) {
						updateVolumeStatePointValues(volIndex, t, yinput);
					}
					Vix = varContext->evalExpression(VELOCITY_X_EXP, statePointValues);
					if (dimension > 1) {
						Viy = varContext->evalExpression(VELOCITY_Y_EXP, statePointValues);
					}
					if (dimension > 2) {
						Viz = varContext->evalExpression(VELOCITY_Z_EXP, statePointValues);
					}
				}
				double Vis[6] = {Viz, Viy, Vix, Vix, Viy, Viz};
#endif

				double neighborLambdas[6] = {lambdaZ, lambdaY, lambdaX, lambdaX, lambdaY, lambdaZ};
				double neighborLambdaAreas[6] = {lambdaAreaZ, lambdaAreaY, lambdaAreaX, lambdaAreaX, lambdaAreaY, lambdaAreaZ};

				int numColumns = M->getColumns(vectorIndex, columns, values);
				colCount = 0;
				for (int n = 0; n < 6; n ++) {
					int neighborIndex = volumeNeighbors[n];
					if (neighborIndex < 0) {
						continue;
					}
					int neighborVectorIndex = getVolumeElementVectorOffset(neighborIndex, r) + activeVarCount;
					double lambda = neighborLambdas[n];

					double D = Di;
					if (!bHasConstantDiffusion) {
						updateVolumeStatePointValues(neighborIndex, t, yinput, statePointValues);
						double Dj = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);
						D = (Di + Dj < epsilon) ? (0.0) : (2 * Di * Dj/(Di + Dj));
					}

					double Aij = 0;
#ifdef INCLUDE_ADVECTION_IN_PC
					if (var->isAdvecting()) {
						updateVolumeStatePointValues(neighborIndex, t, yinput);
						int advectDir = advectDirs[n];
						double Vi = Vis[n];
						double Vj = varContext->evalExpression(velocityExpIndexes[n], statePointValues);
						double V = 0.5 * (Vi + Vj);
						double lambdaArea = neighborLambdaAreas[n];
						Aij = max(D * lambda + advectDir * 0.5 * V * lambdaArea, max(advectDir * V * lambdaArea, 0));;
						Aii += max(D * lambda - advectDir * 0.5 * V * lambdaArea, max(- advectDir * V * lambdaArea, 0));
					} else {
#endif
						Aij = D * lambda;
						Aii += Aij;
#ifdef INCLUDE_ADVECTION_IN_PC
					}
#endif
					assert(columns[colCount] == neighborVectorIndex);
					values[colCount] = -gamma * Aij;
					colCount ++;
				} // end for n
				assert(colCount == numColumns);
				M->setDiag(vectorIndex, 1.0 + gamma * Aii);
			} // end for ri
		} // end for v
	} // end for r
#ifdef INCLUDE_BOUNDARY_IN_PC
	delete[] epsStatePointValues;
#endif
}

void SundialsPdeScheduler::buildM_Membrane(double t, double* yinput, double gamma) {
	if (simulation->getNumMemVariables() == 0) {
		return;
	}
	SparseMatrixPCG* membraneElementCoupling = ((CartesianMesh*)mesh)->getMembraneCoupling();

	for (int mi = 0; mi < mesh->getNumMembraneElements(); mi ++) {
		for (int v = 0; v < simulation->getNumMemVariables(); v ++) {
			MembraneVariable* var = (MembraneVariable*)simulation->getMemVariable(v);
			if (!var->isPde()) {
				continue;
			}

			int mask = mesh->getMembraneNeighborMask(mi);
			if ((mask & NEIGHBOR_BOUNDARY_MASK) && (mask & BOUNDARY_TYPE_DIRICHLET)) {   // boundary and dirichlet
				continue;
			}

			int vectorIndex = getMembraneElementVectorOffset(mi) + v;

			int32* columns;
			double* values;
			int numColumns = M->getColumns(vectorIndex, columns, values);

			Feature* feature = pMembraneElement[mi].feature;
			MembraneVarContext *varContext = feature->getMembraneVarContext(var);

			updateMembraneStatePointValues(pMembraneElement[mi], t, yinput, statePointValues);
			double Di = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);

			int32* membraneNeighbors;
			double* s_over_d;
			int numMembraneNeighbors = membraneElementCoupling->getColumns(mi, membraneNeighbors, s_over_d);
			assert(numColumns == numMembraneNeighbors);
			double volume = membraneElementCoupling->getValue(mi, mi);
			double Aii = 0;
			for (long j = 0; j < numMembraneNeighbors; j ++) {
				int32 neighborIndex = membraneNeighbors[j];
				int neighborVectorIndex = getMembraneElementVectorOffset(neighborIndex) + v;

				updateMembraneStatePointValues(pMembraneElement[neighborIndex], t, yinput, statePointValues);
				double Dj = varContext->evalExpression(DIFF_RATE_EXP, statePointValues);
				double D = (Di + Dj < epsilon)?(0.0):(2 * Di * Dj/(Di + Dj));

				assert(columns[j] == neighborVectorIndex);
				double Aij = D * s_over_d[j] / volume;
				values[j] = - gamma * Aij;
				Aii += Aij;
			}
			M->setDiag(vectorIndex, 1.0 + gamma * Aii);
		}
	}
}

int SundialsPdeScheduler::getVolumeElementVectorOffset(int volIndex, int regionID) {
	return volVectorOffsets[regionID] + (global2Local[volIndex] - regionOffsets[regionID]) * regionDefinedVolVariableSizes[regionID];
}

int SundialsPdeScheduler::getMembraneElementVectorOffset(int meindex) {
	return memVectorOffset + meindex * simulation->getNumMemVariables();
}

int SundialsPdeScheduler::getVolumeRegionVectorOffset(int regionID) {
	return volRegionVectorOffset + regionID * simulation->getNumVolRegionVariables();
}

int SundialsPdeScheduler::getMembraneRegionVectorOffset(int regionID) {
	return memRegionVectorOffset + regionID * simulation->getNumMemRegionVariables();
}

void SundialsPdeScheduler::updateVolumeStatePointValues(int volIndex, double t, double* yinput, double* values) {
	values[0] = t;

	WorldCoord wc = mesh->getVolumeWorldCoord(volIndex);

	values[1] = wc.x;
	values[2] = wc.y;
	values[3] = wc.z;

	if (yinput == 0) {
		return;
	}

	int regionID = pVolumeElement[volIndex].region->getId();
	if (simulation->getNumVolVariables() > 0) {
		int vi = getVolumeElementVectorOffset(volIndex, regionID);
		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[regionID]; activeVarCount ++) {
			int varIndex = regionDefinedVolVariableIndexes[regionID][activeVarCount];
			values[volSymbolOffset + varIndex * 3] = yinput[vi + activeVarCount];
		}
	}

	if (simulation->getNumVolRegionVariables() > 0) {
		// fill in volume region variable values
		int volumeRegionElementVectorOffset = getVolumeRegionVectorOffset(regionID);
		memcpy(values + volRegionSymbolOffset, yinput + volumeRegionElementVectorOffset, simulation->getNumVolRegionVariables() * sizeof(double));
	}
	// if field data is used in expressions other than initial conditions, we
	// need to fill the double array the values from field data from value proxy
	//
	// we also need to fill parameter values for parameter optimization.
}

void SundialsPdeScheduler::updateMembraneStatePointValues(MembraneElement& me, double t, double* yinput, double* values) {

	WorldCoord wc = mesh->getMembraneWorldCoord(&me);

	values[0] = t;
	values[1] = wc.x;
	values[2] = wc.y;
	values[3] = wc.z;

	if (yinput == 0) {
		return;
	}

	if (simulation->getNumVolVariables() > 0) {
		// fill in INSIDE and OUTSIDE values
		int insideRegionID = pVolumeElement[me.insideIndexNear].region->getId();
		int outsideRegionID = pVolumeElement[me.outsideIndexNear].region->getId();

		int vi1 = me.insideIndexFar < 0 ? -1 : getVolumeElementVectorOffset(me.insideIndexFar, insideRegionID);
		int vi2 = getVolumeElementVectorOffset(me.insideIndexNear, insideRegionID);
		int vi3 = getVolumeElementVectorOffset(me.outsideIndexNear, outsideRegionID);
		int vi4 = me.outsideIndexFar < 0 ? -1 : getVolumeElementVectorOffset(me.outsideIndexFar, outsideRegionID);

		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[insideRegionID]; activeVarCount ++) {
			int varIndex = regionDefinedVolVariableIndexes[insideRegionID][activeVarCount];

			int iin = volSymbolOffset + varIndex * 3 + 1;
			if (vi1 < 0) {
				values[iin] = yinput[vi2 + activeVarCount];
			} else {
				values[iin] = interp_coeff[0] * yinput[vi2 + activeVarCount] + interp_coeff[1] * yinput[vi1 + activeVarCount];
			}
		}

		for (int activeVarCount = 0; activeVarCount < regionDefinedVolVariableSizes[outsideRegionID]; activeVarCount ++) {
			int varIndex = regionDefinedVolVariableIndexes[outsideRegionID][activeVarCount];
			int iout = volSymbolOffset + varIndex * 3 + 2;
			if (vi4 < 0) {
				values[iout] = yinput[vi3 + activeVarCount];
			} else {
				values[iout] = interp_coeff[0] * yinput[vi3 + activeVarCount] + interp_coeff[1] * yinput[vi4 + activeVarCount];
			}
		}
	}

	if (simulation->getNumMemVariables() > 0) {
		// fill in membrane variable values
		int membraneElementVectorOffset = getMembraneElementVectorOffset(me.index);
		memcpy(values + memSymbolOffset, yinput + membraneElementVectorOffset, simulation->getNumMemVariables() * sizeof(double));
	}

	if (simulation->getNumMemRegionVariables() > 0) {
		// fill in membrane region variable values
		int membraneRegionElementVectorOffset = getMembraneRegionVectorOffset(me.region->getId());
		memcpy(values + memRegionSymbolOffset, yinput + membraneRegionElementVectorOffset, simulation->getNumMemRegionVariables() * sizeof(double));
	}
}

void SundialsPdeScheduler::updateRegionStatePointValues(int regionID, double t, double* yinput, bool bVolumeRegion, double* values) {

	values[0] = t;

	if (yinput == 0) {
		return;
	}

	if (bVolumeRegion) {
		// fill in volume region variable values
		int volumeRegionElementVectorOffset = getVolumeRegionVectorOffset(regionID);
		memcpy(values + volRegionSymbolOffset, yinput + volumeRegionElementVectorOffset, simulation->getNumVolRegionVariables() * sizeof(double));
	} else {
		// fill in membrane region variable values
		int membraneRegionElementVectorOffset = getMembraneRegionVectorOffset(regionID);
		memcpy(values + memRegionSymbolOffset, yinput + membraneRegionElementVectorOffset, simulation->getNumMemRegionVariables() * sizeof(double));
	}
}