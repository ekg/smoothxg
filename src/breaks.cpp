#include <deps/odgi/src/odgi.hpp>
#include "breaks.hpp"
#include "progress.hpp"
#include "atomic_bitvector.hpp"
#include "smooth.hpp"
#include "rkmh.hpp"

namespace smoothxg {

    using namespace handlegraph;


    void _prepare_and_write_fasta_for_block(const xg::XG &graph,
                                            const block_t &block,
                                            const uint64_t &block_id,
                                            const std::string& prefix,
                                            const std::string& suffix = ""){
        std::vector<std::string> seqs;
        std::vector<std::string> names;
        for (auto &path_range : block.path_ranges) {
            seqs.emplace_back();
            auto &seq = seqs.back();
            for (step_handle_t step = path_range.begin; step != path_range.end;
                 step = graph.get_next_step(step)) {
                seq.append(graph.get_sequence(graph.get_handle_of_step(step)));
            }
            std::stringstream namess;
            namess << graph.get_path_name(
                    graph.get_path_handle_of_step(path_range.begin))
                   << "_" << graph.get_position_of_step(path_range.begin);
            names.push_back(namess.str());
        }

        write_fasta_for_block(graph, block, block_id, seqs, names, prefix, suffix);
    }

// break the path ranges at likely VNTR boundaries
// and break the path ranges to be shorter than our "max" sequence size input to spoa
    void break_blocks(const xg::XG &graph,
                      blockset_t *&blockset,
                      const uint64_t &min_length_mash_based_clustering,
                      const double &block_group_identity,
                      const double &block_group_est_identity,
                      const uint64_t &kmer_size,
                      const uint64_t& min_dedup_depth_for_mash_clustering,
                      const double& short_long_seq_lengths_ratio,
                      const uint64_t &max_poa_length,
                      const uint64_t &min_copy_length,
                      const uint64_t &max_copy_length,
                      const uint64_t &min_autocorr_z,
                      const uint64_t &autocorr_stride,
                      const bool &order_paths_from_longest,
                      const bool &break_repeats,
                      const uint64_t &thread_count,
                      const bool &write_block_to_split_fastas
    ) {
        const VectorizableHandleGraph& vec_graph = dynamic_cast<const VectorizableHandleGraph&>(graph);

        std::cerr
                << "[smoothxg::break_and_split_blocks] cutting blocks that contain sequences longer than max-poa-length ("
                << max_poa_length << ")" << std::endl;
        std::cerr << std::fixed << std::setprecision(3) << "[smoothxg::break_and_split_blocks] splitting "
                  << blockset->size() << " blocks " <<
                  "at identity " << block_group_identity << " (edlib-based clustering) and " <<
                  "at estimated-identity " << block_group_est_identity << " (mash-based clustering)" << std::endl;

        std::stringstream breaks_and_splits_banner;
        breaks_and_splits_banner << "[smoothxg::break_and_split_blocks] cutting and splitting " << blockset->size()
                                 << " blocks:";
        progress_meter::ProgressMeter breaks_and_splits_progress(blockset->size(), breaks_and_splits_banner.str());

        std::atomic<uint64_t> n_cut_blocks;
        n_cut_blocks.store(0);

        std::atomic<uint64_t> n_repeat_blocks;
        n_repeat_blocks.store(0);

        std::atomic<uint64_t> split_blocks;
        split_blocks.store(0);

        atomicbitvector::atomic_bv_t block_is_ready(blockset->size());
        std::vector<std::vector<block_t>> ready_blocks(blockset->size());

        auto *broken_blockset = new smoothxg::blockset_t();

        auto write_ready_blocks_lambda = [&]() {
            uint64_t num_blocks = block_is_ready.size();

            uint64_t old_block_id = 0;
            uint64_t new_block_id = 0;

            while (old_block_id < num_blocks) {
                if (block_is_ready.test(old_block_id)) {
                    for (auto &block : ready_blocks[old_block_id]) {
                        broken_blockset->add_block(new_block_id++, block);

                        block.path_ranges.clear();
                        block.path_ranges.shrink_to_fit();
                        std::vector<path_range_t>().swap(block.path_ranges);
                    }

                    ready_blocks[old_block_id].clear();
                    ready_blocks[old_block_id].shrink_to_fit();
                    std::vector<block_t>().swap(ready_blocks[old_block_id]);

                    ++old_block_id;
                } else {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(1));
                }
            }
        };
        std::thread write_ready_blocks_thread(write_ready_blocks_lambda);

#pragma omp parallel for schedule(static, 1) num_threads(thread_count)
        for (uint64_t block_id = 0; block_id < blockset->size(); ++block_id) {
            auto block = blockset->get_block(block_id);

            // Cutting
            // check if we have sequences that are too long
            bool to_break = false;
            for (auto& path_range : block.path_ranges) {
                if (path_range.length > max_poa_length) {
                    to_break = true;
                    break;
                }
            }
            if (block.path_ranges.size() > 1 && to_break) {
                ++n_cut_blocks;
                uint64_t cut_length = max_poa_length;
                bool found_repeat = false;
                // otherwise let's see if we've got repeats that we can use to chop things up
                // find if there is a repeat
                if (break_repeats) {
                    std::vector<sautocorr::repeat_t> repeats;
                    for (auto& path_range : block.path_ranges) {
                        // steps in id space
                        std::string seq;
                        std::string name = graph.get_path_name(graph.get_path_handle_of_step(path_range.begin));
                        for (step_handle_t step = path_range.begin;
                             step != path_range.end;
                             step = graph.get_next_step(step)) {
                            seq.append(graph.get_sequence(graph.get_handle_of_step(step)));
                        }
                        if (seq.length() >= 2 * min_copy_length) {
                            //std::cerr << "on " << name << "\t" << seq.length() << std::endl;
                            std::vector<uint8_t> vec(seq.begin(), seq.end());
                            sautocorr::repeat_t result = sautocorr::repeat(vec,
                                                                           min_copy_length,
                                                                           max_copy_length,
                                                                           min_copy_length,
                                                                           min_autocorr_z,
                                                                           autocorr_stride);
                            repeats.push_back(result);
                        }

                        seq.clear();
                        seq.shrink_to_fit();
                        std::string().swap(seq);
                    }
                    // if there is, set the cut length to some fraction of it
                    std::vector<double> lengths;
                    double max_z = 0;
                    for (auto& repeat : repeats) {
                        if (repeat.length > 0) {
                            lengths.push_back(repeat.length);
                            max_z = std::max(repeat.z_score, max_z);
                        }
                    }
                    found_repeat = !lengths.empty();
                    if (found_repeat) {
                        double repeat_length = sautocorr::vec_mean(lengths.begin(), lengths.end());
                        cut_length = std::round(repeat_length / 2.0);
                        ++n_repeat_blocks;
                        //std::cerr << "found repeat of " << repeat_length << " and Z-score " << max_z << " cutting to " << cut_length << std::endl;
                    } else {
                        // if not, chop blindly
                        cut_length = max_poa_length;
                    }
                }
                std::vector<path_range_t> chopped_ranges;
                for (auto& path_range : block.path_ranges) {
                    if (!found_repeat && path_range.length < cut_length) {
                        chopped_ranges.push_back(path_range);
                        continue;
                    }
                    // now find outlier clusters based on stdev and mean
                    // extract a minimum viable repeat length
                    // scan across the step vector, looking for where the repeat region begins and ends
                    // cut at the repeat boundaries

                    // Q: should we determine the repeat length for each sequence or all?
                    // each is simple, but maybe expensive
                    // all could provide higher precision, but it's muddier

                    // if this doesn't work, we're going to blindly cut anyway
                    uint64_t last_cut = 0;
                    step_handle_t last_end = path_range.begin;
                    //path_range_t* new_range = nullptr;
                    uint64_t pos = 0;
                    step_handle_t step;
                    for (step = path_range.begin;
                         step != path_range.end;
                         step = graph.get_next_step(step)) {
                        //handle_t h = graph.get_handle_of_step(step);
                        //uint64_t id = graph.get_id(h);
                        //int64_t node_pos = vec_graph.node_vector_offset(id);
                        pos += graph.get_length(graph.get_handle_of_step(step));
                        if (pos - last_cut > cut_length) {
                            step_handle_t next = graph.get_next_step(step);
                            chopped_ranges.push_back({last_end, next, pos - last_cut});
                            last_end = next;
                            last_cut = pos;
                        }
                    }
                    if (step != last_end) {
                        chopped_ranges.push_back({last_end, step, pos - last_cut});
                    }
                }
                block.path_ranges = chopped_ranges;
                // order the path ranges from longest/shortest to shortest/longest
                // this gets called lots of times... probably best to make it std::sort or not parallel
                std::sort(
                        block.path_ranges.begin(), block.path_ranges.end(),
                        order_paths_from_longest
                        ?
                        [](const path_range_t& a,
                           const path_range_t& b) {
                            return a.length > b.length;
                        }
                        :
                        [](const path_range_t& a,
                           const path_range_t& b) {
                            return a.length < b.length;
                        }
                );
                //block.broken = true;
                //block.is_repeat = found_repeat;
            }

            // Splitting
            // ensure that the sequences in the block are within our identity threshold
            // if not, peel them off into splits
            if ((block_group_identity > 0 || block_group_est_identity > 0) && block.path_ranges.size() > 1) {
                bool use_containment_metric = short_long_seq_lengths_ratio > 0;

                std::vector<std::pair<std::uint64_t, std::string>> rank_and_seqs_dedup;
                std::vector<std::vector<uint64_t>> seqs_dedup_original_ranks;

                // Deduplication
                for (uint64_t rank = 0; rank < block.path_ranges.size(); ++rank) {
                    auto& path_range = block.path_ranges[rank];

                    std::string seq;
                    for (step_handle_t step = path_range.begin;
                         step != path_range.end;
                         step = graph.get_next_step(step)) {
                        seq.append(graph.get_sequence(graph.get_handle_of_step(step)));
                    }
                    auto seq_rev = odgi::reverse_complement(seq);

                    bool new_seq = true;
                    for (uint64_t j = 0; j < rank_and_seqs_dedup.size(); ++j) {
                        auto& seqs_dedup = rank_and_seqs_dedup[j].second;

                        if (seq == seqs_dedup || seq_rev == seqs_dedup) {
                            seqs_dedup_original_ranks[j].push_back(rank);
                            new_seq = false;
                            break;
                        }
                    }

                    if (new_seq) {
                        rank_and_seqs_dedup.push_back({rank_and_seqs_dedup.size(), seq});

                        seqs_dedup_original_ranks.emplace_back();
                        seqs_dedup_original_ranks.back().push_back(rank);
                    }

                    std::string().swap(seq);
                    std::string().swap(seq_rev);
                }

                // Sort by length and lexicographically, to have similar sequences close to each other in the order
                std::sort(
                        rank_and_seqs_dedup.begin(), rank_and_seqs_dedup.end(),
                        [](const std::pair<std::uint64_t, std::string>& a,
                           const std::pair<std::uint64_t, std::string>& b) {
                            return std::make_tuple(a.second.size(), std::ref(a.second)) < std::make_tuple(b.second.size(), std::ref(b.second));
                        }
                );

                std::vector<std::string *> seqs_dedup;

                std::vector<std::vector<mkmh::hash_t>> seq_hashes;
                std::vector<int> seq_hash_lens;

                bool mash_based_clustering_enabled = min_length_mash_based_clustering > 0 &&
                        (min_dedup_depth_for_mash_clustering == 0 ||
                        rank_and_seqs_dedup.size() >= min_dedup_depth_for_mash_clustering);

                if (mash_based_clustering_enabled) {
                    seqs_dedup.resize(rank_and_seqs_dedup.size());

                    // Prepare sequence pointers
                    for (uint64_t i = 0; i < rank_and_seqs_dedup.size(); ++i) {
                        if (rank_and_seqs_dedup[i].second.length() >= min_length_mash_based_clustering) {
                            seqs_dedup[i] = &rank_and_seqs_dedup[i].second;
                        }
                    }

                    // Calculate hashes
                    seq_hashes.resize(rank_and_seqs_dedup.size());
                    seq_hash_lens.resize(rank_and_seqs_dedup.size());

                    rkmh::hash_sequences(seqs_dedup, seq_hashes, seq_hash_lens, kmer_size);
                }

                auto start_time = std::chrono::steady_clock::now();

                // iterate through the seqs
                // for each sequence try to match it to a group at the given identity/distance threshold
                // if we can't get it to match, add a new group
                std::vector<std::vector<uint64_t>> groups;

                groups.push_back({0}); // seed with the first sequence
                for (uint64_t i = 1; i < rank_and_seqs_dedup.size(); ++i) {
                    auto& curr_fwd = rank_and_seqs_dedup[i].second;
                    auto curr_rev = odgi::reverse_complement(curr_fwd);

                    uint64_t len_threshold_for_edit_clustering = use_containment_metric ? 0 :
                            ceil(block_group_identity * (double) curr_fwd.length());

                    double one_minus_block_group_id = 1.0 - block_group_identity;

                    // Not for the containment metric
                    uint64_t max_distance_for_edit_clustering = floor(one_minus_block_group_id * (double) curr_fwd.length()) + 1;

                    uint64_t len_threshold_for_mash_clustering = 0;
                    if (mash_based_clustering_enabled && !use_containment_metric) {
                        double value = exp(-one_minus_block_group_id * kmer_size);
                        len_threshold_for_mash_clustering = ceil((double) seq_hashes[i].size() * value / (2.0 - value));
                    }

                    const EdlibAlignMode edlib_align_mode = use_containment_metric ? EDLIB_MODE_HW : EDLIB_MODE_NW;

                    uint64_t best_group = 0;
                    bool cluster_found = false;

                    bool fwd_or_rev = true;
                    for (auto &curr : {curr_fwd, curr_rev}) {
                        // Start looking at from the last group
                        for (int64_t j = groups.size() - 1; j >= 0; --j) {
                            auto &group = groups[j];

                            // Start looking at from the last added sequence to the group
                            for (int64_t k = group.size() - 1; k >= 0; --k) {
                                auto &other = rank_and_seqs_dedup[group[k]].second;

                                if (use_containment_metric &&
                                    ((double) other.length() / (double) curr.length() < short_long_seq_lengths_ratio)) {
                                    // The curr_len/other_len is too low
                                    break;
                                }

                                if (mash_based_clustering_enabled &&
                                    curr.length() >= min_length_mash_based_clustering &&
                                    other.length() >= min_length_mash_based_clustering) {
                                    if (fwd_or_rev) {
                                        if (seq_hashes[group[k]].size() < len_threshold_for_mash_clustering) {
                                            // With a mash-based clustering, the identity would be above the threshold
                                            break;
                                        }

                                        double est_identity = 1 - rkmh::compare(seq_hashes[i], seq_hashes[group[k]], kmer_size, use_containment_metric);
                                        if (est_identity >= block_group_est_identity) {
                                            best_group = j;
                                            cluster_found = true;

                                            break; // Stop with this group
                                        }

                                    } //else: With the mash distance, we already manage the strandness, and here we already tried to align the curr sequence in the other strand
                                } else {
                                    if (other.length() < len_threshold_for_edit_clustering) {
                                        // With an edit-based clustering, the identity would be below the threshold
                                        break;
                                    }

                                    double id = -1;
                                    EdlibAlignResult result = edlibAlign(
                                            curr.c_str(), curr.size(), other.c_str(), other.size(),
                                            edlibNewAlignConfig(
                                                    use_containment_metric ?
                                                    floor(one_minus_block_group_id * (double) other.length()) + 1 :
                                                    max_distance_for_edit_clustering,
                                                    edlib_align_mode,
                                                    EDLIB_TASK_DISTANCE, NULL, 0
                                                    )
                                    );
                                    if (result.status == EDLIB_STATUS_OK && result.editDistance >= 0) {
                                        //curr.size() >= other.size() by design

                                        if (use_containment_metric) {
                                            id = (double) ((int) other.size() - result.editDistance) / (double) (other.size());
                                        } else {
                                            id = (double) ((int) curr.size() - result.editDistance) / (double) (curr.size());
                                        }
                                    }
                                    edlibFreeAlignResult(result);

                                    if (id >= block_group_identity) {
                                        best_group = j;
                                        cluster_found = true;

                                        break; // Stop with this group
                                    }
                                }
                            }

                            if (cluster_found) {
                                break;
                            }
                        }

                        if (cluster_found) {
                            break;
                        }

                        fwd_or_rev = false;
                    }
                    if (cluster_found) {
                        groups[best_group].push_back(i);
                    } else {
                        groups.push_back({i});
                    }
                }

                if (groups.size() == 1) {
                    // nothing to do
                    ready_blocks[block_id].push_back(block);
                } else {
                    ++split_blocks;

                    uint64_t i = 0;
                    for (auto &group : groups) {
                        block_t new_block;
                        //new_block.is_split = true;

                        /*
                        std::cerr << "group " << i << " contains ";
                        for (auto& j : group) std::cerr << " " << j;
                        std::cerr << std::endl;
                        */

                        for (auto &j : group) {
                            // Take the original path_ranges following their original order in the block
                            for (auto &jj : seqs_dedup_original_ranks[rank_and_seqs_dedup[j].first]) {
                                new_block.path_ranges.push_back(block.path_ranges[jj]);
                            }
                        }
                        //for (auto& path_range : new_block.path_ranges) {
                        //    //new_block.total_path_length += path_range.length;
                        //    //new_block.max_path_length = std::max(new_block.max_path_length, path_range.length);
                        //}

                        ready_blocks[block_id].push_back(new_block);

                        if (write_block_to_split_fastas) {
                            _prepare_and_write_fasta_for_block(graph, new_block, block_id, "smoothxg_",
                                                               "_" + std::to_string(i++));
                        }
                    }

                    if (write_block_to_split_fastas) {
                        std::chrono::duration<double> elapsed_time = std::chrono::steady_clock::now() - start_time;

                        // collect sequences
                        _prepare_and_write_fasta_for_block(graph, block, block_id, "smoothxg_",
                                                           "_split_in_" + std::to_string(groups.size()) + "_in_" +
                                                           std::to_string(elapsed_time.count()) + "s");
                    }
                }
            } else {
                // nothing to do
                ready_blocks[block_id].push_back(block);
            }

            block_is_ready.set(block_id);

            breaks_and_splits_progress.increment(1);
        }

        breaks_and_splits_progress.finish();

        std::cerr << "[smoothxg::break_and_split_blocks] cut " << n_cut_blocks << " blocks of which " << n_repeat_blocks
                  << " had repeats" << std::endl;
        std::cerr << "[smoothxg::break_and_split_blocks] split " << split_blocks << " blocks" << std::endl;

        write_ready_blocks_thread.join();

        ready_blocks.clear();
        ready_blocks.shrink_to_fit();
        std::vector<std::vector<block_t>>().swap(ready_blocks);

        delete blockset;
        blockset = broken_blockset;
        blockset->index(thread_count);
    }
}
