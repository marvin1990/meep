#include "meep-ctl.hpp"
#include <ctlgeom.h>

using namespace ctlio;

#define master_printf meep::master_printf
#define MTS material_type_struct

static meep::ndim dim = meep::D3;

/***********************************************************************/

void set_dimensions(int dims)
{
  if (dims == CYLINDRICAL) {
    dimensions = 2;
    dim = meep::Dcyl;
  }
  else {
    dimensions = dims;
    dim = meep::ndim(dims - 1);
  }
}

vector3 vec_to_vector3(const meep::vec &v)
{
  vector3 v3;
  
  switch (v.dim) {
  case meep::D1:
    v3.x = 0;
    v3.y = 0;
    v3.z = v.z();
    break;
  case meep::D2:
    v3.x = v.x();
    v3.y = v.y();
    v3.z = 0;
    break;
  case meep::D3:
    v3.x = v.x();
    v3.y = v.y();
    v3.z = v.z();
    break;
  case meep::Dcyl:
    v3.x = v.r();
    v3.y = 0;
    v3.z = v.z();
    break;
  }
  return v3;
}

meep::vec vector3_to_vec(const vector3 v3)
{
  switch (dim) {
  case meep::D1:
    return meep::vec(v3.z);
  case meep::D2:
    return meep::vec(v3.x, v3.y);
  case meep::D3:
    return meep::vec(v3.x, v3.y, v3.z);
  case meep::Dcyl:
    return meep::veccyl(v3.x, v3.z);
  default:
    meep::abort("unknown dimensionality in vector3_to_vec");
  }
}

static geom_box gv2box(const meep::geometric_volume &gv)
{
  geom_box box;
  box.low = vec_to_vector3(gv.get_min_corner());
  box.high = vec_to_vector3(gv.get_max_corner());
  return box;
}

/***********************************************************************/

class geom_epsilon : public meep::material_function {
  geometric_object_list geometry;
  geom_box_tree geometry_tree;
  geom_box_tree restricted_tree;
  
public:
  geom_epsilon(geometric_object_list g,
	       const meep::geometric_volume &gv);
  virtual ~geom_epsilon();
  
  virtual void set_volume(const meep::geometric_volume &gv);
  virtual void unset_volume(void);
  virtual double eps(const meep::vec &r);
  virtual bool has_kerr();
  virtual double kerr(const meep::vec &r);

  virtual double sigma(const meep::vec &r);
  void add_polarizabilities(meep::structure *s);
};

geom_epsilon::geom_epsilon(geometric_object_list g,
			   const meep::geometric_volume &gv)
{
  geometry = g; // don't bother making a copy, only used in one place
  
  if (meep::am_master()) {
    for (int i = 0; i < geometry.num_items; ++i) {
      display_geometric_object_info(5, geometry.items[i]);
      
      if (geometry.items[i].material.which_subclass 
	  == MTS::DIELECTRIC)
	printf("%*sdielectric constant epsilon = %g\n",
	       5 + 5, "",
	       geometry.items[i].material.
	       subclass.dielectric_data->epsilon);
    }
  }
  
  geom_fix_objects0(geometry);
  geom_box box = gv2box(gv);
  geometry_tree = create_geom_box_tree0(geometry, box);
  if (verbose && meep::am_master()) {
    printf("Geometric-object bounding-box tree:\n");
    display_geom_box_tree(5, geometry_tree);
    
    int tree_depth, tree_nobjects;
    geom_box_tree_stats(geometry_tree, &tree_depth, &tree_nobjects);
    master_printf("Geometric object tree has depth %d "
		  "and %d object nodes (vs. %d actual objects)\n",
		  tree_depth, tree_nobjects, geometry.num_items);
  }
  
  restricted_tree = geometry_tree;
}

geom_epsilon::~geom_epsilon()
{
  unset_volume();
  destroy_geom_box_tree(geometry_tree);
}

void geom_epsilon::unset_volume(void)
{
  if (restricted_tree != geometry_tree) {
    destroy_geom_box_tree(restricted_tree);
    restricted_tree = geometry_tree;
  }
}

void geom_epsilon::set_volume(const meep::geometric_volume &gv)
{
  unset_volume();
  
  geom_box box = gv2box(gv);
  restricted_tree = create_geom_box_tree0(geometry, box);
}

static material_type eval_material_func(function material_func, vector3 p)
{
  SCM pscm = ctl_convert_vector3_to_scm(p);
  material_type material;
  SCM mo;
  
  mo = gh_call1(material_func, pscm);
  material_type_input(mo, &material);
  
  while (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material_type m;
    
    mo = gh_call1(material.subclass.
		  material_function_data->material_func,
		  pscm);
    material_type_input(mo, &m);
    material_type_destroy(material);
    material = m;
  }
  
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material_type_copy(&default_material, &material);
  }
  CK(material.which_subclass != MTS::MATERIAL_FUNCTION,
     "infinite loop in material functions");
  
  return material;
}

double geom_epsilon::eps(const meep::vec &r)
{
  double eps = 1.0;
  vector3 p = vec_to_vector3(r);

#ifdef DEBUG
  if (p.x < restricted_tree->b.low.x ||
      p.y < restricted_tree->b.low.y ||
      p.z < restricted_tree->b.low.z ||
      p.x > restricted_tree->b.high.x ||
      p.y > restricted_tree->b.high.y ||
      p.z > restricted_tree->b.high.z)
    meep::abort("invalid point (%g,%g,%g)\n", p.x,p.y,p.z);
#endif

  boolean inobject;
  material_type material =
    material_of_unshifted_point_in_tree_inobject(p, restricted_tree, &inobject);
  
  int destroy_material = 0;
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material = default_material;
  }
  if (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material = eval_material_func(material.subclass.
				  material_function_data->material_func,
				  p);
    destroy_material = 1;
  }
  
  switch (material.which_subclass) {
  case MTS::DIELECTRIC:
    eps = material.subclass.dielectric_data->epsilon;
    break;
  case MTS::PERFECT_METAL:
    eps = -meep::infinity;
    break;
  default:
    CK(0, "unknown material type");
  }
  
  if (destroy_material)
    material_type_destroy(material);
  
  return eps;
}

bool geom_epsilon::has_kerr()
{
  for (int i = 0; i < geometry.num_items; ++i) {
    if (geometry.items[i].material.which_subclass == MTS::DIELECTRIC) {
      if (geometry.items[i].material.subclass.dielectric_data->chi3 != 0)
	return true; 
    }
  }
    /* FIXME: what to do about material-functions?
       Currently, we require that at least *one* ordinary material
       property have non-zero chi3 for Kerr to be enabled.   It might
       be better to have set_kerr automatically delete kerr[] if the
       chi3's are all zero. */
  return (default_material.which_subclass == MTS::DIELECTRIC &&
	  default_material.subclass.dielectric_data->chi3 != 0);
}

double geom_epsilon::kerr(const meep::vec &r) {
  vector3 p = vec_to_vector3(r);

  boolean inobject;
  material_type material =
    material_of_unshifted_point_in_tree_inobject(p, restricted_tree, &inobject);
  
  int destroy_material = 0;
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material = default_material;
  }
  if (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material = eval_material_func(material.subclass.
				  material_function_data->material_func,
				  p);
    destroy_material = 1;
  }
  
  double chi3;
  switch (material.which_subclass) {
  case MTS::DIELECTRIC:
    chi3 = material.subclass.dielectric_data->chi3;
    break;
  default:
    chi3 = 0;
  }
  
  if (destroy_material)
    material_type_destroy(material);
  
  return chi3;
}

double geom_epsilon::sigma(const meep::vec &r) {
  vector3 p = vec_to_vector3(r);

  boolean inobject;
  material_type material =
    material_of_unshifted_point_in_tree_inobject(p, restricted_tree, &inobject);
  
  int destroy_material = 0;
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material = default_material;
  }
  if (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material = eval_material_func(material.subclass.
				  material_function_data->material_func,
				  p);
    destroy_material = 1;
  }
  
  double sigma = 0;
  if (material.which_subclass == MTS::DIELECTRIC) {
    polarizability_list plist = 
      material.subclass.dielectric_data->polarizations;
    for (int j = 0; j < plist.num_items; ++j)
      if (plist.items[j].omega == omega &&
	  plist.items[j].gamma == gamma &&
	  plist.items[j].delta_epsilon == deps &&
	  plist.items[j].energy_saturation == energy_sat) {
	sigma = plist.items[j].sigma;
	break;
      }
  }
  
  if (destroy_material)
    material_type_destroy(material);

  return sigma;
}

struct pol {
  double omega, gamma, deps, esat;
  struct pol *next;
};

// add a polarization to the list if it is not already there
static pol *add_pol(pol *pols,
		    double omega, double gamma, double deps, double esat)
{
  struct pol *p = pols;
  while (p && !(p->omega == omega && p->gamma == gamma
		&& p->deps == deps && p->esat == esat))
    p = p->next;
  if (!p) {
    p = new pol;
    p->omega = omega;
    p->gamma = gamma;
    p->deps = deps;
    p->esat = esat;
    p->next = pols;
    pols = p;
  }
  return pols;
}

static pol *add_pols(pol *pols, const polarizability_list plist) {
  for (int j = 0; j < plist.num_items; ++j) {
    pols = add_pol(pols,
		   plist.items[j].omega, plist.items[j].gamma,
		   plist.items[j].delta_epsilon,
		   plist.items[j].energy_saturation);
  }
  return pols;
}

void geom_epsilon::add_polarizabilities(meep::structure *s) {
  pol *pols = 0;

  // construct a list of the unique polarizabilities in the geometry:
  for (int i = 0; i < geometry.num_items; ++i) {
    if (geometry.items[i].material.which_subclass == MTS::DIELECTRIC)
      pols = add_pols(pols, geometry.items[i].material
		      .subclass.dielectric_data->polarizations);
  }
  if (default_material.which_subclass == MTS::DIELECTRIC)
    pols = add_pols(pols, default_material
		    .subclass.dielectric_data->polarizations);
    
  for (struct pol *p = pols; p; p = p->next) {
    master_printf("polarizability: omega=%g, gamma=%g, deps=%g, esat=%g\n",
		  p->omega, p->gamma, p->deps, p->esat);
    s->add_polarizability(*this, p->omega, p->gamma, p->deps, p->esat);
  }
  
  while (pols) {
    struct pol *p = pols;
    pols = pols->next;
    delete p;
  }
}

/***********************************************************************/

meep::structure *make_structure(int dims, vector3 size, vector3 center,
				double resolution, bool enable_averaging,
				bool ensure_periodicity_p,
				geometric_object_list geometry,
				material_type default_mat,
				pml_list pml_layers,
				symmetry_list symmetries,
				int num_chunks, double Courant)
{
  master_printf("-----------\nInitializing structure...\n");
  
  // only cartesian lattices are currently allowed
  geom_initialize();
  geometry_center = center;
  
  number no_size = 2.0 / ctl_get_number("infinity");
  if (size.x <= no_size)
    size.x = 0.0;
  if (size.y <= no_size)
    size.y = 0.0;
  if (size.z <= no_size)
    size.z = 0.0;
  
  set_dimensions(dims);
  
  geometry_lattice.size = size;

  master_printf("Working in %s dimensions.\n", meep::dimension_name(dim));
  
  meep::volume v;
  switch (dims) {
  case 0: case 1:
    v = meep::vol1d(size.z, resolution);
    break;
  case 2:
    v = meep::vol2d(size.x, size.y, resolution);
    break;
  case 3:
    v = meep::vol3d(size.x, size.y, size.z, resolution);
    break;
  case CYLINDRICAL:
    v = meep::volcyl(size.x, size.z, resolution);
    break;
  default:
    CK(0, "unsupported dimensionality");
  }
  v.center_origin();
  v.shift_origin(vector3_to_vec(center));
  
  meep::symmetry S;
  for (int i = 0; i < symmetries.num_items; ++i) 
    switch (symmetries.items[i].which_subclass) {
    case symmetry::SYMMETRY_SELF: break; // identity
    case symmetry::MIRROR_SYM:
      S = S + meep::mirror(meep::direction(symmetries.items[i].direction), v)
	* complex<double>(symmetries.items[i].phase.re,
			  symmetries.items[i].phase.im);
      break;
    case symmetry::ROTATE2_SYM:
      S = S + meep::rotate2(meep::direction(symmetries.items[i].direction), v)
	* complex<double>(symmetries.items[i].phase.re,
			  symmetries.items[i].phase.im);
      break;
    case symmetry::ROTATE4_SYM:
      S = S + meep::rotate4(meep::direction(symmetries.items[i].direction), v)
	* complex<double>(symmetries.items[i].phase.re,
			  symmetries.items[i].phase.im);
      break;
    }

  meep::boundary_region br;
  for (int i = 0; i < pml_layers.num_items; ++i) {
    using namespace meep;
    if (pml_layers.items[i].direction == -1) {
      LOOP_OVER_DIRECTIONS(v.dim, d) {
	if (pml_layers.items[i].side == -1) {
	  FOR_SIDES(b)
	    br = br + meep::boundary_region(meep::boundary_region::PML,
					    pml_layers.items[i].thickness,
					    pml_layers.items[i].strength,
					    d, b);
	}
	else
	  br = br + meep::boundary_region(meep::boundary_region::PML,
					  pml_layers.items[i].thickness,
					  pml_layers.items[i].strength,
					  d,
					  (meep::boundary_side) 
					  pml_layers.items[i].side);
      }
    }
    else {
	if (pml_layers.items[i].side == -1) {
	  FOR_SIDES(b)
	    br = br + meep::boundary_region(meep::boundary_region::PML,
					    pml_layers.items[i].thickness,
					    pml_layers.items[i].strength,
					    (meep::direction)
					    pml_layers.items[i].direction,
					    b);
	}
	else
	  br = br + meep::boundary_region(meep::boundary_region::PML,
					  pml_layers.items[i].thickness,
					  pml_layers.items[i].strength,
					  (meep::direction)
					  pml_layers.items[i].direction,
					  (meep::boundary_side) 
					  pml_layers.items[i].side);
    }
  }
  
  ensure_periodicity = ensure_periodicity_p;
  default_material = default_mat;
  geom_epsilon geps(geometry, v.pad().surroundings());

  meep::structure *s = new meep::structure(v, geps, br, S, 
					   num_chunks, Courant,
					   enable_averaging);

  geps.add_polarizabilities(s);

  master_printf("-----------\n");
  
  return s;
}

/*************************************************************************/
