#include <physics/mam/eamxx_mam_microphysics_process_interface.hpp>
#include <share/io/scream_scorpio_interface.hpp>
#include <share/property_checks/field_lower_bound_check.hpp>
#include <share/property_checks/field_within_interval_check.hpp>

#include "scream_config.h" // for SCREAM_CIME_BUILD

#include <netcdf.h> // for serial NetCDF file reads on MPI root
#include <ekat/ekat_assert.hpp>

// NOTE: see the impl/ directory for the contents of the impl namespace
#include "impl/compute_o3_column_density.cpp"
#include "impl/compute_water_content.cpp"
#include "impl/gas_phase_chemistry.cpp"

namespace scream
{

MAMMicrophysics::MAMMicrophysics(
    const ekat::Comm& comm,
    const ekat::ParameterList& params)
  : AtmosphereProcess(comm, params),
    aero_config_() {
  configure(params);
}

AtmosphereProcessType MAMMicrophysics::type() const {
  return AtmosphereProcessType::Physics;
}

std::string MAMMicrophysics::name() const {
  return "mam4_micro";
}

namespace {

void set_data_file(const char *name, const char *path, char location[MAX_FILENAME_LEN]) {
  EKAT_REQUIRE_MSG(strlen(SCREAM_DATA_DIR) + strlen(path) < MAX_FILENAME_LEN,
    "Error! " << name << " path is too long (must be < " << MAX_FILENAME_LEN << " characters)");
  sprintf(location, "%s/%s", SCREAM_DATA_DIR, path);
}

}

#define set_file_location(data_file, path) set_data_file(#data_file, path, data_file)

void MAMMicrophysics::set_defaults_() {
  config_.amicphys.do_cond = true;
  config_.amicphys.do_rename = true;
  config_.amicphys.do_newnuc = true;
  config_.amicphys.do_coag = true;

  config_.amicphys.nucleation = {};
  config_.amicphys.nucleation.dens_so4a_host = 1770.0;
  config_.amicphys.nucleation.mw_so4a_host = 115.0;
  config_.amicphys.nucleation.newnuc_method_user_choice = 2;
  config_.amicphys.nucleation.pbl_nuc_wang2008_user_choice = 1;
  config_.amicphys.nucleation.adjust_factor_pbl_ratenucl = 1.0;
  config_.amicphys.nucleation.accom_coef_h2so4 = 1.0;
  config_.amicphys.nucleation.newnuc_adjust_factor_dnaitdt = 1.0;

  // these parameters guide the coupling between parameterizations
  // NOTE: mam4xx was ported with these parameters fixed, so it's probably not
  // NOTE: safe to change these without code modifications.
  config_.amicphys.gaexch_h2so4_uptake_optaa = 2;
  config_.amicphys.newnuc_h2so4_conc_optaa = 2;

  //===========================================================
  // default data file locations (relative to SCREAM_DATA_DIR)
  //===========================================================

  // many of these paths were extracted from
  // e3smv2/bld/namelist_files/namelist_defaults_eam.xml

  // photolysis
  set_file_location(config_.photolysis.rsf_file,         "../waccm/phot/RSF_GT200nm_v3.0_c080811.nc");
  set_file_location(config_.photolysis.xs_long_file,     "../waccm/phot/temp_prs_GT200nm_JPL10_c130206.nc");

  // stratospheric chemistry
  set_file_location(config_.linoz.chlorine_loading_file, "../cam/chem/trop_mozart/ub/Linoz_Chlorine_Loading_CMIP6_0003-2017_c20171114.nc");
}

void MAMMicrophysics::configure(const ekat::ParameterList& params) {
  set_defaults_();
  // FIXME: implement "namelist" parsing
}

void MAMMicrophysics::set_grids(const std::shared_ptr<const GridsManager> grids_manager) {
  using namespace ekat::units;

  auto q_unit = kg/kg; // mass mixing ratios [kg stuff / kg air]
  q_unit.set_string("kg/kg");
  auto n_unit = 1/kg;  // number mixing ratios [# / kg air]
  n_unit.set_string("#/kg");
  Units nondim(0,0,0,0,0,0,0);
  const auto m2 = m*m;
  const auto s2 = s*s;

  grid_ = grids_manager->get_grid("Physics");
  const auto& grid_name = grid_->name();

  ncol_ = grid_->get_num_local_dofs();      // number of columns on this rank
  nlev_ = grid_->get_num_vertical_levels(); // number of levels per column

  // get column geometry and locations
  col_areas_      = grid_->get_geometry_data("area").get_view<const Real*>();
  col_latitudes_  = grid_->get_geometry_data("lat").get_view<const Real*>();
  col_longitudes_ = grid_->get_geometry_data("lon").get_view<const Real*>();

  // define the different field layouts that will be used for this process
  using namespace ShortFieldTagsNames;

  // layout for 2D (1d horiz X 1d vertical) variable
  FieldLayout scalar2d_layout_col{ {COL}, {ncol_} };

  // layout for 3D (2d horiz X 1d vertical) variables
  FieldLayout scalar3d_layout_mid{ {COL, LEV}, {ncol_, nlev_} };

  // define fields needed in mam4xx

  // atmospheric quantities
  add_field<Required>("omega", scalar3d_layout_mid, Pa/s, grid_name); // vertical pressure velocity
  add_field<Required>("T_mid", scalar3d_layout_mid, K, grid_name); // Temperature
  add_field<Required>("p_mid", scalar3d_layout_mid, Pa, grid_name); // total pressure
  add_field<Required>("qv", scalar3d_layout_mid, q_unit, grid_name, "tracers"); // specific humidity
  add_field<Required>("qi", scalar3d_layout_mid, q_unit, grid_name, "tracers"); // ice wet mixing ratio
  add_field<Required>("ni", scalar3d_layout_mid, n_unit, grid_name, "tracers"); // ice number mixing ratio
  add_field<Required>("pbl_height", scalar2d_layout_col, m, grid_name); // planetary boundary layer height
  add_field<Required>("pseudo_density", scalar3d_layout_mid, Pa, grid_name); // p_del, hydrostatic pressure
  add_field<Required>("phis",           scalar2d_layout_col, m2/s2, grid_name);
  add_field<Required>("cldfrac_tot", scalar3d_layout_mid, nondim, grid_name); // cloud fraction

  // droplet activation can alter cloud liquid and number mixing ratios
  add_field<Updated>("qc", scalar3d_layout_mid, q_unit, grid_name, "tracers"); // cloud liquid wet mixing ratio
  add_field<Updated>("nc", scalar3d_layout_mid, n_unit, grid_name, "tracers"); // cloud liquid wet number mixing ratio

  // (interstitial) aerosol tracers of interest: mass (q) and number (n) mixing ratios
  for (int m = 0; m < mam_coupling::num_aero_modes(); ++m) {
    const char* int_nmr_field_name = mam_coupling::int_aero_nmr_field_name(m);
    add_field<Updated>(int_nmr_field_name, scalar3d_layout_mid, n_unit, grid_name, "tracers");
    for (int a = 0; a < mam_coupling::num_aero_species(); ++a) {
      const char* int_mmr_field_name = mam_coupling::int_aero_mmr_field_name(m, a);
      if (strlen(int_mmr_field_name) > 0) {
        add_field<Updated>(int_mmr_field_name, scalar3d_layout_mid, q_unit, grid_name, "tracers");
      }
    }
  }

  // aerosol-related gases: mass mixing ratios
  for (int g = 0; g < mam_coupling::num_aero_gases(); ++g) {
    const char* gas_mmr_field_name = mam_coupling::gas_mmr_field_name(g);
    add_field<Updated>(gas_mmr_field_name, scalar3d_layout_mid, q_unit, grid_name, "tracers");
  }

  // Tracers group -- do we need this in addition to the tracers above? In any
  // case, this call should be idempotent, so it can't hurt.
  add_group<Updated>("tracers", grid_name, 1, Bundling::Required);
}

// this checks whether we have the tracers we expect
void MAMMicrophysics::
set_computed_group_impl(const FieldGroup& group) {
  const auto& name = group.m_info->m_group_name;
  EKAT_REQUIRE_MSG(name=="tracers",
    "Error! MAM4 expects a 'tracers' field group (got '" << name << "')\n");

  EKAT_REQUIRE_MSG(group.m_info->m_bundled,
    "Error! MAM4 expects bundled fields for tracers.\n");

  // how many aerosol/gas tracers do we expect?
  int num_tracers = 2 * (mam_coupling::num_aero_modes() +
                         mam_coupling::num_aero_tracers()) +
                    mam_coupling::num_aero_gases();
  EKAT_REQUIRE_MSG(group.m_info->size() >= num_tracers,
    "Error! MAM4 requires at least " << num_tracers << " aerosol tracers.");
}

size_t MAMMicrophysics::requested_buffer_size_in_bytes() const
{
  return mam_coupling::buffer_size(ncol_, nlev_);
}

void MAMMicrophysics::init_buffers(const ATMBufferManager &buffer_manager) {
  EKAT_REQUIRE_MSG(buffer_manager.allocated_bytes() >= requested_buffer_size_in_bytes(),
                   "Error! Insufficient buffer size.\n");

  size_t used_mem = mam_coupling::init_buffer(buffer_manager, ncol_, nlev_, buffer_);
  EKAT_REQUIRE_MSG(used_mem==requested_buffer_size_in_bytes(),
                   "Error! Used memory != requested memory for MAMMicrophysics.");
}

namespace {

using HostView1D    = haero::DeviceType::view_1d<Real>::HostMirror;
using HostViewInt1D = haero::DeviceType::view_1d<int>::HostMirror;

// ON HOST (MPI root rank only), reads the dimension of a NetCDF variable from
// the file with the given ID
int nc_dimension(const char *file, int nc_id, const char *dim_name) {
  int dim_id;
  int result = nc_inq_dimid(nc_id, dim_name, &dim_id);
  EKAT_REQUIRE_MSG(result == 0, "Error! Couldn't fetch " << dim_name <<
    " dimension ID from NetCDF file '" << file << "'\n");
  size_t dim;
  result = nc_inq_dimlen(nc_id, dim_id, &dim);
  EKAT_REQUIRE_MSG(result == 0, "Error! Couldn't fetch " << dim_name <<
    " dimension from NetCDF file '" << file << "'\n");
  return static_cast<int>(dim);
}

// ON HOST (MPI root rank only), reads data from the given NetCDF variable from
// the file with the given ID into the given Kokkos host View
template <typename V>
void read_nc_var(const char *file, int nc_id, const char *var_name, V host_view) {
  int var_id;
  int result = nc_inq_varid(nc_id, var_name, &var_id);
  EKAT_REQUIRE_MSG(result == 0, "Error! Couldn't fetch ID for variable '" << var_name <<
    "' from NetCDF file '" << file << "'\n");
  result = nc_get_var(nc_id, var_id, host_view.data());
  EKAT_REQUIRE_MSG(result == 0, "Error! Couldn't read data for variable '" << var_name <<
    "' from NetCDF file '" << file << "'\n");
}

// ON HOST (MPI root rank only), reads data from the NetCDF variable with the
// given ID, from the file with the given ID, into the given Kokkos host View
template <typename V>
void read_nc_var(const char *file, int nc_id, int var_id, V host_view) {
  int result = nc_get_var(nc_id, var_id, host_view.data());
  EKAT_REQUIRE_MSG(result == 0, "Error! Couldn't read data for variable with ID " <<
    var_id << " from NetCDF file '" << file << "'\n");
}

// ON HOST (MPI root only), sets the lng_indexer and pht_alias_mult_1 host views
// according to parameters in our (hardwired) chemical mechanism
void set_lng_indexer_and_pht_alias_mult_1(const char *file, int nc_id,
                                          HostViewInt1D lng_indexer,
                                          HostView1D pht_alias_mult_1) {
  // NOTE: it seems that the chemical mechanism we're using
  // NOTE: 1. sets pht_alias_lst to a blank string [1]
  // NOTE: 2. sets pht_alias_mult_1 to 1.0 [1]
  // NOTE: 3. sets rxt_tag_lst to ['jh2o2', 'usr_HO2_HO2', 'usr_SO2_OH', 'usr_DMS_OH'] [2]
  // NOTE: References:
  // NOTE: [1] (https://github.com/eagles-project/e3sm_mam4_refactor/blob/refactor-maint-2.0/components/eam/src/chemistry/pp_linoz_mam4_resus_mom_soag/mo_sim_dat.F90#L117)
  // NOTE: [2] (https://github.com/eagles-project/e3sm_mam4_refactor/blob/refactor-maint-2.0/components/eam/src/chemistry/pp_linoz_mam4_resus_mom_soag/mo_sim_dat.F90#L99)

  // populate lng_indexer (see https://github.com/eagles-project/e3sm_mam4_refactor/blob/refactor-maint-2.0/components/eam/src/chemistry/mozart/mo_jlong.F90#L180)
  static const char *var_names[4] = {"jh2o2", "usr_HO2_HO2", "usr_SO2_OH", "usr_DMS_OH"};
  for (int m = 0; m < mam4::mo_photo::phtcnt; ++m) {
    int var_id;
    int result = nc_inq_varid(nc_id, var_names[m], &var_id);
    EKAT_REQUIRE_MSG(result == 0, "Error! Couldn't fetch ID for variable '"
      << var_names[m] << "' from NetCDF file '" << file << "'\n");
    lng_indexer(m) = var_id;
  }

  // set pht_alias_mult_1 to 1
  Kokkos::deep_copy(pht_alias_mult_1, 1.0);
}

// ON HOST (MPI root only), populates the etfphot view using rebinned
// solar data from our solar_data_file
void populate_etfphot(HostView1D we, HostView1D etfphot) {
  // FIXME: It looks like EAM is relying on a piece of infrastructure that
  // FIXME: we just don't have in EAMxx (eam/src/chemistry/utils/solar_data.F90).
  // FIXME: I have no idea whether EAMxx has a plan for supporting this
  // FIXME: solar irradiance / photon flux data, and I'm not going to recreate
  // FIXME: that capability here. So this is an unplugged hole.
  // FIXME:
  // FIXME: If we are going to do this the way EAM does it, the relevant logic
  // FIXME: is the call to rebin() in eam/src/chemistry/mozart/mo_jlong.F90,
  // FIXME: around line 104.

  // FIXME: zero the photon flux for now
  Kokkos::deep_copy(etfphot, 0);
}

// ON HOST, reads the photolysis table (used for gas phase chemistry) from the
// files with the given names
mam4::mo_photo::PhotoTableData read_photo_table(const ekat::Comm& comm,
                                                const char *rsf_file,
                                                const char* xs_long_file) {
  // NOTE: at the time of development, SCREAM's SCORPIO interface seems intended
  // NOTE: for domain-decomposed grid data. The files we're reading here are not
  // NOTE: spatial data, and should be the same everywhere, so we read them
  // NOTE: using serial NetCDF calls on MPI rank 0 and broadcast to other ranks.
  const int mpi_root = 0;
  int rsf_id, xs_long_id; // NetCDF file IDs (used only on MPI root)
  int nw, nump, numsza, numcolo3, numalb, nt, np_xs; // table dimensions
  if (comm.rank() == mpi_root) { // read dimension data from files and broadcast
    // open files
    int result = nc_open(rsf_file, NC_NOWRITE, &rsf_id);
    EKAT_REQUIRE_MSG(result == 0, "Error! Couldn't open rsf_file '" << rsf_file << "'\n");
    result = nc_open(xs_long_file, NC_NOWRITE, &xs_long_id);
    EKAT_REQUIRE_MSG(result == 0, "Error! Couldn't open xs_long_file '" << xs_long_file << "'\n");

    // read and broadcast dimension data
    nump     = nc_dimension(rsf_file, rsf_id, "numz");
    numsza   = nc_dimension(rsf_file, rsf_id, "numsza");
    numalb   = nc_dimension(rsf_file, rsf_id, "numalb");
    numcolo3 = nc_dimension(rsf_file, rsf_id, "numcolo3fact");
    nt       = nc_dimension(xs_long_file, xs_long_id, "numtemp");
    nw       = nc_dimension(xs_long_file, xs_long_id, "numwl");
    np_xs    = nc_dimension(xs_long_file, xs_long_id, "numprs");

    int dim_data[7] = {nump, numsza, numcolo3, numalb, nt, nw, np_xs};
    comm.broadcast(dim_data, 7, mpi_root);
  } else { // receive broadcasted dimension data from root rank
    int dim_data[7];
    comm.broadcast(dim_data, 7, mpi_root);
    nump     = dim_data[0];
    numsza   = dim_data[1];
    numcolo3 = dim_data[2];
    numalb   = dim_data[3];
    nt       = dim_data[4];
    nw       = dim_data[5];
    np_xs    = dim_data[6];
  }

  // set up the lng_indexer and pht_alias_mult_1 views based on our
  // (hardwired) chemical mechanism
  HostViewInt1D lng_indexer_h("lng_indexer(host)", mam4::mo_photo::phtcnt);
  HostView1D pht_alias_mult_1_h("pht_alias_mult_1(host)", 2);
  if (comm.rank() == mpi_root) {
    set_lng_indexer_and_pht_alias_mult_1(xs_long_file, xs_long_id,
                                         lng_indexer_h, pht_alias_mult_1_h);
  }
  comm.broadcast(lng_indexer_h.data(),      mam4::mo_photo::phtcnt, mpi_root);
  comm.broadcast(pht_alias_mult_1_h.data(), 2,                      mpi_root);

  // compute the size of the foremost dimension of xsqy using lng_indexer
  int numj = 0;
  for (int m = 0; m < mam4::mo_photo::phtcnt; ++m) {
    if (lng_indexer_h(m) > 0) {
      for (int mm = 0; mm < m; ++mm) {
        if (lng_indexer_h(mm) == lng_indexer_h(m)) {
          break;
        }
        ++numj;
      }
    }
  }

  // allocate the photolysis table
  auto table = mam4::mo_photo::create_photo_table_data(nw, nt, np_xs, numj,
                                                       nump, numsza, numcolo3,
                                                       numalb);

  // allocate host views for table data
  auto rsf_tab_h = Kokkos::create_mirror_view(table.rsf_tab);
  auto xsqy_h = Kokkos::create_mirror_view(table.xsqy);
  auto sza_h = Kokkos::create_mirror_view(table.sza);
  auto alb_h = Kokkos::create_mirror_view(table.alb);
  auto press_h = Kokkos::create_mirror_view(table.press);
  auto colo3_h = Kokkos::create_mirror_view(table.colo3);
  auto o3rat_h = Kokkos::create_mirror_view(table.o3rat);
  auto etfphot_h = Kokkos::create_mirror_view(table.etfphot);
  auto prs_h = Kokkos::create_mirror_view(table.prs);

  if (comm.rank() == mpi_root) { // read data from files and broadcast
    // read file data into our host views
    read_nc_var(rsf_file, rsf_id, "pm", press_h);
    read_nc_var(rsf_file, rsf_id, "sza", sza_h);
    read_nc_var(rsf_file, rsf_id, "alb", alb_h);
    read_nc_var(rsf_file, rsf_id, "colo3fact", o3rat_h);
    read_nc_var(rsf_file, rsf_id, "colo3", colo3_h);
    read_nc_var(rsf_file, rsf_id, "RSF", rsf_tab_h);

    read_nc_var(xs_long_file, xs_long_id, "pressure", prs_h);

    // read xsqy data (using lng_indexer_h for the first index)
    int ndx = 0;
    for (int m = 0; m < mam4::mo_photo::phtcnt; ++m) {
      if (lng_indexer_h(m) > 0) {
        auto xsqy_ndx_h = ekat::subview(xsqy_h, ndx);
        read_nc_var(xs_long_file, xs_long_id, lng_indexer_h(m), xsqy_ndx_h);
        ++ndx;
      }
    }

    // populate etfphot by rebinning solar data
    HostView1D wc_h("wc", nw), wlintv_h("wlintv", nw), we_h("we", nw+1);
    read_nc_var(rsf_file, rsf_id, "wc", wc_h);
    read_nc_var(rsf_file, rsf_id, "wlintv", wlintv_h);
    for (int i = 0; i < nw; ++i) {
      we_h(i) = wc_h(i) - 0.5 * wlintv_h(i);
    }
    we_h(nw) = wc_h(nw-1) - 0.5 * wlintv_h(nw-1);
    populate_etfphot(we_h, etfphot_h);

    // close the files
    nc_close(rsf_id);
    nc_close(xs_long_id);
  }

  // broadcast host views from MPI root to others
  comm.broadcast(rsf_tab_h.data(), nw*numalb*numcolo3*numsza*nump, mpi_root);
  comm.broadcast(xsqy_h.data(),    numj*nw*nt*np_xs,               mpi_root);
  comm.broadcast(sza_h.data(),     numsza,                         mpi_root);
  comm.broadcast(alb_h.data(),     numalb,                         mpi_root);
  comm.broadcast(press_h.data(),   nump,                           mpi_root);
  comm.broadcast(o3rat_h.data(),   numcolo3,                       mpi_root);
  comm.broadcast(colo3_h.data(),   nump,                           mpi_root);
  comm.broadcast(etfphot_h.data(), nw,                             mpi_root);
  comm.broadcast(prs_h.data(),     np_xs,                          mpi_root);

  // copy host photolysis table into place on device
  Kokkos::deep_copy(table.rsf_tab,          rsf_tab_h);
  Kokkos::deep_copy(table.xsqy,             xsqy_h);
  Kokkos::deep_copy(table.sza,              sza_h);
  Kokkos::deep_copy(table.alb,              alb_h);
  Kokkos::deep_copy(table.press,            press_h);
  Kokkos::deep_copy(table.colo3,            colo3_h);
  Kokkos::deep_copy(table.o3rat,            o3rat_h);
  Kokkos::deep_copy(table.etfphot,          etfphot_h);
  Kokkos::deep_copy(table.prs,              prs_h);
  Kokkos::deep_copy(table.pht_alias_mult_1, pht_alias_mult_1_h);
  Kokkos::deep_copy(table.lng_indexer,      lng_indexer_h);

  // compute gradients (on device)
  Kokkos::parallel_for("del_p", nump-1, KOKKOS_LAMBDA(int i) {
    table.del_p(i) = 1.0/::abs(table.press(i)- table.press(i+1));
  });
  Kokkos::parallel_for("del_sza", numsza-1, KOKKOS_LAMBDA(int i) {
    table.del_sza(i) = 1.0/(table.sza(i+1) - table.sza(i));
  });
  Kokkos::parallel_for("del_alb", numalb-1, KOKKOS_LAMBDA(int i) {
    table.del_alb(i) = 1.0/(table.alb(i+1) - table.alb(i));
  });
  Kokkos::parallel_for("del_o3rat", numcolo3-1, KOKKOS_LAMBDA(int i) {
    table.del_o3rat(i) = 1.0/(table.o3rat(i+1) - table.o3rat(i));
  });
  Kokkos::parallel_for("dprs", np_xs-1, KOKKOS_LAMBDA(int i) {
    table.dprs(i) = 1.0/(table.prs(i) - table.prs(i+1));
  });

  return table;
}

}

void MAMMicrophysics::initialize_impl(const RunType run_type) {

  step_ = 0;

  // populate the wet and dry atmosphere states with views from fields and
  // the buffer
  wet_atm_.qv = get_field_in("qv").get_view<const Real**>();
  wet_atm_.qc = get_field_out("qc").get_view<Real**>();
  wet_atm_.nc = get_field_out("nc").get_view<Real**>();
  wet_atm_.qi = get_field_in("qi").get_view<const Real**>();
  wet_atm_.ni = get_field_in("ni").get_view<const Real**>();
  wet_atm_.omega = get_field_in("omega").get_view<const Real**>();

  dry_atm_.T_mid     = get_field_in("T_mid").get_view<const Real**>();
  dry_atm_.p_mid     = get_field_in("p_mid").get_view<const Real**>();
  dry_atm_.p_del     = get_field_in("pseudo_density").get_view<const Real**>();
  dry_atm_.cldfrac   = get_field_in("cldfrac_tot").get_view<const Real**>(); // FIXME: tot or liq?
  dry_atm_.pblh      = get_field_in("pbl_height").get_view<const Real*>();
  dry_atm_.phis      = get_field_in("phis").get_view<const Real*>();
  dry_atm_.z_mid     = buffer_.z_mid;
  dry_atm_.dz        = buffer_.dz;
  dry_atm_.z_iface   = buffer_.z_iface;
  dry_atm_.qv        = buffer_.qv_dry;
  dry_atm_.qc        = buffer_.qc_dry;
  dry_atm_.nc        = buffer_.nc_dry;
  dry_atm_.qi        = buffer_.qi_dry;
  dry_atm_.ni        = buffer_.ni_dry;
  dry_atm_.w_updraft = buffer_.w_updraft;
  dry_atm_.z_surf = 0.0; // FIXME: for now

  const auto& tracers = get_group_out("tracers");
  const auto& tracers_info = tracers.m_info;

  // perform any initialization work
  if (run_type==RunType::Initial) {
  }

  // set wet/dry aerosol state data (interstitial aerosols only)
  for (int m = 0; m < mam_coupling::num_aero_modes(); ++m) {
    const char* int_nmr_field_name = mam_coupling::int_aero_nmr_field_name(m);
    wet_aero_.int_aero_nmr[m] = get_field_out(int_nmr_field_name).get_view<Real**>();
    dry_aero_.int_aero_nmr[m] = buffer_.dry_int_aero_nmr[m];
    for (int a = 0; a < mam_coupling::num_aero_species(); ++a) {
      const char* int_mmr_field_name = mam_coupling::int_aero_mmr_field_name(m, a);
      if (strlen(int_mmr_field_name) > 0) {
        wet_aero_.int_aero_mmr[m][a] = get_field_out(int_mmr_field_name).get_view<Real**>();
        dry_aero_.int_aero_mmr[m][a] = buffer_.dry_int_aero_mmr[m][a];
      }
    }
  }

  // set wet/dry aerosol-related gas state data
  for (int g = 0; g < mam_coupling::num_aero_gases(); ++g) {
    const char* mmr_field_name = mam_coupling::gas_mmr_field_name(g);
    wet_aero_.gas_mmr[g] = get_field_out(mmr_field_name).get_view<Real**>();
    dry_aero_.gas_mmr[g] = buffer_.dry_gas_mmr[g];
  }

  // create our photolysis rate calculation table
  photo_table_ = read_photo_table(get_comm(),
                                  config_.photolysis.rsf_file,
                                  config_.photolysis.xs_long_file);

  // set up our preprocess/postprocess functors
  preprocess_.initialize(ncol_, nlev_, wet_atm_, wet_aero_, dry_atm_, dry_aero_);
  postprocess_.initialize(ncol_, nlev_, wet_atm_, wet_aero_, dry_atm_, dry_aero_);

  // set field property checks for the fields in this process
  /* e.g.
  using Interval = FieldWithinIntervalCheck;
  using LowerBound = FieldLowerBoundCheck;
  add_postcondition_check<Interval>(get_field_out("T_mid"),m_grid,130.0,500.0,false);
  add_postcondition_check<LowerBound>(get_field_out("pbl_height"),m_grid,0);
  add_postcondition_check<Interval>(get_field_out("cldfrac_liq"),m_grid,0.0,1.0,false);
  add_postcondition_check<LowerBound>(get_field_out("tke"),m_grid,0);
  */

  // set up WSM for internal local variables
  // FIXME: we'll probably need this later, but we'll just use ATMBufferManager for now
  //const auto default_policy = ekat::ExeSpaceUtils<KT::ExeSpace>::get_default_team_policy(ncol_, nlev_);
  //workspace_mgr_.setup(buffer_.wsm_data, nlev_+1, 13+(n_wind_slots+n_trac_slots), default_policy);
}

void MAMMicrophysics::run_impl(const double dt) {

  const auto scan_policy = ekat::ExeSpaceUtils<KT::ExeSpace>::get_thread_range_parallel_scan_team_policy(ncol_, nlev_);
  const auto policy      = ekat::ExeSpaceUtils<KT::ExeSpace>::get_default_team_policy(ncol_, nlev_);

  // preprocess input -- needs a scan for the calculation of atm height
  Kokkos::parallel_for("preprocess", scan_policy, preprocess_);
  Kokkos::fence();

  // reset internal WSM variables
  //workspace_mgr_.reset_internals();

  // NOTE: nothing depends on simulation time (yet), so we can just use zero for now
  double t = 0.0;

  // here's where we store per-column photolysis rates
  // FIXME: this isn't great, but will do for now
  using View2D = haero::DeviceType::view_2d<Real>;
  View2D photo_rates("photo_rates", nlev_, mam4::mo_photo::phtcnt);

  // climatology data for linear stratospheric chemistry
  auto linoz_o3_clim      = buffer_.scratch[0]; // ozone (climatology) [vmr]
  auto linoz_o3col_clim   = buffer_.scratch[1]; // column o3 above box (climatology) [Dobson Units (DU)]
  auto linoz_t_clim       = buffer_.scratch[2]; // temperature (climatology) [K]
  auto linoz_PmL_clim     = buffer_.scratch[3]; // P minus L (climatology) [vmr/s]
  auto linoz_dPmL_dO3     = buffer_.scratch[4]; // sensitivity of P minus L to O3 [1/s]
  auto linoz_dPmL_dT      = buffer_.scratch[5]; // sensitivity of P minus L to T3 [K]
  auto linoz_dPmL_dO3col  = buffer_.scratch[6]; // sensitivity of P minus L to overhead O3 column [vmr/DU]
  auto linoz_cariolle_psc = buffer_.scratch[7]; // Cariolle parameter for PSC loss of ozone [1/s]

  // it's a bit wasteful to store this for all columns, but simpler from an
  // allocation perspective
  auto o3_col_dens = buffer_.scratch[8];

  // FIXME: Read relevant linoz climatology data from file(s) based on time

  // FIXME: Read relevant chlorine loading data from file based on time.

  // loop over atmosphere columns and compute aerosol microphyscs
  Kokkos::parallel_for(policy, KOKKOS_LAMBDA(const ThreadTeam& team) {
    const int icol = team.league_rank(); // column index

    Real col_lat = col_latitudes_(icol); // column latitude (degrees?)

    // fetch column-specific atmosphere state data
    auto atm = mam_coupling::atmosphere_for_column(dry_atm_, icol);
    auto z_iface = ekat::subview(dry_atm_.z_iface, icol);
    Real z_surf = dry_atm_.z_surf;
    Real phis = dry_atm_.phis(icol);

    // set surface state data
    haero::Surface sfc{};

    // fetch column-specific subviews into aerosol prognostics
    mam4::Prognostics progs = mam_coupling::interstitial_aerosols_for_column(dry_aero_, icol);

    // set up diagnostics
    mam4::Diagnostics diags(nlev_);

    // calculate o3 column densities (first component of col_dens in Fortran code)
    auto o3_col_dens_i = ekat::subview(o3_col_dens, icol);
    impl::compute_o3_column_density(team, atm, progs, o3_col_dens_i);

    // set up photolysis work arrays for this column.
    mam4::mo_photo::PhotoTableWorkArrays photo_work_arrays;
    // FIXME: set views here

    // ... look up photolysis rates from our table
    // NOTE: the table interpolation operates on an entire column of data, so we
    // NOTE: must do it before dispatching to individual vertical levels
    Real zenith_angle = 0.0; // FIXME: need to get this from EAMxx [radians]
    Real surf_albedo = 0.0; // FIXME: surface albedo
    Real esfact = 0.0; // FIXME: earth-sun distance factor
    mam4::ColumnView lwc; // FIXME: liquid water cloud content: where do we get this?
    mam4::mo_photo::table_photo(photo_rates, atm.pressure, atm.hydrostatic_dp,
      atm.temperature, o3_col_dens_i, zenith_angle, surf_albedo, lwc,
      atm.cloud_fraction, esfact, photo_table_, photo_work_arrays);

    // compute external forcings at time t(n+1) [molecules/cm^3/s]
    constexpr int extcnt = mam4::gas_chemistry::extcnt;
    view_2d extfrc; // FIXME: where to allocate? (nlev, extcnt)
    mam4::mo_setext::Forcing forcings[extcnt]; // FIXME: forcings seem to require file data
    mam4::mo_setext::extfrc_set(forcings, extfrc);

    // compute aerosol microphysics on each vertical level within this column
    Kokkos::parallel_for(Kokkos::TeamThreadRange(team, nlev_), [&](const int k) {

      constexpr int num_modes = mam4::AeroConfig::num_modes();
      constexpr int num_aero_ids = mam4::AeroConfig::num_aerosol_ids();
      constexpr int gas_pcnst = mam_coupling::gas_pcnst();
      constexpr int nqtendbb = mam_coupling::nqtendbb();

      // extract atm state variables (input)
      Real temp    = atm.temperature(k);
      Real pmid    = atm.pressure(k);
      Real pdel    = atm.hydrostatic_dp(k);
      Real zm      = atm.height(k);
      Real zi      = z_iface(k);
      Real pblh    = atm.planetary_boundary_layer_height;
      Real qv      = atm.vapor_mixing_ratio(k);
      Real cldfrac = atm.cloud_fraction(k);

      // extract aerosol state variables into "working arrays" (mass mixing ratios)
      // (in EAM, this is done in the gas_phase_chemdr subroutine defined within
      //  mozart/mo_gas_phase_chemdr.F90)
      Real q[gas_pcnst] = {};
      Real qqcw[gas_pcnst] = {};
      mam_coupling::transfer_prognostics_to_work_arrays(progs, k, q, qqcw);

      // convert mass mixing ratios to volume mixing ratios (VMR), equivalent
      // to tracer mixing ratios (TMR))
      Real vmr[gas_pcnst], vmrcw[gas_pcnst];
      mam_coupling::convert_work_arrays_to_vmr(q, qqcw, vmr, vmrcw);

      // aerosol/gas species tendencies (output)
      Real vmr_tendbb[gas_pcnst][nqtendbb] = {};
      Real vmrcw_tendbb[gas_pcnst][nqtendbb] = {};

      // create work array copies to retain "pre-chemistry" values
      Real vmr_pregaschem[gas_pcnst] = {};
      Real vmr_precldchem[gas_pcnst] = {};
      Real vmrcw_precldchem[gas_pcnst] = {};
      for (int i = 0; i < gas_pcnst; ++i) {
        vmr_pregaschem[i] = vmr[i];
        vmr_precldchem[i] = vmr[i];
        vmrcw_precldchem[i] = vmrcw[i];
      }

      //---------------------
      // Gas Phase Chemistry
      //---------------------
      Real photo_rates_k[mam4::mo_photo::phtcnt];
      for (int i = 0; i < mam4::mo_photo::phtcnt; ++i) {
        photo_rates_k[i] = photo_rates(k, i);
      }
      Real extfrc_k[extcnt];
      for (int i = 0; i < extcnt; ++i) {
        extfrc_k[i] = extfrc(k, i);
      }
      constexpr int nfs = mam4::gas_chemistry::nfs; // number of "fixed species"
      // NOTE: we compute invariants here and pass them out to use later with
      // NOTE: setsox
      Real invariants[nfs];
      impl::gas_phase_chemistry(zm, zi, phis, temp, pmid, pdel, dt,
                                photo_rates_k, extfrc_k, vmr, invariants);

      //----------------------
      // Aerosol microphysics
      //----------------------
      // the logic below is taken from the aero_model_gasaerexch subroutine in
      // eam/src/chemistry/modal_aero/aero_model.F90

      // aqueous chemistry ...
      const int loffset = 8; // FIXME: offset of first tracer in work arrays
                             // FIXME: (taken from mam4xx setsox validation test)
      const Real mbar = haero::Constants::molec_weight_dry_air; // FIXME: ???
      constexpr int indexm = 0;  // FIXME: index of xhnm in invariants array (??)
      Real cldnum = 0.0; // FIXME: droplet number concentration: where do we get this?
      setsox_single_level(loffset, dt, pmid, pdel, temp, mbar, lwc(k),
        cldfrac, cldnum, invariants[indexm], config_.setsox, vmrcw, vmr);

      // calculate aerosol water content using water uptake treatment
      // * dry and wet diameters [m]
      // * wet densities [kg/m3]
      // * aerosol water mass mixing ratio [kg/kg]
      Real dgncur_a[num_modes]    = {};
      Real dgncur_awet[num_modes] = {};
      Real wetdens[num_modes]     = {};
      Real qaerwat[num_modes]     = {};
      impl::compute_water_content(progs, k, qv, temp, pmid, dgncur_a, dgncur_awet, wetdens, qaerwat);

      // do aerosol microphysics (gas-aerosol exchange, nucleation, coagulation)
      impl::modal_aero_amicphys_intr(config_.amicphys, step_, dt, t, pmid, pdel,
                                     zm, pblh, qv, cldfrac, vmr, vmrcw, vmr_pregaschem,
                                     vmr_precldchem, vmrcw_precldchem, vmr_tendbb,
                                     vmrcw_tendbb, dgncur_a, dgncur_awet,
                                     wetdens, qaerwat);

      //-----------------
      // LINOZ chemistry
      //-----------------

      // the following things are diagnostics, which we're not
      // including in the first rev
      Real do3_linoz, do3_linoz_psc, ss_o3, o3col_du_diag, o3clim_linoz_diag,
           zenith_angle_degrees;

      // FIXME: Need to get chlorine loading data from file
      Real chlorine_loading = 0.0;

      Real rlats = col_lat * M_PI / 180.0; // convert column latitude to radians
      int o3_ndx; // FIXME: need to set this
      mam4::lin_strat_chem::lin_strat_chem_solve_kk(o3_col_dens_i(k), temp,
        zenith_angle, pmid, dt, rlats,
        linoz_o3_clim(icol, k), linoz_t_clim(icol, k), linoz_o3col_clim(icol, k),
        linoz_PmL_clim(icol, k), linoz_dPmL_dO3(icol, k), linoz_dPmL_dT(icol, k),
        linoz_dPmL_dO3col(icol, k), linoz_cariolle_psc(icol, k),
        chlorine_loading, config_.linoz.psc_T, vmr[o3_ndx],
        do3_linoz, do3_linoz_psc, ss_o3,
        o3col_du_diag, o3clim_linoz_diag, zenith_angle_degrees);

      // update source terms above the ozone decay threshold
      if (k > nlev_ - config_.linoz.o3_lbl - 1) {
        Real do3_mass; // diagnostic, not needed
        mam4::lin_strat_chem::lin_strat_sfcsink_kk(dt, pdel, vmr[o3_ndx], config_.linoz.o3_sfc,
          config_.linoz.o3_tau, do3_mass);
      }

      // ... check for negative values and reset to zero
      for (int i = 0; i < gas_pcnst; ++i) {
        if (vmr[i] < 0.0) vmr[i] = 0.0;
      }

      //----------------------
      // Dry deposition (gas)
      //----------------------

      // FIXME: need to find this in mam4xx

      // transfer updated prognostics from work arrays
      mam_coupling::convert_work_arrays_to_mmr(vmr, vmrcw, q, qqcw);
      mam_coupling::transfer_work_arrays_to_prognostics(q, qqcw, progs, k);
    });
  });

  // postprocess output
  Kokkos::parallel_for("postprocess", policy, postprocess_);
  Kokkos::fence();
}

void MAMMicrophysics::finalize_impl() {
}

} // namespace scream
