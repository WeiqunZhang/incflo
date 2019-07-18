#include <AMReX_EBMultiFabUtil.H>

#include <incflo.H>
#include <mac_F.H>
#include <convection_F.H>

//
// Compute acc using the vel passed in
//
void incflo::ComputeUGradU(Vector<std::unique_ptr<MultiFab>>& conv_in,
                           Vector<std::unique_ptr<MultiFab>>& vel_in,
                           Real time)
{
	BL_PROFILE("incflo::ComputeUGradU");

    // Extrapolate velocity field to cell faces
    ComputeVelocityAtFaces(vel_in, time);

    // Do projection on all AMR-level_ins in one shot
	mac_projection->apply_projection(m_u_mac, m_v_mac, m_w_mac, ro, time, steady_state);

    for(int lev = 0; lev <= finest_level; lev++)
    {
        Box domain(geom[lev].Domain());

        // Get EB geometric info
        Array< const MultiCutFab*,AMREX_SPACEDIM> areafrac;
        Array< const MultiCutFab*,AMREX_SPACEDIM> facecent;
        const amrex::MultiFab*                    volfrac;
        const amrex::MultiCutFab*                 bndrycent;

        areafrac  =   ebfactory[lev] -> getAreaFrac();
        facecent  =   ebfactory[lev] -> getFaceCent();
        volfrac   = &(ebfactory[lev] -> getVolFrac());
        bndrycent = &(ebfactory[lev] -> getBndryCent());

#ifdef _OPENMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
        for(MFIter mfi(*vel_in[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
        {
            // Tilebox
            Box bx = mfi.tilebox();

            // this is to check efficiently if this tile contains any eb stuff
            const EBFArrayBox& vel_in_fab = static_cast<EBFArrayBox const&>((*vel_in[lev])[mfi]);
            const EBCellFlagFab& flags = vel_in_fab.getEBCellFlagFab();

            if(flags.getType(amrex::grow(bx, 0)) == FabType::covered)
            {
                // If tile is completely covered by EB geometry, set slopes
                // value to some very large number so we know if
                // we accidentaly use these covered slopes later in calculations
                conv_in[lev]->setVal(1.2345e300, bx, 0, 3);
            }
            else
            {
                // No cut cells in tile + nghost-cell witdh halo -> use non-eb routine
                if(flags.getType(amrex::grow(bx, nghost)) == FabType::regular)
                {
		  incflo_compute_ugradu(bx, conv_in, vel_in,
					m_u_mac, m_v_mac, m_w_mac,
					&mfi, domain, lev);
                }
                else
                {
		  incflo_compute_ugradu_eb(bx, conv_in, vel_in,
					   m_u_mac, m_v_mac, m_w_mac, &mfi,
					   areafrac, facecent, volfrac, bndrycent,
					   domain, flags, lev);

                }
            }
        }
	}
}

//
// TODO: Documentation
//
void incflo::ComputeVelocityAtFaces(Vector<std::unique_ptr<MultiFab>>& vel_in,
				    Real time)
{
	BL_PROFILE("incflo::ComputeVelocityAtFaces");

    for(int lev = 0; lev <= finest_level; lev++)
    {
        Box domain(geom[lev].Domain());

        // fill ghost cells
        FillPatchVel(lev, time, *vel_in[lev], 0, vel_in[lev]->nComp());

        // Compute the slopes
        ComputeVelocitySlopes(lev, *vel_in[lev]);

        // Get EB geometric info
        Array<const MultiCutFab*, AMREX_SPACEDIM> areafrac;
        areafrac = ebfactory[lev]->getAreaFrac();

    // Then compute velocity at faces
#ifdef _OPENMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
        for(MFIter mfi(*vel_in[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
        {
            // Tilebox
            Box bx = mfi.tilebox();
            Box ubx = mfi.tilebox(e_x);
            Box vbx = mfi.tilebox(e_y);
            Box wbx = mfi.tilebox(e_z);

            // this is to check efficiently if this tile contains any eb stuff
            const EBFArrayBox& vel_in_fab = static_cast<EBFArrayBox const&>((*vel_in[lev])[mfi]);
            const EBCellFlagFab& flags = vel_in_fab.getEBCellFlagFab();

            Real small_vel = 1.e-10;
            Real  huge_vel = 1.e100;

            // Cell-centered velocity
            const auto& ccvel_fab = vel[lev]->array(mfi);

            // Cell-centered slopes
            const auto& xslopes_fab = (xslopes[lev])->array(mfi);
            const auto& yslopes_fab = (yslopes[lev])->array(mfi);
            const auto& zslopes_fab = (zslopes[lev])->array(mfi);

            // Face-centered velocity components
            const auto& umac_fab = (m_u_mac[lev])->array(mfi);
            const auto& vmac_fab = (m_v_mac[lev])->array(mfi);
            const auto& wmac_fab = (m_w_mac[lev])->array(mfi);

            if(flags.getType(amrex::grow(bx, 0)) == FabType::covered)
            {
                m_u_mac[lev]->setVal(1.2345e300, ubx, 0, 1);
                m_v_mac[lev]->setVal(1.2345e300, vbx, 0, 1);
                m_w_mac[lev]->setVal(1.2345e300, wbx, 0, 1);
            }
            else if(flags.getType(amrex::grow(bx, 1)) == FabType::regular)
            {
                // No cut cells in tile + 1-cell witdh halo -> use non-eb routine
                AMREX_HOST_DEVICE_FOR_3D(ubx, i, j, k,
                {
                    // X-faces
                    Real upls     = ccvel_fab(i  ,j,k,0) - 0.5 * xslopes_fab(i  ,j,k,0);
                    Real umns     = ccvel_fab(i-1,j,k,0) + 0.5 * xslopes_fab(i-1,j,k,0);
                    if ( umns < 0.0 && upls > 0.0 )
                    {
                        umac_fab(i,j,k) = 0.0;
                    }
                    else
                    {
                        Real avg = 0.5 * ( upls + umns );
                        if (std::abs(avg) <  small_vel)
                            umac_fab(i,j,k) = 0.0;
                        else if (avg >= 0)
                            umac_fab(i,j,k) = umns;
                        else
                            umac_fab(i,j,k) = upls;
                    }
                });

                AMREX_HOST_DEVICE_FOR_3D(vbx, i, j, k,
                {
                    // Y-faces
                    Real upls     = ccvel_fab(i,j  ,k,1) - 0.5 * yslopes_fab(i,j  ,k,1);
                    Real umns     = ccvel_fab(i,j-1,k,1) + 0.5 * yslopes_fab(i,j-1,k,1);
                    if ( umns < 0.0 && upls > 0.0 )
                    {
                        vmac_fab(i,j,k) = 0.0;
                    }
                    else
                    {
                        Real avg = 0.5 * ( upls + umns );
                        if (std::abs(avg) <  small_vel)
                            vmac_fab(i,j,k) = 0.0;
                        else if (avg >= 0)
                            vmac_fab(i,j,k) = umns;
                        else
                            vmac_fab(i,j,k) = upls;
                    }
                });

                AMREX_HOST_DEVICE_FOR_3D(wbx, i, j, k,
                {
                    // Z-faces
                    Real upls     = ccvel_fab(i,j,k  ,2) - 0.5 * zslopes_fab(i,j,k  ,2);
                    Real umns     = ccvel_fab(i,j,k-1,2) + 0.5 * zslopes_fab(i,j,k-1,2);
                    if ( umns < 0.0 && upls > 0.0 )
                    {
                        wmac_fab(i,j,k) = 0.0;
                    }
                    else
                    {
                        Real avg = 0.5 * ( upls + umns );
                        if ( std::abs(avg) <  small_vel)
                            wmac_fab(i,j,k) = 0.0;
                        else if (avg >= 0)
                            wmac_fab(i,j,k) = umns;
                        else
                            wmac_fab(i,j,k) = upls;
                    }
                });

            }
            else
            {

                // Face-centered areas
                const auto& ax_fab = areafrac[0]->array(mfi);
                const auto& ay_fab = areafrac[1]->array(mfi);
                const auto& az_fab = areafrac[2]->array(mfi);

                // This FAB has cut cells
                AMREX_HOST_DEVICE_FOR_3D(ubx, i, j, k,
                {
                    // X-faces
                    if (ax_fab(i,j,k) > 0.0)
                    {
                        Real upls     = ccvel_fab(i  ,j,k,0) - 0.5 * xslopes_fab(i  ,j,k,0);
                        Real umns     = ccvel_fab(i-1,j,k,0) + 0.5 * xslopes_fab(i-1,j,k,0);
                        if ( umns < 0.0 && upls > 0.0 )
                        {
                            umac_fab(i,j,k) = 0.0;
                        }
                        else
                        {
                            Real avg = 0.5 * ( upls + umns );
                            if (std::abs(avg) <  small_vel)
                                umac_fab(i,j,k) = 0.0;
                            else if (avg >= 0)
                                umac_fab(i,j,k) = umns;
                            else
                                umac_fab(i,j,k) = upls;
                        }
                    }
                    else
                    {
                        umac_fab(i,j,k) = huge_vel;
                    }
                });

                AMREX_HOST_DEVICE_FOR_3D(vbx, i, j, k,
                {
                    // Y-faces
                    if (ay_fab(i,j,k) > 0.0)
                    {
                        Real upls     = ccvel_fab(i,j  ,k,1) - 0.5 * yslopes_fab(i,j  ,k,1);
                        Real umns     = ccvel_fab(i,j-1,k,1) + 0.5 * yslopes_fab(i,j-1,k,1);
                        if ( umns < 0.0 && upls > 0.0 )
                        {
                            vmac_fab(i,j,k) = 0.0;
                        }
                        else
                        {
                            Real avg = 0.5 * ( upls + umns );
                            if ( std::abs(avg) <  small_vel)
                                vmac_fab(i,j,k) = 0.0;
                            else if (avg >= 0)
                                vmac_fab(i,j,k) = umns;
                            else
                                vmac_fab(i,j,k) = upls;
                        }
                    }
                    else
                    {
                        vmac_fab(i,j,k) = huge_vel;
                    }
                });

                AMREX_HOST_DEVICE_FOR_3D(wbx, i, j, k,
                {
                    // Z-faces
                    if (az_fab(i,j,k) > 0.0)
                    {
                       Real upls     = ccvel_fab(i,j,k  ,2) - 0.5 * zslopes_fab(i,j,k  ,2);
                       Real umns     = ccvel_fab(i,j,k-1,2) + 0.5 * zslopes_fab(i,j,k-1,2);
                       if ( umns < 0.0 && upls > 0.0 )
                       {
                            wmac_fab(i,j,k) = 0.0;
                       }
                       else
                       {
                            Real avg = 0.5 * ( upls + umns );
                            if (std::abs(avg) <  small_vel)
                                wmac_fab(i,j,k) = 0.0;
                            else if (avg >= 0)
                                wmac_fab(i,j,k) = umns;
                            else
                                wmac_fab(i,j,k) = upls;
                       }
                    }
                    else
                    {
                        wmac_fab(i,j,k) = huge_vel;
                    }
                });

            } // Cut cells
        } // MFIter
    } // Levels

#ifdef AMREX_USE_CUDA
    Gpu::Device::synchronize();
#endif

}
//
// Compute the slopes of each velocity component in all three directions.
//
void incflo::ComputeVelocitySlopes(int lev, MultiFab& vel_in)
{
  BL_PROFILE("incflo::ComputeVelocitySlopes");

  EB_set_covered(vel_in, covered_val);

  Box domain(geom[lev].Domain());

#ifdef _OPENMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
  for(MFIter mfi(vel_in, TilingIfNotGPU()); mfi.isValid(); ++mfi) {
    // Tilebox
    Box bx = mfi.tilebox();

    // this is to check efficiently if this tile contains any eb stuff
    const EBFArrayBox& vel_in_fab = static_cast<EBFArrayBox const&>(vel_in[mfi]);
    const EBCellFlagFab& flags = vel_in_fab.getEBCellFlagFab();

    if(flags.getType(amrex::grow(bx, 0)) == FabType::covered) {
      // If tile is completely covered by EB geometry, set slopes
      // value to some very large number so we know if
      // we accidentaly use these covered slopes later in calculations
      xslopes[lev]->setVal(1.2345e300, bx, 0, 3);
      yslopes[lev]->setVal(1.2345e300, bx, 0, 3);
      zslopes[lev]->setVal(1.2345e300, bx, 0, 3);
    }
    else {
      const auto&  vel_fab =      vel_in.array(mfi);
      const auto&   xs_fab = xslopes[lev]->array(mfi);
      const auto&   ys_fab = yslopes[lev]->array(mfi);
      const auto&   zs_fab = zslopes[lev]->array(mfi);

      int ncomp = vel_in.nComp();

      // No cut cells in tile + 1-cell witdh halo -> use non-eb routine
      if(flags.getType(amrex::grow(bx, 1)) == FabType::regular) {
	AMREX_HOST_DEVICE_FOR_4D(bx, ncomp, i, j, k, dir,
	{
	  // X direction
	  Real du_xl = 2.0*(vel_fab(i  ,j,k,dir) - vel_fab(i-1,j,k,dir));
	  Real du_xr = 2.0*(vel_fab(i+1,j,k,dir) - vel_fab(i  ,j,k,dir));
	  Real du_xc = 0.5*(vel_fab(i+1,j,k,dir) - vel_fab(i-1,j,k,dir));

	  Real xslope = amrex::min(std::abs(du_xl),std::abs(du_xc),std::abs(du_xr));
	  xslope            = (du_xr*du_xl > 0.0) ? xslope : 0.0;
	  xs_fab(i,j,k,dir) = (du_xc       > 0.0) ? xslope : -xslope;

	  // Y direction
	  Real du_yl = 2.0*(vel_fab(i,j  ,k,dir) - vel_fab(i,j-1,k,dir));
	  Real du_yr = 2.0*(vel_fab(i,j+1,k,dir) - vel_fab(i,j  ,k,dir));
	  Real du_yc = 0.5*(vel_fab(i,j+1,k,dir) - vel_fab(i,j-1,k,dir));

	  Real yslope = amrex::min(std::abs(du_yl),std::abs(du_yc),std::abs(du_yr));
	  yslope            = (du_yr*du_yl > 0.0) ? yslope : 0.0;
	  ys_fab(i,j,k,dir) = (du_yc       > 0.0) ? yslope : -yslope;

	  // Z direction
	  Real du_zl = 2.0*(vel_fab(i,j,k  ,dir) - vel_fab(i,j,k-1,dir));
	  Real du_zr = 2.0*(vel_fab(i,j,k+1,dir) - vel_fab(i,j,k  ,dir));
	  Real du_zc = 0.5*(vel_fab(i,j,k+1,dir) - vel_fab(i,j,k-1,dir));

	  Real zslope = amrex::min(std::abs(du_zl),std::abs(du_zc),std::abs(du_zr));
	  zslope            = (du_zr*du_zl > 0.0) ? zslope : 0.0;
	  zs_fab(i,j,k,dir) = (du_zc       > 0.0) ? zslope : -zslope;
	});
      }
      else {
	const auto& flag_fab = flags.array();

	AMREX_HOST_DEVICE_FOR_4D(bx, ncomp, i, j, k, dir,
        {
	  if (flag_fab(i,j,k).isCovered()) {
	    xs_fab(i,j,k,dir) = 0.0;
	    ys_fab(i,j,k,dir) = 0.0;
	    zs_fab(i,j,k,dir) = 0.0;
	  }
	  else {
	    // X direction
	    Real du_xl = (flag_fab(i-1,j,k).isCovered()) ? 0.0 :
	      2.0*(vel_fab(i  ,j,k,dir) - vel_fab(i-1,j,k,dir));
	    Real du_xr = (flag_fab(i+1,j,k).isCovered()) ? 0.0 :
	      2.0*(vel_fab(i+1,j,k,dir) - vel_fab(i  ,j,k,dir));
	    Real du_xc = 0.5*(vel_fab(i+1,j,k,dir) - vel_fab(i-1,j,k,dir));
	    
	    Real xslope = amrex::min(std::abs(du_xl),std::abs(du_xc),std::abs(du_xr));
	    xslope            = (du_xr*du_xl > 0.0) ? xslope : 0.0;
	    xs_fab(i,j,k,dir) = (du_xc       > 0.0) ? xslope : -xslope;
	    
	    // Y direction
	    Real du_yl = (flag_fab(i,j-1,k).isCovered()) ? 0.0 :
	      2.0*(vel_fab(i,j  ,k,dir) - vel_fab(i,j-1,k,dir));
	    Real du_yr = (flag_fab(i,j+1,k).isCovered()) ? 0.0 :
	      2.0*(vel_fab(i,j+1,k,dir) - vel_fab(i,j  ,k,dir));
	    Real du_yc = 0.5*(vel_fab(i,j+1,k,dir) - vel_fab(i,j-1,k,dir));
	    
	    Real yslope = amrex::min(std::abs(du_yl),std::abs(du_yc),std::abs(du_yr));
	    yslope            = (du_yr*du_yl > 0.0) ? yslope : 0.0;
	    ys_fab(i,j,k,dir) = (du_yc       > 0.0) ? yslope : -yslope;
	    
	    // Z direction
	    Real du_zl = (flag_fab(i,j,k-1).isCovered()) ? 0.0 :
	      2.0*(vel_fab(i,j,k  ,dir) - vel_fab(i,j,k-1,dir));
	    Real du_zr = (flag_fab(i,j,k+1).isCovered()) ? 0.0 :
	      2.0*(vel_fab(i,j,k+1,dir) - vel_fab(i,j,k  ,dir));
	    Real du_zc = 0.5*(vel_fab(i,j,k+1,dir) - vel_fab(i,j,k-1,dir));
	    
	    Real zslope = amrex::min(std::abs(du_zl),std::abs(du_zc),std::abs(du_zr));
	    zslope          = (du_zr*du_zl > 0.0) ? zslope : 0.0;
	    zs_fab(i,j,k,dir) = (du_zc       > 0.0) ? zslope : -zslope;
	  }
	});
      }

      // TODO -- do we have domain and ilo_fab, etc on GPU???
      // TODO -- we need to use "MINF" from the Fortran, not hard-wire this to 20

      const auto& flag_fab =         flags.array();

      const auto&  ilo_ifab  = bc_ilo[lev]->array();
      const auto&  ihi_ifab  = bc_ihi[lev]->array();
      const auto&  jlo_ifab  = bc_jlo[lev]->array();
      const auto&  jhi_ifab  = bc_jhi[lev]->array();
      const auto&  klo_ifab  = bc_klo[lev]->array();
      const auto&  khi_ifab  = bc_khi[lev]->array();

      AMREX_HOST_DEVICE_FOR_4D(bx, ncomp, i, j, k, dir,
      {
	if ( (i == domain.smallEnd(0)) && !flag_fab(i,j,k).isCovered() && ilo_ifab(i-1,j,k,0) == 20) {
	  Real du_xl = 2.0*(vel_fab(i  ,j,k,dir) - vel_fab(i-1,j,k,dir));
	  Real du_xr = 2.0*(vel_fab(i+1,j,k,dir) - vel_fab(i  ,j,k,dir));
	  Real du_xc = (vel_fab(i+1,j,k,dir)+3.0*vel_fab(i,j,k,dir)-4.0*vel_fab(i-1,j,k,dir))/3.0;
	  
	  Real xslope = amrex::min(std::abs(du_xl),std::abs(du_xc),std::abs(du_xr));
	  xslope            = (du_xr*du_xl > 0.0) ? xslope : 0.0;
	  xs_fab(i,j,k,dir) = (du_xc       > 0.0) ? xslope : -xslope;
	}
	if ( (i == domain.bigEnd(0)) && !flag_fab(i,j,k).isCovered() && ihi_ifab(i+1,j,k,0) == 20) {
	  Real du_xl = 2.0*(vel_fab(i  ,j,k,dir) - vel_fab(i-1,j,k,dir));
	  Real du_xr = 2.0*(vel_fab(i+1,j,k,dir) - vel_fab(i  ,j,k,dir));
	  Real du_xc = -(vel_fab(i-1,j,k,dir)+3.0*vel_fab(i,j,k,dir)-4.0*vel_fab(i+1,j,k,dir))/3.0;

	  Real xslope = amrex::min(std::abs(du_xl),std::abs(du_xc),std::abs(du_xr));
	  xslope            = (du_xr*du_xl > 0.0) ? xslope : 0.0;
	  xs_fab(i,j,k,dir) = (du_xc       > 0.0) ? xslope : -xslope;
	}

	if ( (j == domain.smallEnd(1)) && !flag_fab(i,j,k).isCovered() && jlo_ifab(i,j-1,k,0) == 20) {
	  Real du_yl = 2.0*(vel_fab(i,j  ,k,dir) - vel_fab(i,j-1,k,dir));
	  Real du_yr = 2.0*(vel_fab(i,j+1,k,dir) - vel_fab(i,j  ,k,dir));
	  Real du_yc = (vel_fab(i,j+1,k,dir)+3.0*vel_fab(i,j,k,dir)-4.0*vel_fab(i,j-1,k,dir))/3.0;

	  Real yslope = amrex::min(std::abs(du_yl),std::abs(du_yc),std::abs(du_yr));
	  yslope            = (du_yr*du_yl > 0.0) ? yslope : 0.0;
	  ys_fab(i,j,k,dir) = (du_yc       > 0.0) ? yslope : -yslope;
	}
	if ( (j == domain.bigEnd(1)) && !flag_fab(i,j,k).isCovered() && jhi_ifab(i,j+1,k,0) == 20) {
	  Real du_yl = 2.0*(vel_fab(i,j  ,k,dir) - vel_fab(i,j-1,k,dir));
	  Real du_yr = 2.0*(vel_fab(i,j+1,k,dir) - vel_fab(i,j  ,k,dir));
	  Real du_yc = -(vel_fab(i,j-1,k,dir)+3.0*vel_fab(i,j,k,dir)-4.0*vel_fab(i,j+1,k,dir))/3.0;

	  Real yslope = amrex::min(std::abs(du_yl),std::abs(du_yc),std::abs(du_yr));
	  yslope            = (du_yr*du_yl > 0.0) ? yslope : 0.0;
	  ys_fab(i,j,k,dir) = (du_yc       > 0.0) ? yslope : -yslope;
	}

	if ( (k == domain.smallEnd(2)) && !flag_fab(i,j,k).isCovered() && klo_ifab(i,j,k-1,0) == 20) {
	  Real du_zl = 2.0*(vel_fab(i,j,k  ,dir) - vel_fab(i,j,k-1,dir));
	  Real du_zr = 2.0*(vel_fab(i,j,k+1,dir) - vel_fab(i,j,k  ,dir));
	  Real du_zc = (vel_fab(i,j,k+1,dir)+3.0*vel_fab(i,j,k,dir)-4.0*vel_fab(i,j,k-1,dir))/3.0;

	  Real zslope = amrex::min(std::abs(du_zl),std::abs(du_zc),std::abs(du_zr));
	  zslope            = (du_zr*du_zl > 0.0) ? zslope : 0.0;
	  zs_fab(i,j,k,dir) = (du_zc       > 0.0) ? zslope : -zslope;
	}
	if ( (k == domain.bigEnd(2)) && !flag_fab(i,j,k).isCovered() && khi_ifab(i,j,k+1,0) == 20) {
	  Real du_zl = 2.0*(vel_fab(i,j,k  ,dir) - vel_fab(i,j,k-1,dir));
	  Real du_zr = 2.0*(vel_fab(i,j,k+1,dir) - vel_fab(i,j,k  ,dir));
	  Real du_zc = -(vel_fab(i,j,k-1,dir)+3.0*vel_fab(i,j,k,dir)-4.0*vel_fab(i,j,k+1,dir))/3.0;

	  Real zslope = amrex::min(std::abs(du_zl),std::abs(du_zc),std::abs(du_zr));
	  zslope            = (du_zr*du_zl > 0.0) ? zslope : 0.0;
	  zs_fab(i,j,k,dir) = (du_zc       > 0.0) ? zslope : -zslope;
	}
      });
    }
  }
	
#ifdef AMREX_USE_CUDA
  Gpu::Device::synchronize();
#endif

  xslopes[lev]->FillBoundary(geom[lev].periodicity());
  yslopes[lev]->FillBoundary(geom[lev].periodicity());
  zslopes[lev]->FillBoundary(geom[lev].periodicity());
}

