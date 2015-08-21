//  ********************************************************************
//  This file is part of KAT - the K-mer Analysis Toolkit.
//
//  KAT is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  KAT is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with KAT.  If not, see <http://www.gnu.org/licenses/>.
//  *******************************************************************

#include <iostream>
#include <string.h>
#include <stdint.h>
#include <vector>
#include <math.h>
#include <memory>
#include <thread>
using std::vector;
using std::string;
using std::cerr;
using std::endl;
using std::stringstream;
using std::shared_ptr;
using std::make_shared;
using std::thread;
using std::ofstream;

#include <seqan/basic.h>
#include <seqan/sequence.h>
#include <seqan/seq_io.h>

#include <boost/algorithm/string.hpp>
#include <boost/exception/all.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/program_options.hpp>
namespace po = boost::program_options;
namespace bfs = boost::filesystem;
using bfs::path;
using boost::lexical_cast;

#include <jellyfish/mer_dna.hpp>
#include <jellyfish_helper.hpp>

#include "inc/matrix/matrix_metadata_extractor.hpp"
#include "inc/matrix/threaded_sparse_matrix.hpp"

#include "sect.hpp"

kat::Sect::Sect(const vector<path> _counts_files, const path _seq_file) {
    input.input = _counts_files;
    input.index = 1;
    seqFile = _seq_file;
    outputPrefix = "kat-sect";
    gcBins = 1001;
    cvgBins = 1001;
    cvgLogscale = false;
    threads = 1;
    merLen = DEFAULT_MER_LEN;
    noCountStats = false;
    median = true;
    verbose = false;
    contamination_mx = nullptr;
}

        
void kat::Sect::execute() {

    if (!bfs::exists(seqFile) && !bfs::symbolic_link_exists(seqFile)) {
        BOOST_THROW_EXCEPTION(SectException() << SectErrorInfo(string(
                "Could not find sequence file at: " + seqFile.string() + "; please check the path and try again.")));
    }

    bucket_size = BATCH_SIZE / threads;
    remaining = BATCH_SIZE % (bucket_size < 1 ? 1 : threads);

    // Validate input
    input.validateInput();
    
    // Create output directory
    path parentDir = bfs::absolute(outputPrefix).parent_path();
    if (!bfs::exists(parentDir) || !bfs::is_directory(parentDir)) {
        if (!bfs::create_directories(parentDir)) {
            BOOST_THROW_EXCEPTION(SectException() << SectErrorInfo(string(
                    "Could not create output directory: ") + parentDir.string()));
        }
    }
    
    // Either count or load input
    if (input.mode == InputHandler::InputHandler::InputMode::COUNT) {
        input.count(merLen, threads);
    }
    else {
        input.loadHeader();
        input.loadHash(true);                
    }

    contamination_mx = make_shared<ThreadedSparseMatrix>(gcBins, cvgBins, threads);

    // Do the core of the work here
    processSeqFile();

    // Dump any hashes that were previously counted to disk if requested
    // NOTE: MUST BE DONE AFTER COMPARISON AS THIS CLEARS ENTRIES FROM HASH ARRAY!
    if (input.dumpHash) {
        path outputPath(outputPrefix.string() + "-hash.jf" + lexical_cast<string>(merLen));
        input.dump(outputPath, threads, true);     
    }
    
    // Merge results from contamination matrix
    merge();
}

void kat::Sect::save() {
    
    auto_cpu_timer timer(1, "  Time taken: %ws\n\n");        

    cout << "Saving results to disk ...";
    cout.flush();
    
    // Send contamination matrix to file
    ofstream contamination_mx_stream(string(outputPrefix.string() + "-contamination.mx").c_str());
    printContaminationMatrix(contamination_mx_stream, seqFile);
    contamination_mx_stream.close();  
    
    cout << " done.";
    cout.flush();
}

void kat::Sect::processSeqFile() {
    
    auto_cpu_timer timer(1, "  Time taken: %ws\n\n");     
    
    cout << "Calculating kmer coverage across sequences ...";
    cout.flush();
    
    // Setup space for storing output
    offset = 0;
    recordsInBatch = 0;

    names = seqan::StringSet<seqan::CharString>();
    seqs = seqan::StringSet<seqan::CharString>();
    
    // Open file, create RecordReader and check all is well
    seqan::SeqFileIn reader(seqFile.c_str());

    // Setup output stream for jellyfish initialisation
    std::ostream* out_stream = verbose ? &cerr : (std::ostream*)0;

    
    // Setup output streams for files
    if (verbose)
        *out_stream << endl;

    // Sequence K-mer counts output stream
    shared_ptr<ofstream> count_path_stream = nullptr;
    if (!noCountStats) {
        count_path_stream = make_shared<ofstream>(string(outputPrefix.string() + "-counts.cvg").c_str());
    }

    // Average sequence coverage and GC% scores output stream
    ofstream cvg_gc_stream(string(outputPrefix.string() + "-stats.csv").c_str());
    cvg_gc_stream << "seq_name\tcoverage\tgc%\tseq_length\tnon_zero_bases\tpercent_covered" << endl;
    
    // Processes sequences in batches of records to reduce memory requirements
    while (!seqan::atEnd(reader)) {
        if (verbose)
            *out_stream << "Loading Batch of sequences... ";

        seqan::clear(names);
        seqan::clear(seqs);

        seqan::readRecords(names, seqs, reader, BATCH_SIZE);

        recordsInBatch = seqan::length(names);

        if (verbose)
            *out_stream << "Loaded " << recordsInBatch << " records.  Processing batch... ";

        // Allocate memory for output produced by this batch
        createBatchVars(recordsInBatch);

        // Process batch with worker threads
        // Process each sequence is processed in a different thread.
        // In each thread lookup each K-mer in the hash
        analyseBatch();

        // Output counts for this batch if (not not) requested
        if (!noCountStats)
            printCounts(*count_path_stream);

        // Output stats
        printStatTable(cvg_gc_stream);

        // Remove any batch specific variables from memory
        destroyBatchVars();

        // Increment batch management vars
        offset += recordsInBatch;

        if (verbose)
            *out_stream << "done" << endl;
    }

    // Close output streams
    if (!noCountStats) {
        count_path_stream->close();                
    }

    seqan::close(reader);

    cvg_gc_stream.close();
    
    cout << " done.";
    cout.flush();
}

void kat::Sect::merge() {
    
    auto_cpu_timer timer(1, "  Time taken: %ws\n\n");     
    
    cout << "Merging matrices ...";
    cout.flush();
    
    contamination_mx->mergeThreadedMatricies();
    cout << " done.";
    cout.flush();
}

void kat::Sect::analyseBatch() {

    thread t[threads];

    for(int i = 0; i < threads; i++) {
        t[i] = thread(&Sect::analyseBatchSlice, this, i);
    }

    for(int i = 0; i < threads; i++){
        t[i].join();
    }            
}

void kat::Sect::analyseBatchSlice(int th_id) {
    // Check to see if we have useful work to do for this thread, return if not
    if (bucket_size < 1 && th_id >= recordsInBatch) {
        return;
    }

    //processInBlocks(th_id);
    processInterlaced(th_id);
}

void kat::Sect::destroyBatchVars() {
    for (uint16_t i = 0; i < counts->size(); i++) {
        counts->at(i)->clear();
    }
    counts->clear();            
    coverages->clear();
    gcs->clear();
    lengths->clear();
    nonZero->clear();
    percentNonZero->clear();
}

void kat::Sect::createBatchVars(uint16_t batchSize) {
    counts = make_shared<vector<shared_ptr<vector<uint64_t>>>>(batchSize);
    coverages = make_shared<vector<double>>(batchSize);
    gcs = make_shared<vector<double>>(batchSize);
    lengths = make_shared<vector<uint32_t>>(batchSize);
    nonZero = make_shared<vector<uint32_t>>(batchSize);
    percentNonZero = make_shared<vector<double>>(batchSize);
}

void kat::Sect::printCounts(std::ostream &out) {
    for (uint32_t i = 0; i < recordsInBatch; i++) {
        out << ">" << seqan::toCString(names[i]) << endl;

        shared_ptr<vector<uint64_t>> seqCounts = counts->at(i);

        if (seqCounts != NULL && !seqCounts->empty()) {
            out << seqCounts->at(0);

            for (size_t j = 1; j < seqCounts->size(); j++) {
                out << " " << seqCounts->at(j);
            }

            out << endl;
        } else {
            out << "0" << endl;
        }
    }
}

void kat::Sect::printStatTable(std::ostream &out) {
    for (uint32_t i = 0; i < recordsInBatch; i++) {
        out     << names[i] << "\t" 
                << (*coverages)[i] << "\t" 
                << (*gcs)[i] << "\t" 
                << (*lengths)[i] << "\t" 
                << (*nonZero)[i] << "\t" 
                << std::fixed << std::setprecision(5) << (*percentNonZero)[i] << endl;
    }
}

// Print K-mer comparison matrix

void kat::Sect::printContaminationMatrix(std::ostream &out, const path seqFile) {
    SM64 mx = contamination_mx->getFinalMatrix();

    out << mme::KEY_TITLE << "Contamination Plot for " << seqFile.string() << " and " << hashFile << endl;
    out << mme::KEY_X_LABEL << "GC%" << endl;
    out << mme::KEY_Y_LABEL << "Average K-mer Coverage" << endl;
    out << mme::KEY_Z_LABEL << "Base Count per bin" << endl;
    out << mme::KEY_NB_COLUMNS << gcBins << endl;
    out << mme::KEY_NB_ROWS << cvgBins << endl;
    out << mme::KEY_MAX_VAL << mx.getMaxVal() << endl;
    out << mme::KEY_TRANSPOSE << "0" << endl;
    out << mme::MX_META_END << endl;

    mx.printMatrix(out);
}

// This method won't be optimal in most cases... Fasta files are normally sorted by length (largest first)
// So first thread will be asked to do more work than the rest

void kat::Sect::processInBlocks(uint16_t th_id) {
    size_t start = bucket_size < 1 ? th_id : th_id * bucket_size;
    size_t end = bucket_size < 1 ? th_id : start + bucket_size - 1;
    for (size_t i = start; i <= end; i++) {
        processSeq(i, th_id);
    }

    // Process a remainder if required
    if (th_id < remaining) {
        size_t rem_idx = (threads * bucket_size) + th_id;
        processSeq(rem_idx, th_id);
    }
}

// This method is probably makes more efficient use of multiple cores on a length sorted fasta file

void kat::Sect::processInterlaced(uint16_t th_id) {
    size_t start = th_id;
    size_t end = recordsInBatch;
    for (size_t i = start; i < end; i += threads) {
        processSeq(i, th_id);
    }
}

bool validKmer(const string& merstr) {
    for(uint32_t i = 0; i < merstr.size(); i++) {        
        switch(merstr[i]) {
            case 'A':
            case 'T':
            case 'G':
            case 'C':
                break;
            default:
                return false;
        }
    }
    
    return true;
}

void kat::Sect::processSeq(const size_t index, const uint16_t th_id) {

    // There's no substring functionality in SeqAn in this version (2.0.0).  So we'll just
    // use regular c++ string's for this bit.  This conversion of strings:
    // {CharString -> c++ string -> substring -> jellyfish mer_dna} is 
    // inefficient. Reducing the number of conversions necessary will 
    // make a big performance improvement here
    stringstream ssSeq;
    ssSeq << seqs[index];
    string seq = ssSeq.str();

    uint64_t seqLength = seq.length();
    uint64_t nbCounts = seqLength - merLen + 1;
    double average_cvg = 0.0;
    uint64_t nbNonZero = 0;
    
    if (seqLength < merLen) {

        cerr << names[index] << ": " << seq << " is too short to compute coverage.  Sequence length is "
                << seqLength << " and K-mer length is " << merLen << ". Setting sequence coverage to 0." << endl;
    } else {

        shared_ptr<vector<uint64_t>> seqCounts = make_shared<vector<uint64_t>>(nbCounts, 0);

        uint64_t sum = 0;

        for (uint64_t i = 0; i < nbCounts; i++) {

            string merstr = seq.substr(i, merLen);

            // Jellyfish compacted hash does not support Ns so if we find one set this mer count to 0
            if (!validKmer(merstr)) {
                (*seqCounts)[i] = 0;
            } else {                
                mer_dna mer(merstr);
                uint64_t count = JellyfishHelper::getCount(input.hash, mer, input.canonical);
                sum += count;
                (*seqCounts)[i] = count;
            }
        }

        (*counts)[index] = seqCounts;
        
        if (seqCounts != 0) nbNonZero++;

        if (median) {

            // Create a copy of the counts, and sort it first, then take median value
            vector<uint64_t> sortedSeqCounts = *seqCounts;                    
            std::sort(sortedSeqCounts.begin(), sortedSeqCounts.end());
            average_cvg = (double)(sortedSeqCounts[sortedSeqCounts.size() / 2]);                    
        }
        else {
            // Calculate the mean
            average_cvg = (double)sum / (double)nbCounts;                    
        }

        (*coverages)[index] = average_cvg;
    }

    // Add length
    (*lengths)[index] = seqLength;
    (*nonZero)[index] = nbNonZero;
    (*percentNonZero)[index] = (double)nbNonZero / (double)seqLength;

    // Calc GC%
    uint64_t gs = 0;
    uint64_t cs = 0;
    uint64_t ns = 0;

    for (uint64_t i = 0; i < seqLength; i++) {
        char c = seq[i];

        if (c == 'G' || c == 'g')
            gs++;
        else if (c == 'C' || c == 'c')
            cs++;
        else if (c == 'N' || c == 'n')
            ns++;
    }

    double gc_perc = ((double) (gs + cs)) / ((double) (seqLength - ns));
    (*gcs)[index] = gc_perc;

    double log_cvg = cvgLogscale ? log10(average_cvg) : average_cvg;

    // Assume log_cvg 5 is max value
    double compressed_cvg = cvgLogscale ? log_cvg * (cvgBins / 5.0) : average_cvg * 0.1;

    uint16_t x = gc_perc * gcBins; // Convert double to 1.dp
    uint16_t y = compressed_cvg >= cvgBins ? cvgBins - 1 : compressed_cvg; // Simply cap the y value

    // Add bases to matrix
    contamination_mx->incTM(th_id, x, y, seqLength);
}

int kat::Sect::main(int argc, char *argv[]) {

    vector<path>    counts_files;
    path            seq_file;
    path            output_prefix;
    uint16_t        gc_bins;
    uint16_t        cvg_bins;
    bool            cvg_logscale;
    uint16_t        threads;
    bool            canonical;
    uint16_t        mer_len;
    uint64_t        hash_size;
    bool            no_count_stats;
    bool            mean;
    bool            dump_hash;
    bool            verbose;
    bool            help;

    // Declare the supported options.
    po::options_description generic_options(Sect::helpMessage(), 100);
    generic_options.add_options()
            ("output_prefix,o", po::value<path>(&output_prefix)->default_value("kat-sect"), 
                "Path prefix for files generated by this program.")
            ("gc_bins,x", po::value<uint16_t>(&gc_bins)->default_value(1001),
                "Number of bins for the gc data when creating the contamination matrix.")
            ("cvg_bins,y", po::value<uint16_t>(&cvg_bins)->default_value(1001),
                "Number of bins for the cvg data when creating the contamination matrix.")
            ("cvg_logscale,l", po::bool_switch(&cvg_logscale)->default_value(false),
                "Compresses cvg scores into logscale for determining the cvg bins within the contamination matrix. Otherwise compresses cvg scores by a factor of 0.1 into the available bins.")
            ("threads,t", po::value<uint16_t>(&threads)->default_value(1),
                "The number of threads to use")
            ("canonical,C", po::bool_switch(&canonical)->default_value(false),
                "IMPORTANT: Whether the jellyfish hashes contains K-mers produced for both strands.  If this is not set to the same value as was produced during jellyfish counting then output will be unpredictable.")
            ("mer_len,m", po::value<uint16_t>(&mer_len)->default_value(DEFAULT_MER_LEN),
                "The kmer length to use in the kmer hashes.  Larger values will provide more discriminating power between kmers but at the expense of additional memory and lower coverage.")
            ("hash_size,H", po::value<uint64_t>(&hash_size)->default_value(DEFAULT_HASH_SIZE),
                "If kmer counting is required for the input, then use this value as the hash size.  If this hash size is not large enough for your dataset then the default behaviour is to double the size of the hash and recount, which will increase runtime and memory usage.")
            ("no_count_stats,n", po::bool_switch(&no_count_stats)->default_value(false),
                "Tells SECT not to output count stats.  Sometimes when using SECT on read files the output can get very large.  When flagged this just outputs summary stats for each sequence.")
            ("mean", po::bool_switch(&mean)->default_value(false),
                "When calculating average sequence coverage, use mean rather than the median kmer frequency.")   
            ("dump_hash,d", po::bool_switch(&dump_hash)->default_value(false), 
                        "Dumps any jellyfish hashes to disk that were produced during this run.") 
            ("verbose,v", po::bool_switch(&verbose)->default_value(false), 
                "Print extra information.")
            ("help", po::bool_switch(&help)->default_value(false), "Produce help message.")
            ;

    // Hidden options, will be allowed both on command line and
    // in config file, but will not be shown to the user.
    po::options_description hidden_options("Hidden options");
    hidden_options.add_options()
            ("seq_file,S", po::value<path>(&seq_file), "Path to the sequnce file to analyse for kmer coverage.")
            ("counts_files,i", po::value<std::vector<path>>(&counts_files), "Path(s) to the input files containing kmer counts.")
            ;

    // Positional option for the input bam file
    po::positional_options_description p;
    p.add("seq_file", 1);
    p.add("counts_files", 100);


    // Combine non-positional options
    po::options_description cmdline_options;
    cmdline_options.add(generic_options).add(hidden_options);

    // Parse command line
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(cmdline_options).positional(p).run(), vm);
    po::notify(vm);

    // Output help information the exit if requested
    if (help || argc <= 1) {
        cout << generic_options << endl;
        return 1;
    }



    auto_cpu_timer timer(1, "KAT SECT completed.\nTotal runtime: %ws\n\n");        

    cout << "Running KAT in SECT mode" << endl
         << "------------------------" << endl << endl;

    // Create the sequence coverage object
    Sect sect(counts_files, seq_file);
    sect.setOutputPrefix(output_prefix);
    sect.setGcBins(gc_bins);
    sect.setCvgBins(cvg_bins);
    sect.setCvgLogscale(cvg_logscale);
    sect.setThreads(threads);
    sect.setCanonical(canonical);
    sect.setMerLen(mer_len);
    sect.setHashSize(hash_size);
    sect.setNoCountStats(no_count_stats);
    sect.setMedian(!mean);
    sect.setDumpHash(dump_hash);
    sect.setVerbose(verbose);

    // Do the work (outputs data to files as it goes)
    sect.execute();

    return 0;
}
