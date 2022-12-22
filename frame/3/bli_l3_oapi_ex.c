/*

   BLIS
   An object-based framework for developing high-performance BLAS-like
   libraries.

   Copyright (C) 2021, The University of Texas at Austin

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

//
// Define object-based interfaces (expert).
//

err_t bli_l3_return_early_if_trivial
      (
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const obj_t*  beta,
       const obj_t*  c
      )
{
	// If C has a zero dimension, return early.
	if ( bli_obj_has_zero_dim( c ) )
        return BLIS_SUCCESS;

	// If alpha is zero, or if A or B has a zero dimension, scale C by beta
	// and return early.
	if ( bli_obj_equals( alpha, &BLIS_ZERO ) ||
	     bli_obj_has_zero_dim( a ) ||
	     bli_obj_has_zero_dim( b ) )
	{
		bli_scalm( beta, c );
		return BLIS_SUCCESS;
	}

    return BLIS_FAILURE;
}

// If a sandbox was enabled, we forgo defining bli_gemm_ex() since it will be
// defined in the sandbox environment.
#ifndef BLIS_ENABLE_SANDBOX

void PASTEMAC(gemm,BLIS_OAPI_EX_SUF)
     (
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check the operands.
	if ( bli_error_checking_is_enabled() )
		bli_gemm_check( alpha, a, b, beta, c, cntx );

	// Check for zero dimensions, alpha == 0, or other conditions which
    // mean that we don't actually have to perform a full l3 operation.
	if ( bli_l3_return_early_if_trivial( alpha, a, b, beta, c ) == BLIS_SUCCESS)
		return;

	// Execute the small/unpacked oapi handler. If it finds that the problem
	// does not fall within the thresholds that define "small", or for some
	// other reason decides not to use the small/unpacked implementation,
	// the function returns with BLIS_FAILURE, which causes execution to
	// proceed towards the conventional implementation.
	if ( bli_gemmsup( alpha, a, b, beta, c, cntx, rntm ) == BLIS_SUCCESS )
		return;

	// Initialize a local runtime with global settings if necessary. Note
	// that in the case that a runtime is passed in, we make a local copy.
	rntm_t rntm_l;
	if ( rntm == NULL ) { bli_rntm_init_from_global( &rntm_l ); }
	else                { rntm_l = *rntm;                       }

	// Default to using native execution.
	num_t dt = bli_obj_dt( c );
	ind_t im = BLIS_NAT;

	// If each matrix operand has a complex storage datatype, try to get an
	// induced method (if one is available and enabled). NOTE: Allowing
	// precisions to vary while using 1m, which is what we do here, is unique
	// to gemm; other level-3 operations use 1m only if all storage datatypes
	// are equal (and they ignore the computation precision).
	if ( bli_obj_is_complex( c ) &&
	     bli_obj_is_complex( a ) &&
	     bli_obj_is_complex( b ) )
	{
		// Find the highest priority induced method that is both enabled and
		// available for the current operation. (If an induced method is
		// available but not enabled, or simply unavailable, BLIS_NAT will
		// be returned here.)
		im = bli_gemmind_find_avail( dt );
	}

	// If necessary, obtain a valid context from the gks using the induced
	// method id determined above.
	if ( cntx == NULL ) cntx = bli_gks_query_ind_cntx( im );

#if 0
#ifdef BLIS_ENABLE_SMALL_MATRIX
	// Only handle small problems separately for homogeneous datatypes.
	if ( bli_obj_dt( a ) == bli_obj_dt( b ) &&
	     bli_obj_dt( a ) == bli_obj_dt( c ) &&
	     bli_obj_comp_prec( c ) == bli_obj_prec( c ) )
	{
		err_t status = bli_gemm_small( alpha, a, b, beta, c, cntx, cntl );
		if ( status == BLIS_SUCCESS ) return;
	}
#endif
#endif

	// Alias A, B, and C in case we need to apply transformations.
	obj_t a_local;
	obj_t b_local;
	obj_t c_local;
	bli_obj_alias_and_reset_origin( a, &a_local );
	bli_obj_alias_and_reset_origin( b, &b_local );
	bli_obj_alias_and_reset_origin( c, &c_local );

	// An optimization: If C is stored by rows and the micro-kernel prefers
	// contiguous columns, or if C is stored by columns and the micro-kernel
	// prefers contiguous rows, transpose the entire operation to allow the
	// micro-kernel to access elements of C in its preferred manner.
	if ( bli_cntx_dislikes_storage_of( &c_local, BLIS_GEMM_VIR_UKR, cntx ) )
	{
		bli_obj_swap( &a_local, &b_local );

		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

	// Set the pack schemas within the objects.
	bli_l3_set_schemas( &a_local, &b_local, &c_local, cntx );

#ifdef BLIS_ENABLE_GEMM_MD
	cntx_t cntx_local;

	// If any of the storage datatypes differ, or if the computation precision
	// differs from the storage precision of C, utilize the mixed datatype
	// code path.
	// NOTE: If we ever want to support the caller setting the computation
	// domain explicitly, we will need to check the computation dt against the
	// storage dt of C (instead of the computation precision against the
	// storage precision of C).
	if ( bli_obj_dt( &c_local ) != bli_obj_dt( &a_local ) ||
	     bli_obj_dt( &c_local ) != bli_obj_dt( &b_local ) ||
	     bli_obj_comp_prec( &c_local ) != bli_obj_prec( &c_local ) )
	{
		// Handle mixed datatype cases in bli_gemm_md(), which may modify
		// the objects or the context. (If the context is modified, cntx
		// is adjusted to point to cntx_local.)
		bli_gemm_md( &a_local, &b_local, beta, &c_local, &cntx_local, &cntx );
	}
#endif

	// Next, we handle the possibility of needing to typecast alpha to the
	// computation datatype and/or beta to the storage datatype of C.

	// Attach alpha to B, and in the process typecast alpha to the target
	// datatype of the matrix (which in this case is equal to the computation
	// datatype).
	bli_obj_scalar_attach( BLIS_NO_CONJUGATE, alpha, &b_local );

	// Attach beta to C, and in the process typecast beta to the target
	// datatype of the matrix (which in this case is equal to the storage
	// datatype of C).
	bli_obj_scalar_attach( BLIS_NO_CONJUGATE, beta,  &c_local );

	// Change the alpha and beta pointers to BLIS_ONE since the values have
	// now been typecast and attached to the matrices above.
	alpha = &BLIS_ONE;
	beta  = &BLIS_ONE;

	// Parse and interpret the contents of the rntm_t object to properly
	// set the ways of parallelism for each loop, and then make any
	// additional modifications necessary for the current operation.
	bli_rntm_set_ways_for_op
	(
	  BLIS_GEMM,
	  BLIS_LEFT, // ignored for gemm/hemm/symm
	  bli_obj_length( &c_local ),
	  bli_obj_width( &c_local ),
	  bli_obj_width( &a_local ),
	  &rntm_l
	);

	      obj_t* cp    = &c_local;
	const obj_t* betap = beta;

#ifdef BLIS_ENABLE_GEMM_MD
#ifdef BLIS_ENABLE_GEMM_MD_EXTRA_MEM
	// If any of the following conditions are met, create a temporary matrix
	// conformal to C into which we will accumulate the matrix product:
	// - the storage precision of C differs from the computation precision;
	// - the domains are mixed as crr;
	// - the storage format of C does not match the preferred orientation
	//   of the ccr or crc cases.
	// Then, after the computation is complete, this matrix will be copied
	// or accumulated back to C.
	const bool is_ccr_mismatch =
	             ( bli_gemm_md_is_ccr( &a_local, &b_local, &c_local ) &&
                   !bli_obj_is_col_stored( &c_local ) );
	const bool is_crc_mismatch =
	             ( bli_gemm_md_is_crc( &a_local, &b_local, &c_local ) &&
                   !bli_obj_is_row_stored( &c_local ) );

	obj_t ct;
	bool  use_ct = FALSE;

	// FGVZ: Consider adding another guard here that only creates and uses a
	// temporary matrix for accumulation if k < c * kc, where c is some small
	// constant like 2. And don't forget to use the same conditional for the
	// castm() and free() at the end.
	if (
	     bli_obj_prec( &c_local ) != bli_obj_comp_prec( &c_local ) ||
	     bli_gemm_md_is_crr( &a_local, &b_local, &c_local ) ||
	     is_ccr_mismatch ||
	     is_crc_mismatch
	   )
	{
		use_ct = TRUE;
	}

	// If we need a temporary matrix conformal to C for whatever reason,
	// we create it and prepare to use it now.
	if ( use_ct )
	{
		const dim_t m     = bli_obj_length( &c_local );
		const dim_t n     = bli_obj_width( &c_local );
		      inc_t rs    = bli_obj_row_stride( &c_local );
		      inc_t cs    = bli_obj_col_stride( &c_local );

		      num_t dt_ct = bli_obj_domain( &c_local ) |
		                    bli_obj_comp_prec( &c_local );

		// When performing the crr case, accumulate to a contiguously-stored
		// real matrix so we do not have to repeatedly update C with general
		// stride.
		if ( bli_gemm_md_is_crr( &a_local, &b_local, &c_local ) )
			dt_ct = BLIS_REAL | bli_obj_comp_prec( &c_local );

		// When performing the mismatched ccr or crc cases, now is the time
		// to specify the appropriate storage so the gemm_md_c2r_ref() virtual
		// microkernel can output directly to C (instead of using a temporary
		// microtile).
		if      ( is_ccr_mismatch ) { rs = 1; cs = m; }
		else if ( is_crc_mismatch ) { rs = n; cs = 1; }

		bli_obj_create( dt_ct, m, n, rs, cs, &ct );

		const num_t dt_exec = bli_obj_exec_dt( &c_local );
		const num_t dt_comp = bli_obj_comp_dt( &c_local );

		bli_obj_set_target_dt( dt_ct, &ct );
		bli_obj_set_exec_dt( dt_exec, &ct );
		bli_obj_set_comp_dt( dt_comp, &ct );

		// A naive approach would cast C to the comptuation datatype,
		// compute with beta, and then cast the result back to the
		// user-provided output matrix. However, we employ a different
		// approach that halves the number of memops on C (or its
		// typecast temporary) by writing the A*B product directly to
		// temporary storage, and then using xpbym to scale the
		// output matrix by beta and accumulate/cast the A*B product.
		//bli_castm( &c_local, &ct );
		betap = &BLIS_ZERO;

		cp = &ct;
	}
#endif
#endif

	// This is part of a hack to support mixed domain in bli_gemm_front().
	// Sometimes we need to specify a non-standard schema for A and B, and
	// we decided to transmit them via the schema field in the obj_t's
	// rather than pass them in as function parameters. Once the values
	// have been read, we immediately reset them back to their expected
	// values for unpacked objects.
	pack_t schema_a = bli_obj_pack_schema( &a_local );
	pack_t schema_b = bli_obj_pack_schema( &b_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &a_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &b_local );

	cntl_t* cntl = bli_gemm_cntl_create
	(
	  NULL,
	  BLIS_GEMM,
	  schema_a,
	  schema_b,
	  bli_obj_ker_fn( c )
	);

	// Invoke the internal back-end via the thread handler.
	bli_l3_thread_decorator
	(
	  bli_l3_int,
	  alpha,
	  &a_local,
	  &b_local,
	  betap,
	  cp,
	  cntx,
	  cntl,
	  &rntm_l
	);

	// Free the thread's local control tree.
	bli_cntl_free( NULL, cntl );

#ifdef BLIS_ENABLE_GEMM_MD
#ifdef BLIS_ENABLE_GEMM_MD_EXTRA_MEM
	// If we created a temporary matrix conformal to C for whatever reason,
	// we copy/accumulate the result back to C and then release the object.
	if ( use_ct )
	{
		obj_t beta_local;

		bli_obj_scalar_detach( &c_local, &beta_local );

		//bli_castnzm( &ct, &c_local );
		bli_xpbym( &ct, &beta_local, &c_local );

		bli_obj_free( &ct );
	}
#endif
#endif
}

#endif


void PASTEMAC(gemmt,BLIS_OAPI_EX_SUF)
     (
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check the operands.
	if ( bli_error_checking_is_enabled() )
		bli_gemmt_check( alpha, a, b, beta, c, cntx );

	// Check for zero dimensions, alpha == 0, or other conditions which
    // mean that we don't actually have to perform a full l3 operation.
	if ( bli_l3_return_early_if_trivial( alpha, a, b, beta, c ) == BLIS_SUCCESS)
		return;

	// Initialize a local runtime with global settings if necessary. Note
	// that in the case that a runtime is passed in, we make a local copy.
	rntm_t rntm_l;
	if ( rntm == NULL ) { bli_rntm_init_from_global( &rntm_l ); }
	else                { rntm_l = *rntm;                       }

	// Default to using native execution.
	num_t dt = bli_obj_dt( c );
	ind_t im = BLIS_NAT;

	// If all matrix operands are complex and of the same storage datatype, try
	// to get an induced method (if one is available and enabled).
	if ( bli_obj_dt( a ) == bli_obj_dt( c ) &&
	     bli_obj_dt( b ) == bli_obj_dt( c ) &&
	     bli_obj_is_complex( c ) )
	{
		// Find the highest priority induced method that is both enabled and
		// available for the current operation. (If an induced method is
		// available but not enabled, or simply unavailable, BLIS_NAT will
		// be returned here.)
		im = bli_gemmtind_find_avail( dt );
	}

	// If necessary, obtain a valid context from the gks using the induced
	// method id determined above.
	if ( cntx == NULL ) cntx = bli_gks_query_ind_cntx( im );

	// Alias A, B, and C in case we need to apply transformations.
	obj_t a_local;
	obj_t b_local;
	obj_t c_local;
	bli_obj_alias_and_reset_origin( a, &a_local );
	bli_obj_alias_and_reset_origin( b, &b_local );
	bli_obj_alias_and_reset_origin( c, &c_local );

	// An optimization: If C is stored by rows and the micro-kernel prefers
	// contiguous columns, or if C is stored by columns and the micro-kernel
	// prefers contiguous rows, transpose the entire operation to allow the
	// micro-kernel to access elements of C in its preferred manner.
	if ( bli_cntx_dislikes_storage_of( &c_local, BLIS_GEMM_VIR_UKR, cntx ) )
	{
		bli_obj_swap( &a_local, &b_local );

		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

	// Set the pack schemas within the objects, as appropriate.
	bli_l3_set_schemas( &a_local, &b_local, &c_local, cntx );

	// Parse and interpret the contents of the rntm_t object to properly
	// set the ways of parallelism for each loop, and then make any
	// additional modifications necessary for the current operation.
	bli_rntm_set_ways_for_op
	(
	  BLIS_GEMM,
	  BLIS_LEFT, // ignored for gemm/hemm/symm/gemmt
	  bli_obj_length( &c_local ),
	  bli_obj_width( &c_local ),
	  bli_obj_width( &a_local ),
	  &rntm_l
	);

	// This is part of a hack to support mixed domain in bli_gemm_front().
	// Sometimes we need to specify a non-standard schema for A and B, and
	// we decided to transmit them via the schema field in the obj_t's
	// rather than pass them in as function parameters. Once the values
	// have been read, we immediately reset them back to their expected
	// values for unpacked objects.
	pack_t schema_a = bli_obj_pack_schema( &a_local );
	pack_t schema_b = bli_obj_pack_schema( &b_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &a_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &b_local );

	cntl_t* cntl = bli_gemm_cntl_create
	(
	  NULL,
	  BLIS_GEMMT,
	  schema_a,
	  schema_b,
	  bli_obj_ker_fn( c )
	);

	// Invoke the internal back-end via the thread handler.
	bli_l3_thread_decorator
	(
	  bli_l3_int,
	  alpha,
	  &a_local,
	  &b_local,
	  beta,
	  &c_local,
	  cntx,
	  cntl,
	  &rntm_l
	);

	// Free the thread's local control tree.
	bli_cntl_free( NULL, cntl );
}


void PASTEMAC(her2k,BLIS_OAPI_EX_SUF)
     (
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check parameters.
	if ( bli_error_checking_is_enabled() )
		bli_her2k_check( alpha, a, b, beta, c, cntx );

	obj_t alphah;
	bli_obj_alias_to( alpha, &alphah );
	bli_obj_toggle_conj( &alphah );

	obj_t ah;
	bli_obj_alias_to( a, &ah );
	bli_obj_toggle_trans( &ah );
	bli_obj_toggle_conj( &ah );

	obj_t bh;
	bli_obj_alias_to( b, &bh );
	bli_obj_toggle_trans( &bh );
	bli_obj_toggle_conj( &bh );

	// Invoke gemmt twice, using beta only the first time.
	PASTEMAC(gemmt,BLIS_OAPI_EX_SUF)(   alpha, a, &bh,      beta, c, cntx, rntm );
	PASTEMAC(gemmt,BLIS_OAPI_EX_SUF)( &alphah, b, &ah, &BLIS_ONE, c, cntx, rntm );

	// The Hermitian rank-2k product was computed as alpha*A*B'+alpha'*B*A', even for
	// the diagonal elements. Mathematically, the imaginary components of
	// diagonal elements of a Hermitian rank-2k product should always be
	// zero. However, in practice, they sometimes accumulate meaningless
	// non-zero values. To prevent this, we explicitly set those values
	// to zero before returning.
	bli_setid( &BLIS_ZERO, c );
}


void PASTEMAC(syr2k,BLIS_OAPI_EX_SUF)
     (
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check parameters.
	if ( bli_error_checking_is_enabled() )
		bli_syr2k_check( alpha, a, b, beta, c, cntx );

	obj_t at;
	bli_obj_alias_to( a, &at );
	bli_obj_toggle_trans( &at );

	obj_t bt;
	bli_obj_alias_to( b, &bt );
	bli_obj_toggle_trans( &bt );

	// Invoke gemmt twice, using beta only the first time.
	PASTEMAC(gemmt,BLIS_OAPI_EX_SUF)( alpha, a, &bt,      beta, c, cntx, rntm );
	PASTEMAC(gemmt,BLIS_OAPI_EX_SUF)( alpha, b, &at, &BLIS_ONE, c, cntx, rntm );
}


void PASTEMAC(hemm,BLIS_OAPI_EX_SUF)
     (
             side_t  side,
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check the operands.
	if ( bli_error_checking_is_enabled() )
		bli_hemm_check( side, alpha, a, b, beta, c, cntx );

	// Check for zero dimensions, alpha == 0, or other conditions which
    // mean that we don't actually have to perform a full l3 operation.
	if ( bli_l3_return_early_if_trivial( alpha, a, b, beta, c ) == BLIS_SUCCESS)
		return;

	// Initialize a local runtime with global settings if necessary. Note
	// that in the case that a runtime is passed in, we make a local copy.
	rntm_t rntm_l;
	if ( rntm == NULL ) { bli_rntm_init_from_global( &rntm_l ); }
	else                { rntm_l = *rntm;                       }

	// Default to using native execution.
	num_t dt = bli_obj_dt( c );
	ind_t im = BLIS_NAT;

	// If all matrix operands are complex and of the same storage datatype, try
	// to get an induced method (if one is available and enabled).
	if ( bli_obj_dt( a ) == bli_obj_dt( c ) &&
	     bli_obj_dt( b ) == bli_obj_dt( c ) &&
	     bli_obj_is_complex( c ) )
	{
		// Find the highest priority induced method that is both enabled and
		// available for the current operation. (If an induced method is
		// available but not enabled, or simply unavailable, BLIS_NAT will
		// be returned here.)
		im = bli_hemmind_find_avail( dt );
	}

	// If necessary, obtain a valid context from the gks using the induced
	// method id determined above.
	if ( cntx == NULL ) cntx = bli_gks_query_ind_cntx( im );

	// Alias A, B, and C in case we need to apply transformations.
	obj_t a_local;
	obj_t b_local;
	obj_t c_local;
	bli_obj_alias_and_reset_origin( a, &a_local );
	bli_obj_alias_and_reset_origin( b, &b_local );
	bli_obj_alias_and_reset_origin( c, &c_local );

#ifdef BLIS_DISABLE_HEMM_RIGHT
	// NOTE: This case casts right-side hemm in terms of left side. This is
	// necessary when the current subconfiguration uses a gemm microkernel
	// that assumes that the packing kernel will have already duplicated
	// (broadcast) element of B in the packed copy of B. Supporting
	// duplication within the logic that packs micropanels from Hermitian/
	// matrices would be ugly, and so we simply don't support it. As a
	// consequence, those subconfigurations need a way to force the Hermitian
	// matrix to be on the left (and thus the general matrix to the on the
	// right). So our solution is that in those cases, the subconfigurations
	// simply #define BLIS_DISABLE_HEMM_RIGHT.

	// NOTE: This case casts right-side hemm in terms of left side. This can
	// lead to the microkernel being executed on an output matrix with the
	// microkernel's general stride IO case (unless the microkernel supports
	// both both row and column IO cases as well).

	// If A is being multiplied from the right, transpose all operands
	// so that we can perform the computation as if A were being multiplied
	// from the left.
	if ( bli_is_right( side ) )
	{
		bli_toggle_side( &side );
		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

#else
	// NOTE: This case computes right-side hemm/symm natively by packing
	// elements of the Hermitian/symmetric matrix A to micropanels of the
	// right-hand packed matrix operand "B", and elements of the general
	// matrix B to micropanels of the left-hand packed matrix operand "A".
	// This code path always gives us the opportunity to transpose the
	// entire operation so that the effective storage format of the output
	// matrix matches the microkernel's output preference. Thus, from a
	// performance perspective, this case is preferred.

	// An optimization: If C is stored by rows and the micro-kernel prefers
	// contiguous columns, or if C is stored by columns and the micro-kernel
	// prefers contiguous rows, transpose the entire operation to allow the
	// micro-kernel to access elements of C in its preferred manner.
	//if ( !bli_obj_is_1x1( &c_local ) ) // NOTE: This conditional should NOT
	                                     // be enabled. See issue #342 comments.
	if ( bli_cntx_dislikes_storage_of( &c_local, BLIS_GEMM_VIR_UKR, cntx ) )
	{
		bli_toggle_side( &side );
		bli_obj_toggle_conj( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

	// If the Hermitian/symmetric matrix A is being multiplied from the right,
	// swap A and B so that the Hermitian/symmetric matrix will actually be on
	// the right.
	if ( bli_is_right( side ) )
	{
		bli_obj_swap( &a_local, &b_local );
	}
#endif

	// Set the pack schemas within the objects.
	bli_l3_set_schemas( &a_local, &b_local, &c_local, cntx );

	// Parse and interpret the contents of the rntm_t object to properly
	// set the ways of parallelism for each loop, and then make any
	// additional modifications necessary for the current operation.
	bli_rntm_set_ways_for_op
	(
	  BLIS_HEMM,
	  BLIS_LEFT, // ignored for gemm/hemm/symm
	  bli_obj_length( &c_local ),
	  bli_obj_width( &c_local ),
	  bli_obj_width( &a_local ),
	  &rntm_l
	);

	// This is part of a hack to support mixed domain in bli_gemm_front().
	// Sometimes we need to specify a non-standard schema for A and B, and
	// we decided to transmit them via the schema field in the obj_t's
	// rather than pass them in as function parameters. Once the values
	// have been read, we immediately reset them back to their expected
	// values for unpacked objects.
	pack_t schema_a = bli_obj_pack_schema( &a_local );
	pack_t schema_b = bli_obj_pack_schema( &b_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &a_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &b_local );

	cntl_t* cntl = bli_gemm_cntl_create
	(
	  NULL,
	  BLIS_GEMM,
	  schema_a,
	  schema_b,
	  bli_obj_ker_fn( c )
	);

	// Invoke the internal back-end.
	bli_l3_thread_decorator
	(
	  bli_l3_int,
	  alpha,
	  &a_local,
	  &b_local,
	  beta,
	  &c_local,
	  cntx,
	  cntl,
	  &rntm_l
	);

	// Free the thread's local control tree.
	bli_cntl_free( NULL, cntl );
}


void PASTEMAC(symm,BLIS_OAPI_EX_SUF)
     (
             side_t  side,
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check the operands.
	if ( bli_error_checking_is_enabled() )
		bli_symm_check( side, alpha, a, b, beta, c, cntx );

	// Check for zero dimensions, alpha == 0, or other conditions which
    // mean that we don't actually have to perform a full l3 operation.
	if ( bli_l3_return_early_if_trivial( alpha, a, b, beta, c ) == BLIS_SUCCESS)
		return;

	// Initialize a local runtime with global settings if necessary. Note
	// that in the case that a runtime is passed in, we make a local copy.
	rntm_t rntm_l;
	if ( rntm == NULL ) { bli_rntm_init_from_global( &rntm_l ); }
	else                { rntm_l = *rntm;                       }

	// Default to using native execution.
	num_t dt = bli_obj_dt( c );
	ind_t im = BLIS_NAT;

	// If all matrix operands are complex and of the same storage datatype, try
	// to get an induced method (if one is available and enabled).
	if ( bli_obj_dt( a ) == bli_obj_dt( c ) &&
	     bli_obj_dt( b ) == bli_obj_dt( c ) &&
	     bli_obj_is_complex( c ) )
	{
		// Find the highest priority induced method that is both enabled and
		// available for the current operation. (If an induced method is
		// available but not enabled, or simply unavailable, BLIS_NAT will
		// be returned here.)
		im = bli_symmind_find_avail( dt );
	}

	// If necessary, obtain a valid context from the gks using the induced
	// method id determined above.
	if ( cntx == NULL ) cntx = bli_gks_query_ind_cntx( im );

	// Alias A, B, and C in case we need to apply transformations.
	obj_t a_local;
	obj_t b_local;
	obj_t c_local;
	bli_obj_alias_and_reset_origin( a, &a_local );
	bli_obj_alias_and_reset_origin( b, &b_local );
	bli_obj_alias_and_reset_origin( c, &c_local );

#ifdef BLIS_DISABLE_SYMM_RIGHT
	// NOTE: This case casts right-side symm in terms of left side. This is
	// necessary when the current subconfiguration uses a gemm microkernel
	// that assumes that the packing kernel will have already duplicated
	// (broadcast) element of B in the packed copy of B. Supporting
	// duplication within the logic that packs micropanels from symmetric
	// matrices would be ugly, and so we simply don't support it. As a
	// consequence, those subconfigurations need a way to force the symmetric
	// matrix to be on the left (and thus the general matrix to the on the
	// right). So our solution is that in those cases, the subconfigurations
	// simply #define BLIS_DISABLE_SYMM_RIGHT.

	// NOTE: This case casts right-side symm in terms of left side. This can
	// lead to the microkernel being executed on an output matrix with the
	// microkernel's general stride IO case (unless the microkernel supports
	// both both row and column IO cases as well).

	// If A is being multiplied from the right, transpose all operands
	// so that we can perform the computation as if A were being multiplied
	// from the left.
	if ( bli_is_right( side ) )
	{
		bli_toggle_side( &side );
		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

#else
	// NOTE: This case computes right-side hemm/symm natively by packing
	// elements of the Hermitian/symmetric matrix A to micropanels of the
	// right-hand packed matrix operand "B", and elements of the general
	// matrix B to micropanels of the left-hand packed matrix operand "A".
	// This code path always gives us the opportunity to transpose the
	// entire operation so that the effective storage format of the output
	// matrix matches the microkernel's output preference. Thus, from a
	// performance perspective, this case is preferred.

	// An optimization: If C is stored by rows and the micro-kernel prefers
	// contiguous columns, or if C is stored by columns and the micro-kernel
	// prefers contiguous rows, transpose the entire operation to allow the
	// micro-kernel to access elements of C in its preferred manner.
	//if ( !bli_obj_is_1x1( &c_local ) ) // NOTE: This conditional should NOT
	                                     // be enabled. See issue #342 comments.
	if ( bli_cntx_dislikes_storage_of( &c_local, BLIS_GEMM_VIR_UKR, cntx ) )
	{
		bli_toggle_side( &side );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

	// If the Hermitian/symmetric matrix A is being multiplied from the right,
	// swap A and B so that the Hermitian/symmetric matrix will actually be on
	// the right.
	if ( bli_is_right( side ) )
	{
		bli_obj_swap( &a_local, &b_local );
	}
#endif

	// Set the pack schemas within the objects.
	bli_l3_set_schemas( &a_local, &b_local, &c_local, cntx );

	// Parse and interpret the contents of the rntm_t object to properly
	// set the ways of parallelism for each loop, and then make any
	// additional modifications necessary for the current operation.
	bli_rntm_set_ways_for_op
	(
	  BLIS_SYMM,
	  BLIS_LEFT, // ignored for gemm/hemm/symm
	  bli_obj_length( &c_local ),
	  bli_obj_width( &c_local ),
	  bli_obj_width( &a_local ),
	  &rntm_l
	);

	// This is part of a hack to support mixed domain in bli_gemm_front().
	// Sometimes we need to specify a non-standard schema for A and B, and
	// we decided to transmit them via the schema field in the obj_t's
	// rather than pass them in as function parameters. Once the values
	// have been read, we immediately reset them back to their expected
	// values for unpacked objects.
	pack_t schema_a = bli_obj_pack_schema( &a_local );
	pack_t schema_b = bli_obj_pack_schema( &b_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &a_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &b_local );

	cntl_t* cntl = bli_gemm_cntl_create
	(
	  NULL,
	  BLIS_GEMM,
	  schema_a,
	  schema_b,
	  bli_obj_ker_fn( c )
	);

	// Invoke the internal back-end.
	bli_l3_thread_decorator
	(
	  bli_l3_int,
	  alpha,
	  &a_local,
	  &b_local,
	  beta,
	  &c_local,
	  cntx,
	  cntl,
	  &rntm_l
	);

	// Free the thread's local control tree.
	bli_cntl_free( NULL, cntl );
}


void PASTEMAC(trmm3,BLIS_OAPI_EX_SUF)
     (
             side_t  side,
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check the operands.
	if ( bli_error_checking_is_enabled() )
		bli_trmm3_check( side, alpha, a, b, beta, c, cntx );

	// Check for zero dimensions, alpha == 0, or other conditions which
    // mean that we don't actually have to perform a full l3 operation.
	if ( bli_l3_return_early_if_trivial( alpha, a, b, beta, c ) == BLIS_SUCCESS)
		return;

	// Initialize a local runtime with global settings if necessary. Note
	// that in the case that a runtime is passed in, we make a local copy.
	rntm_t rntm_l;
	if ( rntm == NULL ) { bli_rntm_init_from_global( &rntm_l ); }
	else                { rntm_l = *rntm;                       }

	// Default to using native execution.
	num_t dt = bli_obj_dt( c );
	ind_t im = BLIS_NAT;

	// If all matrix operands are complex and of the same storage datatype, try
	// to get an induced method (if one is available and enabled).
	if ( bli_obj_dt( a ) == bli_obj_dt( c ) &&
	     bli_obj_dt( b ) == bli_obj_dt( c ) &&
	     bli_obj_is_complex( c ) )
	{
		// Find the highest priority induced method that is both enabled and
		// available for the current operation. (If an induced method is
		// available but not enabled, or simply unavailable, BLIS_NAT will
		// be returned here.)
		im = bli_trmm3ind_find_avail( dt );
	}

	// If necessary, obtain a valid context from the gks using the induced
	// method id determined above.
	if ( cntx == NULL ) cntx = bli_gks_query_ind_cntx( im );

	// Alias A, B, and C so we can tweak the objects if necessary.
	obj_t a_local;
	obj_t b_local;
	obj_t c_local;
	bli_obj_alias_and_reset_origin( a, &a_local );
	bli_obj_alias_and_reset_origin( b, &b_local );
	bli_obj_alias_and_reset_origin( c, &c_local );

	// We do not explicitly implement the cases where A is transposed.
	// However, we can still handle them. Specifically, if A is marked as
	// needing a transposition, we simply induce a transposition. This
	// allows us to only explicitly implement the no-transpose cases. Once
	// the transposition is induced, the correct algorithm will be called,
	// since, for example, an algorithm over a transposed lower triangular
	// matrix A moves in the same direction (forwards) as a non-transposed
	// upper triangular matrix. And with the transposition induced, the
	// matrix now appears to be upper triangular, so the upper triangular
	// algorithm will grab the correct partitions, as if it were upper
	// triangular (with no transpose) all along.
	if ( bli_obj_has_trans( &a_local ) )
	{
		bli_obj_induce_trans( &a_local );
		bli_obj_set_onlytrans( BLIS_NO_TRANSPOSE, &a_local );
	}

#ifdef BLIS_DISABLE_TRMM3_RIGHT
	// NOTE: This case casts right-side trmm3 in terms of left side. This is
	// necessary when the current subconfiguration uses a gemm microkernel
	// that assumes that the packing kernel will have already duplicated
	// (broadcast) element of B in the packed copy of B. Supporting
	// duplication within the logic that packs micropanels from triangular
	// matrices would be ugly, and so we simply don't support it. As a
	// consequence, those subconfigurations need a way to force the triangular
	// matrix to be on the left (and thus the general matrix to the on the
	// right). So our solution is that in those cases, the subconfigurations
	// simply #define BLIS_DISABLE_TRMM3_RIGHT.

	// NOTE: This case casts right-side trmm3 in terms of left side. This can
	// lead to the microkernel being executed on an output matrix with the
	// microkernel's general stride IO case (unless the microkernel supports
	// both both row and column IO cases as well).

	// NOTE: Casting right-side trmm3 in terms of left side reduces the number
	// of macrokernels exercised to two (trmm_ll and trmm_lu).

	// If A is being multiplied from the right, transpose all operands
	// so that we can perform the computation as if A were being multiplied
	// from the left.
	if ( bli_is_right( side ) )
	{
		bli_toggle_side( &side );
		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

#else

	// An optimization: If C is stored by rows and the micro-kernel prefers
	// contiguous columns, or if C is stored by columns and the micro-kernel
	// prefers contiguous rows, transpose the entire operation to allow the
	// micro-kernel to access elements of C in its preferred manner.
	if ( bli_cntx_dislikes_storage_of( &c_local, BLIS_GEMM_VIR_UKR, cntx ) )
	{
		bli_toggle_side( &side );
		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

	// If A is being multiplied from the right, swap A and B so that
	// the matrix will actually be on the right.
	if ( bli_is_right( side ) )
	{
		bli_obj_swap( &a_local, &b_local );
	}

#endif

	// Set the pack schemas within the objects.
	bli_l3_set_schemas( &a_local, &b_local, &c_local, cntx );

	// Parse and interpret the contents of the rntm_t object to properly
	// set the ways of parallelism for each loop, and then make any
	// additional modifications necessary for the current operation.
	bli_rntm_set_ways_for_op
	(
	  BLIS_TRMM3,
	  side,
	  bli_obj_length( &c_local ),
	  bli_obj_width( &c_local ),
	  bli_obj_width( &a_local ),
	  &rntm_l
	);

	// This is part of a hack to support mixed domain in bli_gemm_front().
	// Sometimes we need to specify a non-standard schema for A and B, and
	// we decided to transmit them via the schema field in the obj_t's
	// rather than pass them in as function parameters. Once the values
	// have been read, we immediately reset them back to their expected
	// values for unpacked objects.
	pack_t schema_a = bli_obj_pack_schema( &a_local );
	pack_t schema_b = bli_obj_pack_schema( &b_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &a_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &b_local );

	cntl_t* cntl = bli_gemm_cntl_create
	(
	  NULL,
	  BLIS_TRMM,
	  schema_a,
	  schema_b,
	  bli_obj_ker_fn( &c_local )
	);

	// Invoke the internal back-end.
	bli_l3_thread_decorator
	(
	  bli_l3_int,
	  alpha,
	  &a_local,
	  &b_local,
	  beta,
	  &c_local,
	  cntx,
	  cntl,
	  &rntm_l
	);

	// Free the thread's local control tree.
	bli_cntl_free( NULL, cntl );
}


void PASTEMAC(herk,BLIS_OAPI_EX_SUF)
     (
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check parameters.
	if ( bli_error_checking_is_enabled() )
		bli_herk_check( alpha, a, beta, c, cntx );

	obj_t ah;
	bli_obj_alias_to( a, &ah );
	bli_obj_toggle_trans( &ah );
	bli_obj_toggle_conj( &ah );

	PASTEMAC(gemmt,BLIS_OAPI_EX_SUF)( alpha, a, &ah, beta, c, cntx, rntm );

	// The Hermitian rank-k product was computed as Re(alpha)*A*A', even for the
	// diagonal elements. Mathematically, the imaginary components of
	// diagonal elements of a Hermitian rank-k product should always be
	// zero. However, in practice, they sometimes accumulate meaningless
	// non-zero values. To prevent this, we explicitly set those values
	// to zero before returning.
	bli_setid( &BLIS_ZERO, c );
}


void PASTEMAC(syrk,BLIS_OAPI_EX_SUF)
     (
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  beta,
       const obj_t*  c,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check parameters.
	if ( bli_error_checking_is_enabled() )
		bli_syrk_check( alpha, a, beta, c, cntx );

	obj_t at;
	bli_obj_alias_to( a, &at );
	bli_obj_toggle_trans( &at );

	PASTEMAC(gemmt,BLIS_OAPI_EX_SUF)( alpha, a, &at, beta, c, cntx, rntm );
}


void PASTEMAC(trmm,BLIS_OAPI_EX_SUF)
     (
             side_t  side,
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check the operands.
	if ( bli_error_checking_is_enabled() )
		bli_trmm_check( side, alpha, a, b, cntx );

	// Check for zero dimensions, alpha == 0, or other conditions which
    // mean that we don't actually have to perform a full l3 operation.
	if ( bli_l3_return_early_if_trivial( alpha, a, b, &BLIS_ZERO, b ) == BLIS_SUCCESS)
		return;

	// Initialize a local runtime with global settings if necessary. Note
	// that in the case that a runtime is passed in, we make a local copy.
	rntm_t rntm_l;
	if ( rntm == NULL ) { bli_rntm_init_from_global( &rntm_l ); }
	else                { rntm_l = *rntm;                       }

	// Default to using native execution.
	num_t dt = bli_obj_dt( b );
	ind_t im = BLIS_NAT;

	// If all matrix operands are complex and of the same storage datatype, try
	// to get an induced method (if one is available and enabled).
	if ( bli_obj_dt( a ) == bli_obj_dt( b ) &&
	     bli_obj_is_complex( b ) )
	{
		// Find the highest priority induced method that is both enabled and
		// available for the current operation. (If an induced method is
		// available but not enabled, or simply unavailable, BLIS_NAT will
		// be returned here.)
		im = bli_trmmind_find_avail( dt );
	}

	// If necessary, obtain a valid context from the gks using the induced
	// method id determined above.
	if ( cntx == NULL ) cntx = bli_gks_query_ind_cntx( im );

	// Alias A and B so we can tweak the objects if necessary.
	obj_t a_local;
	obj_t b_local;
	obj_t c_local;
	bli_obj_alias_and_reset_origin( a, &a_local );
	bli_obj_alias_and_reset_origin( b, &b_local );
	bli_obj_alias_and_reset_origin( b, &c_local );

	// We do not explicitly implement the cases where A is transposed.
	// However, we can still handle them. Specifically, if A is marked as
	// needing a transposition, we simply induce a transposition. This
	// allows us to only explicitly implement the no-transpose cases. Once
	// the transposition is induced, the correct algorithm will be called,
	// since, for example, an algorithm over a transposed lower triangular
	// matrix A moves in the same direction (forwards) as a non-transposed
	// upper triangular matrix. And with the transposition induced, the
	// matrix now appears to be upper triangular, so the upper triangular
	// algorithm will grab the correct partitions, as if it were upper
	// triangular (with no transpose) all along.
	if ( bli_obj_has_trans( &a_local ) )
	{
		bli_obj_induce_trans( &a_local );
		bli_obj_set_onlytrans( BLIS_NO_TRANSPOSE, &a_local );
	}

#ifdef BLIS_DISABLE_TRMM_RIGHT
	// NOTE: This case casts right-side trmm in terms of left side. This is
	// necessary when the current subconfiguration uses a gemm microkernel
	// that assumes that the packing kernel will have already duplicated
	// (broadcast) element of B in the packed copy of B. Supporting
	// duplication within the logic that packs micropanels from triangular
	// matrices would be ugly, and so we simply don't support it. As a
	// consequence, those subconfigurations need a way to force the triangular
	// matrix to be on the left (and thus the general matrix to the on the
	// right). So our solution is that in those cases, the subconfigurations
	// simply #define BLIS_DISABLE_TRMM_RIGHT.

	// NOTE: This case casts right-side trmm in terms of left side. This can
	// lead to the microkernel being executed on an output matrix with the
	// microkernel's general stride IO case (unless the microkernel supports
	// both both row and column IO cases as well).

	// NOTE: Casting right-side trmm in terms of left side reduces the number
	// of macrokernels exercised to two (trmm_ll and trmm_lu).

	// If A is being multiplied from the right, transpose all operands
	// so that we can perform the computation as if A were being multiplied
	// from the left.
	if ( bli_is_right( side ) )
	{
		bli_toggle_side( &side );
		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

#else
	// NOTE: This case computes right-side trmm natively with trmm_rl and
	// trmm_ru macrokernels. This code path always gives us the opportunity
	// to transpose the entire operation so that the effective storage format
	// of the output matrix matches the microkernel's output preference.
	// Thus, from a performance perspective, this case is preferred.

	// An optimization: If C is stored by rows and the micro-kernel prefers
	// contiguous columns, or if C is stored by columns and the micro-kernel
	// prefers contiguous rows, transpose the entire operation to allow the
	// micro-kernel to access elements of C in its preferred manner.
	// NOTE: We disable the optimization for 1x1 matrices since the concept
	// of row- vs. column storage breaks down.
	//if ( !bli_obj_is_1x1( &c_local ) ) // NOTE: This conditional should NOT
	                                     // be enabled. See issue #342 comments.
	if ( bli_cntx_dislikes_storage_of( &c_local, BLIS_GEMM_VIR_UKR, cntx ) )
	{
		bli_toggle_side( &side );
		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

	// If A is being multiplied from the right, swap A and B so that
	// the matrix will actually be on the right.
	if ( bli_is_right( side ) )
	{
		bli_obj_swap( &a_local, &b_local );
	}

#endif

	// Set the pack schemas within the objects.
	bli_l3_set_schemas( &a_local, &b_local, &c_local, cntx );

	// Parse and interpret the contents of the rntm_t object to properly
	// set the ways of parallelism for each loop, and then make any
	// additional modifications necessary for the current operation.
	bli_rntm_set_ways_for_op
	(
	  BLIS_TRMM,
	  side,
	  bli_obj_length( &c_local ),
	  bli_obj_width( &c_local ),
	  bli_obj_width( &a_local ),
	  &rntm_l
	);

	// This is part of a hack to support mixed domain in bli_gemm_front().
	// Sometimes we need to specify a non-standard schema for A and B, and
	// we decided to transmit them via the schema field in the obj_t's
	// rather than pass them in as function parameters. Once the values
	// have been read, we immediately reset them back to their expected
	// values for unpacked objects.
	pack_t schema_a = bli_obj_pack_schema( &a_local );
	pack_t schema_b = bli_obj_pack_schema( &b_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &a_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &b_local );

	cntl_t* cntl = bli_gemm_cntl_create
	(
	  NULL,
	  BLIS_TRMM,
	  schema_a,
	  schema_b,
	  bli_obj_ker_fn( &c_local )
	);

	// Invoke the internal back-end.
	bli_l3_thread_decorator
	(
	  bli_l3_int,
	  alpha,
	  &a_local,
	  &b_local,
	  &BLIS_ZERO,
	  &c_local,
	  cntx,
	  cntl,
	  &rntm_l
	);

	// Free the thread's local control tree.
	bli_cntl_free( NULL, cntl );
}


void PASTEMAC(trsm,BLIS_OAPI_EX_SUF)
     (
             side_t  side,
       const obj_t*  alpha,
       const obj_t*  a,
       const obj_t*  b,
       const cntx_t* cntx,
       const rntm_t* rntm
     )
{
	bli_init_once();

	// Check the operands.
	if ( bli_error_checking_is_enabled() )
		bli_trsm_check( side, alpha, a, b, cntx );

	// Check for zero dimensions, alpha == 0, or other conditions which
    // mean that we don't actually have to perform a full l3 operation.
	if ( bli_l3_return_early_if_trivial( alpha, a, b, &BLIS_ZERO, b ) == BLIS_SUCCESS)
		return;

	// Initialize a local runtime with global settings if necessary. Note
	// that in the case that a runtime is passed in, we make a local copy.
	rntm_t rntm_l;
	if ( rntm == NULL ) { bli_rntm_init_from_global( &rntm_l ); }
	else                { rntm_l = *rntm;                       }

	// Default to using native execution.
	num_t dt = bli_obj_dt( b );
	ind_t im = BLIS_NAT;

	// If all matrix operands are complex and of the same storage datatype, try
	// to get an induced method (if one is available and enabled).
	if ( bli_obj_dt( a ) == bli_obj_dt( b ) &&
	     bli_obj_is_complex( b ) )
	{
		// Find the highest priority induced method that is both enabled and
		// available for the current operation. (If an induced method is
		// available but not enabled, or simply unavailable, BLIS_NAT will
		// be returned here.)
		im = bli_trsmind_find_avail( dt );
	}

	// If necessary, obtain a valid context from the gks using the induced
	// method id determined above.
	if ( cntx == NULL ) cntx = bli_gks_query_ind_cntx( im );

#if 0
#ifdef BLIS_ENABLE_SMALL_MATRIX_TRSM
	gint_t status = bli_trsm_small( side, alpha, a, b, cntx, cntl );
	if ( status == BLIS_SUCCESS ) return;
#endif
#endif

	// Alias A and B so we can tweak the objects if necessary.
	obj_t a_local;
	obj_t b_local;
	obj_t c_local;
	bli_obj_alias_and_reset_origin( a, &a_local );
	bli_obj_alias_and_reset_origin( b, &b_local );
	bli_obj_alias_and_reset_origin( b, &c_local );

	// We do not explicitly implement the cases where A is transposed.
	// However, we can still handle them. Specifically, if A is marked as
	// needing a transposition, we simply induce a transposition. This
	// allows us to only explicitly implement the no-transpose cases. Once
	// the transposition is induced, the correct algorithm will be called,
	// since, for example, an algorithm over a transposed lower triangular
	// matrix A moves in the same direction (forwards) as a non-transposed
	// upper triangular matrix. And with the transposition induced, the
	// matrix now appears to be upper triangular, so the upper triangular
	// algorithm will grab the correct partitions, as if it were upper
	// triangular (with no transpose) all along.
	if ( bli_obj_has_trans( &a_local ) )
	{
		bli_obj_induce_trans( &a_local );
		bli_obj_set_onlytrans( BLIS_NO_TRANSPOSE, &a_local );
	}

#if 1

	// If A is being solved against from the right, transpose all operands
	// so that we can perform the computation as if A were being solved
	// from the left.
	if ( bli_is_right( side ) )
	{
		bli_toggle_side( &side );
		bli_obj_induce_trans( &a_local );
		bli_obj_induce_trans( &b_local );
		bli_obj_induce_trans( &c_local );
	}

#else

	// NOTE: Enabling this code requires that BLIS NOT be configured with
	// BLIS_RELAX_MCNR_NCMR_CONSTRAINTS defined.
#ifdef BLIS_RELAX_MCNR_NCMR_CONSTRAINTS
	#error "BLIS_RELAX_MCNR_NCMR_CONSTRAINTS must not be defined for current trsm_r implementation."
#endif

	// If A is being solved against from the right, swap A and B so that
	// the triangular matrix will actually be on the right.
	if ( bli_is_right( side ) )
	{
		bli_obj_swap( &a_local, &b_local );
	}

#endif

	// Set the pack schemas within the objects.
	bli_l3_set_schemas( &a_local, &b_local, &c_local, cntx );

	// Parse and interpret the contents of the rntm_t object to properly
	// set the ways of parallelism for each loop, and then make any
	// additional modifications necessary for the current operation.
	bli_rntm_set_ways_for_op
	(
	  BLIS_TRSM,
	  side,
	  bli_obj_length( &c_local ),
	  bli_obj_width( &c_local ),
	  bli_obj_width( &a_local ),
	  &rntm_l
	);

	// This is part of a hack to support mixed domain in bli_gemm_front().
	// Sometimes we need to specify a non-standard schema for A and B, and
	// we decided to transmit them via the schema field in the obj_t's
	// rather than pass them in as function parameters. Once the values
	// have been read, we immediately reset them back to their expected
	// values for unpacked objects.
	pack_t schema_a = bli_obj_pack_schema( &a_local );
	pack_t schema_b = bli_obj_pack_schema( &b_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &a_local );
	bli_obj_set_pack_schema( BLIS_NOT_PACKED, &b_local );

	cntl_t* cntl = bli_trsm_cntl_create
	(
	  NULL,
	  bli_obj_is_triangular( a ) ? BLIS_LEFT : BLIS_RIGHT,
	  schema_a,
	  schema_b,
	  bli_obj_ker_fn( &c_local )
	);

	// Invoke the internal back-end.
	bli_l3_thread_decorator
	(
	  bli_l3_int,
	  alpha,
	  &a_local,
	  &b_local,
	  alpha,
	  &c_local,
	  cntx,
	  cntl,
	  &rntm_l
	);

	// Free the thread's local control tree.
	bli_cntl_free( NULL, cntl );
}
