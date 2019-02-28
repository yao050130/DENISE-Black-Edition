/*------------------------------------------------------------------------
 * Module for extraction of Preconditioned Conjugate Gradient Method (PCG)
 * for the material parameters vs, rho and mu, rho respectively
 * 
 * Daniel Koehn
 * Kiel, 22.12.2017
 * ----------------------------------------------------------------------*/

#include "fd.h"

void extract_PCG_SH(float * PCG_old, float ** waveconv_u, float ** waveconv_rho){

	extern int NX, NY, IDX, IDY;
	int i, j, h;	

	/* ============================================================================================================================================================== */
	/* ===================================================== GRADIENT VS/ZS/mu ====================================================================================== */
	/* ============================================================================================================================================================== */
	
	h=1;
	/* store gradient */
	for (i=1;i<=NX;i=i+IDX){
	   for (j=1;j<=NY;j=j+IDY){

		 waveconv_u[j][i] = PCG_old[h];

                 h++;
	   }
	}

	/* ============================================================================================================================================================== */
	/* ===================================================== GRADIENT rho =========================================================================================== */
	/* ============================================================================================================================================================== */

	/* store gradient */
	for (i=1;i<=NX;i=i+IDX){
	   for (j=1;j<=NY;j=j+IDY){

		 waveconv_rho[j][i] = PCG_old[h];

                 h++;
	   }
	}


}
