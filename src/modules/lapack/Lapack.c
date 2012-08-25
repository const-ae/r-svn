/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 2001--2012  The R Core Team.
 *  Copyright (C) 2003--2010  The R Foundation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

/* <UTF8> All chars are ASCII */

/* Interface routines, callable from R using .Call, for Lapack code */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <Defn.h>
#include <ctype.h> /* for toupper */

#include "Lapack.h"

/* FIXME: Would want to make these available to packages;
 * BUT:  1) cannot be in libRlapack {since that may be external}
 *       2) Pkgs cannot get it from the C-Lapack interface code {lapack.so}
 *          since that is R-internal
 */
static char La_norm_type(const char *typstr)
{
    char typup;

    if (strlen(typstr) != 1)
	error(
	    _("argument type[1]='%s' must be a character string of string length 1"),
	    typstr);
    typup = (char) toupper(*typstr);
    if (typup == '1')
	typup = 'O'; /* aliases */
    else if (typup == 'E')
	typup = 'F';
    else if (typup != 'M' && typup != 'O' && typup != 'I' && typup != 'F')
	error(_("argument type[1]='%s' must be one of 'M','1','O','I','F' or 'E'"),
	      typstr);
    return typup;
}

/* Lapack condition number approximation: currently only supports _1 or _Inf norm : */
static char La_rcond_type(const char *typstr)
{
    char typup;

    if (strlen(typstr) != 1)
	error(_("argument type[1]='%s' must be a character string of string length 1"),
	      typstr);
    typup = (char) toupper(*typstr);
    if (typup == '1')
	typup = 'O'; /* alias */
    else if (typup != 'O' && typup != 'I')
	error(_("argument type[1]='%s' must be one of '1','O', or 'I'"),
	      typstr);
    return typup; /* 'O' or 'I' */
}


static SEXP La_svd(SEXP jobu, SEXP jobv, SEXP x, SEXP s, SEXP u, SEXP v,
		   SEXP method)
{
    int *xdims, n, p, lwork, info = 0;
    SEXP val, nm;

    if (!(isString(jobu) && isString(jobv)))
	error(_("'jobu' and 'jobv' must be character strings"));
    if (!isString(method))
	error(_("'method' must be a character string"));
    xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    n = xdims[0]; p = xdims[1];
    /* work on a copy of x: duplicate would copy too much, like dimnames */
    double *xvals = (double *) R_alloc(n * (size_t) p, sizeof(double));
    Memcpy(xvals, REAL(x), n * (size_t) p);

    {
	int ldu = INTEGER(getAttrib(u, R_DimSymbol))[0],
	    ldvt = INTEGER(getAttrib(v, R_DimSymbol))[0];
	double tmp;
	/* min(n,p) large is implausible, but ... */
	int *iwork= (int *) R_alloc(8*(size_t)(n < p ? n : p), sizeof(int));

	/* ask for optimal size of work array */
	lwork = -1;
	F77_CALL(dgesdd)(CHAR(STRING_ELT(jobu, 0)),
			 &n, &p, xvals, &n, REAL(s),
			 REAL(u), &ldu,
			 REAL(v), &ldvt,
			 &tmp, &lwork, iwork, &info);
	if (info != 0)
	    error(_("error code %d from Lapack routine '%s'"), info, "dgesdd");
	lwork = (int) tmp;
	double *work = (double *) R_alloc(lwork, sizeof(double));
	F77_CALL(dgesdd)(CHAR(STRING_ELT(jobu, 0)),
			 &n, &p, xvals, &n, REAL(s),
			 REAL(u), &ldu,
			 REAL(v), &ldvt,
			 work, &lwork, iwork, &info);
	if (info != 0)
	    error(_("error code %d from Lapack routine '%s'"), info, "dgesdd");
    }

    val = PROTECT(allocVector(VECSXP, 3));
    nm = PROTECT(allocVector(STRSXP, 3));
    SET_STRING_ELT(nm, 0, mkChar("d"));
    SET_STRING_ELT(nm, 1, mkChar("u"));
    SET_STRING_ELT(nm, 2, mkChar("vt"));
    setAttrib(val, R_NamesSymbol, nm);
    SET_VECTOR_ELT(val, 0, s);
    SET_VECTOR_ELT(val, 1, u);
    SET_VECTOR_ELT(val, 2, v);
    UNPROTECT(2);
    return val;
}

static SEXP La_rs(SEXP xin, SEXP only_values)
{
    int *xdims, n, lwork, info = 0, ov;
    char jobv[1], uplo[1], range[1];
    SEXP values, ret, nm, x, z = R_NilValue;
    double *work, *rx, *rvalues, tmp, *rz = NULL;
    int liwork, *iwork, itmp, m;
    double vl = 0.0, vu = 0.0, abstol = 0.0;
    /* valgrind seems to think vu should be set, but it is documented
       not to be used if range='a' */
    int il, iu, *isuppz;

    PROTECT(x = duplicate(xin));
    rx = REAL(x);
    uplo[0] = 'L';
    xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    n = xdims[0];
    if (n != xdims[1])
	error(_("'x' must be a square numeric matrix"));
    ov = asLogical(only_values);
    if (ov == NA_LOGICAL) error(_("invalid '%s' argument"), "only.values");
    if (ov) jobv[0] = 'N'; else jobv[0] = 'V';

    PROTECT(values = allocVector(REALSXP, n));
    rvalues = REAL(values);

    range[0] = 'A';
    if (!ov) {
	PROTECT(z = allocMatrix(REALSXP, n, n));
	rz = REAL(z);
    }
    isuppz = (int *) R_alloc(2*n, sizeof(int));
    /* ask for optimal size of work arrays */
    lwork = -1; liwork = -1;
    F77_CALL(dsyevr)(jobv, range, uplo, &n, rx, &n,
		     &vl, &vu, &il, &iu, &abstol, &m, rvalues,
		     rz, &n, isuppz,
		     &tmp, &lwork, &itmp, &liwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dsyevr");
    lwork = (int) tmp;
    liwork = itmp;

    work = (double *) R_alloc(lwork, sizeof(double));
    iwork = (int *) R_alloc(liwork, sizeof(int));
    F77_CALL(dsyevr)(jobv, range, uplo, &n, rx, &n,
		     &vl, &vu, &il, &iu, &abstol, &m, rvalues,
		     rz, &n, isuppz,
		     work, &lwork, iwork, &liwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dsyevr");

    if (!ov) {
	ret = PROTECT(allocVector(VECSXP, 2));
	nm = PROTECT(allocVector(STRSXP, 2));
	SET_STRING_ELT(nm, 1, mkChar("vectors"));
	SET_VECTOR_ELT(ret, 1, z);
	UNPROTECT_PTR(z);
    } else {
	ret = PROTECT(allocVector(VECSXP, 1));
	nm = PROTECT(allocVector(STRSXP, 1));
    }
    SET_STRING_ELT(nm, 0, mkChar("values"));
    setAttrib(ret, R_NamesSymbol, nm);
    SET_VECTOR_ELT(ret, 0, values);
    UNPROTECT(4);
    return ret;
}

static SEXP unscramble(const double* imaginary, int n,
		       const double* vecs)
{
    int i, j;
    SEXP s = allocMatrix(CPLXSXP, n, n);
    size_t N = n;
    for (j = 0; j < n; j++) {
	if (imaginary[j] != 0) {
	    int j1 = j + 1;
	    for (i = 0; i < n; i++) {
		COMPLEX(s)[i+N*j].r = COMPLEX(s)[i+N*j1].r = vecs[i + j * N];
		COMPLEX(s)[i+N*j1].i = -(COMPLEX(s)[i+N*j].i = vecs[i + j1 * N]);
	    }
	    j = j1;
	} else {
	    for (i = 0; i < n; i++) {
		COMPLEX(s)[i+N*j].r = vecs[i + j * N];
		COMPLEX(s)[i+N*j].i = 0.0;
	    }
	}
    }
    return s;
}

static SEXP La_rg(SEXP x, SEXP only_values)
{
    Rboolean vectors, complexValues;
    int i, n, lwork, info, *xdims, ov;
    double *work, *wR, *wI, *left, *right, *xvals, tmp;
    char jobVL[1], jobVR[1];
    SEXP ret, nm, val;

    xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    n = xdims[0];
    if (n != xdims[1])
	error(_("'x' must be a square numeric matrix"));

    xvals = (double *) R_alloc(n * n, sizeof(double));
    /* work on a copy of x */
    Memcpy(xvals, REAL(x), (size_t) n * n);
    ov = asLogical(only_values);
    if (ov == NA_LOGICAL) error(_("invalid '%s' argument"), "only.values");
    vectors = !ov;
    jobVL[0] = jobVR[0] = 'N';
    left = right = (double *) 0;
    if (vectors) {
	jobVR[0] = 'V';
	right = (double *) R_alloc(n * n, sizeof(double));
    }
    wR = (double *) R_alloc(n, sizeof(double));
    wI = (double *) R_alloc(n, sizeof(double));
    /* ask for optimal size of work array */
    lwork = -1;
    F77_CALL(dgeev)(jobVL, jobVR, &n, xvals, &n, wR, wI,
		    left, &n, right, &n, &tmp, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dgeev");
    lwork = (int) tmp;
    work = (double *) R_alloc(lwork, sizeof(double));
    F77_CALL(dgeev)(jobVL, jobVR, &n, xvals, &n, wR, wI,
		    left, &n, right, &n, work, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dgeev");

    complexValues = FALSE;
    for (i = 0; i < n; i++)
	/* This test used to be !=0 for R < 2.3.0.  This is OK for 0+0i */
	if (fabs(wI[i]) >  10 * R_AccuracyInfo.eps * fabs(wR[i])) {
	    complexValues = TRUE;
	    break;
	}
    ret = PROTECT(allocVector(VECSXP, 2));
    nm = PROTECT(allocVector(STRSXP, 2));
    SET_STRING_ELT(nm, 0, mkChar("values"));
    SET_STRING_ELT(nm, 1, mkChar("vectors"));
    setAttrib(ret, R_NamesSymbol, nm);
    SET_VECTOR_ELT(ret, 1, R_NilValue);
    if (complexValues) {
	val = allocVector(CPLXSXP, n);
	for (i = 0; i < n; i++) {
	    COMPLEX(val)[i].r = wR[i];
	    COMPLEX(val)[i].i = wI[i];
	}
	SET_VECTOR_ELT(ret, 0, val);

	if (vectors)
	    SET_VECTOR_ELT(ret, 1, unscramble(wI, n, right));
    } else {
	val = allocVector(REALSXP, n);
	for (i = 0; i < n; i++)
	    REAL(val)[i] = wR[i];
	SET_VECTOR_ELT(ret, 0, val);
	if(vectors) {
	    val = allocMatrix(REALSXP, n, n);
	    for (i = 0; i < (n * n); i++)
		REAL(val)[i] = right[i];
	    SET_VECTOR_ELT(ret, 1, val);
	}
    }
    UNPROTECT(2);
    return ret;
}

static SEXP La_dlange(SEXP A, SEXP type)
{
    SEXP x, val;
    int *xdims, m, n, nprot = 0;
    double *work = NULL;
    char typNorm[] = {'\0', '\0'};

    if (!isString(type))
	error(_("'type' must be a character string"));
    if (!isReal(A) && isNumeric(A)) {
	x = PROTECT(coerceVector(A, REALSXP)); nprot++;
    } else
	x = A;
    if (!(isMatrix(x) && isReal(x))) {
	UNPROTECT(nprot);
	error(_("'A' must be a numeric matrix"));
    }

    xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    m = xdims[0];
    n = xdims[1]; /* m x n  matrix {using Lapack naming convention} */

    typNorm[0] = La_norm_type(CHAR(asChar(type)));

    val = PROTECT(allocVector(REALSXP, 1)); nprot++;
    if(*typNorm == 'I') work = (double *) R_alloc(m, sizeof(double));
    REAL(val)[0] = F77_CALL(dlange)(typNorm, &m, &n, REAL(x), &m, work);

    UNPROTECT(nprot);
    return val;
}


/* ------------------------------------------------------------ */
static SEXP La_dgecon(SEXP A, SEXP norm)
{
    SEXP x, val;
    int *xdims, m, n, info, *iwork;
    double anorm, *work;
    char typNorm[] = {'\0', '\0'};

    if (!isString(norm))
	error(_("'norm' must be a character string"));
    if (!isReal(A) && isNumeric(A))
	x = PROTECT(coerceVector(A, REALSXP));
    else
	x = PROTECT(duplicate(A)); /* will be overwritten by LU */
    if (!(isMatrix(x) && isReal(x))) {
	UNPROTECT(1);
	error(_("'A' must be a numeric matrix"));
    }

    xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    m = xdims[0];
    n = xdims[1]; /* m x n  matrix {using Lapack naming convention} */

    typNorm[0] = La_rcond_type(CHAR(asChar(norm)));

    val = PROTECT(allocVector(REALSXP, 1));
    work = (double *) R_alloc((*typNorm == 'I' && m > 4*n) ? m : 4*n,
			      sizeof(double));
    iwork = (int *) R_alloc(m, sizeof(int));

    anorm = F77_CALL(dlange)(typNorm, &m, &n, REAL(x), &m, work);

    /* Compute the LU-decomposition and overwrite 'x' with result :*/
    F77_CALL(dgetrf)(&m, &n, REAL(x), &m, iwork, &info);
    if (info) {
	if (info < 0) {
	    UNPROTECT(2);
	    error(_("error [%d] from Lapack 'dgetrf()'"), info);
	}
	else { /* i := info > 0:  LU decomp. is completed, but  U[i,i] = 0
		* <==> singularity */
#if 0
	    warning(_("exact singularity: U[%d,%d] = 0 in LU-decomposition {Lapack 'dgetrf()'}"),
		    info,info);
#endif
	    REAL(val)[0] = 0.; /* rcond = 0 <==> singularity */
	    UNPROTECT(2);
	    return val;
	}
    }
    F77_CALL(dgecon)(typNorm, &n, REAL(x), &n, &anorm,
		     /* rcond = */ REAL(val),
		     work, iwork, &info);
    UNPROTECT(2);
    if (info) error(_("error [%d] from Lapack 'dgecon()'"), info);
    return val;
}

static SEXP La_dtrcon(SEXP A, SEXP norm)
{
    SEXP x, val;
    int *xdims, n, nprot = 0, info;
    char typNorm[] = {'\0', '\0'};

    if (!isString(norm))
	error(_("'norm' must be a character string"));
    if (!isReal(A) && isNumeric(A)) {
	nprot++;
	x = PROTECT(coerceVector(A, REALSXP));
    } else
	x = A; /* no copy needed */
    if (!(isMatrix(x) && isReal(x))) {
	UNPROTECT(nprot);
	error(_("'A' must be a numeric matrix"));
    }
    xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    n = xdims[0];
    if(n != xdims[1]) {
	UNPROTECT(nprot);
	error(_("'A' must be a *square* matrix"));
    }

    typNorm[0] = La_rcond_type(CHAR(asChar(norm)));

    nprot++;
    val = PROTECT(allocVector(REALSXP, 1));

    F77_CALL(dtrcon)(typNorm, "U", "N", &n, REAL(x), &n,
		     REAL(val),
		     /* work : */ (double *) R_alloc(3*n, sizeof(double)),
		     /* iwork: */ (int *)    R_alloc(n, sizeof(int)),
		     &info);
    UNPROTECT(nprot);
    if (info) error(_("error [%d] from Lapack 'dtrcon()'"), info);
    return val;
}

static SEXP La_zgecon(SEXP A, SEXP norm)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    SEXP val;
    Rcomplex *avals; /* copy of A, to be modified */
    int *dims, n, info;
    double anorm, *rwork;
    char typNorm[] = {'\0', '\0'};

    if (!isString(norm))
	error(_("'norm' must be a character string"));
    if (!(isMatrix(A) && isComplex(A)))
	error(_("'A' must be a complex matrix"));
    dims = INTEGER(coerceVector(getAttrib(A, R_DimSymbol), INTSXP));
    n = dims[0];
    if(n != dims[1])
	error(_("'A' must be a *square* matrix"));

    typNorm[0] = La_rcond_type(CHAR(asChar(norm)));

    val = PROTECT(allocVector(REALSXP, 1));

    rwork = (double *) R_alloc(2*n, sizeof(Rcomplex));
    anorm = F77_CALL(zlange)(typNorm, &n, &n, COMPLEX(A), &n, rwork);

    /* Compute the LU-decomposition and overwrite 'x' with result;
     * working on a copy of A : */
    avals = (Rcomplex *) R_alloc(n * n, sizeof(Rcomplex));
    Memcpy(avals, COMPLEX(A), (size_t) n * n);
    F77_CALL(zgetrf)(&n, &n, avals, &n,
		     /* iwork: */(int *) R_alloc(n, sizeof(int)),
		     &info);
    if (info) {
	UNPROTECT(1);
	error(_("error [%d] from Lapack 'zgetrf()'"), info);
    }

    F77_CALL(zgecon)(typNorm, &n, avals, &n, &anorm,
		     /* rcond = */ REAL(val),
		     /* work : */ (Rcomplex *) R_alloc(4*n, sizeof(Rcomplex)),
		     rwork, &info);
    UNPROTECT(1);
    if (info) error(_("error [%d] from Lapack 'zgecon()'"), info);
    return val;

#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}

static SEXP La_ztrcon(SEXP A, SEXP norm)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    SEXP val;
    int *dims, n, info;
    char typNorm[] = {'\0', '\0'};

    if (!isString(norm))
	error(_("'norm' must be a character string"));
    if (!(isMatrix(A) && isComplex(A)))
	error(_("'A' must be a complex matrix"));
    dims = INTEGER(coerceVector(getAttrib(A, R_DimSymbol), INTSXP));
    n = dims[0];
    if(n != dims[1])
	error(_("'A' must be a *square* matrix"));

    typNorm[0] = La_rcond_type(CHAR(asChar(norm)));

    val = PROTECT(allocVector(REALSXP, 1));

    F77_CALL(ztrcon)(typNorm, "U", "N", &n, COMPLEX(A), &n,
		     REAL(val),
		     /* work : */ (Rcomplex *) R_alloc(2*n, sizeof(Rcomplex)),
		     /* rwork: */ (double *)   R_alloc(n, sizeof(double)),
		     &info);
    UNPROTECT(1);
    if (info) error(_("error [%d] from Lapack 'ztrcon()'"), info);
    return val;
#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}



// see the comments in La_zgesv
static SEXP La_zgesv(SEXP A, SEXP Bin)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    int n, p, info, *ipiv, *Adims, *Bdims;
    Rcomplex *avals;
    SEXP B, Ad, Bd, Adn, Bdn;

    if (!(isMatrix(A) && isComplex(A)))
	error(_("'a' must be a complex matrix"));
    if (!isComplex(Bin)) error(_("'b' must be a complex vector or matrix"));
    PROTECT(Ad = coerceVector(getAttrib(A, R_DimSymbol), INTSXP));
    Adims = INTEGER(Ad);
    n = Adims[0];
    if(n == 0) error(_("'a' is 0-diml"));
    if(Adims[1] != n)
	error(_("'a' (%d x %d) must be square"), n, Adims[1]);
    Adn = getAttrib(A, R_DimNamesSymbol);

    if (isMatrix(Bin)) {
	PROTECT(Bd = coerceVector(getAttrib(Bin, R_DimSymbol), INTSXP));
	Bdims = INTEGER(Bd);
	p = Bdims[1];
	if(p == 0) error(_("no right-hand side in 'b'"));
	if(Bdims[0] != n)
	    error(_("'b' (%d x %d) must be compatible with 'a' (%d x %d)"),
		  Bdims[0], p, n, n);
	UNPROTECT(1);
	PROTECT(B = allocMatrix(CPLXSXP, n, p));
	SEXP Bindn =  getAttrib(Bin, R_DimNamesSymbol);
	if (!isNull(Adn) || !isNull(Bindn)) {
	    Bdn = allocVector(VECSXP, 2);
	    if (!isNull(Adn)) SET_VECTOR_ELT(Bdn, 0, VECTOR_ELT(Adn, 1));
	    if (!isNull(Bindn)) SET_VECTOR_ELT(Bdn, 1, VECTOR_ELT(Bindn, 1));
	    if (!isNull(VECTOR_ELT(Bdn, 0)) && !isNull(VECTOR_ELT(Bdn, 1)))
		setAttrib(B, R_DimNamesSymbol, Bdn);
	}
    } else {
	p = 1;
	if(length(Bin) != n)
	    error(_("'b' (%d x %d) must be compatible with 'a' (%d x %d)"),
		  length(Bin), p, n, n);	
	PROTECT(B = allocVector(CPLXSXP, n));
	if (!isNull(Adn)) setAttrib(B, R_NamesSymbol, VECTOR_ELT(Adn, 1));
    }
    Memcpy(COMPLEX(B), COMPLEX(Bin), (size_t)n * p);

    ipiv = (int *) R_alloc(n, sizeof(int));

    /* work on a copy of A */
    avals = (Rcomplex *) R_alloc(n * n, sizeof(Rcomplex));
    Memcpy(avals, COMPLEX(A), (size_t) n * n);
    F77_CALL(zgesv)(&n, &p, avals, &n, ipiv, COMPLEX(B), &n, &info);
    if (info < 0)
	error(_("argument %d of Lapack routine %s had invalid value"),
	      -info, "zgesv");
    if (info > 0)
	error(("Lapack routine zgesv: system is exactly singular"));
    UNPROTECT(3);
    return B;
#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}

static SEXP La_zgeqp3(SEXP Ain)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    int i, m, n, *Adims, info, lwork;
    Rcomplex *work, tmp;
    double *rwork;
    SEXP val, nm, jpvt, tau, rank, A;

    if (!(isMatrix(Ain) && isComplex(Ain)))
	error(_("'a' must be a complex matrix"));
    PROTECT(A = duplicate(Ain));
    Adims = INTEGER(coerceVector(getAttrib(A, R_DimSymbol), INTSXP));
    m = Adims[0];
    n = Adims[1];
    rwork = (double *) R_alloc(2*n, sizeof(double));

    jpvt = PROTECT(allocVector(INTSXP, n));
    for (i = 0; i < n; i++) INTEGER(jpvt)[i] = 0;
    tau = PROTECT(allocVector(CPLXSXP, m < n ? m : n));
    lwork = -1;
    F77_CALL(zgeqp3)(&m, &n, COMPLEX(A), &m, INTEGER(jpvt), COMPLEX(tau),
		     &tmp, &lwork, rwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zgeqp3");
    lwork = (int) tmp.r;
    work = (Rcomplex *) R_alloc(lwork, sizeof(Rcomplex));
    F77_CALL(zgeqp3)(&m, &n, COMPLEX(A), &m, INTEGER(jpvt), COMPLEX(tau),
		     work, &lwork, rwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zgeqp3");
    val = PROTECT(allocVector(VECSXP, 4));
    nm = PROTECT(allocVector(STRSXP, 4));
    rank = PROTECT(ScalarInteger(m < n ? m : n));
    SET_STRING_ELT(nm, 0, mkChar("qr"));
    SET_STRING_ELT(nm, 1, mkChar("rank"));
    SET_STRING_ELT(nm, 2, mkChar("qraux"));
    SET_STRING_ELT(nm, 3, mkChar("pivot"));
    setAttrib(val, R_NamesSymbol, nm);
    SET_VECTOR_ELT(val, 0, A);
    SET_VECTOR_ELT(val, 1, rank);
    SET_VECTOR_ELT(val, 2, tau);
    SET_VECTOR_ELT(val, 3, jpvt);
    UNPROTECT(6);
    return val;
#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}

static SEXP qr_coef_cmplx(SEXP Q, SEXP Bin)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    int n, nrhs, lwork, info, k, *Bdims, *Qdims;
    SEXP B, qr=VECTOR_ELT(Q, 0), tau=VECTOR_ELT(Q, 2);
    Rcomplex *work, tmp;

    k = LENGTH(tau);
    if (!(isMatrix(Bin) && isComplex(Bin)))
	error(_("'b' must be a complex matrix"));

    PROTECT(B = duplicate(Bin));
    Qdims = INTEGER(coerceVector(getAttrib(qr, R_DimSymbol), INTSXP));
    n = Qdims[0];
    Bdims = INTEGER(coerceVector(getAttrib(Bin, R_DimSymbol), INTSXP));
    if(Bdims[0] != n)
	error(_("right-hand side should have %d not %d rows"), n, Bdims[0]);
    nrhs = Bdims[1];
    lwork = -1;
    F77_CALL(zunmqr)("L", "C", &n, &nrhs, &k,
		     COMPLEX(qr), &n, COMPLEX(tau), COMPLEX(B), &n,
		     &tmp, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zunmqr");
    lwork = (int) tmp.r;
    work = (Rcomplex *) R_alloc(lwork, sizeof(Rcomplex));
    F77_CALL(zunmqr)("L", "C", &n, &nrhs, &k,
		     COMPLEX(qr), &n, COMPLEX(tau), COMPLEX(B), &n,
		     work, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zunmqr");
    F77_CALL(ztrtrs)("U", "N", "N", &k, &nrhs,
		     COMPLEX(qr), &n, COMPLEX(B), &n, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "ztrtrs");
    UNPROTECT(1);
    return B;
#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}

static SEXP qr_qy_cmplx(SEXP Q, SEXP Bin, SEXP trans)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    int n, nrhs, lwork, info, k, *Bdims, *Qdims, tr;
    SEXP B, qr=VECTOR_ELT(Q, 0), tau=VECTOR_ELT(Q, 2);
    Rcomplex *work, tmp;

    k = LENGTH(tau);
    if (!(isMatrix(Bin) && isComplex(Bin)))
	error(_("'b' must be a complex matrix"));
    tr = asLogical(trans);
    if(tr == NA_LOGICAL) error(_("invalid '%s' argument"), "trans");

    PROTECT(B = duplicate(Bin));
    Qdims = INTEGER(coerceVector(getAttrib(qr, R_DimSymbol), INTSXP));
    n = Qdims[0];
    Bdims = INTEGER(coerceVector(getAttrib(B, R_DimSymbol), INTSXP));
    if(Bdims[0] != n)
	error(_("right-hand side should have %d not %d rows"), n, Bdims[0]);
    nrhs = Bdims[1];
    lwork = -1;
    F77_CALL(zunmqr)("L", tr ? "C" : "N", &n, &nrhs, &k,
		     COMPLEX(qr), &n, COMPLEX(tau), COMPLEX(B), &n,
		     &tmp, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zunmqr");
    lwork = (int) tmp.r;
    work = (Rcomplex *) R_alloc(lwork, sizeof(Rcomplex));
    F77_CALL(zunmqr)("L", tr ? "C" : "N", &n, &nrhs, &k,
		     COMPLEX(qr), &n, COMPLEX(tau), COMPLEX(B), &n,
		     work, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zunmqr");
    UNPROTECT(1);
    return B;
#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}

static SEXP La_svd_cmplx(SEXP jobu, SEXP jobv, SEXP x, SEXP s, SEXP u, SEXP v)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    SEXP val, nm;

    if (!(isString(jobu) && isString(jobv)))
	error(_("'jobu' and 'jobv' must be character strings"));
    int *xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    int n = xdims[0], p = xdims[1];

    /* The underlying LAPACK, specifically ZLARF, does not work with
     * long arrays */
    if ((double)n * (double)p > INT_MAX)
	error(_("matrices of 2^31 or more elements are not supported"));
    double *rwork = (double *) R_alloc(5*(size_t)(n < p ? n:p), sizeof(double));

    /* work on a copy of x: duplicate would copy too much, like dimnames */
    Rcomplex *xvals = (Rcomplex *) R_alloc(n * (size_t) p, sizeof(Rcomplex));
    Memcpy(xvals, COMPLEX(x), n * (size_t) p);

    /* ask for optimal size of work array */
    int lwork = -1, info;
    Rcomplex tmp;
    F77_CALL(zgesvd)(CHAR(STRING_ELT(jobu, 0)), CHAR(STRING_ELT(jobv, 0)),
		     &n, &p, xvals, &n, REAL(s),
		     COMPLEX(u), INTEGER(getAttrib(u, R_DimSymbol)),
		     COMPLEX(v), INTEGER(getAttrib(v, R_DimSymbol)),
		     &tmp, &lwork, rwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zgesvd");
    lwork = (int) tmp.r;
    Rcomplex *work = (Rcomplex *) R_alloc(lwork, sizeof(Rcomplex));
    F77_CALL(zgesvd)(CHAR(STRING_ELT(jobu, 0)), CHAR(STRING_ELT(jobv, 0)),
		     &n, &p, xvals, &n, REAL(s),
		     COMPLEX(u), INTEGER(getAttrib(u, R_DimSymbol)),
		     COMPLEX(v), INTEGER(getAttrib(v, R_DimSymbol)),
		     work, &lwork, rwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zgesvd");
    val = PROTECT(allocVector(VECSXP, 3));
    nm = PROTECT(allocVector(STRSXP, 3));
    SET_STRING_ELT(nm, 0, mkChar("d"));
    SET_STRING_ELT(nm, 1, mkChar("u"));
    SET_STRING_ELT(nm, 2, mkChar("vt"));
    setAttrib(val, R_NamesSymbol, nm);
    SET_VECTOR_ELT(val, 0, s);
    SET_VECTOR_ELT(val, 1, u);
    SET_VECTOR_ELT(val, 2, v);
    UNPROTECT(2);
    return val;
#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}

static SEXP La_rs_cmplx(SEXP xin, SEXP only_values)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    int *xdims, n, lwork, info, ov;
    char jobv[1], uplo[1];
    SEXP values, ret, nm, x;
    Rcomplex *work, *rx, tmp;
    double *rwork, *rvalues;

    PROTECT(x = duplicate(xin));
    rx = COMPLEX(x);
    uplo[0] = 'L';
    xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    n = xdims[0];
    if (n != xdims[1])
	error(_("'x' must be a square numeric matrix"));
    ov = asLogical(only_values);
    if (ov == NA_LOGICAL) error(_("invalid '%s' argument"), "only.values");
    if (ov) jobv[0] = 'N'; else jobv[0] = 'V';

    PROTECT(values = allocVector(REALSXP, n));
    rvalues = REAL(values);
    rwork = (double *) R_alloc((3*n-2) > 1 ? 3*n-2 : 1, sizeof(double));
    /* ask for optimal size of work array */
    lwork = -1;
    F77_CALL(zheev)(jobv, uplo, &n, rx, &n, rvalues, &tmp, &lwork, rwork,
		    &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zheev");
    lwork = (int) tmp.r;
    work = (Rcomplex *) R_alloc(lwork, sizeof(Rcomplex));
    F77_CALL(zheev)(jobv, uplo, &n, rx, &n, rvalues, work, &lwork, rwork,
		    &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zheev");
    if (!ov) {
	ret = PROTECT(allocVector(VECSXP, 2));
	nm = PROTECT(allocVector(STRSXP, 2));
	SET_STRING_ELT(nm, 1, mkChar("vectors"));
	SET_VECTOR_ELT(ret, 1, x);
    }
    else {
	ret = PROTECT(allocVector(VECSXP, 1));
	nm = PROTECT(allocVector(STRSXP, 1));
    }
    SET_STRING_ELT(nm, 0, mkChar("values"));
    setAttrib(ret, R_NamesSymbol, nm);
    SET_VECTOR_ELT(ret, 0, values);
    UNPROTECT(4);
    return ret;
#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}

static SEXP La_rg_cmplx(SEXP x, SEXP only_values)
{
#ifdef HAVE_FORTRAN_DOUBLE_COMPLEX
    int  n, lwork, info, *xdims, ov;
    Rcomplex *work, *left, *right, *xvals, tmp;
    double *rwork;
    char jobVL[1], jobVR[1];
    SEXP ret, nm, values, val = R_NilValue;

    xdims = INTEGER(coerceVector(getAttrib(x, R_DimSymbol), INTSXP));
    n = xdims[0];
    if (n != xdims[1])
	error(_("'x' must be a square numeric matrix"));

    xvals = (Rcomplex *) R_alloc(n * n, sizeof(Rcomplex));
    /* work on a copy of x */
    Memcpy(xvals, COMPLEX(x), (size_t) n * n);
    ov = asLogical(only_values);
    if (ov == NA_LOGICAL) error(_("invalid '%s' argument"), "only.values");
    jobVL[0] = jobVR[0] = 'N';
    left = right = (Rcomplex *) 0;
    if (!ov) {
	jobVR[0] = 'V';
	PROTECT(val = allocMatrix(CPLXSXP, n, n));
	right = COMPLEX(val);
    }
    PROTECT(values = allocVector(CPLXSXP, n));
    rwork = (double *) R_alloc(2*n, sizeof(double));
    /* ask for optimal size of work array */
    lwork = -1;
    F77_CALL(zgeev)(jobVL, jobVR, &n, xvals, &n, COMPLEX(values),
		    left, &n, right, &n, &tmp, &lwork, rwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zgeev");
    lwork = (int) tmp.r;
    work = (Rcomplex *) R_alloc(lwork, sizeof(Rcomplex));
    F77_CALL(zgeev)(jobVL, jobVR, &n, xvals, &n, COMPLEX(values),
		    left, &n, right, &n, work, &lwork, rwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "zgeev");

    if(!ov){
	ret = PROTECT(allocVector(VECSXP, 2));
	nm = PROTECT(allocVector(STRSXP, 2));
	SET_STRING_ELT(nm, 1, mkChar("vectors"));
	SET_VECTOR_ELT(ret, 1, val);
    } else {
	ret = PROTECT(allocVector(VECSXP, 1));
	nm = PROTECT(allocVector(STRSXP, 1));
    }
    SET_STRING_ELT(nm, 0, mkChar("values"));
    SET_VECTOR_ELT(ret, 0, values);
    setAttrib(ret, R_NamesSymbol, nm);
    UNPROTECT(ov ? 3 : 4);
    return ret;
#else
    error(_("Fortran complex functions are not available on this platform"));
    return R_NilValue; /* -Wall */
#endif
}

/* ------------------------------------------------------------ */

static SEXP La_chol(SEXP A)
{
    if (isMatrix(A)) {
	SEXP ans = PROTECT((TYPEOF(A) == REALSXP)?duplicate(A):
			   coerceVector(A, REALSXP));
	SEXP adims = getAttrib(A, R_DimSymbol);
	int m = INTEGER(adims)[0];
	int n = INTEGER(adims)[1];
	int i, j;

	if (m != n) error(_("'a' must be a square matrix"));
	if (m <= 0) error(_("'a' must have dims > 0"));
	size_t N = n;
	for (j = 0; j < n; j++) {	/* zero the lower triangle */
	    for (i = j+1; i < n; i++) {
		REAL(ans)[i + N * j] = 0.;
	    }
	}

	F77_CALL(dpotrf)("Upper", &m, REAL(ans), &m, &i);
	if (i != 0) {
	    if (i > 0)
		error(_("the leading minor of order %d is not positive definite"),
		      i);
	    error(_("argument %d of Lapack routine %s had invalid value"),
		  -i, "dpotrf");
	}
	unprotect(1);
	return ans;
    }
    else error(_("'a' must be a numeric matrix"));
    return R_NilValue; /* -Wall */
}

static SEXP La_chol2inv(SEXP A, SEXP size)
{
    int sz = asInteger(size);
    if (sz == NA_INTEGER || sz < 1) {
	error(_("'size' argument must be a positive integer"));
	return R_NilValue; /* -Wall */
    } else {
	SEXP ans, Amat = A; /* -Wall: we initialize here as for the 1x1 case */
	int m = 1, n = 1, i, j, nprot = 0;

	if (sz == 1 && !isMatrix(A) && isReal(A)) {
	    /* nothing to do; m = n = 1; ... */
	} else if (isMatrix(A)) {
	    SEXP adims = getAttrib(A, R_DimSymbol);
	    Amat = PROTECT(coerceVector(A, REALSXP)); nprot++;
	    m = INTEGER(adims)[0];
	    n = INTEGER(adims)[1];
	} else error(_("'a' must be a numeric matrix"));

	if (sz > n) { UNPROTECT(nprot); error(_("'size' cannot exceed ncol(x) = %d"), n); }
	if (sz > m) { UNPROTECT(nprot); error(_("'size' cannot exceed nrow(x) = %d"), m); }
	ans = PROTECT(allocMatrix(REALSXP, sz, sz)); nprot++;
	size_t M = m, SZ = sz;
	for (j = 0; j < sz; j++) {
	    for (i = 0; i <= j; i++)
		REAL(ans)[i + j * SZ] = REAL(Amat)[i + j * M];
	}
	F77_CALL(dpotri)("Upper", &sz, REAL(ans), &sz, &i);
	if (i != 0) {
	    UNPROTECT(nprot);
	    if (i > 0)
		error(_("element (%d, %d) is zero, so the inverse cannot be computed"),
		      i, i);
	    error(_("argument %d of Lapack routine %s had invalid value"),
		  -i, "dpotri");
	}
	for (j = 0; j < sz; j++) {
	    for (i = j+1; i < sz; i++)
		REAL(ans)[i + j * SZ] = REAL(ans)[j + i * SZ];
	}
	UNPROTECT(nprot);
	return ans;
    }
}

/* ------------------------------------------------------------ */

static SEXP La_dgesv(SEXP A, SEXP Bin, SEXP tolin)
{
    int n, p, info, *ipiv, *Adims, *Bdims;
    double *avals, anorm, rcond, tol = asReal(tolin), *work;
    SEXP B, Ad, Bd, Adn, Bdn;

    if (!(isMatrix(A) && (TYPEOF(A) == REALSXP || TYPEOF(A) == INTSXP)))
	error(_("'a' must be a numeric matrix"));
    PROTECT(Ad = coerceVector(getAttrib(A, R_DimSymbol), INTSXP));
    Adims = INTEGER(Ad);
    n = Adims[0];
    if(n == 0) error(_("'a' is 0-diml"));
    if(Adims[1] != n)
	error(_("'a' (%d x %d) must be square"), n, Adims[1]);
    Adn = getAttrib(A, R_DimNamesSymbol);

    if (isMatrix(Bin)) {
	PROTECT(Bd = coerceVector(getAttrib(Bin, R_DimSymbol), INTSXP));
	Bdims = INTEGER(Bd);
	p = Bdims[1];
	if(p == 0) error(_("no right-hand side in 'b'"));
	if(Bdims[0] != n)
	    error(_("'b' (%d x %d) must be compatible with 'a' (%d x %d)"),
		  Bdims[0], p, n, n);
	UNPROTECT(1);
	PROTECT(B = allocMatrix(REALSXP, n, p));
	SEXP Bindn =  getAttrib(Bin, R_DimNamesSymbol);
	// This is somewhat odd, but Matrix relies on dropping NULL dimnames
	if (!isNull(Adn) || !isNull(Bindn)) {
	    // rownames(ans) = colnames(A), colnames(ans) = colnames(Bin)
	    Bdn = allocVector(VECSXP, 2);
	    if (!isNull(Adn)) SET_VECTOR_ELT(Bdn, 0, VECTOR_ELT(Adn, 1));
	    if (!isNull(Bindn)) SET_VECTOR_ELT(Bdn, 1, VECTOR_ELT(Bindn, 1));
	    if (!isNull(VECTOR_ELT(Bdn, 0)) && !isNull(VECTOR_ELT(Bdn, 1)))
		setAttrib(B, R_DimNamesSymbol, Bdn);
	}
    } else {
	p = 1;
	if(length(Bin) != n)
	    error(_("'b' (%d x %d) must be compatible with 'a' (%d x %d)"),
		  length(Bin), p, n, n);	
	PROTECT(B = allocVector(REALSXP, n));
	if (!isNull(Adn)) setAttrib(B, R_NamesSymbol, VECTOR_ELT(Adn, 1));
    }
    Memcpy(REAL(B), REAL(Bin), (size_t)n * p);
    
    ipiv = (int *) R_alloc(n, sizeof(int));

    /* work on a copy of A */
    PROTECT(A = coerceVector(A, REALSXP));
    avals = (double *) R_alloc((size_t)n * n, sizeof(double));
    Memcpy(avals, REAL(A), (size_t)n * n);
    F77_CALL(dgesv)(&n, &p, avals, &n, ipiv, REAL(B), &n, &info);
    if (info < 0)
	error(_("argument %d of Lapack routine %s had invalid value"),
	      -info, "dgesv");
    if (info > 0)
	error(_("Lapack routine %s: system is exactly singular: U[%d,%d] = 0"),
	      "dgesv", info, info);
    if(tol > 0) {
	anorm = F77_CALL(dlange)("1", &n, &n, REAL(A), &n, (double*) NULL);
	work = (double *) R_alloc(4*n, sizeof(double));
	F77_CALL(dgecon)("1", &n, avals, &n, &anorm, &rcond, work, ipiv, &info);
	if (rcond < tol)
	    error(_("system is computationally singular: reciprocal condition number = %g"),
		  rcond);
    }
    UNPROTECT(2);
    return B;
}

static SEXP La_dgeqp3(SEXP Ain)
{
    int i, m, n, *Adims, info, lwork;
    double *work, tmp;
    SEXP val, nm, jpvt, tau, rank, A;

    if (!(isMatrix(Ain) && isReal(Ain)))
	error(_("'a' must be a numeric matrix"));
    PROTECT(A = duplicate(Ain));
    Adims = INTEGER(coerceVector(getAttrib(A, R_DimSymbol), INTSXP));
    m = Adims[0];
    n = Adims[1];

    jpvt = PROTECT(allocVector(INTSXP, n));
    for (i = 0; i < n; i++) INTEGER(jpvt)[i] = 0;
    tau = PROTECT(allocVector(REALSXP, m < n ? m : n));
    lwork = -1;
    F77_CALL(dgeqp3)(&m, &n, REAL(A), &m, INTEGER(jpvt), REAL(tau),
		     &tmp, &lwork, &info);
    if (info < 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dgeqp3");
    lwork = (int) tmp;
    work = (double *) R_alloc(lwork, sizeof(double));
    F77_CALL(dgeqp3)(&m, &n, REAL(A), &m, INTEGER(jpvt), REAL(tau),
		     work, &lwork, &info);
    if (info < 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dgeqp3");
    val = PROTECT(allocVector(VECSXP, 4));
    nm = PROTECT(allocVector(STRSXP, 4));
    rank = PROTECT(ScalarInteger(m < n ? m : n));
    SET_STRING_ELT(nm, 0, mkChar("qr"));
    SET_STRING_ELT(nm, 1, mkChar("rank"));
    SET_STRING_ELT(nm, 2, mkChar("qraux"));
    SET_STRING_ELT(nm, 3, mkChar("pivot"));
    setAttrib(val, R_NamesSymbol, nm);
    SET_VECTOR_ELT(val, 0, A);
    SET_VECTOR_ELT(val, 1, rank);
    SET_VECTOR_ELT(val, 2, tau);
    SET_VECTOR_ELT(val, 3, jpvt);
    UNPROTECT(6);
    return val;
}

static SEXP qr_coef_real(SEXP Q, SEXP Bin)
{
    int n, nrhs, lwork, info, k, *Bdims, *Qdims;
    SEXP B, qr=VECTOR_ELT(Q, 0), tau=VECTOR_ELT(Q, 2);
    double *work, tmp;

    k = LENGTH(tau);
    if (!(isMatrix(Bin) && isReal(Bin)))
	error(_("'b' must be a numeric matrix"));

    PROTECT(B = duplicate(Bin));
    Qdims = INTEGER(coerceVector(getAttrib(qr, R_DimSymbol), INTSXP));
    n = Qdims[0];
    Bdims = INTEGER(coerceVector(getAttrib(B, R_DimSymbol), INTSXP));
    if(Bdims[0] != n)
	error(_("right-hand side should have %d not %d rows"), n, Bdims[0]);
    nrhs = Bdims[1];
    lwork = -1;
    F77_CALL(dormqr)("L", "T", &n, &nrhs, &k,
		     REAL(qr), &n, REAL(tau), REAL(B), &n,
		     &tmp, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dormqr");
    lwork = (int) tmp;
    work = (double *) R_alloc(lwork, sizeof(double));
    F77_CALL(dormqr)("L", "T", &n, &nrhs, &k,
		     REAL(qr), &n, REAL(tau), REAL(B), &n,
		     work, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dormqr");
    F77_CALL(dtrtrs)("U", "N", "N", &k, &nrhs,
		     REAL(qr), &n, REAL(B), &n, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dtrtrs");
    UNPROTECT(1);
    return B;
}

static SEXP qr_qy_real(SEXP Q, SEXP Bin, SEXP trans)
{
    int n, nrhs, lwork, info, k, *Bdims, *Qdims, tr;
    SEXP B, qr=VECTOR_ELT(Q, 0), tau=VECTOR_ELT(Q, 2);
    double *work, tmp;

    k = LENGTH(tau);
    if (!(isMatrix(Bin) && isReal(Bin)))
	error(_("'b' must be a numeric matrix"));
    tr = asLogical(trans);
    if(tr == NA_LOGICAL) error(_("invalid '%s' argument"), "trans");

    PROTECT(B = duplicate(Bin));
    Qdims = INTEGER(coerceVector(getAttrib(qr, R_DimSymbol), INTSXP));
    n = Qdims[0];
    Bdims = INTEGER(coerceVector(getAttrib(B, R_DimSymbol), INTSXP));
    if(Bdims[0] != n)
	error(_("right-hand side should have %d not %d rows"), n, Bdims[0]);
    nrhs = Bdims[1];
    lwork = -1;
    F77_CALL(dormqr)("L", tr ? "T" : "N", &n, &nrhs, &k,
		     REAL(qr), &n, REAL(tau), REAL(B), &n,
		     &tmp, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dormqr");
    lwork = (int) tmp;
    work = (double *) R_alloc(lwork, sizeof(double));
    F77_CALL(dormqr)("L", tr ? "T" : "N", &n, &nrhs, &k,
		     REAL(qr), &n, REAL(tau), REAL(B), &n,
		     work, &lwork, &info);
    if (info != 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dormqr");
    UNPROTECT(1);
    return B;
}

/* TODO : add  a *complex* version, using  LAPACK ZGETRF() */
static SEXP det_ge_real(SEXP Ain, SEXP logarithm)
{
    int i, n, *Adims, info, *jpvt, sign, useLog;
    double modulus = 0.0; /* -Wall */
    SEXP val, nm, A;

    if (!(isMatrix(Ain) && isReal(Ain)))
	error(_("'a' must be a numeric matrix"));
    useLog = asLogical(logarithm);
    if (useLog == NA_LOGICAL) error(_("argument 'logarithm' must be logical"));
    PROTECT(A = duplicate(Ain));
    Adims = INTEGER(coerceVector(getAttrib(A, R_DimSymbol), INTSXP));
    n = Adims[0];
    if (Adims[1] != n)
	error(_("'a' must be a square matrix"));
    jpvt = (int *) R_alloc(n, sizeof(int));
    F77_CALL(dgetrf)(&n, &n, REAL(A), &n, jpvt, &info);
    sign = 1;
    if (info < 0)
	error(_("error code %d from Lapack routine '%s'"), info, "dgetrf");
    else if (info > 0) { /* Singular matrix:  U[i,i] (i := info) is 0 */
	/*warning("Lapack dgetrf(): singular matrix: U[%d,%d]=0", info,info);*/
	modulus =
#ifdef _not_quite_/* unfortunately does not work -- FIXME */
	    (ISNAN(REAL(A)[(info-1)*n + (info-1)])) /* pivot is NA/NaN */
	    ? R_NaN :
#endif
	    (useLog ? R_NegInf : 0.);
    }
    else {
	for (i = 0; i < n; i++) if (jpvt[i] != (i + 1))
	    sign = -sign;
	if (useLog) {
	    modulus = 0.0;
	    size_t N1 = n+1;
	    for (i = 0; i < n; i++) {
		double dii = REAL(A)[i * N1]; /* ith diagonal element */
		modulus += log(dii < 0 ? -dii : dii);
		if (dii < 0) sign = -sign;
	    }
	} else {
	    modulus = 1.0;
	    size_t N1 = n+1;
	    for (i = 0; i < n; i++)
		modulus *= REAL(A)[i * N1];
	    if (modulus < 0) {
		modulus = -modulus;
		sign = -sign;
	    }
	}
    }
    val = PROTECT(allocVector(VECSXP, 2));
    nm = PROTECT(allocVector(STRSXP, 2));
    SET_STRING_ELT(nm, 0, mkChar("modulus"));
    SET_STRING_ELT(nm, 1, mkChar("sign"));
    setAttrib(val, R_NamesSymbol, nm);
    SET_VECTOR_ELT(val, 0, ScalarReal(modulus));
    setAttrib(VECTOR_ELT(val, 0), install("logarithm"), ScalarLogical(useLog));
    SET_VECTOR_ELT(val, 1, ScalarInteger(sign));
    setAttrib(val, R_ClassSymbol, ScalarString(mkChar("det")));
    UNPROTECT(3);
    return val;
}

static SEXP mod_do_lapack(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans = R_NilValue;

    switch(PRIMVAL(op)) {
    case 0: ans = La_zgeqp3(CAR(args)); break;
    case 1: ans = La_rs(CAR(args), CADR(args)); break;
    case 2: ans = La_rs_cmplx(CAR(args), CADR(args)); break;
    case 3: ans = La_rg(CAR(args), CADR(args)); break;
    case 41: ans = La_rg_cmplx(CAR(args), CADR(args)); break;
    case 5: ans = La_rs(CAR(args), CADR(args)); break;
    case 51: ans = La_rs_cmplx(CAR(args), CADR(args)); break;
    case 6: ans = La_dlange(CAR(args), CADR(args)); break;
    case 7: ans = La_dgecon(CAR(args), CADR(args)); break;
    case 8: ans = La_dtrcon(CAR(args), CADR(args)); break;
    case 9: ans = La_zgecon(CAR(args), CADR(args)); break;
    case 10: ans = La_ztrcon(CAR(args), CADR(args)); break;
    case 11: ans = La_zgesv(CAR(args), CADR(args)); break;

    case 100: ans = La_dgesv(CAR(args), CADR(args), CADDR(args)); break;
    case 101: ans = La_dgeqp3(CAR(args)); break;

    case 200: ans = La_chol(CAR(args)); break;
    case 201: ans = La_chol2inv(CAR(args), CADR(args)); break;

    case 300: ans = qr_coef_real(CAR(args), CADR(args)); break;
    case 301: ans = qr_qy_real(CAR(args), CADR(args), CADDR(args)); break;
    case 302: ans = det_ge_real(CAR(args), CADR(args)); break;
    case 303: ans = qr_coef_cmplx(CAR(args), CADR(args)); break;
    case 304: ans = qr_qy_cmplx(CAR(args), CADR(args), CADDR(args)); break;

    case 400:
    {
	SEXP a1, a2, a3, a4, a5, a6;
	a1 = CAR(args); args = CDR(args);
	a2 = CAR(args); args = CDR(args);
	a3 = CAR(args); args = CDR(args);
	a4 = CAR(args); args = CDR(args);
	a5 = CAR(args); args = CDR(args);
	a6 = CAR(args); args = CDR(args);
	ans = La_svd(a1, a2, a3, a4, a5, a6, CAR(args));
	break;
    }
    case 401:
    {
	SEXP a1, a2, a3, a4, a5;
	a1 = CAR(args); args = CDR(args);
	a2 = CAR(args); args = CDR(args);
	a3 = CAR(args); args = CDR(args);
	a4 = CAR(args); args = CDR(args);
	a5 = CAR(args); args = CDR(args);
	ans = La_svd_cmplx(a1, a2, a3, a4, a5, CAR(args));
	break;
    }
    }

    return ans;
}


/* ------------------------------------------------------------ */

#include <Rmodules/Rlapack.h>
#include <R_ext/Rdynload.h>

void
R_init_lapack(DllInfo *info)
{
    R_LapackRoutines *tmp;
    tmp = Calloc(1, R_LapackRoutines);

    tmp->do_lapack = mod_do_lapack;
    R_setLapackRoutines(tmp);
}
