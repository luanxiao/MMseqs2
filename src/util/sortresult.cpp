#include "Parameters.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"
#include "Util.h"
#include "Matcher.h"
#include "QueryMatcher.h"

#ifdef OPENMP
#include <omp.h>
#endif

int sortresult(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    par.overrideParameterDescription((Command &) command, par.PARAM_MAX_SEQS.uniqid, "maximum result sequences per query", NULL,
                                     par.PARAM_MAX_SEQS.category & ~MMseqsParameter::COMMAND_EXPERT);
    par.parseParameters(argc, argv, command, 2);

    DBReader<unsigned int> reader(par.db1.c_str(), par.db1Index.c_str());
    reader.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    DBWriter writer(par.db2.c_str(), par.db2Index.c_str(), par.threads);
    writer.open();

   #pragma omp parallel
    {
        int thread_idx = 0;
#ifdef OPENMP
        thread_idx = omp_get_thread_num();
#endif
        char *entry[255];
        char buffer[2048];

        std::vector<Matcher::result_t> alnResults;
        alnResults.reserve(300);

        std::vector<hit_t> prefResults;
        prefResults.reserve(300);

#pragma omp for schedule(dynamic, 5)
        for (size_t i = 0; i < reader.getSize(); ++i) {
            Debug::printProgress(i);

            unsigned int key = reader.getDbKey(i);
            char *data = reader.getData(i);

            int format = -1;
            while (*data != '\0') {
                const size_t columns = Util::getWordsOfLine(data, entry, 255);
                if (columns >= Matcher::ALN_RES_WITH_OUT_BT_COL_CNT) {
                    alnResults.emplace_back(Matcher::parseAlignmentRecord(data, true));
                    format = columns >= Matcher::ALN_RES_WITH_BT_COL_CNT ? 1 : 0;
                } else if (columns == 3) {
                    prefResults.emplace_back(QueryMatcher::parsePrefilterHit(data));
                    format = 2;
                } else {
                    Debug(Debug::ERROR) << "Invalid input result format ("<<columns<<" columns).\n";
                    EXIT(EXIT_FAILURE);
                }
                data = Util::skipLine(data);
            }

            writer.writeStart(thread_idx);
            if (format == 0 || format == 1) {
                std::sort(alnResults.begin(), alnResults.end(), Matcher::compareHits);
                size_t maxEntries = std::min(alnResults.size(), par.maxResListLen);
                for (size_t i = 0; i < maxEntries; ++i) {
                    size_t length = Matcher::resultToBuffer(buffer, alnResults[i], format == 1, false);
                    writer.writeAdd(buffer, length, thread_idx);
                }
            } else if (format == 2) {
                std::sort(prefResults.begin(), prefResults.end(), hit_t::compareHitsByPValueAndId);
                size_t maxEntries = std::min(prefResults.size(), par.maxResListLen);
                for (size_t i = 0; i < maxEntries; ++i) {
                    size_t length = QueryMatcher::prefilterHitToBuffer(buffer, prefResults[i]);
                    writer.writeAdd(buffer, length, thread_idx);
                }
            }
            writer.writeEnd(key, thread_idx);

            alnResults.clear();
            prefResults.clear();
        }
    }

    writer.close();
    reader.close();

    return EXIT_SUCCESS;
}


