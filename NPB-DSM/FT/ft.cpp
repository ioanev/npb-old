/*--------------------------------------------------------------------

    Information on NAS Parallel Benchmarks is available at:

    http://www.nas.nasa.gov/Software/NPB/

    Authors: D. Bailey
         W. Saphir

    CPP and OpenMP version:
            Dalvan Griebler <dalvangriebler@gmail.com>
            Júnior Löff <loffjh@gmail.com>

--------------------------------------------------------------------*/

/* argodsm header   */
#include "argo.hpp"

#include <iostream>
#include "npb-CPP.hpp"

/* global variables */
#include "global.hpp"

/* function declarations */
static void evolve(dcomplex *u0, dcomplex *u1, int t, int *indexmap, int d[3]);
static void compute_initial_conditions(dcomplex *u0, int d[3]);
static void ipow46(double a, int exponent, double *result);
static void setup(void);
static void compute_indexmap(int *indexmap, int d[3]);
static void print_timers(void);
static void fft(int dir, dcomplex *x1, dcomplex *x2);
static void cffts1(int is, int d[3], dcomplex *x, dcomplex *xout, dcomplex y0[NX][FFTBLOCKPAD], dcomplex y1[NX][FFTBLOCKPAD]);
static void cffts2(int is, int d[3], dcomplex *x, dcomplex *xout, dcomplex y0[NX][FFTBLOCKPAD], dcomplex y1[NX][FFTBLOCKPAD]);
static void cffts3(int is, int d[3], dcomplex *x, dcomplex *xout, dcomplex y0[NX][FFTBLOCKPAD], dcomplex y1[NX][FFTBLOCKPAD]);
static void fft_init (int n);
static void cfftz (int is, int m, int n, dcomplex x[NX][FFTBLOCKPAD], dcomplex y[NX][FFTBLOCKPAD]);
static void fftz2 (int is, int l, int m, int n, int ny, int ny1, dcomplex u[NX], dcomplex x[NX][FFTBLOCKPAD], dcomplex y[NX][FFTBLOCKPAD]);
static int ilog2(int n);
static void checksum(int i, dcomplex *u1, int d[3]);
static void verify (int d1, int d2, int d3, int nt, boolean *verified, char *class_npb);

#define at(x, y, z) (((x) * NY * NX) + ((y) * NX) + (z))

/*--------------------------------------------------------------------
c FT benchmark
c-------------------------------------------------------------------*/

int main(int argc, char **argv) {

    /*c-------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    argo::init(10*1024*1024*1024UL);

    /*------------------------------------------------------------------
    c u0, u1, u2 are the main arrays in the problem. 
    c Depending on the decomposition, these arrays will have different 
    c dimensions. To accomodate all possibilities, we allocate them as 
    c one-dimensional arrays and pass them to subroutines for different 
    c views
    c  - u0 contains the initial (transformed) initial condition
    c  - u1 and u2 are working arrays
    c  - indexmap maps i,j,k of u0 to the correct i^2+j^2+k^2 for the
    c    time evolution operator. 
    c-----------------------------------------------------------------*/

    /*--------------------------------------------------------------------
    c Large arrays are in common so that they are allocated on the
    c heap rather than the stack. This common block is not
    c referenced directly anywhere else. Padding is to avoid accidental 
    c cache problems, since all array sizes are powers of two.
    c-------------------------------------------------------------------*/

    int i;
    int iter;
    char class_npb;
    boolean verified;
    double total_time, mflops;

    #pragma omp parallel
	{
		#if defined(_OPENMP)
		#pragma omp master
			nthreads = omp_get_num_threads();
		#endif /* _OPENMP */
	}

    workrank = argo::node_id();
    numtasks = argo::number_of_nodes();

    isum = argo::conew_array<dcomplex>(numtasks);
    sums = argo::conew_array<dcomplex>(NITER_DEFAULT+1);
    int *indexmap = argo::conew_array<int>(NZ * NY * NX);
    dcomplex *u0 = argo::conew_array<dcomplex>(NZ * NY * NX);
    dcomplex *u1 = argo::conew_array<dcomplex>(NZ * NY * NX);
    dcomplex *u2 = argo::conew_array<dcomplex>(NZ * NY * NX);

    /*--------------------------------------------------------------------
    c Run the entire problem once to make sure all data is touched. 
    c This reduces variable startup costs, which is important for such a 
    c short benchmark. The other NPB 2 implementations are similar. 
    c-------------------------------------------------------------------*/

    for (i = 0; i < T_MAX; i++) {
        timer_clear(i);
    }
    setup();

    compute_indexmap(indexmap, dims[2]);
    compute_initial_conditions(u1, dims[0]);
    fft_init (dims[0][0]);
    
    #pragma omp parallel
    {
        fft(1, u1, u0);
    }

    /*--------------------------------------------------------------------
    c Start over from the beginning. Note that all operations must
    c be timed, in contrast to other benchmarks. 
    c-------------------------------------------------------------------*/
    
    for (i = 0; i < T_MAX; i++) {
        timer_clear(i);
    }

    timer_start(T_TOTAL);
    if (TIMERS_ENABLED == TRUE) timer_start(T_SETUP);

    compute_indexmap(indexmap, dims[2]);
    compute_initial_conditions(u1, dims[0]);
    fft_init (dims[0][0]);

    #pragma omp parallel private(iter) firstprivate(niter)
    {
        if (TIMERS_ENABLED == TRUE) {
            #pragma omp master
            timer_stop(T_SETUP);
        }
        if (TIMERS_ENABLED == TRUE) {
            #pragma omp master   
            timer_start(T_FFT);
        }

        fft(1, u1, u0);
        argo::barrier(nthreads);

        if (TIMERS_ENABLED == TRUE) {
            #pragma omp master      
            timer_stop(T_FFT);
        }

        for (iter = 1; iter <= niter; iter++) {
            if (TIMERS_ENABLED == TRUE) {
                #pragma omp master      
                timer_start(T_EVOLVE);
            }

            evolve(u0, u1, iter, indexmap, dims[0]);
            argo::barrier(nthreads);
            
            if (TIMERS_ENABLED == TRUE) {
                #pragma omp master      
                timer_stop(T_EVOLVE);
            }
            if (TIMERS_ENABLED == TRUE) {
                #pragma omp master      
                timer_start(T_FFT);
            }

            fft(-1, u1, u2);
            argo::barrier(nthreads);

            if (TIMERS_ENABLED == TRUE) {
                #pragma omp master      
                timer_stop(T_FFT);
            }
            if (TIMERS_ENABLED == TRUE) {
                #pragma omp master      
                timer_start(T_CHECKSUM);
            }

            checksum(iter, u2, dims[0]);

            if (TIMERS_ENABLED == TRUE) {
                #pragma omp master      
                timer_stop(T_CHECKSUM);
            }
        }
    }
    argo::barrier();

    if (workrank == 0)
    {
        verify(NX, NY, NZ, niter, &verified, &class_npb);

        timer_stop(T_TOTAL);
        total_time = timer_read(T_TOTAL);

        if( total_time != 0.0) {
            mflops = 1.0e-6*(double)(NTOTAL) *
            (14.8157+7.19641*log((double)(NTOTAL))
            +  (5.23518+7.21113*log((double)(NTOTAL)))*niter)/total_time;
        } else {
            mflops = 0.0;
        }
        c_print_results((char*)"FT", class_npb, NX, NY, NZ, niter, numtasks*nthreads, total_time, mflops, (char*)"          floating point", verified, 
        (char*)NPBVERSION, (char*)COMPILETIME, (char*)CS1, (char*)CS2, (char*)CS3, (char*)CS4, (char*)CS5, (char*)CS6, (char*)CS7);
        if (TIMERS_ENABLED == TRUE) print_timers();
    }

    argo::codelete_array(isum);
    argo::codelete_array(sums);
    argo::codelete_array(indexmap);
    argo::codelete_array(u0);
    argo::codelete_array(u1);
    argo::codelete_array(u2);

    argo::finalize();

    return 0;
}

/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void evolve(dcomplex *u0, dcomplex *u1, int t, int *indexmap, int d[3]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*--------------------------------------------------------------------
    c evolve u0 -> u1 (t time steps) in fourier space
    c-------------------------------------------------------------------*/

    int i, j, k;

    static int chunk = d[2] / numtasks;
    static int beg = workrank * chunk;
    static int end = (workrank != numtasks - 1) ? beg + chunk : d[2];
    
    #pragma omp for
    for (k = beg; k < end; k++) {
        for (j = 0; j < d[1]; j++) {
            for (i = 0; i < d[0]; i++) {
                crmul(u1[at(k, j, i)], u0[at(k, j, i)], ex[t*indexmap[at(k, j, i)]]);
            }
        }
    }
}

/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void compute_initial_conditions(dcomplex *u0, int d[3]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*--------------------------------------------------------------------
    c Fill in array u0 with initial conditions from 
    c random number generator 
    c-------------------------------------------------------------------*/

    int k;
    /*double x0, start, an, dummy;*/
    double x0, start, an;
    //double tmp[NX*2*MAXDIM+1];
    int i,j,t;
    double starts[NZ];

    start = SEED;
    
    /*--------------------------------------------------------------------
    c Jump to the starting element for our first plane.
    c-------------------------------------------------------------------*/

    ipow46(A, (zstart[0]-1)*2*NX*NY + (ystart[0]-1)*2*NX, &an);
    /*dummy = */randlc(&start, an);
    ipow46(A, 2*NX*NY, &an);

    starts[0] = start;
    for(int i=1; i<dims[0][2]; i++){
        randlc(&start, an);
        starts[i] = start;
    }

    /*--------------------------------------------------------------------
    c Go through by z planes filling in one square at a time.
    c-------------------------------------------------------------------*/

    static int chunk = dims[0][2] / numtasks;
    static int beg = workrank * chunk;
    static int end = (workrank != numtasks - 1) ? beg + chunk : dims[0][2];

    #pragma omp parallel private(k, i, j, t, x0)
    {
        double * tmp = new double[NX*2*MAXDIM+1];

        #pragma omp for
        for (k = beg; k < end; k++) {
            x0 = starts[k];
            vranlc(2*NX*dims[0][1], &x0, A, tmp);

            t = 1;
            for (j = 0; j < dims[0][1]; j++)
                for (i = 0; i < NX; i++) {
                    u0[at(k, j, i)].real = tmp[t++];
                    u0[at(k, j, i)].imag = tmp[t++];
                }

            //if (k != dims[0][2]) /*dummy = */randlc(&start, an);
        }

        delete[] tmp;
    }
}

/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void ipow46(double a, int exponent, double *result) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*--------------------------------------------------------------------
    c compute a^exponent mod 2^46
    c-------------------------------------------------------------------*/

    /*double dummy, q, r;*/
    double q, r;
    int n, n2;

    /*--------------------------------------------------------------------
    c Use
    c   a^n = a^(n/2)*a^(n/2) if n even else
    c   a^n = a*a^(n-1)       if n odd
    c-------------------------------------------------------------------*/
    *result = 1;
    if (exponent == 0) return;
    q = a;
    r = 1;
    n = exponent;

    while (n > 1) {
        n2 = n/2;
        if (n2 * 2 == n) {
            /*dummy = */randlc(&q, q);
            n = n2;
        } else {
            /*dummy = */randlc(&r, q);
            n = n-1;
        }
    }
    /*dummy = */randlc(&r, q);
    *result = r;
}

/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void setup(void) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*int ierr, i, j, fstatus;*/
    int i;

    if (workrank == 0) {
        printf("\n\n NAS Parallel Benchmarks 4.0 OpenMP C++ version" " - FT Benchmark\n\n");
        printf("\n\n Developed by: Dalvan Griebler <dalvan.griebler@acad.pucrs.br>\n");
    }

    niter = NITER_DEFAULT;

    if (workrank == 0) {
        printf(" Size                : %3lux%3lux%3lu\n", NX, NY, NZ);
        printf(" Iterations          :     %7d\n", niter);
    }

    /* 1004 format(' Number of processes :     ', i7)
    1005 format(' Processor array     :     ', i3, 'x', i3)
    1006 format(' WARNING: compiled for ', i5, ' processes. ',
    >       ' Will not verify. ')*/

    for (i = 0;i < 3 ; i++) {
        dims[i][0] = NX;
        dims[i][1] = NY;
        dims[i][2] = NZ;
    }


    for (i = 0; i < 3; i++) {
        xstart[i] = 1;
        xend[i]   = NX;
        ystart[i] = 1;
        yend[i]   = NY;
        zstart[i] = 1;
        zend[i]   = NZ;
    }

    /*--------------------------------------------------------------------
    c Set up info for blocking of ffts and transposes.  This improves
    c performance on cache-based systems. Blocking involves
    c working on a chunk of the problem at a time, taking chunks
    c along the first, second, or third dimension. 
    c
    c - In cffts1 blocking is on 2nd dimension (with fft on 1st dim)
    c - In cffts2/3 blocking is on 1st dimension (with fft on 2nd and 3rd dims)

    c Since 1st dim is always in processor, we'll assume it's long enough 
    c (default blocking factor is 16 so min size for 1st dim is 16)
    c The only case we have to worry about is cffts1 in a 2d decomposition. 
    c so the blocking factor should not be larger than the 2nd dimension. 
    c-------------------------------------------------------------------*/

    fftblock = FFTBLOCK_DEFAULT;
    fftblockpad = FFTBLOCKPAD_DEFAULT;

    if (fftblock != FFTBLOCK_DEFAULT) fftblockpad = fftblock+3;
}
      
/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void compute_indexmap(int *indexmap, int d[3]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*--------------------------------------------------------------------
    c compute function from local (i,j,k) to ibar^2+jbar^2+kbar^2 
    c for time evolution exponent. 
    c-------------------------------------------------------------------*/

    int i, j, k, ii, ii2, jj, ij2, kk;
    double ap;

    /*--------------------------------------------------------------------
    c basically we want to convert the fortran indices 
    c   1 2 3 4 5 6 7 8 
    c to 
    c   0 1 2 3 -4 -3 -2 -1
    c The following magic formula does the trick:
    c mod(i-1+n/2, n) - n/2
    c-------------------------------------------------------------------*/

    static int chunk = dims[2][0] / numtasks;
    static int beg = workrank * chunk;
    static int end = (workrank != numtasks - 1) ? beg + chunk : dims[2][0];

    #pragma omp parallel for private(i, j, k, ii, ii2, jj, ij2, kk)
    for (i = beg; i < end; i++) {
        ii =  (i+1+xstart[2]-2+NX/2)%NX - NX/2;
        ii2 = ii*ii;
        for (j = 0; j < dims[2][1]; j++) {
            jj = (j+1+ystart[2]-2+NY/2)%NY - NY/2;
            ij2 = jj*jj+ii2;
            for (k = 0; k < dims[2][2]; k++) {
                kk = (k+1+zstart[2]-2+NZ/2)%NZ - NZ/2;
                indexmap[at(k, j, i)] = kk*kk+ij2;
            }
        }
    }

    /*--------------------------------------------------------------------
    c compute array of exponentials for time evolution. 
    c-------------------------------------------------------------------*/

    ap = - 4.0 * ALPHA * PI * PI;

    ex[0] = 1.0;
    ex[1] = exp(ap);
    for (i = 2; i <= EXPMAX; i++) {
        ex[i] = ex[i-1]*ex[1];
    }
}

/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void print_timers(void) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    int i;
    const char *tstrings[] = { "          total ",
    "          setup ", 
    "            fft ", 
    "         evolve ", 
    "       checksum ", 
    "         fftlow ", 
    "        fftcopy " };

    for (i = 0; i < T_MAX; i++) {
        if (timer_read(i) != 0.0) {
            printf("timer %2d(%16s( :%10.6f\n", i, tstrings[i], timer_read(i));
        }
    }
}

/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void fft(int dir, dcomplex *x1, dcomplex *x2) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    dcomplex y0[NX][FFTBLOCKPAD];
    dcomplex y1[NX][FFTBLOCKPAD];

    /*--------------------------------------------------------------------
    c note: args x1, x2 must be different arrays
    c note: args for cfftsx are (direction, layout, xin, xout, scratch)
    c       xin/xout may be the same and it can be somewhat faster
    c       if they are
    c-------------------------------------------------------------------*/

    if (dir == 1) {
        cffts1(1, dims[0], x1, x1, y0, y1);	 /* x1 -> x1 */
        cffts2(1, dims[1], x1, x1, y0, y1);	 /* x1 -> x1 */
        argo::barrier(nthreads);
        cffts3(1, dims[2], x1, x2, y0, y1);	 /* x1 -> x2 */
    } else {
        cffts3(-1, dims[2], x1, x1, y0, y1); /* x1 -> x1 */
        argo::barrier(nthreads);
        cffts2(-1, dims[1], x1, x1, y0, y1); /* x1 -> x1 */
        cffts1(-1, dims[0], x1, x2, y0, y1); /* x1 -> x2 */
    }
}


/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void cffts1(int is, int d[3], dcomplex *x, dcomplex *xout,dcomplex y0[NX][FFTBLOCKPAD],dcomplex y1[NX][FFTBLOCKPAD]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    int logd[3];
    int i, j, k, jj;

    for (i = 0; i < 3; i++) {
        logd[i] = ilog2(d[i]);
    }

    static int chunk = d[2] / numtasks;
    static int beg = workrank * chunk;
    static int end = (workrank != numtasks - 1) ? beg + chunk : d[2];

    #pragma omp for
    for (k = beg; k < end; k++) {
        for (jj = 0; jj <= d[1] - fftblock; jj+=fftblock) {
                      if (TIMERS_ENABLED == TRUE) timer_start(T_FFTCOPY); 
            for (j = 0; j < fftblock; j++) {
                for (i = 0; i < d[0]; i++) {
                    y0[i][j].real = x[at(k, j+jj, i)].real;
                    y0[i][j].imag = x[at(k, j+jj, i)].imag;
                }
            }
                      if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTCOPY); 

                      if (TIMERS_ENABLED == TRUE) timer_start(T_FFTLOW); 
            cfftz (is, logd[0],
            d[0], y0, y1);

                      if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTLOW); 
                      if (TIMERS_ENABLED == TRUE) timer_start(T_FFTCOPY); 
            for (j = 0; j < fftblock; j++) {
                for (i = 0; i < d[0]; i++) {
                    xout[at(k, j+jj, i)].real = y0[i][j].real;
                    xout[at(k, j+jj, i)].imag = y0[i][j].imag;
                }
            }
                      if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTCOPY); 
        }
    }
}


/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void cffts2(int is, int d[3], dcomplex *x,dcomplex *xout,dcomplex y0[NX][FFTBLOCKPAD],dcomplex y1[NX][FFTBLOCKPAD]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    int logd[3];
    int i, j, k, ii;

    for (i = 0; i < 3; i++) {
        logd[i] = ilog2(d[i]);
    }

    static int chunk = d[2] / numtasks;
    static int beg = workrank * chunk;
    static int end = (workrank != numtasks - 1) ? beg + chunk : d[2];

    #pragma omp for
    for (k = beg; k < end; k++) {
        for (ii = 0; ii <= d[0] - fftblock; ii+=fftblock) {
            	    if (TIMERS_ENABLED == TRUE) timer_start(T_FFTCOPY); 
            for (j = 0; j < d[1]; j++) {
                for (i = 0; i < fftblock; i++) {
                    y0[j][i].real = x[at(k, j, i+ii)].real;
                    y0[j][i].imag = x[at(k, j, i+ii)].imag;
                }
            }
            	    if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTCOPY); 
            	    if (TIMERS_ENABLED == TRUE) timer_start(T_FFTLOW); 
            cfftz (is, logd[1], 
            d[1], y0, y1);

                      if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTLOW); 
                      if (TIMERS_ENABLED == TRUE) timer_start(T_FFTCOPY); 
            for (j = 0; j < d[1]; j++) {
                for (i = 0; i < fftblock; i++) {
                xout[at(k, j, i+ii)].real = y0[j][i].real;
                xout[at(k, j, i+ii)].imag = y0[j][i].imag;
                }
            }
                   if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTCOPY); 
        }
    }
}

/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void cffts3(int is, int d[3], dcomplex *x,dcomplex *xout,dcomplex y0[NX][FFTBLOCKPAD], dcomplex y1[NX][FFTBLOCKPAD]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    int logd[3];
    int i, j, k, ii;

    for (i = 0;i < 3; i++) {
        logd[i] = ilog2(d[i]);
    }

    static int chunk = d[1] / numtasks;
    static int beg = workrank * chunk;
    static int end = (workrank != numtasks - 1) ? beg + chunk : d[1];

    #pragma omp for
    for (j = beg; j < end; j++) {
        for (ii = 0; ii <= d[0] - fftblock; ii+=fftblock) {
        	    if (TIMERS_ENABLED == TRUE) timer_start(T_FFTCOPY); 
            for (k = 0; k < d[2]; k++) {
                for (i = 0; i < fftblock; i++) {
                    y0[k][i].real = x[at(k, j, i+ii)].real;
                    y0[k][i].imag = x[at(k, j, i+ii)].imag;
                }
            }

                       if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTCOPY); 
                       if (TIMERS_ENABLED == TRUE) timer_start(T_FFTLOW); 
            cfftz (is, logd[2],
            d[2], y0, y1);
                       if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTLOW); 
                       if (TIMERS_ENABLED == TRUE) timer_start(T_FFTCOPY); 
            for (k = 0; k < d[2]; k++) {
                for (i = 0; i < fftblock; i++) {
                    xout[at(k, j, i+ii)].real = y0[k][i].real;
                    xout[at(k, j, i+ii)].imag = y0[k][i].imag;
                }
            }
                       if (TIMERS_ENABLED == TRUE) timer_stop(T_FFTCOPY); 
        }
    }
}


/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void fft_init (int n) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*--------------------------------------------------------------------
    c compute the roots-of-unity array that will be used for subsequent FFTs. 
    c-------------------------------------------------------------------*/

    /*int m,nu,ku,i,j,ln;*/
    int m,ku,i,j,ln;
    double t, ti;


    /*--------------------------------------------------------------------
    c   Initialize the U array with sines and cosines in a manner that permits
    c   stride one access at each FFT iteration.
    c-------------------------------------------------------------------*/
    /*nu = n;*/
    m = ilog2(n);
    u[0].real = (double)m;
    u[0].imag = 0.0;
    ku = 1;
    ln = 1;

    for (j = 1; j <= m; j++) {
        t = PI / ln;

        for (i = 0; i <= ln - 1; i++) {
            ti = i * t;
            u[i+ku].real = cos(ti);
            u[i+ku].imag = sin(ti);
        }

        ku = ku + ln;
        ln = 2 * ln;
    }
}


/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void cfftz (int is, int m, int n, dcomplex x[NX][FFTBLOCKPAD], dcomplex y[NX][FFTBLOCKPAD]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*--------------------------------------------------------------------
    c   Computes NY N-point complex-to-complex FFTs of X using an algorithm due
    c   to Swarztrauber.  X is both the input and the output array, while Y is a 
    c   scratch array.  It is assumed that N = 2^M.  Before calling CFFTZ to 
    c   perform FFTs, the array U must be initialized by calling CFFTZ with IS 
    c   set to 0 and M set to MX, where MX is the maximum value of M for any 
    c   subsequent call.
    c-------------------------------------------------------------------*/

    int i,j,l,mx;

    /*--------------------------------------------------------------------
    c   Check if input parameters are invalid.
    c-------------------------------------------------------------------*/
    mx = (int)(u[0].real);
    if ((is != 1 && is != -1) || m < 1 || m > mx) {
        printf("CFFTZ: Either U has not been initialized, or else\n"
        "one of the input parameters is invalid%5d%5d%5d\n",
        is, m, mx);
        exit(1);
    }

    /*--------------------------------------------------------------------
    c   Perform one variant of the Stockham FFT.
    c-------------------------------------------------------------------*/
    for (l = 1; l <= m; l+=2) {
        fftz2 (is, l, m, n, fftblock, fftblockpad, u, x, y);
        if (l == m) break;
        fftz2 (is, l + 1, m, n, fftblock, fftblockpad, u, y, x);
    }

    /*--------------------------------------------------------------------
    c   Copy Y to X.
    c-------------------------------------------------------------------*/
    if (m % 2 == 1) {
        for (j = 0; j < n; j++) {
            for (i = 0; i < fftblock; i++) {
                x[j][i].real = y[j][i].real;
                x[j][i].imag = y[j][i].imag;
            }
        }
    }
}


/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void fftz2 (int is, int l, int m, int n, int ny, int ny1,dcomplex u[NX], dcomplex x[NX][FFTBLOCKPAD], dcomplex y[NX][FFTBLOCKPAD]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*--------------------------------------------------------------------
    c   Performs the L-th iteration of the second variant of the Stockham FFT.
    c-------------------------------------------------------------------*/

    int k,n1,li,lj,lk,ku,i,j,i11,i12,i21,i22;
    /*dcomplex u1,x11,x21;*/
    dcomplex u1;

    /*--------------------------------------------------------------------
    c   Set initial parameters.
    c-------------------------------------------------------------------*/

    n1 = n / 2;
    if (l-1 == 0) {
        lk = 1;
    } else {
        lk = 2 << ((l - 1)-1);
    }
    if (m-l == 0) {
        li = 1;
    } else {
        li = 2 << ((m - l)-1);
    }
    lj = 2 * lk;
    ku = li;

    for (i = 0; i < li; i++) {

        i11 = i * lk;
        i12 = i11 + n1;
        i21 = i * lj;
        i22 = i21 + lk;
        if (is >= 1) {
            u1.real = u[ku+i].real;
            u1.imag = u[ku+i].imag;
        } else {
            u1.real = u[ku+i].real;
            u1.imag = -u[ku+i].imag;
        }

        /*--------------------------------------------------------------------
        c   This loop is vectorizable.
        c-------------------------------------------------------------------*/
        for (k = 0; k < lk; k++) {
            for (j = 0; j < ny; j++) {
                double x11real, x11imag;
                double x21real, x21imag;
                x11real = x[i11+k][j].real;
                x11imag = x[i11+k][j].imag;
                x21real = x[i12+k][j].real;
                x21imag = x[i12+k][j].imag;
                y[i21+k][j].real = x11real + x21real;
                y[i21+k][j].imag = x11imag + x21imag;
                y[i22+k][j].real = u1.real * (x11real - x21real)
                - u1.imag * (x11imag - x21imag);
                y[i22+k][j].imag = u1.real * (x11imag - x21imag)
                + u1.imag * (x11real - x21real);
            }
        }
    }
}


/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static int ilog2(int n) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    int nn, lg;

    if (n == 1) {
        return 0;
    }
    lg = 1;
    nn = 2;
    while (nn < n) {
        nn = nn << 1;
        lg++;
    }

    return lg;
}


/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void checksum(int i, dcomplex *u1, int d[3]) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*int j, q,r,s, ierr;*/
    int j, q,r,s;
    /*dcomplex chk,allchk;*/
    dcomplex chk;

    chk.real = 0.0;
    chk.imag = 0.0;

    #pragma omp single
    {
        isum[workrank].real = 0.0;
        isum[workrank].imag = 0.0;
    }
    
    static int chunk = 1024 / numtasks;
    static int beg = 1 + workrank * chunk;
    static int end = (workrank != numtasks - 1) ? workrank * chunk + chunk : 1024;

    #pragma omp for nowait
    for (j = beg; j <= end; j++) {
        q = j%NX+1;
        if (q >= xstart[0] && q <= xend[0]) {
            r = (3*j)%NY+1;
            if (r >= ystart[0] && r <= yend[0]) {
                s = (5*j)%NZ+1;
                if (s >= zstart[0] && s <= zend[0]) {
                    cadd(chk,chk,u1[at(s-zstart[0], r-ystart[0], q-xstart[0])]);
                }
            }
        }
    }
    #pragma omp critical
    {
        isum[workrank].real += chk.real;
        isum[workrank].imag += chk.imag;
    }
    argo::barrier(nthreads);

    #pragma omp master
    {
        if (workrank == 0) {
            for (j = 0; j < numtasks; j++) {
                sums[i].real += isum[j].real;
                sums[i].imag += isum[j].imag;
            }

            /* complex % real */
            sums[i].real = sums[i].real/(double)(NTOTAL);
            sums[i].imag = sums[i].imag/(double)(NTOTAL);

            printf("T = %5d     Checksum = %22.12e %22.12e\n",
            i, sums[i].real, sums[i].imag);
        }
    }
}


/*--------------------------------------------------------------------
c-------------------------------------------------------------------*/

static void verify (int d1, int d2, int d3, int nt, boolean *verified, char *class_npb) {

    /*--------------------------------------------------------------------
    c-------------------------------------------------------------------*/

    /*int ierr, size, i;*/
    int i;
    double err, epsilon;

    /*--------------------------------------------------------------------
    c   Sample size reference checksums
    c-------------------------------------------------------------------*/
        
    /*--------------------------------------------------------------------
    c   class_npb S size reference checksums
    c-------------------------------------------------------------------*/
    double vdata_real_s[6+1] = { 0.0,
				 5.546087004964e+02,
				 5.546385409189e+02,
				 5.546148406171e+02,
				 5.545423607415e+02,
				 5.544255039624e+02,
				 5.542683411902e+02 };
    double vdata_imag_s[6+1] = { 0.0,
				 4.845363331978e+02,
				 4.865304269511e+02,
				 4.883910722336e+02,
				 4.901273169046e+02,
				 4.917475857993e+02,
				 4.932597244941e+02 };
    /*--------------------------------------------------------------------
    c   class_npb W size reference checksums
    c-------------------------------------------------------------------*/
    double vdata_real_w[6+1] = { 0.0,
				 5.673612178944e+02,
				 5.631436885271e+02,
				 5.594024089970e+02,
				 5.560698047020e+02,
				 5.530898991250e+02,
				 5.504159734538e+02 };
    double vdata_imag_w[6+1] = { 0.0,
				 5.293246849175e+02,
				 5.282149986629e+02,
				 5.270996558037e+02, 
				 5.260027904925e+02, 
				 5.249400845633e+02,
				 5.239212247086e+02 };
    /*--------------------------------------------------------------------
    c   class_npb A size reference checksums
    c-------------------------------------------------------------------*/
    double vdata_real_a[6+1] = { 0.0,
				 5.046735008193e+02,
				 5.059412319734e+02,
				 5.069376896287e+02,
				 5.077892868474e+02,
				 5.085233095391e+02,
				 5.091487099959e+02 };
    double vdata_imag_a[6+1] = { 0.0,
				 5.114047905510e+02,
				 5.098809666433e+02,
				 5.098144042213e+02,
				 5.101336130759e+02,
				 5.104914655194e+02,
				 5.107917842803e+02 };
    /*--------------------------------------------------------------------
    c   class_npb B size reference checksums
    c-------------------------------------------------------------------*/
    double vdata_real_b[20+1] = { 0.0,
				  5.177643571579e+02,
				  5.154521291263e+02,
				  5.146409228649e+02,
				  5.142378756213e+02,
				  5.139626667737e+02,
				  5.137423460082e+02,
				  5.135547056878e+02,
				  5.133910925466e+02,
				  5.132470705390e+02,
				  5.131197729984e+02,
				  5.130070319283e+02,
				  5.129070537032e+02,
				  5.128182883502e+02,
				  5.127393733383e+02,
				  5.126691062020e+02,
				  5.126064276004e+02,
				  5.125504076570e+02,
				  5.125002331720e+02,
				  5.124551951846e+02,
				  5.124146770029e+02 };
    double vdata_imag_b[20+1] = { 0.0,
				  5.077803458597e+02,
				  5.088249431599e+02,                  
				  5.096208912659e+02,
				  5.101023387619e+02,                  
				  5.103976610617e+02,                  
				  5.105948019802e+02,                  
				  5.107404165783e+02,                  
				  5.108576573661e+02,                  
				  5.109577278523e+02,
				  5.110460304483e+02,                  
				  5.111252433800e+02,                  
				  5.111968077718e+02,                  
				  5.112616233064e+02,                  
				  5.113203605551e+02,                  
				  5.113735928093e+02,                  
				  5.114218460548e+02,
				  5.114656139760e+02,
				  5.115053595966e+02,
				  5.115415130407e+02,
				  5.115744692211e+02 };
    /*--------------------------------------------------------------------
    c   class_npb C size reference checksums
    c-------------------------------------------------------------------*/
    double vdata_real_c[20+1] = { 0.0,
				  5.195078707457e+02,
				  5.155422171134e+02,
				  5.144678022222e+02,
				  5.140150594328e+02,
				  5.137550426810e+02,
				  5.135811056728e+02,
				  5.134569343165e+02,
				  5.133651975661e+02,
				  5.132955192805e+02,
				  5.132410471738e+02,
				  5.131971141679e+02,
				  5.131605205716e+02,
				  5.131290734194e+02,
				  5.131012720314e+02,
				  5.130760908195e+02,
				  5.130528295923e+02,
				  5.130310107773e+02,
				  5.130103090133e+02,
				  5.129905029333e+02,
				  5.129714421109e+02 };
    double vdata_imag_c[20+1] = { 0.0,
				  5.149019699238e+02,
				  5.127578201997e+02,
				  5.122251847514e+02,
				  5.121090289018e+02,
				  5.121143685824e+02,
				  5.121496764568e+02,
				  5.121870921893e+02,
				  5.122193250322e+02,
				  5.122454735794e+02,
				  5.122663649603e+02,
				  5.122830879827e+02,
				  5.122965869718e+02,
				  5.123075927445e+02,
				  5.123166486553e+02,
				  5.123241541685e+02,
				  5.123304037599e+02,
				  5.123356167976e+02,
				  5.123399592211e+02,
				  5.123435588985e+02,
				  5.123465164008e+02 };
    /*--------------------------------------------------------------------
    c   class_npb D size reference checksums
    c-------------------------------------------------------------------*/
    double vdata_real_d[25+1] = { 0.0,
				  5.122230065252e+02,
				  5.120463975765e+02,
				  5.119865766760e+02,
				  5.119518799488e+02,
				  5.119269088223e+02,
				  5.119082416858e+02,
				  5.118943814638e+02,
				  5.118842385057e+02,
				  5.118769435632e+02,
				  5.118718203448e+02,
				  5.118683569061e+02,
				  5.118661708593e+02,
				  5.118649768950e+02,
				  5.118645605626e+02,
				  5.118647586618e+02,
				  5.118654451572e+02,
				  5.118665212451e+02,
				  5.118679083821e+02,
				  5.118695433664e+02,
				  5.118713748264e+02,
                  5.118733606701e+02,
                  5.118754661974e+02,
                  5.118776626738e+02,
                  5.118799262314e+02,
                  5.118822370068e+02 };
    double vdata_imag_d[25+1] = { 0.0,
				  5.118534037109e+02,
                  5.117061181082e+02,
                  5.117096364601e+02,
                  5.117373863950e+02,
                  5.117680347632e+02,
                  5.117967875532e+02,
                  5.118225281841e+02,
                  5.118451629348e+02,
                  5.118649119387e+02,
                  5.118820803844e+02,
                  5.118969781011e+02,
                  5.119098918835e+02,
                  5.119210777066e+02,
                  5.119307604484e+02,
                  5.119391362671e+02,
                  5.119463757241e+02,
                  5.119526269238e+02,
                  5.119580184108e+02,
                  5.119626617538e+02,
                  5.119666538138e+02,
                  5.119700787219e+02,
                  5.119730095953e+02,
                  5.119755100241e+02,
                  5.119776353561e+02,
                  5.119794338060e+02 };
    /*--------------------------------------------------------------------
    c   class_npb E size reference checksums
    c-------------------------------------------------------------------*/
    double vdata_real_e[25+1] = { 0.0,
				  5.121601045346e+02,
				  5.120905403678e+02,
				  5.120623229306e+02,
				  5.120438418997e+02,
				  5.120311521872e+02,
				  5.120226088809e+02,
				  5.120169296534e+02,
				  5.120131225172e+02,
				  5.120104767108e+02,
				  5.120085127969e+02,
				  5.120069224127e+02,
				  5.120055158164e+02,
				  5.120041820159e+02,
				  5.120028605402e+02,
				  5.120015223011e+02,
				  5.120001570022e+02,
				  5.119987650555e+02,
				  5.119973525091e+02,
				  5.119959279472e+02,
				  5.119945006558e+02,
                  5.119930795911e+02,
                  5.119916728462e+02,
                  5.119902874185e+02,
                  5.119889291565e+02,
                  5.119876028049e+02 };
    double vdata_imag_e[25+1] = { 0.0,
				  5.117395998266e+02,
                  5.118614716182e+02,
                  5.119074203747e+02,
                  5.119345900733e+02,
                  5.119551325550e+02,
                  5.119720179919e+02,
                  5.119861371665e+02,
                  5.119979364402e+02,
                  5.120077674092e+02,
                  5.120159443121e+02,
                  5.120227453670e+02,
                  5.120284096041e+02,
                  5.120331373793e+02,
                  5.120370938679e+02,
                  5.120404138831e+02,
                  5.120432068837e+02,
                  5.120455615860e+02,
                  5.120475499442e+02,
                  5.120492304629e+02,
                  5.120506508902e+02,
                  5.120518503782e+02,
                  5.120528612016e+02,
                  5.120537101195e+02,
                  5.120544194514e+02,
                  5.120550079284e+02 };
    /*--------------------------------------------------------------------
    c   class_npb F size reference checksums
    c-------------------------------------------------------------------*/
    double vdata_real_f[25+1] = { 0.0,
				  5.119892866928e+02,
				  5.119560157487e+02,
				  5.119437960123e+02,
				  5.119395628845e+02,
				  5.119390371879e+02,
				  5.119405091840e+02,
				  5.119430444528e+02,
				  5.119460702242e+02,
				  5.119492377036e+02,
				  5.119523446268e+02,
				  5.119552825361e+02,
				  5.119580008777e+02,
				  5.119604834177e+02,
				  5.119627332821e+02,
				  5.119647637538e+02,
				  5.119665927740e+02,
				  5.119682397643e+02,
				  5.119697238718e+02,
				  5.119710630664e+02,
				  5.119722737384e+02,
                  5.119733705802e+02,
                  5.119743666226e+02,
                  5.119752733481e+02,
                  5.119761008382e+02,
                  5.119768579280e+02 };
    double vdata_imag_f[25+1] = { 0.0,
				  5.121457822747e+02,
                  5.121009044434e+02,
                  5.120761074285e+02,
                  5.120614320496e+02,
                  5.120514085624e+02,
                  5.120438117102e+02,
                  5.120376348915e+02,
                  5.120323831062e+02,
                  5.120277980818e+02,
                  5.120237368268e+02,
                  5.120201137845e+02,
                  5.120168723492e+02,
                  5.120139707209e+02,
                  5.120113749334e+02,
                  5.120090554887e+02,
                  5.120069857863e+02,
                  5.120051414260e+02,
                  5.120034999132e+02,
                  5.120020405355e+02,
                  5.120007442976e+02,
                  5.119995938652e+02,
                  5.119985735001e+02,
                  5.119976689792e+02,
                  5.119968675026e+02,
                  5.119961575929e+02 };

    epsilon = 1.0e-12;
    *verified = TRUE;
    *class_npb = 'U';

    if (d1 == 64 &&
	d2 == 64 &&
	d3 == 64 &&
	nt == 6) {
	*class_npb = 'S';
    for (i = 1; i <= nt; i++) {
        err = (get_real(sums[i]) - vdata_real_s[i]) / vdata_real_s[i];
        if (fabs(err) > epsilon) {
            *verified = FALSE;
            break;
        }
        err = (get_imag(sums[i]) - vdata_imag_s[i]) / vdata_imag_s[i];
        if (fabs(err) > epsilon) {
            *verified = FALSE;
            break;
        }
    }
    } else if (d1 == 128 &&
        d2 == 128 &&
        d3 == 32 &&
        nt == 6) {
        *class_npb = 'W';
        for (i = 1; i <= nt; i++) {
            err = (get_real(sums[i]) - vdata_real_w[i]) / vdata_real_w[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
            err = (get_imag(sums[i]) - vdata_imag_w[i]) / vdata_imag_w[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
        }
    } else if (d1 == 256 &&
        d2 == 256 &&
        d3 == 128 &&
        nt == 6) {
        *class_npb = 'A';
        for (i = 1; i <= nt; i++) {
            err = (get_real(sums[i]) - vdata_real_a[i]) / vdata_real_a[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
            err = (get_imag(sums[i]) - vdata_imag_a[i]) / vdata_imag_a[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
        }
    } else if (d1 == 512 && d2 == 256 && d3 == 256 && nt == 20) {
        *class_npb = 'B';
        for (i = 1; i <= nt; i++) {
            err = (get_real(sums[i]) - vdata_real_b[i]) / vdata_real_b[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
            err = (get_imag(sums[i]) - vdata_imag_b[i]) / vdata_imag_b[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
        }
    } else if (d1 == 512 &&
        d2 == 512 &&
        d3 == 512 &&
        nt == 20) {
        *class_npb = 'C';
        for (i = 1; i <= nt; i++) {
            err = (get_real(sums[i]) - vdata_real_c[i]) / vdata_real_c[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
            err = (get_imag(sums[i]) - vdata_imag_c[i]) / vdata_imag_c[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
        }
    } else if (d1 == 2048 &&
        d2 == 1024 &&
        d3 == 1024 &&
        nt == 25) {
        *class_npb = 'D';
        for (i = 1; i <= nt; i++) {
            err = (get_real(sums[i]) - vdata_real_d[i]) / vdata_real_d[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
            err = (get_imag(sums[i]) - vdata_imag_d[i]) / vdata_imag_d[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
        }
    } else if (d1 == 4096 &&
        d2 == 2048 &&
        d3 == 2048 &&
        nt == 25) {
        *class_npb = 'E';
        for (i = 1; i <= nt; i++) {
            err = (get_real(sums[i]) - vdata_real_e[i]) / vdata_real_e[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
            err = (get_imag(sums[i]) - vdata_imag_e[i]) / vdata_imag_e[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
        }
    } else if (d1 == 8192 &&
        d2 == 4096 &&
        d3 == 4096 &&
        nt == 25) {
        *class_npb = 'F';
        for (i = 1; i <= nt; i++) {
            err = (get_real(sums[i]) - vdata_real_f[i]) / vdata_real_f[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
            err = (get_imag(sums[i]) - vdata_imag_f[i]) / vdata_imag_f[i];
            if (fabs(err) > epsilon) {
                *verified = FALSE;
                break;
            }
        }
    }

    if (*class_npb != 'U') {
        printf("Result verification successful\n");
    } else {
        printf("Result verification failed\n");
    }
    printf("class_npb = %1c\n", *class_npb);
}
