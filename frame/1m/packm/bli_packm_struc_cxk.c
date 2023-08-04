/*

   BLIS
   An object-based framework for developing high-performance BLAS-like
   libraries.

   Copyright (C) 2014, The University of Texas at Austin

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    - Neither the name(s) of the copyright holder(s) nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "blis.h"

#undef  GENTFUNC2R
#define GENTFUNC2R( ctypec, ctypep, ctypep_r, chc, chp, chp_r, varname ) \
\
void PASTEMAC2(chc,chp,varname) \
     ( \
             struc_t strucc, \
             diag_t  diagc, \
             uplo_t  uploc, \
             conj_t  conjc, \
             pack_t  schema, \
             bool    invdiag, \
             dim_t   panel_dim, \
             dim_t   panel_len, \
             dim_t   panel_dim_max, \
             dim_t   panel_len_max, \
             dim_t   panel_dim_off, \
             dim_t   panel_len_off, \
             dim_t   panel_bcast, \
       const void*   kappa, \
       const void*   c, inc_t incc, inc_t ldc, \
             void*   p,             inc_t ldp, \
       const void*   params, \
       const cntx_t* cntx \
     ) \
{ \
	cntl_t* cntl          = ( cntl_t* )params; \
\
	num_t   dt_c          = PASTEMAC(chc,type); \
	num_t   dt_p          = PASTEMAC(chp,type); \
	num_t   dt_pr         = PASTEMAC(chp_r,type); \
\
	dim_t   panel_len_pad = panel_len_max - panel_len; \
\
	dim_t   panel_dim_r   = bli_packm_def_cntl_bmult_m_def( cntl ); \
	dim_t   ldp_r         = ldp; \
\
	ukr2_t   cxk_ker_id   = BLIS_PACKM_KER; \
	ukr2_t   cxc_ker_id   = BLIS_PACKM_DIAG_KER; \
\
	if ( bli_is_1m_packed( schema ) ) \
	{ \
		cxk_ker_id = BLIS_PACKM_1ER_KER; \
		cxc_ker_id = BLIS_PACKM_DIAG_1ER_KER; \
	} \
	else if ( dt_p != dt_pr ) \
	{ \
		/* We will zero out portions of the triangular matrix using a real-domain macro */ \
		panel_dim_r *= 2; \
		ldp_r *= 2; \
	} \
\
	packm_cxk_ker_ft      f_cxk  = bli_cntx_get_ukr2_dt( dt_c, dt_p, cxk_ker_id, cntx ); \
	packm_cxc_diag_ker_ft f_cxc  = bli_cntx_get_ukr2_dt( dt_c, dt_p, cxc_ker_id, cntx ); \
\
	/* For general matrices, pack and return early */ \
	if ( bli_is_general( strucc ) ) \
	{ \
		f_cxk \
		( \
		  conjc, \
		  schema, \
		  panel_dim, \
		  panel_dim_max, \
		  panel_bcast, \
		  panel_len, \
		  panel_len_max, \
		  kappa, \
		  c, incc, ldc, \
		  p,       ldp, \
		  params, \
		  cntx \
		); \
		return; \
	} \
\
	/* Sanity check. Diagonals should not intersect the short end of
	   a micro-panel. If they do, then somehow the constraints on
	   cache blocksizes being a whole multiple of the register
	   blocksizes was somehow violated. */ \
	doff_t diagoffc = panel_dim_off - panel_len_off; \
	if ( (          -panel_dim < diagoffc && diagoffc <         0 ) || \
		 ( panel_len-panel_dim < diagoffc && diagoffc < panel_len ) ) \
		bli_check_error_code( BLIS_NOT_YET_IMPLEMENTED ); \
\
	/* For triangular, symmetric, and hermitian matrices we need to consider
	   three parts. */ \
\
	/* Pack to p10. */ \
	if ( 0 < diagoffc ) \
	{ \
		dim_t   p10_len     = bli_min( diagoffc, panel_len ); \
		dim_t   p10_len_max = p10_len == panel_len ? panel_len_max : p10_len; \
		ctypep* p10         = ( ctypep* )p; \
		conj_t  conjc10     = conjc; \
		ctypec* c10         = ( ctypec* )c; \
		inc_t   incc10      = incc; \
		inc_t   ldc10       = ldc; \
\
		if ( bli_is_upper( uploc ) ) \
		{ \
			bli_reflect_to_stored_part( diagoffc, c10, incc10, ldc10 ); \
\
			if ( bli_is_hermitian( strucc ) ) \
				bli_toggle_conj( &conjc10 ); \
		} \
\
		/* If we are referencing the unstored part of a triangular matrix,
		   explicitly store zeros */ \
		if ( bli_is_upper( uploc ) && bli_is_triangular( strucc ) ) \
		{ \
			PASTEMAC(chp_r,set0s_mxn) \
			( \
			  panel_dim_r, \
			  p10_len_max * ( bli_is_1m_packed( schema ) ? 2 : 1), \
			  ( ctypep_r* )p10, 1, ldp_r \
			); \
		} \
		else \
		{ \
			f_cxk \
			( \
			  conjc10, \
			  schema, \
			  panel_dim, \
			  panel_dim_max, \
			  panel_bcast, \
			  p10_len, \
			  p10_len_max, \
			  kappa, \
			  c10, incc10, ldc10, \
			  p10,         ldp, \
			  params, \
			  cntx \
			); \
		} \
	} \
\
	/* Pack to p11. */ \
	if ( 0 <= diagoffc && diagoffc + panel_dim <= panel_len ) \
	{ \
		dim_t  i           = diagoffc; \
		dim_t  p11_len_max = panel_dim + ( diagoffc + panel_dim == panel_len \
		                                   ? panel_len_pad : 0 ); \
		ctypep* p11         = ( ctypep* )p + i * ldp; \
		conj_t  conjc11     = conjc; \
		ctypec* c11         = ( ctypec* )c + i * ldc; \
		inc_t   incc11      = incc; \
		inc_t   ldc11       = ldc; \
\
		f_cxc \
		( \
		  strucc, \
		  diagc, \
		  uploc, \
		  conjc11, \
		  schema, \
		  invdiag, \
		  panel_dim, \
		  panel_dim_max, \
		  panel_bcast, \
		  p11_len_max, \
		  kappa, \
		  c11, incc11, ldc11, \
		  p11,         ldp, \
		  params, \
		  cntx \
		); \
	} \
\
	/* Pack to p12. */ \
	if ( diagoffc + panel_dim < panel_len ) \
	{ \
		dim_t   i           = bli_max( 0, diagoffc + panel_dim ); \
		dim_t   p12_len     = panel_len - i; \
		/* If we are packing p12, then it is always the last partial block
		   and so we should make sure to pad with zeros if necessary. */ \
		dim_t   p12_len_max = p12_len + panel_len_pad; \
		ctypep*  p12        = ( ctypep* )p + i * ldp; \
		conj_t  conjc12     = conjc; \
		ctypec*  c12        = ( ctypec* )c + i * ldc; \
		inc_t   incc12      = incc; \
		inc_t   ldc12       = ldc; \
\
		if ( bli_is_lower( uploc ) ) \
		{ \
			bli_reflect_to_stored_part( diagoffc - i, c12, incc12, ldc12 ); \
\
			if ( bli_is_hermitian( strucc ) ) \
				bli_toggle_conj( &conjc12 ); \
		} \
\
		/* If we are referencing the unstored part of a triangular matrix,
		   explicitly store zeros */ \
		if ( bli_is_lower( uploc ) && bli_is_triangular( strucc ) ) \
		{ \
			PASTEMAC(chp_r,set0s_mxn) \
			( \
			  panel_dim_r, \
			  p12_len_max * ( bli_is_1m_packed( schema ) ? 2 : 1), \
			  ( ctypep_r* )p12, 1, ldp_r \
			); \
		} \
		else \
		{ \
			f_cxk \
			( \
			  conjc12, \
			  schema, \
			  panel_dim, \
			  panel_dim_max, \
			  panel_bcast, \
			  p12_len, \
			  p12_len_max, \
			  kappa, \
			  c12, incc12, ldc12, \
			  p12,         ldp, \
			  params, \
			  cntx \
			); \
		} \
	} \
}

INSERT_GENTFUNC2R_BASIC( packm_struc_cxk )
INSERT_GENTFUNC2R_MIX_DP( packm_struc_cxk )

