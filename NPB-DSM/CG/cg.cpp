/*--------------------------------------------------------------------

	Information on NAS Parallel Benchmarks is available at:

	http://www.nas.nasa.gov/Software/NPB/

	Authors: M. Yarrow
           C. Kuszmaul

	CPP and OpenMP version:
  			Dalvan Griebler <dalvangriebler@gmail.com>
			Júnior Löff <loffjh@gmail.com>

--------------------------------------------------------------------*/

/*
c---------------------------------------------------------------------
c  Note: please observe that in the routine conj_grad three 
c  implementations of the sparse matrix-vector multiply have
c  been supplied.  The default matrix-vector multiply is not
c  loop unrolled.  The alternate implementations are unrolled
c  to a depth of 2 and unrolled to a depth of 8.  Please
c  experiment with these to find the fastest for your particular
c  architecture.  If reporting timing results, any of these three may
c  be used without penalty.
c---------------------------------------------------------------------
*/

#include "argo.hpp"
#include <iostream>
#include "npbparams.hpp"
#include "npb-CPP.hpp"

#define	NZ	NA*(NONZER+1)*(NONZER+1)+NA*(NONZER+2)

/* global variables */

/* common /partit_size/ */
static int naa;
static int nzz;
static int firstrow;
static int lastrow;
static int firstcol;
static int lastcol;

/* common /main_int_mem/ */
int *colidx;	/* colidx[1:NZ] 	*/
int *rowstr;	/* rowstr[1:NA+1] 	*/
int *iv;		/* iv[1:2*NA+1] 	*/
int *arow;		/* arow[1:NZ] 		*/
int *acol;		/* acol[1:NZ] 		*/

/* common /main_flt_mem/ */
double *v;		/* v[1:NA+1] 		*/
double *aelt;	/* aelt[1:NZ] 		*/
double *a;		/* a[1:NZ] */

/* common /global_mem/ */
double *gnorm_temps;
double *x;		/* x[1:NA+2]		*/
double *z;		/* z[1:NA+2] 		*/
double *p;		/* p[1:NA+2] 		*/
double *q;		/* q[1:NA+2] 		*/
double *r;		/* r[1:NA+2] 		*/
double *w;		/* w[1:NA+2] 		*/

/* common /urando/ */
static double amult;
static double tran;

/* common /rank,tasks,threads/ */
int workrank;
int numtasks;
int nthreads;

/* function declarations */
static void conj_grad (int colidx[], int rowstr[], double x[], 
	double z[], double a[], double p[], double q[], double r[], 
	double w[], double *rnorm);
static void makea(int n, int nz, double a[], int colidx[], int rowstr[],
	int nonzer, int firstrow, int lastrow, int firstcol,
	int lastcol, double rcond, int arow[], int acol[],
	double aelt[], double v[], int iv[], double shift );
static void sparse(double a[], int colidx[], int rowstr[], int n,
	int arow[], int acol[], double aelt[],
	int firstrow, int lastrow,
	double x[], boolean mark[], int nzloc[], int nnza);
static void sprnvc(int n, int nz, double v[], int iv[], int nzloc[], int mark[]);
static int icnvrt(double x, int ipwr2);
static void vecset(int n, double v[], int iv[], int *nzv, int i, double val);

/*--------------------------------------------------------------------
      program cg
--------------------------------------------------------------------*/

int main(int argc, char **argv)
{
	argo::init(20*1024*1024*1024UL);

	int	i, j, k, it;
	double zeta;
	double rnorm;
	double norm_temp11;
	double norm_temp12;
	double t, mflops;
	char class_npb;
	boolean verified;
	double zeta_verify_value, epsilon;

	#pragma omp parallel
	{
		#if defined(_OPENMP)
		#pragma omp master
			nthreads = omp_get_num_threads();
		#endif /* _OPENMP */
	}

	colidx = new int[NZ+1];
	rowstr = new int[NA+1+1];
	iv = new int[2*NA+1+1];
	arow = new int[NZ+1];
	acol = new int[NZ+1];

	v = new double[NA+1+1];
	aelt = new double[NZ+1];
	a = new double[NZ+1];

	workrank = argo::node_id();
    numtasks = argo::number_of_nodes();

	gnorm_temps = argo::conew_array<double>(2*numtasks);
	x = argo::conew_array<double>(NA+2+1);
	z = argo::conew_array<double>(NA+2+1);
	p = argo::conew_array<double>(NA+2+1);
	q = argo::conew_array<double>(NA+2+1);
	r = argo::conew_array<double>(NA+2+1);
	w = argo::conew_array<double>(NA+2+1);

	firstrow = 1;
	lastrow  = NA;
	firstcol = 1;
	lastcol  = NA;

	if (NA == 1400 && NONZER == 7 && NITER == 15 && SHIFT == 10.0) {
		class_npb = 'S';
		zeta_verify_value = 8.5971775078648;
	} else if (NA == 7000 && NONZER == 8 && NITER == 15 && SHIFT == 12.0) {
		class_npb = 'W';
		zeta_verify_value = 10.362595087124;
	} else if (NA == 14000 && NONZER == 11 && NITER == 15 && SHIFT == 20.0) {
		class_npb = 'A';
		zeta_verify_value = 17.130235054029;
	} else if (NA == 75000 && NONZER == 13 && NITER == 75 && SHIFT == 60.0) {
		class_npb = 'B';
		zeta_verify_value = 22.712745482631;
	} else if (NA == 150000 && NONZER == 15 && NITER == 75 && SHIFT == 110.0) {
		class_npb = 'C';
		zeta_verify_value = 28.973605592845;
	} else if (NA == 1500000 && NONZER == 21 && NITER == 100 && SHIFT == 500.0) {
		class_npb = 'D';
		zeta_verify_value = 52.514532105794;
	} else if (NA == 9000000 && NONZER == 26 && NITER == 100 && SHIFT == 1.5e3) {
		class_npb = 'E';
		zeta_verify_value = 77.522164599383;
	} else if (NA == 54000000 && NONZER == 31 && NITER == 100 && SHIFT == 5.0e3) {
		class_npb = 'F';
		zeta_verify_value = 107.3070826433;
	} else {
		class_npb = 'U';
	}

	if (workrank == 0) {
		printf("\n\n NAS Parallel Benchmarks 4.0 OpenMP C++ version"" - CG Benchmark\n");
		printf("\n\n Developed by: Dalvan Griebler <dalvan.griebler@acad.pucrs.br>\n");
		printf(" Size: %10d\n", NA);
		printf(" Iterations: %5d\n", NITER);
	}

	naa = NA;
	nzz = NZ;

	/*--------------------------------------------------------------------
	c  Initialize random number generator
	c-------------------------------------------------------------------*/
	tran    = 314159265.0;
	amult   = 1220703125.0;
	zeta    = randlc( &tran, amult );

	/*--------------------------------------------------------------------
	c  
	c-------------------------------------------------------------------*/
	makea(naa, nzz, a, colidx, rowstr, NONZER,
	firstrow, lastrow, firstcol, lastcol, 
	RCOND, arow, acol, aelt, v, iv, SHIFT);

	/*---------------------------------------------------------------------
	c  Note: as a result of the above call to makea:
	c        values of j used in indexing rowstr go from 1 --> lastrow-firstrow+1
	c        values of colidx which are col indexes go from firstcol --> lastcol
	c        So:
	c        Shift the col index vals from actual (firstcol --> lastcol ) 
	c        to local, i.e., (1 --> lastcol-firstcol+1)
	c---------------------------------------------------------------------*/
	#pragma omp parallel private(it,i,j,k)	
	{
		#pragma omp for nowait
		for (j = 1; j <= lastrow - firstrow + 1; j++) {
			for (k = rowstr[j]; k < rowstr[j+1]; k++) {
				colidx[k] = colidx[k] - firstcol + 1;
			}
		}
		
		/*--------------------------------------------------------------------
		c  set starting vector to (1, 1, .... 1)
		c-------------------------------------------------------------------*/
		int chunk = (NA+1) / numtasks;
    	int beg = 1 + workrank * chunk;
    	int end = (workrank != numtasks - 1) ? workrank * chunk + chunk : NA+1;

		#pragma omp for nowait
		for (i = beg; i <= end; i++) {
			x[i] = 1.0;
		}
		#pragma omp single
			zeta  = 0.0;

		/*-------------------------------------------------------------------
		c---->
		c  Do one iteration untimed to init all code and data page tables
		c---->                    (then reinit, start timing, to niter its)
		c-------------------------------------------------------------------*/

		for (it = 1; it <= 1; it++) {

			/*--------------------------------------------------------------------
			c  The call to the conjugate gradient routine:
			c-------------------------------------------------------------------*/
			conj_grad (colidx, rowstr, x, z, a, p, q, r, w, &rnorm);

			/*--------------------------------------------------------------------
			c  zeta = shift + 1/(x.z)
			c  So, first: (x.z)
			c  Also, find norm of z
			c  So, first: (z.z)
			c-------------------------------------------------------------------*/
			chunk = (lastcol-firstcol+1) / numtasks;
    		beg = 1 + workrank * chunk;
    		end = (workrank != numtasks - 1) ? workrank * chunk + chunk : lastcol-firstcol+1;

			#pragma omp single
			{	
				norm_temp11 = 0.0;
				norm_temp12 = 0.0;
			} /* end single */

			#pragma omp for reduction(+:norm_temp11,norm_temp12)
			for (j = beg; j <= end; j++) {
				norm_temp11 = norm_temp11 + x[j]*z[j];
				norm_temp12 = norm_temp12 + z[j]*z[j];
			}
			#pragma omp single
				norm_temp12 = 1.0 / sqrt( norm_temp12 );

			/*--------------------------------------------------------------------
			c  Normalize z to obtain x
			c-------------------------------------------------------------------*/
			#pragma omp for
			for (j = beg; j <= end; j++) {
				x[j] = norm_temp12*z[j];
			}

		} /* end of do one iteration untimed */

		/*--------------------------------------------------------------------
		c  set starting vector to (1, 1, .... 1)
		c-------------------------------------------------------------------*/
		chunk = (NA+1) / numtasks;
    	beg = 1 + workrank * chunk;
    	end = (workrank != numtasks - 1) ? workrank * chunk + chunk : NA+1;

		#pragma omp for nowait
		for (i = beg; i <= end; i++) {
			x[i] = 1.0;
		}
		#pragma omp single    
			zeta  = 0.0;

	} /* end parallel */

	argo::barrier();

	timer_clear( 1 );

	timer_start( 1 );

	/*--------------------------------------------------------------------
	c---->
	c  Main Iteration for inverse power method
	c---->
	c-------------------------------------------------------------------*/

	#pragma omp parallel private(it,i,j,k)
	{
		for (it = 1; it <= NITER; it++) {

			/*--------------------------------------------------------------------
			c  The call to the conjugate gradient routine:
			c-------------------------------------------------------------------*/
			conj_grad(colidx, rowstr, x, z, a, p, q, r, w, &rnorm);

			/*--------------------------------------------------------------------
			c  zeta = shift + 1/(x.z)
			c  So, first: (x.z)
			c  Also, find norm of z
			c  So, first: (z.z)
			c-------------------------------------------------------------------*/
			static int chunk = (lastcol-firstcol+1) / numtasks;
    		static int beg = 1 + workrank * chunk;
    		static int end = (workrank != numtasks - 1) ? workrank * chunk + chunk : lastcol-firstcol+1;

			#pragma omp single
			{	
				norm_temp11 = 0.0;
				norm_temp12 = 0.0;
			} /* end single */

			#pragma omp for reduction(+:norm_temp11,norm_temp12)
			for (j = beg; j <= end; j++) {
				norm_temp11 = norm_temp11 + x[j]*z[j];
				norm_temp12 = norm_temp12 + z[j]*z[j];
			}

			#pragma omp single
			{
				gnorm_temps[2*workrank] = norm_temp11;
				gnorm_temps[2*workrank+1] = norm_temp12;

				argo::barrier();

				for (j = 0; j < numtasks; j++) {
					if (j != workrank) {
						norm_temp11 += gnorm_temps[2*j];
						norm_temp12 += gnorm_temps[2*j+1];
					}
				}

				norm_temp12 = 1.0 / sqrt( norm_temp12 );

				zeta = SHIFT + 1.0 / norm_temp11;

				if (workrank == 0) {
					if( it == 1 ) {
						printf("   iteration           ||r||                 zeta\n");
					}
					printf("    %5d       %20.14e%20.13e\n", it, rnorm, zeta);
				}
			} /* end single */

			/*--------------------------------------------------------------------
			c  Normalize z to obtain x
			c-------------------------------------------------------------------*/
			#pragma omp for 
			for (j = beg; j <= end; j++) {
				x[j] = norm_temp12*z[j];
			} /* end of main iter inv pow meth */

			argo::barrier(nthreads);
		}
	} /* end parallel */

	timer_stop( 1 );
	/*--------------------------------------------------------------------
	c  End of timed section
	c-------------------------------------------------------------------*/

	t = timer_read( 1 );

	if (workrank == 0)
	{
		printf(" Benchmark completed\n");

		epsilon = 1.0e-10;
		if (class_npb != 'U') {
			if (fabs(zeta - zeta_verify_value) <= epsilon) {
				verified = TRUE;
				printf(" VERIFICATION SUCCESSFUL\n");
				printf(" Zeta is    %20.12e\n", zeta);
				printf(" Error is   %20.12e\n", zeta - zeta_verify_value);
			} else {
				verified = FALSE;
				printf(" VERIFICATION FAILED\n");
				printf(" Zeta                %20.12e\n", zeta);
				printf(" The correct zeta is %20.12e\n", zeta_verify_value);
			}
		} else {
			verified = FALSE;
			printf(" Problem size unknown\n");
			printf(" NO VERIFICATION PERFORMED\n");
		}
		if ( t != 0.0 ) {
			mflops = (2.0*NITER*NA)
			* (3.0+(NONZER*(NONZER+1)) + 25.0*(5.0+(NONZER*(NONZER+1))) + 3.0 )
			/ t / 1000000.0;
		} else {
			mflops = 0.0;
		}
		c_print_results((char*)"CG", class_npb, NA, 0, 0, NITER, numtasks*nthreads, t, mflops, (char*)"          floating point",	verified, (char*)NPBVERSION, (char*)COMPILETIME, (char*)CS1, (char*)CS2, (char*)CS3, (char*)CS4, (char*)CS5, (char*)CS6, (char*)CS7);
	}

	delete[] colidx;
	delete[] rowstr;
	delete[] iv;
	delete[] arow;
	delete[] acol;

	delete[] v;
	delete[] aelt;
	delete[] a;

	argo::codelete_array(gnorm_temps);
	argo::codelete_array(x);
	argo::codelete_array(z);
	argo::codelete_array(p);
	argo::codelete_array(q);
	argo::codelete_array(r);
	argo::codelete_array(w);

	argo::finalize();

	return 0;
}

/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/
static void conj_grad (
	int colidx[],	/* colidx[1:nzz] */
	int rowstr[],	/* rowstr[1:naa+1] */
	double x[],		/* x[*] */
	double z[],		/* z[*] */
	double a[],		/* a[1:nzz] */
	double p[],		/* p[*] */
	double q[],		/* q[*] */
	double r[],		/* r[*] */
	double w[],		/* w[*] */
	double *rnorm )
/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/
    
/*---------------------------------------------------------------------
c  Floaging point arrays here are named as in NPB1 spec discussion of 
c  CG algorithm
c---------------------------------------------------------------------*/
{
	static double d, sum, rho, rho0, alpha, beta;
	int j, k;
	int cgit, cgitmax = 25;

	static int chunk_naa = (naa+1) / numtasks;
    static int beg_naa = 1 + workrank * chunk_naa;
    static int end_naa = (workrank != numtasks - 1) ? workrank * chunk_naa + chunk_naa : naa+1;

	static int chunk_row = (lastrow-firstrow+1) / numtasks;
	static int beg_row = 1 + workrank * chunk_row;
	static int end_row = (workrank != numtasks - 1) ? workrank * chunk_row + chunk_row : lastrow-firstrow+1;

	static int chunk_col = (lastcol-firstcol+1) / numtasks;
    static int beg_col = 1 + workrank * chunk_col;
    static int end_col = (workrank != numtasks - 1) ? workrank * chunk_col + chunk_col : lastcol-firstcol+1;

	#pragma omp single nowait
		rho = 0.0;

	/*--------------------------------------------------------------------
	c  Initialize the CG algorithm:
	c-------------------------------------------------------------------*/
	#pragma omp for nowait
	    for (j = beg_naa; j <= end_naa; j++) {
			q[j] = 0.0;
			z[j] = 0.0;
			r[j] = x[j];
			p[j] = r[j];
			w[j] = 0.0;
	    }

	/*--------------------------------------------------------------------
	c  rho = r.r
	c  Now, obtain the norm of r: First, sum squares of r elements locally...
	c-------------------------------------------------------------------*/	
	#pragma omp for reduction(+:rho)
	for (j = beg_col; j <= end_col; j++) {
		rho = rho + x[j]*x[j];
	}

	#pragma omp master
	gnorm_temps[workrank] = rho;

	argo::barrier(nthreads);

	#pragma omp single
	for (j = 0; j < numtasks; j++)
		if (j != workrank)
			rho += gnorm_temps[j];

	/*--------------------------------------------------------------------
	c---->
	c  The conj grad iteration loop
	c---->
	c-------------------------------------------------------------------*/
    for (cgit = 1; cgit <= cgitmax; cgit++) {
		#pragma omp single nowait
		{	
			rho0 = rho;
			d = 0.0;
			rho = 0.0;
		} /* end single */
      
		/*--------------------------------------------------------------------
		c  q = A.p
		c  The partition submatrix-vector multiply: use workspace w
		c---------------------------------------------------------------------
		C
		C  NOTE: this version of the multiply is actually (slightly: maybe %5) 
		C        faster on the sp2 on 16 nodes than is the unrolled-by-2 version 
		C        below.   On the Cray t3d, the reverse is true, i.e., the 
		C        unrolled-by-two version is some 10% faster.  
		C        The unrolled-by-8 version below is significantly faster
		C        on the Cray t3d - overall speed of code is 1.5 times faster.
		*/

		argo::barrier(nthreads);

		/* rolled version */      
		#pragma omp for private(sum,k)
		for (j = beg_row; j <= end_row; j++) {
			sum = 0.0;
			for (k = rowstr[j]; k < rowstr[j+1]; k++) {
				sum = sum + a[k]*p[colidx[k]];
		    }
			w[j] = sum;
		}

		argo::barrier(nthreads);
		
	/* unrolled-by-two version
		#pragma omp for private(i,k)
		for (j = 1; j <= lastrow-firstrow+1; j++) {
			int iresidue;
			double sum1, sum2;
			i = rowstr[j]; 
			iresidue = (rowstr[j+1]-i) % 2;
			sum1 = 0.0;
			sum2 = 0.0;
			if (iresidue == 1) sum1 = sum1 + a[i]*p[colidx[i]];
			for (k = i+iresidue; k <= rowstr[j+1]-2; k += 2) {
				sum1 = sum1 + a[k]   * p[colidx[k]];
				sum2 = sum2 + a[k+1] * p[colidx[k+1]];
			}
			w[j] = sum1 + sum2;
		}
	*/
	/* unrolled-by-8 version
		#pragma omp for private(i,k,sum)
		for (j = 1; j <= lastrow-firstrow+1; j++) {
			int iresidue;
			i = rowstr[j]; 
			iresidue = (rowstr[j+1]-i) % 8;
			sum = 0.0;
			for (k = i; k <= i+iresidue-1; k++) {
				sum = sum +  a[k] * p[colidx[k]];
			}
			for (k = i+iresidue; k <= rowstr[j+1]-8; k += 8) {
				sum = sum + a[k  ] * p[colidx[k  ]]
				+ a[k+1] * p[colidx[k+1]]
				+ a[k+2] * p[colidx[k+2]]
				+ a[k+3] * p[colidx[k+3]]
				+ a[k+4] * p[colidx[k+4]]
				+ a[k+5] * p[colidx[k+5]]
				+ a[k+6] * p[colidx[k+6]]
				+ a[k+7] * p[colidx[k+7]];
			}
			w[j] = sum;
		}
	*/
		
		#pragma omp for
		for (j = beg_col; j <= end_col; j++) {
			q[j] = w[j];
		}
	
		/*--------------------------------------------------------------------
		c  Clear w for reuse...
		c-------------------------------------------------------------------*/
		#pragma omp for	nowait
		for (j = beg_col; j <= end_col; j++) {
			w[j] = 0.0;
		}

		/*--------------------------------------------------------------------
		c  Obtain p.q
		c-------------------------------------------------------------------*/
		#pragma omp for reduction(+:d)
		for (j = beg_col; j <= end_col; j++) {
			d = d + p[j]*q[j];
		}

		#pragma omp master
		gnorm_temps[workrank] = d;

		argo::barrier(nthreads);

		#pragma omp single
		for (j = 0; j < numtasks; j++)
			if (j != workrank)
				d += gnorm_temps[j];

		/*--------------------------------------------------------------------
		c  Obtain alpha = rho / (p.q)
		c-------------------------------------------------------------------*/
		#pragma omp single	
			alpha = rho0 / d;

		/*--------------------------------------------------------------------
		c  Save a temporary of rho
		c-------------------------------------------------------------------*/
			/*	rho0 = rho;*/

		/*---------------------------------------------------------------------
		c  Obtain z = z + alpha*p
		c  and    r = r - alpha*q
		c---------------------------------------------------------------------*/
		#pragma omp for
		for (j = beg_col; j <= end_col; j++) {
			z[j] = z[j] + alpha*p[j];
			r[j] = r[j] - alpha*q[j];
		}

		argo::barrier(nthreads);
            
		/*---------------------------------------------------------------------
		c  rho = r.r
		c  Now, obtain the norm of r: First, sum squares of r elements locally...
		c---------------------------------------------------------------------*/
		#pragma omp for reduction(+:rho)	
		for (j = beg_col; j <= end_col; j++) {
			rho = rho + r[j]*r[j];
		}

		#pragma omp master
		gnorm_temps[workrank] = rho;

		argo::barrier(nthreads);

		#pragma omp single
		for (j = 0; j < numtasks; j++)
			if (j != workrank)
				rho += gnorm_temps[j];

		/*--------------------------------------------------------------------
		c  Obtain beta:
		c-------------------------------------------------------------------*/
		#pragma omp single	
		beta = rho / rho0;

		/*--------------------------------------------------------------------
		c  p = r + beta*p
		c-------------------------------------------------------------------*/
		#pragma omp for
		for (j = beg_col; j <= end_col; j++) {
			p[j] = r[j] + beta*p[j];
		}
	} /* end of do cgit=1,cgitmax */

	/*---------------------------------------------------------------------
	c  Compute residual norm explicitly:  ||r|| = ||x - A.z||
	c  First, form A.z
	c  The partition submatrix-vector multiply
	c---------------------------------------------------------------------*/
	#pragma omp single nowait
		sum = 0.0;
    
	#pragma omp for private(d, k)
	for (j = beg_row; j <= end_row; j++) {
		d = 0.0;
		for (k = rowstr[j]; k <= rowstr[j+1]-1; k++) {
			d = d + a[k]*z[colidx[k]];
		}
		w[j] = d;
	}

	argo::barrier(nthreads);

	#pragma omp for
	for (j = beg_col; j <= end_col; j++) {
		r[j] = w[j];
	}

	/*--------------------------------------------------------------------
	c  At this point, r contains A.z
	c-------------------------------------------------------------------*/
	#pragma omp for reduction(+:sum) private(d)
	for (j = beg_col; j <= end_col; j++) {
		d = x[j] - r[j];
		sum = sum + d*d;
	}

	#pragma omp single nowait
		gnorm_temps[workrank] = sum;

	argo::barrier(nthreads);

	#pragma omp single
	for (j = 0; j < numtasks; j++)
		if (j != workrank)
			sum += gnorm_temps[j];

	#pragma omp single
	{
		(*rnorm) = sqrt(sum);
	} /* end single */
}

/*---------------------------------------------------------------------
c       generate the test problem for benchmark 6
c       makea generates a sparse matrix with a
c       prescribed sparsity distribution
c
c       parameter    type        usage
c
c       input
c
c       n            i           number of cols/rows of matrix
c       nz           i           nonzeros as declared array size
c       rcond        r*8         condition number
c       shift        r*8         main diagonal shift
c
c       output
c
c       a            r*8         array for nonzeros
c       colidx       i           col indices
c       rowstr       i           row pointers
c
c       workspace
c
c       iv, arow, acol i
c       v, aelt        r*8
c---------------------------------------------------------------------*/
static void makea(
	int n,
	int nz,
	double a[],		/* a[1:nz] */
	int colidx[],	/* colidx[1:nz] */
	int rowstr[],	/* rowstr[1:n+1] */
	int nonzer,
	int firstrow,
	int lastrow,
	int firstcol,
	int lastcol,
	double rcond,
	int arow[],		/* arow[1:nz] */
	int acol[],		/* acol[1:nz] */
	double aelt[],	/* aelt[1:nz] */
	double v[],		/* v[1:n+1] */
	int iv[],		/* iv[1:2*n+1] */
	double shift )
{
	int i, nnza, iouter, ivelt, ivelt1, irow, nzv;

	/*--------------------------------------------------------------------
	c      nonzer is approximately  (int(sqrt(nnza /n)));
	c-------------------------------------------------------------------*/

	double size, ratio, scale;
	int jcol;

	size = 1.0;
	ratio = pow(rcond, (1.0 / (double)n));
	nnza = 0;

	/*---------------------------------------------------------------------
	c  Initialize colidx(n+1 .. 2n) to zero.
	c  Used by sprnvc to mark nonzero positions
	c---------------------------------------------------------------------*/
	#pragma omp parallel for
	for (i = 1; i <= n; i++) {
		colidx[n+i] = 0;
	}
	for (iouter = 1; iouter <= n; iouter++) {
		nzv = nonzer;
		sprnvc(n, nzv, v, iv, &(colidx[0]), &(colidx[n]));
		vecset(n, v, iv, &nzv, iouter, 0.5);
		for (ivelt = 1; ivelt <= nzv; ivelt++){
			jcol = iv[ivelt];
			if (jcol >= firstcol && jcol <= lastcol) {
				scale = size * v[ivelt];
				for (ivelt1 = 1; ivelt1 <= nzv; ivelt1++) {
					irow = iv[ivelt1];
					if (irow >= firstrow && irow <= lastrow) {
						nnza = nnza + 1;
						if (nnza > nz) {
							printf("Space for matrix elements exceeded in" " makea\n");
							printf("nnza, nzmax = %d, %d\n", nnza, nz);
							printf("iouter = %d\n", iouter);
							exit(1);
						}
						acol[nnza] = jcol;
						arow[nnza] = irow;
						aelt[nnza] = v[ivelt1] * scale;
					}
				}
			}
		}
		size = size * ratio;
	}

	/*---------------------------------------------------------------------
	c       ... add the identity * rcond to the generated matrix to bound
	c           the smallest eigenvalue from below by rcond
	c---------------------------------------------------------------------*/
	for (i = firstrow; i <= lastrow; i++) {
		if (i >= firstcol && i <= lastcol) {
			iouter = n + i;
			nnza = nnza + 1;
			if (nnza > nz) {
				printf("Space for matrix elements exceeded in makea\n");
				printf("nnza, nzmax = %d, %d\n", nnza, nz);
				printf("iouter = %d\n", iouter);
				exit(1);
			}
			acol[nnza] = i;
			arow[nnza] = i;
			aelt[nnza] = rcond - shift;
		}
	}

	/*---------------------------------------------------------------------
	c       ... make the sparse matrix from list of elements with duplicates
	c           (v and iv are used as  workspace)
	c---------------------------------------------------------------------*/
	sparse(a, colidx, rowstr, n, arow, acol, aelt, firstrow, lastrow, v, &(iv[0]), &(iv[n]), nnza);
}

/*---------------------------------------------------
c       generate a sparse matrix from a list of
c       [col, row, element] tri
c---------------------------------------------------*/
static void sparse(
	double a[],		/* a[1:*] */
	int colidx[],	/* colidx[1:*] */
	int rowstr[],	/* rowstr[1:*] */
	int n,
	int arow[],		/* arow[1:*] */
	int acol[],		/* acol[1:*] */
	double aelt[],	/* aelt[1:*] */
	int firstrow,
	int lastrow,
	double x[],		/* x[1:n] */
	boolean mark[],	/* mark[1:n] */
	int nzloc[],	/* nzloc[1:n] */
	int nnza)
/*---------------------------------------------------------------------
c       rows range from firstrow to lastrow
c       the rowstr pointers are defined for nrows = lastrow-firstrow+1 values
c---------------------------------------------------------------------*/
{
	int nrows;
	int i, j, jajp1, nza, k, nzrow;
	double xi;

	/*--------------------------------------------------------------------
	c    how many rows of result
	c-------------------------------------------------------------------*/
	nrows = lastrow - firstrow + 1;

	/*--------------------------------------------------------------------
	c     ...count the number of triples in each row
	c-------------------------------------------------------------------*/
	#pragma omp parallel for     
	for (j = 1; j <= n; j++) {
		rowstr[j] = 0;
		mark[j] = FALSE;
	}
	rowstr[n+1] = 0;

	for (nza = 1; nza <= nnza; nza++) {
		j = (arow[nza] - firstrow + 1) + 1;
		rowstr[j] = rowstr[j] + 1;
	}
	rowstr[1] = 1;
	for (j = 2; j <= nrows+1; j++) {
		rowstr[j] = rowstr[j] + rowstr[j-1];
	}

	/*---------------------------------------------------------------------
	c     ... rowstr(j) now is the location of the first nonzero
	c           of row j of a
	c---------------------------------------------------------------------*/

	/*--------------------------------------------------------------------
	c     ... do a bucket sort of the triples on the row index
	c-------------------------------------------------------------------*/
	for (nza = 1; nza <= nnza; nza++) {
		j = arow[nza] - firstrow + 1;
		k = rowstr[j];
		a[k] = aelt[nza];
		colidx[k] = acol[nza];
		rowstr[j] = rowstr[j] + 1;
	}

	/*--------------------------------------------------------------------
	c       ... rowstr(j) now points to the first element of row j+1
	c-------------------------------------------------------------------*/
	for (j = nrows; j >= 1; j--) {
		rowstr[j+1] = rowstr[j];
	}
	rowstr[1] = 1;

	/*--------------------------------------------------------------------
	c       ... generate the actual output rows by adding elements
	c-------------------------------------------------------------------*/
	nza = 0;
	#pragma omp parallel for    
	for (i = 1; i <= n; i++) {
		x[i] = 0.0;
		mark[i] = FALSE;
	}

	jajp1 = rowstr[1];
	for (j = 1; j <= nrows; j++) {
		nzrow = 0;

		/*--------------------------------------------------------------------
		c          ...loop over the jth row of a
		c-------------------------------------------------------------------*/
		for (k = jajp1; k < rowstr[j+1]; k++) {
			i = colidx[k];
			x[i] = x[i] + a[k];
			if ( mark[i] == FALSE && x[i] != 0.0) {
				mark[i] = TRUE;
				nzrow = nzrow + 1;
				nzloc[nzrow] = i;
			}
		}

		/*--------------------------------------------------------------------
		c          ... extract the nonzeros of this row
		c-------------------------------------------------------------------*/
		for (k = 1; k <= nzrow; k++) {
			i = nzloc[k];
			mark[i] = FALSE;
			xi = x[i];
			x[i] = 0.0;
			if (xi != 0.0) {
				nza = nza + 1;
				a[nza] = xi;
				colidx[nza] = i;
			}
		}
		jajp1 = rowstr[j+1];
		rowstr[j+1] = nza + rowstr[1];
	}
}

/*---------------------------------------------------------------------
c       generate a sparse n-vector (v, iv)
c       having nzv nonzeros
c
c       mark(i) is set to 1 if position i is nonzero.
c       mark is all zero on entry and is reset to all zero before exit
c       this corrects a performance bug found by John G. Lewis, caused by
c       reinitialization of mark on every one of the n calls to sprnvc
---------------------------------------------------------------------*/
static void sprnvc(
	int n,
	int nz,
	double v[],		/* v[1:*] */
	int iv[],		/* iv[1:*] */
	int nzloc[],	/* nzloc[1:n] */
	int mark[] ) 	/* mark[1:n] */
{
	int nn1;
	int nzrow, nzv, ii, i;
	double vecelt, vecloc;

	nzv = 0;
	nzrow = 0;
	nn1 = 1;
	do {
		nn1 = 2 * nn1;
	} while (nn1 < n);

	/*--------------------------------------------------------------------
	c    nn1 is the smallest power of two not less than n
	c-------------------------------------------------------------------*/

	while (nzv < nz) {
		vecelt = randlc(&tran, amult);

		/*--------------------------------------------------------------------
		c   generate an integer between 1 and n in a portable manner
		c-------------------------------------------------------------------*/
		vecloc = randlc(&tran, amult);
		i = icnvrt(vecloc, nn1) + 1;
		if (i > n) continue;

		/*--------------------------------------------------------------------
		c  was this integer generated already?
		c-------------------------------------------------------------------*/
		if (mark[i] == 0) {
			mark[i] = 1;
			nzrow = nzrow + 1;
			nzloc[nzrow] = i;
			nzv = nzv + 1;
			v[nzv] = vecelt;
			iv[nzv] = i;
		}
	}

	for (ii = 1; ii <= nzrow; ii++) {
		i = nzloc[ii];
		mark[i] = 0;
	}
}

/*---------------------------------------------------------------------
* scale a double precision number x in (0,1) by a power of 2 and chop it
*---------------------------------------------------------------------*/
static int icnvrt(double x, int ipwr2) {
	return ((int)(ipwr2 * x));
}

/*--------------------------------------------------------------------
c       set ith element of sparse vector (v, iv) with
c       nzv nonzeros to val
c-------------------------------------------------------------------*/
static void vecset(
	int n,
	double v[],	/* v[1:*] */
	int iv[],	/* iv[1:*] */
	int *nzv,
	int i,
	double val)
{
	int k;
	boolean set;

	set = FALSE;
	for (k = 1; k <= *nzv; k++) {
		if (iv[k] == i) {
			v[k] = val;
			set  = TRUE;
		}
	}
	if (set == FALSE) {
		*nzv = *nzv + 1;
		v[*nzv] = val;
		iv[*nzv] = i;
	}
}
