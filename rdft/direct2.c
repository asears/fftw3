/*
 * Copyright (c) 2002 Matteo Frigo
 * Copyright (c) 2002 Steven G. Johnson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: direct2.c,v 1.10 2002-09-25 01:15:57 athena Exp $ */

/* direct RDFT2 R2HC/HC2R solver, if we have a codelet */

#include "rdft.h"

typedef union {
     kr2hc r2hc;
     khc2r hc2r;
} kodelet;

typedef struct {
     solver super;
     union {
	  const kr2hc_desc *r2hc;
	  const khc2r_desc *hc2r;
     } desc;
     kodelet k;
     uint sz;
     rdft_kind kind;
     const char *nam;
} S;

typedef struct {
     plan_rdft2 super;

     stride is, os;
     uint vl;
     int ivs, ovs;
     kodelet k;
     const S *slv;
     int ilast;
} P;

static void apply_r2hc(plan *ego_, R *r, R *rio, R *iio)
{
     P *ego = (P *) ego_;
     uint i, vl = ego->vl, ovs = ego->ovs;
     ego->k.r2hc(r, rio, iio, ego->is, ego->os, ego->os,
		 vl, ego->ivs, ovs);
     for (i = 0; i < vl; ++i, iio += ovs)
	  iio[0] = iio[ego->ilast] = 0;
}

static void apply_hc2r(plan *ego_, R *r, R *rio, R *iio)
{
     P *ego = (P *) ego_;
     ego->k.hc2r(rio, iio, r, ego->os, ego->os, ego->is,
		 ego->vl, ego->ivs, ego->ovs);
}

static void destroy(plan *ego_)
{
     P *ego = (P *) ego_;
     X(stride_destroy)(ego->is);
     X(stride_destroy)(ego->os);
}

static void print(plan *ego_, printer *p)
{
     P *ego = (P *) ego_;
     const S *s = ego->slv;

     p->print(p, "(rdft2-%s-direct-%u%v \"%s\")", 
	      X(rdft_kind_str)(s->kind), s->sz, ego->vl, s->nam);
}

static int applicable(const solver *ego_, const problem *p_)
{
     if (RDFT2P(p_)) {
          const S *ego = (const S *) ego_;
          const problem_rdft2 *p = (const problem_rdft2 *) p_;
	  uint vl;
	  int ivs, ovs;

	  X(tensor_tornk1)(p->vecsz, &vl, &ivs, &ovs);

          return (
	       1
	       && p->sz->rnk == 1
	       && p->vecsz->rnk <= 1
	       && p->sz->dims[0].n == ego->sz
	       && p->kind == ego->kind

	       /* check strides etc */
	       && (!R2HC_KINDP(ego->kind) ||
		   ego->desc.r2hc->genus->okp(ego->desc.r2hc, 
					      p->r, p->rio, p->rio,
					      p->sz->dims[0].is,
					      p->sz->dims[0].os,
					      p->sz->dims[0].os,
					      vl, ivs, ovs))
	       && (!HC2R_KINDP(ego->kind) ||
		   ego->desc.hc2r->genus->okp(ego->desc.hc2r,
					      p->rio, p->rio, p->r,
					      p->sz->dims[0].is,
					      p->sz->dims[0].is,
					      p->sz->dims[0].os,
					      vl, ivs, ovs))
	       
	       && (0
		   /* can operate out-of-place */
		   || p->r != p->rio
		   || p->r != p->iio

		   /*
		    * can compute one transform in-place, no matter
		    * what the strides are.
		    */
		   || p->vecsz->rnk == 0

		   /* can operate in-place as long as strides are the same */
		   || X(rdft2_inplace_strides)(p, RNK_MINFTY)
		    )
	       );
     }

     return 0;
}

static plan *mkplan(const solver *ego_, const problem *p_, planner *plnr)
{
     const S *ego = (const S *) ego_;
     P *pln;
     const problem_rdft2 *p;
     iodim d, *vd;
     int r2hc_kindp;

     static const plan_adt padt = {
	  X(rdft2_solve), X(null_awake), print, destroy
     };

     UNUSED(plnr);

     if (!applicable(ego_, p_))
          return (plan *)0;

     p = (const problem_rdft2 *) p_;

     r2hc_kindp = R2HC_KINDP(p->kind);
     A(r2hc_kindp || HC2R_KINDP(p->kind));

     pln = MKPLAN_RDFT2(P, &padt, r2hc_kindp ? apply_r2hc : apply_hc2r);

     d = p->sz->dims[0];
     vd = p->vecsz->dims;

     pln->k = ego->k;

     pln->is = X(mkstride)(ego->sz, r2hc_kindp ? d.is : d.os);
     pln->os = X(mkstride)(d.n/2 + 1, r2hc_kindp ? d.os : d.is);

     X(tensor_tornk1)(p->vecsz, &pln->vl, &pln->ivs, &pln->ovs);

     pln->ilast = (d.n % 2) ? 0 : (d.n/2) * d.os; /* Nyquist freq., if any */

     pln->slv = ego;
     X(ops_zero)(&pln->super.super.ops);
     if (r2hc_kindp)
	  X(ops_madd2)(pln->vl / ego->desc.r2hc->genus->vl,
		       &ego->desc.r2hc->ops,
		       &pln->super.super.ops);
     else {
	  X(ops_madd2)(pln->vl / ego->desc.hc2r->genus->vl,
		       &ego->desc.hc2r->ops,
		       &pln->super.super.ops);
	  pln->super.super.ops.other += 2 * pln->vl; /* + 2 stores */
     }

     return &(pln->super.super);
}

/* constructor */
solver *X(mksolver_rdft2_r2hc_direct)(kr2hc k, const kr2hc_desc *desc)
{
     static const solver_adt sadt = { mkplan };
     S *slv = MKSOLVER(S, &sadt);
     slv->k.r2hc = k;
     slv->desc.r2hc = desc;
     slv->sz = desc->sz;
     slv->nam = desc->nam;
     slv->kind = desc->genus->kind;
     return &(slv->super);
}

solver *X(mksolver_rdft2_hc2r_direct)(khc2r k, const khc2r_desc *desc)
{
     static const solver_adt sadt = { mkplan };
     S *slv = MKSOLVER(S, &sadt);
     slv->k.hc2r = k;
     slv->desc.hc2r = desc;
     slv->sz = desc->sz;
     slv->nam = desc->nam;
     slv->kind = desc->genus->kind;
     return &(slv->super);
}
