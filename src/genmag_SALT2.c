/**************************************************************
 Generate SN obs-frame mags using SALT2 model.
                                                             
 R. Kessler  Apr 2009 : re-written for sim & fitter; includes errors


 There are three init functions that must be called
 exeternally in the following order:

 1.  init_primary_SALT2(name, NLAM, spec);
     passes the primary spectrum (Vega, BD17, 1/lam^2 ...)

 2.  init_filter_SALT2(ifilt_obs..) is called for each
     observer-frame filter; user passes transmission vs.
     wavelength along with a global lambda-shift.
     Remember to pass Bessell-B band to compute mB at
     the end of each fit.

 3.  init_genmag_SALT2() reads SED templates and computes
     needed info from filters.


             HISTORY
            ~~~~~~~~~~~

 Mar 13, 2012: use optional magSmear function;
                see MAGSMEAR.FUNPAR_FLAG.

          
 Jan 27, 2013: minor fix in genmag_SALT2 to avoid aborting when
               Trest = 50.0000 exactly.

 Feb 13, 2013: new function set_RVMW_SALT2(RV) to override default RV=3.1.

 May 11, 2013: fix  magerrFudge_SALT2 to add fudged mag-errors in quadrature
               instead of replacing magerr_model.

 May 15, 2013: remove obsolete code under SALT2_CFIT_FLAG.

 May 18, 2013: new function checkLamRange_SALT2().

 Jul 3 2013:  fix aweful bug in gencovar_SALT2;
              was double-counting kcor term on diagonals.

 Jul 9, 2013: in SALT2magerr, when vartot<0 set it to .01^2 as JG does

 Jul 12, 2013: in SALT2magerr, replace magerr_model with exact calculation
               instead of approximation.

 Jul 25, 2013: inside integration loop (INTEG_zSED_SALT2),
               add continue statement when TRANS=0 ... to speed integration
               for leakage filters that have lots of bins with TRANS=0.

 Aug 23 2013: move checkLamRange_SEDMODEL to genmag_SEDMODEL.c

 Sep 18, 2013: remove function set_rvmw_SALT2() and use generic call to
               init_MWXT_SEDMODEL() to init OPT_MWCOLORLAW and RVMW.

 May 5 2014:  Add Fratio output argument to INTEG_zSED_SALT2( ... )
              to easily allow computing ratio with/without 
              Galactic extinction in the integrals.
   
 July 15 2016: add RV_host & AV_host arguments to genmag_SALT2,
               to allow for simulating Mandel's BayeSN model.
           
 July 30 2016: in fill_SALT2_TABLE_SED(), check for uniform bins.
               Now catches missing LAM=8490 bin in SALT2.Guy10_UV2IR.

 Aug 31 2016: in genSpec_SALT2(), return of Trest is outside epoch
              range of SALT2 ; cannot extrapolate spectra.

*************************************/


#include <stdio.h> 
#include <math.h>     // log10, pow, ceil, floor
#include <stdlib.h>   // includes exit(),atof()

#include "sntools.h"           // community tools
#include "sntools_genSmear.h"
#include "sntools_spectrograph.h"
#include "genmag_SEDtools.h"
#include "genmag_SALT2.h" 
#include "MWgaldust.h"

// =======================================================
// define mangled functions with underscore (for fortran)

int init_genmag_salt2__(char *model_version, char *model_extrap, int *OPTMASK ) {
  int istat;
  istat = init_genmag_SALT2 ( model_version, model_extrap,  *OPTMASK ) ;
  return istat ;
} 


void genmag_salt2__(int *OPTMASK, int *ifilt, 
		    double *x0, double *x1, double *x1_forErr, double *c, 
		    double *mwebv, double *RV_host, double *AV_host,
		    double *z, double *z_forErr, int *nobs, double *Tobs_list, 
		    double *magobs_list, double *magerr_list ) {

  genmag_SALT2(*OPTMASK, *ifilt, *x0, *x1, *x1_forErr, *c, *mwebv, 
	       *RV_host, *AV_host, *z, *z_forErr, *nobs, Tobs_list, 
	       magobs_list, magerr_list );
}


double salt2x0calc_(double *alpha, double *beta, double *x1,   
		    double *c, double *dlmag ) {
  double x0;
  x0 = SALT2x0calc(*alpha, *beta, *x1,  *c, *dlmag );
  return x0;
} //

double salt2mbcalc_(double *x0) {
  double mB;
  mB = SALT2mBcalc(*x0);
  return mB ;
} //



int gencovar_salt2__ (
		  int *MATSIZE         // (I) row-len (or col-len)
                  ,int *ifilt_obs      // (I) list of 'matsize' filter indices
                  ,double *epobs       // (I) list of 'matsize' rest days
		  ,double *z            // (I) redshift
		  ,double *x0
		  ,double *x1
		  ,double *c
		  ,double *mwebv
		  ,double *RV_host
		  ,double *AV_host
                  ,double *covar       // (O) covariance matrix
                  ) {
  int istat ;
  istat = gencovar_SALT2 ( *MATSIZE, ifilt_obs, epobs, *z, *x0, *x1, *c,
			   *mwebv, *RV_host, *AV_host, covar ) ;
  return istat;
}

// external spline function

extern void in2dex_(int *ispline, int *N2D,
		    double *XX, double *YY, double *ZZ,
		    double *XLIM, double *YLIM, double *SS, int *IERR );

extern double ge2dex_ ( int *IND, double *Trest, double *Lrest, int *IERR ) ;

/****************************************************************
  init_genmag_SALT:
    o reads in the filters from FilterFiles
    o calculates filter mean and AB zeropoint
    o reads the templates from TemplateFiles
    o returns true if successful

   Note: must call init_filter_SEDMODEL() and init_primary_SEDMODEL()
          before calling this function.

  Feb 24, 2009 RSK: read colorCorrection from file

  Oct 13, 2009: if SED.DAT exists, NSURFACE=1; else NSURFACE=2 (nominal)

  Apr 24, 2010:  call  check_sedflux_bins(...) to ensure idential 
                 SED binning for each surface.

  Jun 27, 2010: add OPTMASK argument. Bit 8 (OPTMASK=128) sets 
                NLAMPOW_SEDMODEL=0 to implement legacy style 
                where color*XTMW is outside the integrals.


  OPTMASK bit 8 (128) : legacy option sets NLAMPOW_SEDMODEL=0

  Jul 27, 2010:  new call to load_mBoff_SALT2();

  Aug 04, 2010: call colordump_SALT2 for more wavelengths ...
                enough to span 2000 to 10,000 A.

  Jan 18, 2011: read model from getenv(PRIVATE_MODELPATH_NAME) if this
                env variable exists.

  Mar 3, 2011: move errmap-reading  into read_SALT2errmaps().

  Aug 9 2017: Lrange[1] -> 30000 (was 20000)  
 
  Mar 18 2017: add genmag_SALT2 argument z_forErr; allows passing
               fixed redshift for photo-z fits, analogous to x1_forErr.

  Jun 24 2018: add argument MODEL_EXTRAP to override
                GENMODEL_EXTRAP_LATETIME in SALT2.INFO file

  Aug 02 2019:
    set SALT2_PREFIX_FILENAME to either salt2 (default) or salt3.
    File name prefixes thus correspond to model name.

 Aug 26 2019: implement RELAX_IDIOT_CHECK_SALT2 for P18 to avoid abort.

 Nov 7 2019: for SALT3, remove x1*M1/M0 term in error; see ISMODEL_SALT3.
 Jan 19 2020: in INTEG_zSED_SALT2, fix memory leak related to local magSmear.

****************************************************************/

int init_genmag_SALT2(char *MODEL_VERSION, char *MODEL_EXTRAP_LATETIME,
		      int OPTMASK ) {

  double Trange[2], Lrange[2]     ;
  int  ised, LEGACY_colorXTMW ;
  int  retval = 0   ;
  int  ABORT_on_LAMRANGE_ERROR = 0;
  char BANNER[120], tmpFile[200], sedcomment[40], version[60]  ;
  char fnam[] = "init_genmag_SALT2" ;

  // -------------- BEGIN --------------

  // extrac OPTMASK options
  ABORT_on_LAMRANGE_ERROR = ( OPTMASK &  64 ) ; // Sep 9 2019
  LEGACY_colorXTMW        = ( OPTMASK & 128 ) ;

  sprintf(BANNER, "%s : Initialize %s", fnam, MODEL_VERSION );
  print_banner(BANNER);
 

  if ( NFILT_SEDMODEL == 0 ) {
    sprintf(c1err,"No filters defined ?!?!?!? " );
    sprintf(c2err,"Need to call init_filter_SEDMODEL");
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err); 
  }

  sprintf(SALT2_INFO_FILE,     "SALT2.INFO" );

  // summarize filter info
  filtdump_SEDMODEL();


  // ==========================================
  // construct path to SALT2 surfaces

  extract_MODELNAME(MODEL_VERSION,             // input
		    SALT2_MODELPATH, version); // returned
  
  if ( getenv(PRIVATE_MODELPATH_NAME) != NULL ) {
    sprintf( SALT2_MODELPATH, "%s/%s", 
	     getenv(PRIVATE_MODELPATH_NAME), version );    
  }
  else if ( strlen(SALT2_MODELPATH) > 0 ) {
    // do nothing;
  }
  else {
    // default location under $SNDATA_ROOT
    sprintf( SALT2_MODELPATH, "%s/models/SALT2/%s", 
	     getenv("SNDATA_ROOT"), version );
  }
  

  // Aug 02 2019: set prefix for filenames to allow salt2 or salt3 prefix
  ISMODEL_SALT3=0; sprintf(SALT2_PREFIX_FILENAME,"salt2"); // default
  if ( strstr(version,"SALT3") != NULL ) 
    { sprintf(SALT2_PREFIX_FILENAME,"salt3");  ISMODEL_SALT3=1; } 

  RELAX_IDIOT_CHECK_SALT2 = ( strstr(version,"P18") != NULL );


  // set defaults for two surfaces (nominal SALT2)
  SEDMODEL.NSURFACE   = 2 ;
  SEDMODEL.FLUXSCALE  = X0SCALE_SALT2; 
  SEDMODEL.MAGERR_FIX = -9.0 ;        // => use calculated errors

  // May 2013:
  // if re-reading the same SALT2 version, then skip re-reading the files.
  // WARNING: SALT2_VERSION is not set on first pass, so it may give
  //          valgrind errors.
  int SKIPREAD = 0 ;
  if ( strcmp(SALT2_VERSION,version) == 0 ) { 
    printf("\t Re-init %s -> skip reading files. \n", version);
    fflush(stdout);
    init_SALT2interp_SEDFLUX();
    init_SALT2interp_ERRMAP();
    SKIPREAD = 1;  // set logical in case we need it later
    return retval ;
  }

  sprintf(SALT2_VERSION,"%s", version); // May 15 2013

  // ============================

  read_SALT2_INFO_FILE();  

  // check option to override late-time extrap model from sim-input file
  if ( strlen(MODEL_EXTRAP_LATETIME) > 0 ) { 
    sprintf(INPUT_EXTRAP_LATETIME.FILENAME, "%s", 
	    MODEL_EXTRAP_LATETIME); 
  }

  // ============================
  
  // set extreme ranges to read anything
  Trange[0] = -20. ;
  Trange[1] = 200. ;
  Lrange[0] = LAMMIN_SEDMODEL ;
  Lrange[1] = LAMMAX_SEDMODEL ;

  SEDMODEL_MWEBV_LAST     = -999.   ;
  SEDMODEL_HOSTXT_LAST.AV = -999.   ;
  SEDMODEL_HOSTXT_LAST.z  = -999.   ;

  SPECTROGRAPH_SEDMODEL.NBLAM_TOT = 0 ; // spectrograph option

  malloc_SEDFLUX_SEDMODEL(&TEMP_SEDMODEL,0,0,0);

  // ------- Now read the spectral templates -----------

  for ( ised = 0 ; ised < SEDMODEL.NSURFACE ; ised++ ) {

    sprintf(tmpFile, "%s/%s_template_%d.dat", 
	    SALT2_MODELPATH, SALT2_PREFIX_FILENAME, ised );

    //  printf("  Read Template file: \n\t %s \n", tmpFile);

    sprintf(sedcomment,"SALT2-%d", ised);

    rd_sedFlux(tmpFile, sedcomment, Trange, Lrange
	       ,MXBIN_DAYSED_SEDMODEL, MXBIN_LAMSED_SEDMODEL, 0
	       ,&TEMP_SEDMODEL.NDAY, TEMP_SEDMODEL.DAY, &TEMP_SEDMODEL.DAYSTEP
	       ,&TEMP_SEDMODEL.NLAM, TEMP_SEDMODEL.LAM, &TEMP_SEDMODEL.LAMSTEP
	       ,TEMP_SEDMODEL.FLUX,  TEMP_SEDMODEL.FLUXERR );


    // July 18 2018: check for UV extrap to avoid filter dropouts
    double UVLAM = INPUTS_SEDMODEL.UVLAM_EXTRAPFLUX;
    if ( UVLAM > 0.0 ) { UVLAM_EXTRAPFLUX_SEDMODEL(UVLAM,&TEMP_SEDMODEL); }

    // make sure that DAY and LAM binning is identical for each surface
    check_sedflux_bins(ised, "DAY", 
       TEMP_SEDMODEL.NDAY, TEMP_SEDMODEL.DAY[0], TEMP_SEDMODEL.DAYSTEP);
    check_sedflux_bins(ised, "LAM", 
       TEMP_SEDMODEL.NLAM, TEMP_SEDMODEL.LAM[0], TEMP_SEDMODEL.LAMSTEP);

    // transfer TEMP_SEDMODEL to permanent storage
    fill_SALT2_TABLE_SED(ised);

  } //  end loop over SED templates


  load_mBoff_SALT2();

  // ========== read error maps with same format as SED flux
  read_SALT2errmaps(Trange,Lrange);  


  // ------------------
  // Read color-dispersion vs. wavelength
  read_SALT2colorDisp();

  // abort if any ERRMAP has invalid wavelength range (Sep 2019)
  if ( ABORT_on_LAMRANGE_ERROR ) { check_lamRange_SALT2errmap(-1); }

  // fill/calculate color-law table vs. color and rest-lambda
  fill_SALT2_TABLE_COLORLAW();

  // init interp (for splines only)
  init_SALT2interp_SEDFLUX();
  init_SALT2interp_ERRMAP();

  NCALL_DBUG_SALT2 = 0;

  // Summarize CL and errors vs. lambda for Trest = x1 = 0.
  errorSummary_SALT2();

  init_extrap_latetime_SALT2();

  fflush(stdout) ;


  //  test_SALT2colorlaw1();

  // ===========================================

  printf("\n  %s : Done. \n", fnam );

  //  debugexit("SALT2 init");
  return retval;

} // end of function init_genmag_SALT2


// ***********************************************
void fill_SALT2_TABLE_SED(int ISED) {

  // transfer original TEMP_SEDMODEL contents to permanent
  // (dynamically allocated) SALT2_TABLE.SEDFLUX.
  // Note that SEDMODEL.DAY is not allocated or filled
  // since the DAY-bins here must be uniform.
  //
  // If spline-interp option is set, then apply spline-interp
  // here and store finer-binned SEDs so that faster linear-
  // interpolation can be used inside the integration loops.
  //
  // Jun 9, 2011: load SEDMODEL.LAMMIN[ISED] and SEDMODEL.LAMMAX[ISED]
  // Dec 30, 2013: add PRE-ABORT dump when FRATIO is too large.
  // Jul 30, 2016: call check_uniform_bins( ... )
  
#define N1DBIN_SPLINE 3

  int 
    jflux_orig, IDAY, ILAM, IDAY_ORIG, ILAM_ORIG,iday, ilam
    ,NDAY_ORIG,  NLAM_ORIG, NDAY_TABLE, NLAM_TABLE
    ,NREBIN_DAY, NREBIN_LAM, INTERP_OPT, EDGE, I8, I8p
    ;

  double 
    xi, DAY, LAM, DIF, FRAC, DAYSTEP_ORIG, LAMSTEP_ORIG
    ,F2D_orig[N1DBIN_SPLINE][N1DBIN_SPLINE]
    ,F_interp, F_orig, FDIF, FSUM, FRATIO
    ,FDAY[N1DBIN_SPLINE],FRATIO_CHECK
    ,*ptrLAM, *ptrDAY
    ;

  char 
     cmsg1[40], cmsg2[40]
    ,fnam[]   = "fill_SALT2_TABLE_SED" 
    ,tagLAM[] = "interp-LAM"
    ,tagDAY[] = "interp-DAY"
    ;

  // ---------------- BEGIN --------------

  NDAY_ORIG  = TEMP_SEDMODEL.NDAY ;
  NLAM_ORIG  = TEMP_SEDMODEL.NLAM ;
  INTERP_OPT = INPUT_SALT2_INFO.SEDFLUX_INTERP_OPT ;
  
  // ----------
  // check for uniform binning (July 2016)
  check_uniform_bins(NDAY_ORIG, TEMP_SEDMODEL.DAY, "DayGrid(SALT2)");
  //  check_uniform_bins(NLAM_ORIG, TEMP_SEDMODEL.LAM, "LamGrid(SALT2)");

  // ----------
  if ( INTERP_OPT == SALT2_INTERP_SPLINE ) {
    NREBIN_DAY = INPUT_SALT2_INFO.INTERP_SEDREBIN_DAY ; 
    NREBIN_LAM = INPUT_SALT2_INFO.INTERP_SEDREBIN_LAM ; 
  }
  else {
    NREBIN_DAY = NREBIN_LAM = 1;
  }

  NDAY_TABLE = NDAY_ORIG * NREBIN_DAY ; 
  NLAM_TABLE = NLAM_ORIG * NREBIN_LAM ; 

  SEDMODEL.LAMSTEP[ISED] = TEMP_SEDMODEL.LAMSTEP ; // Jul 30 2016

  I8  = sizeof(double);
  I8p = sizeof(double*);

  if ( ISED == 0 ) {
    // load SEDMODEL struct for IFILTSTAT function
    SEDMODEL.LAMMIN_ALL = TEMP_SEDMODEL.LAM[0] ;
    SEDMODEL.LAMMAX_ALL = TEMP_SEDMODEL.LAM[NLAM_ORIG-1] ;

    SALT2_TABLE.NDAY    = NDAY_TABLE ;
    SALT2_TABLE.DAYSTEP = TEMP_SEDMODEL.DAYSTEP/(double)(NREBIN_DAY) ;
    SALT2_TABLE.DAYMIN  = TEMP_SEDMODEL.DAY[0] ;
    SALT2_TABLE.DAYMAX  = TEMP_SEDMODEL.DAY[NDAY_ORIG-1] ;
    SALT2_TABLE.DAY     = (double*)malloc(I8*NDAY_TABLE);

    SALT2_TABLE.NLAMSED = NLAM_TABLE ;
    SALT2_TABLE.LAMSTEP = TEMP_SEDMODEL.LAMSTEP/(double)(NREBIN_LAM) ;
    SALT2_TABLE.LAMMIN  = TEMP_SEDMODEL.LAM[0] ;
    SALT2_TABLE.LAMMAX  = TEMP_SEDMODEL.LAM[NLAM_ORIG-1] ;
    SALT2_TABLE.LAMSED  = (double*)malloc(I8*NLAM_TABLE);

    for ( IDAY=0; IDAY < NDAY_TABLE; IDAY++ ) {  
      xi = (double)IDAY ;    
      SALT2_TABLE.DAY[IDAY] = 
	SALT2_TABLE.DAYMIN + (xi * SALT2_TABLE.DAYSTEP);
    }

    for ( ILAM=0; ILAM < NLAM_TABLE; ILAM++ ) {  
      xi = (double)ILAM ;    
      SALT2_TABLE.LAMSED[ILAM] = 
	SALT2_TABLE.LAMMIN + (xi * SALT2_TABLE.LAMSTEP);
    }
  }

  sprintf(cmsg1,"LAM(MIN,MAX,STEP)=%4.0f,%4.0f,%1.0f",
	  SALT2_TABLE.LAMMIN, SALT2_TABLE.LAMMAX, SALT2_TABLE.LAMSTEP );
  sprintf(cmsg2,"DAY(MIN,MAX,STEP)=%2.0f,%2.0f,%2.1f",
	  SALT2_TABLE.DAYMIN, SALT2_TABLE.DAYMAX, SALT2_TABLE.DAYSTEP );
  printf("  Store SED-%d  %s  %s \n\n", ISED, cmsg1, cmsg2 );


  DAYSTEP_ORIG = TEMP_SEDMODEL.DAYSTEP ;
  LAMSTEP_ORIG = TEMP_SEDMODEL.LAMSTEP ;

  // --------------------------------------
  // allocate memory for SED surface
  SALT2_TABLE.SEDFLUX[ISED] = (double**)malloc(I8p*NDAY_TABLE);
  for ( IDAY=0; IDAY < NDAY_TABLE; IDAY++ ) {
    SALT2_TABLE.SEDFLUX[ISED][IDAY] = (double*)malloc(I8*NLAM_TABLE); 
  }

  // --------------------------------------
  // store SED table.

  for ( IDAY=0; IDAY < NDAY_TABLE; IDAY++ ) {

    // get day-index on original grid
    DAY       = SALT2_TABLE.DAY[IDAY]; // fine-binned DAY
    DIF       = DAY - SALT2_TABLE.DAYMIN + 0.0001 ;
    IDAY_ORIG = (int)(DIF/DAYSTEP_ORIG);

    if ( INTERP_OPT == SALT2_INTERP_SPLINE ) {
      FRAC = (DAY - TEMP_SEDMODEL.DAY[IDAY_ORIG])/DAYSTEP_ORIG;
      if ( FRAC < 0.5 && IDAY_ORIG > 0 ) { IDAY_ORIG-- ; }
      if ( IDAY_ORIG > NDAY_ORIG - N1DBIN_SPLINE ) 
	{ IDAY_ORIG = NDAY_ORIG - N1DBIN_SPLINE ; }
    }

    for ( ILAM=0; ILAM < NLAM_TABLE; ILAM++ ) {

      // for LINEAR option, just take value at node (no interp here)
      if ( INTERP_OPT == SALT2_INTERP_LINEAR ) {
	  jflux_orig  = NLAM_ORIG*IDAY + ILAM ;
	  SALT2_TABLE.SEDFLUX[ISED][IDAY][ILAM] = 
	    TEMP_SEDMODEL.FLUX[jflux_orig] ;
	  continue ; // skip interp stuff below
      }

      // the code below is for the spline option

      // get lam-index on original grid
      LAM       = SALT2_TABLE.LAMSED[ILAM]; // fine-binned lambda
      DIF       = LAM - SALT2_TABLE.LAMMIN + 0.0001 ;
      ILAM_ORIG = (int)(DIF/LAMSTEP_ORIG);

      FRAC  = (LAM - TEMP_SEDMODEL.LAM[ILAM_ORIG])/LAMSTEP_ORIG ;
      if ( FRAC < 0.5  &&  ILAM_ORIG > 0 ) { ILAM_ORIG-- ; }
      if ( ILAM_ORIG > NLAM_ORIG - N1DBIN_SPLINE ) 
	{ ILAM_ORIG = NLAM_ORIG - N1DBIN_SPLINE ; }

      // build temp flux-grid around point to interpolate

      ptrLAM = &TEMP_SEDMODEL.LAM[ILAM_ORIG] ;
      ptrDAY = &TEMP_SEDMODEL.DAY[IDAY_ORIG] ;

      for ( iday=0; iday < N1DBIN_SPLINE; iday++ ) {
	for ( ilam=0; ilam < N1DBIN_SPLINE; ilam++ ) {
	  jflux_orig  = NLAM_ORIG*(IDAY_ORIG+iday) + (ILAM_ORIG+ilam) ;
	  F2D_orig[iday][ilam] = TEMP_SEDMODEL.FLUX[jflux_orig] ;
	}
	// interpolate across lambda to get FDAY
	FDAY[iday] = quadInterp( LAM, ptrLAM, F2D_orig[iday], tagLAM);
      }

      // Now interpolate across DAY
      F_interp = quadInterp( DAY, ptrDAY, FDAY, tagDAY);
      SALT2_TABLE.SEDFLUX[ISED][IDAY][ILAM] = F_interp ;

      // DDDDDDDDDDDDDDDDDDDDDDDDDDDDD
      if ( IDAY == -10 && ILAM < -6 ) {
	printf(" DDDDD ------------------------------------- \n");

	printf(" DDDDD ptrDAY = %5.1f %5.1f %5.1f \n",
	       *(ptrDAY+0),  *(ptrDAY+1), *(ptrDAY+2) );

	printf(" DDDDD FDAY = %le %le %le \n",
	       *(FDAY+0),  *(FDAY+1), *(FDAY+2) );

	printf(" DDDDD DAY[%d]=%6.1f LAM[%d]=%7.1f  F_interp = %le \n",
	       IDAY, DAY, ILAM, LAM, F_interp );
      }
      // DDDDDDDDDDDDDDDDDDDDD


    } // ILAM
  } // IDAY


  // Now an idiot check.
  // Loop over original grid (nodes) and make sure that
  // the finer grid agrees at the nodes.
  // Start at 2nd index for  both DAY and LAM to avoid sharp rise at start

  for ( IDAY_ORIG=1; IDAY_ORIG < NDAY_ORIG; IDAY_ORIG++ ) {
    for ( ILAM_ORIG=1; ILAM_ORIG < NLAM_ORIG; ILAM_ORIG++ ) {

      EDGE = 0;
      if ( IDAY_ORIG == 0 || IDAY_ORIG == NDAY_ORIG-1 ) { EDGE = 1 ; }
      if ( ILAM_ORIG == 0 || ILAM_ORIG == NLAM_ORIG-1 ) { EDGE = 1 ; }

      // make looser check at edge-boundary where the interpolation
      // may be a little off.
      if ( EDGE || RELAX_IDIOT_CHECK_SALT2 ) 
	{ FRATIO_CHECK = 1.0E-3 ; }
      else
	{ FRATIO_CHECK = 1.0E-5 ; } // Aug 28 2019: E-6 -> E-5

       

      IDAY = IDAY_ORIG * NREBIN_DAY ;
      ILAM = ILAM_ORIG * NREBIN_LAM ;

      jflux_orig  = NLAM_ORIG*IDAY_ORIG + ILAM_ORIG ;
      F_orig      = TEMP_SEDMODEL.FLUX[jflux_orig] ;
      F_interp    = SALT2_TABLE.SEDFLUX[ISED][IDAY][ILAM];
      FDIF        = F_interp - F_orig ;
      FSUM        = F_interp + F_orig ;

      if ( RELAX_IDIOT_CHECK_SALT2 && F_orig < 1.0E-25 ) { continue; } 

      if ( FSUM > 0.0 ) 
	{ FRATIO = FDIF / FSUM ; }
      else
	{ FRATIO = 0.0 ; }

	    
      if ( fabs(FRATIO) > FRATIO_CHECK ) {
	print_preAbort_banner(fnam);
	printf("  FRATIO = FDIF/FSUM = %f  (FRATIO_CHECK=%le)\n", 
	       FRATIO, FRATIO_CHECK);
	printf("  IDAY=%4d  IDAY_ORIG=%4d  \n", IDAY, IDAY_ORIG);
	printf("  ILAM=%4d  ILAM_ORIG=%4d  \n", ILAM, ILAM_ORIG);
	printf("\n");

	// print 3x3 Flux matrix vs. LAM and DAY 
	int ilam, iday, jflux;	
	
	printf("\t LAM\\DAY");
	for(iday=IDAY_ORIG-1; iday<=IDAY_ORIG+1; iday++ ) 
	  { printf("   %8.1f     ", SALT2_TABLE.DAY[iday*NREBIN_DAY] ); }
	printf("\n");

	for(ilam=ILAM_ORIG-1; ilam <= ILAM_ORIG+1; ilam++ ) {
	  printf("\t %6.1f : ", SALT2_TABLE.LAMSED[ilam*NREBIN_LAM] );
	  for(iday=IDAY_ORIG-1; iday<=IDAY_ORIG+1; iday++ ) {
	    jflux     = NLAM_ORIG*iday + ilam ;
	    printf("%14.6le  ", TEMP_SEDMODEL.FLUX[jflux]);
	  }
	  printf("\n");
	}	

	sprintf(c1err,"Bad SED-%d interp at DAY[%d]=%3.1f  LAM[%d]=%6.1f"
		,ISED
		,IDAY_ORIG, SALT2_TABLE.DAY[IDAY]
		,ILAM_ORIG, SALT2_TABLE.LAMSED[ILAM] );
	sprintf(c2err,"F[interp/orig] = %le / %le = %f",
		F_interp, F_orig, F_interp/F_orig );
	errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
      }
    }
  }
  
  /*
  IDAY = 12;  ILAM = 40; 
  printf("\t  xxxx DAY = SALT2_TABLE.DAY[%d] = %f \n", 
	 IDAY, SALT2_TABLE.DAY[IDAY] );
  printf("\t  xxxx LAM = SALT2_TABLE.LAMSED[%d] = %f \n", 
	 ILAM, SALT2_TABLE.LAMSED[ILAM] );
  printf("\t  xxxx SEDFLUX[%d] = %le \n", 
	 ISED, SALT2_TABLE.SEDFLUX[ISED][IDAY][ILAM] );
  if ( ISED == 1 )  { debugexit("store SED"); } 
  */

} // end of fill_SALT2_TABLE_SED





// ***********************************************
void fill_SALT2_TABLE_COLORLAW(void) {

  // Create and fill color law table as a function of
  // color and rest-frame wavelength. The lambda bins
  // are the same bins as for the SED.

  int NLAMSED, NCBIN, ilam, ic, I8, I8p;
  double LAM, CMIN, CMAX, CSTEP, CVAL, xc, CCOR ;

  char fnam[] = "fill_SALT2_TABLE_COLORLAW";
  
  // ------------- BEGIN ------------------

  // first hard-wire the binning for color ...
  // perhaps later read this from somewhere.

  
  SALT2_TABLE.NCBIN  =  401  ;
  SALT2_TABLE.CMIN   = -2.0  ;
  SALT2_TABLE.CMAX   = +2.0  ;
  SALT2_TABLE.CSTEP  =  0.01 ;


  // get local 'NBIN' variables
  NLAMSED  = SALT2_TABLE.NLAMSED ;
  NCBIN    = SALT2_TABLE.NCBIN   ;
  CMIN     = SALT2_TABLE.CMIN    ;
  CMAX     = SALT2_TABLE.CMAX    ;
  CSTEP    = SALT2_TABLE.CSTEP   ;

  printf("  Create ColorLaw Table: COLOR(MIN,MAX,STEP) = %3.1f,%2.1f,%3.2f\n",
	 CMIN,CMAX, CSTEP );

  // allocate memory for table
  I8  = sizeof(double);
  I8p = sizeof(double*);

  SALT2_TABLE.COLOR     = (double *)malloc(I8*NCBIN);
  SALT2_TABLE.COLORLAW  = (double**)malloc(I8p*NCBIN);
  for ( ic=0; ic < NCBIN; ic++ ) {

    xc = (double)ic;
    CVAL = CMIN + xc*CSTEP ;
    SALT2_TABLE.COLOR[ic] = CVAL ;
    SALT2_TABLE.COLORLAW[ic] = (double*)malloc(I8*NLAMSED);

    for ( ilam=0; ilam < NLAMSED; ilam++ ) {
      LAM  = SALT2_TABLE.LAMSED[ilam];
      CCOR = SALT2colorCor(LAM,CVAL);
      SALT2_TABLE.COLORLAW[ic][ilam] = CCOR ;
    } // ilam
  } //  ic

  // sanity checks on color-table

  if ( SALT2_TABLE.COLOR[0] != CMIN ) {
    sprintf(c1err, "SALT2_TABLE.COLOR[0] = %f", SALT2_TABLE.COLOR[0]);
    sprintf(c2err, "but should be CMIN = %f", CMIN);
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
  }

  if ( SALT2_TABLE.COLOR[NCBIN-1] != CMAX ) {
    sprintf(c1err, "SALT2_TABLE.COLOR[%d] = %f", 
	    NCBIN-1, SALT2_TABLE.COLOR[NCBIN-1]);
    sprintf(c2err, "but should be CMAX = %f", CMAX);
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
  }


} // end of fill_SALT2_TABLE_colorLaw



// ***********************************************
void read_SALT2errmaps(double Trange[2], double Lrange[2] ) {
  // Mar 2011
  // Read error maps that depend on Trest vs. lambda
  // (move code from init_genmag_SALT2).
  //
  // May 2011: check array bound
  // Jul 2013: add array-bound check on NBTOT = NBLAM*NDAY
  
  int imap, NDAY, NLAM, NBTOT;
  double DUMMY[20];

  char
    tmpFile[200]
    ,sedcomment[80]
    ,*prefix = SALT2_PREFIX_FILENAME
    ,fnam[] = "read_SALT2errmaps" ;
    ;

  // ----------- BEGIN -----------    

  printf("\n Read SALT2 ERROR MAPS: \n");
  fflush(stdout);

  NERRMAP_BAD_SALT2 = 0 ;

  // hard-wire filenames for erro maps
  sprintf(SALT2_ERRMAP_FILES[0], "%s_lc_relative_variance_0.dat", prefix );
  sprintf(SALT2_ERRMAP_FILES[1], "%s_lc_relative_variance_1.dat", prefix );
  sprintf(SALT2_ERRMAP_FILES[2], "%s_lc_relative_covariance_01.dat", prefix );
  sprintf(SALT2_ERRMAP_FILES[3], "%s_lc_dispersion_scaling.dat", prefix );
  sprintf(SALT2_ERRMAP_FILES[4], "%s_color_dispersion.dat",      prefix );

  sprintf(SALT2_ERRMAP_COMMENT[0],  "VAR0" );
  sprintf(SALT2_ERRMAP_COMMENT[1],  "VAR1" );
  sprintf(SALT2_ERRMAP_COMMENT[2],  "COVAR" );
  sprintf(SALT2_ERRMAP_COMMENT[3],  "ERRSCALE" );
  sprintf(SALT2_ERRMAP_COMMENT[4],  "COLOR-DISPERSION" );


  for ( imap=0; imap < NERRMAP; imap++ ) {

    if ( imap >= INDEX_ERRMAP_COLORDISP ) { continue ; } // read elsewhere

    sprintf(tmpFile, "%s/%s", SALT2_MODELPATH, SALT2_ERRMAP_FILES[imap] );
    sprintf(sedcomment, "SALT2-%s", SALT2_ERRMAP_COMMENT[imap] );


    rd_sedFlux(tmpFile, sedcomment, Trange, Lrange
	       ,MXBIN_DAYSED_SEDMODEL, MXBIN_LAMSED_SEDMODEL, 0   // inputs
	       ,&SALT2_ERRMAP[imap].NDAY    // outputs
	       ,SALT2_ERRMAP[imap].DAY      // idem ...
	       ,&SALT2_ERRMAP[imap].DAYSTEP
	       ,&SALT2_ERRMAP[imap].NLAM
	       ,SALT2_ERRMAP[imap].LAM
	       ,&SALT2_ERRMAP[imap].LAMSTEP
	       ,SALT2_ERRMAP[imap].VALUE 
	       ,DUMMY
	       );

    NLAM = SALT2_ERRMAP[imap].NLAM ;
    SALT2_ERRMAP[imap].LAMMIN  = SALT2_ERRMAP[imap].LAM[0] ;
    SALT2_ERRMAP[imap].LAMMAX  = SALT2_ERRMAP[imap].LAM[NLAM-1] ;

    NDAY = SALT2_ERRMAP[imap].NDAY ;
    SALT2_ERRMAP[imap].DAYMIN  = SALT2_ERRMAP[imap].DAY[0] ;
    SALT2_ERRMAP[imap].DAYMAX  = SALT2_ERRMAP[imap].DAY[NDAY-1] ;

    NBTOT = NLAM*NDAY ;
    if ( NBTOT >= MXBIN_VAR_SALT2 ) {
      sprintf(c1err,"NLAM*NDAY=%d*%d = %d exceeds bound of MXBIN_VAR_SALT2=%d",
	      NLAM, NDAY, NBTOT, MXBIN_VAR_SALT2);
      sprintf(c2err,"See '%s'", tmpFile);
      errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
    }


    // Sep 2019: make sure wave range covers SED wave range
    check_lamRange_SALT2errmap(imap);
    check_dayRange_SALT2errmap(imap);

    fflush(stdout);

  }   //  imap


} // end of read_SALT2errmaps


// ***************************************
void init_SALT2interp_SEDFLUX(void) {

  int OPT ;
  //  char fnam[] = "init_SALT2interp_SEDFLUX" ;

  // ---------- BEGIN -----------

  OPT = INPUT_SALT2_INFO.SEDFLUX_INTERP_OPT ;
  if ( OPT != SALT2_INTERP_SPLINE ) { return ; }

  // nothing to do here.

} // end of init_SALT2interp_SEDFLUX


// ***************************************
void init_SALT2interp_ERRMAP(void) {

  // if spline-option is set for error maps,
  // then init splines

  int OPT, imap, ispline, iday, ilam, N2DBIN, jtmp, IERR ;
  double ERRTMP, XM, SS ;


  char fnam[] = "init_SALT2interp_ERRMAP" ;

  // ---------- BEGIN -----------

  OPT = INPUT_SALT2_INFO.ERRMAP_INTERP_OPT ;
  if ( OPT != SALT2_INTERP_SPLINE ) { return ; }

  for ( imap=0; imap < NERRMAP; imap++ ) {

    if ( imap >= INDEX_ERRMAP_COLORDISP ) { continue ; }

      ispline = SALT2_TABLE.INDEX_SPLINE[1] + imap + 1 ; 
      SALT2_ERRMAP[imap].INDEX_SPLINE = ispline ; 

      SALT2_SPLINE_ARGS.DAYLIM[0] = SALT2_ERRMAP[imap].DAYMIN ;
      SALT2_SPLINE_ARGS.DAYLIM[1] = SALT2_ERRMAP[imap].DAYMAX ;
      SALT2_SPLINE_ARGS.LAMLIM[0] = SALT2_ERRMAP[imap].LAMMIN ;
      SALT2_SPLINE_ARGS.LAMLIM[1] = SALT2_ERRMAP[imap].LAMMAX ;

      // for spline, use every other day and every other lambda bin
      N2DBIN = 0;
      for ( iday=0; iday <  SALT2_ERRMAP[imap].NDAY ; iday+=2 ) {
	for ( ilam=0; ilam <  SALT2_ERRMAP[imap].NLAM ; ilam+=2 ) {
	  N2DBIN++ ;

	  jtmp = SALT2_ERRMAP[imap].NLAM *iday + ilam ;
	  ERRTMP = SALT2_ERRMAP[imap].VALUE[jtmp] ;
          if ( ERRTMP == 0.0 ) ERRTMP = 1.0E-9 ;

	  SALT2_SPLINE_ARGS.DAY[N2DBIN-1] = SALT2_ERRMAP[imap].DAY[iday] ;
	  SALT2_SPLINE_ARGS.LAM[N2DBIN-1] = SALT2_ERRMAP[imap].LAM[ilam] ;
	  SALT2_SPLINE_ARGS.VALUE[N2DBIN-1] = log10(ERRTMP*ERRTMP) ;

	}// ilam
      } // iday
     
      XM  = (double)N2DBIN ;  SS  = XM ;

      in2dex_(&ispline, &N2DBIN
	      , SALT2_SPLINE_ARGS.DAY
	      , SALT2_SPLINE_ARGS.LAM
	      , SALT2_SPLINE_ARGS.VALUE
	      , SALT2_SPLINE_ARGS.DAYLIM
	      , SALT2_SPLINE_ARGS.LAMLIM
	      , &SS, &IERR ) ; 

      printf("\t Init SPLINE %2d  for error map: %d nodes (IERR=%d) \n", 
	     ispline, N2DBIN, IERR);

      if ( IERR > 0 ) {
	sprintf(c1err,"IN2DEX SPLINE-INIT is bad: IERR=%d", IERR );
	sprintf(c2err,"ispline=%d  SS=%le \n", ispline, SS);
	errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
      }

    
  } // imap

} // end of init_SALT2interp_ERRMAP


// **************************************
void getFileName_SALT2colorDisp(char *fileName) {
  int imap = INDEX_ERRMAP_COLORDISP ;
  sprintf(fileName, "%s/%s", SALT2_MODELPATH, SALT2_ERRMAP_FILES[imap] );
} 


// ***********************************************
void read_SALT2colorDisp(void) {

  // Mar 2, 2011
  // Read color dispersion vs. wavelength 
  //
  // If the color_dispersion  file returns Nrow=0, we have
  // the older Guy07 color-dispersion model, so just hard-wire
  // this map using a 3rd-order polynomial fit.
  // This hard-wire allows reading older SALT2 models
  // without needing to update the color-dispersion map.
  //
  // Sep 6, 2019: 
  //   + start ERRMAP at ilam index=0 (not 1)
  //   + call check_lamRange_SALT2errmap(imap);
  //

  int imap, NLAM, ilam, MXBIN ;
  char tmpFile[MXPATHLEN];

  // define parameters for Guy07 color dispersion model
#define NPOLY_G07 4  // 3rd-order poly fit to G07 color law
  double 
    lam, cDisp, xi,  PTMP
  ,*ptrPoly
  ,G07POLY_NULL[NPOLY_G07] = { 0.0 , 0.0, 0.0, 0.0 }
  ,G07POLY_UB[NPOLY_G07]= {6.2736, -0.43743E-02, 0.10167E-05, -0.78765E-10 }
  ,G07POLY_RI[NPOLY_G07]= {0.53882, -0.19852E-03, 0.18285E-07, -0.81849E-16 }
  ;

  int i;

  // ---------- BEGIN ------------

  imap = INDEX_ERRMAP_COLORDISP ;
  SALT2_ERRMAP[imap].NLAM   = 0 ;  // init to nothing
  SALT2_ERRMAP[imap].LAMMIN = 0.0 ;
  SALT2_ERRMAP[imap].LAMMAX = 0.0 ;

  if ( INPUT_SALT2_INFO.ERRMAP_KCOR_OPT == 0 ) {
    printf("\n  Ignore color-dispersion (KCOR) errors. \n" );
    return ;
  }

  sprintf(tmpFile, "%s/%s", SALT2_MODELPATH, SALT2_ERRMAP_FILES[imap] );

  if ( MXBIN_VAR_SALT2 < MXBIN_LAMSED_SEDMODEL ) 
    { MXBIN = MXBIN_VAR_SALT2-1 ; }        // size of VALUE array
  else 
    { MXBIN = MXBIN_LAMSED_SEDMODEL-1 ; }  // size of LAM array


  rd2columnFile( tmpFile, MXBIN
		,&SALT2_ERRMAP[imap].NLAM
		,SALT2_ERRMAP[imap].LAM
		,SALT2_ERRMAP[imap].VALUE
		);

  NLAM = SALT2_ERRMAP[imap].NLAM ;

  printf("\n  Read color-dispersion vs. lambda from %s \n",
	 SALT2_ERRMAP_FILES[imap] );

  // if nothing was read, then assume we have the older
  // Guy07 model and use polynominal parametrization
  // to hard-wire the color disp.

  if ( NLAM == 0 ) {

    // use binning of nominal surfaces
    NLAM = SALT2_TABLE.NLAMSED;
    SALT2_ERRMAP[imap].NLAM =  NLAM ;

    for ( ilam=0; ilam < NLAM; ilam++ ) {

      lam = SALT2_TABLE.LAMSED[ilam];

      if      ( lam < 4400.0 ) {  ptrPoly = G07POLY_UB   ; }
      else if ( lam < 5500.0 ) {  ptrPoly = G07POLY_NULL ; }
      else                     {  ptrPoly = G07POLY_RI   ; }

      cDisp = 0.0 ;      
      for ( i=0; i < NPOLY_G07 ; i++ ) {
	xi     = (double)i;
	PTMP   = *(ptrPoly+i) ;
	cDisp += ( PTMP * pow(lam,xi) );
      }
      
      SALT2_ERRMAP[imap].LAM[ilam]   = lam ;
      SALT2_ERRMAP[imap].VALUE[ilam] = cDisp ; 
    }

    printf("  Model is pre-G10 => hard-wire G07 color disp. \n");
  }

  SALT2_ERRMAP[imap].LAMMIN = SALT2_ERRMAP[imap].LAM[0] ;
  SALT2_ERRMAP[imap].LAMMAX = SALT2_ERRMAP[imap].LAM[NLAM-1] ;

  check_lamRange_SALT2errmap(imap);

  fflush(stdout);

} // end of read_SALT2colorDisp


// =================================
void read_SALT2_INFO_FILE(void) {

  // March 18, 2010 R.Kessler
  // read SALT2.INFO file, and fill SALT2_INFO structure
  // 
  // Aug  2, 2010: read COLORLAW_VERSION: <version>
  // May  2, 2011: read SEDFLUX_INTERP_OPT 
  // Nov 24, 2011: read MAG_OFFSET
  // Oct 25, 2015: read optional RESTLAM_FORCEZEROFLUX

  char
     infoFile[MXPATHLEN]
    ,c_get[60]
    ,fnam[]        = "read_SALT2_INFO_FILE" 
    ,CHAR_ERROPT[3][20] = { "OFF", "Linear", "Spline" }
  ,CHAR_SEDOPT[3][20] = { "OFF", "Linear", "Spline" }
  ,CHAR_OFFON[2][8]  = { "OFF", "ON" }
  ,ctmp[100]
     ;

  double UVLAM = INPUTS_SEDMODEL.UVLAM_EXTRAPFLUX;

  int     OPT, ipar, NPAR_READ, IVER, i ;

  double *errtmp, *ptrpar;
  FILE *fp;

  // ------- BEGIN ---------

  printf("  Read SALT2 model parameters from  \n\t  %s\n",
	 SALT2_MODELPATH );

  sprintf(infoFile, "%s/%s", SALT2_MODELPATH, SALT2_INFO_FILE );

 
  if (( fp = fopen(infoFile, "rt")) == NULL ) {
    sprintf(c1err,"Could not open SALT2 info file:");
    sprintf(c2err," %s", infoFile );
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err); 
  }

  // init info variables to reasonable  default values

  INPUT_SALT2_INFO.RESTLAMMIN_FILTERCEN   = 2900. ; // Angstroms
  INPUT_SALT2_INFO.RESTLAMMAX_FILTERCEN   = 7000. ;
  INPUT_SALT2_INFO.MAGERR_FLOOR      = 0.005 ; 
  for ( i=0; i< 3; i++ ) {
    INPUT_SALT2_INFO.MAGERR_LAMOBS[i]  = 0.00 ;
    INPUT_SALT2_INFO.MAGERR_LAMOBS[i]  = 0.00 ;
  }

  // SED rebin factors if SEDFLUX_INTERP_OPT=2
  INPUT_SALT2_INFO.INTERP_SEDREBIN_LAM    = 2 ;
  INPUT_SALT2_INFO.INTERP_SEDREBIN_DAY    = 5 ;
  INPUT_SALT2_INFO.SEDFLUX_INTERP_OPT = 2 ; // 1=linear,   2=> Spline
  INPUT_SALT2_INFO.ERRMAP_INTERP_OPT  = 2 ; // 1=>linear,  2=> spline
  INPUT_SALT2_INFO.ERRMAP_KCOR_OPT    = 1 ; // 1=>ON
  INPUT_SALT2_INFO.COLORLAW_VERSION   = IVER = 0;
  INPUT_SALT2_INFO.NCOLORLAW_PARAMS   = 4;
  INPUT_SALT2_INFO.COLOR_OFFSET       = 0.0 ;
  INPUT_SALT2_INFO.MAG_OFFSET         = 0.0 ;

  ptrpar = INPUT_SALT2_INFO.COLORLAW_PARAMS ;
  for ( ipar = 0; ipar < MXCOLORPAR ; ipar++ ) 
    { INPUT_SALT2_INFO.COLORLAW_PARAMS[ipar] = 0.0 ; }

  // Jul 2, 2010: 1st two parameters are reference wavelengths
  INPUT_SALT2_INFO.COLORLAW_PARAMS[0] = B_WAVELENGTH ;
  INPUT_SALT2_INFO.COLORLAW_PARAMS[1] = V_WAVELENGTH ;

  INPUT_SALT2_INFO.RESTLAM_FORCEZEROFLUX[0] = 0.0 ;
  INPUT_SALT2_INFO.RESTLAM_FORCEZEROFLUX[1] = 0.0 ;

  // read info variables

  while( (fscanf(fp, "%s", c_get)) != EOF) {

    if ( strcmp(c_get, "RESTLAMBDA_RANGE:") == 0 ) {
      readdouble(fp, 1, &INPUT_SALT2_INFO.RESTLAMMIN_FILTERCEN );
      readdouble(fp, 1, &INPUT_SALT2_INFO.RESTLAMMAX_FILTERCEN );

      if ( UVLAM > 0.0 )
	{ INPUT_SALT2_INFO.RESTLAMMIN_FILTERCEN = UVLAM + 700.; }
    }


    if ( strcmp(c_get, "COLORLAW_VERSION:") == 0 ) {
      readint(fp, 1, &IVER );
      INPUT_SALT2_INFO.COLORLAW_VERSION = IVER ;

      if ( IVER == 0 ) 
	INPUT_SALT2_INFO.NCOLORLAW_PARAMS  = 4 ;
      else if ( IVER == 1 ) 
	INPUT_SALT2_INFO.NCOLORLAW_PARAMS  = 9 ;
      else {
	sprintf(c1err,"Invalid COLORLAW_VERSION = %d", IVER );
	sprintf(c2err,"Valid versions are 0,1 only");
	errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
      }
    }


    if ( strcmp(c_get, "COLORLAW_PARAMS:") == 0 ||  // new key
	 strcmp(c_get, "COLORCOR_PARAMS:") == 0     // allow old key
	 ) {
      // read all but B,V_WAVELENGTH
      NPAR_READ = INPUT_SALT2_INFO.NCOLORLAW_PARAMS - 2 ; 
      readdouble(fp, NPAR_READ, &INPUT_SALT2_INFO.COLORLAW_PARAMS[2] );
    }
    if ( strcmp(c_get, "COLOR_OFFSET:") == 0 ) {
      readdouble(fp, 1, &INPUT_SALT2_INFO.COLOR_OFFSET );
    }

    if ( strcmp(c_get, "MAG_OFFSET:") == 0 ) {
      readdouble(fp, 1, &INPUT_SALT2_INFO.MAG_OFFSET );
    }
    if ( strcmp(c_get, "MAGERR_FLOOR:") == 0 ) {
      readdouble(fp, 1, &INPUT_SALT2_INFO.MAGERR_FLOOR );
    }
    if ( strcmp(c_get, "MAGERR_LAMOBS:") == 0 ) {
      readdouble(fp, 3, INPUT_SALT2_INFO.MAGERR_LAMOBS );
    }
    if ( strcmp(c_get, "MAGERR_LAMREST:") == 0 ) {
      readdouble(fp, 3, INPUT_SALT2_INFO.MAGERR_LAMREST );
    }

    if ( strcmp(c_get, "ERRMAP_INTERP_OPT:") == 0 ) {
      readint(fp, 1, &INPUT_SALT2_INFO.ERRMAP_INTERP_OPT );
    }

    if ( strcmp(c_get, "SEDFLUX_INTERP_OPT:") == 0 ) {
      readint(fp, 1, &INPUT_SALT2_INFO.SEDFLUX_INTERP_OPT );
    }

    if ( strcmp(c_get, "ERRMAP_KCOR_OPT:") == 0 ) {
      readint(fp, 1, &INPUT_SALT2_INFO.ERRMAP_KCOR_OPT );
    }

    if ( strcmp(c_get, "RESTLAM_FORCEZEROFLUX:") == 0 ) {
      readdouble(fp, 2, INPUT_SALT2_INFO.RESTLAM_FORCEZEROFLUX );
    }

  } // end while


  // transfer filter-lambda range to SEDMODEL struct
  SEDMODEL.RESTLAMMIN_FILTERCEN = INPUT_SALT2_INFO.RESTLAMMIN_FILTERCEN ;
  SEDMODEL.RESTLAMMAX_FILTERCEN = INPUT_SALT2_INFO.RESTLAMMAX_FILTERCEN ;

  // print INFO to screen

  printf("\n  SALT2.INFO \n");
  printf("\t RESTLAMBDA_RANGE:  %6.0f - %6.0f A\n"
	 ,INPUT_SALT2_INFO.RESTLAMMIN_FILTERCEN
	 ,INPUT_SALT2_INFO.RESTLAMMAX_FILTERCEN );

  printf("\t Global MAG OFFSET:  %6.3f mag  \n", 
	 INPUT_SALT2_INFO.MAG_OFFSET ); 

  printf("\t COLOR OFFSET:  %6.3f mag  \n", 
	 INPUT_SALT2_INFO.COLOR_OFFSET ); 

  // dump colorlaw parameters based on version
  printf("\t COLORLAW PARAMS:  \n" );
  printf("\t    B,V_WAVELENGTH = %6.1f , %6.1f \n", 
	 B_WAVELENGTH,  V_WAVELENGTH );

  if ( IVER == 0 ) {
    printf("\t    Polynomial params: %f %f \n", *(ptrpar+2), *(ptrpar+3) );
  }
  else if ( IVER == 1 ) {
    printf("\t    INTERP LAMBDA RANGE: %7.1f - %7.1f \n", 
	   *(ptrpar+2), *(ptrpar+3) );
    printf("\t    Polynomial params: %6.3f %6.3f %6.3f %6.3f \n", 
	   *(ptrpar+5), *(ptrpar+6), *(ptrpar+7), *(ptrpar+8) );
  }

  
  printf("\t MAGERR_FLOOR:  %6.3f mag  \n", INPUT_SALT2_INFO.MAGERR_FLOOR ); 

  errtmp = INPUT_SALT2_INFO.MAGERR_LAMOBS ;
  if ( *errtmp > 0.0 ) {
    printf("\t MAGERR(OBS)  += %6.3f mag for %6.0f  < LAMOBS < %6.0f \n",
	   *(errtmp+0), *(errtmp+1), *(errtmp+2) );
  }

  errtmp = INPUT_SALT2_INFO.MAGERR_LAMREST ;
  if ( *errtmp > 0.0 ) {
    printf("\t MAGERR(REST) = %6.3f mag for %6.0f < LAMREST < %6.0f \n",
	   *(errtmp+0), *(errtmp+1), *(errtmp+2) );
  }

  double  *ptrLam = INPUT_SALT2_INFO.RESTLAM_FORCEZEROFLUX;
  if ( ptrLam[1] > 0.0 ) {
    printf("\t Force flux=0 for %.0f < RESTLAM < %.0f \n",
	   ptrLam[0], ptrLam[1] );
  }

  // ---

  OPT = INPUT_SALT2_INFO.SEDFLUX_INTERP_OPT;
  
  if ( OPT == SALT2_INTERP_LINEAR ) {
    sprintf(ctmp,"%s", CHAR_SEDOPT[OPT] );
  }
  else if ( OPT == SALT2_INTERP_SPLINE ) {
    sprintf(ctmp,"%s  then Linear with LAMSTEP/%d and DAYSTEP/%d"
	    ,CHAR_SEDOPT[OPT]
	    ,INPUT_SALT2_INFO.INTERP_SEDREBIN_LAM
	    ,INPUT_SALT2_INFO.INTERP_SEDREBIN_DAY
	    );
  }
  else {
    sprintf(c1err,"Invalid SEDFLUX_INTERP_OPT = %d", OPT );
    sprintf(c2err,"Check SALT2.INFO file above");
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err);        
  }
  
  printf("\t SEDFLUX_INTERP_OPT:  %d  (%s) \n", OPT, ctmp);


  // ---
  OPT = INPUT_SALT2_INFO.ERRMAP_INTERP_OPT;
  printf("\t ERRMAP_INTERP_OPT:   %d  (%s) \n", OPT, CHAR_ERROPT[OPT] );
  if ( OPT < 0 || OPT > 2 ) {
    sprintf(c1err,"Invalid ERRMAP_INTERP_OPT = %d", OPT );
    sprintf(c2err,"Check SALT2.INFO file above");
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err); 
  }

  OPT = INPUT_SALT2_INFO.ERRMAP_KCOR_OPT;
  printf("\t ERRMAP_KCOR_OPT:     %d  (%s) \n", OPT, CHAR_OFFON[OPT] );


  printf("\n");    fflush(stdout);

} // end of read_SALT2_INFO_FILE


// =========================================
void check_lamRange_SALT2errmap(int imap) {

  // Sep 6 2019
  // If imap>=0, print ERROR message if ERRMAP wave range
  // does not cover SED wave range. Also increment NERRMAP_BAD_SALT2.
  // If imap < 0 && NERRMAP_BAD_SALT2>0, abort.

  double SED_LAMMIN = SALT2_TABLE.LAMMIN ;
  double SED_LAMMAX = SALT2_TABLE.LAMMAX ;

  double ERRMAP_LAMMIN, ERRMAP_LAMMAX ;

  double tol     = 10.0;
  int    DISABLE = 0 ;
  char fnam[] = "check_lamRange_SALT2errmap" ;


  // ----------- BEGIN -------------

  if ( DISABLE ) { return ; }

  if ( imap < 0 ) {
    if ( NERRMAP_BAD_SALT2 > 0 ) {
      sprintf(c1err,"%d ERRMAPs have invalid wavelength range.",
	      NERRMAP_BAD_SALT2 );
      sprintf(c2err,"grep stdout for 'ERRMAP:'  to see all errors.");
      errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
    }
    else
      { return ; }
  }

  ERRMAP_LAMMIN = SALT2_ERRMAP[imap].LAMMIN ;
  ERRMAP_LAMMAX = SALT2_ERRMAP[imap].LAMMAX ;

  if ( ERRMAP_LAMMIN-tol > SED_LAMMIN || ERRMAP_LAMMAX+tol < SED_LAMMAX ) {
    NERRMAP_BAD_SALT2++ ;
    printf("\nERRMAP: WARNING for ERRMAP file %d: %s\n", 
	   imap, SALT2_ERRMAP_FILES[imap] );
    printf("ERRMAP:     SED_LAMRANGE:    %.1f to %.1f A\n", 
	   SED_LAMMIN, SED_LAMMAX);
    printf("ERRMAP:     ERRMAP_LAMRANGE: %.1f to %.1f A "
	   "does not cover SED_LAMRANGE\n", 
	   ERRMAP_LAMMIN, ERRMAP_LAMMAX);       
  }

  return;

} // end check_lamRange_SALT2errmap

// =========================================
void check_dayRange_SALT2errmap(int imap) {

  // Sep 6 2019
  // Give error warning if ERRMAP[imap] day-range does not
  // cover SED day range, and increment NERRMAP_BAD_SALT2.

  double SED_DAYMIN    = SALT2_TABLE.DAYMIN ;
  double SED_DAYMAX    = SALT2_TABLE.DAYMAX ;
  double ERRMAP_DAYMIN = SALT2_ERRMAP[imap].DAYMIN ;
  double ERRMAP_DAYMAX = SALT2_ERRMAP[imap].DAYMAX ;
  double tol = 1.1 ;
  int    DISABLE = 0 ;
  char fnam[] = "check_dayRange_SALT2errmap" ;

  // ----------- BEGIN -------------

  if ( DISABLE ) { return ; }

  if ( ERRMAP_DAYMIN-tol > SED_DAYMIN || ERRMAP_DAYMAX+tol < SED_DAYMAX ) {
    NERRMAP_BAD_SALT2++ ;
    printf("\nERRMAP: WARNING for ERRMAP file: %s\n", 
	   SALT2_ERRMAP_FILES[imap] );
    printf("ERRMAP:     SED_DAYRANGE:    %.1f to %.1f days\n", 
	   SED_DAYMIN, SED_DAYMAX);
    printf("ERRMAP:     ERRMAP_DAYRANGE: %.1f to %.1f days "
	   "does not cover SED_DAYRANGE\n", 
	   ERRMAP_DAYMIN, ERRMAP_DAYMAX);       
  }
  
  return;

} // end check_dayRange_SALT2errmap


// ==========================================================
void init_extrap_latetime_SALT2(void) {

  // Created June 25 2018
  // Init optional mag-extrapolation for epochs later than
  // what is defined in SALT2 model. 
  // Note that default extrapolation is in SED-flux space,
  // extrapolation last few days, but this extrap can have
  // large errors.
  //
  // Input: model_extrap_latetime --> fileName with model info

  char *fileName = INPUT_EXTRAP_LATETIME.FILENAME ;
  char fnam[] = "init_extrap_latetime_SALT2" ;

  int    ipar, ilam, NLAMBIN=0;
  int    NPAR_READ = NPAR_EXTRAP_LATETIME ;
  double DAYMIN, LAM, TAU1, TAU2, EXPRATIO, MAGSLOPE1, MAGSLOPE2, DAYPIVOT;
  double TMPVAL[10];

  FILE *fp;
  char c_get[60];

  // -------------- BEGIN ----------

  INPUT_EXTRAP_LATETIME.NLAMBIN = 0 ;

  if ( IGNOREFILE(fileName) ) { return ; }
  ENVreplace(fileName,fnam,1);

  fp = fopen(fileName,"rt");
  if ( !fp ) {
    sprintf(c1err,"Could not open MODEL_EXTRAP_LATETIME:");
    sprintf(c2err,"%s", fileName);
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
  }

  printf("\n   Read EXTRAP_LATETIME parameters from :\n");
  printf("\t %s \n", fileName);
  fflush(stdout);

  INPUT_EXTRAP_LATETIME.NLAMBIN   = 1 ;
  INPUT_EXTRAP_LATETIME.DAYMIN    = 0.0 ;

  while( (fscanf(fp, "%s", c_get)) != EOF) {

    if ( strcmp(c_get,"EXTRAP_DAYMIN:") == 0 ) 
      { readdouble(fp, 1, &INPUT_EXTRAP_LATETIME.DAYMIN ); }

    if ( strcmp(c_get,"EXTRAP_PARLIST:") == 0 ) { 
      readdouble(fp, NPAR_READ, TMPVAL );
      if ( NLAMBIN < MXLAMBIN_EXTRAP_LATETIME ) {
	for(ipar=0; ipar < NPAR_READ; ipar++ ) 
	  { INPUT_EXTRAP_LATETIME.PARLIST[ipar][NLAMBIN] = TMPVAL[ipar]; } 
      }
      NLAMBIN++ ;
      INPUT_EXTRAP_LATETIME.NLAMBIN = NLAMBIN ;
    }

  }

  fclose(fp);

  if ( NLAMBIN >= MXLAMBIN_EXTRAP_LATETIME ) {
    sprintf(c1err,"NLAMBIN=%d exceeds bound of %d", 
	    NLAMBIN, MXLAMBIN_EXTRAP_LATETIME);
    sprintf(c2err,"Check MXLAMBIN_EXTRAP_LATETIME = %d",
	    MXLAMBIN_EXTRAP_LATETIME);
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 	
  }

  // -----------------------------------------------------
  // prep/check stuff stuff


  NLAMBIN = INPUT_EXTRAP_LATETIME.NLAMBIN ;
  DAYMIN  = INPUT_EXTRAP_LATETIME.DAYMIN  ;

  if ( DAYMIN < 10.0 ) { 
    sprintf(c1err,"Invalid DAYMIN=%.2f (too small)", DAYMIN);
    sprintf(c2err,"Check EXTRAP_DAYMIN key");
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
  }

  // compute a few quantities and print info for each lambin

  printf("\t DAYMIN_EXTRAP = %.1f \n", DAYMIN );
  printf("\n\t FLUX_EXTRAP(t) ~ [ exp(t/TAU1) + RATIO*exp(t/TAU2) ] \n");

  printf("                                TAU1     TAU2     DAY when\n");
  printf("   LAM    TAU1   TAU2   RATIO  mag/day  mag/day    F1=F2 \n");
  printf("   --------------------------------------------------------\n");

  for(ilam=0; ilam < NLAMBIN; ilam++ ) {  
    LAM  = INPUT_EXTRAP_LATETIME.PARLIST[IPAR_EXTRAP_LAM][ilam] ;
    TAU1 = INPUT_EXTRAP_LATETIME.PARLIST[IPAR_EXTRAP_TAU1][ilam] ;
    TAU2 = INPUT_EXTRAP_LATETIME.PARLIST[IPAR_EXTRAP_TAU2][ilam] ;

    if ( TAU2 < TAU1 ) {
      sprintf(c1err,"Invalid TAU2(%.2f) < TAU1(%.2f)", TAU2, TAU1);
      sprintf(c2err,"Check EXTRAP_PARLIST with lam=%.1f", LAM);
      errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
    }

    EXPRATIO = INPUT_EXTRAP_LATETIME.PARLIST[IPAR_EXTRAP_EXPRATIO][ilam] ;

    MAGSLOPE1 = 1.086/TAU1;
    MAGSLOPE2 = 1.086/TAU2;
    if ( EXPRATIO > 1.0E-9 && TAU1 > 0.0 && TAU2 > 0.0 ) 
      { DAYPIVOT  = log(1.0/EXPRATIO) / (1.0/TAU1 - 1.0/TAU2); }
    else
      { DAYPIVOT  = 1.0E4; }

    INPUT_EXTRAP_LATETIME.PARLIST[IPAR_EXTRAP_MAGSLOPE1][ilam] = MAGSLOPE1;
    INPUT_EXTRAP_LATETIME.PARLIST[IPAR_EXTRAP_MAGSLOPE2][ilam] = MAGSLOPE2;
    INPUT_EXTRAP_LATETIME.PARLIST[IPAR_EXTRAP_DAYPIVOT][ilam]  = DAYPIVOT;

    printf(" %7.1f %6.2f %6.2f %6.4f  %6.3f   %6.3f     %.0f \n",
	   LAM, TAU1, TAU2, EXPRATIO,    MAGSLOPE1, MAGSLOPE2, DAYPIVOT);
    fflush(stdout);
  }
  printf("   --------------------------------------------------------\n");


  return ;

} // end init_extrap_latetime_SALT2


// ===============================================
double genmag_extrap_latetime_SALT2(double mag_daymin, double day,
				    double lam ) {

  // Created Jun 25 2018
  // for input mag_daymin, return extrapolated magnitude.
  //
  // Inputs:
  //   mag_daymin = mag (obs or rest) to extrapolate from 'day' 
  //   day        = rest-frame day (day=0 at peak brightness)
  //   lam        = rest-frame wavelength of filter
  //

  int    NLAMBIN = INPUT_EXTRAP_LATETIME.NLAMBIN ;  
  double DAYMIN  = INPUT_EXTRAP_LATETIME.DAYMIN ;  
  double mag_extrap = mag_daymin;

  double arg, F_DAYMIN, F_EXTRAP, VAL, PARLIST[MXPAR_EXTRAP_LATETIME];
  double *ptrLam, *ptrVal;
  int    ipar;
  int    NPAR = NPAR_EXTRAP_LATETIME ;
  int    OPT_INTERP = 1; // linear
  int    LDMP = 0, ABORT=0 ;
  char   fnam[] = "genmag_extrap_latetime_SALT2" ;

  // ----------- BEGIN ---------

  if ( day < DAYMIN ) {
    sprintf(c1err,"Invalid day=%.2f is < DAYMIN=%.2f", day, DAYMIN);
    sprintf(c2err,"day must be > DAYMIN");
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
  }

  // compute flux at daymin
  arg      = 0.4*(mag_daymin - ZEROPOINT_FLUXCAL_DEFAULT);
  F_DAYMIN = pow(10.0,-arg);

  // interpolate each extrap parameter vs. wavelength
  for(ipar=1; ipar < NPAR; ipar++ ) { // skip LAM parameter
   
    ptrLam = INPUT_EXTRAP_LATETIME.PARLIST[IPAR_EXTRAP_LAM] ;
    ptrVal = INPUT_EXTRAP_LATETIME.PARLIST[ipar] ;
    if ( lam < ptrLam[0] ) {
      VAL = ptrVal[0];
    }
    else if ( lam > ptrLam[NLAMBIN-1] ) {
      VAL = ptrVal[NLAMBIN-1];
    }
    else {
      VAL = interp_1DFUN(OPT_INTERP, lam, 
			 NLAMBIN, ptrLam, ptrVal, fnam );
    }
    PARLIST[ipar] = VAL ;
  }


  // ----------
  
  double TAU1  = PARLIST[IPAR_EXTRAP_TAU1] ;
  double TAU2  = PARLIST[IPAR_EXTRAP_TAU2] ;
  double RATIO = PARLIST[IPAR_EXTRAP_EXPRATIO] ;
  double DAYDIF = 0.0 ;
  double FTMP, FNORM ;

  // get reference extrap flux at DAYDIF = DAY-DAYMIN =0
  FTMP  = FLUXFUN_EXTRAP_LATETIME(DAYDIF,TAU1,TAU2,RATIO);
  FNORM = F_DAYMIN / FTMP;

  DAYDIF   = day - DAYMIN ;
  F_EXTRAP = FNORM * FLUXFUN_EXTRAP_LATETIME(DAYDIF,TAU1,TAU2,RATIO);

  mag_extrap = ZEROPOINT_FLUXCAL_DEFAULT - 2.5*log10(F_EXTRAP);

  if ( mag_extrap > 40.0 ) { mag_extrap = MAG_ZEROFLUX; }

  if ( mag_extrap < 0.0 || mag_extrap > 99. ) { ABORT=1; }
  if ( LDMP || ABORT ) {
    printf(" xxx \n");
    printf(" xxx -------- DUMP   %s  ---------- \n", fnam);
    printf(" xxx INPUTS: mag_daymin=%.3f  day=%.3f  lam=%.1f \n",
	   mag_daymin, day, lam);
    printf(" xxx TAU1=%.3f  TAU2=%.3f  RATIO=%.5f \n", 
	   TAU1, TAU2, RATIO );
    printf(" xxx F_DAYMIN = %f   FLUXFUN_EXTRAP(0)=%f \n",
	   F_DAYMIN, FTMP);
    printf(" xxx DAYDIF=%.2f  F_EXTRAP=%f  --> mag_extrap=%.3f \n",
	   DAYDIF, F_EXTRAP, mag_extrap);

    if ( ABORT ) {
      sprintf(c1err,"Crazy mag_extrap = %le", mag_extrap);
      sprintf(c2err,"Check above DUMP");
      errmsg(SEV_FATAL, 0, fnam, c1err, c2err );     
    }

  }


  return(mag_extrap);

} // end genmag_extrap_latetime_SALT2


double FLUXFUN_EXTRAP_LATETIME(double t, double tau1, double tau2, 
			       double ratio) {
  double F1 = exp(-t/tau1);
  double F2 = ratio * exp(-t/tau2);
  double F  = F1 + F2;
  return(F);
} 

/**********************************************
  SALT-II color correction formula
**********************************************/
double SALT2colorCor(double lam_rest, double c ) {

  // Compute/return flux-correction from color-law.
  // The color-law version 'IVER' is from the SALT2.INFO file.
  //
  // Jul 2, 2010: 
  // replace formula with a call to generic SALT2colorlaw0 function

  double cc ;
  int IVER;
  char fnam[] = "SALT2colorCor" ;

  // -------- BEGIN ---------


  IVER = INPUT_SALT2_INFO.COLORLAW_VERSION ;
  cc   = c - INPUT_SALT2_INFO.COLOR_OFFSET ;

  if ( IVER == 0 ) {
    return SALT2colorlaw0(lam_rest, cc, INPUT_SALT2_INFO.COLORLAW_PARAMS );
  }
  else if ( IVER == 1 ) {
    return SALT2colorlaw1(lam_rest, cc, INPUT_SALT2_INFO.COLORLAW_PARAMS );
  }
  else {
    sprintf(c1err,"Invalid COLORLAW_VERSION = %d", IVER );
    sprintf(c2err,"Valid versions are 0,1 only");
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err ); 
    return(-9.0);
  }

  return(-9.0);

} // end of SALT2colorCor



// ****************************************************************
void genmag_SALT2(
		  int OPTMASK     // (I) bit-mask of options (LSB=0)
		  ,int ifilt_obs  // (I) absolute filter index
		  ,double x0      // (I) SALT2 x0 parameter
		  ,double x1      // (I) SALT2 x1-stretch parameter
		  ,double x1_forErr // (I) x1 used for error calc.
		  ,double c       // (I) SALT2 color parameter 
		  ,double mwebv   // (I) Galactic extinction: E(B-V)
		  ,double RV_host // (I) for Mandel SALT2+XThost model
		  ,double AV_host // (I) for Mandel SALT2+XThost model
		  ,double z       // (I) Supernova redshift
		  ,double z_forErr// (I) z used for error calc (Mar 2018)
		  ,int Nobs       // (I) number of epochs
		  ,double *Tobs_list   // (I) list of obs times (since mB max) 
		  ,double *magobs_list  // (O) observed mag values
		  ,double *magerr_list  // (O) model mag errors
		  ) {

  /****
  Return observer frame mag in absolute filter index "ifilt_obs" 
  for input SALT2 parameters.

   OPTMASK=1 => return flux instead of mag ; magerr still in mag.
                     (to avoid discontinuity for negative flux)

   OPTMASK=2 => print warning message when model flux < 0

   OPTMASK=4 => set errors to zero  (July 2013)

  May 1, 2011: in addition to major change to use brute-force integration,
               fix bug in calculation of relx1.


  Jan 27, 2013: fix Trest logic for EXTRAPFLAG,
    Trest <  SALT2_TABLE.DAYMIN  -> Trest <=  SALT2_TABLE.DAYMIN+epsT 
    Trest >  SALT2_TABLE.DAYMAX  -> Trest >=  SALT2_TABLE.DAYMAX-epsT

 Dec 27 2014: fix aweful extrapolation bug (EXTRAPFLAG>0);
              was causing constant flux/mag when Trest > model range.

 Oct 25 2015: check option to force flux=0 for specific restLam range.
              See  INPUT_SALT2_INFO.RESTLAM_FORCEZEROFLUX.

 July 2016: add arguments RV_Host and AV_host
 
 Jun 25 2018: check mag-extrap option for late times

  ***/

  double 
    meanlam_obs,  meanlam_rest, ZP, z1
    ,Tobs, Tobs_interp, Trest, Trest_interp, flux, flux_interp
    ,arg, magerr, Finteg, Finteg_ratio, FspecDum[10]
    ,lamrest_forErr, Trest_forErr, z1_forErr, magobs
    ;

  double
     fluxmin = 1.0E-30
    ,epsT    = 1.0E-5
    ;

  char *cfilt;
  int  ifilt, epobs, EXTRAPFLAG_SEDFLUX, EXTRAPFLAG_DMP = 0 ;
  int  EXTRAPFLAG_MAG ;
  int  LDMP_DEBUG,OPT_PRINT_BADFLUX, OPT_RETURN_MAG ;
  int  OPT_RETURN_FLUX, OPT_DOERR ;    

  char fnam[] = "genmag_SALT2" ;

  // ----------------- BEGIN -----------------

  // parse bit-mask options

  OPT_PRINT_BADFLUX = OPT_RETURN_FLUX =  0; // default flags OFF
  OPT_RETURN_MAG = OPT_DOERR = 1 ;       // default flags on
  LDMP_DEBUG=0;

  if ( (OPTMASK & 1)  ) { OPT_RETURN_MAG    = 0 ; OPT_RETURN_FLUX = 1; }
  if ( (OPTMASK & 2)  ) { OPT_PRINT_BADFLUX = 1 ; }
  if ( (OPTMASK & 4)  ) { OPT_DOERR         = 0 ; } // Jul 2013
  if ( (OPTMASK & 8)  ) { LDMP_DEBUG        = 1 ; }

  // translate absolute filter index into sparse index
  ifilt = IFILTMAP_SEDMODEL[ifilt_obs] ;
  z1    = 1. + z ;

  // filter info for this "ifilt"
  meanlam_obs  = FILTER_SEDMODEL[ifilt].mean ;  // mean lambda
  ZP           = FILTER_SEDMODEL[ifilt].ZP ;
  cfilt        = FILTER_SEDMODEL[ifilt].name ;
  meanlam_rest = meanlam_obs/z1 ;

  // make sure filter-lambda range is valid
  checkLamRange_SEDMODEL(ifilt,z,fnam);

  // store info for Galactic & host extinction
  fill_TABLE_MWXT_SEDMODEL(MWXT_SEDMODEL.RV, mwebv);
  fill_TABLE_HOSTXT_SEDMODEL(RV_host, AV_host, z);   // July 2016

  //determine integer times which sandwich the times in Tobs

  for ( epobs=0; epobs < Nobs; epobs++ ) {

    Tobs = Tobs_list[epobs];
    Trest = Tobs / z1 ;

    EXTRAPFLAG_MAG = EXTRAPFLAG_SEDFLUX = 0 ; 
    Trest_interp = Trest; 

    if ( Trest <= SALT2_TABLE.DAYMIN+epsT )
      { EXTRAPFLAG_SEDFLUX = -1;  Trest_interp = SALT2_TABLE.DAYMIN+epsT ; }
    else if ( Trest >= SALT2_TABLE.DAYMAX-epsT ) 
      { EXTRAPFLAG_SEDFLUX = +1;  Trest_interp = SALT2_TABLE.DAYMAX-epsT ; }


    // check mag-extrap option for late times (June 25 2018)
    if ( INPUT_EXTRAP_LATETIME.NLAMBIN && 
	 Trest > INPUT_EXTRAP_LATETIME.DAYMIN ) { 
      Trest_interp = INPUT_EXTRAP_LATETIME.DAYMIN ;
      EXTRAPFLAG_SEDFLUX = 0 ; // turn off SEDFLUX extrapolation
      EXTRAPFLAG_MAG     = 1 ;
    }
   

    // brute force integration
    Tobs_interp = Trest_interp * z1 ;
    INTEG_zSED_SALT2(0,ifilt_obs, z, Tobs_interp, x0,x1,c, RV_host, AV_host,
		     &Finteg, &Finteg_ratio, FspecDum ); // returned
    flux_interp = Finteg ;

    // ------------------------

    if ( EXTRAPFLAG_SEDFLUX == 0 ) { 
      flux = flux_interp ;
    }
    else {
      // SED flux extrapolation
      double Trest_edge, Trest_tmp, flux_edge, flux_tmp, Tobs_tmp ;
      double slope_flux ;
      double nday_slope  = 3.*(double)EXTRAPFLAG_SEDFLUX ;

      // measure slope dTrest/dFlux using last nday_slope days of model
      Trest_edge = Trest_interp ;
      Trest_tmp  = Trest_edge - nday_slope ;
      flux_edge  = flux_interp ;
      Tobs_tmp   = Trest_tmp * z1 ;
      INTEG_zSED_SALT2(0,ifilt_obs,z,Tobs_tmp, x0,x1,c, RV_host,AV_host,
		       &Finteg, &Finteg_ratio, FspecDum ); // returned
      flux_tmp = Finteg;
      
      slope_flux = -(flux_tmp - flux_edge)/nday_slope ;

      // extrapolate model
      flux = modelflux_extrap( Trest, Trest_edge, 
			       flux_edge, slope_flux, EXTRAPFLAG_DMP ) ;
    }

    // -------------------
    // Oct 25 2015: check option to force flux to zero
    if ( meanlam_rest > INPUT_SALT2_INFO.RESTLAM_FORCEZEROFLUX[0] &&
	 meanlam_rest < INPUT_SALT2_INFO.RESTLAM_FORCEZEROFLUX[1] ) {
      flux = 0.0 ;
    }

    // ------------------------
    if ( flux <= fluxmin || isnan(flux) ) {

      if ( OPT_PRINT_BADFLUX ) {
	printf("  genmag_SALT2 Warning:");
	printf(" Flux(%s)<0 at Trest = %6.2f => return mag=99 \n",
	     cfilt, Trest );
      }
      magobs = MAG_ZEROFLUX ;
    }
    else{
      magobs = ZP - 2.5*log10(flux) + INPUT_SALT2_INFO.MAG_OFFSET ;
      if ( EXTRAPFLAG_MAG ) {
	magobs = genmag_extrap_latetime_SALT2(magobs,Trest,meanlam_rest);
      }
    }
    
    // check option to return flux intead of mag;
    // preserves continutity for negative model fluxes.
    if ( OPT_RETURN_FLUX ) 
      { arg = -0.4 * magobs;  magobs = pow(TEN,arg);  }


    // load output array
    magobs_list[epobs] = magobs ;

    // ------------- DEBUG DUMP ONLY ------------------
    //    LDMP_DEBUG = ( ifilt_obs == 1 && magobs > 90.0 ) ;

    if ( LDMP_DEBUG ) {
      printf("\n xxxx ================================================= \n");
      printf(" xxxx genmag_SALT2 dump \n" ) ;
      printf(" xxxx Trest(%s) = %6.2f   LAMrest = %6.0f  z=%6.4f\n", 
	     cfilt, Trest, meanlam_rest, z );
      printf(" xxxx flux=%f   mag=%f   OPT_RETURN_FLUX=%d \n", 
	     flux, magobs_list[epobs], OPT_RETURN_FLUX );
      printf(" xxxx x1=%6.3f  c=%6.3f  Finteg=%9.3le   \n", 
	     x1, c, Finteg ) ;
      printf(" xxxx ZP=%f  mwebv=%f \n", ZP, mwebv);
      printf(" xxxx colorCor = %f\n",  SALT2colorCor(meanlam_rest,c) ) ;
      fflush(stdout);
    }
    // -------- END OF DEBUG DUMP  ------------

    // get the mag error and pass the LDMP_DEBUG flag from above.
    if ( OPT_DOERR ) {
      z1_forErr      = (1.0 + z_forErr);
      Trest_forErr   = Tobs / z1_forErr ;
      lamrest_forErr = meanlam_obs / z1_forErr ;
      magerr = SALT2magerr(Trest_forErr, lamrest_forErr, z_forErr,
			   x1_forErr, Finteg_ratio, LDMP_DEBUG );
    }
    else
      { magerr = 0.0 ; }

    // load magerr onto output list
    magerr_list[epobs] = magerr ;  // load error to output array


  } // end epobs loop over epochs


  return ;

} // end of genmag_SALT2


// *****************************************
double SALT2magerr(double Trest, double lamRest, double z,
		   double x1, double Finteg_ratio, int LDMP ) {

  // Created Jun 2011 by R.Kessler
  // return mag-error for this epoch and rest-frame <lamRest>.
  //
  // Inputs:
  //   - Trest   : rest-frame epoch relative to peak brightness (days)
  //   - lamRest : <lamObs>/(1+z) = mean wavelength in rest-frame
  //   - z       : redshift
  //   - x1      : stretch parameter.
  //   - Finteg_ratio : flux-ratio between the surfaces,
  //                     Finteg[1] / Finteg[0]
  //
  //   - LDMP : dump-and-exit flag
  //
  // Nov 7 2019: for SALT3 (retraining), set relx1=0. We don't understand
  //             the origin of this term, so scrap it for SALT3.
  // 
  double 
     ERRMAP[NERRMAP]
    ,Trest_tmp
    ,vartot, var0, var1
    ,relsig0, relsig1, relx1
    ,covar01, rho, errscale
    ,fracerr_snake
    ,fracerr_kcor
    ,fracerr_TOT
    ,magerr_model, magerr
    ,lamObs
    ,ONE = 1.0
    ;

  char fnam[] = "SALT2magerr" ;

  // ---------------- BEGIN ---------------
  
  // Make sure that Trest is within range of map.

  if ( Trest > SALT2_ERRMAP[0].DAYMAX ) 
    { Trest_tmp = SALT2_ERRMAP[0].DAYMAX ; }
  else if ( Trest < SALT2_ERRMAP[0].DAYMIN ) 
    { Trest_tmp = SALT2_ERRMAP[0].DAYMIN ; }
  else
    { Trest_tmp = Trest ; }

  get_SALT2_ERRMAP(Trest_tmp, lamRest, ERRMAP ) ;

  // strip off the goodies
  var0     = ERRMAP[INDEX_ERRMAP_VAR0] ;  // sigma(S0)/S0
  var1     = ERRMAP[INDEX_ERRMAP_VAR1] ;  // sigma(S1)/S0
  covar01  = ERRMAP[INDEX_ERRMAP_COVAR01] ;  // 
  errscale = ERRMAP[INDEX_ERRMAP_SCAL] ;  // error fudge  

  relx1    = x1 * Finteg_ratio ;
  if ( ISMODEL_SALT3 ) { relx1 = 0.0 ; } 

  // compute fractional error as in  Guy's ModelRelativeError function
  vartot  = var0 + var1*x1*x1 + (2.0 * x1* covar01) ;
  //  if ( vartot < 0 ) { vartot = -vartot ; }  // protect in wierd cases
  if ( vartot < 0 ) { vartot = 0.01*0.01 ; }  // Jul 9 2013: follow JG 
  
  fracerr_snake = errscale * sqrt(vartot)/fabs(ONE + relx1) ;   
  fracerr_kcor  = SALT2colorDisp(lamRest,fnam); 

  // get total fractional  error.
  fracerr_TOT = sqrt( pow(fracerr_snake,2.0) + pow(fracerr_kcor,2.0) ) ;

  // convert frac-error to mag-error, and load return array
  if ( fracerr_TOT > .999 ) 
    { magerr_model = 5.0 ; }
  else  { 
    // magerr_model  = -2.5*log10(arg) ;  // dumb approx
    magerr_model  = (2.5/LNTEN) * fracerr_TOT ;  // exact, Jul 12 2013
  }

  // check for error fudges
  lamObs = lamRest * ( 1. + z );
  magerr = magerrFudge_SALT2(magerr_model, lamObs, lamRest );


  // ------------- DEBUG DUMP ONLY ------------------
  if ( LDMP ) {
    relsig0 = sqrt(var0);
    relsig1 = sqrt(var1);
    rho = covar01 / (relsig0*relsig1);

    // printf("\n xxxx ================================================= \n");
    printf(" xxxx \t SALT2magerr dump \n" );

    printf(" xxxx Trest=%6.2f  lamRest = %6.0f   z=%6.4f\n", 
	   Trest, lamRest, z );

    printf(" xxxx var0=%le  var1=%le  vartot=%le  \n", 
	   var0, var1, vartot );

    printf(" xxxx relsig0=%f  relsig1=%f  rho=%f  scale=%f\n", 
	   relsig0, relsig1, rho, errscale );

    printf(" xxxx fracerr[snake,kcor] = %f , %f \n", 
	   fracerr_snake, fracerr_kcor );

    printf(" xxxx fracerr_TOT=%f   x1*S1/S0=%f \n", 
	   fracerr_TOT, relx1 );
    printf(" xxxx magerr(model,final) = %7.3f , %7.3f \n", 
	   magerr_model, magerr );

    //    debugexit("SALT2 MODEL DUMP");

  }
    // -------- END OF DEBUG DUMP  ------------

  return magerr ;

} // end of SALT2magerr


// **********************************************
double magerrFudge_SALT2(double magerr_model,
			 double meanlam_obs, double meanlam_rest ) {

  // Mar 01, 2011 R.Kessler
  // return fudged magerr for requested fudges.
  // If no fudges are defined or used, return magerr_model
  //
  // May 11 2013: arg, add fudged errors in quadrature to magerr_model
  //              instead of replacing error.
  //          

  double magerr, floor, magerr_add, sqerr ;

  // ------- BEGIN --------

  magerr = magerr_model ; // default error is input model error


  // check global floor on error
  floor = INPUT_SALT2_INFO.MAGERR_FLOOR;
  if ( magerr < floor )  { magerr = floor ; }

  // check obs-frame fudge
  if ( meanlam_obs >= INPUT_SALT2_INFO.MAGERR_LAMOBS[1] &&
       meanlam_obs <= INPUT_SALT2_INFO.MAGERR_LAMOBS[2] ) {
    magerr_add = INPUT_SALT2_INFO.MAGERR_LAMOBS[0] ;
    sqerr  = magerr_model*magerr_model + magerr_add*magerr_add ;
    magerr = sqrt(sqerr);
  }

  // check rest-frame fudge
  if ( meanlam_rest >= INPUT_SALT2_INFO.MAGERR_LAMREST[1] &&
       meanlam_rest <= INPUT_SALT2_INFO.MAGERR_LAMREST[2] ) {
    magerr_add = INPUT_SALT2_INFO.MAGERR_LAMREST[0] ;
    sqerr = magerr_model*magerr_model + magerr_add*magerr_add ;
    magerr = sqrt(sqerr);
  }
       
  return magerr ;

} // end of magerrFudge_SALT2



// **********************************************
void INTEG_zSED_SALT2(int OPT_SPEC, int ifilt_obs, double z, double Tobs, 
		      double x0, double x1, double c,
		      double RV_host, double AV_host,
		      double *Finteg, double *Fratio, double *Fspec ) {

  // May 2011
  // obs-frame integration of SALT2 flux.
  // Returns Finteg that includex  filter-trans, SALT2 SEDs, 
  // color-law, and Galactic extinction.
  // This routine samples each filter-transmission
  // grid-point, and should give a better result
  // than integrating over SED-lambda (integSALT2_SEDFLUX),
  // particularly at high redshifts.
  //
  // Do linear interpolation here.
  // Optional splines are done in fill_SALT2_TABLE_SED
  // where the SEDs are stored with finer binning so that
  // linear interp is adequate here.
  //
  // OPT_SPEC > 0 --> return Fspec spectrum within filter band.
  //                  Array size = size of filter-trans array.
  //
  // ------------ HISTORY --------------
  //
  // Jul 25 2013:   Inside ilamobs loop, if ( TRANS < 1.0E-12 ) { continue ; }
  // Jan 21, 2014: fix sign error for instrinsic smear;
  //               no effect for symmetric models, but might affect
  //               asymmetric smearing models. See FSMEAR
  //
  // May 2014: return Fratio used for error calc (without MWEBV)
  // Jul 2016: 
  //   +remove un-used mwebv argument, and add RV_host & AV_host args.
  //   +Compute XTHOST_FRAC = host-galaxy extinction.
  //   + refactor to apply x0 & x1 to comput total flux.
  //     The two SED fluxe,  Finteg[2], are now replaced with
  //       Finteg = x0*(Finteg_old[0] + x1 * Finteg[1])
  //     Fratio still has the same meaning as before.
  //  
  // Nov 10 2016: fix DO_SPEC bug that is probably benign.
  // Nov 19 2016: add OPT_SPEC input
  // Dec 21 2016: bugfix for OPT_SPEC; TRANS=1 for Finteg_spec,
  //              not for Finteg_filter. Should not affect returned
  //              spectrum, but only affects local Finteg 
  //
  // Jan 16 2017: 
  //   +  remove LAMSED factor from spectrum 
  //   + little refactor with Fbin_forFlux and Fbin_forSpec
  //
  // Mar 29 2019: fix Fspec normalization factor of hc
  //
  // Apr 23 2019: remove buggy z1 factor inside OPT_SPEC
  //              (caught by D.Jones)
  //
  // Jan 19 2020:
  //   replace local magSmear[ilam] with global GENSMEAR.MAGSMEAR_LIST
  //   so that it works properly with repeat function.

  int  
    ifilt, NLAMFILT, ilamobs, ilamsed, jlam
    ,IDAY, NDAY, nday, iday, ised, ic
    ,ISTAT_GENSMEAR, LABORT, LDMP
    ;

  double
    LAMOBS, LAMSED, z1, LAMDIF, LAMSED_MIN, LAMSED_MAX
    ,LAMFILT_STEP, LAMSED_STEP, LAMSPEC_STEP, LAMRATIO
    ,DAYSTEP, DAYMIN, DAYDIF, Trest
    ,MWXT_FRAC, HOSTXT_FRAC, CCOR, CCOR_LAM0, CCOR_LAM1, CDIF, CNEAR
    ,FRAC_INTERP_DAY, FRAC_INTERP_COLOR, FRAC_INTERP_LAMSED
    ,TRANS, MODELNORM_Fspec, MODELNORM_Finteg, *ptr_FLUXSED[2][4] 
    ,FSED[4], FTMP, FDIF, VAL0, VAL1, mean, arg, FSMEAR
    ,lam[MXBIN_LAMFILT_SEDMODEL]
    ,Finteg_filter[2], Finteg_forErr[2], Finteg_spec[2]
    ,Fbin_forFlux, Fbin_forSpec
    ,hc8 = (double)hc ;

  int  DO_SPECTROGRAPH = ( ifilt_obs == JFILT_SPECTROGRAPH ) ;

  char *cfilt ;
  char fnam[] = "INTEG_zSED_SALT2" ;

  // ----------- BEGIN ---------------

  *Fratio   = 0.0 ;
  *Finteg   = 0.0 ;
  Fspec[0] = 0.0 ; // init only first element

  Finteg_filter[0]  = 0.0 ;
  Finteg_filter[1]  = 0.0 ;
  Finteg_forErr[0] = 0.0 ;
  Finteg_forErr[1] = 0.0 ;

  ifilt     = IFILTMAP_SEDMODEL[ifilt_obs] ;
  NLAMFILT  = FILTER_SEDMODEL[ifilt].NLAM ;
  cfilt     = FILTER_SEDMODEL[ifilt].name ;
  z1        = 1. + z ;
  Trest     = Tobs/z1 ;

  LAMFILT_STEP = FILTER_SEDMODEL[ifilt].lamstep; 
  LAMSED_STEP  = SALT2_TABLE.LAMSTEP ;    // step size of SALT2 model

  // Compute flux normalization factor.
  // Note that the 1+z factor is missing because the integration 
  // is over observer-lambda instead of lambda-rest.
  MODELNORM_Fspec  = LAMFILT_STEP * SEDMODEL.FLUXSCALE ;
  MODELNORM_Finteg = LAMFILT_STEP * SEDMODEL.FLUXSCALE / hc8 ;

  // for SED find rest-frame 'iday' and DAYFRAC used to 
  // interpolate SED in TREST-space.
  DAYSTEP = SALT2_TABLE.DAYSTEP ;
  DAYMIN  = SALT2_TABLE.DAY[0]  ;
  DAYDIF  = Trest - DAYMIN ;
  IDAY    = (int)(DAYDIF/DAYSTEP);  
  NDAY    = SALT2_TABLE.NDAY ;
  DAYDIF  = Trest - SALT2_TABLE.DAY[IDAY] ;

  nday    = 2 ; 
  FRAC_INTERP_DAY = DAYDIF/DAYSTEP ;
  
  // get color-index needed for interpolation of table.
  CDIF  = c - SALT2_TABLE.CMIN ;
  ic    = (int)(CDIF / SALT2_TABLE.CSTEP) ;
  // make sure that 'ic' is within bounds.
  if ( ic < 0 ) 
    { ic = 0 ; }
  if ( ic > SALT2_TABLE.NCBIN - 2 ) 
    { ic = SALT2_TABLE.NCBIN - 2 ; }

  CNEAR = SALT2_TABLE.COLOR[ic] ;
  FRAC_INTERP_COLOR = (c - CNEAR)/SALT2_TABLE.CSTEP ;

  // get rest-frame SED pointers
  for(ised=0; ised<=1; ised++ ) {
    for ( iday=0; iday<nday; iday++ ) {
      ptr_FLUXSED[ised][iday] = SALT2_TABLE.SEDFLUX[ised][IDAY+iday] ;
    }
  }

  // evaluate optional smearing from function
  // Should be used only for simulation (not for fitting mode)
  ISTAT_GENSMEAR = istat_genSmear();  
  if ( ISTAT_GENSMEAR  ) {
    //  printf(" xxx %s: z=%.3f ifilt_obs=%d \n", fnam, z, ifilt_obs); 
    int NLAMTMP = 0 ;
    for ( ilamobs=0; ilamobs < NLAMFILT; ilamobs++ ) {
      LAMOBS       = FILTER_SEDMODEL[ifilt].lam[ilamobs] ;
      LAMSED       = LAMOBS/z1;   // rest-frame wavelength
      lam[ilamobs] = LAMSED ; 

      // protect undefined red end for low-z (July 2016)
      if ( LAMSED >= SALT2_TABLE.LAMMAX ) { continue ; }       
      NLAMTMP++ ;
    }
    // xxx mark delete    get_genSmear( Trest, NLAMTMP, lam, magSmear) ;
    get_genSmear( Trest, NLAMTMP, lam, GENSMEAR.MAGSMEAR_LIST) ;
  }


  // Loop over obs-filter lambda-bins. XTMW has the same binning,
  // but the color and SED flux must be interpolated.

  for ( ilamobs=0; ilamobs < NLAMFILT; ilamobs++ ) {

    TRANS  = FILTER_SEDMODEL[ifilt].transSN[ilamobs] ;

    if ( TRANS < 1.0E-12 && OPT_SPEC==0) 
      { continue ; } // Jul 2013 - skip zeros for leakage

    MWXT_FRAC  = SEDMODEL_TABLE_MWXT_FRAC[ifilt][ilamobs] ;

    // July 2016: check for host extinction.    
    if( RV_host > 1.0E-9 && AV_host > 1.0E-9 ) 
      { HOSTXT_FRAC = SEDMODEL_TABLE_HOSTXT_FRAC[ifilt][ilamobs] ; }
    else 
      { HOSTXT_FRAC = 1.0 ; } // standard SALT2 model has no host extinction

    LAMOBS     = FILTER_SEDMODEL[ifilt].lam[ilamobs] ;
    LAMSED     = LAMOBS / z1 ;  // rest-frame lambda
    LAMSED_MIN = LAMSED_MAX = LAMSED ;  // default is no sub-bins 

    LDMP = 0; // (OPT_SPEC>0 && ifilt_obs==2 );

    // check spectrum options
    if ( OPT_SPEC > 0 ) {
      for(ised=0; ised<=1; ised++ ) { Finteg_spec[ised] = 0.0 ; }
      if ( DO_SPECTROGRAPH )  {
	// prepare sub-bins since SPECTROGRAPH bins can be large
	LAMSED_MIN = SPECTROGRAPH_SEDMODEL.LAMMIN_LIST[ilamobs]/z1 ; 
	LAMSED_MAX = SPECTROGRAPH_SEDMODEL.LAMMAX_LIST[ilamobs]/z1 ;
      }
    } // end OPT_SPEC

    // loop over rest-frame lambda (for SPECTROGRAPH)
    for(LAMSED = LAMSED_MIN; LAMSED <= LAMSED_MAX; LAMSED+=LAMSED_STEP ) {

      // bail if outside model range 
      if ( LAMSED <= SALT2_TABLE.LAMMIN ) { continue ; }
      if ( LAMSED >= SALT2_TABLE.LAMMAX ) { continue ; } 

      // get rest-frame lambda index and interp-fraction for SED space
      LAMDIF  = LAMSED - SALT2_TABLE.LAMMIN ;
      ilamsed = (int)(LAMDIF/LAMSED_STEP);
      LAMDIF  = LAMSED - SALT2_TABLE.LAMSED[ilamsed] ;
      FRAC_INTERP_LAMSED = LAMDIF / LAMSED_STEP ; // 0-1

      if ( LDMP ) { 
	printf(" xxx -------------- %s DUMP ------------- \n", fnam ); 
	printf(" xxx LAMOBS=%.1f  LAMSED=%.2f \n", LAMOBS, LAMSED ); 
	printf(" xxx FRAC_INTERP_[CCOR,LAMSED] = %.3f , %.3f \n",	       
	       FRAC_INTERP_COLOR , FRAC_INTERP_LAMSED ); 
	printf(" xxx Tobs=%.3f  Trest=%.3f \n", Tobs, Trest);
	fflush(stdout);
      }
      
      LABORT = ( FRAC_INTERP_LAMSED < -1.0E-8 || 
		 FRAC_INTERP_LAMSED > 1.0000000001 ) ;

      if ( LABORT ) {
	mean = FILTER_SEDMODEL[ifilt].mean ;
	print_preAbort_banner(fnam);
	printf("\t LAMOBS = %7.2f  LAMDIF=%7.2f\n",  LAMOBS, LAMDIF);
	printf("\t LAMSED = LAMOBS/(1+z) = %7.2f \n", LAMSED );
	printf("\t LAMSTEP=%4.1f  LAMMIN=%6.1f \n", 
	       LAMSED_STEP, SALT2_TABLE.LAMMIN );
	printf("\t ilamobs=%d   ilamsed = %d \n", 	     
	       ilamobs, ilamsed );
	printf("\t Tobs=%f  Trest=%f \n", Tobs, Trest);
	printf("\t <LAMFILT(%s)> = %7.2f(OBS)  %7.2f(REST) \n", 
	       cfilt, mean, mean/z1);
	for( jlam=ilamsed-2; jlam <= ilamsed+2; jlam++ ) {
	  printf("\t SALT2_TABLE.LAMSED[ilamsed=%d] = %f\n", 
		 jlam, SALT2_TABLE.LAMSED[jlam] ); 
	}
	sprintf(c1err,"Invalid FRAC_INTERP_LAMSED=%le ", 
		FRAC_INTERP_LAMSED );
	sprintf(c2err,"check Tobs(%s)=%6.2f at z=%5.3f  c=%6.3f",
		cfilt, Tobs, z, c);
	errmsg(SEV_FATAL, 0, fnam, c1err, c2err); 
      }

      // interpolated color correction in 2D space of color and LAMSED
      VAL0  = SALT2_TABLE.COLORLAW[ic+0][ilamsed];
      VAL1  = SALT2_TABLE.COLORLAW[ic+1][ilamsed];
      CCOR_LAM0  = VAL0 + (VAL1-VAL0) * FRAC_INTERP_COLOR ;
      
      VAL0  = SALT2_TABLE.COLORLAW[ic+0][ilamsed+1];
      VAL1  = SALT2_TABLE.COLORLAW[ic+1][ilamsed+1];
      CCOR_LAM1  = VAL0 + (VAL1-VAL0) * FRAC_INTERP_COLOR ;
      
      CCOR = CCOR_LAM0 + (CCOR_LAM1-CCOR_LAM0)*FRAC_INTERP_LAMSED ;

      // interpolate SED Fluxes to LAMSED
      for(ised=0; ised<=1; ised++ ) {
	for ( iday=0; iday<nday; iday++ ) {
	  
	  VAL0 = ptr_FLUXSED[ised][iday][ilamsed+0] ;
	  VAL1 = ptr_FLUXSED[ised][iday][ilamsed+1] ;
	  FSED[iday] = VAL0 + (VAL1-VAL0)*FRAC_INTERP_LAMSED ;
	  if ( LDMP ) {
	    printf(" xxx ised=%d iday=%d : VAL0,1=%f,%f  FSED=%f \n",
		   ised, iday, VAL0, VAL1, FSED[iday] );
	  }
	} // iday	
	
	FDIF = FSED[1] - FSED[0] ;
	FTMP = FSED[0] + FDIF * FRAC_INTERP_DAY ; 
	
	// check option to smear SALT2 flux with intrinsic scatter
	if ( ISTAT_GENSMEAR ) {
	  // xxx mark delete  arg   =  -0.4*magSmear[ilamobs] ; 
	  arg     =  -0.4*GENSMEAR.MAGSMEAR_LIST[ilamobs] ; 
	  FSMEAR  =  pow(TEN,arg)  ;        // fraction change in flux
	  FTMP   *=  FSMEAR;                // adjust flux for smearing
	}
	
	// update integral for each SED surface
	Fbin_forFlux = (FTMP * CCOR * HOSTXT_FRAC*MWXT_FRAC * LAMSED*TRANS);
	Fbin_forSpec = (FTMP * CCOR * HOSTXT_FRAC*MWXT_FRAC );

	if ( OPT_SPEC ) { 
	  LAMSPEC_STEP = LAMFILT_STEP ; // default for filters

	  // switch to SED bin size for SPECTROGRAPH
	  if ( DO_SPECTROGRAPH ) {
	    if ( LAMSED+LAMSED_STEP < LAMSED_MAX ) 
	      { LAMSPEC_STEP = LAMSED_STEP  ; } // obs-frame lamStep
	    else
	      { LAMSPEC_STEP = (LAMSED_MAX-LAMSED) ; }
	  }

	  LAMRATIO            = LAMSPEC_STEP/LAMFILT_STEP ; // binSize ratio
          Finteg_spec[ised]  += (Fbin_forSpec * LAMRATIO );

	} // end OPT_SPEC

	Finteg_filter[ised]  +=  Fbin_forFlux ;
	Finteg_forErr[ised]  += (Fbin_forFlux/MWXT_FRAC) ;	

      } // ised

    } // end LAMSED loop 

    if ( OPT_SPEC ) {
      Fspec[ilamobs]  = x0 * ( Finteg_spec[0] + x1*Finteg_spec[1] );
      Fspec[ilamobs] *= MODELNORM_Fspec ;
    }
    
  } // ilam (obs-filters)


  // compute total flux in filter
  *Finteg  = x0 * ( Finteg_filter[0] + x1 * Finteg_filter[1] );
  *Finteg *= MODELNORM_Finteg ;

  // May 6 2014:
  // Compute flux-ratio without Galactic extinction
  // (bug found by R. Biswas, and fixed at v10_35b)
  if ( Finteg_filter[0] != 0.0 ) 
    { *Fratio = Finteg_forErr[1] / Finteg_forErr[0] ; }

  return ;

} // end of INTEG_zSED_SALT2


// ==============================================================
void get_fluxRest_SALT2(double LAMREST_MIN, double LAMREST_MAX,
			double *fluxRest) {

  // !!!! probably OBSOLETE !!!!
  //
  // Return rest-frame flux for each compoment, Flux0 & Flux1,
  // integrated between lamRest_min & lamRest_max.
  // Do NOT include extinction from MW or host.

  double LAMREST ;
  //  char fnam[] = "get_fluxRest_SALT2" ;

  // ---------------- BEGIN -----------------

  fluxRest[0] = fluxRest[1] = 0.0 ;  

  LAMREST = LAMREST_MIN; // xxx replace with loop


  return ;

} // end get_fluxRest_SALT2


// **********************************************
double SALT2x0calc(
		   double alpha   // (I)
		   ,double beta   // (I)
		   ,double x1     // (I)
		   ,double c      // (I)
		   ,double dlmag  // (I) distance modulus
		   ) {

  // April 2, 2009 R.Kessler
  // Translate luminosity and color parameters into x0.

  double x0, x0inv, arg ;

  // -------- BEGIN ---------
  arg     = 0.4 * ( dlmag - alpha*x1 + beta*c ) ;
  x0inv   = X0SCALE_SALT2 * pow(TEN,arg); 
  x0      = 1./x0inv ;
  return  x0;  

}  // end of SALT2x0calc


// ***********************************
void load_mBoff_SALT2(void) {

  // Created July 27, 2010 by R.Kessler
  // Fill global mBoff_SALT2 used to compute mB.
  // Note that in the simulation mB is not used to 
  // generate fluxes but is used only as a reference.
  //
  // If B filter exists then
  //    mBoff_SALT2 = ZP  - 2.5*log10(S0)
  // where S0 is the zero-surface integral with z = Zat10pc,
  // and then  mB = mBoff_SALT2 - 2.5*log10(x0)
  //
  // Aug 11, 2010: just hard-wire mBoff to have same offset
  //               regardless of filter system.

  //  char fnam[] = "load_mBoff_SALT2" ;

  // -------- BEGIN --------

  // Aug 11, 2010: hard-wire to value based on SNLS VEGA system.
  mBoff_SALT2 = 10.635 ;
  printf("\t mB = %7.4f - 2.5*log10(x0)  \n", mBoff_SALT2 );


} // end of load_mBoff_SALT2

// ***********************************
double SALT2mBcalc(double x0) {

  // April 12, 2009 R.Kessler
  // Translate x0 into mB.
  //
  // July 27, 2010: use mBoff_SALT2 instead of hard-wired 10.63

  double mB;

  // -------- BEGIN ---------

  mB = mBoff_SALT2 - 2.5*log10(x0);
  return mB;

}  // end of SALT2mBcalc


// ***********************************************
void get_SALT2_ERRMAP(double Trest, double Lrest, double *ERRMAP ) {

  /***
   Apr 14, 2009: 
   return error values from each of the NERRMAP maps.
   Trest         :  (I) rest-frame epoch (days,  T=0 at peak)
   Lrest         :  (I) rest-frame wavelength (A)
   SALT2modelerr :  (O) error-map values: var0, var1, covar01, scale

   Aug 27, 2009: 
      interpolate in both Trest & lambda (instead of just lambda).
      Still not always continuous, but chi2-kinks are much smaller
      than before.

  Jun 2, 2011: renamed from get_SALT2modelerr to get_SALT2_ERRMAP().

  Sep 9 2019: 
    + protect iday_min and ilam_min from being negative. Negative indices
      can occur because ERRMAPs don't always cover SED range.
           
  ***/

  int imap, jval, iday_min, iday_max, ilam_min, ilam_max ;
  int NLAM, NDAY, IND, IERR ;

  double val, val0, val1, valdif, val_linear, val_spline, tmp;
  double LMIN, LSTEP, LDIF, TMIN, TSTEP, TDIF, val_atlammin, val_atlammax ;

  //  char fnam[] = "get_SALT2_ERRMAP";

  // ------------ BEGIN --------

  for ( imap=0; imap < NERRMAP; imap++ ) {

    if ( imap >= INDEX_ERRMAP_COLORDISP ) { continue ; }

    LMIN  = SALT2_ERRMAP[imap].LAMMIN ;
    LSTEP = SALT2_ERRMAP[imap].LAMSTEP ;

    TMIN  = SALT2_ERRMAP[imap].DAYMIN ;
    TSTEP = SALT2_ERRMAP[imap].DAYSTEP ;

    NLAM  = SALT2_ERRMAP[imap].NLAM ;
    NDAY  = SALT2_ERRMAP[imap].NDAY ;

    // get indices that sandwhich Trest and Lrest

    iday_min = (int)((Trest - TMIN)/TSTEP) ;
    if ( iday_min >= NDAY-1 ) { iday_min = NDAY - 2 ; }
    if ( iday_min <  0      ) { iday_min = 0; } // Sep 9 2019
    iday_max = iday_min + 1;

    ilam_min = (int)((Lrest - LMIN)/LSTEP) ;
    if ( ilam_min >= NLAM-1 ) { ilam_min = NLAM - 2 ; }
    if ( ilam_min <  0      ) { ilam_min = 0;         }
    ilam_max = ilam_min + 1;

    
    // Aug 27, 2009: 
    // interpolate Trest at LAM-MIN
    jval  = NLAM*iday_min + ilam_min ;
    val0  = SALT2_ERRMAP[imap].VALUE[jval];
    jval  = NLAM*iday_max + ilam_min ;
    val1  = SALT2_ERRMAP[imap].VALUE[jval];
    TDIF  = Trest - SALT2_ERRMAP[imap].DAY[iday_min];
    val_atlammin  = val0 + (val1-val0) * TDIF/TSTEP ;


    // interpolate Trest at LAM-MAX
    jval  = NLAM*iday_min + ilam_max ;
    val0  = SALT2_ERRMAP[imap].VALUE[jval];
    jval  = NLAM*iday_max + ilam_max ;
    val1  = SALT2_ERRMAP[imap].VALUE[jval];
    TDIF  = Trest - SALT2_ERRMAP[imap].DAY[iday_min];
    val_atlammax  = val0 + (val1-val0) * TDIF/TSTEP ;

    // interpolate in lambda space
    LDIF       = Lrest - SALT2_ERRMAP[imap].LAM[ilam_min];
    valdif     = val_atlammax - val_atlammin ;
    val_linear = val_atlammin + (valdif * LDIF/LSTEP) ;
    val        = val_linear ;

    if ( INPUT_SALT2_INFO.ERRMAP_INTERP_OPT == 0 ) 
      { val = 0.0 ; }

    if ( INPUT_SALT2_INFO.ERRMAP_INTERP_OPT == 2 ) {

      IND    = SALT2_ERRMAP[imap].INDEX_SPLINE ; 
      tmp    = ge2dex_ ( &IND, &Trest, &Lrest, &IERR ) 	;
      val_spline  = sqrt(pow(TEN,tmp)) ;

      // Use the sign of the linear interp because
      // the sign in storing the error-squared is lost.

      if ( val_linear < 0.0 ) { val = -val_spline ; }
      else                    { val = +val_spline ; }

      /*
      NCALL_DBUG_SALT2++ ;
      printf(" xxx imap=%d  Trest=%6.2f Lrest=%6.0f : ",
	     imap, Trest, Lrest );
      printf("val(lin,spline)= %le, %le \n", val_linear, val );
      if ( NCALL_DBUG_SALT2 > 50 )  debugexit("SALT2 sline");
      */

    }  // SPLINE option


    ERRMAP[imap] = val ;
    
  } // end of imap loop

} // end of get_SALT2_ERRMAP


// *******************************************************
int gencovar_SALT2(int MATSIZE, int *ifiltobsList, double *epobsList, 
		   double z, double x0, double x1, double c, double mwebv,
		   double RV_host, double AV_host,
		   double *covar ) {

  // Jun 2, 2011 R.Kessler
  // return *covar matrix that depends on ifilt_obs and redshift. 
  // *covar is the covariance in mag^2 units,
  //        FAC * k(lambda)^2 
  // where FAC converts flux-fraction-squared error into mag^2 error,
  // and k(lambda) are from the salt2_color_dispersion.dat file.
  // Input 'matsize' is the size of one row or column;
  // the output *covar size is matsize^2.
  //
  // Jul 3 2013: 
  //  fix aweful bug that was double-counting the kcor error
  //  for the diagonal elements.
  //    COV_TMP += COV_DIAG  -> COV_TMP = COV_DIAG
  //  ARRRRRRRRRGH !!!
  //
  //  July 2016: add new inputs args RV_host & AV_host

  int  icovar, irow, icol, ifilt_obs, ifilt_row, ifilt_col, ifilt ;
  int ISDIAG, LDMP ;

  double 
    COV_TMP,  COV_DIAG
    ,meanlam_obs, meanlam_rest, invZ1
    ,cDisp[MXFILT_SEDMODEL]
    ,Finteg, Fratio, FspecDum[10], magerr
    ,Tobs, Trest, Trest_tmp
    ,Trest_row, Trest_col
    ,FAC = 1.17882   //  [ 2.5/ln(10) ]^2
    ;

    char *cfilt, cdum0[40], cdum1[40];

    char fnam[] = "gencovar_SALT2" ;

  // -------------- BEGIN -----------------
  
  invZ1 = 1.0/(1.+z);
  icovar = 0 ;

  // init  cDisp to -9 in each filter
  for(ifilt=1; ifilt <= NFILT_SEDMODEL; ifilt++) {
    ifilt_obs = FILTER_SEDMODEL[ifilt].ifilt_obs ;
    cDisp[ifilt_obs] = -9.0 ;
  }


  for ( irow=0; irow < MATSIZE; irow++ ) {
    for ( icol=0; icol < MATSIZE; icol++ ) {

      Tobs  = *(epobsList+irow) ;  Trest_row  = Tobs * invZ1 ;
      Tobs  = *(epobsList+icol) ;  Trest_col  = Tobs * invZ1 ;
      ISDIAG = ( irow == icol ) ;

      meanlam_rest  = -9.0 ;
      ifilt_row     = *(ifiltobsList+irow);
      ifilt_col     = *(ifiltobsList+icol);
      COV_TMP       = 0.0 ; 
      COV_DIAG      = 0.0 ;

      // get cDisp for this filter; avoid repeating SALT2colorDisp calls
      if ( cDisp[ifilt_row] < -1.0 ) {
	ifilt         = IFILTMAP_SEDMODEL[ifilt_row] ;
	meanlam_obs   = FILTER_SEDMODEL[ifilt].mean ;  // mean lambda
	meanlam_rest  = meanlam_obs * invZ1 ; 
	cDisp[ifilt_row] = SALT2colorDisp(meanlam_rest,fnam);    
      }

      // set covariances only for same passband.
      if ( ifilt_col == ifilt_row ) 
	{ COV_TMP = FAC * pow(cDisp[ifilt_row],2.0);  }

      // check for local dump option
      LDMP  = (COV_TMP != 0.0 || ISDIAG) && 
	(fabs(Trest_row) < -1.0 && fabs(Trest_col) < -1.0); 

      if ( LDMP ) 
	{ printf(" xxx ############ COV_MODEL DUMP ############### \n"); }

      // diagonal-only term
      if ( ISDIAG ) {
	Tobs          = *(epobsList+irow) ;  
	Trest         = Tobs * invZ1 ;
	ifilt         = IFILTMAP_SEDMODEL[ifilt_row] ;
	cfilt         = FILTER_SEDMODEL[ifilt].name ;
	meanlam_obs   = FILTER_SEDMODEL[ifilt].mean ;  // mean lambda
	meanlam_rest  = meanlam_obs * invZ1 ; 


	// make sure that Trest is within the map range
	if ( Trest > SALT2_ERRMAP[0].DAYMAX ) 
	  { Trest_tmp = SALT2_ERRMAP[0].DAYMAX ; }
	else if ( Trest < SALT2_ERRMAP[0].DAYMIN ) 
	  { Trest_tmp = SALT2_ERRMAP[0].DAYMIN ; }
	else
	  { Trest_tmp = Trest ; }

	Trest = Trest_tmp ;
	Tobs  = Trest * ( 1. + z );

	INTEG_zSED_SALT2(0,ifilt_row,z,Tobs, x0, x1, c,    // input
			 RV_host, AV_host,               // input
			 &Finteg, &Fratio, FspecDum);    // returned
	
	magerr = SALT2magerr(Trest, meanlam_rest, z, x1, Fratio, LDMP );
	COV_DIAG = magerr*magerr ;
	COV_TMP = COV_DIAG ;
      }
      
      covar[icovar] = COV_TMP ;  // load output array  
      icovar++ ;                   // increment local pointer

      if ( LDMP && COV_TMP != 0.0  ) {

	ifilt         = IFILTMAP_SEDMODEL[ifilt_row] ;
	cfilt         = FILTER_SEDMODEL[ifilt].name ;
	sprintf(cdum0,"%s:Tobs=%7.3f", cfilt, Tobs);

	ifilt         = IFILTMAP_SEDMODEL[ifilt_col] ;
	cfilt         = FILTER_SEDMODEL[ifilt].name ;
	sprintf(cdum1,"%s:Tobs=%7.3f", cfilt, Tobs);
 
	printf(" xxx COV_MAGERR[ %s , %s ] = %le \n", cdum0,cdum1, COV_TMP );

	if ( ISDIAG ) {
	  printf(" xxx ----------------- \n");
	  printf(" xxx COV_DIAGON[ %s , %s ] = %le \n", cdum0,cdum1, COV_DIAG);
	  printf(" xxx meanlam_rest = %f  z=%f  x1=%f  Fratio=%f \n",
		 meanlam_rest, z, x1, Fratio );
	  printf(" xxx ----------------- \n");
	}

	fflush(stdout);
      }

    } // icol
  } //  irow

  return SUCCESS ; 

} // end of gencovar_SALT2

// ***********************************************
double SALT2colorDisp(double lam, char *callFun) {

  // Mar 2011
  // Return color dispersion for input rest-wavelength "lam".
  // Since this function INTERPOLATES, and does NOT extrapolate,
  // this function aborts if 'lam' is outside the valid
  // rest-lambda range. Make sure to check that 'lam' is valid
  // before calling this function.
  //
  // Jan 28 2020:
  //  if UV extrap is used, extrapolate instead of aborting
  //
  int imap, NLAM ;
  double cDisp, LAMMIN, LAMMAX ;
  double *mapLam, *mapDisp ;
  char fnam[] = "SALT2colorDisp" ;

  // ------------ BEGIN --------------

  // strip off goodies into local variables
  imap    = INDEX_ERRMAP_COLORDISP ;
  NLAM    = SALT2_ERRMAP[imap].NLAM ;
  LAMMIN  = SALT2_ERRMAP[imap].LAMMIN ;
  LAMMAX  = SALT2_ERRMAP[imap].LAMMAX ;
  mapLam  = SALT2_ERRMAP[imap].LAM ;
  mapDisp = SALT2_ERRMAP[imap].VALUE ;

  if ( NLAM <= 0 ) { cDisp = 0.0 ; return cDisp ; }

  
  if ( INPUTS_SEDMODEL.UVLAM_EXTRAPFLUX > 0.0 && lam < LAMMIN ) 
    { cDisp = mapDisp[0]; return(cDisp);  }

  // first some sanity checks
  if ( lam < LAMMIN || lam > LAMMAX ) {  
    sprintf(c1err,"lam=%f outside lookup range (called from %s)", 
	    lam, callFun );
    sprintf(c2err,"Valid range is %7.1f to %7.1f A ", LAMMIN, LAMMAX);
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err); 
  }

  if ( NLAM <= 1 ) {
    sprintf(c1err,"Cannot do map-lookup with %d lambda bins (callFun=%s).", 
	    NLAM, callFun);
    sprintf(c2err,"Check %s",  SALT2_ERRMAP_FILES[imap] );
    errmsg(SEV_FATAL, 0, fnam, c1err, c2err); 
  }


  // use generic linear interpolator.
  cDisp = interp_1DFUN(OPT_INTERP_LINEAR, lam, 
		       NLAM, mapLam, mapDisp, "cDisp" );


  // return final result
  return cDisp ;
    

} // end of SALT2colorDisp


// ***********************************************
void colordump_SALT2(double lam, double c, char *cfilt) {

  double colorCor;
  char cCor[60];
  // -------- BEGIN --------

  colorCor = SALT2colorCor(lam,c);
  if ( fabs(colorCor) < 100 ) 
    { sprintf(cCor,"%7.3f", colorCor ); }
  else
    { sprintf(cCor,"%9.3le", colorCor ); }

  printf("\t ColorTerm[ lam=%5.0f (%s)  c=%4.1f ] = %s \n", 
	 lam, cfilt, c, cCor);

} // end of colordump_SALT2

// ===============================
void errorSummary_SALT2(void) {

  // summarize errors and CL(lambda) in a list vs. lambda.

  int NLAM, ilam, imap, LLAM ;
  double LAMLIST[100], lam, Trest, ERRMAP[NERRMAP] ;
  double var0, var1, covar01, errscale, S0fracErr, colorCor, c, colorDisp ;   
  char cCor[20];
  //  char fnam[] = "errorSummary_SALT2" ;

  // ---------- BEGIN --------

  NLAM = 0;

  // hard-wire list of lambda values to check
  NLAM++ ; LAMLIST[NLAM] = 2000.0 ; // A
  NLAM++ ; LAMLIST[NLAM] = 2500.0 ;
  NLAM++ ; LAMLIST[NLAM] = 3000.0 ;
  NLAM++ ; LAMLIST[NLAM] = U_WAVELENGTH ;
  NLAM++ ; LAMLIST[NLAM] = 3560.0 ;   // u band
  NLAM++ ; LAMLIST[NLAM] = 3900.0 ;
  NLAM++ ; LAMLIST[NLAM] = B_WAVELENGTH ;
  NLAM++ ; LAMLIST[NLAM] = 4720.0 ;  // g band
  NLAM++ ; LAMLIST[NLAM] = V_WAVELENGTH ;
  NLAM++ ; LAMLIST[NLAM] = 6185.0 ;  // r band
  NLAM++ ; LAMLIST[NLAM] = R_WAVELENGTH ;
  NLAM++ ; LAMLIST[NLAM] = 7500.0 ;  // i band
  NLAM++ ; LAMLIST[NLAM] = 8030.0 ;  // I band
  NLAM++ ; LAMLIST[NLAM] = 8500.0 ;
  NLAM++ ; LAMLIST[NLAM] = 9210.0 ;  // z
  NLAM++ ; LAMLIST[NLAM] = 9940.0 ;  // Y


  Trest = 0.0;
  c = 1.0; // color value

  printf("\n");
  printf("                               peak     color  \n" );
  printf("            LAMBDA(A)  e^CL    dS0/S0   disp   \n" );
  printf("  --------------------------------------------- \n" );

  for ( ilam = 1; ilam <= NLAM; ilam++ ) {

    lam = LAMLIST[ilam];

    // color correcction (note that this is not an error)
    colorCor = SALT2colorCor(lam,c);
    if ( fabs(colorCor) < 100. ) 
      { sprintf(cCor,"%7.3f", colorCor ); }
    else
      { sprintf(cCor,"%9.3le", colorCor ); }



    // fractional flux error with x1=0
    get_SALT2_ERRMAP ( Trest, lam, ERRMAP );
    var0       = ERRMAP[0] ;  // sigma(S0)/S0
    var1       = ERRMAP[1] ;  // sigma(S1)/S0
    covar01    = ERRMAP[2] ;  // 
    errscale   = ERRMAP[3] ;  // error fudge
    S0fracErr  = errscale * sqrt(var0);         // dF/F with x1=0
	   
    // color dispersion
    imap = INDEX_ERRMAP_COLORDISP ;
    if ( lam >= SALT2_ERRMAP[imap].LAMMIN &&
	 lam <= SALT2_ERRMAP[imap].LAMMAX ) {
      
      colorDisp = interp_1DFUN(OPT_INTERP_LINEAR, lam
			    ,SALT2_ERRMAP[imap].NLAM
			    ,SALT2_ERRMAP[imap].LAM
			    ,SALT2_ERRMAP[imap].VALUE
			    ,"colorDispSummary" );      
    }
    else
      { colorDisp = 0.0 ; }


    LLAM = (int)lam ;
    printf("  LAMINFO:  %6d  %8s   %6.4f   %5.3f  \n", 
	   LLAM, cCor, S0fracErr, colorDisp );

  }  // end of ilam loop



} // end of errorSummary_SALT2

// ================================
void test_SALT2colorlaw1(void) {

#define NCTEST 5
  double c[NCTEST] ;
  double claw[NCTEST] ;
  double colorPar[20];

  double lambda;

  int i, irow ;

  // --------- BEGIN ------------

  // define parameters from Julien's test code (read_color_law.c)
  colorPar[0] = B_WAVELENGTH ;
  colorPar[1] = V_WAVELENGTH ;
  colorPar[2] = 3700.0 ;
  colorPar[3] = 8000.0 ;
  colorPar[4] = 4.0 ;      // nparams
  colorPar[5] = -1.77139; 
  colorPar[6] =  2.38305 ; 
  colorPar[7] = -1.16417 ; 
  colorPar[8] =  0.178494 ;

  c[0] = 0.2 ;
  c[1] = 0.4 ;
  c[2] = 0.6 ;
  c[3] = 0.8 ;
  c[4] = 1.0 ;

  irow = 0;

  for( lambda=2500; lambda<9000; lambda+=100) {

    for ( i=0; i < NCTEST ; i++ ) {
      claw[i] = SALT2colorlaw1(lambda,c[i], colorPar) ;
    }

    irow++ ;
    printf("SN: %4.4d  %6.1f  %f %f %f %f %f\n", 
	   irow, lambda, claw[0], claw[1], claw[2],claw[3], claw[4] );

  }
 
  debugexit("Done testing SALT2colorlaw1");
} // end of test_SALT2colorlaw1


// ========================================================================
// ============== SPECTROGRAPH FUNCTIONS (July 2016) ======================
// ========================================================================


// ==========================
void genSpec_SALT2(double x0, double x1, double c, double mwebv,
                   double RV_host, double AV_host,  double z,
                   double Tobs, double *GENFLUX_LIST, double *GENMAG_LIST ) {

  // July 2016
  // For input SALT2 params, return *GENFLUX_LIST and *GENMAG_LIST.
  // FLUXGEN units are arbitrary since snlc_sim program will add
  // fluctuations based on user-input SNR.
  // 
  // Inputs:
  //   x0,x1,c  : SALT2 params
  //   mwebv    : MW E(B-V)
  //   RV/AV_host : host extinction
  //   Tobs     : T - Tpeak, obs frame (scalar, not array)
  //
  // Output:
  //   *GENFLUX_LIST : flux vs. wavelength bin.
  //                   flux=0 outside SALT2 model range (no aborts).
  //
  //   *GENMAG_LIST :  mag
  //
  // Note that output arrays have length NBLAM (see below)
  //
  // Aug 31 2016: return of Trest is outside defined epoch range.
  //              Can extrapolate mags, but NOT spectra !
  //
  // Mar 29 2019: apply MAG_OFFSET to GENFLUX_LIST
  //
  // ------------------------------------------

  int    NBLAM      = SPECTROGRAPH_SEDMODEL.NBLAM_TOT ;
  double MAG_OFFSET = INPUT_SALT2_INFO.MAG_OFFSET ;  

  int ilam ;  
  double Trest, Fratio, Finteg, FTMP, GENFLUX, ZP, MAG, LAM, z1, FSCALE_ZP ;
  double hc8 = (double)hc ;
  //  char fnam[] = "genSpec_SALT2" ;

  // -------------- BEGIN --------------

  z1 = 1.0 + z;

  // init entire spectum to zero.
  for(ilam=0; ilam < NBLAM; ilam++ ) { GENFLUX_LIST[ilam] = 0.0 ; }

  Trest = Tobs/(1.0+z);
  if ( Trest < SALT2_TABLE.DAYMIN+0.1 ) { return ; }
  if ( Trest > SALT2_TABLE.DAYMAX-0.1 ) { return ; }
	
  INTEG_zSED_SALT2(1, JFILT_SPECTROGRAPH, z, Tobs, 
		   x0, x1, c,	RV_host, AV_host,
		   &Finteg, &Fratio, GENFLUX_LIST ) ;

  FSCALE_ZP = pow(TEN,-0.4*MAG_OFFSET);

  // convert generated fluxes into mags
  for(ilam=0; ilam < NBLAM; ilam++ ) { 
    GENFLUX_LIST[ilam] *= FSCALE_ZP ;  // Mar 29 2019

    GENFLUX = GENFLUX_LIST[ilam] ;
    LAM     = SPECTROGRAPH_SEDMODEL.LAMAVG_LIST[ilam] ;
    ZP      = SPECTROGRAPH_SEDMODEL.ZP_LIST[ilam] ;
    FTMP    = (LAM/(hc8*z1)) * GENFLUX;
    if ( ZP > 0.0 && FTMP > 0.0 )   { 
      MAG = -2.5*log10(FTMP) + ZP;     // xxx  + MAG_OFFSET ;  
    }
    else  { 
      MAG = MAG_UNDEFINED ;  // model undefined
    }

    GENMAG_LIST[ilam] = MAG ;

  } // end ilam loop over SPECTROGRAPH bins

  return ;

} // end genSpec_SALT2


// ======================================================
int getSpec_band_SALT2(int ifilt_obs, float Tobs_f, float z_f,
                       float x0_f, float x1_f, float c_f, float mwebv_f,
		       float *LAMLIST_f, float *FLUXLIST_f) {

  // Created Nov 2016
  // Return spectrum in band 'ifilt_obs' with passed SALT2 params.
  // Spectrum is returned as LAMLIST_f and FLUXLIST_f.
  // Note that all function args are float, but local 
  // variables are double.

  int ifilt      = IFILTMAP_SEDMODEL[ifilt_obs] ;
  int NBLAM      = FILTER_SEDMODEL[ifilt].NLAM ;
  int MEMD   = NBLAM * sizeof(double);
  int ilam ;
  double LAMOBS, LAMREST, z1, Finteg, Finteg_check, Fratio, TRANS ;
  double RV_host=-9.0, AV_host=0.0 ;

  double Tobs  = (double)Tobs_f ;
  double z     = (double)z_f ;
  double x0    = (double)x0_f ;
  double x1    = (double)x1_f ;
  double c     = (double)c_f ;
  double *FLUXLIST = (double*) malloc ( MEMD );
  double Trest = Tobs/(1.0 + z) ;

  //   char fnam[] = "getSpec_band_SALT2" ;

  // ------------- BEGIN ---------------

  if ( Trest <= SALT2_TABLE.DAYMIN ) { return(0); }
  if ( Trest >= SALT2_TABLE.DAYMAX ) { return(0); }

  INTEG_zSED_SALT2(1, ifilt_obs, z, Tobs,         // (I)
		   x0, x1, c, RV_host, AV_host,   // (I)
		   &Finteg, &Fratio, FLUXLIST ) ; // (O)
  
  Finteg_check = 0.0 ;  z1=1.0+z ;
  for(ilam=0; ilam < NBLAM; ilam++ ) {
    LAMOBS  = FILTER_SEDMODEL[ifilt].lam[ilam] ;
    LAMLIST_f[ilam]  = (float)LAMOBS ;
    FLUXLIST_f[ilam] = (float)FLUXLIST[ilam];

    // check Finteg; FLUXLIST already includes LAMSTEP
    TRANS   = FILTER_SEDMODEL[ifilt].transSN[ilam] ;
    LAMREST = LAMOBS/z1 ;
    Finteg_check += ( TRANS * LAMREST * FLUXLIST[ilam] ); 
  }
  
  /*
  printf(" xxx Tobs=%5.1f z=%.3f ifiltobs=%2d: Ratio_Finteg=%.3f (%le)\n",
	 Tobs, z, ifilt_obs, Finteg_check/Finteg, Finteg );  
  */

  free(FLUXLIST);

  return(NBLAM);

} // end getSpec_band_SALT2

int getspec_band_salt2__(int *ifilt_obs, float *Tobs, float *z,
			 float *x0, float *x1, float *c, float *mwebv,
			 float *LAMLIST, float *FLUXLIST) {
  int NBLAM;
  NBLAM = getSpec_band_SALT2(*ifilt_obs, *Tobs, *z, 
			     *x0, *x1, *c, *mwebv, LAMLIST, FLUXLIST ) ;
  return(NBLAM);
} 
