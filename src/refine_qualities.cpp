#include "refine_qualities.hpp"

#include "access.hpp"
#include "algebra.hpp"
#include "loop.hpp"
#include "metric.hpp"
#include "quality.hpp"
#include "refine_topology.hpp"
#include "simplices.hpp"

namespace Omega_h {

struct RealRefineQualities {
  RealRefineQualities(Mesh*, LOs) {}
  template <Int dim>
  INLINE Real measure(Int, Few<Vector<dim>, dim + 1> p, Few<LO, dim>) const {
    return real_element_quality(p);
  }
};

struct MetricRefineQualities {
  Reals vert_metrics;
  Reals midpt_metrics;
  MetricRefineQualities(Mesh* mesh, LOs candidates)
      : vert_metrics(mesh->get_array<Real>(VERT, "metric")),
        /* TODO: we could reuse the results of this instead of recomputing
         * them when transferring an OMEGA_H_METRIC field in transfer.cpp
         */
        midpt_metrics(get_mident_metrics(
            mesh, EDGE, candidates, mesh->get_array<Real>(VERT, "metric"))) {}
  template <Int dim>
  DEVICE Real measure(
      Int cand, Few<Vector<dim>, dim + 1> p, Few<LO, dim> csv2v) const {
    Few<Matrix<dim, dim>, dim + 1> ms;
    for (Int csv = 0; csv < dim; ++csv)
      ms[csv] = get_symm<dim>(vert_metrics, csv2v[csv]);
    ms[dim] = get_symm<dim>(midpt_metrics, cand);
    auto m = maxdet_metric(ms);
    return metric_element_quality(p, m);
  }
};

template <typename Measure, Int dim>
static Reals refine_qualities_tmpl(Mesh* mesh, LOs candidates) {
  auto ev2v = mesh->ask_verts_of(EDGE);
  auto cv2v = mesh->ask_verts_of(dim);
  auto e2c = mesh->ask_up(EDGE, dim);
  auto e2ec = e2c.a2ab;
  auto ec2c = e2c.ab2b;
  auto ec_codes = e2c.codes;
  auto coords = mesh->coords();
  auto ncands = candidates.size();
  auto measure = Measure(mesh, candidates);
  Write<Real> quals_w(ncands);
  auto f = LAMBDA(LO cand) {
    auto e = candidates[cand];
    auto eev2v = gather_verts<2>(ev2v, e);
    auto ep = gather_vectors<2, dim>(coords, eev2v);
    auto midp = (ep[0] + ep[1]) / 2.;
    auto minqual = 1.0;
    for (auto ec = e2ec[e]; ec < e2ec[e + 1]; ++ec) {
      auto c = ec2c[ec];
      auto code = ec_codes[ec];
      auto cce = code_which_down(code);
      auto rot = code_rotation(code);
      auto ccv2v = gather_verts<dim + 1>(cv2v, c);
      for (Int eev = 0; eev < 2; ++eev) {
        /* a new cell is formed from an old cell by finding
           its side that is opposite to one of the edge endpoints
           and connecting it to the midpoint to form the new cell
           (see refine_domain_interiors) */
        auto cev = eev ^ rot;
        auto ccv = DownTemplate<dim, EDGE>::get(cce, cev);
        auto ccs = OppositeTemplate<dim, VERT>::get(ccv);
        Few<LO, dim> csv2v;
        Few<Vector<dim>, dim + 1> ncp;
        for (Int csv = 0; csv < dim; ++csv) {
          auto ccv2 = DownTemplate<dim, dim - 1>::get(ccs, csv);
          auto v2 = ccv2v[ccv2];
          csv2v[csv] = v2;
          ncp[csv] = get_vector<dim>(coords, v2);
        }
        ncp[dim] = midp;
        flip_new_elem<dim>(&csv2v[0]);
        flip_new_elem<dim>(&ncp[0]);
        auto cqual = measure.measure(cand, ncp, csv2v);
        minqual = min2(minqual, cqual);
      }
    }
    quals_w[cand] = minqual;
  };
  parallel_for(ncands, f);
  auto cand_quals = Reals(quals_w);
  return mesh->sync_subset_array(EDGE, cand_quals, candidates, -1.0, 1);
}

Reals refine_qualities(Mesh* mesh, LOs candidates) {
  auto dim = mesh->dim();
  auto have_metric = mesh->has_tag(VERT, "metric");
  if (have_metric) {
    if (dim == 3) {
      return refine_qualities_tmpl<MetricRefineQualities, 3>(mesh, candidates);
    } else {
      CHECK(dim == 2);
      return refine_qualities_tmpl<MetricRefineQualities, 2>(mesh, candidates);
    }
  } else {
    if (dim == 3) {
      return refine_qualities_tmpl<RealRefineQualities, 3>(mesh, candidates);
    } else {
      CHECK(dim == 2);
      return refine_qualities_tmpl<RealRefineQualities, 2>(mesh, candidates);
    }
  }
}

}  // end namespace Omega_h
