/*
 * Full Waveform Inversion (2D acoustic problem)  
 *
 * Daniel Koehn
 * Kiel, 12/06/2017
 */

#include "fd.h"

void FWI_AC(){

/* global variables */
/* ---------------- */

/* forward modelling */
extern int MYID, FDORDER, NX, NY, NT, L, READMOD, QUELLART, RUN_MULTIPLE_SHOTS, TIME_FILT, READREC;
extern int LOG, SEISMO, N_STREAMER, FW, NXG, NYG, IENDX, IENDY, NTDTINV, IDXI, IDYI, NXNYI, INV_STF, DTINV;
extern float FC_SPIKE_1, FC_SPIKE_2, FC, FC_START, TIME, DT;
extern char LOG_FILE[STRING_SIZE], MFILE[STRING_SIZE];
extern FILE *FP;

/* gravity modelling/inversion */
extern int GRAVITY, NZGRAV, NGRAVB, GRAV_TYPE, BACK_DENSITY;
extern char GRAV_DATA_OUT[STRING_SIZE], GRAV_DATA_IN[STRING_SIZE], GRAV_STAT_POS[STRING_SIZE], DFILE[STRING_SIZE];
extern float LAM_GRAV, GAMMA_GRAV, LAM_GRAV_GRAD, L2_GRAV_IT1;

/* full waveform inversion */
extern int GRAD_METHOD, NLBFGS, ITERMAX, IDX, IDY, INVMAT1, EPRECOND, PCG_BETA, LNORM;
extern int GRAD_FORM, POS[3], QUELLTYPB, MIN_ITER, MODEL_FILTER, INV_MOD_OUT, ROWI;
extern float FC_END, PRO, C_vp, C_vs, C_rho;
extern char MISFIT_LOG_FILE[STRING_SIZE], JACOBIAN[STRING_SIZE];
extern char *FILEINP1;

/* local variables */
int ns, nseismograms=0, nt, nd, fdo3, j, i, iter, h, hin, iter_true, SHOTINC, s=0;
int buffsize, ntr=0, ntr_loc=0, ntr_glob=0, nsrc=0, nsrc_loc=0, nsrc_glob=0, ishot, nshots=0, itestshot;

float sum, eps_scale, opteps_vp, opteps_vs, opteps_rho, Vp_avg, Vs_avg, rho_avg, Vs_sum, Vp_sum, rho_sum;
char *buff_addr, ext[10], *fileinp, jac[225], source_signal_file[STRING_SIZE];

double time1, time2, time7, time8, time_av_v_update=0.0, time_av_s_update=0.0, time_av_v_exchange=0.0, time_av_s_exchange=0.0, time_av_timestep=0.0;
	
double L2sum, *L2t;
	
float ** taper_coeff, * epst1, *hc=NULL;
int * DTINV_help;

MPI_Request *req_send, *req_rec;
MPI_Status  *send_statuses, *rec_statuses;

/* Variables for step length calculation */
int step1, step3=0;
float eps_true, tmp;

/* Variables for the L-BFGS method */
float * rho_LBFGS, * alpha_LBFGS, * beta_LBFGS; 
float * y_LBFGS, * s_LBFGS, * q_LBFGS, * r_LBFGS;
int NLBFGS_class, LBFGS_pointer, NLBFGS_vec;

/* Variables for PCG */
float * PCG_old, * PCG_new, * PCG_dir;
int PCG_class, PCG_vec;

/* Variables for energy weighted gradient */
float ** Ws, **Wr, **We;

/* parameters for FWI-workflow */
int stagemax=0, nstage;

/*vector for abort criterion*/
double * L2_hist=NULL;

/* help variable for MIN_ITER */
int min_iter_help=0;

/* parameters for gravity inversion */
float * gz_mod, * gz_res;
float ** gravpos=NULL, ** rho_grav=NULL, ** rho_grav_ext=NULL;
float ** grad_grav=NULL;
int ngrav=0, nxgrav, nygrav;
float L2_grav, FWImax, GRAVmax, FWImax_all, GRAVmax_all ;
char jac_grav[STRING_SIZE];

/* parameters for random number generation */
int ra, ra1;

FILE *FPL2, *FP_stage, *FP_GRAV, *LAMBDA;

if (MYID == 0){
   time1=MPI_Wtime(); 
   clock();
}

/* open log-file (each PE is using different file) */
/*	fp=stdout; */
sprintf(ext,".%i",MYID);  
strcat(LOG_FILE,ext);

if ((MYID==0) && (LOG==1)) FP=stdout;
else FP=fopen(LOG_FILE,"w");
fprintf(FP," This is the log-file generated by PE %d \n\n",MYID);

/* ----------------------- */
/* define FD grid geometry */
/* ----------------------- */

/* domain decomposition */
initproc();

NT=iround(TIME/DT); /* number of timesteps */

/* output of parameters to log-file or stdout */
if (MYID==0) write_par(FP);

/* NXG, NYG denote size of the entire (global) grid */
NXG=NX;
NYG=NY;

/* In the following, NX and NY denote size of the local grid ! */
NX = IENDX;
NY = IENDY;

NTDTINV=ceil((float)NT/(float)DTINV);		/* round towards next higher integer value */

/* save every IDXI and IDYI spatial point during the forward modelling */
IDXI=1;
IDYI=1;

NXNYI=(NX/IDXI)*(NY/IDYI);
SHOTINC=1;

/* use only every DTINV time sample for the inversion */
DTINV_help=ivector(1,NT);

/* read parameters from workflow-file (stdin) */
FP_stage=fopen(FILEINP1,"r");
if(FP_stage==NULL) {
	if (MYID == 0){
		printf("\n==================================================================\n");
		printf(" Cannot open Denise workflow input file %s \n",FILEINP1);
		printf("\n==================================================================\n\n");
		err(" --- ");
	}
}

/* estimate number of lines in FWI-workflow */
i=0;
stagemax=0;
while ((i=fgetc(FP_stage)) != EOF)
if (i=='\n') ++stagemax;
rewind(FP_stage);
stagemax--;
fclose(FP_stage);

/* define data structures for PSV problem */
struct waveAC;
struct waveAC_PML;
struct matAC;
struct fwiPSV;
struct mpiPSV;
struct seisPSV;
struct seisPSVfwi;
struct acq;

nd = FDORDER/2 + 1;
fdo3 = 2*nd;
buffsize=2.0*2.0*fdo3*(NX +NY)*sizeof(MPI_FLOAT);

/* allocate buffer for buffering messages */
buff_addr=malloc(buffsize);
if (!buff_addr) err("allocation failure for buffer for MPI_Bsend !");
MPI_Buffer_attach(buff_addr,buffsize);

/* allocation for request and status arrays */
req_send=(MPI_Request *)malloc(REQUEST_COUNT*sizeof(MPI_Request));
req_rec=(MPI_Request *)malloc(REQUEST_COUNT*sizeof(MPI_Request));
send_statuses=(MPI_Status *)malloc(REQUEST_COUNT*sizeof(MPI_Status));
rec_statuses=(MPI_Status *)malloc(REQUEST_COUNT*sizeof(MPI_Status));

/* --------- add different modules here ------------------------ */
ns=NT;	/* in a FWI one has to keep all samples of the forward modeled data
	at the receiver positions to calculate the adjoint sources and to do 
	the backpropagation; look at function saveseis_glob.c to see that every
	NDT sample for the forward modeled wavefield is written to su files*/

if (SEISMO && (READREC!=2)){

   acq.recpos=receiver(FP, &ntr, ishot);
   acq.recswitch = ivector(1,ntr);
   acq.recpos_loc = splitrec(acq.recpos,&ntr_loc, ntr, acq.recswitch);
   ntr_glob=ntr;
   ntr=ntr_loc;   
   
}

if(READREC!=2){

   /* Memory for seismic data */
   alloc_seisPSV(ntr,ns,&seisPSV);

   /* Memory for FWI seismic data */ 
   alloc_seisPSVfwi(ntr,ntr_glob,ns,&seisPSVfwi);
   
   /* Memory for full data seismograms */
   alloc_seisPSVfull(&seisPSV,ntr_glob);

}

/* memory allocation for abort criterion*/
L2_hist = dvector(1,1000);

/* estimate memory requirement of the variables in megabytes*/
	
switch (SEISMO){
case 1 : /* particle velocities only */
	nseismograms=2;	
	break;	
case 2 : /* pressure only */
	nseismograms=1;	
	break;	
case 3 : /* curl and div only */
	nseismograms=2;		
	break;	
case 4 : /* everything */
	nseismograms=5;		
	break;
}		

/* calculate memory requirements for AC forward problem */
mem_fwiPSV(nseismograms,ntr,ns,fdo3,nd,buffsize,ntr_glob);

if(GRAVITY==1 || GRAVITY==2){
  
  if(GRAV_TYPE == 1){
  sprintf(GRAV_DATA_OUT, "./gravity/grav_mod.dat"); /* output file of gravity data */
  sprintf(GRAV_DATA_IN, "./gravity/grav_field.dat");  /* input file of gravity data */
  }
  if(GRAV_TYPE == 2){
  sprintf(GRAV_DATA_OUT, "./gravity/grav_grad_mod.dat"); /* output file of gravity gradient data */
  sprintf(GRAV_DATA_IN, "./gravity/grav_grad_field.dat");  /* input file of gravity gradientdata */
  }
  sprintf(GRAV_STAT_POS, "./gravity/grav_stat.dat"); /* file with station positions for gravity modelling */

  /* size of the extended gravity model */
  nxgrav = NXG + 2*NGRAVB;
  nygrav = NYG + NGRAVB;

}

/* allocate memory for AC forward problem */
alloc_AC(&waveAC,&waveAC_PML);

/* calculate damping coefficients for CPMLs (AC problem)*/
if(FW>0){PML_pro(waveAC_PML.d_x, waveAC_PML.K_x, waveAC_PML.alpha_prime_x, waveAC_PML.a_x, waveAC_PML.b_x, waveAC_PML.d_x_half, waveAC_PML.K_x_half, waveAC_PML.alpha_prime_x_half, waveAC_PML.a_x_half, 
                 waveAC_PML.b_x_half, waveAC_PML.d_y, waveAC_PML.K_y, waveAC_PML.alpha_prime_y, waveAC_PML.a_y, waveAC_PML.b_y, waveAC_PML.d_y_half, waveAC_PML.K_y_half, waveAC_PML.alpha_prime_y_half, 
                 waveAC_PML.a_y_half, waveAC_PML.b_y_half);
}

/* allocate memory for AC material parameters */
alloc_matAC(&matAC);

/* allocate memory for AC FWI parameters */
alloc_fwiAC(&fwiPSV);

/* allocate memory for PSV MPI variables */
alloc_mpiPSV(&mpiPSV);

/* Variables for l-BFGS method */
if(GRAD_METHOD==2){

  NLBFGS_class = 2;                 /* number of parameter classes */ 
  NLBFGS_vec = NLBFGS_class*NX*NY;  /* length of one LBFGS-parameter class */
  LBFGS_pointer = 1;                /* initiate pointer in the cyclic LBFGS-vectors */
  
  y_LBFGS  =  vector(1,NLBFGS_vec*NLBFGS);
  s_LBFGS  =  vector(1,NLBFGS_vec*NLBFGS);

  q_LBFGS  =  vector(1,NLBFGS_vec);
  r_LBFGS  =  vector(1,NLBFGS_vec);

  rho_LBFGS = vector(1,NLBFGS);
  alpha_LBFGS = vector(1,NLBFGS);
  beta_LBFGS = vector(1,NLBFGS);
  
}

/* Variables for PCG method */
if(GRAD_METHOD==1){

  PCG_class = 2;                 /* number of parameter classes */ 
  PCG_vec = PCG_class*NX*NY;  	 /* length of one PCG-parameter class */
  
  PCG_old  =  vector(1,PCG_vec);
  PCG_new  =  vector(1,PCG_vec);
  PCG_dir  =  vector(1,PCG_vec);
 
}


taper_coeff=  matrix(1,NY,1,NX);

/* memory for source position definition */
acq.srcpos1=fmatrix(1,8,1,1);

/* memory of L2 norm */
L2t = dvector(1,4);
epst1 = vector(1,3);
	
fprintf(FP," ... memory allocation for PE %d was successfull.\n\n", MYID);

/* Holberg coefficients for FD operators*/
hc = holbergcoeff();

MPI_Barrier(MPI_COMM_WORLD);

/* Reading source positions from SOURCE_FILE */ 	
acq.srcpos=sources(&nsrc);
nsrc_glob=nsrc;

/* create model grids */
/*if(L){
	if (READMOD) readmod_visc_PSV(matPSV.prho,matPSV.ppi,matPSV.pu,matPSV.ptaus,matPSV.ptaup,matPSV.peta);
		else model(matPSV.prho,matPSV.ppi,matPSV.pu,matPSV.ptaus,matPSV.ptaup,matPSV.peta);
} else{*/
	if (READMOD) readmod_AC(matAC.prho,matAC.ppi);
    		else model_acoustic(matAC.prho,matAC.ppi);
/*}*/

/* check if the FD run will be stable and free of numerical dispersion */
/*if(L){
	checkfd_ssg_visc(FP,matPSV.prho,matPSV.ppi,matPSV.pu,matPSV.ptaus,matPSV.ptaup,matPSV.peta,hc);
} else{*/
	checkfd_acoustic(FP,matAC.prho,matAC.ppi,hc);
/*}*/



if(GRAVITY==1 || GRAVITY==2){
 
  /* read station positions */
  MPI_Barrier(MPI_COMM_WORLD);
  gravpos=read_grav_pos(&ngrav);

  /* define model and residual data vector for gz (z-component of the gravity field) */
  gz_mod = vector(1,ngrav);
  gz_res = vector(1,ngrav);

  /* only forward modelling of gravity data */
  if(GRAVITY==1){

    /* global density model */
    rho_grav =  matrix(1,NYG,1,NXG);
    rho_grav_ext =  matrix(1,nygrav,1,nxgrav);

    read_density_glob(rho_grav,1);
    extend_mod(rho_grav,rho_grav_ext,nxgrav,nygrav);
    grav_mod(rho_grav_ext,ngrav,gravpos,gz_mod,nxgrav,nygrav,NZGRAV);

    free_matrix(rho_grav,1,NYG,1,NXG);
    free_matrix(rho_grav_ext,1,nygrav,1,nxgrav);

  }

  if(GRAVITY==2){
    grad_grav =  matrix(1,NY,1,NX);
  }

} 
      
SHOTINC=1;
    
iter_true=1;
/* Begin of FWI-workflow */
for(nstage=1;nstage<=stagemax;nstage++){

/* read workflow input file *.inp */
FP_stage=fopen(FILEINP1,"r");
read_par_inv(FP_stage,nstage,stagemax);
/*fclose(FP_stage);*/

if((EPRECOND==1)||(EPRECOND==3)){
  Ws = matrix(1,NY,1,NX); /* total energy of the source wavefield */
  Wr = matrix(1,NY,1,NX); /* total energy of the receiver wavefield */
  We = matrix(1,NY,1,NX); /* total energy of source and receiver wavefield */
}

FC=FC_END;

iter=1;
/* --------------------------------------
 * Begin of Full Waveform iteration loop
 * -------------------------------------- */
while(iter<=ITERMAX){

      /* Apply random objective waveform inversion (AGC & L2 - norm)*/
      if(ROWI){
      
         /* fetch random number on MYID==0 */
         if(MYID==0){
      
            /* initialize random number generator */
      	    srand((unsigned)time(NULL));
      
      	    /* generate random number between 1 and 100 */
            ra = rand();
      	    ra1 = (ra % 100) + 1;
            
         }
      
         /* broadcast random number over all MPI processes */	       
         MPI_Barrier(MPI_COMM_WORLD);
         MPI_Bcast(&ra1,1,MPI_INT,0,MPI_COMM_WORLD);
      
         if(ra1<=50){LNORM=8;}
	 if(ra1>50){LNORM=2;}
      
         if(MYID==0){
            printf("ra1 = %d \t LNORM = %d \n", ra1, LNORM);      
         }
      
      }
        
      MPI_Barrier(MPI_COMM_WORLD);

if(GRAD_METHOD==2){
  
  /* increase pointer to LBFGS-vector*/
  if(iter>2){
    LBFGS_pointer++;
  }
  
  /* if LBFGS-pointer > NLBFGS -> set LBFGS_pointer=1 */ 
  if(LBFGS_pointer>NLBFGS){LBFGS_pointer=1;}

}

if (MYID==0)
   {
   time2=MPI_Wtime();
   fprintf(FP,"\n\n\n ------------------------------------------------------------------\n");
   fprintf(FP,"\n\n\n                   TDFWI ITERATION %d \t of %d \n",iter,ITERMAX);
   fprintf(FP,"\n\n\n ------------------------------------------------------------------\n");
   }

/* For the calculation of the material parameters between gridpoints
   they have to be averaged. For this, values lying at 0 and NX+1,
   for example, are required on the local grid. These are now copied from the
   neighbouring grids */		
/*if (L){
	matcopy_PSV(matPSV.prho,matPSV.ppi,matPSV.pu,matPSV.ptaus,matPSV.ptaup);
} else{*/
	matcopy_acoustic_AC(matAC.prho,matAC.ppi);
/*}*/

MPI_Barrier(MPI_COMM_WORLD);

av_rho(matAC.prho,matAC.prip,matAC.prjp);
/*if (L) av_tau(matPSV.ptaus,matPSV.ptausipjp);*/


/* Preparing memory variables for update_s (viscoelastic) */
/*if (L) prepare_update_s_visc_PSV(matPSV.etajm,matPSV.etaip,matPSV.peta,matPSV.fipjp,matPSV.pu,matPSV.puipjp,matPSV.ppi,matPSV.prho,matPSV.ptaus,matPSV.ptaup,matPSV.ptausipjp,matPSV.f,matPSV.g,
		matPSV.bip,matPSV.bjm,matPSV.cip,matPSV.cjm,matPSV.dip,matPSV.d,matPSV.e);*/


if(iter_true==1){

    for (i=1;i<=NX;i=i+IDX){ 
	for (j=1;j<=NY;j=j+IDY){
	
	if(INVMAT1==1){
	
	  fwiPSV.Vp0[j][i] = matAC.ppi[j][i];
	  fwiPSV.Rho0[j][i] = matAC.prho[j][i];

        }
	  
                 
		 
	if(INVMAT1==2){
        
	  fwiPSV.Vp0[j][i] = sqrt((matAC.ppi[j][i])*matAC.prho[j][i]);
	  fwiPSV.Rho0[j][i] = matAC.prho[j][i];
	
	}
	 
	if(INVMAT1==3){
        
	  fwiPSV.Vp0[j][i] = matAC.ppi[j][i];
	  fwiPSV.Rho0[j][i] = matAC.prho[j][i];
	
	}  
	
    }
    }

/* ----------------------------- */
/* calculate Covariance matrices */
/* ----------------------------- */

	 Vp_avg = 0.0;
	 Vs_avg = 0.0;
	 rho_avg = 0.0;
	 
        for (i=1;i<=NX;i=i+IDX){
           for (j=1;j<=NY;j=j+IDY){
	  
		 /* calculate average Vp, Vs */
                 Vp_avg+=matAC.ppi[j][i];
		 
		 /* calculate average rho */
		 rho_avg+=matAC.prho[j][i];
	
           }
        }
		
        /* calculate average Vp, Vs and rho of all CPUs*/
        Vp_sum = 0.0;
        MPI_Allreduce(&Vp_avg,&Vp_sum,1,MPI_FLOAT,MPI_SUM,MPI_COMM_WORLD);
        Vp_avg=Vp_sum;	
	
	rho_sum = 0.0;
        MPI_Allreduce(&rho_avg,&rho_sum,1,MPI_FLOAT,MPI_SUM,MPI_COMM_WORLD);
        rho_avg=rho_sum;
	
	Vp_avg /=NXG*NYG; 
	rho_avg /=NXG*NYG;
	
	if(MYID==0){
           printf("Vp_avg = %.0f \t rho_avg = %.0f \n ",Vp_avg,rho_avg);	
	}
	
	C_vp = Vp_avg;
	C_rho = rho_avg;


}

/* Open Log File for L2 norm */
if(MYID==0){
  if(iter_true==1){
    FPL2=fopen(MISFIT_LOG_FILE,"w");
  }

  if(iter_true>1){
    FPL2=fopen(MISFIT_LOG_FILE,"a");
  }
}

/* ---------------------------------------------------------------------------------------------------- */
/* --------- Calculate gradient and objective function using the adjoint state method ----------------- */
/* ---------------------------------------------------------------------------------------------------- */

L2sum = grad_obj_ac(&waveAC, &waveAC_PML, &matAC, &fwiPSV, &mpiPSV, &seisPSV, &seisPSVfwi, &acq, hc, iter, nsrc, ns, ntr, ntr_glob, 
nsrc_glob, nsrc_loc, ntr_loc, nstage, We, Ws, Wr, taper_coeff, hin, DTINV_help, req_send, req_rec);

L2t[1]=L2sum;
L2t[4]=L2sum;

if(GRAVITY==2){

  /* save seismic L2-norm of seismic data residuals */
  L2sum = L2t[1];

  /* global density model */
  rho_grav =  matrix(1,NYG,1,NXG);
  rho_grav_ext =  matrix(1,nygrav,1,nxgrav);

  /* model gravity data */
  /* save current density model */
  sprintf(jac_grav,"%s_tmp.rho.%i.%i",JACOBIAN,POS[1],POS[2]);
  FP_GRAV=fopen(jac_grav,"wb");

  for (i=1;i<=NX;i=i+IDX){
      for (j=1;j<=NY;j=j+IDY){
          fwrite(&matAC.prho[j][i],sizeof(float),1,FP_GRAV);
      }
  }
	
  fclose(FP_GRAV);

  MPI_Barrier(MPI_COMM_WORLD);
          
  /* merge model file */ 
  sprintf(jac_grav,"%s_tmp.rho",JACOBIAN);
  if (MYID==0) mergemod(jac_grav,3);
  
  MPI_Barrier(MPI_COMM_WORLD);
  
  /* gravity forward modelling */
  read_density_glob(rho_grav,2);
  extend_mod(rho_grav,rho_grav_ext,nxgrav,nygrav);
  grav_mod(rho_grav_ext,ngrav,gravpos,gz_mod,nxgrav,nygrav,NZGRAV);

  /* calculate gravity data residuals */
  L2_grav=calc_res_grav(ngrav,gz_mod,gz_res);

  /* calculate lambda 1 */
  if(iter==1){
  	LAM_GRAV = GAMMA_GRAV * (L2sum/L2_grav);
  }

  /* add gravity penalty term to the seismic objective function */
  L2t[1]+=LAM_GRAV * L2_grav;
  L2t[4]+=LAM_GRAV * L2_grav;

  /* calculate gravity gradient */
  for (i=1;i<=NX;i=i+IDX){
       for (j=1;j<=NY;j=j+IDY){
           grad_grav[j][i]=0.0;
       }
  }
  grav_grad(ngrav,gravpos,grad_grav,gz_res);
  
  MPI_Barrier(MPI_COMM_WORLD);        

  /* merge model file */
  sprintf(jac,"%s_grav",JACOBIAN);          
  if (MYID==0) mergemod(jac,3); 

  /* free memory */
  free_matrix(rho_grav,1,NYG,1,NXG);
  free_matrix(rho_grav_ext,1,nygrav,1,nxgrav);
  

}

/* Interpolate missing spatial gradient values in case IDXI > 1 || IDXY > 1 */
/* ------------------------------------------------------------------------ */

if((IDXI>1)||(IDYI>1)){

   interpol(IDXI,IDYI,fwiPSV.waveconv,1);
   interpol(IDXI,IDYI,fwiPSV.waveconv_rho,1);

}

/* Add gravity gradient to FWI density gradient */
/* -------------------------------------------- */
	
   if(GRAVITY==2){
		 		 
     /* calculate maximum values of waveconv_rho and grad_grav */
     FWImax = 0.0;
     GRAVmax = 0.0;
	
     for (i=1;i<=NX;i++){
        for (j=1;j<=NY;j++){
		
	    if(fabs(fwiPSV.waveconv_rho[j][i])>FWImax){FWImax=fabs(fwiPSV.waveconv_rho[j][i]);}
	    if(fabs(grad_grav[j][i])>GRAVmax){GRAVmax=fabs(grad_grav[j][i]);}
		
        }
     }
	
     MPI_Allreduce(&FWImax,&FWImax_all,  1,MPI_FLOAT,MPI_MAX,MPI_COMM_WORLD);
     MPI_Allreduce(&GRAVmax,&GRAVmax_all,1,MPI_FLOAT,MPI_MAX,MPI_COMM_WORLD);
		
    /* calculate lambda 2, normalized with respect to the maximum gradients */
	if(iter==1){
		LAM_GRAV_GRAD = GAMMA_GRAV * (FWImax_all/GRAVmax_all);
	} 
		 
     /* add gravity gradient to seismic gradient with respect to the density */
     for (i=1;i<=NX;i++){
        for (j=1;j<=NY;j++){
			
            fwiPSV.waveconv_rho[j][i] += LAM_GRAV_GRAD * grad_grav[j][i];
				
        }
     }
		
   }

/* apply smoothness constraints to gradients */
smooth_grad(fwiPSV.waveconv, matAC.ppi);
smooth_grad(fwiPSV.waveconv_rho, matAC.ppi);

/* Preconditioning of gradients after shot summation and smoothing */
precond_AC(&fwiPSV,&acq,nsrc,ntr_glob,taper_coeff,FP_GRAV);

/* Use preconditioned conjugate gradient optimization method */
if(GRAD_METHOD==1){

    /* calculate steepest descent direction */
    descent(fwiPSV.waveconv,fwiPSV.gradp);
    descent(fwiPSV.waveconv_rho,fwiPSV.gradp_rho);

    /* store current gradients in PCG_new vector */
    store_PCG_AC(PCG_new,fwiPSV.gradp,fwiPSV.gradp_rho);

    /* apply PCG method */
    PCG(PCG_new,PCG_old,PCG_dir,PCG_class);

    /* extract CG-search directions */
    extract_PCG_AC(PCG_dir,fwiPSV.waveconv,fwiPSV.waveconv_rho);

    /* store old gradients in PCG_old vector */
    store_PCG_AC(PCG_old,fwiPSV.gradp,fwiPSV.gradp_rho);

    /* steepest descent direction -> gradient direction */
    descent(fwiPSV.waveconv,fwiPSV.waveconv);
    descent(fwiPSV.waveconv_rho,fwiPSV.waveconv_rho);

}

/* Use l-BFGS optimization */
if(GRAD_METHOD==2){ 

    /* store models and gradients in l-BFGS vectors */
    store_LBFGS_AC(taper_coeff, nsrc, acq.srcpos, acq.recpos, ntr_glob, iter, fwiPSV.waveconv, fwiPSV.gradp, fwiPSV.waveconv_rho, 
		    fwiPSV.gradp_rho, y_LBFGS, s_LBFGS, q_LBFGS, matAC.ppi, matAC.prho, NXNYI, LBFGS_pointer, NLBFGS, NLBFGS_vec);

    /* apply l-BFGS optimization */
    LBFGS(iter, y_LBFGS, s_LBFGS, rho_LBFGS, alpha_LBFGS, q_LBFGS, r_LBFGS, beta_LBFGS, LBFGS_pointer, NLBFGS, NLBFGS_vec);

    /* extract gradients and save old models/gradients for next l-BFGS iteration */
    extract_LBFGS_AC(iter, fwiPSV.waveconv, fwiPSV.gradp, fwiPSV.waveconv_rho, fwiPSV.gradp_rho, matAC.ppi, matAC.prho, r_LBFGS);

}

opteps_vp=0.0;
opteps_rho=0.0;

/* ============================================================================================================================*/
/* =============================================== test loop L2 ===============================================================*/
/* ============================================================================================================================*/

/* set min_iter_help to initial global value of MIN_ITER */
if(iter==1){min_iter_help=MIN_ITER;}

/* Estimate optimum step length ... */

/* ... by line search (parabolic fitting) */
eps_scale = step_length_est_ac(&waveAC,&waveAC_PML,&matAC,&fwiPSV,&mpiPSV,&seisPSV,&seisPSVfwi,&acq,hc,iter,nsrc,ns,ntr,ntr_glob,epst1,L2t,nsrc_glob,nsrc_loc,&step1,&step3,nxgrav,nygrav,ngrav,gravpos,gz_mod,NZGRAV,
                                ntr_loc,Ws,Wr,hin,DTINV_help,req_send,req_rec);

/* no model update due to steplength estimation failed or update with the smallest steplength if the number of iteration is smaller than the minimum number of iteration per
frequency MIN_ITER */
if((iter>min_iter_help)&&(step1==0)){ 
	eps_scale=0.0;
	opteps_vp=0.0;
}
else{
	opteps_vp=eps_scale;
}

/* write log-parameter files */
if(MYID==0){
printf("MYID = %d \t opteps_vp = %e \t opteps_vs = %e \t opteps_rho = %e \n",MYID,opteps_vp,opteps_vs,opteps_rho);
printf("MYID = %d \t L2t[1] = %e \t L2t[2] = %e \t L2t[3] = %e \t L2t[4] = %e \n",MYID,L2t[1],L2t[2],L2t[3],L2t[4]);
printf("MYID = %d \t epst1[1] = %e \t epst1[2] = %e \t epst1[3] = %e \n",MYID,epst1[1],epst1[2],epst1[3]);

/*output of log file for combined inversion*/
if(iter_true==1 && GRAVITY){
    LAMBDA = fopen("gravity/lambda.dat","w");
}
if(iter_true>1 && GRAVITY){
    LAMBDA = fopen("gravity/lambda.dat","a");
}

if(GRAVITY){
    fprintf(LAMBDA,"%d \t %d \t %e \t %e \t %e \t %e \t %e \t %e \t %e \n",nstage,iter,LAM_GRAV,L2sum,L2_grav,L2t[4],LAM_GRAV_GRAD,FWImax_all,GRAVmax_all);
    fclose(LAMBDA);
}

}

if(MYID==0){
if (TIME_FILT==0){
	fprintf(FPL2,"%e \t %e \t %e \t %e \t %e \t %e \t %e \t %e \t %d \n",opteps_vp,epst1[1],epst1[2],epst1[3],L2t[1],L2t[2],L2t[3],L2t[4],nstage);}
else{
	fprintf(FPL2,"%e \t %e \t %e \t %e \t %e \t %e \t %e \t %e \t %f \t %f \t %d \n",opteps_vp,epst1[1],epst1[2],epst1[3],L2t[1],L2t[2],L2t[3],L2t[4],FC_START,FC,nstage);}}


/* saving history of final L2*/
L2_hist[iter]=L2t[4];
s=0;


/* calculate optimal change in the material parameters */
eps_true=calc_mat_change_test_AC(fwiPSV.waveconv,fwiPSV.waveconv_rho,fwiPSV.prho_old,matAC.prho,fwiPSV.ppi_old,matAC.ppi,iter,1,eps_scale,0);

/* smoothing the velocity models vp and vs */
/*if (MODEL_FILTER){
smooth_model(matPSV.ppi,matPSV.pu,matPSV.prho,iter);
}*/

if(MYID==0){	
/*	fprintf(FPL2,"=============================================================\n");
	fprintf(FPL2,"=============================================================\n");
	fprintf(FPL2,"STATISTICS FOR ITERATION STEP %d \n",iter);
	fprintf(FPL2,"=============================================================\n");
	fprintf(FPL2,"=============================================================\n");*/
/*	fprintf(FPL2,"Low-pass filter at %e Hz\n",freq);
	fprintf(FPL2,"----------------------------------------------\n");
*/	/*fprintf(FPL2,"L2 at iteration step n = %e \n",L2);*/
/*        fprintf(FPL2,"%e \t %e \t %e \t %e \t %e \t %e \t %e \t %e \n",EPSILON,EPSILON_u,EPSILON_rho,L2t[4],betaVp,betaVs,betarho,sqrt(C_vp));*/

	/*fprintf(FPL2,"----------------------------------------------\n");*/
/*	fprintf(FPL2,"=============================================================\n");
	fprintf(FPL2,"=============================================================\n\n\n");*/
}

if(MYID==0){
  fclose(FPL2);
}

if (iter>min_iter_help){

float diff=0.0, pro=PRO;

/* calculating differnce of the actual L2 and before two iterations, dividing with L2_hist[iter-2] provide changing in procent*/
diff=fabs((L2_hist[iter-2]-L2_hist[iter])/L2_hist[iter-2]);
	
	if((diff<=pro)||(step3==1)){
        
        	/* output of the model at the end of given FWI stage */
		if(INV_MOD_OUT==0){
        	    model_freq_out_AC(matAC.ppi,matAC.prho,nstage,FC);
		}
		s=1;
		min_iter_help=0;
		min_iter_help=iter+MIN_ITER;
		iter=0;

        	if(GRAD_METHOD==1){
	  		zero_PCG(PCG_old, PCG_new, PCG_dir, PCG_vec);
		}

        	if(GRAD_METHOD==2){
	  		zero_LBFGS(NLBFGS, NLBFGS_vec, y_LBFGS, s_LBFGS, q_LBFGS, r_LBFGS, alpha_LBFGS, beta_LBFGS, rho_LBFGS);
          		LBFGS_pointer = 1;  
		}

        	if(MYID==0){
			if(step3==1){
			        printf("\n Steplength estimation failed step3=%d \n Changing to next FWI stage \n",step3);
			}
			else{
  				printf("\n Reached the abort criterion of pro=%e and diff=%e \n Changing to next FWI stage \n",pro,diff);
			}
	
		}
		break;
	}
}

/* output of the model after each FWI iteration */
if(INV_MOD_OUT==1){
    model_it_out_AC(matAC.ppi,matAC.prho,nstage,iter,FC);
}

iter++;
iter_true++;

/* ====================================== */
} /* end of fullwaveform iteration loop*/
/* ====================================== */

} /* End of FWI-workflow loop */

if(GRAD_METHOD==1){
  free_vector(PCG_old,1,PCG_vec);
  free_vector(PCG_new,1,PCG_vec);
  free_vector(PCG_dir,1,PCG_vec);
}

/* deallocate memory for PSV forward problem */
dealloc_AC(&waveAC,&waveAC_PML);

/* deallocation of memory */
free_matrix(fwiPSV.Vp0,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.Rho0,-nd+1,NY+nd,-nd+1,NX+nd);

free_matrix(matAC.prho,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.prho_old,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(matAC.prip,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(matAC.prjp,-nd+1,NY+nd,-nd+1,NX+nd);

free_matrix(matAC.ppi,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.ppi_old,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_lam,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_shot,-nd+1,NY+nd,-nd+1,NX+nd);

free_matrix(mpiPSV.bufferlef_to_rig,1,NY,1,fdo3);
free_matrix(mpiPSV.bufferrig_to_lef,1,NY,1,fdo3);
free_matrix(mpiPSV.buffertop_to_bot,1,NX,1,fdo3);
free_matrix(mpiPSV.bufferbot_to_top,1,NX,1,fdo3);

free_vector(hc,0,6);

free_matrix(fwiPSV.gradg,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.gradp,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.gradg_rho,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.gradp_rho,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_rho,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_rho_s,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_rho_shot,-nd+1,NY+nd,-nd+1,NX+nd);

free_vector(fwiPSV.forward_prop_x,1,NY*NX*NT);
free_vector(fwiPSV.forward_prop_rho_x,1,NY*NX*NT);
free_vector(fwiPSV.forward_prop_rho_y,1,NY*NX*NT);

if (nsrc_loc>0){	
	free_matrix(acq.signals,1,nsrc_loc,1,NT);
	free_matrix(acq.srcpos_loc,1,8,1,nsrc_loc);
	free_matrix(acq.srcpos_loc_back,1,6,1,nsrc_loc);
}		   

 /* free memory for global source positions */
 free_matrix(acq.srcpos,1,8,1,nsrc);

 /* free memory for source position definition */
 free_matrix(acq.srcpos1,1,8,1,1);
 
 /* free memory for abort criterion */
 free_dvector(L2_hist,1,1000);
 		
 free_dvector(L2t,1,4);
 free_vector(epst1,1,3);

 if(READREC!=2){

    if (SEISMO) free_imatrix(acq.recpos,1,3,1,ntr_glob);

    if ((ntr>0) && (SEISMO)){

            free_imatrix(acq.recpos_loc,1,3,1,ntr);
            acq.recpos_loc = NULL;
 
            switch (SEISMO){
            case 1 : /* particle velocities only */
                    free_matrix(seisPSV.sectionvx,1,ntr,1,ns);
                    free_matrix(seisPSV.sectionvy,1,ntr,1,ns);
                    seisPSV.sectionvx=NULL;
                    seisPSV.sectionvy=NULL;
                    break;
             case 2 : /* pressure only */
                    free_matrix(seisPSV.sectionp,1,ntr,1,ns);
                    break;
             case 3 : /* curl and div only */
                    free_matrix(seisPSV.sectioncurl,1,ntr,1,ns);
                    free_matrix(seisPSV.sectiondiv,1,ntr,1,ns);
                    break;
             case 4 : /* everything */
                    free_matrix(seisPSV.sectionvx,1,ntr,1,ns);
                    free_matrix(seisPSV.sectionvy,1,ntr,1,ns);
                    free_matrix(seisPSV.sectionp,1,ntr,1,ns);
                    free_matrix(seisPSV.sectioncurl,1,ntr,1,ns);
                    free_matrix(seisPSV.sectiondiv,1,ntr,1,ns);
                    break;

             }

    }

    free_matrix(seisPSVfwi.sectionread,1,ntr_glob,1,ns);
    free_ivector(acq.recswitch,1,ntr);
    
    if((QUELLTYPB==1)||(QUELLTYPB==3)||(QUELLTYPB==5)||(QUELLTYPB==7)){
       free_matrix(seisPSVfwi.sectionvxdata,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionvxdiff,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionvxdiffold,1,ntr,1,ns);
    }

    if((QUELLTYPB==1)||(QUELLTYPB==2)||(QUELLTYPB==6)||(QUELLTYPB==7)){    
       free_matrix(seisPSVfwi.sectionvydata,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionvydiff,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionvydiffold,1,ntr,1,ns);
    }
    
    if(QUELLTYPB>=4){    
       free_matrix(seisPSVfwi.sectionpdata,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionpdiff,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionpdiffold,1,ntr,1,ns);
    }    

    if(SEISMO){
       free_matrix(seisPSV.fulldata,1,ntr_glob,1,NT); 
    }

    if(SEISMO==1){
       free_matrix(seisPSV.fulldata_vx,1,ntr_glob,1,NT);
       free_matrix(seisPSV.fulldata_vy,1,ntr_glob,1,NT);
    }

    if(SEISMO==2){
       free_matrix(seisPSV.fulldata_p,1,ntr_glob,1,NT);
    } 
 
    if(SEISMO==3){
       free_matrix(seisPSV.fulldata_curl,1,ntr_glob,1,NT);
       free_matrix(seisPSV.fulldata_div,1,ntr_glob,1,NT);
    }

    if(SEISMO==4){
       free_matrix(seisPSV.fulldata_vx,1,ntr_glob,1,NT);
       free_matrix(seisPSV.fulldata_vy,1,ntr_glob,1,NT);
       free_matrix(seisPSV.fulldata_p,1,ntr_glob,1,NT); 
       free_matrix(seisPSV.fulldata_curl,1,ntr_glob,1,NT);
       free_matrix(seisPSV.fulldata_div,1,ntr_glob,1,NT);
    }
 
 }

 free_ivector(DTINV_help,1,NT);
 
 /* free memory for viscoelastic modeling variables */
 /*if (L) {
		free_matrix(matPSV.ptaus,-nd+1,NY+nd,-nd+1,NX+nd);
		free_matrix(matPSV.ptausipjp,-nd+1,NY+nd,-nd+1,NX+nd);
		free_matrix(matPSV.ptaup,-nd+1,NY+nd,-nd+1,NX+nd);
		free_vector(matPSV.peta,1,L);
		free_vector(matPSV.etaip,1,L);
		free_vector(matPSV.etajm,1,L);
		free_vector(matPSV.bip,1,L);
		free_vector(matPSV.bjm,1,L);
		free_vector(matPSV.cip,1,L);
		free_vector(matPSV.cjm,1,L);
		free_matrix(matPSV.f,-nd+1,NY+nd,-nd+1,NX+nd);
		free_matrix(matPSV.g,-nd+1,NY+nd,-nd+1,NX+nd);
		free_matrix(matPSV.fipjp,-nd+1,NY+nd,-nd+1,NX+nd);
		free_f3tensor(matPSV.dip,-nd+1,NY+nd,-nd+1,NX+nd,1,L);
		free_f3tensor(matPSV.d,-nd+1,NY+nd,-nd+1,NX+nd,1,L);
		free_f3tensor(matPSV.e,-nd+1,NY+nd,-nd+1,NX+nd,1,L);
}*/

if(GRAVITY){

  free_matrix(gravpos,1,2,1,ngrav);
  free_vector(gz_mod,1,ngrav);
  free_vector(gz_res,1,ngrav);

  if(GRAVITY==2){
    free_matrix(grad_grav,1,NY,1,NX);
  }

}
 
/* de-allocate buffer for messages */
MPI_Buffer_detach(buff_addr,&buffsize);

MPI_Barrier(MPI_COMM_WORLD);

if (MYID==0){
	fprintf(FP,"\n **Info from main (written by PE %d): \n",MYID);
	fprintf(FP," CPU time of program per PE: %li seconds.\n",clock()/CLOCKS_PER_SEC);
	time8=MPI_Wtime();
	fprintf(FP," Total real time of program: %4.2f seconds.\n",time8-time1);
	time_av_v_update=time_av_v_update/(double)NT;
	time_av_s_update=time_av_s_update/(double)NT;
	time_av_v_exchange=time_av_v_exchange/(double)NT;
	time_av_s_exchange=time_av_s_exchange/(double)NT;
	time_av_timestep=time_av_timestep/(double)NT;
	/* fprintf(FP," Average times for \n");
	fprintf(FP," velocity update:  \t %5.3f seconds  \n",time_av_v_update);
	fprintf(FP," stress update:  \t %5.3f seconds  \n",time_av_s_update);
	fprintf(FP," velocity exchange:  \t %5.3f seconds  \n",time_av_v_exchange);
	fprintf(FP," stress exchange:  \t %5.3f seconds  \n",time_av_s_exchange);
	fprintf(FP," timestep:  \t %5.3f seconds  \n",time_av_timestep);*/
		
}

fclose(FP);


}



