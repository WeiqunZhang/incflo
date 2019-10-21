#include <NodalProjection.H>
#include <AMReX.H>
#include <incflo.H>
#include <AMReX_EBMultiFabUtil.H>
#include <incflo_proj_F.H>

void
NodalProjection::define (const incflo* a_incflo)
{
    m_incflo = a_incflo;
    m_ok     = true;
}

//
// Perform projection:
//
//     vel = vel - sigma*grad(phi)
//
//  where phi is the solution of
//
//   div( sigma * grad(phi) ) = div(vel) + S_cc + S_nd
//
//  where vel, sigma, S_cc are cell-centered variables
//  and phi and S_nd are nodal variables
//
//  grad(phi) is node-centered.
//
void
NodalProjection::project (      Vector< std::unique_ptr< amrex::MultiFab > >& a_vel,
                          const Vector< std::unique_ptr< amrex::MultiFab > >& a_sigma,
                          const Vector< std::unique_ptr< amrex::MultiFab > >& a_S_cc,
                          const Vector< std::unique_ptr< amrex::MultiFab > >& a_S_nd )
{
    AMREX_ALWAYS_ASSERT(m_ok);
    BL_PROFILE("NodalProjection::project");

    amrex::Print() << "Nodal Projection:" << std::endl;

    // Setup object for projection and initialize value sof internals
    setup();

    // Compute RHS
    m_matrix -> compRHS( GetVecOfPtrs(m_rhs),  GetVecOfPtrs(a_vel), GetVecOfConstPtrs(a_S_cc),
                         GetVecOfPtrs(a_S_nd) );

    // Print diagnostics
    amrex::Print() << " >> Before projection:" << std::endl;
    printInfo();

    // Set matrix coefficients
    for (int lev(0); lev < a_sigma.size(); ++lev)
        m_matrix -> setSigma(lev, *a_sigma[lev]);

    // Solve
    m_solver -> solve( GetVecOfPtrs(m_phi), GetVecOfConstPtrs(m_rhs), m_mg_rtol, m_mg_atol );

    // Get fluxes -- fluxes = - sigma*grad(phi)
    m_solver -> getFluxes( GetVecOfPtrs(m_fluxes) );

    // Perform projection
    for (int lev(0); lev < m_phi.size(); ++lev)
    {
        // vel = vel + fluxes = vel - sigma * grad(phi),
        MultiFab::Add( *a_vel[lev], *m_fluxes[lev], 0, 0, AMREX_SPACEDIM, 0);

        // set m_fluxes = -fluxes/sigma = grad(phi)
        m_fluxes[lev] -> mult(- 1.0, m_fluxes[lev]->nGrow() );
        for (int n(0); n < AMREX_SPACEDIM; ++n)
            MultiFab::Divide(*m_fluxes[lev], *a_sigma[lev], 0, n, 1, m_fluxes[lev]->nGrow() );

        // Fill boundaries and apply scale factor to phi
        m_phi[lev] -> FillBoundary( m_incflo -> geom[lev].periodicity() );
    }

    // Compute RHS -- this is only needed to print out post projection values
    m_matrix -> compRHS( GetVecOfPtrs(m_rhs),  GetVecOfPtrs(a_vel), GetVecOfConstPtrs(a_S_cc),
                         GetVecOfPtrs(a_S_nd) );

    // Print diagnostics
    amrex::Print() << " >> After projection:" << std::endl;
    printInfo();
}


//
// Read from input file
//
void
NodalProjection::readParameters ()
{
    ParmParse pp("mfix");
    pp.query( "mg_verbose"             , m_mg_verbose );
    pp.query( "mg_cg_verbose"          , m_mg_cg_verbose );
    pp.query( "mg_maxiter"             , m_mg_maxiter );
    pp.query( "mg_cg_maxiter"          , m_mg_cg_maxiter );
    pp.query( "mg_rtol"                , m_mg_rtol );
    pp.query( "mg_atol"                , m_mg_atol );
    pp.query( "mg_max_coarsening_level", m_mg_max_coarsening_level );
    pp.query( "bottom_solver_type"     , m_bottom_solver_type );
}


//
// Setup object before solve
//
void
NodalProjection::setup ()
{
    BL_PROFILE("NodalProjection::setup");
    AMREX_ALWAYS_ASSERT(m_ok);

    readParameters();

    // Set number of levels
    int nlev( m_incflo -> grids.size() );

    // Resize member data if necessary
    if ( nlev != m_phi.size() )
    {
        m_phi.resize(nlev);
        m_fluxes.resize(nlev);
        m_rhs.resize(nlev);
    }

    // Regrid if necessary
    int nghost(1);      // We use 1 ghost node only -- it should be enough

    bool need_regrid(false);  // if BA and DM changed on any level, we need
                              // to update the matrix and the solver as well

    for (int lev(0); lev < nlev; ++lev )
    {
        const auto& ba = m_incflo -> grids[lev];
        const auto& dm = m_incflo -> dmap[lev];
        const auto& eb = *(m_incflo -> ebfactory[lev]);

        if ( (m_phi[lev] == nullptr)                 ||
             (m_phi[lev] -> boxArray()        != ba) ||
             (m_phi[lev] -> DistributionMap() != dm)  )
        {
            // Cell-centered data
            m_fluxes[lev].reset(new MultiFab(ba, dm, 3, nghost, MFInfo(), eb));

            // Node-centered data
            const auto& ba_nd = amrex::convert(ba, IntVect{1,1,1});
            m_phi[lev].reset(new MultiFab(ba_nd, dm, 1, nghost, MFInfo(), eb));
            m_rhs[lev].reset(new MultiFab(ba_nd, dm, 1, nghost, MFInfo(), eb));

            need_regrid = true;
        }

    }

    // Setup matrix and solver
    if ( (m_matrix == nullptr) || need_regrid )
    {

        // Setup boundary conditions
        Box domain(m_incflo -> geom[0].Domain());
        int bc_lo[3], bc_hi[3];
        set_ppe_bcs(bc_lo, bc_hi,
                    domain.loVect(), domain.hiVect(),
                    &nghost,
                    m_incflo -> bc_ilo[0]->dataPtr(),
                    m_incflo -> bc_ihi[0]->dataPtr(),
                    m_incflo -> bc_jlo[0]->dataPtr(),
                    m_incflo -> bc_jhi[0]->dataPtr(),
                    m_incflo -> bc_klo[0]->dataPtr(),
                    m_incflo -> bc_khi[0]->dataPtr());


        //
        // Setup Matrix
        //
        LPInfo                       info;
        info.setMaxCoarseningLevel(m_mg_max_coarsening_level);
        m_matrix.reset(new MLNodeLaplacian(m_incflo->geom, m_incflo->grids,
                                           m_incflo->dmap, info,
                                           GetVecOfConstPtrs(m_incflo->ebfactory)));

        m_matrix->setGaussSeidel(true);
        m_matrix->setHarmonicAverage(false);

        m_matrix->setDomainBC(
            {(LinOpBCType) bc_lo[0], (LinOpBCType) bc_lo[1], (LinOpBCType) bc_lo[2]},
            {(LinOpBCType) bc_hi[0], (LinOpBCType) bc_hi[1], (LinOpBCType) bc_hi[2]}
            );



        //
        // Setup solver
        //
        m_solver.reset(new MLMG(*m_matrix));

        m_solver->setMaxIter(m_mg_maxiter);
        m_solver->setVerbose(m_mg_verbose);
        m_solver->setCGVerbose(m_mg_cg_verbose);
        m_solver->setCGMaxIter(m_mg_cg_maxiter);

        if (m_bottom_solver_type == "smoother")
        {
            m_solver->setBottomSolver(MLMG::BottomSolver::smoother);
        }
        else if (m_bottom_solver_type == "bicg")
        {
            m_solver->setBottomSolver(MLMG::BottomSolver::bicgstab);
        }
        else if (m_bottom_solver_type == "cg")
        {
            m_solver->setBottomSolver(MLMG::BottomSolver::cg);
        }
        else if (m_bottom_solver_type == "bicgcg")
        {
            m_solver->setBottomSolver(MLMG::BottomSolver::bicgcg);
        }
        else if (m_bottom_solver_type == "cgbicg")
        {
            m_solver->setBottomSolver(MLMG::BottomSolver::cgbicg);
        }
        else if (m_bottom_solver_type == "hypre")
        {
#ifdef AMREX_USE_HYPRE
            m_solver->setBottomSolver(MLMG::BottomSolver::hypre);
#else
            amrex::Abort("AMReX was not built with HYPRE support");
#endif
        }

    }

    // Initialize all variables
    for (int lev(0); lev < nlev; ++lev)
    {
        m_phi[lev] -> setVal(0.0);
        m_fluxes[lev] -> setVal(0.0);
        m_rhs[lev] ->  setVal(0.0);
    }

}



void
NodalProjection::printInfo ()
{
    for (int lev(0); lev < m_rhs.size(); ++lev)
    {
        amrex::Print() << "  * On lev " << lev
                       << " max(abs(divu)) = "
                       << m_rhs[lev]->norm0(0,0,false,true)
                       << std::endl;
    }
}
