#include <physics/mam/eamxx_mam_optics_process_interface.hpp>
#include <share/property_checks/field_lower_bound_check.hpp>
#include <share/property_checks/field_within_interval_check.hpp>

#include "scream_config.h" // for SCREAM_CIME_BUILD

#include <ekat/ekat_assert.hpp>

namespace scream
{

MAMOptics::MAMOptics(
    const ekat::Comm& comm,
    const ekat::ParameterList& params)
  : AtmosphereProcess(comm, params),
    aero_config_() {
}

AtmosphereProcessType MAMOptics::type() const {
  return AtmosphereProcessType::Physics;
}

std::string MAMOptics::name() const {
  return "mam4_optics";
}

void MAMOptics::set_grids(const std::shared_ptr<const GridsManager> grids_manager) {
  using namespace ekat::units;

  grid_ = grids_manager->get_grid("Physics");
  const auto& grid_name = grid_->name();

  ncol_ = grid_->get_num_local_dofs();      // number of columns on this rank
  nlev_ = grid_->get_num_vertical_levels(); // number of levels per column
  nswbands_ = mam4::modal_aer_opt::nswbands;//14;                           // number of shortwave bands
  nlwbands_ = mam4::modal_aer_opt::nlwbands;//16;                           // number of longwave bands

  // Define the different field layouts that will be used for this process
  using namespace ShortFieldTagsNames;

  // Define aerosol optics fields computed by this process.
  auto nondim = Units::nondimensional();
  FieldLayout scalar3d_swband_layout { {COL, SWBND, LEV}, {ncol_, nswbands_, nlev_} };
  FieldLayout scalar3d_lwband_layout { {COL, LWBND, LEV}, {ncol_, nlwbands_, nlev_} };

  // layout for 3D (2d horiz X 1d vertical) variables
  FieldLayout scalar3d_layout_mid{ {COL, LEV}, {ncol_, nlev_} };

  FieldLayout scalar3d_layout_int{ {COL, ILEV}, {ncol_, nlev_+1} };

  add_field<Required>("T_mid", scalar3d_layout_mid, K, grid_name); // Temperature
  add_field<Required>("p_mid", scalar3d_layout_mid, Pa, grid_name); // total pressure
  add_field<Required>("cldfrac_tot", scalar3d_layout_mid, nondim, grid_name); // total pressure
  add_field<Required>("z_int", scalar3d_layout_int, m, grid_name); // vertical position at interface
  add_field<Required>("z_mid", scalar3d_layout_mid, m, grid_name); // vertical position pressure
  add_field<Required>("p_int", scalar3d_layout_int, Pa, grid_name); // total pressure
  add_field<Required>("pseudo_density", scalar3d_layout_mid, Pa, grid_name);
  add_field<Required>("pseudo_density_dry", scalar3d_layout_mid, Pa, grid_name);


  // shortwave aerosol scattering asymmetry parameter [-]
  add_field<Computed>("aero_g_sw",   scalar3d_swband_layout, nondim, grid_name);
  // shortwave aerosol single-scattering albedo [-]
  add_field<Computed>("aero_ssa_sw", scalar3d_swband_layout, nondim, grid_name);
  // shortwave aerosol optical depth [-]
  add_field<Computed>("aero_tau_sw", scalar3d_swband_layout, nondim, grid_name);
  // longwave aerosol optical depth [-]
  add_field<Computed>("aero_tau_lw", scalar3d_lwband_layout, nondim, grid_name);

  // FIXME: this field doesn't belong here, but this is a convenient place to
  // FIXME: put it for now.
  // number mixing ratio for CCN
  using Spack      = ekat::Pack<Real,SCREAM_SMALL_PACK_SIZE>;
  using Pack       = ekat::Pack<Real,Spack::n>;
  constexpr int ps = Pack::n;
  // FieldLayout scalar3d_layout_mid { {COL, LEV}, {ncol_, nlev_} };
  add_field<Computed>("nccn", scalar3d_layout_mid, 1/kg, grid_name, ps);
}

void MAMOptics::initialize_impl(const RunType run_type) {

  dry_atm_.T_mid     = get_field_in("T_mid").get_view<const Real**>();
  dry_atm_.p_mid     = get_field_in("p_mid").get_view<const Real**>();
  // FIXME, there are two version of p_int in the nc file: p_dry_int and p_int
  // change to const Real
  p_int_     = get_field_in("p_int").get_view<const Real**>();

  dry_atm_.cldfrac   = get_field_in("cldfrac_tot").get_view<const Real**>(); // FIXME: tot or liq?
  // dry_atm_.pblh      = get_field_in("pbl_height").get_view<const Real*>();
  // FIXME: use const Real; why are using buffer in microphysics
  z_mid_     = get_field_in("z_mid").get_view<const Real**>();
  z_iface_   = get_field_in("z_int").get_view<const Real**>();

  p_del_     = get_field_in("pseudo_density").get_view<const Real**>();
  // FIXME: In the nc file, there is also pseudo_density_dry
  dry_atm_.p_del     = get_field_in("pseudo_density_dry").get_view<const Real**>();

  // FIXME: we have nvars in several process.
  constexpr int nvars = mam4::ndrop::nvars;
  constexpr int nlwbands = mam4::modal_aer_opt::nlwbands;
  constexpr int maxd_aspectype = mam4::ndrop::maxd_aspectype;
  constexpr int ntot_amode = mam4::AeroConfig::num_modes();

  state_q_ = mam_coupling::view_2d("state_q_", nlev_, nvars);
  Kokkos::deep_copy(state_q_,10);
  ext_cmip6_lw_ = mam_coupling::view_2d("ext_cmip6_lw_", nlev_, nlwbands);
  odap_aer_ = mam_coupling::view_2d("odap_aer_", nlev_, nlwbands);
  specrefndxlw_  = mam_coupling::complex_view_2d("specrefndxlw_",nlwbands, maxd_aspectype );

  for (int d1 = 0; d1 < ntot_amode; ++d1)
  for (int d5 = 0; d5 < nlwbands; ++d5) {
        absplw_[d1][d5] =
            mam_coupling::view_3d("absplw_", mam4::modal_aer_opt::coef_number,
                                             mam4::modal_aer_opt::refindex_real,
                                             mam4::modal_aer_opt::refindex_im);
  }

  for (int d1 = 0; d1 < ntot_amode; ++d1)
  for (int d3 = 0; d3 < nlwbands; ++d3) {
        refrtablw_[d1][d3] = mam_coupling::view_1d("refrtablw",
        mam4::modal_aer_opt::refindex_real);
  } // d3

  for (int d1 = 0; d1 < ntot_amode; ++d1)
      for (int d3 = 0; d3 < nlwbands; ++d3) {
        refitablw_[d1][d3] = mam_coupling::view_1d("refitablw",
         mam4::modal_aer_opt::refindex_im);
  } // d3
  // FIXME: work arrays
  mass_ = mam_coupling::view_1d("mass",nlev_);
  cheb_ = mam_coupling::view_2d("cheb", mam4::modal_aer_opt::coef_number, nlev_);

  dgnumwet_m_ = mam_coupling::view_2d("dgnumwet_m", nlev_, ntot_amode);
  dgnumdry_m_ = mam_coupling::view_2d("dgnumdry_m", nlev_, ntot_amode);

  radsurf_ = mam_coupling::view_1d("radsurf",nlev_);
  logradsurf_ = mam_coupling::view_1d("logradsurf",nlev_);

  specrefindex_=mam_coupling::complex_view_2d("specrefindex",
   mam4::modal_aer_opt::max_nspec, nlwbands);
  qaerwat_m_ = mam_coupling::view_2d ("qaerwat_m", nlev_, ntot_amode);

  ext_cmip6_lw_inv_m_ = mam_coupling::view_2d ("qaerwat_m", nlev_, nlwbands);

}
void MAMOptics::run_impl(const double dt) {

  const auto policy = ekat::ExeSpaceUtils<KT::ExeSpace>::get_default_team_policy(ncol_, nlev_);

  // get the aerosol optics fields
  auto aero_g_sw   = get_field_out("aero_g_sw").get_view<Real***>();
  auto aero_ssa_sw = get_field_out("aero_ssa_sw").get_view<Real***>();
  auto aero_tau_sw = get_field_out("aero_tau_sw").get_view<Real***>();
  auto aero_tau_lw = get_field_out("aero_tau_lw").get_view<Real***>();

  printf("dt %e\n",dt);

  auto aero_nccn   = get_field_out("nccn").get_view<Real**>(); // FIXME: get rid of this
  // constexpr int pver = mam4::nlev;
  constexpr int ntot_amode=mam4::AeroConfig::num_modes();
  constexpr int maxd_aspectype = mam4::ndrop::maxd_aspectype;
  constexpr int nspec_max = mam4::ndrop::nspec_max;

  if (false) { // remove when ready to do actual calculations
    // populate these fields with reasonable representative values
    Kokkos::deep_copy(aero_g_sw, 0.5);
    Kokkos::deep_copy(aero_ssa_sw, 0.7);
    Kokkos::deep_copy(aero_tau_sw, 0.0);
    Kokkos::deep_copy(aero_tau_lw, 0.0);
    Kokkos::deep_copy(aero_nccn, 50.0);
  } else {


    // Compute optical properties on all local columns.
    // (Strictly speaking, we don't need this parallel_for here yet, but we leave
    //  it in anticipation of column-specific aerosol optics to come.)
    Kokkos::parallel_for(policy, KOKKOS_LAMBDA(const ThreadTeam& team) {
      const Int icol = team.league_rank(); // column index

      auto g_sw = ekat::subview(aero_g_sw, icol);
      auto ssa_sw = ekat::subview(aero_ssa_sw, icol);
      auto tau_sw = ekat::subview(aero_tau_sw, icol);
      auto tau_lw = ekat::subview(aero_tau_lw, icol);

      // FIXME: Get rid of this
      // auto nccn = ekat::subview(aero_nccn, icol);
      auto pmid =  ekat::subview(dry_atm_.p_mid, icol);
      auto temperature =  ekat::subview(dry_atm_.T_mid, icol);
      auto cldn = ekat::subview(dry_atm_.cldfrac, icol);

      // FIXME: interface pressure [Pa]
      auto pint =  ekat::subview(p_int_, icol);
      auto zm =  ekat::subview(z_mid_, icol);
      // FIXME: dry mass pressure interval [Pa]
      // FIXME:
      auto zi= ekat::subview(z_iface_, icol);
      auto pdel = ekat::subview(p_del_, icol);
      auto pdeldry = ekat::subview(dry_atm_.p_del, icol);
      printf("temperature %e\n",temperature(0));
      printf("pmid %e\n",pmid(0));
      printf("cldn %e\n",cldn(0));
      printf("state_q_ %e\n",state_q_(0,0));
      printf("zm %e\n",zm(0));
      printf("pdeldry %e\n",pdeldry(0));
      printf("pdel %e\n",pdel(0));

      int nspec_amode[ntot_amode];

          int lspectype_amode[maxd_aspectype][ntot_amode];
          int lmassptr_amode[maxd_aspectype][ntot_amode];
          Real specdens_amode[maxd_aspectype];
          Real spechygro[maxd_aspectype];
          int numptr_amode[ntot_amode];
          int mam_idx[ntot_amode][nspec_max];
          int mam_cnst_idx[ntot_amode][nspec_max];

          mam4::ndrop::get_e3sm_parameters(nspec_amode, lspectype_amode, lmassptr_amode,
                              numptr_amode, specdens_amode, spechygro, mam_idx,
                              mam_cnst_idx);

          Real sigmag_amode[ntot_amode] = {0.18000000000000000e+001,
                                           0.16000000000000001e+001,
                                           0.18000000000000000e+001,
                                           0.16000000238418579e+001};

#if 0
          calcsize_process.compute_tendencies(team, t, dt, atm, sfc, progs,
                                              diags, tends);
#endif

          team.team_barrier();
          // FIXME Try to avoid this deep copy.
          for (int imode = 0; imode < ntot_amode; ++imode) {
            for (int kk = 0; kk < pver; ++kk) {
              dgnumdry_m(kk, imode) =
                  diags.dry_geometric_mean_diameter_i[imode](kk);
            }
          }


  #if 1
 mam4::aer_rad_props::aer_rad_props_lw(
    dt, pmid, pint,
    temperature, zm, zi,
    state_q_, pdel, pdeldry,
    cldn, ext_cmip6_lw_,
    // const ColumnView qqcw_fld[pcnst],
    odap_aer_,
    //
    nspec_amode, sigmag_amode,
    lmassptr_amode,
    spechygro, specdens_amode,
    lspectype_amode,
    specrefndxlw_,
    crefwlw_,
    crefwsw_,
    absplw_,
    refrtablw_,
    refitablw_,
    // work views
    mass_, cheb_, dgnumwet_m_,
    dgnumdry_m_, radsurf_,
    logradsurf_, specrefindex_,
    qaerwat_m_, ext_cmip6_lw_inv_m_);

#endif
    });

    Kokkos::deep_copy(aero_g_sw, 0.5);
    Kokkos::deep_copy(aero_ssa_sw, 0.7);
    Kokkos::deep_copy(aero_tau_sw, 0.0);
    Kokkos::deep_copy(aero_tau_lw, 0.0);
    Kokkos::deep_copy(aero_nccn, 50.0);


  }
  printf("Oscar is here... done \n");

}

void MAMOptics::finalize_impl()
{
}

} // namespace scream
