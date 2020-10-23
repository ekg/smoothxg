/** \file smoothxg
 *
 * smooth a graph
 */


#include <omp.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include "args.hxx"
#include "sdsl/bit_vectors.hpp"
#include "chain.hpp"
#include "blocks.hpp"
#include "smooth.hpp"
#include "xg.hpp"
#include "prep.hpp"
#include "cleanup.hpp"
#include "breaks.hpp"
#include "utils.hpp"
#include "odgi/odgi.hpp"
#include "consensus_graph.hpp"

using namespace std;
using namespace xg;

int main(int argc, char** argv) {
    args::ArgumentParser parser("smoothxg: collinear block finder and graph consensus generator");
    args::HelpFlag help(parser, "help", "display this help menu", {'h', "help"});
    args::ValueFlag<std::string> gfa_in(parser, "FILE", "index the graph in this GFA file", {'g', "gfa-in"});
    //args::ValueFlag<std::string> xg_out(parser, "FILE", "write the resulting xg index to this file", {'o', "out"});
    args::ValueFlag<std::string> xg_in(parser, "FILE", "read the xg index from this file", {'i', "in"});

    args::ValueFlag<std::string> write_msa_in_maf_format(parser, "FILE","write the multiple sequence alignments (MSAs) in MAF format in this file",{'m', "write-msa-in-maf-format"});
    args::Flag add_consensus(parser, "bool", "include consensus sequence in the smoothed graph", {'a', "add-consensus"});
    args::ValueFlag<std::string> write_consensus_graph(parser, "FILE", "write the consensus graph in this file", {'s', "write-consensus-graph"});
    args::ValueFlag<uint64_t> _consensus_jump_max(parser, "N", "preserve all divergences from the consensus paths greater than this length [default: 100]", {'C', "consensus-jump-max"});
    args::Flag do_not_merge_blocks(parser, "bool","do not merge contiguous MAF blocks in the MAF output and consensus sequences in the smoothed graph",{'M', "not-merge-blocks"});

    args::ValueFlag<std::string> base(parser, "BASE", "use this basename for temporary files during build", {'b', "base"});
    args::Flag no_prep(parser, "bool", "do not prepare the graph for processing (prep is equivalent to odgi chop followed by odgi sort -p sYgs, and is disabled when taking XG input)", {'n', "no-prep"});
    args::ValueFlag<uint64_t> _max_block_weight(parser, "N", "maximum seed sequence in block [default: 10000]", {'w', "block-weight-max"});
    args::ValueFlag<uint64_t> _max_block_jump(parser, "N", "maximum path jump to include in block [default: 5000]", {'j', "path-jump-max"});
    args::ValueFlag<uint64_t> _min_subpath(parser, "N", "minimum length of a subpath to include in partial order alignment [default: 0 / no filter]", {'k', "subpath-min"});
    args::ValueFlag<uint64_t> _max_edge_jump(parser, "N", "maximum edge jump before breaking [default: 5000]", {'e', "edge-jump-max"});
    args::ValueFlag<double> _min_segment_ratio(parser, "N", "split out segments in a block that are less than this fraction of the length of the longest path range in the block [default: 0.1]", {'R', "min-segment-ratio"});
    args::ValueFlag<uint64_t> _min_copy_length(parser, "N", "minimum repeat length to collapse [default: 1000]", {'c', "copy-length-min"});
    args::ValueFlag<uint64_t> _max_copy_length(parser, "N", "maximum repeat length to attempt to detect [default: 20000]", {'W', "copy-length-max"});
    args::ValueFlag<uint64_t> _max_poa_length(parser, "N", "maximum sequence length to put into poa [default: 10000]", {'l', "poa-length-max"});
    args::ValueFlag<uint64_t> num_threads(parser, "N", "use this many threads during parallel steps", {'t', "threads"});
    args::ValueFlag<std::string> poa_params(parser, "match,mismatch,gap1,ext1(,gap2,ext2)", "score parameters for partial order alignment, if 4 then gaps are affine, if 6 then gaps are convex [default: 2,4,4,2,24,1]", {'p', "poa-params"});
    args::ValueFlag<int> _prep_node_chop(parser, "N", "during prep, chop nodes to this length [default: 100]", {'X', "chop-to"});
    args::ValueFlag<float> _prep_sgd_min_term_updates(parser, "N", "path-guided SGD sort quality parameter (N * sum_path_length updates per iteration) for graph prep [default: 1]", {'U', "path-sgd-term-updates"});
    args::Flag use_spoa(parser, "use-spoa", "run spoa (in local alignment mode) instead of abPOA (in global alignment mode) for smoothing", {'S', "spoa"});
    args::Flag change_alignment_mode(parser, "change-alignment-mode", "change the alignment mode of spoa to global, the local alignment mode of abpoa is currently not supported", {'Z', "change-alignment-mode"});
    args::Flag no_toposort(parser, "no-toposort", "don't apply topological sorting in the sort pipeline", {'T', "no-toposort"});
    args::Flag validate(parser, "validate", "validate construction", {'V', "validate"});
    args::Flag keep_temp(parser, "keep-temp", "keep temporary files", {'K', "keep-temp"});
    args::Flag debug(parser, "debug", "enable debugging", {'d', "debug"});
    try {
        parser.ParseCLI(argc, argv);
    } catch (args::Help) {
        std::cout << parser;
        return 0;
    } catch (args::ParseError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }
    if (argc==1) {
        std::cout << parser;
        return 1;
    }

    if (args::get(do_not_merge_blocks) && (!write_msa_in_maf_format && !args::get(add_consensus))) {
        std::cerr << "[smoothxg::main] error: Please specify -m/--write-msa-in-maf-format and/or -a/--add-consensus "
                     "to use the -M/--not-merge-blocks option." << std::endl;
        return 1;
    }

    if (!args::get(add_consensus) && write_consensus_graph) {
        std::cerr << "[smoothxg::main] error: Please only use the -s/--write-consensus-graph parameter together with"
                   "the -a/--add-consensus option." << std::endl;
        return 1;
    }

    if (_min_subpath && write_consensus_graph) {
        std::cerr << "[smoothxg::main] error: Please only use the -s/--write-consensus-graph parameter without"
                   "the -k/--subpath option." << std::endl;
        return 1;
    }

    if (args::get(keep_temp) && args::get(no_prep)) {
        std::cerr << "[smoothxg::main] error: Please specify -K/--keep-temp or -n/--no-prep, not both." << std::endl;
        return 1;
    }

    size_t n_threads = args::get(num_threads);
    if (n_threads) {
        omp_set_num_threads(args::get(num_threads));
    } else {
        n_threads = 1;
        omp_set_num_threads(1);
    }

    uint64_t max_block_weight = _max_block_weight ? args::get(_max_block_weight) : 10000;
    uint64_t max_block_jump = _max_block_jump ? args::get(_max_block_jump) : 5000;
    uint64_t min_subpath = _min_subpath ? args::get(_min_subpath) : 0;
    uint64_t max_edge_jump = _max_edge_jump ? args::get(_max_edge_jump) : 5000;
    uint64_t min_copy_length = _min_copy_length ? args::get(_min_copy_length) : 1000;
    uint64_t max_copy_length = _max_copy_length ? args::get(_max_copy_length) : 20000;
    uint64_t max_poa_length = _max_poa_length ? args::get(_max_poa_length) : 10000;
    double min_segment_ratio = _min_segment_ratio ? args::get(_min_segment_ratio) : 0.1;

    if (!args::get(use_spoa) && args::get(change_alignment_mode)) {
        std::cerr
                << "[smoothxg::main] error: Currently, the local alignment mode of abpoa is not supported. As default "
                << "abpoa is ran in global mode. You can select spoa in local alignment mode via -S, --spoa. To run spoa in "
                   "global mode, please additionally specify -Z, --change-alignment-mode."
                << std::endl;
        return 1;
    }

    int poa_m = 2;
    int poa_n = 4;
    int poa_g = 4;
    int poa_e = 2;
    int poa_q = 24;
    int poa_c = 1;

    if (!args::get(poa_params).empty()) {
        if (args::get(poa_params).find(',') == std::string::npos) {
            std::cerr << "[smoothxg::main] error: either 4 or 6 POA scoring parameters must be given to -p --poa-params" << std::endl;
            return 1;
        }
        std::vector<std::string> params_str = smoothxg::split(args::get(poa_params),',');
        std::vector<int> params(params_str.size());
        std::transform(params_str.begin(), params_str.end(), params.begin(),
                       [](const std::string& s) { return std::stoi(s); });
        if (params.size() == 6) {
            poa_m = params[0];
            poa_n = params[1];
            poa_g = params[2];
            poa_e = params[3];
            poa_q = params[4];
            poa_c = params[5];
        } else if (params.size() == 4) {
            poa_m = params[0];
            poa_n = params[1];
            poa_g = params[2];
            poa_e = params[3];
            if (args::get(use_spoa)) {
                poa_q = poa_g;
                poa_c = poa_e;
            } else {
                poa_q = 0;
                poa_c = 0;
            }
        } else {
            std::cerr << "[smoothxg::main] error: either 4 or 6 POA scoring parameters must be given to -p --poa-params" << std::endl;
            return 1;
        }

    }
    
    bool order_paths_from_longest = args::get(use_spoa);
    float term_updates = (_prep_sgd_min_term_updates ? args::get(_prep_sgd_min_term_updates) : 1);
    float node_chop = (_prep_node_chop ? args::get(_prep_node_chop) : 100);

    std::cerr << "[smoothxg::main] loading graph" << std::endl;
    XG graph;
    if (!args::get(xg_in).empty()) {
        std::ifstream in(args::get(xg_in));
        graph.deserialize(in);
    } else if (!args::get(gfa_in).empty()) {
        // prep the graph by default
        std::string gfa_in_name;
        if (!args::get(no_prep)) {
            if (args::get(base).empty()){
                gfa_in_name = args::get(gfa_in) + ".prep.gfa";
            }else{
                gfa_in_name = args::get(base) + '/' + args::get(gfa_in) + ".prep.gfa";
            }
            std::cerr << "[smoothxg::main] prepping graph for smoothing" << std::endl;
            smoothxg::prep(args::get(gfa_in), gfa_in_name, node_chop, term_updates, !args::get(no_toposort));
        } else {
            gfa_in_name = args::get(gfa_in);
        }
        std::cerr << "[smoothxg::main] building xg index" << std::endl;
        graph.from_gfa(gfa_in_name, args::get(validate),
                       args::get(base).empty() ? gfa_in_name : args::get(base));
        if (!args::get(keep_temp) && !args::get(no_prep)) {
            std::remove(gfa_in_name.c_str());
        }
    }

    auto blocks = smoothxg::smoothable_blocks(graph,
                                              max_block_weight,
                                              max_block_jump,
                                              min_subpath,
                                              max_edge_jump,
                                              order_paths_from_longest);

    uint64_t min_autocorr_z = 5;
    uint64_t autocorr_stride = 50;
    smoothxg::break_blocks(graph,
                           blocks,
                           max_poa_length,
                           min_copy_length,
                           max_copy_length,
                           min_autocorr_z,
                           autocorr_stride,
                           order_paths_from_longest,
                           true,
                           min_segment_ratio,
                           n_threads,
                           write_consensus_graph);

    bool local_alignment = args::get(use_spoa) ^ args::get(change_alignment_mode);

    std::string maf_header;
    if (write_msa_in_maf_format) {
        basic_string<char> filename;
        if (!args::get(xg_in).empty()) {
            size_t found = args::get(xg_in).find_last_of("/\\");
            filename = (args::get(xg_in).substr(found + 1));
        } else if (!args::get(gfa_in).empty()) {
            size_t found = args::get(gfa_in).find_last_of("/\\");
            filename = (args::get(gfa_in).substr(found + 1));
        }

        maf_header += "##maf version=1\n";
        maf_header += "# smoothxg\n";
        maf_header += "# input=" + filename + " sequences=" + std::to_string(graph.get_path_count()) + "\n";

        // POA
        maf_header += "# POA=";
        maf_header += (args::get(use_spoa) ? "SPOA" : "abPOA");
        maf_header += " alignment_mode=";
        maf_header += (local_alignment ? "local" : "global");
        maf_header += " order_paths=from_";
        maf_header += (order_paths_from_longest ? "longest" : "shortest");
        maf_header += "\n";

        // break_blocks parameters
        maf_header += "# max_block_weight=" + std::to_string(max_block_weight) +
                " max_block_jump=" + std::to_string(max_block_jump) +
                " min_subpath=" + std::to_string(min_subpath) +
                " max_edge_jump=" + std::to_string(max_edge_jump) + "\n";

        // break_blocks
        maf_header += "# max_poa_length=" + std::to_string(max_poa_length) +
                " min_copy_length=" + std::to_string(min_copy_length) +
                " max_copy_length=" + std::to_string(max_copy_length) +
                " min_autocorr_z=" + std::to_string(min_autocorr_z) +
                " autocorr_stride=" + std::to_string(autocorr_stride) + "\n";
    }

    std::vector<path_handle_t> consensus_paths;
    auto smoothed = smoothxg::smooth_and_lace(graph,
                                              blocks,
                                              poa_m,
                                              poa_n,
                                              poa_g,
                                              poa_e,
                                              poa_q,
                                              poa_c,
                                              local_alignment,
                                              args::get(write_msa_in_maf_format), maf_header, !args::get(do_not_merge_blocks),
                                              !args::get(use_spoa),
                                              args::get(add_consensus) ? "Consensus_" : "",
                                              consensus_paths);

    uint64_t consensus_jump_max = _consensus_jump_max ? args::get(_consensus_jump_max) : 100;

    // do we need to build the consensus graph?
    if (write_consensus_graph) {
        odgi::graph_t consensus_graph = smoothxg::create_consensus_graph(smoothed, consensus_paths, consensus_jump_max, n_threads, "consenae");
        smoothxg::cleanup(consensus_graph, term_updates, !args::get(no_toposort));
        ofstream o(args::get(write_consensus_graph));
        consensus_graph.to_gfa(o);
        o.close();
    }

    std::cerr << "[smoothxg::main] sorting smoothed graph" << std::endl;
    smoothxg::cleanup(smoothed, term_updates, !args::get(no_toposort));

    std::cerr << "[smoothxg::main] writing smoothed graph" << std::endl;
    smoothed.to_gfa(std::cout);

    return 0;
}
