#include "kmermatcher.h"
#include "Indexer.h"
#include "ReducedMatrix.h"
#include "DBWriter.h"
#include "SubstitutionMatrix.h"
#include "Util.h"
#include "Parameters.h"
#include "Matcher.h"
#include "Debug.h"
#include "DBReader.h"
#include "omptl/omptl_algorithm"
#include "MathUtil.h"
#include "FileUtil.h"
#include "NucleotideMatrix.h"
#include "QueryMatcher.h"
#include "FileUtil.h"
#include "Timer.h"
#include "tantan.h"

#include <limits>
#include <string>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <queue>
#include <sys/mman.h>
#ifdef OPENMP
#include <omp.h>
#endif

#ifndef SIZE_T_MAX
#define SIZE_T_MAX ((size_t) -1)
#endif

#define RoL(val, numbits) (val << numbits) ^ (val >> (32 - numbits))
unsigned circ_hash(const int * x, unsigned length, const unsigned rol){
    short unsigned RAND[21] = {0x4567, 0x23c6, 0x9869, 0x4873, 0xdc51, 0x5cff, 0x944a, 0x58ec, 0x1f29, 0x7ccd, 0x58ba, 0xd7ab, 0x41f2, 0x1efb, 0xa9e3, 0xe146, 0x007c, 0x62c2, 0x0854, 0x27f8, 0x231b};
    short unsigned h = 0x0;
    h = h^ RAND[x[0]];                  // XOR h and ki
    for (unsigned int i = 1; i < length; ++i){
        h = RoL(h, rol);
        h ^= RAND[x[i]];                   // XOR h and ki
    }
    return h;
}

// Rolling hash for CRC variant: compute hash value for next key x[0:length-1] from previous hash value hash( x[-1:length-2] ) and x_first = x[-1]
unsigned circ_hash_next(const int * x, unsigned length, int x_first, short unsigned h, const unsigned rol){
    short unsigned RAND[21] = {0x4567, 0x23c6, 0x9869, 0x4873, 0xdc51, 0x5cff, 0x944a, 0x58ec, 0x1f29, 0x7ccd, 0x58ba, 0xd7ab, 0x41f2, 0x1efb, 0xa9e3, 0xe146, 0x007c, 0x62c2, 0x0854, 0x27f8, 0x231b};
    h ^= RoL(RAND[x_first], (5*(length-1)) % 16); // undo INITIAL_VALUE and first letter x[0] of old key
    h =  RoL(h, rol); // circularly permute all letters x[1:length-1] to 5 positions to left
    h ^= RAND[x[length-1]]; // add new, last letter of new key x[1:length]
    return h;
}
#undef RoL

size_t fillKmerPositionArray(KmerPosition * hashSeqPair, DBReader<unsigned int> &seqDbr,
                             Parameters & par, BaseMatrix * subMat,
                             size_t KMER_SIZE, size_t chooseTopKmer,
                             size_t splits, size_t split){
    size_t offset = 0;
    int querySeqType  =  seqDbr.getDbtype();
    ProbabilityMatrix *probMatrix = NULL;
    if (par.maskMode == 1) {
        probMatrix = new ProbabilityMatrix(*subMat);
    }

    struct SequencePosition{
        short score;
        size_t kmer;
        unsigned int pos;
        static bool compareByScore(const SequencePosition &first, const SequencePosition &second){
            if(first.score < second.score)
                return true;
            if(second.score < first.score)
                return false;
            if(first.kmer < second.kmer)
                return true;
            if(second.kmer < first.kmer)
                return false;

            return false;
        }
    };
#pragma omp parallel
    {
        Sequence seq(par.maxSeqLen, querySeqType, subMat, KMER_SIZE, false, false);
        Indexer idxer(subMat->alphabetSize, KMER_SIZE);
        char * charSequence = new char[par.maxSeqLen];
        const unsigned int BUFFER_SIZE = 1024;
        size_t bufferPos = 0;
        KmerPosition * threadKmerBuffer = new KmerPosition[BUFFER_SIZE];
        SequencePosition * kmers = new SequencePosition[par.maxSeqLen+1];
        int highestSeq[32];
        for(size_t i = 0; i<KMER_SIZE;i++){
            highestSeq[i]=subMat->alphabetSize-1;
        }
        size_t highestPossibleIndex = idxer.int2index(highestSeq);
        const size_t flushSize = 100000000;
        size_t iterations = static_cast<size_t>(ceil(static_cast<double>(seqDbr.getSize()) / static_cast<double>(flushSize)));
        for (size_t i = 0; i < iterations; i++) {
            size_t start = (i * flushSize);
            size_t bucketSize = std::min(seqDbr.getSize() - (i * flushSize), flushSize);

#pragma omp for schedule(dynamic, 100)
            for (size_t id = start; id < (start + bucketSize); id++) {
                Debug::printProgress(id);
                seq.mapSequence(id, id, seqDbr.getData(id));
                size_t seqHash = highestPossibleIndex + static_cast<unsigned int>(Util::hash(seq.int_sequence, seq.L));

                // mask using tantan
                if (par.maskMode == 1) {
                    for (int i = 0; i < seq.L; i++) {
                        charSequence[i] = (char) seq.int_sequence[i];
                    }
                    tantan::maskSequences(charSequence,
                                          charSequence + seq.L,
                                          50 /*options.maxCycleLength*/,
                                          probMatrix->probMatrixPointers,
                                          0.005 /*options.repeatProb*/,
                                          0.05 /*options.repeatEndProb*/,
                                          0.5 /*options.repeatOffsetProbDecay*/,
                                          0, 0,
                                          0.9 /*options.minMaskProb*/, probMatrix->hardMaskTable);
                    for (int i = 0; i < seq.L; i++) {
                        seq.int_sequence[i] = charSequence[i];
                    }
                };


                int seqKmerCount = 0;
                unsigned int seqId = seq.getId();
                unsigned short prevHash = 0;
                unsigned int prevFirstRes = 0;
                if (seq.hasNextKmer()) {
                    const int *kmer = seq.nextKmer();
                    prevHash = circ_hash(kmer, KMER_SIZE, par.hashShift);
                    prevFirstRes = kmer[0];
                }
                while (seq.hasNextKmer()) {
                    const int *kmer = seq.nextKmer();
                    //float kmerScore = 1.0;
                    prevHash = circ_hash_next(kmer, KMER_SIZE, prevFirstRes, prevHash, par.hashShift);
                    prevFirstRes = kmer[0];
                    size_t xCount = 0;
                    for (size_t kpos = 0; kpos < KMER_SIZE; kpos++) {
                        xCount += (kmer[kpos] == subMat->aa2int[(int) 'X']);
                    }
                    if (xCount > 0) {
                        continue;
                    }
                    (kmers + seqKmerCount)->score = prevHash;
                    size_t kmerIdx = idxer.int2index(kmer, 0, KMER_SIZE);
                    (kmers + seqKmerCount)->kmer = kmerIdx;
                    (kmers + seqKmerCount)->pos = seq.getCurrentPosition();
                    seqKmerCount++;
                }
                if (seqKmerCount > 1) {
                    std::stable_sort(kmers, kmers + seqKmerCount, SequencePosition::compareByScore);
                }
                size_t kmerConsidered = std::min(static_cast<int>(chooseTopKmer - 1), seqKmerCount);
                if(par.skipNRepeatKmer > 0 ){
                    size_t prevKmer = SIZE_T_MAX;
                    kmers[seqKmerCount].kmer=SIZE_T_MAX;
                    int repeatKmerCnt = 0;
                    for (int topKmer = 0; topKmer < seqKmerCount; topKmer++) {
                        repeatKmerCnt += (
                                (kmers + topKmer)->kmer == (kmers + topKmer + 1)->kmer ||
                                (kmers + topKmer)->kmer == prevKmer);
                        prevKmer = threadKmerBuffer[bufferPos].kmer;
                    }
                    if(repeatKmerCnt >= par.skipNRepeatKmer){
                        kmerConsidered = 0;
                    }
                }

                // add k-mer to represent the identity
                if (seqHash%splits == split) {
                    threadKmerBuffer[bufferPos].kmer = seqHash;
                    threadKmerBuffer[bufferPos].id = seqId;
                    threadKmerBuffer[bufferPos].pos = 0;
                    threadKmerBuffer[bufferPos].seqLen = seq.L;
                    bufferPos++;
                    if (bufferPos >= BUFFER_SIZE) {
                        size_t writeOffset = __sync_fetch_and_add(&offset, bufferPos);
                        memcpy(hashSeqPair + writeOffset, threadKmerBuffer, sizeof(KmerPosition) * bufferPos);
                        bufferPos = 0;
                    }
                }
                for (size_t topKmer = 0; topKmer < kmerConsidered; topKmer++) {
                    size_t splitIdx = (kmers + topKmer)->kmer % splits;
                    if (splitIdx != split) {
                        continue;
                    }

                    threadKmerBuffer[bufferPos].kmer = (kmers + topKmer)->kmer;
                    threadKmerBuffer[bufferPos].id = seqId;
                    threadKmerBuffer[bufferPos].pos = (kmers + topKmer)->pos;
                    threadKmerBuffer[bufferPos].seqLen = seq.L;
                    bufferPos++;
                    if (bufferPos >= BUFFER_SIZE) {
                        size_t writeOffset = __sync_fetch_and_add(&offset, bufferPos);
                        memcpy(hashSeqPair + writeOffset, threadKmerBuffer, sizeof(KmerPosition) * bufferPos);
                        bufferPos = 0;
                    }
                }
            }
#pragma omp barrier
            unsigned int thread_idx = 0;
#ifdef OPENMP
            thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif
            if (thread_idx == 0) {
                seqDbr.remapData();
            }
#pragma omp barrier
        }

        if(bufferPos > 0){
            size_t writeOffset = __sync_fetch_and_add(&offset, bufferPos);
            memcpy(hashSeqPair+writeOffset, threadKmerBuffer, sizeof(KmerPosition) * bufferPos);
        }
        delete [] kmers;
        delete [] charSequence;
        delete [] threadKmerBuffer;
    }

    if (probMatrix != NULL) {
        delete probMatrix;
    }
    return offset;
}

KmerPosition * doComputation(size_t totalKmers, size_t split, size_t splits, std::string splitFile,
                             DBReader<unsigned int> & seqDbr, Parameters & par, BaseMatrix  * subMat,
                             size_t KMER_SIZE, size_t chooseTopKmer) {

    Debug(Debug::INFO) << "Generate k-mers list " << split <<"\n";

    size_t splitKmerCount = (splits > 1) ? static_cast<size_t >(static_cast<double>(totalKmers/splits) * 1.2) : totalKmers;

    KmerPosition * hashSeqPair = new(std::nothrow) KmerPosition[splitKmerCount + 1];
    Util::checkAllocation(hashSeqPair, "Could not allocate memory");
#pragma omp parallel for
    for (size_t i = 0; i < splitKmerCount + 1; i++) {
        hashSeqPair[i].kmer = SIZE_T_MAX;
    }

    Timer timer;
    size_t elementsToSort = fillKmerPositionArray(hashSeqPair, seqDbr, par, subMat, KMER_SIZE, chooseTopKmer, splits, split);
    Debug(Debug::INFO) << "\nTime for fill: " << timer.lap() << "\n";
    if(splits == 1){
        seqDbr.unmapData();
    }
    Debug(Debug::INFO) << "Done." << "\n";
    Debug(Debug::INFO) << "Sort kmer ... ";
    timer.reset();
    omptl::sort(hashSeqPair, hashSeqPair + elementsToSort, KmerPosition::compareRepSequenceAndIdAndPos);
    //kx::radix_sort(hashSeqPair, hashSeqPair + elementsToSort, KmerComparision());
    Debug(Debug::INFO) << "Done." << "\n";
    Debug(Debug::INFO) << "Time for sort: " << timer.lap() << "\n";
    // assign rep. sequence to same kmer members
    // The longest sequence is the first since we sorted by kmer, seq.Len and id
    size_t writePos = 0;
    {
        size_t prevHash = hashSeqPair[0].kmer;
        size_t repSeqId = hashSeqPair[0].id;
        size_t prevHashStart = 0;
        size_t prevSetSize = 0;
        size_t queryLen;
        unsigned int repSeq_i_pos = hashSeqPair[0].pos;
        for (size_t elementIdx = 0; elementIdx < splitKmerCount+1; elementIdx++) {
            if (prevHash != hashSeqPair[elementIdx].kmer) {
                for (size_t i = prevHashStart; i < elementIdx; i++) {
                    size_t rId =  (hashSeqPair[i].kmer != SIZE_T_MAX) ? ((prevSetSize == 1) ? SIZE_T_MAX
                                                                                            : repSeqId) : SIZE_T_MAX;

                    hashSeqPair[i].kmer = SIZE_T_MAX;
                    // remove singletones from set
                    if(rId != SIZE_T_MAX){
                        short diagonal = repSeq_i_pos - hashSeqPair[i].pos;
                        bool canBeExtended = diagonal < 0 || (diagonal > (queryLen - hashSeqPair[i].seqLen));
                        if(par.includeOnlyExtendable == false || (canBeExtended && par.includeOnlyExtendable ==true )){
                            hashSeqPair[writePos].kmer = rId;
                            hashSeqPair[writePos].pos = diagonal;
                            hashSeqPair[writePos].seqLen = hashSeqPair[i].seqLen;
                            hashSeqPair[writePos].id = hashSeqPair[i].id;
                            writePos++;
                        }
                    }
                }
                prevSetSize = 0;
                prevHashStart = elementIdx;
                repSeqId = hashSeqPair[elementIdx].id;
                queryLen = hashSeqPair[elementIdx].seqLen;
                repSeq_i_pos = hashSeqPair[elementIdx].pos;
            }
            if (hashSeqPair[elementIdx].kmer == SIZE_T_MAX) {
                break;
            }
            prevSetSize++;
            prevHash = hashSeqPair[elementIdx].kmer;
        }
    }
    // sort by rep. sequence (stored in kmer) and sequence id
    Debug(Debug::INFO) << "Sort by rep. sequence ... ";
    timer.reset();
    omptl::sort(hashSeqPair, hashSeqPair + writePos, KmerPosition::compareRepSequenceAndIdAndDiag);
    //kx::radix_sort(hashSeqPair, hashSeqPair + elementsToSort, SequenceComparision());
    Debug(Debug::INFO) << "Done\n";
    Debug(Debug::INFO) << "Time for sort: " << timer.lap() << "\n";

    if(splits > 1){
        writeKmersToDisk(splitFile, hashSeqPair, writePos + 1);
        delete [] hashSeqPair;
        hashSeqPair = NULL;
    }
    return hashSeqPair;
}

void setLinearFilterDefault(Parameters *p) {
    p->spacedKmer = false;
    p->covThr = 0.8;
    p->maskMode = 0;
    p->kmerSize = Parameters::CLUST_LINEAR_DEFAULT_K;
    p->alphabetSize = Parameters::CLUST_LINEAR_DEFAULT_ALPH_SIZE;
    p->kmersPerSequence = Parameters::CLUST_LINEAR_KMER_PER_SEQ;
}


size_t computeKmerCount(DBReader<unsigned int> &reader, size_t KMER_SIZE, size_t chooseTopKmer) {
    size_t totalKmers = 0;
    for(size_t id = 0; id < reader.getSize(); id++ ){
        int kmerAdjustedSeqLen = std::max(0, static_cast<int>(reader.getSeqLens(id) - 2 ) - static_cast<int>(KMER_SIZE ) + 1) ;
        totalKmers += std::min(kmerAdjustedSeqLen, static_cast<int>( chooseTopKmer ) );
    }
    return totalKmers;
}

size_t computeMemoryNeededLinearfilter(size_t totalKmer) {
    return sizeof(KmerPosition) * totalKmer;
}


int kmermatcher(int argc, const char **argv, const Command &command) {
    MMseqsMPI::init(argc, argv);

    Parameters &par = Parameters::getInstance();
    setLinearFilterDefault(&par);
    par.parseParameters(argc, argv, command, 2, false, 0, MMseqsParameter::COMMAND_CLUSTLINEAR);

    DBReader<unsigned int> seqDbr(par.db1.c_str(), par.db1Index.c_str());
    seqDbr.open(DBReader<unsigned int>::NOSORT);
    int querySeqType  =  seqDbr.getDbtype();

    setKmerLengthAndAlphabet(par, seqDbr.getAminoAcidDBSize(), querySeqType);
    std::vector<MMseqsParameter>* params = command.params;
    par.printParameters(command.cmd, argc, argv, *params);
    Debug(Debug::INFO) << "Database type: " << seqDbr.getDbTypeName() << "\n";

    BaseMatrix *subMat;
    if (querySeqType == Sequence::NUCLEOTIDES) {
        subMat = new NucleotideMatrix(par.scoringMatrixFile.c_str(), 1.0, 0.0);
    }else {
        if (par.alphabetSize == 21) {
            subMat = new SubstitutionMatrix(par.scoringMatrixFile.c_str(), 2.0, 0.0);
        } else {
            SubstitutionMatrix sMat(par.scoringMatrixFile.c_str(), 2.0, 0.0);
            subMat = new ReducedMatrix(sMat.probMatrix, sMat.subMatrixPseudoCounts, sMat.aa2int, sMat.int2aa, sMat.alphabetSize, par.alphabetSize, 2.0);
        }
    }

    //seqDbr.readMmapedDataInMemory();
    const size_t KMER_SIZE = par.kmerSize;
    size_t chooseTopKmer = par.kmersPerSequence;

    size_t memoryLimit;
    if (par.splitMemoryLimit > 0) {
        memoryLimit = static_cast<size_t>(par.splitMemoryLimit) * 1024;
    } else {
        memoryLimit = static_cast<size_t>(Util::getTotalSystemMemory() * 0.9);
    }
    Debug(Debug::INFO) << "\n";
    size_t totalKmers = computeKmerCount(seqDbr, KMER_SIZE, chooseTopKmer);
    size_t totalSizeNeeded = computeMemoryNeededLinearfilter(totalKmers);
    Debug(Debug::INFO) << "Needed memory (" << totalSizeNeeded << " byte) of total memory (" << memoryLimit << " byte)\n";
    // compute splits
    size_t splits = static_cast<size_t>(std::ceil(static_cast<float>(totalSizeNeeded) / memoryLimit));
    if (splits > 1) {
        // security buffer
        splits += 1;
    }

    Debug(Debug::INFO) << "Process file into " << splits << " parts\n";
    std::vector<std::string> splitFiles;
    KmerPosition *hashSeqPair = NULL;

    size_t mpiRank = 0;
#ifdef HAVE_MPI
    splits = std::max(static_cast<size_t>(MMseqsMPI::numProc), splits);
    size_t fromSplit = 0;
    size_t splitCount = 1;
    mpiRank = MMseqsMPI::rank;
    // if split size is great than nodes than we have to
    // distribute all splits equally over all nodes
    unsigned int * splitCntPerProc = new unsigned int[MMseqsMPI::numProc];
    memset(splitCntPerProc, 0, sizeof(unsigned int) * MMseqsMPI::numProc);
    for(size_t i = 0; i < splits; i++){
        splitCntPerProc[i % MMseqsMPI::numProc] += 1;
    }
    for(int i = 0; i < MMseqsMPI::rank; i++){
        fromSplit += splitCntPerProc[i];
    }
    splitCount = splitCntPerProc[MMseqsMPI::rank];
    delete[] splitCntPerProc;

    for(size_t split = fromSplit; split < fromSplit+splitCount; split++) {
        std::string splitFileName = par.db2 + "_split_" +SSTR(split);
        hashSeqPair = doComputation(totalKmers, split, splits, splitFileName, seqDbr, par, subMat, KMER_SIZE, chooseTopKmer);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if(mpiRank == 0){
        for(size_t split = 0; split < splits; split++) {
            std::string splitFileName = par.db2 + "_split_" +SSTR(split);
            splitFiles.push_back(splitFileName);
        }
    }
#else
    for(size_t split = 0; split < splits; split++) {
        std::string splitFileName = par.db2 + "_split_" +SSTR(split);
        hashSeqPair = doComputation(totalKmers, split, splits, splitFileName, seqDbr, par, subMat, KMER_SIZE, chooseTopKmer);
        splitFiles.push_back(splitFileName);
    }
#endif
    if(mpiRank == 0){
        std::vector<char> repSequence(seqDbr.getSize());
        std::fill(repSequence.begin(), repSequence.end(), false);
        // write result
        DBWriter dbw(par.db2.c_str(), par.db2Index.c_str(), par.threads);
        dbw.open();

        Timer timer;
        if(splits > 1) {
            std::cout << "How many splits: " << splits<<std::endl;
            seqDbr.unmapData();
            mergeKmerFilesAndOutput(seqDbr, dbw, splitFiles, repSequence, par.covMode, par.cov);
        } else {
            writeKmerMatcherResult(seqDbr, dbw, hashSeqPair, totalKmers, repSequence, par.covMode, par.cov, par.threads);
        }
        Debug(Debug::INFO) << "Time for fill: " << timer.lap() << "\n";
        // add missing entries to the result (needed for clustering)

        {
#pragma omp parallel for
            for (size_t id = 0; id < seqDbr.getSize(); id++) {
                char buffer[100];
                int thread_idx = 0;
#ifdef OPENMP
                thread_idx = omp_get_thread_num();
#endif
                if (repSequence[id] == false) {
                    hit_t h;
                    h.pScore = 0;
                    h.diagonal = 0;
                    h.seqId = seqDbr.getDbKey(id);
                    int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
                    dbw.writeData(buffer, len, seqDbr.getDbKey(id), thread_idx);
                }
            }
        }
        dbw.close();

    }
    // free memory
    delete subMat;
    if(hashSeqPair){
        delete [] hashSeqPair;
    }
    seqDbr.close();

    return EXIT_SUCCESS;
}

void writeKmerMatcherResult(DBReader<unsigned int> & seqDbr, DBWriter & dbw,
                            KmerPosition *hashSeqPair, size_t totalKmers,
                            std::vector<char> &repSequence, int covMode, float covThr,
                            size_t threads) {
    std::vector<size_t> threadOffsets;
    size_t splitSize = totalKmers/threads;
    threadOffsets.push_back(0);
    for(size_t thread = 1; thread < threads; thread++){
        unsigned int repSeqId = static_cast<unsigned int>(hashSeqPair[thread*splitSize].kmer);
        for(size_t pos = thread*splitSize; pos < totalKmers; pos++){
            if(repSeqId != hashSeqPair[pos].kmer){
                threadOffsets.push_back(pos);
                break;
            }
        }
    }
    threadOffsets.push_back(totalKmers);
#pragma omp parallel for schedule(dynamic, 1)
    for(size_t thread = 0; thread < threads; thread++){
        std::string prefResultsOutString;
        prefResultsOutString.reserve(100000000);
        char buffer[100];
        size_t lastTargetId = SIZE_T_MAX;
        unsigned int writeSets = 0;
        unsigned int queryLength = 0;
        size_t kmerPos=0;
        size_t repSeqId = SIZE_T_MAX;
        for(kmerPos = threadOffsets[thread]; kmerPos < threadOffsets[thread+1] && hashSeqPair[kmerPos].kmer != SIZE_T_MAX; kmerPos++){
            if(repSeqId != hashSeqPair[kmerPos].kmer) {
                if (writeSets > 0) {
                    repSequence[repSeqId] = true;
                    dbw.writeData(prefResultsOutString.c_str(), prefResultsOutString.length(), seqDbr.getDbKey(repSeqId), thread);
                }else{
                    if(repSeqId != SIZE_T_MAX) {
                        repSequence[repSeqId] = false;
                    }
                }
                lastTargetId = SIZE_T_MAX;
                prefResultsOutString.clear();
                repSeqId = hashSeqPair[kmerPos].kmer;
                queryLength = hashSeqPair[kmerPos].seqLen;
                hit_t h;
                h.seqId = seqDbr.getDbKey(repSeqId);
                h.pScore = 0;
                h.diagonal = 0;
                int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
                // TODO: error handling for len
                prefResultsOutString.append(buffer, len);
            }
            unsigned int targetId = hashSeqPair[kmerPos].id;
            unsigned int targetLength = hashSeqPair[kmerPos].seqLen;
            unsigned short diagonal = hashSeqPair[kmerPos].pos;
            // remove similar double sequence hit
            if(targetId != repSeqId && lastTargetId != targetId ){
                if(Util::canBeCovered(covThr, covMode,
                                      static_cast<float>(queryLength),
                                      static_cast<float>(targetLength)) == false){
                    lastTargetId = targetId;
                    continue;
                }
            }else{
                lastTargetId = targetId;
                continue;
            }
            hit_t h;
            h.seqId = seqDbr.getDbKey(targetId);
            h.pScore = 0;
            h.diagonal = diagonal;
            int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
            prefResultsOutString.append(buffer, len);
            lastTargetId = targetId;
            writeSets++;
        }
        if (writeSets > 0) {
            repSequence[repSeqId] = true;
            dbw.writeData(prefResultsOutString.c_str(), prefResultsOutString.length(), seqDbr.getDbKey(repSeqId), thread);
        }else{
            if(repSeqId != SIZE_T_MAX) {
                repSequence[repSeqId] = false;
            }
        }
    }
}


typedef std::priority_queue<FileKmerPosition, std::vector<FileKmerPosition>, CompareResultBySeqId> KmerPositionQueue;

size_t queueNextEntry(KmerPositionQueue &queue, int file, size_t offsetPos, KmerEntry *entries, size_t entrySize) {
    if(offsetPos + 1 >= entrySize){
        return offsetPos;
    }
    unsigned int repSeqId = entries[offsetPos].seqId;
    size_t pos = 0;
    while(entries[offsetPos + pos].seqId != UINT_MAX){
        queue.push(FileKmerPosition(repSeqId, entries[offsetPos+pos].seqId,  entries[offsetPos+pos].diagonal, file));
        pos++;
    }
    queue.push(FileKmerPosition(repSeqId, UINT_MAX, 0, file));
    pos++;
    return offsetPos+pos;
}

void mergeKmerFilesAndOutput(DBReader<unsigned int> & seqDbr, DBWriter & dbw,
                             std::vector<std::string> tmpFiles, std::vector<char> &repSequence,
                             int covMode, float covThr) {
    Debug(Debug::INFO) << "Merge splits ... ";

    const int fileCnt = tmpFiles.size();
    FILE ** files       = new FILE*[fileCnt];
    KmerEntry **entries = new KmerEntry*[fileCnt];
    size_t * entrySizes = new size_t[fileCnt];
    size_t * offsetPos  = new size_t[fileCnt];
    size_t * dataSizes  = new size_t[fileCnt];
    // init structures
    for(size_t file = 0; file < tmpFiles.size(); file++){
        files[file] = FileUtil::openFileOrDie(tmpFiles[file].c_str(),"r",true);
        size_t dataSize;
        entries[file]    = (KmerEntry*)FileUtil::mmapFile(files[file], &dataSize);
        dataSizes[file]  = dataSize;
        entrySizes[file] = dataSize/sizeof(KmerEntry);
    }
    KmerPositionQueue queue;
    // read one entry for each file
    for(int file = 0; file < fileCnt; file++ ){
        offsetPos[file]=queueNextEntry(queue, file, 0, entries[file], entrySizes[file]);
    }
    std::string prefResultsOutString;
    prefResultsOutString.reserve(100000000);
    char buffer[100];
    FileKmerPosition filePrevsKmerPos;
    filePrevsKmerPos.id = UINT_MAX;
    FileKmerPosition res;
    size_t queryLength;
    if(queue.empty() == false){
        res = queue.top();
        hit_t h;
        h.seqId = seqDbr.getDbKey(res.repSeq);
        h.pScore = 0;
        h.diagonal = 0;
        int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
        prefResultsOutString.append(buffer, len);
        queryLength = seqDbr.getSeqLens(res.repSeq);
    }
    while(queue.empty() == false) {
        res = queue.top();
        queue.pop();
        if(res.id==UINT_MAX){
            offsetPos[res.file] = queueNextEntry(queue, res.file, offsetPos[res.file],
                                                 entries[res.file], entrySizes[res.file]);
            dbw.writeData(prefResultsOutString.c_str(), prefResultsOutString.length(), seqDbr.getDbKey(res.repSeq), 0);
            repSequence[res.repSeq]=true;
            prefResultsOutString.clear();
            // skipe UINT MAX entries
            while(queue.empty() == false && queue.top().id==UINT_MAX){
                res = queue.top();
                queue.pop();
                offsetPos[res.file] = queueNextEntry(queue, res.file, offsetPos[res.file],
                                                     entries[res.file], entrySizes[res.file]);
            }
            if(queue.empty() == false){
                res = queue.top();
                hit_t h;
                h.seqId = seqDbr.getDbKey(res.repSeq);
                h.pScore = 0;
                h.diagonal = 0;
                int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
                prefResultsOutString.append(buffer, len);
                queryLength = seqDbr.getSeqLens(res.repSeq);
            }
        }
        // if its not a duplicate
        if(filePrevsKmerPos.id != res.id && res.repSeq != res.id && res.id!=UINT_MAX){
            unsigned int targetLength = seqDbr.getSeqLens(res.id);
            if(Util::canBeCovered(covThr, covMode,
                                  static_cast<float>(queryLength),
                                  static_cast<float>(targetLength)) == true){
                hit_t h;
                h.seqId = seqDbr.getDbKey(res.id);
                h.pScore = 0;
                h.diagonal = res.pos;
                int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
                prefResultsOutString.append(buffer, len);
            }
        }
        filePrevsKmerPos = res;
    }
    for(size_t file = 0; file < tmpFiles.size(); file++) {
        fclose(files[file]);
        if(munmap((void*)entries[file], dataSizes[file]) < 0){
            Debug(Debug::ERROR) << "Failed to munmap memory dataSize=" << dataSizes[file] <<"\n";
            EXIT(EXIT_FAILURE);
        }
    }
    Debug(Debug::INFO) << "Done\n";

    delete [] dataSizes;
    delete [] offsetPos;
    delete [] entries;
    delete [] entrySizes;
    delete [] files;
}



void writeKmersToDisk(std::string tmpFile, KmerPosition *hashSeqPair, size_t totalKmers) {
    size_t repSeqId = SIZE_T_MAX;
    size_t lastTargetId = SIZE_T_MAX;
    FILE* filePtr = fopen(tmpFile.c_str(), "wb");
    if(filePtr == NULL) { perror(tmpFile.c_str()); EXIT(EXIT_FAILURE); }
    unsigned int writeSets = 0;
    const size_t BUFFER_SIZE = 2048;
    size_t bufferPos = 0;
    size_t elemenetCnt = 0;
    KmerEntry writeBuffer[BUFFER_SIZE];
    KmerEntry nullEntry;
    nullEntry.seqId=UINT_MAX;
    nullEntry.diagonal=0;
    for(size_t kmerPos = 0; kmerPos < totalKmers && hashSeqPair[kmerPos].kmer != SIZE_T_MAX; kmerPos++){
        if(repSeqId != hashSeqPair[kmerPos].kmer) {
            if (writeSets > 0 && elemenetCnt > 0) {
                if(bufferPos > 0){
                    fwrite(writeBuffer, sizeof(KmerEntry), bufferPos, filePtr);
                }
                fwrite(&nullEntry, sizeof(KmerEntry), 1, filePtr);
            }
            lastTargetId = SIZE_T_MAX;
            bufferPos=0;
            elemenetCnt=0;
            repSeqId = hashSeqPair[kmerPos].kmer;
            writeBuffer[bufferPos].seqId = repSeqId;
            writeBuffer[bufferPos].diagonal = 0;
            bufferPos++;
        }
        unsigned int targetId = hashSeqPair[kmerPos].id;
        unsigned short diagonal = hashSeqPair[kmerPos].pos;
        // remove similar double sequence hit
        if(targetId != repSeqId && lastTargetId != targetId ){
            ;
        }else{
            lastTargetId = targetId;
            continue;
        }
        elemenetCnt++;
        writeBuffer[bufferPos].seqId = targetId;
        writeBuffer[bufferPos].diagonal = diagonal;
        bufferPos++;
        if(bufferPos >= BUFFER_SIZE){
            fwrite(writeBuffer, sizeof(KmerEntry), bufferPos, filePtr);
            bufferPos=0;
        }
        lastTargetId = targetId;
        writeSets++;
    }
    if (writeSets > 0 && elemenetCnt > 0 && bufferPos > 0) {
        fwrite(writeBuffer, sizeof(KmerEntry), bufferPos, filePtr);
        fwrite(&nullEntry,  sizeof(KmerEntry), 1, filePtr);
    }
    fclose(filePtr);
}

void setKmerLengthAndAlphabet(Parameters &parameters, size_t aaDbSize, int seqTyp) {
    if(seqTyp == Sequence::NUCLEOTIDES){
        if(parameters.kmerSize == 0) {
            parameters.kmerSize = std::max(15, static_cast<int>(log(static_cast<float>(aaDbSize))/log(4)));
            parameters.alphabetSize = 5;

        }
        if(parameters.kmersPerSequence == 0){
            parameters.kmersPerSequence = 60;
        }
    }else{
        if(parameters.kmerSize == 0){
            if((parameters.seqIdThr+0.001)>=0.9){
                parameters.kmerSize = 14;
                parameters.alphabetSize = 13;
            }else{
                parameters.kmerSize = std::max(10, static_cast<int>(log(static_cast<float>(aaDbSize))/log(8.7)));
                parameters.alphabetSize = 13;
            }
        }
        if(parameters.kmersPerSequence == 0){
            parameters.kmersPerSequence = 20;
        }
    }
}

#undef SIZE_T_MAX