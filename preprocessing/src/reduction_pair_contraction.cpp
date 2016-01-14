// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <lamure/pre/reduction_pair_contraction.h>
#include <lamure/pre/surfel.h>
#include <set>
#include <functional>
#include <queue>
#include <deque>
#include <map>
#include <unordered_map>

namespace lamure {

namespace pre {
struct edge_t
{
  edge_t(surfel_id_t p1, surfel_id_t p2) 
   :a{p1 < p2 ? p1 : p2}
   ,b{p1 < p2 ? p2 : p1}
   {};

  surfel_id_t a;
  surfel_id_t b;
};

bool operator==(const edge_t& e1, const edge_t& e2) {
  if(e1.a == e2.a)   {
    return e1.b == e2.b;
  }
  return false;
}

bool operator<(const edge_t& e1, const edge_t& e2) {
  if(e1.a < e2.a) {
    return true;
  }
  else {
    if(e1.a == e2.a) {
      return e1.b < e2.b;
    }
    else return false;
  }
}

std::ostream& operator<<(std::ostream& os, const edge_t& e) {
  os << "<" << e.a << "--" << e.b << ">";
  return os;
}

struct contraction_op;

struct contraction {
  contraction(edge_t e, mat4r quad, real err, surfel surf)
   :edge{e}
   ,quadric{quad}
   ,error{err}
   ,new_surfel{surf}
  {}

  edge_t edge;
  mat4r quadric;
  real error;
  surfel new_surfel;

  contraction_op* cont_op;
};

bool operator<(const contraction& c1, const contraction& c2) {
  return c1.error < c2.error;
}

struct contraction_op {
  contraction_op(contraction* c)
   :cont{c}
  {};

  contraction* cont;
};

bool operator>(const contraction_op& c1, const contraction_op& c2) {
  // invalid op -> must return false to form strict-weak ordering
  // otherwise undefined behaviour for sort
  if (!c1.cont) {
    return false;
  }
  if(!c2.cont) {
    return true;
  }
  return c1.cont->error > c2.cont->error;
}

}
}

const size_t num_surfels_per_node = 3000;
const size_t num_nodes_per_level = 14348907;
namespace std {
template <> struct hash<lamure::pre::edge_t>
{
  size_t operator()(lamure::pre::edge_t const & e) const noexcept
  {
    uint16_t h1 = e.a.node_idx * num_surfels_per_node + e.a.surfel_idx;  
    uint16_t h2 = e.b.node_idx * num_surfels_per_node + e.b.surfel_idx;  
    size_t hash = h1;
    hash += h2 >> 16;
    return hash;
  }
};
template <> struct hash<lamure::surfel_id_t>
{
  size_t operator()(lamure::surfel_id_t const & s) const noexcept
  {
    return s.node_idx * num_surfels_per_node + s.surfel_idx;  
  }
};
}
namespace lamure {
namespace pre {

bool a = false;
size_t num_neighbours = 20;

surfel_mem_array reduction_pair_contraction::
create_lod(real& reduction_error,
          const std::vector<surfel_mem_array*>& input,
          const uint32_t surfels_per_node,
          const bvh& tree,
          const size_t start_node_id) const
{
  const uint32_t fan_factor = input.size();
  size_t num_surfels = 0;
  size_t min_num_surfels = input[0]->length(); 
  //compute max total number of surfels from all nodes
  for (size_t node_idx = 0; node_idx < fan_factor; ++node_idx) {
    num_surfels += input[node_idx]->length();

    if (input[node_idx]->length() < min_num_surfels) {
      min_num_surfels = input[node_idx]->length();
    }
  }

  std::map<surfel_id_t, mat4r> quadrics{};
  std::vector<std::vector<surfel>> node_surfels{input.size() + 1, std::vector<surfel>{}};
  std::set<edge_t> edges{};

  std::cout << "creating quadrics" << std::endl;
  // accumulate edges and point quadrics
  for (node_id_type node_idx = 0; node_idx < fan_factor; ++node_idx) {
    for (size_t surfel_idx = 0; surfel_idx < input[node_idx]->length(); ++surfel_idx) {
      
      surfel curr_surfel = input[node_idx]->read_surfel(surfel_idx);
      // save surfel
      node_surfels[node_idx].push_back(curr_surfel);
      surfel_id_t curr_id = surfel_id_t{node_idx, surfel_idx};

      assert(node_idx < num_nodes_per_level && surfel_idx < num_surfels_per_node);
      // get and store neighbours
      auto nearest_neighbours = get_local_nearest_neighbours(input, number_of_neighbours_, curr_id);
      std::vector<surfel_id_t> neighbour_ids{};
      for (const auto& pair : nearest_neighbours) {
        neighbour_ids.push_back(pair.first);
      }

      mat4r curr_quadric = mat4r::zero();
      for (auto const& neighbour : nearest_neighbours) {
        edge_t curr_edge = edge_t{curr_id, neighbour.first}; 
        assert(neighbour.first.node_idx < num_nodes_per_level && neighbour.first.surfel_idx <num_surfels_per_node);
        if (edges.find(curr_edge) == edges.end()) {
          edges.insert(curr_edge);
        }
        // accumulate quadric
        surfel neighbour_surfel = input[neighbour.first.node_idx]->read_surfel(neighbour.first.surfel_idx);
        curr_quadric += edge_quadric(curr_surfel.normal(), neighbour_surfel.normal(), curr_surfel.pos(), neighbour_surfel.pos());
      }
      quadrics[curr_id] = curr_quadric;
    }
  }

  // allocate space for new surfels that will be created
  node_surfels.back() = std::vector<surfel>{num_surfels - surfels_per_node};

  std::cout << "creating contractions" << std::endl;
  auto create_contraction = [&node_surfels, &quadrics](const edge_t& edge)->contraction {
    const surfel& surfel1 = node_surfels[edge.a.node_idx][edge.a.surfel_idx];
    const surfel& surfel2 = node_surfels[edge.b.node_idx][edge.b.surfel_idx];
    // new surfel is mean of both old surfels
    surfel new_surfel = surfel{(surfel1.pos() + surfel2.pos()) * 0.5f,
                          (surfel1.color() + surfel2.color()) * 0.5f,
                          (surfel1.radius() + surfel2.radius()) * 0.5f,
                          (normalize(surfel1.normal() + surfel2.normal()))
                          };
                          
    mat4r new_quadric = (quadrics.at(edge.a) + quadrics.at(edge.b));
    real error = quadric_error(new_surfel.pos(), new_quadric);

    return contraction{edge, new_quadric, error, new_surfel};
  };

  // store contractions and operations in queue
  std::unordered_map<surfel_id_t, std::map<surfel_id_t,std::shared_ptr<contraction>>> contractions{};
  std::vector<std::shared_ptr<contraction_op>> contraction_queue{};
  for (const auto& edge : edges) {
    if (contractions[edge.a].find(edge.b) != contractions[edge.a].end()) {
      continue;
    }
    // map contraction to both surfels
    contractions[edge.a][edge.b] = std::make_shared<contraction>(create_contraction(edge));
    contractions[edge.b][edge.a] = contractions.at(edge.a).at(edge.b);
    // check surfel id bounds
    assert(edge.a.node_idx < num_nodes_per_level && edge.a.surfel_idx <num_surfels_per_node);
    assert(edge.b.node_idx < num_nodes_per_level && edge.b.surfel_idx <num_surfels_per_node);
    // store contraction operation pointing to new contraction
    contraction_op op{contractions.at(edge.a).at(edge.b).get()};
    contraction_queue.push_back(std::make_shared<contraction_op>(op));  
    // let contraction point to related contraction_op
    contractions.at(edge.a).at(edge.b)->cont_op = contraction_queue.back().get();  
    //check pointer correctness
    assert(contraction_queue.back().get() == contraction_queue.back()->cont->cont_op);
    assert(contractions.at(edge.a).at(edge.b).get() == contractions.at(edge.a).at(edge.b).get()->cont_op->cont);
    assert(contractions.at(edge.a).at(edge.b).get() == contractions.at(edge.b).at(edge.a).get());
  }

  std::cout << "doing contractions" << std::endl;
  // work off queue until target num of surfels is reached
  for (size_t i = 0; i < num_surfels - surfels_per_node; ++i) {
    // update queue, cheapest contraction at the back
    std::sort(contraction_queue.begin(), contraction_queue.end(), 
      [](const std::shared_ptr<contraction_op>& a, const std::shared_ptr<contraction_op>& b){
         return *a > *b;
       }
      );

    contraction curr_contraction = *(contraction_queue.back()->cont);
    contraction_queue.pop_back();
    surfel_id_t new_id = surfel_id_t{node_surfels.size()-1, i};
    // save new surfels in vector at back
    const surfel& new_surfel = curr_contraction.new_surfel;
    // save new surfel
    node_surfels.back()[i] = new_surfel;
    const surfel_id_t& old_id_1 = curr_contraction.edge.a;
    const surfel_id_t& old_id_2 = curr_contraction.edge.b;
    // invalidate old surfels
    node_surfels[old_id_1.node_idx][old_id_1.surfel_idx].radius() = -1.0f;
    node_surfels[old_id_2.node_idx][old_id_2.surfel_idx].radius() = -1.0f;

    // add new point quadric
    quadrics[new_id] = curr_contraction.quadric;
    // delete old point quadrics
    quadrics.erase(curr_contraction.edge.a);
    quadrics.erase(curr_contraction.edge.b);
   
    auto update_contraction = [&create_contraction, &contractions]
      (const surfel_id_t& new_id, const surfel_id_t& old_id, const std::pair<const surfel_id_t, std::shared_ptr<contraction>>& cont) {
        edge_t new_edge = edge_t{new_id, cont.first};
        // store new contraction
        contractions[new_id][cont.first] = std::make_shared<contraction>(create_contraction(new_edge));
        // update contractions of neighbour
        assert(contractions.find(cont.first) != contractions.end());
        contractions.at(cont.first).erase(old_id);
        contractions.at(cont.first)[new_id] = contractions.at(new_id).at(cont.first);
        // get attached operation
        contraction_op* operation = cont.second->cont_op;
        // transfer contraction op
        contractions.at(new_id).at(cont.first)->cont_op = operation;
        operation->cont = contractions.at(new_id).at(cont.first).get();
        // check for pointer correctness
        assert(contractions.at(new_edge.a).at(new_edge.b).get() == contractions.at(new_edge.a).at(new_edge.b).get()->cont_op->cont);
        assert(contractions.at(new_edge.a).at(new_edge.b).get() == contractions.at(new_edge.b).at(new_edge.a).get());
    };

    for(const auto& cont : contractions.at(old_id_1)) {
      if (cont.first != old_id_2) {
        update_contraction(new_id, old_id_1, cont);
        assert(contractions.at(cont.first).find(old_id_1) == contractions.at(cont.first).end());
      }
      else {
        // invalidate operation
        cont.second->cont_op->cont = nullptr;
      }
    }
    // in case no new contractions were created yet
    if(contractions.find(new_id) == contractions.end()) {
      std::cout << old_id_1 << std::endl;
      contractions[new_id];
      // throw std::exception{};
    }
    for(const auto& cont : contractions.at(old_id_2)) {
      if (cont.first != old_id_1) {
        if(contractions.at(new_id).find(cont.first) == contractions.at(new_id).end()) {
          update_contraction(new_id, old_id_2, cont);
        }
        else {
          // already added -> remove duplicate contractions
          contractions.at(cont.first).erase(old_id_2);
          // // and invalidate respective operation
          cont.second->cont_op->cont = nullptr;
        }
        assert(contractions.at(cont.first).find(old_id_2) == contractions.at(cont.first).end());
      }
      else {
        // invalidate operation
        cont.second->cont_op->cont = nullptr;
      }
    }
    // remove old mapping
    contractions.erase(old_id_1);
    contractions.erase(old_id_2);
    //remove invalid contraction operations
    contraction_queue.erase(std::remove_if(contraction_queue.begin(), contraction_queue.end(), [](const std::shared_ptr<contraction_op>& op){return op->cont == nullptr;}), contraction_queue.end());
  }

  std::cout << "copying surfels" << std::endl;
  // save valid surfels in mem array
  surfel_mem_array mem_array(std::make_shared<surfel_vector>(surfel_vector()), 0, 0);
  for (const auto& node : node_surfels) {
    for (const auto& surfel : node) {
      if (surfel.radius() > 0.0f) {
        mem_array.mem_data()->push_back(surfel);
      }
    }
  }
  mem_array.set_length(mem_array.mem_data()->size());

  reduction_error = 0.0;

  return mem_array;
}


lamure::mat4r edge_quadric(const vec3f& normal_p1, const vec3f& normal_p2, const vec3r& p1, const vec3r& p2)
{
    vec3r edge_dir = normalize(scm::math::length_sqr(p2) > scm::math::length_sqr(p1) ? p2 - p1 : p1 - p2);
    vec3r tangent = normalize(cross(normalize(vec3r(normal_p1 + normal_p2)), edge_dir));

    vec3r normal = cross(tangent, edge_dir);
    normal /= normal.x + normal.y + normal.y;
    vec4r hessian = vec4r{normal, dot(p1, normal)}; 
    lamure::mat4r quadric = mat4r{hessian * hessian.x,
                                  hessian * hessian.y,
                                  hessian * hessian.z,
                                  hessian * hessian.w};

    return quadric;
}

lamure::real quadric_error(const vec3r& p, const mat4r& quadric)
{
  vec4r p_h = vec4r{p, 1.0f};
  vec4r p_transformed = quadric * p_h;
  return dot(p_h, p_transformed);
}

// get k nearest neighbours in simplification nodes
std::vector<std::pair<surfel_id_t, real>> 
get_local_nearest_neighbours(const std::vector<surfel_mem_array*>& input,
                             size_t num_local_neighbours,
                             surfel_id_t const& target_surfel) {

    //size_t current_node = target_surfel.node_idx;
    vec3r center = input[target_surfel.node_idx]->read_surfel(target_surfel.surfel_idx).pos();

    std::vector<std::pair<surfel_id_t, real>> candidates;
    real max_candidate_distance = std::numeric_limits<real>::infinity();

    for (size_t local_node_id = 0; local_node_id < input.size(); ++local_node_id) {
        for (size_t surfel_id = 0; surfel_id < input[local_node_id]->length(); ++surfel_id) {
            if (surfel_id != target_surfel.surfel_idx || local_node_id != target_surfel.node_idx) {
                const surfel& current_surfel = input[local_node_id]->read_surfel(surfel_id);
                real distance_to_center = scm::math::length_sqr(center - current_surfel.pos());

                if (candidates.size() < num_local_neighbours || (distance_to_center < max_candidate_distance)) {
                    if (candidates.size() == num_local_neighbours)
                        candidates.pop_back();

                    candidates.push_back(std::make_pair(surfel_id_t(local_node_id, surfel_id), distance_to_center));

                    for (uint16_t k = candidates.size() - 1; k > 0; --k) {
                        if (candidates[k].second < candidates[k - 1].second) {
                            std::swap(candidates[k], candidates[k - 1]);
                        }
                        else
                            break;
                    }

                    max_candidate_distance = candidates.back().second;
                }
            }
        }
    }

    return candidates;
}

} // namespace pre
} // namespace lamure
