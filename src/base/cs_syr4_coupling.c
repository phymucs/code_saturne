/*============================================================================
 * Syrthes 4 coupling
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2012 EDF S.A.

  This program is free software; you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation; either version 2 of the License, or (at your option) any later
  version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
  Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

/*----------------------------------------------------------------------------
 * PLE library headers
 *----------------------------------------------------------------------------*/

#include <ple_coupling.h>
#include <ple_locator.h>

/*----------------------------------------------------------------------------
 * Local headers
 *----------------------------------------------------------------------------*/

#include "bft_mem.h"
#include "bft_error.h"
#include "bft_printf.h"

#include "fvm_nodal.h"
#include "fvm_nodal_extract.h"
#include "fvm_nodal_project.h"
#include "fvm_parall.h"
#include "fvm_selector.h"

#include "cs_coupling.h"
#include "cs_log.h"
#include "cs_mesh.h"
#include "cs_mesh_quantities.h"
#include "cs_mesh_connect.h"
#include "cs_selector.h"
#include "cs_post.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_syr4_coupling.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Local Macro Definitions
 *============================================================================*/

const int  cs_syr4_coupling_tag = 'C'+'S'+'_'+'C'+'O'+'U'+'P'+'L'+'A'+'G'+'E';

/*=============================================================================
 * Local Structure Definitions
 *============================================================================*/

/* Structure associated with Syrthes coupling */

typedef struct _cs_syr4_coupling_ent_t {

  /* Mesh-related members */

  ple_locator_t     *locator;        /* Associated locator */

  int                elt_dim;        /* Element dimension */
  cs_lnum_t          n_elts;         /* Number of coupled elements */

  fvm_nodal_t       *elts;           /* Coupled elements */

  /* Saved arrays for post processing (float for reduced memory use) */

  int                post_mesh_id;   /* 0 if post-processing is not active,
                                        or post-processing mesh_id (< 0) */
  float             *solid_temp;     /* Wall temperature (received) */
  float             *flux;           /* Flux (calculated) */
  float             *tfluid_tmp;     /* Fluid temperature (points to flux in
                                        transient stage where solid_temp and
                                        fluid_temp are known, NULL once
                                        flux is calculated) */

  /* Saved array for volume coupling. Will be used to build source term. */

  double           *hvol;            /* Volumetric exchange coefficient. */
  double           *sol_temp;        /* Solid temperature sent by SYRTHES */

} cs_syr4_coupling_ent_t;

/* Structure associated with Syrthes coupling */

struct _cs_syr4_coupling_t {

  /* Mesh-related members */

  int                      dim;            /* Coupled mesh dimension */
  int                      ref_axis;       /* Selected axis for edge extraction */

  char                    *syr_name;       /* Application name, or -1 */

  char                    *face_sel;       /* Face selection criteria */
  char                    *cell_sel;       /* Face selection criteria */

  cs_syr4_coupling_ent_t  *faces;          /* Wall coupling structure */
  cs_syr4_coupling_ent_t  *cells;          /* Volume coupling structure */

  int                      verbosity;      /* Verbosity level */
  int                      visualization;  /* Visualization output flag */

  /* Communication-related members */

#if defined(HAVE_MPI)

  MPI_Comm           comm;           /* Associated MPI communicator */

  int                n_syr_ranks;    /* Number of associated SYRTHES ranks */
  int                syr_root_rank;  /* First associated SYRTHES rank */

#endif

};

/*============================================================================
 *  Global variables
 *============================================================================*/

static int                   cs_glob_syr4_n_couplings = 0;
static cs_syr4_coupling_t  **cs_glob_syr4_couplings = NULL;

/* Start and end (negative) numbers associated with
   dedicated post processing meshes */

static int  cs_glob_syr4_post_mesh_ext[2] = {0, 1};

static int  cs_syr4_coupling_conservativity = 0; /* No forcing by default */
static int  cs_syr4_coupling_implicit = 1;

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Initialize communicator for Syrthes coupling
 *
 * parameters:
 *   syr_coupling  <-> Syrthes coupling structure
 *   coupling_id   <-- id of this coupling (for log file message)
 *----------------------------------------------------------------------------*/

static void
_init_comm(cs_syr4_coupling_t *syr_coupling,
           int                 coupling_id)

{
#if defined(HAVE_MPI)

  int  mpi_flag = 0;
  int local_range[2] = {-1, -1};
  int distant_range[2] = {-1, -1};

  MPI_Initialized(&mpi_flag);

  if (mpi_flag == 0)
    return;

  bft_printf(_(" SYRTHES coupling %d: initializing MPI communication ... "),
             coupling_id);
  bft_printf_flush();

  ple_coupling_mpi_intracomm_create(MPI_COMM_WORLD,
                                    cs_glob_mpi_comm,
                                    syr_coupling->syr_root_rank,
                                    &(syr_coupling->comm),
                                    local_range,
                                    distant_range);

  bft_printf(_("[ok]\n"));
  bft_printf(_("  Local ranks = [%d..%d], distant ranks = [%d..%d].\n\n"),
             local_range[0], local_range[1] - 1,
             distant_range[0], distant_range[1] - 1);
  bft_printf_flush();

  syr_coupling->n_syr_ranks = distant_range[1] - distant_range[0];
  syr_coupling->syr_root_rank = distant_range[0];

#endif
}

/*----------------------------------------------------------------------------
 * Free communicator for Syrthes coupling
 *
 * parameters:
 *   syr_coupling  <-> Syrthes coupling structure
 *----------------------------------------------------------------------------*/

static void
_finalize_comm(cs_syr4_coupling_t *syr_coupling)
{
#if defined(HAVE_MPI)

  if (syr_coupling == NULL)
    return;

  if (syr_coupling->comm != MPI_COMM_NULL) {
    MPI_Comm_free(&(syr_coupling->comm));
    syr_coupling->comm = MPI_COMM_NULL;
  }

#endif
}

/*----------------------------------------------------------------------------
 * Exchange synchronization messages between Code_Saturne and SYRTHES.
 *
 * parameters:
 *   syr_coupling  <--  Syrthes coupling structure
 *   op_name_send  <--  operation name to send, or NULL. Only the 32
 *                      first characters are sent if the nae is longer.
 *   op_name_recv  <--  operation name to receive, or NULL (size: 33)
 *----------------------------------------------------------------------------*/

static void
_exchange_sync(cs_syr4_coupling_t  *syr_coupling,
               const char          *op_name_send,
               char                *op_name_recv)
{
#if defined(HAVE_MPI)

  if (cs_glob_rank_id < 1) {

    MPI_Status status;

    if (op_name_send != NULL) {

      char _op_name_send[33];
      strncpy(_op_name_send, op_name_send, 32);
      _op_name_send[32] = '\0';

    /* Exchange command messages */
      if (op_name_recv != NULL) {
        MPI_Sendrecv(_op_name_send, 32, MPI_CHAR,
                     syr_coupling->syr_root_rank, cs_syr4_coupling_tag,
                     op_name_recv, 32, MPI_CHAR,
                     syr_coupling->syr_root_rank, cs_syr4_coupling_tag,
                     syr_coupling->comm, &status);
      }

      else
        MPI_Send(_op_name_send, 32, MPI_CHAR,
                 syr_coupling->syr_root_rank, cs_syr4_coupling_tag,
                 syr_coupling->comm);

    }
    else if (op_name_recv != NULL) {
      MPI_Recv(op_name_recv, 32, MPI_CHAR,
               syr_coupling->syr_root_rank, cs_syr4_coupling_tag,
               syr_coupling->comm, &status);
    }

  }

  if (op_name_recv != NULL && cs_glob_rank_id > -1) {
    MPI_Bcast(op_name_recv, 32, MPI_CHAR, 0, cs_glob_mpi_comm);
    op_name_recv[32] = '\0';
  }

#endif
}

/*----------------------------------------------------------------------------
 * Post process variables associated with Syrthes couplings
 *
 * parameters:
 *   coupling        <--  Void pointer to SYRTHES coupling structure
 *   nt_cur_abs      <--  Current time step
 *   t_cur_abs       <--  Current time value
 *----------------------------------------------------------------------------*/

static void
_cs_syr4_coupling_post_function(void       *coupling,
                                cs_int_t    nt_cur_abs,
                                cs_real_t   t_cur_abs)
{
  int type_id, var_id;

  const cs_syr4_coupling_t  *syr_coupling = coupling;
  cs_syr4_coupling_ent_t *coupling_ent = NULL;
  const char *var_name[2] = {N_("Wall T"), N_("Flux")};

  for (type_id = 0; type_id < 2; type_id++) {

    if (type_id == 0)
      coupling_ent = syr_coupling->faces;
    else
      coupling_ent = syr_coupling->cells;

    if (coupling_ent != NULL) {

      if (coupling_ent->post_mesh_id != 0) {

        const float *cell_var[2] = {NULL, NULL};
        const float *face_var[2] = {NULL, NULL};

        if (type_id == 0) {
          face_var[0] = coupling_ent->solid_temp;
          face_var[1] = coupling_ent->flux;
        }
        else {
          cell_var[0] = coupling_ent->solid_temp;
          cell_var[1] = coupling_ent->flux;
        }

        for (var_id = 0; var_id < 2; var_id++)
          cs_post_write_var(coupling_ent->post_mesh_id,
                            _(var_name[var_id]),
                            1,
                            false,
                            false,
                            CS_POST_TYPE_float,
                            nt_cur_abs,
                            t_cur_abs,
                            cell_var[var_id],
                            NULL,
                            face_var[var_id]);

      }
    }
  }
}

/*----------------------------------------------------------------------------
 * Initialize post-processing of a SYRTHES coupling
 *
 * parameters:
 *   syr_coupling <-- partially initialized SYRTHES coupling structure
 *   coupling_ent <-- associated coupling mesh entity
 *----------------------------------------------------------------------------*/

static void
_post_init(cs_syr4_coupling_t      *syr_coupling,
           cs_syr4_coupling_ent_t  *coupling_ent)
{
  int dim_shift = 0;
  int coupling_id = -1;

  const int writer_id = -1;
  const int writer_ids[] = {writer_id};

  /* Determine coupling id */

  for (coupling_id = 0;
       (   coupling_id < cs_glob_syr4_n_couplings
        && cs_glob_syr4_couplings[coupling_id] != syr_coupling);
       coupling_id++);

  /* Exit silently if associated writer is not available */

  if (cs_post_writer_exists(writer_id) != true)
    return;

  /* Initialize post processing flag, and free previous arrays in
     case this function is called more than once */

  coupling_ent->post_mesh_id = cs_post_get_free_mesh_id();

  if (coupling_ent->solid_temp != NULL)
    BFT_FREE(coupling_ent->solid_temp);
  if (coupling_ent->flux != NULL)
    BFT_FREE(coupling_ent->flux);

  /* Allocate arrays */

  if (coupling_ent->n_elts > 0) {
    BFT_MALLOC(coupling_ent->solid_temp, coupling_ent->n_elts, float);
    BFT_MALLOC(coupling_ent->flux, coupling_ent->n_elts, float);
  }
  coupling_ent->tfluid_tmp = NULL;

  /* Associate external mesh description with post processing subsystem */

  if (syr_coupling->dim == 2)
    dim_shift = 1;

  cs_post_define_existing_mesh(coupling_ent->post_mesh_id,
                               coupling_ent->elts,
                               dim_shift,
                               false,
                               false,
                               1,
                               writer_ids);

  /* Register post processing function */

  cs_post_add_time_dep_output(_cs_syr4_coupling_post_function,
                              (void *)syr_coupling);

  /* Update start and end (negative) numbers associated with
     dedicated post processing meshes */

  if (cs_glob_syr4_post_mesh_ext[0] == 0)
    cs_glob_syr4_post_mesh_ext[0] = coupling_ent->post_mesh_id;

  cs_glob_syr4_post_mesh_ext[1] = coupling_ent->post_mesh_id;
}

/*----------------------------------------------------------------------------
 * Define nodal mesh for Syrthes coupling from selection criteria.
 *
 * parameters:
 *   syr_coupling    <-- partially initialized SYRTHES coupling structure
 *   select_criteria <-- selection criteria
 *   elt_dim         <-- element dimension
 *
 * returns:
 *   pointer to created Syrthes coupling entity helper structure
 *----------------------------------------------------------------------------*/

static cs_syr4_coupling_ent_t *
_create_coupled_ent(cs_syr4_coupling_t  *syr_coupling,
                    const char          *select_criteria,
                    int                  elt_dim)
{
  char *coupled_mesh_name = NULL;
  cs_gnum_t n_exterior = 0, n_dist_elts = 0;
  cs_lnum_t *elt_list = NULL;
  cs_coord_t *elt_centers = NULL;
  const double tolerance = 0.1;
  fvm_nodal_t *location_elts = NULL;
  ple_mesh_elements_closest_t *locate_on_closest = NULL;
  float *cs_to_syr_dist = NULL;
  float *syr_to_cs_dist = NULL;

  cs_syr4_coupling_ent_t *coupling_ent = NULL;

  assert(syr_coupling != NULL);

  /* Initialization */

  BFT_MALLOC(coupling_ent, 1, cs_syr4_coupling_ent_t);

  coupling_ent->locator = NULL;
  coupling_ent->elt_dim = elt_dim;

  coupling_ent->n_elts = 0;
  coupling_ent->elts = NULL;

  coupling_ent->post_mesh_id = 0;
  coupling_ent->solid_temp = NULL;
  coupling_ent->flux = NULL;
  coupling_ent->tfluid_tmp = NULL;

  coupling_ent->hvol = NULL;
  coupling_ent->sol_temp = NULL;

  if (syr_coupling->verbosity > 0) {
    bft_printf(_("\nExtracting coupled mesh             ..."));
    bft_printf_flush();
  }

  /* Creation of a new nodal mesh from selected cells */

  if (elt_dim == syr_coupling->dim) {

    BFT_MALLOC(coupled_mesh_name,
                 strlen(_("SYRTHES %s cells"))
               + strlen(syr_coupling->syr_name) + 1, char);
    sprintf(coupled_mesh_name, _("SYRTHES %s cells"), syr_coupling->syr_name);

    BFT_MALLOC(elt_list, cs_glob_mesh->n_cells, cs_lnum_t);

    cs_selector_get_cell_list(select_criteria,
                              &(coupling_ent->n_elts),
                              elt_list);

    coupling_ent->elts
      = cs_mesh_connect_cells_to_nodal(cs_glob_mesh,
                                       coupled_mesh_name,
                                       false,
                                       coupling_ent->n_elts,
                                       elt_list);

    BFT_FREE(elt_list);
    BFT_FREE(coupled_mesh_name);

    /* Allocate additional buffers */

    BFT_MALLOC(coupling_ent->hvol, coupling_ent->n_elts, double);
    BFT_MALLOC(coupling_ent->sol_temp, coupling_ent->n_elts, double);

  }

  /* Creation of a new nodal mesh from selected border faces */

  else if (elt_dim == syr_coupling->dim - 1) {

    locate_on_closest = cs_coupling_point_closest_mesh;

    BFT_MALLOC(coupled_mesh_name,
               strlen("SYRTHES  faces") + strlen(syr_coupling->syr_name) + 1,
               char);
    sprintf(coupled_mesh_name, _("SYRTHES %s faces"), syr_coupling->syr_name);

    BFT_MALLOC(elt_list, cs_glob_mesh->n_b_faces, cs_lnum_t);

    cs_selector_get_b_face_list(select_criteria,
                                &(coupling_ent->n_elts),
                                elt_list);

    coupling_ent->elts
      = cs_mesh_connect_faces_to_nodal(cs_glob_mesh,
                                       coupled_mesh_name,
                                       false,
                                       0,
                                       coupling_ent->n_elts,
                                       NULL,
                                       elt_list);

    BFT_FREE(elt_list);
    BFT_FREE(coupled_mesh_name);

  }

  if (syr_coupling->verbosity > 0) {
    bft_printf(" [ok]\n");
    bft_printf_flush();
  }

  if (fvm_nodal_get_n_g_vertices(coupling_ent->elts) == 0)
    bft_error(__FILE__, __LINE__, 0,
              _(" Selection criteria:\n"
                " \"%s\"\n"
                " leads to an empty mesh for SYRTHES coupling.\n"),
              select_criteria);

  /* In case of 2D coupling, project coupled elements to 2D */

  location_elts = coupling_ent->elts;

  if (syr_coupling->dim == 2) {

    double  a[6];
    cs_lnum_t  n_errors = 0;

    if (syr_coupling->verbosity > 0) {
      bft_printf(_("Projecting the extracted mesh to 2D ..."));
      bft_printf_flush();
    }

    fvm_nodal_project(coupling_ent->elts, syr_coupling->ref_axis, &n_errors);

    if (n_errors > 0)
      bft_error(__FILE__, __LINE__, 0,
                _("Error projecting the extracted mesh."));

    if (syr_coupling->verbosity > 0) {
      bft_printf(" [ok]\n");
      bft_printf_flush();
    }

    location_elts = fvm_nodal_copy(coupling_ent->elts);

    if (syr_coupling->ref_axis == 0) {
      a[0] = 0.; a[1] = 1.; a[2] = 0.; a[3] = 0.; a[4] = 0.; a[5] = 1.;
    }
    else if (syr_coupling->ref_axis == 1) {
      a[0] = 1.; a[1] = 0.; a[2] = 0.; a[3] = 0.; a[4] = 0.; a[5] = 1.;
    }
    else if (syr_coupling->ref_axis == 2) {
      a[0] = 1.; a[1] = 0.; a[2] = 0.; a[3] = 0.; a[4] = 1.; a[5] = 0.;
    }

    fvm_nodal_project_coords(location_elts, a);
  }

  /* Element information */

  if (syr_coupling->verbosity > 0) {
    cs_gnum_t n_g_elts = coupling_ent->n_elts;
    fvm_parall_counter(&n_g_elts, 1);
    bft_printf(_("\nExtracted mesh built of %llu elements.\n"),
               (unsigned long long)n_g_elts);
    bft_printf_flush();
  }

  /* Initialize post-processing */

  if (syr_coupling->visualization != 0)
    _post_init(syr_coupling, coupling_ent);

  /* Build and initialize associated locator */

  if (syr_coupling->verbosity > 0) {
    bft_printf(_("\nLocator structure and mesh creation ..."));
    bft_printf_flush();
  }

#if defined(PLE_HAVE_MPI)
  coupling_ent->locator = ple_locator_create(tolerance,
                                             syr_coupling->comm,
                                             syr_coupling->n_syr_ranks,
                                             syr_coupling->syr_root_rank);
#else
  coupling_ent->locator = ple_locator_create(tolerance);
#endif

  /* Retrieve coordinates using FVM functions rather than previous list and
     coordinates, in case the extracted nodal mesh contains elements in a
     different order (multiple element types) or coordinates are projected
     in 2D. */

  if (coupling_ent->n_elts > 0) {

    if (locate_on_closest != NULL)
      BFT_MALLOC(cs_to_syr_dist, coupling_ent->n_elts, float);

    BFT_MALLOC(elt_centers,
               coupling_ent->n_elts*syr_coupling->dim,
               cs_coord_t);
    fvm_nodal_get_element_centers(location_elts,
                                  CS_INTERLACE,
                                  coupling_ent->elt_dim,
                                  elt_centers);
  }

  /* Locate entities */

  ple_locator_set_mesh(coupling_ent->locator,
                       location_elts,
                       syr_coupling->dim,
                       coupling_ent->n_elts,
                       NULL,
                       elt_centers,
                       cs_to_syr_dist,
                       cs_coupling_mesh_extents,
                       cs_coupling_point_in_mesh,
                       locate_on_closest);

  if (syr_coupling->verbosity > 0) {
    bft_printf(" [ok]\n");
    bft_printf_flush();
  }

  if (elt_centers != NULL)
    BFT_FREE(elt_centers);

  if (syr_coupling->visualization != 0 && locate_on_closest != NULL) {

    cs_post_activate_writer(0, 1);
    cs_post_write_meshes(-1, 0.0);

    cs_post_write_var(coupling_ent->post_mesh_id,
                      _("distance_to_solid"),
                      1,
                      false,
                      false, /* use_parent, */
                      CS_POST_TYPE_float,
                      -1, /* time-independent variable */
                      0.0,
                      NULL,
                      NULL,
                      cs_to_syr_dist);
  }

  BFT_FREE(cs_to_syr_dist);

  /* Post-process distances from SYRTHES points to Code_Saturne faces */

  if (locate_on_closest != NULL) {

    n_dist_elts = ple_locator_get_n_dist_points(coupling_ent->locator);
    BFT_MALLOC(syr_to_cs_dist, n_dist_elts, float);

    ple_locator_exchange_point_var(coupling_ent->locator,
                                   syr_to_cs_dist,
                                   NULL,
                                   NULL,
                                   sizeof(float),
                                   1,
                                   1);

    if (syr_coupling->visualization != 0) {

      cs_lnum_t i;
      int writer_ids[] = {-1};
      int mesh_id = coupling_ent->post_mesh_id - 1;
      cs_lnum_t *p_vtx_num = NULL;
      fvm_io_num_t *vtx_io_num = NULL;
      fvm_nodal_t *syr_points = fvm_nodal_create("SYRTHES face centers",
                                                 syr_coupling->dim);

      BFT_MALLOC(p_vtx_num, n_dist_elts, cs_lnum_t);

      for (i = 0; i < (cs_lnum_t)n_dist_elts; i++)
        p_vtx_num[i] = i+1;

      fvm_nodal_define_vertex_list(syr_points, n_dist_elts, p_vtx_num);
      fvm_nodal_set_shared_vertices
        (syr_points,
         ple_locator_get_dist_coords(coupling_ent->locator));

      if (cs_glob_n_ranks > 1) {

        vtx_io_num = fvm_io_num_create_from_scan(n_dist_elts);

        fvm_nodal_init_io_num(syr_points,
                              fvm_io_num_get_global_num(vtx_io_num),
                              0);

      }

      cs_post_define_existing_mesh(mesh_id,
                                   syr_points,
                                   0,
                                   true,
                                   false,
                                   1,
                                   writer_ids);

      cs_post_activate_writer(0, 1);
      cs_post_write_meshes(-1, 0.0);

      cs_post_write_vertex_var(mesh_id,
                               _("distance_to_fluid"),
                               1,
                               false,
                               false, /* use parent */
                               CS_POST_TYPE_float,
                               -1, /* time-independent variable */
                               0.0,
                               syr_to_cs_dist);

      cs_post_free_mesh(mesh_id);

      if (cs_glob_n_ranks > 1)
        fvm_io_num_destroy(vtx_io_num);

    } /* Do post-processing */

    BFT_FREE(syr_to_cs_dist);

  }

  /* Check that all points are effectively located */

  n_exterior = ple_locator_get_n_exterior(coupling_ent->locator);

  fvm_parall_counter(&n_exterior, 1);

  if (n_exterior)
    bft_error(__FILE__, __LINE__, 0,
              _("Coupling with SYRTHES impossible:\n"
                "%llu element centers from mesh \"%s\"\n"
                "not located on SYRTHES mesh."),
              (unsigned long long)n_exterior, coupled_mesh_name);

  if (location_elts != coupling_ent->elts)
    fvm_nodal_destroy(location_elts);

  return coupling_ent;
}

/*----------------------------------------------------------------------------
 * Log timing info
 *----------------------------------------------------------------------------*/

static void
_all_comm_times(void)
{
  int coupl_id, ent_id;
  cs_syr4_coupling_t *syr_coupling = NULL;

  if (cs_glob_syr4_n_couplings == 0)
    return;

  cs_log_printf(CS_LOG_PERFORMANCE, "\n");
  cs_log_separator(CS_LOG_PERFORMANCE);

  cs_log_printf(CS_LOG_PERFORMANCE,
                _("\nSYRTHES 4 coupling overheads\n"));

  for (coupl_id = 0; coupl_id < cs_glob_syr4_n_couplings; coupl_id++) {

    syr_coupling = cs_glob_syr4_couplings[coupl_id];

    for (ent_id = 0; ent_id < 2; ent_id++) {

      cs_syr4_coupling_ent_t
        *ce = (ent_id == 0) ? syr_coupling->faces : syr_coupling->cells;
      const char *ent_type[] = {N_("surface"), N_("volume")};

      if (ce != NULL) {

        double location_wtime, exchange_wtime;
        double location_comm_wtime, exchange_comm_wtime;

        if (syr_coupling->syr_name != NULL)
          cs_log_printf(CS_LOG_PERFORMANCE,
                        _("\n  %s (%s):\n\n"),
                        syr_coupling->syr_name, _(ent_type[ent_id]));
        else
          cs_log_printf(CS_LOG_PERFORMANCE,
                        _("\n  coupling %d (%s):\n\n"),
                        coupl_id + 1, _(ent_type[ent_id]));

        ple_locator_get_times(ce->locator,
                              &location_wtime,
                              NULL,
                              &exchange_wtime,
                              NULL);

        ple_locator_get_comm_times(ce->locator,
                                   &location_comm_wtime,
                                   NULL,
                                   &exchange_comm_wtime,
                                   NULL);

        cs_log_printf(CS_LOG_PERFORMANCE,
                      _("    location time:                 %12.3f\n"
                        "      communication and wait:      %12.3f\n"
                        "    variable exchange time:        %12.3f\n"
                        "      communication and wait:      %12.3f\n"),
                      location_wtime, location_comm_wtime,
                      exchange_wtime, exchange_comm_wtime);

      }

    }

  }
}

/*----------------------------------------------------------------------------
 * Destroy coupled entity helper structure.
 *
 * parameters:
 *   coupling ent <-> pointer to structure pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_coupled_ent(cs_syr4_coupling_ent_t **coupling_ent)
{
  cs_syr4_coupling_ent_t *ce = *coupling_ent;

  if (ce == NULL)
    return;

  if (ce->locator != NULL)
    ce->locator = ple_locator_destroy(ce->locator);

  if (ce->solid_temp != NULL)
    BFT_FREE(ce->solid_temp);
  if (ce->flux != NULL)
    BFT_FREE(ce->flux);

  if (ce->hvol != NULL)
    BFT_FREE(ce->hvol);

  if (ce->sol_temp != NULL)
    BFT_FREE(ce->sol_temp);

  if (ce->elts != NULL)
    ce->elts = fvm_nodal_destroy(ce->elts);

  BFT_FREE(*coupling_ent);
}

/*----------------------------------------------------------------------------
 * Update post-processing variables of a Syrthes coupling
 *
 * parameters:
 *   coupling_ent <--  Syrthes coupling structure
 *   step         <--  0: var = wall temperature
 *                     1: var = fluid temperature
 *                     2: var = exchange coefficient
 *   var          <--  Pointer to variable values
 *----------------------------------------------------------------------------*/

static void
_post_var_update(cs_syr4_coupling_ent_t  *coupling_ent,
                 int                      step,
                 const cs_real_t         *var)
{
  cs_lnum_t  n_elts, ii;

  assert(coupling_ent != NULL);

  if (coupling_ent->post_mesh_id == 0)
    return;

  assert(coupling_ent->solid_temp != NULL);
  assert(coupling_ent->flux != NULL);

  n_elts = coupling_ent->n_elts;

  /* Allocate arrays */

  switch(step) {

  case 0:
    for (ii = 0; ii < n_elts; ii++)
      coupling_ent->solid_temp[ii] = var[ii];
    break;

  case 1:
    coupling_ent->tfluid_tmp = coupling_ent->flux;
    for (ii = 0; ii < n_elts; ii++)
      coupling_ent->tfluid_tmp[ii] = var[ii];
    break;

  case 2:
    assert(coupling_ent->tfluid_tmp == coupling_ent->flux);
    for (ii = 0; ii < n_elts; ii++)
      coupling_ent->flux[ii] = var[ii] * (  coupling_ent->solid_temp[ii]
                                          - coupling_ent->flux[ii]);
    coupling_ent->tfluid_tmp = NULL;
    break;

  default:
    assert(0);
  }

}

/*----------------------------------------------------------------------------
 * Ensure conservativity thanks to a corrector coefficient computed by SYRTHES
 * SYRTHES computes a global flux for a given tfluid and hfluid field.
 * Code_Saturne sent before its computed global flux for this time step.
 *
 * parameters:
 *   syr_coupling    <-- SYRTHES coupling structure
 *   coupl_face_list <-- list of coupled boundary faces
 *----------------------------------------------------------------------------*/

static void
_ensure_conservativity(cs_syr4_coupling_t   *syr_coupling,
                       const cs_lnum_t       coupl_face_list[])
{
  cs_lnum_t ii, face_id;

  double g_flux = 0.0, _flux = 0.0, coef = 0.0;

  double  *surf = cs_glob_mesh_quantities->b_face_surf;
  cs_syr4_coupling_ent_t  *coupling_ent = NULL;

  /* Sanity checks */

  assert(surf != NULL);
  assert(coupl_face_list != NULL);
  assert(syr_coupling != NULL);
  coupling_ent = syr_coupling->faces;
  assert(coupling_ent != NULL);

  /* Compute Code_Saturne's global flux */

  for (ii = 0; ii < coupling_ent->n_elts; ii++) {
    face_id = coupl_face_list[ii] - 1;
    _flux += coupling_ent->flux[ii] * surf[face_id];
  }

#if defined(HAVE_MPI)
  if (cs_glob_n_ranks > 1)
    MPI_Reduce(&_flux, &g_flux, 1, MPI_DOUBLE, MPI_SUM, 0, cs_glob_mpi_comm);
#endif

  if (cs_glob_n_ranks == 1)
    g_flux = _flux;

  /* Send the computed global flux to SYRTHES and receive the corrector
     coefficient */

#if defined(HAVE_MPI)
  if (cs_glob_rank_id < 1) {

    MPI_Status  status;

    /* Send global flux */

    MPI_Send(&g_flux, 1, MPI_DOUBLE,
             syr_coupling->syr_root_rank,
             cs_syr4_coupling_tag,
             syr_coupling->comm);

    if (syr_coupling->verbosity > 0)
      bft_printf(" Global heat flux exchanged with SYRTHES in W: %5.3e\n",
                 g_flux);

    /* Receive corrector coefficient */

    MPI_Recv(&coef, 1, MPI_DOUBLE,
             syr_coupling->syr_root_rank,
             cs_syr4_coupling_tag,
             syr_coupling->comm,
             &status);

  }
#endif

  /* Print message */

  if (syr_coupling->verbosity > 0)
    bft_printf(" Correction coefficient used to force conservativity during"
               " coupling with SYRTHES: %5.3e\n", coef);

}

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Get number of SYRTHES couplings.
 *
 * returns:
 *   number of SYRTHES couplings
 *----------------------------------------------------------------------------*/

cs_lnum_t
cs_syr4_coupling_n_couplings(void)
{
  return cs_glob_syr4_n_couplings;
}

/*----------------------------------------------------------------------------
 * Get pointer to SYRTHES coupling.
 *
 * parameters:
 *   coupling_id <-- Id (0 to n-1) of SYRTHES coupling
 *
 * returns:
 *   pointer to SYRTHES coupling structure
 *----------------------------------------------------------------------------*/

cs_syr4_coupling_t *
cs_syr4_coupling_by_id(cs_lnum_t coupling_id)
{
  cs_syr4_coupling_t  *retval = NULL;

  if (   coupling_id > -1
      && coupling_id < cs_glob_syr4_n_couplings)
    retval = cs_glob_syr4_couplings[coupling_id];

  return retval;
}

/*----------------------------------------------------------------------------
 * Create a syr4_coupling_t structure.
 *
 * parameters:
 *   dim                <-- spatial mesh dimension
 *   ref_axis           <-- reference axis
 *   face_sel_criterion <-- criterion for selection of boundary faces
 *   cell_sel_criterion <-- criterion for selection of cells
 *   syr_name           <-- SYRTHES application name
 *   verbosity          <-- verbosity level
 *   visualization      <-- visualization output flag
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_add(cs_lnum_t    dim,
                     cs_lnum_t    ref_axis,
                     const char  *face_sel_criterion,
                     const char  *cell_sel_criterion,
                     const char  *syr_name,
                     int          verbosity,
                     int          visualization)
{
  cs_syr4_coupling_t *syr_coupling = NULL;

  /* Allocate _cs_syr4_coupling_t structure */

  BFT_REALLOC(cs_glob_syr4_couplings,
              cs_glob_syr4_n_couplings + 1, cs_syr4_coupling_t *);
  BFT_MALLOC(syr_coupling, 1, cs_syr4_coupling_t);

  syr_coupling->dim = dim;
  syr_coupling->ref_axis = ref_axis;

  syr_coupling->syr_name = NULL;

  if (syr_name != NULL) {
    BFT_MALLOC(syr_coupling->syr_name, strlen(syr_name) + 1, char);
    strcpy(syr_coupling->syr_name, syr_name);
  }
  else {
    BFT_MALLOC(syr_coupling->syr_name, 1, char);
    syr_coupling->syr_name[0] = '\0';
  }

  /* Selection criteria  */

  syr_coupling->face_sel = NULL;
  syr_coupling->cell_sel = NULL;

  if (face_sel_criterion != NULL) {
    BFT_MALLOC(syr_coupling->face_sel, strlen(face_sel_criterion) + 1, char);
    strcpy(syr_coupling->face_sel, face_sel_criterion);
  }
  if (cell_sel_criterion != NULL) {
    BFT_MALLOC(syr_coupling->cell_sel, strlen(cell_sel_criterion) + 1, char);
    strcpy(syr_coupling->cell_sel, cell_sel_criterion);
  }

  if (face_sel_criterion == NULL && cell_sel_criterion == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("Coupling with SYRTHES impossible.\n"
                "No selection criteria for faces or cells to couple."));


  syr_coupling->faces = NULL;
  syr_coupling->cells = NULL;

  syr_coupling->verbosity = verbosity;
  syr_coupling->visualization = visualization;

  /* Initialize communicators */

#if defined(HAVE_MPI)

  syr_coupling->comm = MPI_COMM_NULL;
  syr_coupling->n_syr_ranks = 0;
  syr_coupling->syr_root_rank = -1;

#endif

  /* Update coupling array and return */

  cs_glob_syr4_couplings[cs_glob_syr4_n_couplings] = syr_coupling;
  cs_glob_syr4_n_couplings++;
}

/*----------------------------------------------------------------------------
 * Destroy cs_syr4_coupling_t structures
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_all_destroy(void)
{
  cs_lnum_t i_coupl;
  cs_syr4_coupling_t *syr_coupling = NULL;

  if (cs_glob_syr4_n_couplings == 0)
    return;

  _all_comm_times();

  for (i_coupl = 0; i_coupl < cs_glob_syr4_n_couplings; i_coupl++) {

    syr_coupling = cs_glob_syr4_couplings[i_coupl];

    /* Free _cs_syr4_coupling structure */

    if (syr_coupling->syr_name != NULL)
      BFT_FREE(syr_coupling->syr_name);

    if (syr_coupling->face_sel != NULL)
      BFT_FREE(syr_coupling->face_sel);
    if (syr_coupling->cell_sel != NULL)
      BFT_FREE(syr_coupling->cell_sel);

    if (syr_coupling->faces != NULL)
      _destroy_coupled_ent(&(syr_coupling->faces));
    if (syr_coupling->cells != NULL)
      _destroy_coupled_ent(&(syr_coupling->cells));

    /* Close communication */

    _finalize_comm(syr_coupling);

    BFT_FREE(syr_coupling);

  } /* End of loop on cs_glob_syr4_couplings */

  cs_glob_syr4_n_couplings = 0;
  BFT_FREE(cs_glob_syr4_couplings);

  bft_printf(_("\nStructures associated with SYRTHES 4 coupling freed.\n"));
  bft_printf_flush();
}

/*----------------------------------------------------------------------------
 * Set conservativity forcing flag to True (1) or False (0) for all defined
 * SYRTHES couplings
 *
 * parameter:
 *   flag     <--  Conservativity forcing flag to set
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_set_conservativity(int  flag)
{
  cs_syr4_coupling_conservativity = flag;
}

/*----------------------------------------------------------------------------
 * Set explicit treatment for the source terms in SYRTHES volume couplings
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_set_explicit_treatment(void)
{
  cs_syr4_coupling_implicit = 0;
}

/*----------------------------------------------------------------------------
 * Initialize communicator for SYRTHES coupling
 *
 * parameters:
 *   syr_coupling  <-> SYRTHES coupling structure
 *   coupling_id   <-- id of this coupling (for log file message)
 *   syr_root_rank <-- SYRTHES root rank
 *   n_syr_ranks   <-- Number of ranks associated with SYRTHES
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_init_comm(cs_syr4_coupling_t *syr_coupling,
                           int                 coupling_id,
                           int                 syr_root_rank,
                           int                 n_syr_ranks)
{
#if defined(HAVE_MPI)

  char  volume_flag = ' ';
  char  boundary_flag = ' ';
  char  conservativity_flag = '1';
  char  op_name_send[32 + 1];
  char  op_name_recv[32 + 1];

  syr_coupling->n_syr_ranks = n_syr_ranks;
  syr_coupling->syr_root_rank = syr_root_rank;

  _init_comm(syr_coupling, coupling_id);

  /* Exchange coupling options */

  if (syr_coupling->face_sel != NULL)
    boundary_flag = 'b';
  if (syr_coupling->cell_sel != NULL)
    volume_flag = 'v';
  if (cs_syr4_coupling_conservativity == 0)
    conservativity_flag = '0';

  sprintf(op_name_send, "coupling:type:%c%c%c",
          boundary_flag, volume_flag, conservativity_flag);

  _exchange_sync(syr_coupling, op_name_send, op_name_recv);

  /* Only Code_Saturne set its conservativity flag.
     To avoid an error, set the conservativity flag in op_name_recv */
  op_name_recv[16] = op_name_send[16];

  if (strcmp(op_name_recv, op_name_send))
    bft_error
      (__FILE__, __LINE__, 0,
       _("========================================================\n"
         "   ** Incompatible SYRTHES coupling options:\n"
         "      ------------------------------------------------\n"
         "      Code_Saturne options: \"%s\"\n"
         "      SYRTHES options:      \"%s\"\n"
         "========================================================\n"),
       op_name_send, op_name_recv);

#endif
}

/*----------------------------------------------------------------------------
 * Define coupled mesh and send it to SYRTHES
 *
 * Optional post-processing output is also built at this stage.
 *
 * parameters:
 *   syr_coupling <-- SYRTHES coupling structure
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_init_mesh(cs_syr4_coupling_t  *syr_coupling)
{
  char op_name_send[32 + 1];
  char op_name_recv[32 + 1];

  const cs_lnum_t verbosity = syr_coupling->verbosity;

  if (verbosity > 0)
    bft_printf(_("\n ** Processing the mesh for SYRTHES coupling "
                 "\"%s\"\n\n"),
                 syr_coupling->syr_name);

  /* Define coupled mesh */

  assert(syr_coupling->dim == 3 || syr_coupling->dim == 2);

  if (syr_coupling->face_sel != NULL)
    syr_coupling->faces = _create_coupled_ent(syr_coupling,
                                              syr_coupling->face_sel,
                                              syr_coupling->dim - 1);

  if (syr_coupling->cell_sel != NULL) {
    syr_coupling->cells = _create_coupled_ent(syr_coupling,
                                              syr_coupling->cell_sel,
                                              syr_coupling->dim);
  }

  /* Communication with SYRTHES */
  /*----------------------------*/

  /* Ready to start time iterations */

  strcpy(op_name_send, "coupling:start");

  _exchange_sync(syr_coupling, op_name_send, op_name_recv);

  if (strcmp(op_name_recv, "coupling:start"))
    bft_error(__FILE__, __LINE__, 0,
              _(" Message received from SYRTHES: \"%s\"\n"
                " indicates an error or is unexpected."),
              op_name_recv);

  else if (verbosity > 0) {
    bft_printf(_("\n ** Mesh located for SYRTHES coupling \"%s\".\n\n"),
               syr_coupling->syr_name);
    bft_printf_flush();
  }

}

/*----------------------------------------------------------------------------
 * Return 1 if this coupling is a surface coupling else 0
 *
 * parameters:
 *   syr_coupling <-- SYRTHES coupling structure
 *
 * returns:
 *   1 or 0
 *----------------------------------------------------------------------------*/

int
cs_syr4_coupling_is_surf(const cs_syr4_coupling_t  *syr_coupling)
{
  int retval = 0;

  assert(syr_coupling != NULL);

  if (syr_coupling->faces != NULL)
    retval = 1;

  return retval;
}

/*----------------------------------------------------------------------------
 * Return 1 if this coupling is a volume coupling else 0
 *
 * parameters:
 *   syr_coupling <-- SYRTHES coupling structure
 *
 * returns:
 *   1 or 0
 *----------------------------------------------------------------------------*/

int
cs_syr4_coupling_is_vol(const cs_syr4_coupling_t  *syr_coupling)
{
  int retval = 0;

  assert(syr_coupling != NULL);

  if (syr_coupling->cells != NULL)
    retval = 1;

  return retval;
}

/*----------------------------------------------------------------------------
 * Get number of associated coupled elements in main mesh
 *
 * parameters:
 *   syr_coupling <-- SYRTHES coupling structure
 *   mode          <-- 0 (surface); 1 (volume)
 *
 * returns:
 *   number of vertices in coupled mesh
 *----------------------------------------------------------------------------*/

cs_lnum_t
cs_syr4_coupling_get_n_elts(const cs_syr4_coupling_t *syr_coupling,
                            int                       mode)
{
  cs_syr4_coupling_ent_t  *coupling_ent = NULL;
  cs_lnum_t retval = 0;

  /* Sanity checks */

  assert(syr_coupling != NULL);
  assert(mode == 0 || mode == 1);

  if (mode == 0)
    coupling_ent = syr_coupling->faces;
  else
    coupling_ent = syr_coupling->cells;

  if (coupling_ent != NULL)
    retval = coupling_ent->n_elts;

  return retval;
}

/*----------------------------------------------------------------------------
 * Get local numbering of coupled elements
 *
 * parameters:
 *   syr_coupling  <-- SYRTHES coupling structure
 *   cpl_elt_lst   --> List of coupled elements (1 to n)
 *   mode          <-- 0 (surface); 1 (volume)
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_get_elt_list(const cs_syr4_coupling_t  *syr_coupling,
                              cs_int_t                   cpl_elt_lst[],
                              int                        mode)
{
  cs_syr4_coupling_ent_t  *coupling_ent = NULL;

  /* Sanity checks */

  assert(sizeof(cs_lnum_t) == sizeof(cs_int_t));
  assert(syr_coupling != NULL);
  assert(mode == 0 || mode == 1);

  if (mode == 0)
    coupling_ent = syr_coupling->faces;
  else
    coupling_ent = syr_coupling->cells;

  if (coupling_ent != NULL)
    fvm_nodal_get_parent_num(coupling_ent->elts,
                             coupling_ent->elt_dim,
                             cpl_elt_lst);
}

/*----------------------------------------------------------------------------
 * Receive coupling variables from SYRTHES
 *
 * parameters:
 *   syr_coupling <-- SYRTHES coupling structure
 *   tsolid       --> solid temperature
 *   mode         <-- 0: surface coupling; 1: volume coupling
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_recv_tsolid(cs_syr4_coupling_t  *syr_coupling,
                             cs_real_t            tsolid[],
                             int                  mode)
{
  int  i;

  cs_syr4_coupling_ent_t  *coupling_ent = NULL;

  assert(mode == 0 || mode == 1);

  if (mode == 0)
    coupling_ent = syr_coupling->faces;
  else
    coupling_ent = syr_coupling->cells;

  if (coupling_ent == NULL)
    return;

  /* Receive data */

  ple_locator_exchange_point_var(coupling_ent->locator,
                                 NULL,
                                 tsolid,
                                 NULL,
                                 sizeof(cs_real_t),
                                 1,
                                 0);

  if (coupling_ent->n_elts > 0) {
    _post_var_update(coupling_ent, 0, tsolid);

    if (mode == 1) { /* Saved tsolid for a future used
                        in source term definition */
      assert(coupling_ent->sol_temp != NULL);
      for (i = 0; i < coupling_ent->n_elts; i++)
        coupling_ent->sol_temp[i] = tsolid[i];
    }

  }

}

/*----------------------------------------------------------------------------
 * Send coupling variables to SYRTHES
 *
 * parameters:
 *   syr_coupling    <-- SYRTHES coupling structure
 *   coupl_face_list <-- list of coupled boundary faces
 *   tf              <-- fluid temperature
 *   hf              <-- fluid heat exchange coef. (numerical or user-defined)
 *   mode            <-- 0: surface coupling; 1: volume coupling
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_send_tf_hf(cs_syr4_coupling_t  *syr_coupling,
                            const cs_lnum_t      cpl_elt_lst[],
                            cs_real_t            tf[],
                            cs_real_t            hf[],
                            cs_int_t             mode)
{
  cs_lnum_t ii;

  cs_lnum_t n_dist = 0;

  double *send_var = NULL;
  cs_syr4_coupling_ent_t  *coupling_ent = NULL;

  const cs_lnum_t *dist_loc = NULL;

  assert(mode == 0 || mode == 1);

  if (mode == 0)
    coupling_ent = syr_coupling->faces;
  else
    coupling_ent = syr_coupling->cells;

  if (coupling_ent == NULL)
    return;

  n_dist = ple_locator_get_n_dist_points(coupling_ent->locator);
  dist_loc = ple_locator_get_dist_locations(coupling_ent->locator);

  /* Prepare and send data */

  BFT_MALLOC(send_var, n_dist*2, double);

  for (ii = 0; ii < n_dist; ii++) {
    send_var[ii*2]     = tf[dist_loc[ii] - 1];
    send_var[ii*2 + 1] = hf[dist_loc[ii] - 1];
  }

  ple_locator_exchange_point_var(coupling_ent->locator,
                                 send_var,
                                 NULL,
                                 NULL,
                                 sizeof(double),
                                 2,
                                 0);

  BFT_FREE(send_var);

  if (coupling_ent->n_elts > 0) {
    _post_var_update(coupling_ent, 1, tf);
    _post_var_update(coupling_ent, 2, hf);

    if (mode == 1) { /* Saved hf for a future used in source term definition */
      assert(coupling_ent->hvol != NULL);
      for (ii = 0; ii < coupling_ent->n_elts; ii++)
        coupling_ent->hvol[ii] = hf[ii];
    }

  }

  /* Exchange flux and corrector coefficient to ensure conservativity */

  if (cs_syr4_coupling_conservativity > 0 && mode == 0)
    _ensure_conservativity(syr_coupling, cpl_elt_lst);

}

/*----------------------------------------------------------------------------
 * Compute the explicit/implicit contribution to source terms in case of
 * volume coupling with SYRTHES4
 *
 * parameters:
 *   syr_coupling  <-- SYRTHES coupling structure
 *   tf            <-- fluid temperature
 *   ctbimp        <-> implicit contribution
 *   ctbexp        <-> explicit contribution
 *----------------------------------------------------------------------------*/

void
cs_syr4_coupling_ts_contrib(cs_syr4_coupling_t  *syr_coupling,
                            const cs_real_t      tf[],
                            cs_real_t            ctbimp[],
                            cs_real_t            ctbexp[])
{
  int  i;

  double  *hvol = NULL, *solid_temp = NULL;
  cs_syr4_coupling_ent_t  *ent = NULL;

  /* sanity checks */

  assert(syr_coupling != NULL);
  assert(syr_coupling->cells != NULL);

  ent = syr_coupling->cells;
  hvol = ent->hvol;
  solid_temp = ent->sol_temp;

  assert(hvol != NULL);
  assert(solid_temp != NULL);

  /* Compute contribution */

  if (cs_syr4_coupling_implicit == 0) { /* Explicit treatment */

    for (i = 0; ent->n_elts; i++) {
      ctbexp[i] = -hvol[i] * (tf[i] - solid_temp[i]);
      ctbimp[i] = 0.0;
    }

  }
  else { /* Implicit treatment */

    for (i = 0; ent->n_elts; i++) {
      ctbexp[i] = hvol[i] * solid_temp[i];
      ctbimp[i] = hvol[i];
    }

  } /* Test if implicit */

}

/*----------------------------------------------------------------------------*/

END_C_DECLS
