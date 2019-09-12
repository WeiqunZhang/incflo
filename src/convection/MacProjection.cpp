#include <AMReX_EBFArrayBox.H>
#include <AMReX_EBMultiFabUtil.H>
#include <AMReX_MacProjector.H>
#include <AMReX_MultiFabUtil.H>
#include <AMReX_ParmParse.H>

#include <MacProjection.H>
#include <boundary_conditions_F.H>
#include <mac_F.H>
#include <projection_F.H>
#include <setup_F.H>

// Define unit vectors to easily convert indices
extern const amrex::IntVect e_x(1, 0, 0);
extern const amrex::IntVect e_y(0, 1, 0);
extern const amrex::IntVect e_z(0, 0, 1);

using namespace amrex;

// Constructor
MacProjection::MacProjection(AmrCore* a_amrcore,
							 int a_nghost,
							 Vector<std::unique_ptr<EBFArrayBoxFactory>>* a_ebfactory, 
                             int a_probtype)
{
	m_amrcore = a_amrcore;
	m_nghost = a_nghost;
	m_ebfactory = a_ebfactory;
    m_probtype = a_probtype;

	read_inputs();
}

// Destructor
MacProjection::~MacProjection()
{
}

// Read inputs
void MacProjection::read_inputs()
{
	ParmParse pp("mac");

	// Option to control MLMG behavior
	pp.query("verbose", verbose);
	pp.query("mg_verbose", mg_verbose);
	pp.query("mg_rtol", mg_rtol);
	pp.query("mg_atol", mg_atol);

   // Default bottom solver is bicgcg, but alternatives are 
   // "smoother", "hypre", "cg", "cgbicg", "bicgstab"
   bottom_solver_type = "bicgcg";
   pp.query( "bottom_solver_type",  bottom_solver_type );
}

// Set boundary conditions
void MacProjection::set_bcs(Vector<std::unique_ptr<IArrayBox>>& a_bc_ilo,
							Vector<std::unique_ptr<IArrayBox>>& a_bc_ihi,
							Vector<std::unique_ptr<IArrayBox>>& a_bc_jlo,
							Vector<std::unique_ptr<IArrayBox>>& a_bc_jhi,
							Vector<std::unique_ptr<IArrayBox>>& a_bc_klo,
							Vector<std::unique_ptr<IArrayBox>>& a_bc_khi)
{
	m_bc_ilo = &a_bc_ilo;
	m_bc_ihi = &a_bc_ihi;
	m_bc_jlo = &a_bc_jlo;
	m_bc_jhi = &a_bc_jhi;
	m_bc_klo = &a_bc_klo;
	m_bc_khi = &a_bc_khi;

	int bc_lo[AMREX_SPACEDIM], bc_hi[AMREX_SPACEDIM];
    int lev = 0;
	Box domain(m_amrcore->Geom(0).Domain());

    set_ppe_bc(bc_lo, bc_hi,
               domain.loVect(), domain.hiVect(),
               &m_nghost,
#if (AMREX_SPACEDIM == 2)
               (*m_bc_ilo)[lev]->dataPtr(), (*m_bc_ihi)[lev]->dataPtr(),
               (*m_bc_jlo)[lev]->dataPtr(), (*m_bc_jhi)[lev]->dataPtr());

    m_lobc = {(LinOpBCType)bc_lo[0], (LinOpBCType)bc_lo[1]};
    m_hibc = {(LinOpBCType)bc_hi[0], (LinOpBCType)bc_hi[1]};
#elif (AMREX_SPACEDIM == 3)
               (*m_bc_ilo)[lev]->dataPtr(), (*m_bc_ihi)[lev]->dataPtr(),
               (*m_bc_jlo)[lev]->dataPtr(), (*m_bc_jhi)[lev]->dataPtr(),
               (*m_bc_klo)[lev]->dataPtr(), (*m_bc_khi)[lev]->dataPtr());

    m_lobc = {(LinOpBCType)bc_lo[0], (LinOpBCType)bc_lo[1], (LinOpBCType)bc_lo[2]};
    m_hibc = {(LinOpBCType)bc_hi[0], (LinOpBCType)bc_hi[1], (LinOpBCType)bc_hi[2]};
#endif
}

// redefine working arrays if amrcore has changed
void MacProjection::update_internals()
{

	if(m_divu.size() != (m_amrcore->finestLevel() + 1))
	{
		m_divu.resize(m_amrcore->finestLevel() + 1);
		 m_phi.resize(m_amrcore->finestLevel() + 1);
		   m_b.resize(m_amrcore->finestLevel() + 1);
		  m_ro.resize(m_amrcore->finestLevel() + 1);
	}

	for(int lev = 0; lev <= m_amrcore->finestLevel(); ++lev)
	{

		if(m_divu[lev] == nullptr ||
		   !BoxArray::SameRefs(m_divu[lev]->boxArray(), m_amrcore->boxArray(lev)) ||
		   !DistributionMapping::SameRefs(m_divu[lev]->DistributionMap(),
										  m_amrcore->DistributionMap(lev)))
		{

            m_divu[lev].reset(new MultiFab(m_amrcore->boxArray(lev),
                                           m_amrcore->DistributionMap(lev), 1, m_nghost, 
                                           MFInfo(), *((*m_ebfactory)[lev])));

			m_phi[lev].reset(new MultiFab(m_amrcore->boxArray(lev),
                                          m_amrcore->DistributionMap(lev), 1, m_nghost,
                                          MFInfo(), *((*m_ebfactory)[lev])));

            m_phi[lev]->setVal(0.);

			// Staggered quantities
			BoxArray x_ba = m_amrcore->boxArray(lev);
			x_ba = x_ba.surroundingNodes(0);
             m_b[lev][0].reset(new MultiFab(x_ba, m_amrcore->DistributionMap(lev), 1, m_nghost,
                                           MFInfo(), *((*m_ebfactory)[lev])));
            m_ro[lev][0].reset(new MultiFab(x_ba, m_amrcore->DistributionMap(lev), 1, m_nghost,
                                           MFInfo(), *((*m_ebfactory)[lev])));


			BoxArray y_ba = m_amrcore->boxArray(lev);
			y_ba = y_ba.surroundingNodes(1);
             m_b[lev][1].reset(new MultiFab(y_ba, m_amrcore->DistributionMap(lev), 1, m_nghost,
                                           MFInfo(), *((*m_ebfactory)[lev])));
            m_ro[lev][1].reset(new MultiFab(y_ba, m_amrcore->DistributionMap(lev), 1, m_nghost,
                                           MFInfo(), *((*m_ebfactory)[lev])));

			BoxArray z_ba = m_amrcore->boxArray(lev);
			z_ba = z_ba.surroundingNodes(2);
             m_b[lev][2].reset(new MultiFab(z_ba, m_amrcore->DistributionMap(lev), 1, m_nghost,
                                           MFInfo(), *((*m_ebfactory)[lev])));
            m_ro[lev][2].reset(new MultiFab(z_ba, m_amrcore->DistributionMap(lev), 1, m_nghost,
                                           MFInfo(), *((*m_ebfactory)[lev])));
		};
	}
}

//
// Computes the following decomposition:
//
//    u + c*grad(phi)/ro = u*  with  div(u) = 0
//
// Inputs:
//
//   lev    = the AMR level
//   u,v,w  = the MAC velocity field to be projected
//   ro     = the cell-centered density
//
// Outputs:
//
//  phi     = the projection auxiliary function
//  u,v,w   = the PROJECTED MAC velocity field
//
// Notes:
//
//  phi is computed by solving
//
//       div(grad(phi)/ro) = div(u*)
//
//  WARNING: this method returns the MAC velocity with up-to-date BCs in place
//
void MacProjection::apply_projection(Vector<std::unique_ptr<MultiFab>>& u,
									 Vector<std::unique_ptr<MultiFab>>& v,
									 Vector<std::unique_ptr<MultiFab>>& w,
									 const Vector<std::unique_ptr<MultiFab>>& ro, 
                                     Real time, int steady_state)
{
    BL_PROFILE("MacProjection::apply_projection()");

    if (verbose)
	Print() << "MAC Projection:\n";

    // Check that everything is consistent with amrcore
    update_internals();

    // Setup for solve
    Vector<Array<MultiFab*, AMREX_SPACEDIM>> vel;
    vel.resize(m_amrcore->finestLevel() + 1);

    if(verbose) Print() << " >> Before projection\n";

    for(int lev = 0; lev <= m_amrcore->finestLevel(); ++lev)
    {
        // Compute beta coefficients ( div(beta*grad(phi)) = RHS )
        average_cellcenter_to_face(GetArrOfPtrs(m_ro[lev]), *ro[lev], m_amrcore->Geom(lev));

        for(int dir = 0; dir < AMREX_SPACEDIM; dir++)
        {
            m_b[lev][dir]->setVal(1.0);
            MultiFab::Divide(*m_b[lev][dir], *m_ro[lev][dir], 0, 0, 1, 0);
        }

	// Set velocity bcs
	set_velocity_bcs(lev, u, v, w, time);

	// Store in temporaries
	(vel[lev])[0] = u[lev].get();
	(vel[lev])[1] = v[lev].get();
	(vel[lev])[2] = w[lev].get();

	if(verbose)
	{
            // Fill boundaries before printing div(u) 
            for(int i = 0; i < AMREX_SPACEDIM; i++)
                (vel[lev])[i]->FillBoundary(m_amrcore->Geom(lev).periodicity());

		EB_computeDivergence(*m_divu[lev], GetArrOfConstPtrs(vel[lev]), m_amrcore->Geom(lev));

		Print() << "  * On level " << lev << " max(abs(divu)) = " << norm0(m_divu, lev)
				<< "\n";
        }
    }

    //
    // If we want to set max_coarsening_level we have to send it in to the constructor
    //
    LPInfo lp_info;
    // lp_info.setMaxCoarseningLevel(mac_mg_max_coarsening_level);

    //
    // Perform MAC projection
    //
    MacProjector macproj(vel, GetVecOfArrOfPtrsConst(m_b), m_amrcore->Geom(), lp_info);

    macproj.setDomainBC(m_lobc, m_hibc);

    // The default bottom solver is BiCG
    if(bottom_solver_type == "smoother")
    {
       macproj.setBottomSolver(MLMG::BottomSolver::smoother);
    }
    else if(bottom_solver_type == "hypre")
    {
       macproj.setBottomSolver(MLMG::BottomSolver::hypre);
    }

    // Verbosity for MultiGrid / ConjugateGradients
	macproj.setVerbose(mg_verbose);

    if (steady_state)
    {
        // Solve using m_phi as an initial guess
        macproj.project(GetVecOfPtrs(m_phi), mg_rtol, mg_atol);
    }
    else
    {
        // Solve using initial guess of zero
        macproj.project(mg_rtol, mg_atol);
    }

	if(verbose)
		Print() << " >> After projection\n";

	for(int lev = 0; lev <= m_amrcore->finestLevel(); ++lev)
	{
		if(verbose)
		{
            // Fill boundaries before printing div(u) 
            for(int i = 0; i < AMREX_SPACEDIM; i++)
                (vel[lev])[i]->FillBoundary(m_amrcore->Geom(lev).periodicity());

			EB_computeDivergence(*m_divu[lev], GetArrOfConstPtrs(vel[lev]), m_amrcore->Geom(lev));

			Print() << "  * On level " << lev << " max(abs(divu)) = " << norm0(m_divu, lev)
					<< "\n";
		}
        
		// Set velocity bcs
		set_velocity_bcs(lev, u, v, w, time);
	}
}

//
// Set the BCs for velocity only
//
void MacProjection::set_velocity_bcs(int lev,
									 Vector<std::unique_ptr<MultiFab>>& u,
									 Vector<std::unique_ptr<MultiFab>>& v,
									 Vector<std::unique_ptr<MultiFab>>& w, 
                                     Real time)
{
	BL_PROFILE("MacProjection::set_velocity_bcs()");

	u[lev]->FillBoundary(m_amrcore->Geom(lev).periodicity());
	v[lev]->FillBoundary(m_amrcore->Geom(lev).periodicity());
	w[lev]->FillBoundary(m_amrcore->Geom(lev).periodicity());

	Box domain(m_amrcore->Geom(lev).Domain());

#ifdef _OPENMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
	for(MFIter mfi(*m_divu[lev], false); mfi.isValid(); ++mfi)
	{
		const Box& bx = (*m_divu[lev])[mfi].box();

		set_mac_velocity_bcs(&time, 
                             bx.loVect(),
							 bx.hiVect(),
							 BL_TO_FORTRAN_ANYD((*u[lev])[mfi]),
							 BL_TO_FORTRAN_ANYD((*v[lev])[mfi]),
							 BL_TO_FORTRAN_ANYD((*w[lev])[mfi]),
							 (*m_bc_ilo)[lev]->dataPtr(),
							 (*m_bc_ihi)[lev]->dataPtr(),
							 (*m_bc_jlo)[lev]->dataPtr(),
							 (*m_bc_jhi)[lev]->dataPtr(),
							 (*m_bc_klo)[lev]->dataPtr(),
							 (*m_bc_khi)[lev]->dataPtr(),
							 domain.loVect(),
							 domain.hiVect(),
							 &m_nghost, &m_probtype);
	}
}

//
// Norm 0 for EB Multifab
//
Real MacProjection::norm0(const Vector<std::unique_ptr<MultiFab>>& mf, int lev)
{
	MultiFab mf_tmp(mf[lev]->boxArray(),
					mf[lev]->DistributionMap(),
					mf[lev]->nComp(),
					0,
					MFInfo(),
					*(*m_ebfactory)[lev]);

	MultiFab::Copy(mf_tmp, *mf[lev], 0, 0, 1, 0);
	EB_set_covered(mf_tmp, 0.0);

	return mf_tmp.norm0(0);
}
