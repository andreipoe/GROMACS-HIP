/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team.
 * Copyright (c) 2013,2014,2015,2016,2017,2018, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
#include "gmxpre.h"

#include "constr.h"

#include <assert.h>
#include <stdlib.h>

#include <cmath>

#include <algorithm>

#include "gromacs/domdec/domdec.h"
#include "gromacs/domdec/domdec_struct.h"
#include "gromacs/essentialdynamics/edsam.h"
#include "gromacs/fileio/confio.h"
#include "gromacs/fileio/gmxfio.h"
#include "gromacs/fileio/pdbio.h"
#include "gromacs/gmxlib/nrnb.h"
#include "gromacs/math/utilities.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdlib/gmx_omp_nthreads.h"
#include "gromacs/mdlib/lincs.h"
#include "gromacs/mdlib/mdrun.h"
#include "gromacs/mdlib/settle.h"
#include "gromacs/mdlib/shake.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/mdtypes/mdatom.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/pulling/pull.h"
#include "gromacs/topology/block.h"
#include "gromacs/topology/ifunc.h"
#include "gromacs/topology/mtop_lookup.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/pleasecite.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/txtdump.h"

typedef struct gmx_constr {
    int                ncon_tot;       /* The total number of constraints    */
    int                nflexcon;       /* The number of flexible constraints */
    int                n_at2con_mt;    /* The size of at2con = #moltypes     */
    t_blocka          *at2con_mt;      /* A list of atoms to constraints     */
    int                n_at2settle_mt; /* The size of at2settle = #moltypes  */
    int              **at2settle_mt;   /* A list of atoms to settles         */
    gmx_bool           bInterCGsettles;
    gmx_lincsdata_t    lincsd;         /* LINCS data                         */
    gmx_shakedata_t    shaked;         /* SHAKE data                         */
    gmx_settledata_t   settled;        /* SETTLE data                        */
    int                maxwarn;        /* The maximum number of warnings     */
    int                warncount_lincs;
    int                warncount_settle;
    gmx_edsam_t        ed;             /* The essential dynamics data        */

    /* Thread local working data */
    tensor            *vir_r_m_dr_th;           /* Thread virial contribution */
    bool              *bSettleErrorHasOccurred; /* Did a settle error occur?  */

    /* Only used for printing warnings */
    const gmx_mtop_t  *warn_mtop;     /* Pointer to the global topology     */
} t_gmx_constr;

int n_flexible_constraints(struct gmx_constr *constr)
{
    int nflexcon;

    if (constr)
    {
        nflexcon = constr->nflexcon;
    }
    else
    {
        nflexcon = 0;
    }

    return nflexcon;
}

static void clear_constraint_quantity_nonlocal(gmx_domdec_t *dd, rvec *q)
{
    int nonlocal_at_start, nonlocal_at_end, at;

    dd_get_constraint_range(dd, &nonlocal_at_start, &nonlocal_at_end);

    for (at = nonlocal_at_start; at < nonlocal_at_end; at++)
    {
        clear_rvec(q[at]);
    }
}

void too_many_constraint_warnings(int eConstrAlg, int warncount)
{
    gmx_fatal(FARGS,
              "Too many %s warnings (%d)\n"
              "If you know what you are doing you can %s"
              "set the environment variable GMX_MAXCONSTRWARN to -1,\n"
              "but normally it is better to fix the problem",
              (eConstrAlg == econtLINCS) ? "LINCS" : "SETTLE", warncount,
              (eConstrAlg == econtLINCS) ?
              "adjust the lincs warning threshold in your mdp file\nor " : "\n");
}

static void write_constr_pdb(const char *fn, const char *title,
                             const gmx_mtop_t *mtop,
                             int start, int homenr, const t_commrec *cr,
                             rvec x[], matrix box)
{
    char          fname[STRLEN];
    FILE         *out;
    int           dd_ac0 = 0, dd_ac1 = 0, i, ii, resnr;
    gmx_domdec_t *dd;
    const char   *anm, *resnm;

    dd = nullptr;
    if (DOMAINDECOMP(cr))
    {
        dd = cr->dd;
        dd_get_constraint_range(dd, &dd_ac0, &dd_ac1);
        start  = 0;
        homenr = dd_ac1;
    }

    if (PAR(cr))
    {
        sprintf(fname, "%s_n%d.pdb", fn, cr->sim_nodeid);
    }
    else
    {
        sprintf(fname, "%s.pdb", fn);
    }

    out = gmx_fio_fopen(fname, "w");

    fprintf(out, "TITLE     %s\n", title);
    gmx_write_pdb_box(out, -1, box);
    int molb = 0;
    for (i = start; i < start+homenr; i++)
    {
        if (dd != nullptr)
        {
            if (i >= dd->nat_home && i < dd_ac0)
            {
                continue;
            }
            ii = dd->gatindex[i];
        }
        else
        {
            ii = i;
        }
        mtopGetAtomAndResidueName(mtop, ii, &molb, &anm, &resnr, &resnm, nullptr);
        gmx_fprintf_pdb_atomline(out, epdbATOM, ii+1, anm, ' ', resnm, ' ', resnr, ' ',
                                 10*x[i][XX], 10*x[i][YY], 10*x[i][ZZ], 1.0, 0.0, "");
    }
    fprintf(out, "TER\n");

    gmx_fio_fclose(out);
}

static void dump_confs(FILE *fplog, gmx_int64_t step, const gmx_mtop_t *mtop,
                       int start, int homenr, const t_commrec *cr,
                       rvec x[], rvec xprime[], matrix box)
{
    char  buf[STRLEN], buf2[22];

    char *env = getenv("GMX_SUPPRESS_DUMP");
    if (env)
    {
        return;
    }

    sprintf(buf, "step%sb", gmx_step_str(step, buf2));
    write_constr_pdb(buf, "initial coordinates",
                     mtop, start, homenr, cr, x, box);
    sprintf(buf, "step%sc", gmx_step_str(step, buf2));
    write_constr_pdb(buf, "coordinates after constraining",
                     mtop, start, homenr, cr, xprime, box);
    if (fplog)
    {
        fprintf(fplog, "Wrote pdb files with previous and current coordinates\n");
    }
    fprintf(stderr, "Wrote pdb files with previous and current coordinates\n");
}

gmx_bool constrain(FILE *fplog, gmx_bool bLog, gmx_bool bEner,
                   struct gmx_constr *constr,
                   t_idef *idef, const t_inputrec *ir,
                   const t_commrec *cr,
                   const gmx_multisim_t *ms,
                   gmx_int64_t step, int delta_step,
                   real step_scaling,
                   t_mdatoms *md,
                   rvec *x, rvec *xprime, rvec *min_proj,
                   gmx_bool bMolPBC, matrix box,
                   real lambda, real *dvdlambda,
                   rvec *v, tensor *vir,
                   t_nrnb *nrnb, int econq)
{
    gmx_bool    bOK, bDump;
    int         start, homenr;
    tensor      vir_r_m_dr;
    real        scaled_delta_t;
    real        invdt, vir_fac = 0, t;
    t_ilist    *settle;
    int         nsettle;
    t_pbc       pbc, *pbc_null;
    char        buf[22];
    int         nth, th;

    if (econq == econqForceDispl && !EI_ENERGY_MINIMIZATION(ir->eI))
    {
        gmx_incons("constrain called for forces displacements while not doing energy minimization, can not do this while the LINCS and SETTLE constraint connection matrices are mass weighted");
    }

    bOK   = TRUE;
    bDump = FALSE;

    start  = 0;
    homenr = md->homenr;

    scaled_delta_t = step_scaling * ir->delta_t;

    /* Prepare time step for use in constraint implementations, and
       avoid generating inf when ir->delta_t = 0. */
    if (ir->delta_t == 0)
    {
        invdt = 0.0;
    }
    else
    {
        invdt = 1.0/scaled_delta_t;
    }

    if (ir->efep != efepNO && EI_DYNAMICS(ir->eI))
    {
        /* Set the constraint lengths for the step at which this configuration
         * is meant to be. The invmasses should not be changed.
         */
        lambda += delta_step*ir->fepvals->delta_lambda;
    }

    if (vir != nullptr)
    {
        clear_mat(vir_r_m_dr);
    }

    where();

    settle  = &idef->il[F_SETTLE];
    nsettle = settle->nr/(1+NRAL(F_SETTLE));

    if (nsettle > 0)
    {
        nth = gmx_omp_nthreads_get(emntSETTLE);
    }
    else
    {
        nth = 1;
    }

    /* We do not need full pbc when constraints do not cross charge groups,
     * i.e. when dd->constraint_comm==NULL.
     * Note that PBC for constraints is different from PBC for bondeds.
     * For constraints there is both forward and backward communication.
     */
    if (ir->ePBC != epbcNONE &&
        (cr->dd || bMolPBC) && !(cr->dd && cr->dd->constraint_comm == nullptr))
    {
        /* With pbc=screw the screw has been changed to a shift
         * by the constraint coordinate communication routine,
         * so that here we can use normal pbc.
         */
        pbc_null = set_pbc_dd(&pbc, ir->ePBC,
                              DOMAINDECOMP(cr) ? cr->dd->nc : nullptr,
                              FALSE, box);
    }
    else
    {
        pbc_null = nullptr;
    }

    /* Communicate the coordinates required for the non-local constraints
     * for LINCS and/or SETTLE.
     */
    if (cr->dd)
    {
        dd_move_x_constraints(cr->dd, box, x, xprime, econq == econqCoord);

        if (v != nullptr)
        {
            /* We need to initialize the non-local components of v.
             * We never actually use these values, but we do increment them,
             * so we should avoid uninitialized variables and overflows.
             */
            clear_constraint_quantity_nonlocal(cr->dd, v);
        }
    }

    if (constr->lincsd != nullptr)
    {
        bOK = constrain_lincs(fplog, bLog, bEner, ir, step, constr->lincsd, md, cr, ms,
                              x, xprime, min_proj,
                              box, pbc_null, lambda, dvdlambda,
                              invdt, v, vir != nullptr, vir_r_m_dr,
                              econq, nrnb,
                              constr->maxwarn, &constr->warncount_lincs);
        if (!bOK && constr->maxwarn < INT_MAX)
        {
            if (fplog != nullptr)
            {
                fprintf(fplog, "Constraint error in algorithm %s at step %s\n",
                        econstr_names[econtLINCS], gmx_step_str(step, buf));
            }
            bDump = TRUE;
        }
    }

    if (constr->shaked != nullptr)
    {
        bOK = constrain_shake(fplog, constr->shaked,
                              md->invmass,
                              idef, ir, x, xprime, min_proj, nrnb,
                              lambda, dvdlambda,
                              invdt, v, vir != nullptr, vir_r_m_dr,
                              constr->maxwarn < INT_MAX, econq);

        if (!bOK && constr->maxwarn < INT_MAX)
        {
            if (fplog != nullptr)
            {
                fprintf(fplog, "Constraint error in algorithm %s at step %s\n",
                        econstr_names[econtSHAKE], gmx_step_str(step, buf));
            }
            bDump = TRUE;
        }
    }

    if (nsettle > 0)
    {
        bool bSettleErrorHasOccurred = false;

        switch (econq)
        {
            case econqCoord:
#pragma omp parallel for num_threads(nth) schedule(static)
                for (th = 0; th < nth; th++)
                {
                    try
                    {
                        if (th > 0)
                        {
                            clear_mat(constr->vir_r_m_dr_th[th]);
                        }

                        csettle(constr->settled,
                                nth, th,
                                pbc_null,
                                x[0], xprime[0],
                                invdt, v ? v[0] : nullptr,
                                vir != nullptr,
                                th == 0 ? vir_r_m_dr : constr->vir_r_m_dr_th[th],
                                th == 0 ? &bSettleErrorHasOccurred : &constr->bSettleErrorHasOccurred[th]);
                    }
                    GMX_CATCH_ALL_AND_EXIT_WITH_FATAL_ERROR;
                }
                inc_nrnb(nrnb, eNR_SETTLE, nsettle);
                if (v != nullptr)
                {
                    inc_nrnb(nrnb, eNR_CONSTR_V, nsettle*3);
                }
                if (vir != nullptr)
                {
                    inc_nrnb(nrnb, eNR_CONSTR_VIR, nsettle*3);
                }
                break;
            case econqVeloc:
            case econqDeriv:
            case econqForce:
            case econqForceDispl:
#pragma omp parallel for num_threads(nth) schedule(static)
                for (th = 0; th < nth; th++)
                {
                    try
                    {
                        int calcvir_atom_end;

                        if (vir == nullptr)
                        {
                            calcvir_atom_end = 0;
                        }
                        else
                        {
                            calcvir_atom_end = md->homenr;
                        }

                        if (th > 0)
                        {
                            clear_mat(constr->vir_r_m_dr_th[th]);
                        }

                        int start_th = (nsettle* th   )/nth;
                        int end_th   = (nsettle*(th+1))/nth;

                        if (start_th >= 0 && end_th - start_th > 0)
                        {
                            settle_proj(constr->settled, econq,
                                        end_th-start_th,
                                        settle->iatoms+start_th*(1+NRAL(F_SETTLE)),
                                        pbc_null,
                                        x,
                                        xprime, min_proj, calcvir_atom_end,
                                        th == 0 ? vir_r_m_dr : constr->vir_r_m_dr_th[th]);
                        }
                    }
                    GMX_CATCH_ALL_AND_EXIT_WITH_FATAL_ERROR;
                }
                /* This is an overestimate */
                inc_nrnb(nrnb, eNR_SETTLE, nsettle);
                break;
            case econqDeriv_FlexCon:
                /* Nothing to do, since the are no flexible constraints in settles */
                break;
            default:
                gmx_incons("Unknown constraint quantity for settle");
        }

        if (vir != nullptr)
        {
            /* Reduce the virial contributions over the threads */
            for (int th = 1; th < nth; th++)
            {
                m_add(vir_r_m_dr, constr->vir_r_m_dr_th[th], vir_r_m_dr);
            }
        }

        if (econq == econqCoord)
        {
            for (int th = 1; th < nth; th++)
            {
                bSettleErrorHasOccurred = bSettleErrorHasOccurred || constr->bSettleErrorHasOccurred[th];
            }

            if (bSettleErrorHasOccurred)
            {
                char buf[STRLEN];
                sprintf(buf,
                        "\nstep " "%" GMX_PRId64 ": One or more water molecules can not be settled.\n"
                        "Check for bad contacts and/or reduce the timestep if appropriate.\n",
                        step);
                if (fplog)
                {
                    fprintf(fplog, "%s", buf);
                }
                fprintf(stderr, "%s", buf);
                constr->warncount_settle++;
                if (constr->warncount_settle > constr->maxwarn)
                {
                    too_many_constraint_warnings(-1, constr->warncount_settle);
                }
                bDump = TRUE;

                bOK   = FALSE;
            }
        }
    }

    if (vir != nullptr)
    {
        /* The normal uses of constrain() pass step_scaling = 1.0.
         * The call to constrain() for SD1 that passes step_scaling =
         * 0.5 also passes vir = NULL, so cannot reach this
         * assertion. This assertion should remain until someone knows
         * that this path works for their intended purpose, and then
         * they can use scaled_delta_t instead of ir->delta_t
         * below. */
        assert(gmx_within_tol(step_scaling, 1.0, GMX_REAL_EPS));
        switch (econq)
        {
            case econqCoord:
                vir_fac = 0.5/(ir->delta_t*ir->delta_t);
                break;
            case econqVeloc:
                vir_fac = 0.5/ir->delta_t;
                break;
            case econqForce:
            case econqForceDispl:
                vir_fac = 0.5;
                break;
            default:
                gmx_incons("Unsupported constraint quantity for virial");
        }

        if (EI_VV(ir->eI))
        {
            vir_fac *= 2;  /* only constraining over half the distance here */
        }
        for (int i = 0; i < DIM; i++)
        {
            for (int j = 0; j < DIM; j++)
            {
                (*vir)[i][j] = vir_fac*vir_r_m_dr[i][j];
            }
        }
    }

    if (bDump)
    {
        dump_confs(fplog, step, constr->warn_mtop, start, homenr, cr, x, xprime, box);
    }

    if (econq == econqCoord)
    {
        if (ir->bPull && pull_have_constraint(ir->pull_work))
        {
            if (EI_DYNAMICS(ir->eI))
            {
                t = ir->init_t + (step + delta_step)*ir->delta_t;
            }
            else
            {
                t = ir->init_t;
            }
            set_pbc(&pbc, ir->ePBC, box);
            pull_constraint(ir->pull_work, md, &pbc, cr, ir->delta_t, t, x, xprime, v, *vir);
        }
        if (constr->ed && delta_step > 0)
        {
            /* apply the essential dynamics constraints here */
            do_edsam(ir, step, cr, xprime, v, box, constr->ed);
        }
    }

    return bOK;
}

real *constr_rmsd_data(struct gmx_constr *constr)
{
    if (constr->lincsd)
    {
        return lincs_rmsd_data(constr->lincsd);
    }
    else
    {
        return nullptr;
    }
}

real constr_rmsd(struct gmx_constr *constr)
{
    if (constr->lincsd)
    {
        return lincs_rmsd(constr->lincsd);
    }
    else
    {
        return 0;
    }
}

t_blocka make_at2con(int start, int natoms,
                     const t_ilist *ilist, const t_iparams *iparams,
                     gmx_bool bDynamics, int *nflexiblecons)
{
    int      *count, ncon, con, con_tot, nflexcon, ftype, i, a;
    t_iatom  *ia;
    t_blocka  at2con;
    gmx_bool  bFlexCon;

    snew(count, natoms);
    nflexcon = 0;
    for (ftype = F_CONSTR; ftype <= F_CONSTRNC; ftype++)
    {
        ncon = ilist[ftype].nr/3;
        ia   = ilist[ftype].iatoms;
        for (con = 0; con < ncon; con++)
        {
            bFlexCon = (iparams[ia[0]].constr.dA == 0 &&
                        iparams[ia[0]].constr.dB == 0);
            if (bFlexCon)
            {
                nflexcon++;
            }
            if (bDynamics || !bFlexCon)
            {
                for (i = 1; i < 3; i++)
                {
                    a = ia[i] - start;
                    count[a]++;
                }
            }
            ia += 3;
        }
    }
    *nflexiblecons = nflexcon;

    at2con.nr           = natoms;
    at2con.nalloc_index = at2con.nr+1;
    snew(at2con.index, at2con.nalloc_index);
    at2con.index[0] = 0;
    for (a = 0; a < natoms; a++)
    {
        at2con.index[a+1] = at2con.index[a] + count[a];
        count[a]          = 0;
    }
    at2con.nra      = at2con.index[natoms];
    at2con.nalloc_a = at2con.nra;
    snew(at2con.a, at2con.nalloc_a);

    /* The F_CONSTRNC constraints have constraint numbers
     * that continue after the last F_CONSTR constraint.
     */
    con_tot = 0;
    for (ftype = F_CONSTR; ftype <= F_CONSTRNC; ftype++)
    {
        ncon = ilist[ftype].nr/3;
        ia   = ilist[ftype].iatoms;
        for (con = 0; con < ncon; con++)
        {
            bFlexCon = (iparams[ia[0]].constr.dA == 0 &&
                        iparams[ia[0]].constr.dB == 0);
            if (bDynamics || !bFlexCon)
            {
                for (i = 1; i < 3; i++)
                {
                    a = ia[i] - start;
                    at2con.a[at2con.index[a]+count[a]++] = con_tot;
                }
            }
            con_tot++;
            ia += 3;
        }
    }

    sfree(count);

    return at2con;
}

static int *make_at2settle(int natoms, const t_ilist *ilist)
{
    int *at2s;
    int  a, stride, s;

    snew(at2s, natoms);
    /* Set all to no settle */
    for (a = 0; a < natoms; a++)
    {
        at2s[a] = -1;
    }

    stride = 1 + NRAL(F_SETTLE);

    for (s = 0; s < ilist->nr; s += stride)
    {
        at2s[ilist->iatoms[s+1]] = s/stride;
        at2s[ilist->iatoms[s+2]] = s/stride;
        at2s[ilist->iatoms[s+3]] = s/stride;
    }

    return at2s;
}

void set_constraints(struct gmx_constr *constr,
                     gmx_localtop_t *top, const t_inputrec *ir,
                     const t_mdatoms *md, const t_commrec *cr)
{
    t_idef *idef = &top->idef;

    if (constr->ncon_tot > 0)
    {
        /* With DD we might also need to call LINCS on a domain no constraints for
         * communicating coordinates to other nodes that do have constraints.
         */
        if (ir->eConstrAlg == econtLINCS)
        {
            set_lincs(idef, md, EI_DYNAMICS(ir->eI), cr, constr->lincsd);
        }
        if (ir->eConstrAlg == econtSHAKE)
        {
            if (cr->dd)
            {
                // We are using the local topology, so there are only
                // F_CONSTR constraints.
                make_shake_sblock_dd(constr->shaked, &idef->il[F_CONSTR], &top->cgs, cr->dd);
            }
            else
            {
                make_shake_sblock_serial(constr->shaked, idef, md);
            }
        }
    }

    if (constr->settled)
    {
        settle_set_constraints(constr->settled,
                               &idef->il[F_SETTLE], md);
    }

    /* Make a selection of the local atoms for essential dynamics */
    if (constr->ed && cr->dd)
    {
        dd_make_local_ed_indices(cr->dd, constr->ed);
    }
}

gmx_constr_t init_constraints(FILE *fplog,
                              const gmx_mtop_t *mtop, const t_inputrec *ir,
                              bool doEssentialDynamics,
                              const t_commrec *cr)
{
    int nconstraints =
        gmx_mtop_ftype_count(mtop, F_CONSTR) +
        gmx_mtop_ftype_count(mtop, F_CONSTRNC);
    int nsettles =
        gmx_mtop_ftype_count(mtop, F_SETTLE);

    GMX_RELEASE_ASSERT(!ir->bPull || ir->pull_work != nullptr, "init_constraints called with COM pulling before/without initializing the pull code");

    if (nconstraints + nsettles == 0 &&
        !(ir->bPull && pull_have_constraint(ir->pull_work)) &&
        !doEssentialDynamics)
    {
        return nullptr;
    }

    struct gmx_constr *constr;

    snew(constr, 1);

    constr->ncon_tot = nconstraints;
    constr->nflexcon = 0;
    if (nconstraints > 0)
    {
        constr->n_at2con_mt = mtop->moltype.size();
        snew(constr->at2con_mt, constr->n_at2con_mt);
        for (int mt = 0; mt < static_cast<int>(mtop->moltype.size()); mt++)
        {
            int nflexcon;
            constr->at2con_mt[mt] = make_at2con(0, mtop->moltype[mt].atoms.nr,
                                                mtop->moltype[mt].ilist,
                                                mtop->ffparams.iparams,
                                                EI_DYNAMICS(ir->eI), &nflexcon);
            for (const gmx_molblock_t &molblock : mtop->molblock)
            {
                if (molblock.type == mt)
                {
                    constr->nflexcon += molblock.nmol*nflexcon;
                }
            }
        }

        if (constr->nflexcon > 0)
        {
            if (fplog)
            {
                fprintf(fplog, "There are %d flexible constraints\n",
                        constr->nflexcon);
                if (ir->fc_stepsize == 0)
                {
                    fprintf(fplog, "\n"
                            "WARNING: step size for flexible constraining = 0\n"
                            "         All flexible constraints will be rigid.\n"
                            "         Will try to keep all flexible constraints at their original length,\n"
                            "         but the lengths may exhibit some drift.\n\n");
                    constr->nflexcon = 0;
                }
            }
            if (constr->nflexcon > 0)
            {
                please_cite(fplog, "Hess2002");
            }
        }

        if (ir->eConstrAlg == econtLINCS)
        {
            constr->lincsd = init_lincs(fplog, mtop,
                                        constr->nflexcon, constr->at2con_mt,
                                        DOMAINDECOMP(cr) && cr->dd->bInterCGcons,
                                        ir->nLincsIter, ir->nProjOrder);
        }

        if (ir->eConstrAlg == econtSHAKE)
        {
            if (DOMAINDECOMP(cr) && cr->dd->bInterCGcons)
            {
                gmx_fatal(FARGS, "SHAKE is not supported with domain decomposition and constraint that cross charge group boundaries, use LINCS");
            }
            if (constr->nflexcon)
            {
                gmx_fatal(FARGS, "For this system also velocities and/or forces need to be constrained, this can not be done with SHAKE, you should select LINCS");
            }
            please_cite(fplog, "Ryckaert77a");
            if (ir->bShakeSOR)
            {
                please_cite(fplog, "Barth95a");
            }

            constr->shaked = shake_init();
        }
    }

    if (nsettles > 0)
    {
        please_cite(fplog, "Miyamoto92a");

        constr->bInterCGsettles = inter_charge_group_settles(mtop);

        constr->settled         = settle_init(mtop);

        /* Make an atom to settle index for use in domain decomposition */
        constr->n_at2settle_mt = mtop->moltype.size();
        snew(constr->at2settle_mt, constr->n_at2settle_mt);
        for (size_t mt = 0; mt < mtop->moltype.size(); mt++)
        {
            constr->at2settle_mt[mt] =
                make_at2settle(mtop->moltype[mt].atoms.nr,
                               &mtop->moltype[mt].ilist[F_SETTLE]);
        }

        /* Allocate thread-local work arrays */
        int nthreads = gmx_omp_nthreads_get(emntSETTLE);
        if (nthreads > 1 && constr->vir_r_m_dr_th == nullptr)
        {
            snew(constr->vir_r_m_dr_th, nthreads);
            snew(constr->bSettleErrorHasOccurred, nthreads);
        }
    }

    if (nconstraints + nsettles > 0 && ir->epc == epcMTTK)
    {
        gmx_fatal(FARGS, "Constraints are not implemented with MTTK pressure control.");
    }

    constr->maxwarn = 999;
    char *env       = getenv("GMX_MAXCONSTRWARN");
    if (env)
    {
        constr->maxwarn = 0;
        sscanf(env, "%8d", &constr->maxwarn);
        if (constr->maxwarn < 0)
        {
            constr->maxwarn = INT_MAX;
        }
        if (fplog)
        {
            fprintf(fplog,
                    "Setting the maximum number of constraint warnings to %d\n",
                    constr->maxwarn);
        }
        if (MASTER(cr))
        {
            fprintf(stderr,
                    "Setting the maximum number of constraint warnings to %d\n",
                    constr->maxwarn);
        }
    }
    constr->warncount_lincs  = 0;
    constr->warncount_settle = 0;

    constr->warn_mtop        = mtop;

    return constr;
}

/* Put a pointer to the essential dynamics constraints into the constr struct */
void saveEdsamPointer(gmx_constr_t constr, gmx_edsam_t ed)
{
    constr->ed = ed;
}

const t_blocka *atom2constraints_moltype(gmx_constr_t constr)
{
    return constr->at2con_mt;
}

const int **atom2settle_moltype(gmx_constr_t constr)
{
    return (const int **)constr->at2settle_mt;
}


gmx_bool inter_charge_group_constraints(const gmx_mtop_t *mtop)
{
    const gmx_moltype_t *molt;
    const t_block       *cgs;
    const t_ilist       *il;
    int                 *at2cg, cg, a, ftype, i;
    gmx_bool             bInterCG;

    bInterCG = FALSE;
    for (size_t mb = 0; mb < mtop->molblock.size() && !bInterCG; mb++)
    {
        molt = &mtop->moltype[mtop->molblock[mb].type];

        if (molt->ilist[F_CONSTR].nr   > 0 ||
            molt->ilist[F_CONSTRNC].nr > 0 ||
            molt->ilist[F_SETTLE].nr > 0)
        {
            cgs  = &molt->cgs;
            snew(at2cg, molt->atoms.nr);
            for (cg = 0; cg < cgs->nr; cg++)
            {
                for (a = cgs->index[cg]; a < cgs->index[cg+1]; a++)
                {
                    at2cg[a] = cg;
                }
            }

            for (ftype = F_CONSTR; ftype <= F_CONSTRNC; ftype++)
            {
                il = &molt->ilist[ftype];
                for (i = 0; i < il->nr && !bInterCG; i += 1+NRAL(ftype))
                {
                    if (at2cg[il->iatoms[i+1]] != at2cg[il->iatoms[i+2]])
                    {
                        bInterCG = TRUE;
                    }
                }
            }

            sfree(at2cg);
        }
    }

    return bInterCG;
}

gmx_bool inter_charge_group_settles(const gmx_mtop_t *mtop)
{
    const gmx_moltype_t *molt;
    const t_block       *cgs;
    const t_ilist       *il;
    int                 *at2cg, cg, a, ftype, i;
    gmx_bool             bInterCG;

    bInterCG = FALSE;
    for (size_t mb = 0; mb < mtop->molblock.size() && !bInterCG; mb++)
    {
        molt = &mtop->moltype[mtop->molblock[mb].type];

        if (molt->ilist[F_SETTLE].nr > 0)
        {
            cgs  = &molt->cgs;
            snew(at2cg, molt->atoms.nr);
            for (cg = 0; cg < cgs->nr; cg++)
            {
                for (a = cgs->index[cg]; a < cgs->index[cg+1]; a++)
                {
                    at2cg[a] = cg;
                }
            }

            for (ftype = F_SETTLE; ftype <= F_SETTLE; ftype++)
            {
                il = &molt->ilist[ftype];
                for (i = 0; i < il->nr && !bInterCG; i += 1+NRAL(F_SETTLE))
                {
                    if (at2cg[il->iatoms[i+1]] != at2cg[il->iatoms[i+2]] ||
                        at2cg[il->iatoms[i+1]] != at2cg[il->iatoms[i+3]])
                    {
                        bInterCG = TRUE;
                    }
                }
            }

            sfree(at2cg);
        }
    }

    return bInterCG;
}
