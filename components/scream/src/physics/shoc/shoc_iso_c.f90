module shoc_iso_c
  use iso_c_binding
  implicit none

#include "scream_config.f"
#ifdef SCREAM_DOUBLE_PRECISION
# define c_real c_double
#else
# define c_real c_float
#endif

!
! This file contains bridges from scream c++ to shoc fortran.
!

contains
  subroutine append_precision(string, prefix)

    character(kind=c_char, len=128), intent(inout) :: string
    character(*), intent(in) :: prefix
    real(kind=c_real) :: s

    write (string, '(a,i1,a1)') prefix, sizeof(s), C_NULL_CHAR
  end subroutine append_precision

  subroutine shoc_init_c(nlev, gravit, rair, rh2o, cpair, &
                         zvir, latvap, latice, karman) bind(c)
    use shoc, only: shoc_init, npbl

    integer(kind=c_int), value, intent(in) :: nlev ! number of levels

    real(kind=c_real), value, intent(in)  :: gravit ! gravity
    real(kind=c_real), value, intent(in)  :: rair   ! dry air gas constant
    real(kind=c_real), value, intent(in)  :: rh2o   ! water vapor gas constant
    real(kind=c_real), value, intent(in)  :: cpair  ! specific heat of dry air
    real(kind=c_real), value, intent(in)  :: zvir   ! rh2o/rair - 1
    real(kind=c_real), value, intent(in)  :: latvap ! latent heat of vaporization
    real(kind=c_real), value, intent(in)  :: latice ! latent heat of fusion
    real(kind=c_real), value, intent(in)  :: karman ! Von Karman's constant

    real(kind=c_real) :: pref_mid(nlev) ! unused values

    pref_mid = 0
    call shoc_init(nlev, gravit, rair, rh2o, cpair, &
                   zvir, latvap, latice, karman, &
                   pref_mid, nlev, 1)
    npbl = nlev ! set pbl layer explicitly so we don't need pref_mid.
  end subroutine shoc_init_c

  subroutine shoc_main_c(shcol,nlev,nlevi,dtime,nadv,host_dx, host_dy, thv,  &
     zt_grid, zi_grid, pres, presi, pdel, wthl_sfc, wqw_sfc, uw_sfc, vw_sfc, &
     wtracer_sfc, num_qtracers, w_field, exner,phis, host_dse, tke, thetal,  &
     qw, u_wind, v_wind, qtracers, wthv_sec, tkh, tk, shoc_ql, shoc_cldfrac, &
     pblh, shoc_mix, isotropy, w_sec, thl_sec, qw_sec, qwthl_sec, wthl_sec,  &
     wqw_sec, wtke_sec, uw_sec, vw_sec, w3, wqls_sec, brunt, shoc_ql2) bind(C)
    use shoc, only : shoc_main

    integer(kind=c_int), value, intent(in) :: shcol, nlev, nlevi, num_qtracers, nadv
    real(kind=c_real), value, intent(in) :: dtime
    real(kind=c_real), intent(in), dimension(shcol) :: host_dx, host_dy
    real(kind=c_real), intent(in), dimension(shcol, nlev) :: zt_grid
    real(kind=c_real), intent(in), dimension(shcol, nlevi) :: zi_grid
    real(kind=c_real), intent(in), dimension(shcol, nlev) :: pres
    real(kind=c_real), intent(in), dimension(shcol, nlevi) :: presi
    real(kind=c_real), intent(in), dimension(shcol, nlev) :: pdel, thv, w_field
    real(kind=c_real), intent(in), dimension(shcol) :: wthl_sfc, wqw_sfc, uw_sfc, vw_sfc
    real(kind=c_real), intent(in), dimension(shcol, num_qtracers) :: wtracer_sfc
    real(kind=c_real), intent(in), dimension(shcol, nlev) :: exner
    real(kind=c_real), intent(in), dimension(shcol) :: phis

    real(kind=c_real), intent(inout), dimension(shcol, nlev) :: host_dse, tke, &
       thetal, qw, u_wind, v_wind, wthv_sec
    real(kind=c_real), intent(inout), dimension(shcol, nlev, num_qtracers) :: qtracers
    real(kind=c_real), intent(inout), dimension(shcol, nlev) :: tk, tkh

    real(kind=c_real), intent(out), dimension(shcol, nlev) :: shoc_cldfrac, shoc_ql
    real(kind=c_real), intent(out), dimension(shcol) :: pblh
    real(kind=c_real), intent(out), dimension(shcol, nlev) :: shoc_mix, w_sec
    real(kind=c_real), intent(out), dimension(shcol, nlevi) :: thl_sec, qw_sec, &
       qwthl_sec, wthl_sec, wqw_sec, wtke_sec, uw_sec, vw_sec, w3
    real(kind=c_real), intent(out), dimension(shcol, nlev) :: wqls_sec, isotropy, &
         brunt,shoc_ql2

    call shoc_main(shcol, nlev, nlevi, dtime, nadv, host_dx, host_dy, thv,   &
     zt_grid, zi_grid, pres, presi, pdel, wthl_sfc, wqw_sfc, uw_sfc, vw_sfc, &
     wtracer_sfc, num_qtracers, w_field, exner, phis, host_dse, tke, thetal, &
     qw, u_wind, v_wind, qtracers, wthv_sec, tkh, tk, shoc_ql, shoc_cldfrac, &
     pblh, shoc_mix, isotropy, w_sec, thl_sec, qw_sec, qwthl_sec, wthl_sec,  &
     wqw_sec, wtke_sec, uw_sec, vw_sec, w3, wqls_sec, brunt,shoc_ql2 )
  end subroutine shoc_main_c

  subroutine shoc_use_cxx_c(arg_use_cxx) bind(C)
    use shoc, only: use_cxx

    logical(kind=c_bool), value, intent(in) :: arg_use_cxx

    use_cxx = arg_use_cxx
  end subroutine shoc_use_cxx_c

  subroutine shoc_grid_c(shcol,nlev,nlevi,zt_grid,zi_grid,pdel,dz_zt,dz_zi,rho_zt) bind (C)
    use shoc, only: shoc_grid

    integer(kind=c_int), intent(in), value :: shcol
    integer(kind=c_int), intent(in), value :: nlev
    integer(kind=c_int), intent(in), value :: nlevi
    real(kind=c_real), intent(in) :: zt_grid(shcol,nlev)
    real(kind=c_real), intent(in) :: zi_grid(shcol,nlevi)
    real(kind=c_real), intent(in) :: pdel(shcol,nlev)

    real(kind=c_real), intent(out) :: dz_zt(shcol,nlev)
    real(kind=c_real), intent(out) :: dz_zi(shcol,nlevi)
    real(kind=c_real), intent(out) :: rho_zt(shcol,nlev)

    call shoc_grid(shcol,nlev,nlevi,zt_grid,zi_grid,pdel,dz_zt,dz_zi,rho_zt)

  end subroutine shoc_grid_c
  
  subroutine calc_shoc_varorcovar_c(&
       shcol,nlev,nlevi,tunefac,&                ! Input
       isotropy_zi,tkh_zi,dz_zi,invar1,invar2,&  ! Input
       varorcovar)bind (C)                       ! Input/Output 
       
      use shoc, only: calc_shoc_varorcovar

      integer(kind=c_int), intent(in), value :: shcol
      integer(kind=c_int), intent(in), value :: nlev
      integer(kind=c_int), intent(in), value :: nlevi 
      real(kind=c_real), intent(in), value :: tunefac
      real(kind=c_real), intent(in) :: isotropy_zi(shcol,nlevi)
      real(kind=c_real), intent(in) :: tkh_zi(shcol,nlevi)
      real(kind=c_real), intent(in) :: dz_zi(shcol,nlevi)
      real(kind=c_real), intent(in) :: invar1(shcol,nlev)
      real(kind=c_real), intent(in) :: invar2(shcol,nlev)
	 
      real(kind=c_real), intent(inout) :: varorcovar(shcol,nlevi)
      
      call calc_shoc_varorcovar(&
           shcol,nlev,nlevi,tunefac,&               ! Input
           isotropy_zi,tkh_zi,dz_zi,invar1,invar2,& ! Input
           varorcovar)  
	   
  end subroutine calc_shoc_varorcovar_c      


  subroutine calc_shoc_vertflux_c(shcol, nlev, nlevi, tkh_zi, dz_zi, invar, vertflux) bind (C)
    use shoc, only: calc_shoc_vertflux

    integer(kind=c_int), intent(in), value :: shcol
    integer(kind=c_int), intent(in), value :: nlev
    integer(kind=c_int), intent(in), value :: nlevi
    real(kind=c_real), intent(in) :: tkh_zi(shcol,nlevi)
    real(kind=c_real), intent(in) :: dz_zi(shcol,nlevi)
    real(kind=c_real), intent(in) :: invar(shcol,nlev)

    real(kind=c_real), intent(inout) :: vertflux(shcol,nlevi)

    call calc_shoc_vertflux(shcol, nlev, nlevi, tkh_zi, dz_zi, invar, vertflux)

  end subroutine calc_shoc_vertflux_c

  
  subroutine compute_brunt_shoc_length_c(nlev,nlevi,shcol,dz_zt,thv,thv_zi,brunt) bind (C)
    use shoc, only: compute_brunt_shoc_length

    integer(kind=c_int), intent(in), value :: nlev
    integer(kind=c_int), intent(in), value :: nlevi
    integer(kind=c_int), intent(in), value :: shcol
    real(kind=c_real), intent(in) :: dz_zt(shcol,nlev)
    real(kind=c_real), intent(in) :: thv(shcol,nlev)  
    real(kind=c_real), intent(in) :: thv_zi(shcol,nlevi)

    real(kind=c_real), intent(out) :: brunt(shcol,nlev)

    call compute_brunt_shoc_length(nlev,nlevi,shcol,dz_zt,thv,thv_zi,brunt)

  end subroutine compute_brunt_shoc_length_c  
  
  
  subroutine compute_l_inf_shoc_length_c(nlev,shcol,zt_grid,dz_zt,tke,l_inf) bind (C)
    use shoc, only: compute_l_inf_shoc_length

    integer(kind=c_int), intent(in), value :: nlev
    integer(kind=c_int), intent(in), value :: shcol
    real(kind=c_real), intent(in) :: zt_grid(shcol,nlev)
    real(kind=c_real), intent(in) :: dz_zt(shcol,nlev)
    real(kind=c_real), intent(in) :: tke(shcol,nlev)  

    real(kind=c_real), intent(out) :: l_inf(shcol)

    call compute_l_inf_shoc_length(nlev,shcol,zt_grid,dz_zt,tke,l_inf)

  end subroutine compute_l_inf_shoc_length_c  
  
  
  subroutine compute_conv_vel_shoc_length_c(nlev,shcol,pblh,zt_grid,dz_zt,&
                                            thv,wthv_sec,conv_vel) bind (C)
    use shoc, only: compute_conv_vel_shoc_length

    integer(kind=c_int), intent(in), value :: nlev
    integer(kind=c_int), intent(in), value :: shcol
    real(kind=c_real), intent(in) :: pblh(shcol)
    real(kind=c_real), intent(in) :: zt_grid(shcol,nlev)
    real(kind=c_real), intent(in) :: dz_zt(shcol,nlev)
    real(kind=c_real), intent(in) :: thv(shcol,nlev)
    real(kind=c_real), intent(in) :: wthv_sec(shcol,nlev)  

    real(kind=c_real), intent(out) :: conv_vel(shcol)

    call compute_conv_vel_shoc_length(nlev,shcol,pblh,zt_grid,dz_zt,thv,wthv_sec,conv_vel)

  end subroutine compute_conv_vel_shoc_length_c   

end module shoc_iso_c
