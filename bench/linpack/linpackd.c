/* ES40 emulator -- double-precision LINPACK benchmark for the guest (DEC C / OpenVMS Alpha).
 * Copyright (C) 2026 by gdwnldsKSC
 * All rights reserved.
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS AND CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* The classic 100x100 LINPACK problem: LU factorization with partial pivoting
 * (dgefa) and the triangular solves (dgesl) on a 201-column leading dimension,
 * rolled loops, double precision. MFLOPS = (2n^3/3 + 2n^2) / time. The solve is
 * repeated until the measured interval is long enough for the guest's 10 ms
 * clock tick, and the matrix-generation cost is measured and subtracted.
 *
 * C89 only: builds with DEC C. See linpackd.com for the build and run commands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define N      100
#define LDA    201
#define MIN_SECONDS 5.0

static double a[LDA * N];
static double b[N];
static double x[N];
static int    ipvt[N];

static double second(void)
{
  return (double) clock() / (double) CLOCKS_PER_SEC;
}

/* Congruential test matrix: entries in [-2, 2), b = row sums so the solution is all ones. */
static double matgen(double* m, int lda, int n, double* rhs)
{
  long init = 1325;
  double norma = 0.0;
  int i, j;

  for (j = 0; j < n; j++) {
    for (i = 0; i < n; i++) {
      init = 3125 * init % 65536;
      m[lda * j + i] = ((double) init - 32768.0) / 16384.0;
      if (m[lda * j + i] > norma) norma = m[lda * j + i];
    }
  }
  for (i = 0; i < n; i++) rhs[i] = 0.0;
  for (j = 0; j < n; j++)
    for (i = 0; i < n; i++) rhs[i] += m[lda * j + i];
  return norma;
}

static int idamax(int n, const double* dx)
{
  int i, imax = 0;
  double dmax;
  if (n < 1) return -1;
  dmax = fabs(dx[0]);
  for (i = 1; i < n; i++)
    if (fabs(dx[i]) > dmax) { imax = i; dmax = fabs(dx[i]); }
  return imax;
}

static void dscal(int n, double da, double* dx)
{
  int i;
  for (i = 0; i < n; i++) dx[i] *= da;
}

static void daxpy(int n, double da, const double* dx, double* dy)
{
  int i;
  if (da == 0.0) return;
  for (i = 0; i < n; i++) dy[i] += da * dx[i];
}

/* LU factorization, column oriented, partial pivoting. Returns 0 or the index of a zero pivot. */
static int dgefa(double* m, int lda, int n, int* pvt)
{
  int info = 0, j, k, l, nm1 = n - 1;
  double t;

  for (k = 0; k < nm1; k++) {
    double* col_k = m + lda * k;
    l = idamax(n - k, col_k + k) + k;
    pvt[k] = l;
    if (col_k[l] == 0.0) { info = k; continue; }
    if (l != k) { t = col_k[l]; col_k[l] = col_k[k]; col_k[k] = t; }
    t = -1.0 / col_k[k];
    dscal(n - k - 1, t, col_k + k + 1);
    for (j = k + 1; j < n; j++) {
      double* col_j = m + lda * j;
      t = col_j[l];
      if (l != k) { col_j[l] = col_j[k]; col_j[k] = t; }
      daxpy(n - k - 1, t, col_k + k + 1, col_j + k + 1);
    }
  }
  pvt[nm1] = nm1;
  if (m[lda * nm1 + nm1] == 0.0) info = nm1;
  return info;
}

/* Solve A x = b with the factors from dgefa (job 0). */
static void dgesl(const double* m, int lda, int n, const int* pvt, double* rhs)
{
  int k, kb, l, nm1 = n - 1;
  double t;

  for (k = 0; k < nm1; k++) {           /* L y = b */
    l = pvt[k];
    t = rhs[l];
    if (l != k) { rhs[l] = rhs[k]; rhs[k] = t; }
    daxpy(n - k - 1, t, m + lda * k + k + 1, rhs + k + 1);
  }
  for (kb = 0; kb < n; kb++) {          /* U x = y */
    k = n - kb - 1;
    rhs[k] /= m[lda * k + k];
    t = -rhs[k];
    daxpy(k, t, m + lda * k, rhs);
  }
}

static double epsilon(void)
{
  double eps = 1.0;
  while (1.0 + eps / 2.0 != 1.0) eps /= 2.0;
  return eps;
}

int main(void)
{
  const double ops = (2.0 * (double) N * N * N) / 3.0 + 2.0 * (double) N * N;
  double norma, normx, resid, eps, t0, t1, tgen, tsolve, mflops;
  long reps, r;
  int i, j;

  printf("LINPACK %dx%d double precision, rolled loops, lda = %d\n\n", N, N, LDA);

  /* One instrumented solve for the residual check. */
  norma = matgen(a, LDA, N, b);
  t0 = second();
  dgefa(a, LDA, N, ipvt);
  t1 = second();
  dgesl(a, LDA, N, ipvt, b);
  printf("single solve: dgefa %.2f s, dgesl %.2f s\n", t1 - t0, second() - t1);

  for (i = 0; i < N; i++) x[i] = b[i];
  matgen(a, LDA, N, b);
  for (i = 0; i < N; i++) b[i] = -b[i];
  for (j = 0; j < N; j++) daxpy(N, x[j], a + LDA * j, b);   /* b = A x - b_original */
  resid = 0.0; normx = 0.0;
  for (i = 0; i < N; i++) {
    if (fabs(b[i]) > resid) resid = fabs(b[i]);
    if (fabs(x[i]) > normx) normx = fabs(x[i]);
  }
  eps = epsilon();
  printf("norm. resid  %12.6e   resid  %12.6e   machep  %12.6e\n",
         resid / ((double) N * norma * normx * eps), resid, eps);
  printf("x[0]-1  %12.6e   x[n-1]-1  %12.6e\n\n", x[0] - 1.0, x[N - 1] - 1.0);

  /* Timed runs: grow the repeat count until the interval dwarfs the clock tick. */
  reps = 1;
  for (;;) {
    t0 = second();
    for (r = 0; r < reps; r++) {
      matgen(a, LDA, N, b);
      dgefa(a, LDA, N, ipvt);
      dgesl(a, LDA, N, ipvt, b);
    }
    tsolve = second() - t0;
    if (tsolve >= MIN_SECONDS) break;
    reps *= 2;
  }
  t0 = second();
  for (r = 0; r < reps; r++) matgen(a, LDA, N, b);
  tgen = second() - t0;

  tsolve = (tsolve - tgen) / (double) reps;
  mflops = ops / (tsolve * 1.0e6);
  printf("%ld solves, matgen excluded\n", reps);
  printf("time per solve  %.6f s\n", tsolve);
  printf("Rolled Double Precision LINPACK %dx%d:  %.3f MFLOPS\n", N, N, mflops);
  printf("(solution check: x[0]-1 = %e, x[n-1]-1 = %e)\n", x[0] - 1.0, x[N - 1] - 1.0);
  return 0;
}
