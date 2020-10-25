#include "consensus_graph.hpp"

namespace smoothxg {

bool operator<(const link_path_t& a,
               const link_path_t& b) {
    auto& a_0 = as_integer(a.from_cons);
    auto& a_1 = as_integer(a.to_cons);
    auto& b_0 = as_integer(b.from_cons);
    auto& b_1 = as_integer(b.to_cons);
    return (a_0 < b_0) 
        || (a_0 == b_0
            && (a_1 < b_1
                || (a_1 == b_1
                    && (a.length < b.length
                        || (a.length == b.length
                            && a.hash < b.hash)))));
}

ostream& operator<<(ostream& o, const link_path_t& a) {
    o << "("
      << as_integer(a.from_cons) << " "
      << as_integer(a.to_cons) << " "
      << a.length << " "
      << a.hash << " "
      << as_integer(a.path) << " "
      << as_integers(a.begin)[0] << ":" << as_integers(a.begin)[1] << " "
      << as_integers(a.end)[0] << ":" << as_integers(a.end)[1] << ")";
    return o;
}

// prep the graph into a given GFA file
// we'll then build the xg index on top of that in low memory

odgi::graph_t create_consensus_graph(const odgi::graph_t& smoothed,
                                     std::vector<path_handle_t> consensus_paths,
                                     const uint64_t& consensus_jump_max,
                                     const uint64_t& thread_count,
                                     const std::string& base) {

    // walk each path
    // record distance since last step on a consensus path
    // record first step handle off a consensus path
    // detect consensus switches, writing the distance to the last consensus step, step
    // into an array of tuples
    std::cerr << "[smoothxg::create_consensus_graph] building consensus graph" << std::endl;
    
    std::vector<bool> is_consensus(smoothed.get_path_count()+1, false);
    for (auto& path : consensus_paths) {
        is_consensus[as_integer(path)] = true;
    }

    // this preserves the path names, which we'll need to reconstruct the consensus paths
    // in the graph after we unchop it
    std::vector<std::string> consensus_path_names;
    for (auto& path : consensus_paths) {
        consensus_path_names.push_back(smoothed.get_path_name(path));
    }

    std::vector<path_handle_t> non_consensus_paths;
    non_consensus_paths.reserve(smoothed.get_path_count()+1-consensus_paths.size());
    smoothed.for_each_path_handle(
        [&](const path_handle_t& p) {
            if (!is_consensus[as_integer(p)]) {
                non_consensus_paths.push_back(p);
            }
        });

    auto get_path_seq_length =
        [&](const step_handle_t& begin,
            const step_handle_t& end) {
            uint64_t len = 0;
            for (step_handle_t i = begin; i != end; i = smoothed.get_next_step(i)) {
                len += smoothed.get_length(smoothed.get_handle_of_step(i));
            }
            return len;
        };
    
    auto get_path_seq =
        [&](const step_handle_t& begin,
            const step_handle_t& end) {
            std::string seq;
            for (step_handle_t i = begin; i != end; i = smoothed.get_next_step(i)) {
                seq.append(smoothed.get_sequence(smoothed.get_handle_of_step(i)));
            }
            return seq;
        };

    auto hash_seq =
        [&](const std::string& seq) {
            return std::hash<std::string>{}(seq);
        };

    std::vector<uint64_t> node_offset;
    node_offset.push_back(0);
    smoothed.for_each_handle(
        [&](const handle_t& h) {
            node_offset.push_back(node_offset.back()+smoothed.get_length(h));
        });

    auto start_in_vector =
        [&](const handle_t& h) {
            if (!smoothed.get_is_reverse(h)) {
                return (int64_t) node_offset[smoothed.get_id(h)-1];
            } else {
                return (int64_t) (node_offset[smoothed.get_id(h)-1]
                                  + smoothed.get_length(h));
            }
        };

    auto end_in_vector =
        [&](const handle_t& h) {
            if (smoothed.get_is_reverse(h)) {
                return (int64_t) node_offset[smoothed.get_id(h)-1];
            } else {
                return (int64_t) (node_offset[smoothed.get_id(h)-1]
                                  + smoothed.get_length(h));
            }
        };

    // consensus path -> consensus path : link_path_t
    std::string base_mmset = base + ".link_path_ms";
    mmmulti::set<link_path_t> link_path_ms(base_mmset);
    link_path_ms.open_writer();
    
    paryfor::parallel_for<uint64_t>(
        0, non_consensus_paths.size(), thread_count,
        [&](uint64_t idx, int tid) {
            auto& path = non_consensus_paths[idx];
            // for each step in path
            link_path_t link;
            link.path = path;
            path_handle_t last_seen_consensus;
            bool seen_consensus = false;
            smoothed.for_each_step_in_path(
                path,
                [&](const step_handle_t& step) {
                    // check if we're on the step with any consensus
                    handle_t h = smoothed.get_handle_of_step(step);
                    bool on_consensus = false;
                    path_handle_t curr_consensus;
                    smoothed.for_each_step_on_handle(
                        h,
                        [&](const step_handle_t& s) {
                            path_handle_t p = smoothed.get_path_handle_of_step(s);
                            if (is_consensus[as_integer(p)]) {
                                on_consensus = true;
                                curr_consensus = p;
                            }
                        });
                    // if we're on the consensus
                    if (on_consensus) {
                        // we haven't seen any consensus before?
                        if (!seen_consensus) {
                            // we construct the first link path object
                            link.length = 0;
                            link.from_cons = curr_consensus;
                            link.to_cons = curr_consensus;
                            link.begin = step;
                            link.end = step;
                            link.hash = 0;
                            seen_consensus = true;
                            last_seen_consensus = curr_consensus;
                        } else {
                            /*
                            if (last_seen_consensus != consensus) {
                                std::cerr << "path " << smoothed.get_path_name(path) << " switched from " << smoothed.get_path_name(last_seen_consensus) << " to " << smoothed.get_path_name(consensus) << std::endl;
                                last_seen_consensus = consensus;
                            }
                            */

                            // we've seen a consensus before, and it's the same
                            // and the direction of movement is correct
                            // check the distance in the graph position vector
                            // if it's over some threshold, record the link
                            handle_t last_handle = smoothed.get_handle_of_step(link.end);
                            handle_t curr_handle = smoothed.get_handle_of_step(step);
                            uint64_t jump_length = std::abs(start_in_vector(curr_handle)
                                                            - end_in_vector(last_handle));
                            
                            if (link.from_cons == curr_consensus
                                && jump_length < consensus_jump_max) {
                                link.begin = step;
                                link.end = step;
                                link.length = 0;
                            } else { // or it's different
                                // this is when we write a link candidate record
                                link.to_cons = curr_consensus;
                                //link.begin = smoothed.get_next_step(link.begin);
                                link.end = step;
                                //std::cerr << "writing to mmset" << std::endl;
                                link.length = get_path_seq_length(
                                    smoothed.get_next_step(link.begin),
                                    link.end);
                                stringstream h;
                                std::string seq = get_path_seq(smoothed.get_next_step(link.begin),
                                                               link.end);
                                h << smoothed.get_id(smoothed.get_handle_of_step(link.begin))
                                  << ":"
                                  << smoothed.get_id(smoothed.get_handle_of_step(link.end))
                                  << ":"
                                  << seq;
                                link.hash = hash_seq(h.str());
                                if (as_integer(link.from_cons) > as_integer(link.to_cons)) {
                                    std::swap(link.from_cons, link.to_cons);
                                }
                                link.jump_length = jump_length;
                                link_path_ms.append(link);

                                // reset link
                                link.length = 0;
                                link.from_cons = curr_consensus;
                                link.to_cons = curr_consensus;
                                link.begin = step;
                                link.end = step;
                                link.hash = 0;
                                link.is_rev = smoothed.get_is_reverse(smoothed.get_handle_of_step(step));
                            }
                        }
                    } else {
                        //link.length += smoothed.get_length(h);
                    }
                });
        });

    link_path_ms.index(thread_count);

    // collect sets of link paths that refer to the same consensus path pairs
    // and pick which one to keep in the consensus graph

    std::vector<link_path_t> consensus_links;
    std::vector<link_path_t> curr_links;

    path_handle_t curr_from_cons;
    path_handle_t curr_to_cons;

    auto novel_sequence_length =
        [&](const step_handle_t begin,
            const step_handle_t end,
            ska::flat_hash_set<uint64_t> seen_nodes, // by copy
            const odgi::graph_t& graph) {
            uint64_t novel_bp = 0;
            for (auto s = begin;
                 s != end; s = graph.get_next_step(s)) {
                handle_t h = graph.get_handle_of_step(s);
                uint64_t i = graph.get_id(h);
                if (!seen_nodes.count(i)) {
                    novel_bp += graph.get_length(h);
                    seen_nodes.insert(i);
                }
            }
            return novel_bp;
        };

    auto mark_seen_nodes =
        [&](const step_handle_t begin,
            const step_handle_t end,
            ska::flat_hash_set<uint64_t>& seen_nodes, // by ref
            const odgi::graph_t& graph) {
            for (auto s = begin;
                 s != end; s = graph.get_next_step(s)) {
                handle_t h = graph.get_handle_of_step(s);
                uint64_t i = graph.get_id(h);
                if (!seen_nodes.count(i)) {
                    seen_nodes.insert(i);
                }
            }
        };

    std::vector<std::pair<handle_t, handle_t>> perfect_edges;

    auto compute_best_link =
        [&](const std::vector<link_path_t>& links) {
            std::map<uint64_t, uint64_t> hash_counts;
            std::vector<link_path_t> unique_links;
            uint64_t link_rank = 0;
            for (auto& link : links) {
                //std::cerr << link << std::endl;
                auto& c = hash_counts[link.hash];
                if (c == 0) {
                    unique_links.push_back(link);
                }
                ++c;
            }
            std::map<uint64_t, uint64_t> hash_lengths;
            for (auto& link : links) {
                hash_lengths[link.hash] = link.length;
            }
            uint64_t best_count = 0;
            uint64_t best_hash;
            for (auto& c : hash_counts) {
                if (c.second > best_count) {
                    best_hash = c.first;
                    best_count = c.second;
                }
            }
            //std::cerr << "best hash be " << best_hash << std::endl;
            // save the best link path
            link_path_t most_frequent_link;
            for (auto& link : unique_links) {
                if (link.hash == best_hash) {
                    most_frequent_link = link;
                    break;
                }
            }

            // if we have a 0-length link between consensus ends, add it
            handle_t from_end_fwd
                = smoothed.get_handle_of_step(
                    smoothed.path_back(
                        most_frequent_link.from_cons));
            handle_t to_begin_fwd
                = smoothed.get_handle_of_step(
                    smoothed.path_begin(
                        most_frequent_link.to_cons));
            handle_t from_end_rev = smoothed.flip(to_begin_fwd);
            handle_t to_begin_rev = smoothed.flip(from_end_fwd);

            handle_t from_begin_fwd
                = smoothed.get_handle_of_step(
                    smoothed.path_begin(
                        most_frequent_link.from_cons));
            handle_t to_end_fwd
                = smoothed.get_handle_of_step(
                    smoothed.path_back(
                        most_frequent_link.to_cons));
            handle_t from_begin_rev = smoothed.flip(from_begin_fwd);
            handle_t to_end_rev = smoothed.flip(to_end_fwd);

            // if we can walk forward on the last handle of consensus a and reach consensus b
            // we should add a link and say we're perfect

            bool has_perfect_edge = false;
            bool has_perfect_link = false;
            link_path_t perfect_link;

            if (smoothed.has_edge(from_end_fwd, to_begin_fwd)) {
                auto p = std::make_pair(from_end_fwd, to_begin_fwd);
                perfect_edges.push_back(p);
                has_perfect_edge = true;
            } else if (smoothed.has_edge(to_end_fwd, from_begin_fwd)) {
                auto p = std::make_pair(to_end_fwd, from_begin_fwd);
                perfect_edges.push_back(p);
                has_perfect_edge = true;
            } else {
                for (auto& link : unique_links) {
                    //path_handle_t from_cons;
                    //path_handle_t to_cons;
                    // are we just stepping from the end of one to the beginning of the other?
                    for (step_handle_t s = link.begin;
                         s != link.end; s = smoothed.get_next_step(s)) {
                        step_handle_t next = smoothed.get_next_step(s);
                        handle_t b = smoothed.get_handle_of_step(s);
                        handle_t e = smoothed.get_handle_of_step(next);
                        if (b == from_end_fwd && e == to_begin_fwd
                            || b == from_end_rev && e == to_begin_rev
                            || b == to_begin_fwd && e == from_end_fwd
                            || b == to_begin_rev && e == from_end_rev) {
                            has_perfect_link = true;
                            perfect_link = link;
                            break;
                        }
                    }
                    if (has_perfect_link) {
                        break;
                    }
                }
            }
                
            ska::flat_hash_set<uint64_t> seen_nodes;
            if (has_perfect_edge) {
                // nothing to do
            } else if (has_perfect_link) {
                // nb: should be no nodes to mark
                mark_seen_nodes(perfect_link.begin, perfect_link.end, seen_nodes, smoothed);
                perfect_link.rank = link_rank++;
                consensus_links.push_back(perfect_link);
            } else {
                if (most_frequent_link.from_cons != most_frequent_link.to_cons) {
                    most_frequent_link.rank = link_rank++;
                    consensus_links.push_back(most_frequent_link);
                    mark_seen_nodes(most_frequent_link.begin, most_frequent_link.end, seen_nodes, smoothed);
                }
            }

            for (auto& link : unique_links) {
                if (link.hash == best_hash) {
                    continue;
                }
                uint64_t novel_bp = novel_sequence_length(link.begin, link.end, seen_nodes, smoothed);
                if (link.jump_length >= consensus_jump_max
                    || novel_bp >= consensus_jump_max) {
                    link.rank = link_rank++;
                    consensus_links.push_back(link);
                    mark_seen_nodes(link.begin, link.end, seen_nodes, smoothed);
                }
            }
            // todo when adding we need to break the paths up
        };

    std::cerr << "[smoothxg::create_consensus_graph] finding consensus links" << std::endl;
    // collect edges by node
    // 
    link_path_ms.for_each_value(
        [&](const link_path_t& v) {
            //std::cerr << "on " << v << " with count " << c << std::endl;
            if (curr_links.empty()) {
                curr_from_cons = v.from_cons;
                curr_to_cons = v.to_cons;
                curr_links.push_back(v);
            } else if (curr_from_cons != v.from_cons
                       || curr_to_cons != v.to_cons) {
                // compute the best link in the set of curr_links
                compute_best_link(curr_links);
                // reset the links
                curr_links.clear();
                curr_from_cons = v.from_cons;
                curr_to_cons = v.to_cons;
                curr_links.push_back(v);
            } else {
                curr_links.push_back(v);
            }
        });

    compute_best_link(curr_links);

    link_path_ms.close_reader();
    std::remove(base_mmset.c_str());

    // create new consensus graph which only has the consensus and link paths in it
    odgi::graph_t consensus;
    // add the consensus paths first
    for (auto& path : consensus_paths) {
        // create the path
        path_handle_t path_cons_graph = consensus.create_path_handle(smoothed.get_path_name(path));
        handle_t cur_handle_in_cons_graph;
        // add the current node first, then add the step
        smoothed.for_each_step_in_path(path,
                                       [&consensus, &smoothed, &path, &cur_handle_in_cons_graph, &path_cons_graph]
                                       (const step_handle_t& step) {
            handle_t h = smoothed.get_handle_of_step(step);
            handle_t next_handle;
            nid_t node_id = smoothed.get_id(h);
            if (!consensus.has_node(node_id)) {
               cur_handle_in_cons_graph = consensus.create_handle(smoothed.get_sequence(h), node_id);
            } else {
               cur_handle_in_cons_graph = consensus.get_handle(node_id);
            }
            bool rev = smoothed.get_is_reverse(h);
            if (rev) {
                consensus.append_step(path_cons_graph, consensus.flip(cur_handle_in_cons_graph));
            } else {
                consensus.append_step(path_cons_graph, cur_handle_in_cons_graph);
            };
        });
    }

    auto add_path_segment
        = [&](const link_path_t& link,
              const step_handle_t& begin,
              const step_handle_t& end,
              uint64_t& new_rank) {
              // how long was the last path? should we include it?
              stringstream s;
              s << "Link_" << smoothed.get_path_name(link.from_cons) << "_" << smoothed.get_path_name(link.to_cons) << "_" << link.rank << "_" << new_rank++;
              path_handle_t path_cons_graph = consensus.create_path_handle(s.str());
              for (step_handle_t step = begin;
                   step != end;
                   step = smoothed.get_next_step(step)) {
                  handle_t curr_handle = smoothed.get_handle_of_step(step);
                  nid_t node_id = smoothed.get_id(curr_handle);
                  handle_t curr_handle_in_cons_graph = consensus.create_handle(smoothed.get_sequence(smoothed.get_handle(node_id)), node_id);
                  bool rev = smoothed.get_is_reverse(curr_handle);
                  if (rev) {
                      consensus.append_step(path_cons_graph, consensus.flip(curr_handle_in_cons_graph));
                  } else {
                      consensus.append_step(path_cons_graph, curr_handle_in_cons_graph);
                  }
              }
          };

    std::cerr << "[smoothxg::create_consensus_graph] adding consensus paths" << std::endl;
    // add link paths and edges not in the consensus paths
    std::vector<path_handle_t> link_paths;
    for (auto& link : consensus_links) {
        //std::cerr << "making " << "Link_" << smoothed.get_path_name(link.from_cons) << "_" << smoothed.get_path_name(link.to_cons) << "_" << link.rank << std::endl;
        // create link paths and paths
        // make the path name
        if (link.length > 0) {
            auto& novel_link = link;
            stringstream s;
            s << "Link_" << smoothed.get_path_name(novel_link.from_cons) << "_" << smoothed.get_path_name(novel_link.to_cons) << "_" << novel_link.rank;
            path_handle_t path_cons_graph = consensus.create_path_handle(s.str());
            link_paths.push_back(path_cons_graph);
            handle_t cur_handle_in_cons_graph;
            // add the current node first, then add the step
            for (step_handle_t step = novel_link.begin; //smoothed.get_next_step(novel_link.begin);
                 step != novel_link.end;
                 step = smoothed.get_next_step(step)) {
                handle_t h = smoothed.get_handle_of_step(step);
                handle_t next_handle;
                nid_t node_id = smoothed.get_id(h);
                if (!consensus.has_node(node_id)) {
                    //assert(false);
                    // todo should be in by definition...
                    cur_handle_in_cons_graph = consensus.create_handle(smoothed.get_sequence(smoothed.get_handle(node_id)), node_id);
                } else {
                    cur_handle_in_cons_graph = consensus.get_handle(node_id);
                }
                bool rev = smoothed.get_is_reverse(h);
                if (rev) {
                    consensus.append_step(path_cons_graph, consensus.flip(cur_handle_in_cons_graph));
                } else {
                    consensus.append_step(path_cons_graph, cur_handle_in_cons_graph);
                }
            }
        }
    }

    // finally add the edges
    consensus.for_each_path_handle(
        [&](const path_handle_t& path) {
            consensus.for_each_step_in_path(path, [&] (const step_handle_t step) {
               if (consensus.has_next_step(step)) {
                   step_handle_t next_step = consensus.get_next_step(step);
                   handle_t h = consensus.get_handle_of_step(step);
                   handle_t next_h = consensus.get_handle_of_step(next_step);
                   if (!consensus.has_edge(h, next_h)) {
                       consensus.create_edge(h, next_h);
                   }
               }
            });
        });

    for (auto& e : perfect_edges) {
        handle_t h = consensus.get_handle(
            smoothed.get_id(e.first),
            smoothed.get_is_reverse(e.first));
        handle_t j = consensus.get_handle(
            smoothed.get_id(e.second),
            smoothed.get_is_reverse(e.second));
        consensus.create_edge(h, j);
    }

    auto link_steps =
        [&](const step_handle_t& a, const step_handle_t& b) {
            handle_t from = smoothed.get_handle_of_step(a);
            handle_t to = smoothed.get_handle_of_step(b);
            nid_t from_id = smoothed.get_id(from);
            nid_t to_id = smoothed.get_id(to);
            if (consensus.has_node(from_id)
                && consensus.has_node(to_id)) {
                consensus.create_edge(consensus.get_handle(smoothed.get_id(from),
                                                           smoothed.get_is_reverse(from)),
                                      consensus.get_handle(smoothed.get_id(to),
                                                           smoothed.get_is_reverse(to)));
            }
        };

    for (auto& link : consensus_links) {
        // edge from begin to begin+1
        // edge from end-1 to end
        step_handle_t next = smoothed.get_next_step(link.begin);
        link_steps(link.begin, next);
        step_handle_t prev = smoothed.get_previous_step(link.end);
        if (prev != link.begin) {
            link_steps(prev, link.end);
        }
    }
    // validate consensus graph
    // number of handles
    smoothed.for_each_path_handle(
            [&](const path_handle_t &p) {
                if (is_consensus[as_integer(p)]) {
                    std::string path_name = smoothed.get_path_name(p);
                    if (!consensus.has_path(path_name)) {
                        std::cerr << "[smoothxg::main::create_consensus_graph] error: consensus path " << path_name
                        << " not present in the consensus graph!" << std::endl;
                        exit(1);
                    }
                    smoothed.for_each_step_in_path(p,
                                                   [&]
                                                   (const step_handle_t& step) {
                        handle_t h = smoothed.get_handle_of_step(step);
                        nid_t node_id = smoothed.get_id(h);
                        // node id comparison
                        if (!consensus.has_node(node_id)) {
                            std::cerr << "[smoothxg::main::create_consensus_graph] error: node " << node_id
                            << " not present in the consensus graph!" << std::endl;
                            exit(1);
                        }
                        // node orientation comparison
                        handle_t consensus_h = consensus.get_handle(node_id, smoothed.get_is_reverse(h));
                        if (consensus.get_is_reverse(consensus_h) != smoothed.get_is_reverse(h)) {
                            std::cerr << "[smoothxg::main::create_consensus_graph] error: node " << node_id
                            << " orientation in the consensus graph is " << consensus.get_is_reverse(consensus_h)
                            << " but actually should be " << smoothed.get_is_reverse(h)  << std::endl;
                            exit(1);
                        }
                        // sequence comparison
                        if (consensus.get_sequence(consensus_h) != smoothed.get_sequence(h)) {
                            std::cerr << "[smoothxg::main::create_consensus_graph] error: node " << node_id
                            << " sequence in the consensus graph is " << consensus.get_sequence(consensus_h)
                            << " but actually is " << smoothed.get_sequence(h) << std::endl;
                            exit(1);
                        }
                    });
                }
            });

    consensus.for_each_path_handle(
        [&](const path_handle_t& path) {
            consensus.for_each_step_in_path(path, [&] (const step_handle_t step) {
               if (consensus.has_next_step(step)) {
                   step_handle_t next_step = consensus.get_next_step(step);
                   handle_t h = consensus.get_handle_of_step(step);
                   handle_t next_h = consensus.get_handle_of_step(next_step);
                   if (!consensus.has_edge(h, next_h)) {
                       consensus.create_edge(h, next_h);
                   }
               }
            });
        });

    // unchop the graph
    odgi::algorithms::unchop(consensus);

    /*
    ofstream o("pre_reduce.gfa");
    consensus.to_gfa(o);
    o.close();
    */

    ska::flat_hash_set<uint64_t> consensus_paths_set;
    consensus_paths.clear();
    for (auto& name : consensus_path_names) {
        path_handle_t path = consensus.get_path_handle(name);
        consensus_paths.push_back(path);
        consensus_paths_set.insert(as_integer(path));
    }

    std::cerr << "[smoothxg::create_consensus_graph] cleaning up redundant link paths" << std::endl;

    // remove paths that are contained in others
    // and add less than consensus_jump_max bp of sequence
    std::vector<path_handle_t> links_to_remove;
    struct link_range_t {
        nid_t start;
        nid_t end;
        path_handle_t path;
    };
    std::vector<link_range_t> links_by_start_end;
    for (auto& link : link_paths) {
        nid_t a = consensus.get_id(consensus.get_handle_of_step(consensus.path_begin(link)));
        nid_t b = consensus.get_id(consensus.get_handle_of_step(consensus.path_back(link)));
        if (a > b) std::swap(a, b);
        links_by_start_end.push_back({a, b, link});
    }
    // sort them so as to get longer paths first
    std::sort(links_by_start_end.begin(),
              links_by_start_end.end(),
              [](const link_range_t& a,
                 const link_range_t& b) {
                  return a.start < b.end
                                   || a.start == b.end && a.end > b.end;
              });
    std::vector<std::pair<std::string, std::vector<handle_t>>> updated_links;

    auto save_path_fragment =
        [&](const path_handle_t& link,
            uint64_t& save_rank,
            const step_handle_t& first_novel,
            const step_handle_t& step) {
            stringstream s;
            s << consensus.get_path_name(link) << "_" << save_rank++;
            updated_links.push_back(std::make_pair(s.str(), std::vector<handle_t>()));
            auto& handles = updated_links.back().second;
            for (step_handle_t q = first_novel;
                 q != step; q = consensus.get_next_step(q)) {
                handles.push_back(consensus.get_handle_of_step(q));
            }
        };

    auto novelify =
        [&](const std::vector<path_handle_t>& group) {
            ska::flat_hash_set<uint64_t> internal_nodes;
            for (auto& link : group) {
                step_handle_t begin = consensus.path_begin(link);
                step_handle_t end = consensus.path_end(link);
                for (step_handle_t step = begin;
                     step != end;
                     step = consensus.get_next_step(step)) {
                    handle_t h = consensus.get_handle_of_step(step);
                    internal_nodes.insert(consensus.get_id(h));
                }
            }
            ska::flat_hash_set<uint64_t> seen_nodes;
            ska::flat_hash_set<uint64_t> reached_external_nodes;
            for (auto& link : group) {
                links_to_remove.push_back(link);
                step_handle_t begin = consensus.path_begin(link);
                step_handle_t end = consensus.path_end(link);
                bool in_novel = false;
                uint64_t novel_bp = 0;
                step_handle_t first_novel;
                uint64_t save_rank = 0;
                bool reaches_external = false;
                for (step_handle_t step = begin;
                     step != end;
                     step = consensus.get_next_step(step)) {
                    handle_t h = consensus.get_handle_of_step(step);
                    // if we reach an external node that hasn't been reached yet
                    // we should save the handle
                    nid_t id = consensus.get_id(h);
                    if (!seen_nodes.count(id)) {
                        consensus.follow_edges(
                            h, false,
                            [&](const handle_t& o) {
                                nid_t oid = consensus.get_id(o);
                                if (!reached_external_nodes.count(oid)) {
                                    reached_external_nodes.insert(oid);
                                    consensus.for_each_step_on_handle(
                                        o,
                                        [&](const step_handle_t& s) {
                                            uint64_t k = as_integer(
                                                consensus.get_path_handle_of_step(s));
                                            bool is_consensus = consensus_paths_set.count(k);
                                            reaches_external |= is_consensus;
                                        });
                                }
                            });
                        consensus.follow_edges(
                            h, true,
                            [&](const handle_t& o) {
                                nid_t oid = consensus.get_id(o);
                                if (!reached_external_nodes.count(oid)) {
                                    reached_external_nodes.insert(oid);
                                    consensus.for_each_step_on_handle(
                                        o,
                                        [&](const step_handle_t& s) {
                                            uint64_t k = as_integer(
                                                consensus.get_path_handle_of_step(s));
                                            bool is_consensus = consensus_paths_set.count(k);
                                            reaches_external |= is_consensus;
                                        });
                                }
                            });
                        seen_nodes.insert(id);
                        if (in_novel) {
                            novel_bp += consensus.get_length(h);
                        } else {
                            first_novel = step;
                            novel_bp += consensus.get_length(h);
                            in_novel = true;
                        }
                    } else {
                        if (in_novel) {
                            in_novel = false;
                            if (reaches_external || novel_bp >= consensus_jump_max) {
                                save_path_fragment(link, save_rank, first_novel, step);
                            }
                            reaches_external = false;
                            novel_bp = 0;
                        }
                    }
                }
                if (in_novel) {
                    in_novel = false;
                    if (reaches_external || novel_bp >= consensus_jump_max) {
                        save_path_fragment(link, save_rank, first_novel, end);
                    }
                }
            }
        };

    std::vector<path_handle_t> ordered_links;
    for (auto& p : links_by_start_end) {
        ordered_links.push_back(p.path);
    }

    // working through the links
    // progressively collect sequences greater than our consensus_jump_max
    // removing redundant link path components, writing new link paths
    novelify(ordered_links);

    for (auto& link : links_to_remove) {
        consensus.destroy_path(link);
    }

    std::vector<std::string> link_path_names_to_keep;
    for (auto& g : updated_links) {
        auto& name = g.first;
        auto& handles = g.second;
        link_path_names_to_keep.push_back(name);
        path_handle_t path = consensus.create_path_handle(name);
        for (auto& handle : handles) {
            consensus.append_step(path, handle);
        }
    }

    // do it again for all links at once
    /*
    link_paths.clear();
    for (auto& n : link_path_names_to_keep) {
        link_paths.push_back(consensus.get_path_handle(n));
    }
    */

    // now remove coverage=0 nodes
    std::vector<handle_t> empty_handles;
    consensus.for_each_handle(
        [&](const handle_t& handle) {
            uint64_t c = 0;
            consensus.for_each_step_on_handle(
                handle,
                [&](const step_handle_t& s) {
                    ++c;
                });
            if (c == 0) {
                empty_handles.push_back(handle);
            }
        });
    for (auto& handle : empty_handles) {
        consensus.destroy_handle(handle);
    }

    // force edges in paths
    consensus.for_each_path_handle(
        [&](const path_handle_t& path) {
            consensus.for_each_step_in_path(path, [&] (const step_handle_t step) {
               if (consensus.has_next_step(step)) {
                   step_handle_t next_step = consensus.get_next_step(step);
                   handle_t h = consensus.get_handle_of_step(step);
                   handle_t next_h = consensus.get_handle_of_step(next_step);
                   if (!consensus.has_edge(h, next_h)) {
                       consensus.create_edge(h, next_h);
                   }
               }
            });
        });

    odgi::algorithms::unchop(consensus);

    link_paths.clear();
    for (auto& n : link_path_names_to_keep) {
        link_paths.push_back(consensus.get_path_handle(n));
    }

    // now, for each link path
    // chew back each end until its depth is 1

    std::vector<uint64_t> node_coverage(consensus.get_node_count()+1);
    consensus.for_each_handle(
        [&](const handle_t& handle) {
            node_coverage[consensus.get_id(handle)] = consensus.get_step_count(handle);
        });

    std::vector<std::pair<std::string, std::vector<handle_t>>> to_create;
    for (auto& link : link_paths) {
        //while (
        step_handle_t step = consensus.path_begin(link);
        nid_t id = consensus.get_id(consensus.get_handle_of_step(step));
        while (
            step != consensus.path_back(link)
            && node_coverage[id] > 1) {
            --node_coverage[id];
            step = consensus.get_next_step(step);
            id = consensus.get_id(consensus.get_handle_of_step(step));
        }
        step_handle_t begin = step;
        step = consensus.path_back(link);
        id = consensus.get_id(consensus.get_handle_of_step(step));
        while (step != begin
               && node_coverage[id] > 1) {
            --node_coverage[id];
            step = consensus.get_previous_step(step);
            id = consensus.get_id(consensus.get_handle_of_step(step));
        }
        step_handle_t end = consensus.get_next_step(step);
        std::vector<handle_t> new_path;
        for (step = begin; step != end; step = consensus.get_next_step(step)) {
            new_path.push_back(consensus.get_handle_of_step(step));
        }
        id = consensus.get_id(new_path.front());
        if (new_path.size() == 0
            || new_path.size() == 1
            && node_coverage[id] > 1) {
            --node_coverage[id];
            // only destroy the path
        } else {
            std::string name = consensus.get_path_name(link);
            to_create.push_back(std::make_pair(name, new_path));
        }
    }
    for (auto& p : to_create) {
        path_handle_t path = consensus.create_path_handle(p.first); // the trimmed path
        for (auto& handle : p.second) {
            consensus.append_step(path, handle);
        }
    }
    for (auto& link : link_paths) {
        consensus.destroy_path(link);
    }

    odgi::algorithms::unchop(consensus);

    return consensus;
}

}
