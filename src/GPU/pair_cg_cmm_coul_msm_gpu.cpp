/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov
   
   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.
   
   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Mike Brown (SNL)
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "pair_cg_cmm_coul_msm_gpu.h"
#include "atom.h"
#include "atom_vec.h"
#include "comm.h"
#include "force.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "integrate.h"
#include "memory.h"
#include "error.h"
#include "neigh_request.h"
#include "universe.h"
#include "update.h"
#include "domain.h"
#include "string.h"
#include "kspace.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
enum {C3=0,C4=1};

// External functions from cuda library for atom decomposition

bool cmmm_gpu_init(const int ntypes, double **cutsq, int **cg_type,
                   double **host_lj1, double **host_lj2, double **host_lj3,
                   double **host_lj4, double **offset, double *special_lj,
                   const int nlocal, const int nall, const int max_nbors,
                   const int maxspecial, const double cell_size, int &gpu_mode,
                   FILE *screen, double **host_cut_ljsq, double host_cut_coulsq,
                   double *host_special_coul, const double qqrd2e,
                   const int smooth);
void cmmm_gpu_clear();
int * cmmm_gpu_compute_n(const int ago, const int inum,
	 	         const int nall, double **host_x, int *host_type, 
                         double *boxlo, double *boxhi, int *tag, int **nspecial,
                         int **special, const bool eflag, const bool vflag,
                         const bool eatom, const bool vatom, int &host_start,
                         const double cpu_time, bool &success, double *host_q);
void cmmm_gpu_compute(const int ago, const int inum, const int nall,
		      double **host_x, int *host_type, int *ilist, int *numj,
		      int **firstneigh, const bool eflag, const bool vflag,
		      const bool eatom, const bool vatom, int &host_start,
		      const double cpu_time, bool &success, double *host_q);
double cmmm_gpu_bytes();

using namespace LAMMPS_NS;

/* ---------------------------------------------------------------------- */

PairCGCMMCoulMSMGPU::PairCGCMMCoulMSMGPU(LAMMPS *lmp) : PairCGCMMCoulMSM(lmp),
                                                        gpu_mode(GPU_PAIR)
{
  respa_enable = 0;
  cpu_time = 0.0;
}

/* ----------------------------------------------------------------------
   free all arrays
------------------------------------------------------------------------- */

PairCGCMMCoulMSMGPU::~PairCGCMMCoulMSMGPU()
{
  cmmm_gpu_clear();
}

/* ---------------------------------------------------------------------- */

void PairCGCMMCoulMSMGPU::compute(int eflag, int vflag)
{
  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = vflag_fdotr = 0;
  
  int nall = atom->nlocal + atom->nghost;
  int inum, host_start;
  
  bool success = true;
  
  if (gpu_mode == GPU_NEIGH) {
    inum = atom->nlocal;
    gpulist = cmmm_gpu_compute_n(neighbor->ago, inum, nall, atom->x,
				 atom->type, domain->sublo, domain->subhi,
				 atom->tag, atom->nspecial, atom->special,
				 eflag, vflag, eflag_atom, vflag_atom,
				 host_start, cpu_time, success, atom->q);
  } else {
    inum = list->inum;
    cmmm_gpu_compute(neighbor->ago, inum, nall, atom->x, atom->type,
		     list->ilist, list->numneigh, list->firstneigh, eflag,
		     vflag, eflag_atom, vflag_atom, host_start, cpu_time,
                     success, atom->q);
  }
  if (!success)
    error->one("Out of memory on GPGPU");

  if (host_start<inum) {
    cpu_time = MPI_Wtime();
    if (gpu_mode == GPU_NEIGH)
      cpu_compute(gpulist, host_start, eflag, vflag);
    else
      cpu_compute(host_start, eflag, vflag);
    cpu_time = MPI_Wtime() - cpu_time;
  }
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairCGCMMCoulMSMGPU::init_style()
{
  PairCGCMMCoulMSM::init_style();
  if (force->pair_match("gpu",0) == NULL)
    error->all("Cannot use pair hybrid with multiple GPU pair styles");

  // Repeat cutsq calculation because done after call to init_style
  double maxcut = -1.0;
  double cut;
  for (int i = 1; i <= atom->ntypes; i++) {
    for (int j = i; j <= atom->ntypes; j++) {
      if (setflag[i][j] != 0 || (setflag[i][i] != 0 && setflag[j][j] != 0)) {
        cut = init_one(i,j);
        cut *= cut;
        if (cut > maxcut)
          maxcut = cut;
        cutsq[i][j] = cutsq[j][i] = cut;
      } else
        cutsq[i][j] = cutsq[j][i] = 0.0;
    }
  }
  double cell_size = sqrt(maxcut) + neighbor->skin;

  int maxspecial=0;
  if (atom->molecular)
    maxspecial=atom->maxspecial;
  bool init_ok = cmmm_gpu_init(atom->ntypes+1, cutsq, cg_type, lj1, lj2, lj3,
                               lj4, offset, force->special_lj, atom->nlocal,
                               atom->nlocal+atom->nghost, 300, maxspecial,
                               cell_size, gpu_mode, screen, cut_ljsq,
                               cut_coulsq_global, force->special_coul,
                               force->qqrd2e,_smooth);
  if (!init_ok)
    error->one("Insufficient memory on accelerator (or no fix gpu).\n"); 

  if (force->newton_pair) 
    error->all("Cannot use newton pair with GPU cg/cmm pair style");

  if (gpu_mode != GPU_NEIGH) {
    int irequest = neighbor->request(this);
    neighbor->requests[irequest]->half = 0;
    neighbor->requests[irequest]->full = 1;
  }
}

/* ---------------------------------------------------------------------- */

double PairCGCMMCoulMSMGPU::memory_usage()
{
  double bytes = Pair::memory_usage();
  return bytes + cmmm_gpu_bytes();
}

/* ---------------------------------------------------------------------- */

void PairCGCMMCoulMSMGPU::cpu_compute(int start, int eflag, int vflag)
{
  int i,j,ii,jj,inum,jnum,itype,jtype,itable;
  double qtmp,xtmp,ytmp,ztmp,delx,dely,delz;
  double fraction,table;
  double r,r2inv,r6inv,forcecoul,forcelj,factor_coul,factor_lj;
  double grij,expm2,prefactor,t,erfc;
  int *ilist,*jlist,*numneigh,**firstneigh;
  double rsq;

  double **x = atom->x;
  double **f = atom->f;
  double *q = atom->q;
  int *type = atom->type;
  int nlocal = atom->nlocal;
  int nall = nlocal + atom->nghost;
  double *special_coul = force->special_coul;
  double *special_lj = force->special_lj;
  double qqrd2e = force->qqrd2e;

  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;
  
  // loop over neighbors of my atoms

  for (ii = start; ii < inum; ii++) {
    i = ilist[ii];
    qtmp = q[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    itype = type[i];
    jlist = firstneigh[i];
    jnum = numneigh[i];

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];

      if (j < nall) factor_coul = factor_lj = 1.0;
      else {
	factor_coul = special_coul[j/nall];
	factor_lj = special_lj[j/nall];
	j %= nall;
      }

      const double delx = xtmp - x[j][0];
      const double dely = ytmp - x[j][1];
      const double delz = ztmp - x[j][2];
      const double rsq = delx*delx + dely*dely + delz*delz;
      const int jtype = type[j];

      double evdwl = 0.0;
      double ecoul = 0.0;
      double fpair = 0.0;

      if (rsq < cutsq[itype][jtype]) {
        const double r2inv = 1.0/rsq;
        const int cgt=cg_type[itype][jtype];

        double forcelj  = 0.0;
        double forcecoul = 0.0;

        if (rsq < cut_ljsq[itype][jtype]) {
          forcelj=factor_lj;
          if (eflag) evdwl=factor_lj;

          if (cgt == CG_LJ12_4) {
            const double r4inv=r2inv*r2inv;
            forcelj *= r4inv*(lj1[itype][jtype]*r4inv*r4inv
                       - lj2[itype][jtype]);
            if (eflag) {
              evdwl *= r4inv*(lj3[itype][jtype]*r4inv*r4inv
                       - lj4[itype][jtype]) - offset[itype][jtype];
            }
          } else if (cgt == CG_LJ9_6) {
            const double r3inv = r2inv*sqrt(r2inv);
            const double r6inv = r3inv*r3inv;
            forcelj *= r6inv*(lj1[itype][jtype]*r3inv
                       - lj2[itype][jtype]);
            if (eflag) {
              evdwl *= r6inv*(lj3[itype][jtype]*r3inv
                        - lj4[itype][jtype]) - offset[itype][jtype];
            }
          } else {
            const double r6inv = r2inv*r2inv*r2inv;
            forcelj *= r6inv*(lj1[itype][jtype]*r6inv
                       - lj2[itype][jtype]);
            if (eflag) {
              evdwl *= r6inv*(lj3[itype][jtype]*r6inv
                       - lj4[itype][jtype]) - offset[itype][jtype];
            }
          }
        }

        if (rsq < cut_coulsq_global) {
          const double ir = 1.0/sqrt(rsq);
          const double prefactor = qqrd2e * qtmp*q[j];
          const double r2_ia2 = rsq*_ia2;
          const double r4_ia4 = r2_ia2*r2_ia2;
          if (_smooth==C3) {
            forcecoul = prefactor*(_ia3*(-4.375+5.25*r2_ia2-1.875*r4_ia4)-
                                   ir/rsq);
            if (eflag)
              ecoul = prefactor*(ir+_ia*(2.1875-2.1875*r2_ia2+
                                         1.3125*r4_ia4-0.3125*r2_ia2*r4_ia4));
          } else {
            const double r6_ia6 = r2_ia2*r4_ia4;
            forcecoul = prefactor*(_ia3*(-6.5625+11.8125*r2_ia2-8.4375*r4_ia4+
                                         2.1875*r6_ia6)-ir/rsq);
            if (eflag)
              ecoul = prefactor*(ir+_ia*(2.4609375-3.28125*r2_ia2+
                                         2.953125*r4_ia4-1.40625*r6_ia6+
                                         0.2734375*r4_ia4*r4_ia4));
          }
          if (factor_coul < 1.0) {
            forcecoul -= (1.0-factor_coul)*prefactor*ir;
            if (eflag) ecoul -= (1.0-factor_coul)*prefactor*ir;
          }
        }
        fpair = forcecoul + forcelj * r2inv;

	f[i][0] += delx*fpair;
	f[i][1] += dely*fpair;
	f[i][2] += delz*fpair;
	if (evflag) ev_tally_full(i,evdwl,ecoul,fpair,delx,dely,delz);
      }
    }
  }
}

/* ---------------------------------------------------------------------- */

void PairCGCMMCoulMSMGPU::cpu_compute(int *nbors, int start, int eflag,
                                      int vflag)
{
  int i,j,jnum,itype,jtype,itable;
  double qtmp,xtmp,ytmp,ztmp,delx,dely,delz;
  double fraction,table;
  double r,r2inv,r6inv,forcecoul,forcelj,factor_coul,factor_lj;
  double grij,expm2,prefactor,t,erfc;
  double rsq;

  double **x = atom->x;
  double **f = atom->f;
  double *q = atom->q;
  int *type = atom->type;
  int nlocal = atom->nlocal;
  int nall = nlocal + atom->nghost;
  int stride = nlocal-start;
  double *special_coul = force->special_coul;
  double *special_lj = force->special_lj;
  double qqrd2e = force->qqrd2e;

  // loop over neighbors of my atoms

  for (i = start; i < nlocal; i++) {
    qtmp = q[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    itype = type[i];
    int *nbor = nbors + i - start;
    jnum = *nbor;
    nbor += stride;
    int *nbor_end = nbor + stride * jnum;

    for (; nbor<nbor_end; nbor+=stride) {
      j = *nbor;

      if (j < nall) factor_coul = factor_lj = 1.0;
      else {
	factor_coul = special_coul[j/nall];
	factor_lj = special_lj[j/nall];
	j %= nall;
      }

      delx = xtmp - x[j][0];
      dely = ytmp - x[j][1];
      delz = ztmp - x[j][2];
      rsq = delx*delx + dely*dely + delz*delz;
      jtype = type[j];

      double evdwl = 0.0;
      double ecoul = 0.0;
      double fpair = 0.0;

      if (rsq < cutsq[itype][jtype]) {
        const double r2inv = 1.0/rsq;
        const int cgt=cg_type[itype][jtype];

        double forcelj  = 0.0;
        double forcecoul = 0.0;

        if (rsq < cut_ljsq[itype][jtype]) {
          forcelj=factor_lj;
          if (eflag) evdwl=factor_lj;

          if (cgt == CG_LJ12_4) {
            const double r4inv=r2inv*r2inv;
            forcelj *= r4inv*(lj1[itype][jtype]*r4inv*r4inv
                       - lj2[itype][jtype]);
            if (eflag) {
              evdwl *= r4inv*(lj3[itype][jtype]*r4inv*r4inv
                       - lj4[itype][jtype]) - offset[itype][jtype];
            }
          } else if (cgt == CG_LJ9_6) {
            const double r3inv = r2inv*sqrt(r2inv);
            const double r6inv = r3inv*r3inv;
            forcelj *= r6inv*(lj1[itype][jtype]*r3inv
                       - lj2[itype][jtype]);
            if (eflag) {
              evdwl *= r6inv*(lj3[itype][jtype]*r3inv
                        - lj4[itype][jtype]) - offset[itype][jtype];
            }
          } else {
            const double r6inv = r2inv*r2inv*r2inv;
            forcelj *= r6inv*(lj1[itype][jtype]*r6inv
                       - lj2[itype][jtype]);
            if (eflag) {
              evdwl *= r6inv*(lj3[itype][jtype]*r6inv
                       - lj4[itype][jtype]) - offset[itype][jtype];
            }
          }
        }

        if (rsq < cut_coulsq_global) {
          const double ir = 1.0/sqrt(rsq);
          const double prefactor = qqrd2e * qtmp*q[j];
          const double r2_ia2 = rsq*_ia2;
          const double r4_ia4 = r2_ia2*r2_ia2;
          if (_smooth==C3) {
            forcecoul = prefactor*(_ia3*(-4.375+5.25*r2_ia2-1.875*r4_ia4)-
                                   ir/rsq);
            if (eflag)
              ecoul = prefactor*(ir+_ia*(2.1875-2.1875*r2_ia2+
                                         1.3125*r4_ia4-0.3125*r2_ia2*r4_ia4));
          } else {
            const double r6_ia6 = r2_ia2*r4_ia4;
            forcecoul = prefactor*(_ia3*(-6.5625+11.8125*r2_ia2-8.4375*r4_ia4+
                                         2.1875*r6_ia6)-ir/rsq);
            if (eflag)
              ecoul = prefactor*(ir+_ia*(2.4609375-3.28125*r2_ia2+
                                         2.953125*r4_ia4-1.40625*r6_ia6+
                                         0.2734375*r4_ia4*r4_ia4));
          }
          if (factor_coul < 1.0) {
            forcecoul -= (1.0-factor_coul)*prefactor*ir;
            if (eflag) ecoul -= (1.0-factor_coul)*prefactor*ir;
          }
        }
        fpair = forcecoul + forcelj * r2inv;

	f[i][0] += delx*fpair;
	f[i][1] += dely*fpair;
	f[i][2] += delz*fpair;

        if (j<start) {
  	  if (evflag) ev_tally_full(i,evdwl,ecoul,fpair,delx,dely,delz);
        } else {
          if (j<nlocal) {
	    f[j][0] -= delx*fpair;
	    f[j][1] -= dely*fpair;
	    f[j][2] -= delz*fpair;
  	  }
	  if (evflag) ev_tally(i,j,nlocal,0,
			       evdwl,ecoul,fpair,delx,dely,delz);
        }
      }
    }
  }
}

