/*
   GCTA: a tool for Genome-wide Complex Trait Analysis

   New implementation: read and process genotype of plink format in block way.

   Depends on the class of marker and phenotype

   Developed by Zhili Zheng<zhilizheng@outlook.com>

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   A copy of the GNU General Public License is attached along with this program.
   If not, see <http://www.gnu.org/licenses/>.
*/

#include "Geno.h"
#include "constants.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <chrono>
#include <ctime>
#include <iostream>
#include <iterator>
#include <cmath>
#include <sstream>
#include <iomanip>
#include "utils.hpp"
#include "omp.h"
#include "ThreadPool.h"
#include <cstring>
#include <boost/algorithm/string.hpp>
#include "OptionIO.h"
#include "zlib.h"

#ifdef _WIN64
  #include <intrin.h>
  uint32_t __inline CTZ64U(uint64_t value){
      unsigned long tz = 0;
      _BitScanForward64(&tz, value);
      return tz;
  }
  
  uint32_t __inline CLZ64U(uint64_t value){
      unsigned long lz = 0;
      _BitScanReverse64(&lz, value);
      return 63 - lz;
  }
#else
  //#define CTZU __builtin_ctz
  //#define CLZU __builtin_clz
  #ifdef __linux__
  #pragma message("multiple target")
  __attribute__((target_clones("popcnt","default")))
  #endif
  uint32_t CTZ64U(uint64_t value){
      return __builtin_ctzll(value);
  }
 
#endif

#ifdef __linux__
__attribute__((target("default")))
#endif
uint64_t fill_inter_zero(uint64_t x) {
   uint64_t t;
   t = (x ^ (x >> 16)) & 0x00000000FFFF0000;
   x = x ^ t ^ (t << 16);
   t = (x ^ (x >> 8)) & 0x0000FF000000FF00;
   x = x ^ t ^ (t << 8);
   t = (x ^ (x >> 4)) & 0x00F000F000F000F0;
   x = x ^ t ^ (t << 4);
   t = (x ^ (x >> 2)) & 0x0C0C0C0C0C0C0C0C;
   x = x ^ t ^ (t << 2);
   t = (x ^ (x >> 1)) & 0x2222222222222222;
   x = x ^ t ^ (t << 1);
   return x;
}
#ifdef __linux__
#include <x86intrin.h>
__attribute__((target("bmi2")))
uint64_t fill_inter_zero(uint64_t x) {
    return _pdep_u64(x, 0x5555555555555555U);
}
#endif


typedef uint32_t halfword_t;
const uintptr_t k1LU = (uintptr_t)1;

using std::thread;
using std::to_string;

map<string, string> Geno::options;
map<string, double> Geno::options_d;
vector<string> Geno::processFunctions;

Geno::Geno(Pheno* pheno, Marker* marker) {
    bool has_geno = false;
    if(options.find("geno_file") != options.end()){
        bed_files.push_back(options["geno_file"]);
        has_geno = true;
    }

    if(options.find("m_file") != options.end()){
        bed_files.clear();
        boost::split(bed_files, options["m_file"], boost::is_any_of("\t "));
        std::transform(bed_files.begin(), bed_files.end(), bed_files.begin(), [](string r){return r + ".bed";});
        has_geno = true;
    }
    if(options.find("bgen_file") != options.end()){
        has_geno = true;
    }

    if(!has_geno){
        LOGGER.e(0, "No genotype file specified");
    }


    this->pheno = pheno;
    this->marker = marker;
    num_raw_sample = pheno->count_raw();
    num_byte_per_marker = (num_raw_sample + 3) / 4;
    num_byte_buffer = num_byte_per_marker * Constants::NUM_MARKER_READ;
    last_byte_NA_sample = (4 - (num_raw_sample % 4)) % 4;

    num_keep_sample = pheno->count_keep();
    total_markers = 2 * num_keep_sample;
    num_byte_keep_geno1 = (num_keep_sample + 3) / 4;
    num_item_1geno = (num_keep_sample + 31) / 32;
    num_item_geno_buffer = num_item_1geno * Constants::NUM_MARKER_READ;

    keep_mask = new uint64_t[(num_raw_sample + 63)/64]();
    pheno->getMaskBit(keep_mask);

    isX = false;
    if(options.find("sex") != options.end()){
        isX = true;
        num_male_keep_sample = pheno->count_male();
        total_markers -= pheno->count_male();
        keep_male_mask = new uint64_t[(num_keep_sample + 63)/64]();
        pheno->getMaskBitMale(keep_male_mask);
    }

    if(options.find("bed_file") != options.end()
            || options.find("m_file") != options.end()){
        check_bed();
    }

    string alleleFileName = "";
    if(options.find("update_freq_file") != options.end()){
        alleleFileName = options["update_freq_file"];
    }
    init_AF(alleleFileName);

    init_AsyncBuffer();
    filter_MAF();
}

Geno::~Geno(){
    delete asyncBuffer;
    delete[] keep_mask;
}

void Geno::filter_MAF(){
    if((options_d["min_maf"] != 0.0) || (options_d["max_maf"] != 0.5)){
        LOGGER.i(0, "Computing allele frequencies...");
        vector<function<void (uint64_t *, int)>> callBacks;
        if(isX){ 
            callBacks.push_back(bind(&Geno::freq64_x, this, _1, _2));
        }else{
            callBacks.push_back(bind(&Geno::freq64, this, _1, _2));
        } 
        loop_64block(this->marker->get_extract_index(), callBacks);
        // We adopt the EPSILON from plink, because the double value may have some precision issue;
        double min_maf = options_d["min_maf"] * (1 - Constants::SMALL_EPSILON);
        double max_maf = options_d["max_maf"] * (1 + Constants::SMALL_EPSILON);
        LOGGER.d(0, "min_maf: " + to_string(min_maf) + " max_maf: " + to_string(max_maf));
        vector<uint32_t> extract_index;
        double cur_AF;

        for(int index = 0; index != AFA1.size(); index++){
            cur_AF = AFA1[index];
            if(cur_AF > 0.5) cur_AF = 1.0 - cur_AF;
            if((cur_AF > min_maf) && (cur_AF < max_maf)){
                extract_index.push_back(index);
                LOGGER.d(0, to_string(index) + ": " + to_string(cur_AF));
            }
        }

        vector<double> AFA1o = AFA1;
        //vector<uint32_t> countA1A1o = countA1A1;
        //vector<uint32_t> countA1A2o = countA1A2;
        //vector<uint32_t> countA2A2o = countA2A2;
        vector<uint32_t> countMarkerso = countMarkers;
        //vector<double> RDevo = RDev;

        AFA1.resize(extract_index.size());
        //countA1A1.resize(extract_index.size());
        //countA1A2.resize(extract_index.size());
        //countA2A2.resize(extract_index.size());
        countMarkers.resize(extract_index.size());
        //RDev.resize(extract_index.size());

        #pragma omp parallel for
        for(uint32_t index = 0; index < extract_index.size(); index++){
            uint32_t cur_index = extract_index[index];
            AFA1[index] = AFA1o[cur_index];
            //countA1A1[index] = countA1A1[cur_index];
            //countA1A2[index] = countA1A2[cur_index];
            //countA2A2[index] = countA2A2[cur_index];
            countMarkers[index] = countMarkerso[cur_index];
            //RDev[index] = RDevo[cur_index];
        }

        marker->keep_extracted_index(extract_index);

        init_AsyncBuffer();
        num_blocks = marker->count_extract() / Constants::NUM_MARKER_READ +
                     (marker->count_extract() % Constants::NUM_MARKER_READ != 0);
        LOGGER.i(0, to_string(extract_index.size()) + " SNPs remain from --maf or --max-maf,  ");
    }

}

void Geno::init_AF(string alleleFileName) {
    AFA1.clear();
    //countA1A2.clear();
    //countA1A1.clear();
    //countA2A2.clear();
    countMarkers.clear();
    //RDev.clear();
    if(!alleleFileName.empty()){
        LOGGER.i(0, "Reading frequencies from [" + alleleFileName + "]...");
        vector<int> field_return = {2};
        vector<string> fields;
        vector<bool> a_rev;
        marker->matchSNPListFile(alleleFileName, 3, field_return, fields, a_rev, false);
        AFA1.resize(a_rev.size());
        for(int i = 0; i < a_rev.size(); i++){
            double af;
            try{
                af = stod(fields[i]);
            }catch(std::out_of_range &){
                LOGGER.e(0, "the third columun shall be a number");
            }
            if(af < 0 || af > 1.0){
                LOGGER.e(0, "frequencies value shall range from 0 to 1");
            }
            if(a_rev[i]){
                AFA1[i] = 1.0 - af;
            }else{
                AFA1[i] = af;
            }
        }
        LOGGER.i(0, "Frequencies are updated.");
        num_marker_freq = a_rev.size();
    }
    uint32_t num_marker = marker->count_extract();
    AFA1.resize(num_marker);
    //countA1A1.resize(num_marker);
    //countA1A2.resize(num_marker);
    //countA2A2.resize(num_marker);
    countMarkers.resize(num_marker);
    //RDev = vector<double>(num_marker, 0.0); 
    num_blocks = num_marker / Constants::NUM_MARKER_READ +
                 (num_marker % Constants::NUM_MARKER_READ != 0);
    num_finished_markers = 0; 
    LOGGER.d(0, "The program will run in " + to_string(num_blocks) + " blocks");
}

void Geno::init_AsyncBuffer(){
    if(asyncBuffer){
        delete asyncBuffer;
    }
    asyncBuffer = new AsyncBuffer<uint8_t>(num_byte_buffer);
}


void Geno::out_freq(string filename){
    string name_frq = filename + ".frq";
    LOGGER.i(0, "Saving allele frequencies...");
    std::ofstream o_freq(name_frq.c_str());
    if (!o_freq) { LOGGER.e(0, "can not open the file [" + name_frq + "] to write"); }
    vector<string> out_contents;
    out_contents.reserve(AFA1.size() + 1);
    out_contents.push_back("CHR\tSNP\tPOS\tA1\tA2\tAF\tNCHROBS");
    for(int i = 0; i != AFA1.size(); i++){
        out_contents.push_back(marker->get_marker(marker->getExtractIndex(i)) + "\t" + to_string(AFA1[i])
                               + "\t" + to_string(countMarkers[i]));
    }
    std::copy(out_contents.begin(), out_contents.end(), std::ostream_iterator<string>(o_freq, "\n"));
    o_freq.close();
    LOGGER.i(0, "Allele frequencies of " + to_string(AFA1.size()) + " SNPs have been saved in the file [" + name_frq + "]");
}

bool Geno::check_bed(){
    bool has_error = false;
    FILE *pFile;
    uint64_t f_size;
    uint8_t buffer[3];
    string message;
    uint32_t previous_size  = 0;

    for(int i = 0; i < bed_files.size(); i++){
        string bed_file = bed_files[i];
        uint32_t cur_size =  marker->count_raw(i);

        pFile = fopen(bed_file.c_str(), "rb");
        if(pFile == NULL){
            has_error = true;
            message += "Can't open [" + bed_file + "] to read.\n";
            previous_size = cur_size;
            continue;
        }
        fseek(pFile, 0, SEEK_END);
        f_size = ftell(pFile);
        rewind(pFile);

        if((f_size - 3) != ((uint64_t)num_byte_per_marker) * (cur_size - previous_size)){
            has_error = true;
            message += "Invalid bed file [" + bed_file +
                "]. The sample and SNP number in bed file are different from bim and fam file.\n";
            previous_size = cur_size;
            continue;
        }

        size_t read_count = fread(buffer, 1, 3, pFile);
        fclose(pFile);
        if((read_count != 3) &&
                (*buffer != 0x6c) &&
                (*(buffer+1) != 0x1b) &&
                (*(buffer+2) != 0x01)){
            has_error = true;
            message += "Invalid bed file [" + bed_file +
                "], please convert it into new format (SNP major).\n";
        }
        previous_size = cur_size;
    }

    //delete[] buffer;
    if(has_error){
        LOGGER.e(0, message);
    }else{
        LOGGER.i(0, "BED file(s) check OK.");
    }
    return has_error;
}

void Geno::read_bed(const vector<uint32_t> &raw_marker_index){

    // init start index for each file
    vector<int32_t> pos;
    pos.push_back(-1);
    for(int i = 0; i != bed_files.size() - 1; i++){
        pos.push_back(marker->count_raw(i) - 1);
    }

    //init files handles;
    vector<FILE *> pFiles;
    for(auto & cur_bed_file : bed_files){
        FILE *pFile = fopen(cur_bed_file.c_str(), "rb");
        if(pFile == NULL){
            LOGGER.e(0, "can't open [" + cur_bed_file + "] to read.");
        }

        fseek(pFile, 3, SEEK_SET);
        pFiles.push_back(pFile);
    }

    uint8_t *w_buf = NULL;
    w_buf = asyncBuffer->start_write();
    int num_marker_read = 0;
    for(auto & cur_marker_index : raw_marker_index){
        int cur_file_index = marker->getMIndex(cur_marker_index);
        FILE * pFile = pFiles[cur_file_index];
        int32_t lag_index = cur_marker_index - pos[cur_file_index];
        //very arbitary number to skip
        if(lag_index > 10){
            fseek(pFile, (lag_index - 1) * num_byte_per_marker, SEEK_CUR);
        }else{
            for(int32_t ab_index = 1; ab_index < lag_index; ab_index++){
                fread(w_buf, 1, num_byte_per_marker, pFile);
            }
        }
        size_t read_count = fread(w_buf, 1, num_byte_per_marker, pFile);
        if(read_count != num_byte_per_marker){
            LOGGER.e(0, "read [" + bed_files[cur_file_index] + "] error.\nThere might be some problems in your storage, or have you changed the file?");
        }
        w_buf += num_byte_per_marker;
        pos[cur_file_index] = cur_marker_index;

        num_marker_read += 1;
        if(num_marker_read == Constants::NUM_MARKER_READ){
            asyncBuffer->end_write();
            w_buf = asyncBuffer->start_write();
            num_marker_read = 0;
        }
    }

    asyncBuffer->end_write();

    for(auto & pFile : pFiles){
        fclose(pFile);
    }

/*
    for(int i = 0; i < bed_files.size(); i++){
        cur_bed_file = bed_files[i];

        pFile = fopen(cur_bed_file.c_str(), "rb");
        if(pFile == NULL){
            LOGGER.e(0, "can't open [" + cur_bed_file + "] to read.");
        }

        fseek(pFile, 3, SEEK_SET);

        bool isEOF = false;
        uint64_t index_marker_extracted = 0;
        uint64_t last_index = marker->getExtractIndex(index_marker_extracted);
        fseek(pFile, num_byte_per_marker * last_index, SEEK_CUR);

        int cur_block = 0;
        int last_num_marker = marker->count_extract() % Constants::NUM_MARKER_READ;

        LOGGER.ts("read_geno");

        while(!isEOF){
            w_buf = asyncBuffer->start_write();

            LOGGER.d(0, "read start");
            int num_marker = 0;
            size_t read_count = 0;
            iw_buf = w_buf;
            //begin = clock();
            int cur_num_block = (cur_block != (num_blocks - 1))? Constants::NUM_MARKER_READ : last_num_marker;
            do{
                uint64_t  cur_index = marker->getExtractIndex(index_marker_extracted);
                uint64_t lag_index = cur_index - last_index;

                //very arbitrary value to skip, maybe precised in the future.
                if(lag_index > 10){
                    fseek(pFile, (lag_index - 1) * num_byte_per_marker, SEEK_CUR);
                }else{
                    for(int64_t ab_index = 1; ab_index < lag_index; ab_index++){
                        fread(w_buf, 1, num_byte_per_marker, pFile);
                    }
                }
                read_count = fread(w_buf, 1, num_byte_per_marker, pFile);
                w_buf += num_byte_per_marker;

                last_index = cur_index;
                index_marker_extracted++;
                num_marker++;

                if(read_count != num_byte_per_marker || index_marker_extracted == marker->count_extract()){
                    asyncBuffer->setEOF();
                    isEOF = true;
                    break;
                }
            }while(num_marker != cur_num_block);

            asyncBuffer->end_write();
            LOGGER.d(2, "read block success");
            //std::cout << "   read bed" << "@T: " << 1.0 * (clock() - begin) / CLOCKS_PER_SEC << std::endl;
            cur_block++;
        }
        fclose(pFile);
        */
    //LOGGER.i(0, "Read bed time: " + to_string(LOGGER.tp("read_geno")));
}

/*
void Geno::freq(uint8_t *buf, int num_marker){
    if(num_marker_freq >= marker->count_extract()) return;
    for(int cur_marker_index = 0; cur_marker_index < num_marker; ++cur_marker_index){
        uint32_t curA1A1 = 0, curA1A2 = 0, curA2A2 = 0;
        uint64_t *pbuf = (uint64_t *) (buf + cur_marker_index * num_byte_per_marker);
        for(auto &index : pheno->keep_block_index){
            uint64_t geno_temp = *(pbuf + index);
            if(pheno->mask_add_items[index]){
                geno_temp = (geno_temp & pheno->mask_items[index]) + pheno->mask_add_items[index];
            }
            vector<uint16_t> genos = {(uint16_t)(geno_temp), (uint16_t)(geno_temp >> 16), 
                                       (uint16_t)(geno_temp >> 32), (uint16_t)(geno_temp >> 48)};
            g_table.set_count(genos, curA1A1, curA1A2, curA2A2); 

            int raw_index_marker = num_marker_freq + cur_marker_index;
            countA1A1[raw_index_marker] = curA1A1;
            countA1A2[raw_index_marker] = curA1A2;
            countA2A2[raw_index_marker] = curA2A2;
            AFA1[raw_index_marker] = (2.0 * curA1A1 + curA1A2) / (2.0 * (curA1A1 + curA1A2 + curA2A2));
        }
    }
    num_marker_freq += num_marker;
}
*/

/*
void Geno::freq(uint8_t *buf, int num_marker) {
    if(num_marker_freq >= marker->count_extract()) return;
    //pheno->mask_geno_keep(buf, num_marker);
    int cur_num_marker_read = num_marker;
    static bool isLastTrunkSingle = (num_byte_per_marker % 2 != 0);
    static int num_trunk_per_marker = num_byte_per_marker / 2 + isLastTrunkSingle;
    uint16_t *p_buf;
    uint16_t *trunk_buf;      
    uint32_t curA1A1, curA1A2, curA2A2;
    int raw_index_marker;
    for(int cur_marker_index = 0; cur_marker_index < cur_num_marker_read; ++cur_marker_index){
        //It will cause problems when memory in critical stage.
        //However, other part of this program will also goes wrong in this situation.
        p_buf = (uint16_t *) (buf + cur_marker_index * num_byte_per_marker);
        trunk_buf = p_buf;
        curA1A1 = 0;
        curA1A2 = 0;
        curA2A2 = 0;
        for(int cur_trunk = 0; cur_trunk < num_trunk_per_marker - 1; ++cur_trunk){
            curA1A1 += g_table.get(*trunk_buf, 0);
            curA1A2 += g_table.get(*trunk_buf, 1);
            curA2A2 += g_table.get(*trunk_buf, 2);
            trunk_buf++;
        }

        uint16_t last_trunk = *trunk_buf;
        if(isLastTrunkSingle){
            last_trunk = last_trunk & 255;
            curA1A1 += (g_table.get(last_trunk, 0) - 4 - last_byte_NA_sample);
        }else{
            curA1A1 += (g_table.get(last_trunk, 0) - last_byte_NA_sample); 
        }

        curA1A2 += g_table.get(last_trunk, 1);
        curA2A2 += g_table.get(last_trunk, 2);

        raw_index_marker = num_marker_freq + cur_marker_index;
        

        countA1A1[raw_index_marker] = curA1A1;
        countA1A2[raw_index_marker] = curA1A2;
        countA2A2[raw_index_marker] = curA2A2;
        AFA1[raw_index_marker] = (2.0 * curA1A1 + curA1A2) / (2.0 * (curA1A1 + curA1A2 + curA2A2));
    }
    num_marker_freq += num_marker;
}

void Geno::freq2(uint8_t *buf, int num_marker) {
    //pheno->mask_geno_keep(buf, num_marker);
    const static uint64_t MASK = 6148914691236517205UL; 
    static int pheno_count = pheno->count_keep();
    static int num_trunk = num_byte_per_marker / 8;
    static int remain_bit = (num_byte_per_marker % 8) * 8;
    static int move_bit = 64 - remain_bit;

    if(num_marker_freq >= marker->count_extract()) return;

    int cur_num_marker_read = num_marker;
    
    #pragma omp parallel for schedule(dynamic) 
    for(int cur_marker_index = 0; cur_marker_index < cur_num_marker_read; ++cur_marker_index){
        uint32_t curA1A1, curA1A2, curA2A2;
        uint32_t even_ct = 0, odd_ct = 0, both_ct = 0;
        uint64_t *p_buf = (uint64_t *) (buf + cur_marker_index * num_byte_per_marker);
        int index = 0;
        for(; index < num_trunk ; index++){
            uint64_t g_buf = p_buf[index];
            uint64_t g_buf_h = MASK & (g_buf >> 1);
            odd_ct += popcount(g_buf & MASK);
            even_ct += popcount(g_buf_h);
            both_ct += popcount(g_buf & g_buf_h);
        }
        if(remain_bit){
            uint64_t g_buf = p_buf[index];
            g_buf = (g_buf << move_bit) >> move_bit;
            uint64_t g_buf_h = MASK & (g_buf >> 1);
            odd_ct += popcount(g_buf & MASK);
            even_ct += popcount(g_buf_h);
            both_ct += popcount(g_buf & g_buf_h);
        }

        curA1A1 = pheno_count + both_ct - even_ct - odd_ct;
        curA1A2 = even_ct - both_ct;
        curA2A2 = both_ct;

        int raw_index_marker = num_marker_freq + cur_marker_index;

        countA1A1[raw_index_marker] = curA1A1;
        countA1A2[raw_index_marker] = curA1A2;
        countA2A2[raw_index_marker] = curA2A2;
        AFA1[raw_index_marker] = (2.0 * curA1A1 + curA1A2) / (2.0 * (curA1A1 + curA1A2 + curA2A2));
    }
    num_marker_freq += num_marker;
}
*/

void Geno::sum_geno_x(uint64_t *buf, int num_marker) {
    static bool inited = false;
    static std::ofstream out;
    if(!inited){
        inited = true;
        out.open((options["out"] + ".sum").c_str());
        if (!out) { LOGGER.e(0, "can not open the file [" + options["out"] + ".sum" + "] to write"); }
        out << "CHR\tSNP\tPOS\tA1\tA2\tAAm\tABm\tBBm\tMm\tAAf\tABf\tBBf\tMf" << std::endl;
    }

    const static uint64_t MASK = 6148914691236517205UL; 
    if(num_marker_freq >= marker->count_extract()) return;

    int cur_num_marker_read = num_marker;
    uint32_t *gender_mask = (uint32_t *)keep_male_mask;

    vector<string> out_contents;
    out_contents.resize(cur_num_marker_read);
    
    #pragma omp parallel for schedule(dynamic) 
    for(int cur_marker_index = 0; cur_marker_index < cur_num_marker_read; ++cur_marker_index){
        uint32_t even_ct = 0, odd_ct = 0, both_ct = 0, even_ct_m = 0, odd_ct_m = 0, both_ct_m = 0;
        uint64_t *p_buf = buf + cur_marker_index * num_item_1geno;

        for(int index = 0; index < num_item_1geno ; index++){
            uint64_t mask_gender = *(gender_mask + index);
            mask_gender = fill_inter_zero(mask_gender);
            mask_gender += (mask_gender << 1);

            uint64_t g_buf = p_buf[index];
            uint64_t g_buf_h = MASK & (g_buf >> 1);
            uint64_t g_buf_l = g_buf & MASK;
            uint64_t g_buf_b = g_buf & g_buf_h;
            odd_ct += popcount(g_buf_l);
            even_ct += popcount(g_buf_h);
            both_ct += popcount(g_buf_b);

            even_ct_m += popcount(g_buf_h & mask_gender);
            odd_ct_m += popcount(g_buf_l & mask_gender);
            both_ct_m += popcount(g_buf_b & mask_gender);
        }

        int all_BB = both_ct;
        int all_AB = even_ct - both_ct;
        int all_miss = odd_ct - both_ct;
        int all_AA = num_keep_sample - odd_ct - even_ct + both_ct;

        int m_BB = both_ct_m;
        int m_AB = even_ct_m - both_ct_m;
        int m_miss = odd_ct_m - both_ct_m;
        int m_AA = num_male_keep_sample - odd_ct_m - even_ct_m + both_ct_m;
        
        std::ostringstream os;
        int raw_index_marker = num_marker_freq + cur_marker_index;
        os << marker->get_marker(marker->getExtractIndex(raw_index_marker)) << "\t";
        os << m_AA << "\t" << m_AB << "\t" << m_BB << "\t" << m_miss << "\t";
        os << all_AA - m_AA << "\t" << all_AB - m_AB << "\t" << all_BB - m_BB << "\t" << all_miss - m_miss;

        out_contents[cur_marker_index] = os.str();
 
    }

    std::copy(out_contents.begin(), out_contents.end(), std::ostream_iterator<string>(out, "\n"));
 
    num_marker_freq += num_marker;

}


void Geno::freq64_x(uint64_t *buf, int num_marker) {
    const static uint64_t MASK = 6148914691236517205UL; 
    if(num_marker_freq >= marker->count_extract()) return;

    int cur_num_marker_read = num_marker;
    uint32_t *gender_mask = (uint32_t *)keep_male_mask;
    
    #pragma omp parallel for schedule(dynamic) 
    for(int cur_marker_index = 0; cur_marker_index < cur_num_marker_read; ++cur_marker_index){
        uint32_t even_ct = 0, odd_ct = 0, both_ct = 0, odd_ct_m = 0, both_ct_m = 0;
        uint64_t *p_buf = buf + cur_marker_index * num_item_1geno;
        for(int index = 0; index < num_item_1geno ; index++){
            uint64_t mask_gender = *(gender_mask + index);
            mask_gender = ~fill_inter_zero(mask_gender);

            uint64_t g_buf = p_buf[index];
            uint64_t g_buf_h = MASK & (g_buf >> 1);
            uint64_t g_buf_l = g_buf & MASK;
            uint64_t g_buf_b = g_buf & g_buf_h;
            odd_ct += popcount(g_buf_l);
            even_ct += popcount(g_buf_h);
            both_ct += popcount(g_buf_b);

            odd_ct_m += popcount(g_buf_l & mask_gender);
            both_ct_m += popcount(g_buf_b & mask_gender);
        }

        int raw_index_marker = num_marker_freq + cur_marker_index;
        uint32_t cur_total_markers = total_markers - odd_ct_m - odd_ct + both_ct_m + both_ct;

        double cur_af = 1.0 * (even_ct + both_ct_m) / cur_total_markers;
        if(!marker->isEffecRev(raw_index_marker)){
            cur_af = 1.0 - cur_af;
        }
        AFA1[raw_index_marker] = cur_af;
        //RDev[raw_index_marker] = 2.0 * cur_af * (1.0 - cur_af);
        countMarkers[raw_index_marker] = cur_total_markers;
    }
    num_marker_freq += num_marker;

}

union Geno_prob{
    char byte[4];
    uint32_t value = 0;
};


void Geno::bgen2bed(const vector<uint32_t> &raw_marker_index){
    LOGGER.ts("LOOP_BGEN_BED");
    LOGGER.ts("LOOP_BGEN_TOT");
    vector<uint32_t>& index_keep = pheno->get_index_keep();
    auto buf_size = (num_raw_sample + 31) / 32;
    size_t buf_size_byte = buf_size * 8;

    int num_marker = 1;
    int num_markers = raw_marker_index.size();

    FILE * h_bgen = fopen(options["bgen_file"].c_str(), "rb");
    #pragma omp parallel for schedule(static) ordered
    for(uint32_t index = 0; index < num_markers; index++){
        //LOGGER.i(0, to_string(index) + "NUM_thread: " + to_string(omp_get_max_threads()));
        auto raw_index = raw_marker_index[index];
        uint64_t *buf = new uint64_t[buf_size]();
        uint64_t byte_pos = this->marker->getStartPos(raw_index);
        uint32_t len_comp, len_decomp;
        char * snp_data;

        #pragma omp ordered
        {
            fseek(h_bgen, byte_pos, SEEK_SET);
            len_comp = read1Byte<uint32_t>(h_bgen) - 4;
            len_decomp = read1Byte<uint32_t>(h_bgen);
            snp_data = new char[len_comp];
            readBytes(h_bgen, len_comp, snp_data);
        }
        uLongf dec_size = len_decomp;

        char * dec_data =  new char[len_decomp];
        int z_result = uncompress((Bytef*)dec_data, &dec_size, (Bytef*)snp_data, len_comp);
        delete[] snp_data;
        if(z_result == Z_MEM_ERROR || z_result == Z_BUF_ERROR || dec_size != len_decomp){
            LOGGER.e(0, "decompress genotype data error in " + to_string(raw_index) + "th SNP."); 
        }

        uint32_t n_sample = *(uint32_t *)dec_data;
        if(n_sample != num_raw_sample){
            LOGGER.e(0, "inconsistent number of sample in " + to_string(raw_index) + "th SNP." );
        }
        uint16_t num_alleles = *(uint16_t *)(dec_data + 4);
        if(num_alleles != 2){
            LOGGER.e(0, "multi alleles still detected, the bgen file might be malformed.");
        }

        uint8_t min_ploidy = *(uint8_t *)(dec_data + 6);//2
        uint8_t max_ploidy = *(uint8_t *)(dec_data + 7); //2
        uint8_t * sample_ploidy = (uint8_t *)(dec_data + 8);

        uint8_t *geno_prob = sample_ploidy + n_sample;
        uint8_t is_phased = *(geno_prob);
        uint8_t bits_prob = *(geno_prob+1);
        uint8_t* X_prob = geno_prob + 2;
        uint32_t len_prob = len_decomp - n_sample - 10;
        if(is_phased){
            LOGGER.e(0, "can't support phased data currently.");
        }

        int byte_per_prob = bits_prob / 8;
        int double_byte_per_prob = byte_per_prob * 2;
        if(bits_prob % 8 != 0){
            LOGGER.e(0, "can't support probability bits other than in byte unit.");
        }

        if(len_prob != double_byte_per_prob * n_sample){
            LOGGER.e(0, "malformed data in " + to_string(raw_index) + "th SNP.");
        }

        uint32_t base_value = (1 << bits_prob) - 1;
        uint32_t cut_value = ceil(base_value * options_d["hard_call_thresh"]);

        uint8_t *buf_ptr = (uint8_t *)buf;
        for(uint32_t i = 0; i < num_keep_sample; i++){
            uint32_t item_byte = i >> 2;
            uint32_t move_byte = (i & 3) << 1;

            uint32_t sindex = index_keep[i];
            uint8_t item_ploidy = sample_ploidy[sindex];

            uint8_t geno_value;
            if(item_ploidy > 128){
                geno_value = 1;
            }else if(item_ploidy == 2){
                auto base = sindex * double_byte_per_prob;
                auto base1 = base + byte_per_prob;
                Geno_prob prob_item;
                Geno_prob prob_item1;
                /*
                memcpy(prob_item.byte, X_prob + base,  byte_per_prob); 
                memcpy(prob_item1.byte, X_prob + base1, byte_per_prob); 
                */
                for(int i = 0 ; i != byte_per_prob; i++){
                    prob_item.byte[i] = X_prob[base + i];
                    prob_item1.byte[i] = X_prob[base1 + i];
                }

                uint32_t t1 = prob_item.value;
                uint32_t t2 = prob_item1.value;
                uint32_t t3 = base_value - t1 - t2;
                if(t1 >= cut_value){
                    geno_value = 0;
                }else if(t2 >= cut_value){
                    geno_value = 2;
                }else if(t3 >= cut_value){
                    geno_value = 3;
                }else{
                    geno_value = 1;
                }
            }else{
                LOGGER.e(0, "multiple alleles detected in " + to_string(raw_index) + "th SNP.");
            }
            buf_ptr[item_byte] += geno_value << move_byte;
        }
        //LOGGER.i(0, "MIDDLE: " + to_string(index) + "NUM_thread: " + to_string(omp_get_max_threads()));

        #pragma omp ordered
        save_bed(buf, num_marker);
        delete[] buf;
        delete[] dec_data;
        //#pragma omp ordered
        //LOGGER.i(0, "Finished " + to_string(index) + "NUM_thread: " + to_string(omp_get_max_threads()));
        if(index % 10000 == 0){
            float time_p = LOGGER.tp("LOOP_BGEN_BED");
            if(time_p > 300){
                LOGGER.ts("LOOP_BGEN_BED");
                float elapse_time = LOGGER.tp("LOOP_BGEN_TOT");
                float finished_percent = (float) index / num_markers;
                float remain_time = (1.0 / finished_percent - 1) * elapse_time / 60;

                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << finished_percent * 100 << "% Estimated time remaining " << remain_time << " min"; 
                
                LOGGER.i(1, ss.str());
            }
        }

    }
    closeOut();
    fclose(h_bgen);
}



void Geno::save_bed(uint64_t *buf, int num_marker){
    static string err_string = "can't write to [" + options["out"] + ".bed].";
    static bool inited = false;
    if(!inited){
        hOut = fopen((options["out"] + ".bed").c_str(), "wb");
        if(hOut == NULL){
            LOGGER.e(0, err_string);
        }
        uint8_t * buffer = new uint8_t[3];
        buffer[0] = 0x6c;
        buffer[1] = 0x1b;
        buffer[2] = 0x01;
        if(3 != fwrite(buffer, sizeof(uint8_t), 3, hOut)){
            LOGGER.e(0, err_string);
        }
        inited = true;
        delete[] buffer;
    }

    uint64_t base_buffer = 0;
    for(int i = 0; i < num_marker; i++){
        uint8_t * buffer = (uint8_t *) (buf + base_buffer);
        if(fwrite(buffer, sizeof(uint8_t), num_byte_keep_geno1,hOut) != num_byte_keep_geno1){
            LOGGER.e(0, err_string);
        }
        base_buffer += num_item_1geno;
    }

}

void Geno::closeOut(){
    fclose(hOut);
}


void Geno::freq64(uint64_t *buf, int num_marker) {
    //pheno->mask_geno_keep(buf, num_marker);
    const static uint64_t MASK = 6148914691236517205UL; 
    if(num_marker_freq >= marker->count_extract()) return;

    int cur_num_marker_read = num_marker;
    
    #pragma omp parallel for schedule(dynamic) 
    for(int cur_marker_index = 0; cur_marker_index < cur_num_marker_read; ++cur_marker_index){
        //uint32_t curA1A1, curA1A2, curA2A2;
        uint32_t even_ct = 0, odd_ct = 0, both_ct = 0;
        uint64_t *p_buf = buf + cur_marker_index * num_item_1geno;
        for(int index = 0; index < num_item_1geno ; index++){
            uint64_t g_buf = p_buf[index];
            uint64_t g_buf_h = MASK & (g_buf >> 1);
            odd_ct += popcount(g_buf & MASK);
            even_ct += popcount(g_buf_h);
            both_ct += popcount(g_buf & g_buf_h);
        }

        //curA1A1 = num_keep_sample + both_ct - even_ct - odd_ct;
        //curA1A2 = even_ct - both_ct;
        //curA2A2 = both_ct;

        int raw_index_marker = num_marker_freq + cur_marker_index;

        //countA1A1[raw_index_marker] = curA1A1;
        //countA1A2[raw_index_marker] = curA1A2;
        //countA2A2[raw_index_marker] = curA2A2;
        uint32_t cur_total_markers = (total_markers - 2 * (odd_ct - both_ct));
        double cur_af = 1.0*(even_ct + both_ct) / cur_total_markers;
        if(!marker->isEffecRev(raw_index_marker)){
            cur_af = 1.0 - cur_af;
        }
        AFA1[raw_index_marker] = cur_af;
        //RDev[raw_index_marker] = 2.0 * cur_af * (1.0 - cur_af);
        countMarkers[raw_index_marker] = cur_total_markers;
    }
    num_marker_freq += num_marker;
}

// extracted and revised from plink2.0
// GPL v3, license detailed on github
// https://github.com/chrchang/plink-ng
void copy_quaterarr_nonempty_subset(uint64_t* raw_quaterarr[], const uint64_t* subset_mask, uint32_t raw_quaterarr_entry_ct, uint32_t subset_entry_ct, uint64_t* output_quaterarr[], const int num_marker) {
    // in plink 2.0, we probably want (0-based) bit raw_quaterarr_entry_ct of
    // subset_mask to be always allocated and unset.  This removes a few special
    // cases re: iterating past the end of arrays.
    static const uint32_t kBitsPerWordD2 = 32;

    uint64_t cur_output_word[num_marker];
    memset(cur_output_word, 0, num_marker * 8);

    uint64_t* output_quaterarr_iter[num_marker];
    uint64_t* output_quaterarr_last[num_marker];
    for(int i = 0; i != num_marker; i++){
        output_quaterarr_iter[i] = output_quaterarr[i];
        output_quaterarr_last[i] = &(output_quaterarr[i][subset_entry_ct / kBitsPerWordD2]);
    }
    const uint32_t word_write_halfshift_end = subset_entry_ct % kBitsPerWordD2;
    uint32_t word_write_halfshift = 0;
    // if <= 2/3-filled, use sparse copy algorithm
    // (tried copy_bitarr_subset() approach, that actually worsened things)
    if (subset_entry_ct * (3 * k1LU) <= raw_quaterarr_entry_ct * (2 * k1LU)) {
        uint32_t subset_mask_widx = 0;
        while (1) {
            const uint64_t cur_include_word = subset_mask[subset_mask_widx];
            if (cur_include_word) {
                uint32_t wordhalf_idx = 0;
                uint32_t cur_include_halfword = (halfword_t)cur_include_word;
                while (1) {
                    if (cur_include_halfword) {
                        uint64_t raw_quaterarr_word[num_marker];
                        uint32_t temp_index = subset_mask_widx * 2 + wordhalf_idx;
                        for(int i = 0; i != num_marker; i++){
                            raw_quaterarr_word[i] = raw_quaterarr[i][temp_index];
                        }
                        do {
                            uint32_t rqa_idx_lowbits = CTZ64U(cur_include_halfword);
                            uint32_t lshift = word_write_halfshift * 2; 
                            uint32_t rshift = rqa_idx_lowbits * 2;
                            for(int i = 0; i != num_marker; i++){
                                cur_output_word[i] |= ((raw_quaterarr_word[i] >> rshift) & 3) << lshift;
                            }
                            if (++word_write_halfshift == kBitsPerWordD2) {
                                for(int i = 0; i != num_marker; i++){
                                    *(output_quaterarr_iter[i])++ = cur_output_word[i];
                                }
                                word_write_halfshift = 0;
                                //cur_output_word = 0;
                                memset(cur_output_word, 0, num_marker * 8);
                            }
                            cur_include_halfword &= cur_include_halfword - 1;
                        } while (cur_include_halfword);
                    }
                    if (wordhalf_idx) {
                        break;
                    }
                    ++wordhalf_idx;
                    cur_include_halfword = cur_include_word >> kBitsPerWordD2;
                }
                if (output_quaterarr_iter[0] == output_quaterarr_last[0]) {
                    if (word_write_halfshift == word_write_halfshift_end) {
                        if (word_write_halfshift_end) {
                            for(int i = 0; i != num_marker; i++){
                                *(output_quaterarr_last[i]) = cur_output_word[i];
                            }
                        }
                        return;
                    }
                }
            }
            ++subset_mask_widx;
        }
    }

    const uint64_t* raw_quaterarr_iter[num_marker];
    for(int i = 0; i != num_marker; i++){
        raw_quaterarr_iter[i] = raw_quaterarr[i];
    }
    //const uint64_t* raw_quaterarr_iter = raw_quaterarr;
    while (1) {
        const uint64_t cur_include_word = *subset_mask++;
        uint32_t wordhalf_idx = 0;
        uint64_t cur_include_halfword = (halfword_t)cur_include_word;
        while (1) {
           // uintptr_t raw_quaterarr_word = *raw_quaterarr_iter++;
            uint64_t raw_quaterarr_word[num_marker];
            for(int i = 0; i != num_marker; i++){
                raw_quaterarr_word[i] = *(raw_quaterarr_iter[i]++);
            }
            while (cur_include_halfword) {
                uint32_t rqa_idx_lowbits = CTZ64U(cur_include_halfword); // tailing zero
                uint64_t halfword_invshifted = (~cur_include_halfword) >> rqa_idx_lowbits;
                uint64_t raw_quaterarr_curblock_unmasked[num_marker];
                int m_bit = rqa_idx_lowbits * 2;
                for(int i = 0; i != num_marker; i++){
                    raw_quaterarr_curblock_unmasked[i] = raw_quaterarr_word[i] >> m_bit; 
                }
                //uintptr_t raw_quaterarr_curblock_unmasked = raw_quaterarr_word >> (rqa_idx_lowbits * 2); // remove mask bit tailing zero, not to keep
                uint32_t rqa_block_len = CTZ64U(halfword_invshifted);  // find another keep
                uint32_t block_len_limit = kBitsPerWordD2 - word_write_halfshift;
                m_bit = 2 * word_write_halfshift;
                for(int i = 0; i != num_marker; i++){
                    cur_output_word[i] |= raw_quaterarr_curblock_unmasked[i] << m_bit; // avoid overwrite current saved bits
                }
                if (rqa_block_len < block_len_limit) { //2  16
                    word_write_halfshift += rqa_block_len; // 0 2
                    m_bit = 2 * word_write_halfshift;
                    uint64_t temp_mask = (k1LU << m_bit) - k1LU;
                    for(int i = 0; i != num_marker; i++){
                        cur_output_word[i] &= temp_mask; // mask high end, and keep low needed bits
                    }
                } else {
                    // no need to mask, extra bits vanish off the high end
                    for(int i = 0; i != num_marker; i++){
                        *(output_quaterarr_iter[i]++) = cur_output_word[i];
                    }
                    word_write_halfshift = rqa_block_len - block_len_limit;
                    if (word_write_halfshift) {
                        uint64_t t_mask = ((k1LU << (2 * word_write_halfshift)) - k1LU), mi_bit = 2 * block_len_limit;
                        for(int i = 0; i != num_marker; i++){
                            cur_output_word[i] = (raw_quaterarr_curblock_unmasked[i] >> mi_bit) & t_mask;
                        }
                    } else {
                        // avoid potential right-shift-[word length]
                        //cur_output_word = 0;
                        memset(cur_output_word, 0, num_marker * 8);
                    }
                }
                cur_include_halfword &= (~(k1LU << (rqa_block_len + rqa_idx_lowbits))) + k1LU;
            }
            if (wordhalf_idx) {
                break;
            }
            ++wordhalf_idx;
            cur_include_halfword = cur_include_word >> kBitsPerWordD2;
        }
        if (output_quaterarr_iter[0] == output_quaterarr_last[0]) {
            if (word_write_halfshift == word_write_halfshift_end) {
                if (word_write_halfshift_end) {
                    for(int i = 0; i != num_marker; i++){
                        *(output_quaterarr_last[i]) = cur_output_word[i];
                    }
                }
                return;
            }
        }
    }
}

void Geno::move_geno(uint8_t *buf, uint64_t *keep_list, uint32_t num_raw_sample, uint32_t num_keep_sample, uint32_t num_marker, uint64_t *geno_buf){
    /*
    if(num_raw_sample == num_keep_sample){
        for(int index = 0; index < num_marker; index++){
            memcpy(

        }
    }
    */

    uint32_t num_byte_keep_geno = (num_keep_sample + 3) / 4;
    uint32_t num_byte_per_marker = (num_raw_sample + 3) / 4;
    uint32_t num_qword_per_marker = (num_byte_keep_geno + 7) / 8;

    static int remain_bit = (num_byte_per_marker % 8) * 8;
    static int move_bit = 64 - remain_bit;
    static uint64_t MASK = (UINT64_MAX << move_bit) >> move_bit;

    const int move_markers = 5;

    #pragma omp parallel for schedule(dynamic) 
    for(uint32_t index = 0; index < num_marker; index += move_markers){
        uint64_t *pbuf[move_markers], *gbuf[move_markers];
        for(uint32_t i = 0; i < move_markers; i++){
            pbuf[i] = (uint64_t *) (buf + (index + i) * num_byte_per_marker);
            gbuf[i] = geno_buf + (index + i) * num_qword_per_marker; 
        }
        copy_quaterarr_nonempty_subset(pbuf, keep_list, num_raw_sample, num_keep_sample, gbuf, move_markers);
    }
}

void Geno::loop_64block(const vector<uint32_t> &raw_marker_index, vector<function<void (uint64_t *buf, int num_marker)>> callbacks, bool showLog) {
    if(showLog){
        LOGGER.i(0, "Reading PLINK BED file(s) in SNP-major format...");
        num_finished_markers = 0;
    }
    thread read_thread([this, &raw_marker_index](){this->read_bed(raw_marker_index);});
    read_thread.detach();

    uint8_t *r_buf = NULL;
    bool isEOF = false;
    int cur_num_marker_read;

    if(showLog){
        LOGGER.ts("LOOP_GENO_TOT");
        LOGGER.ts("LOOP_GENO_PRE");
    }
    int cur_num_blocks = (raw_marker_index.size() + Constants::NUM_MARKER_READ - 1) / Constants::NUM_MARKER_READ;

    for(int cur_block = 0; cur_block < cur_num_blocks; ++cur_block){
        std::tie(r_buf, isEOF) = asyncBuffer->start_read();
        //LOGGER.i(0, "time get buffer: " + to_string(LOGGER.tp("LOOP_GENO_PRE")));

        LOGGER.d(0, "Process block " + std::to_string(cur_block));
        if(isEOF && cur_block != (cur_num_blocks - 1)){
            LOGGER.e(0, "read to the end of the BED file, but still didn't finish.");
        }
        //correct the marker read;
        if(cur_block == (cur_num_blocks - 1)){
            cur_num_marker_read = raw_marker_index.size() - Constants::NUM_MARKER_READ * cur_block;
        }else{
            cur_num_marker_read = Constants::NUM_MARKER_READ;
        }

        uint64_t *geno_buf = new uint64_t[num_item_geno_buffer];
        move_geno(r_buf, keep_mask, num_raw_sample, num_keep_sample, cur_num_marker_read, geno_buf);
        asyncBuffer->end_read();

        for(auto callback : callbacks){
         //   LOGGER.i(0, "time1: " + to_string(LOGGER.tp("LOOP_GENO_PRE")));
            callback(geno_buf, cur_num_marker_read);
          //  LOGGER.i(0, "time2: " + to_string(LOGGER.tp("LOOP_GENO_PRE")));
        }

        delete[] geno_buf;

        num_finished_markers += cur_num_marker_read;
        if(showLog && cur_block % 100 == 0){
            float time_p = LOGGER.tp("LOOP_GENO_PRE");
            if(time_p > 300){
                LOGGER.ts("LOOP_GENO_PRE");
                float elapse_time = LOGGER.tp("LOOP_GENO_TOT");
                float finished_percent = (float) cur_block / cur_num_blocks;
                float remain_time = (1.0 / finished_percent - 1) * elapse_time / 60;

                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << finished_percent * 100 << "% Estimated time remaining " << remain_time << " min"; 
                
                LOGGER.i(1, ss.str());
            }
        }
    }
    if(showLog){
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << "100% Finished in " << LOGGER.tp("LOOP_GENO_TOT") / 60 << " min";
        LOGGER.i(1, ss.str());
    }
}


void Geno::makeMarkerX(uint64_t *buf, int cur_marker, double *w_buf, bool center, bool std){
    static uint32_t last_sample = num_keep_sample % 32;
    static uint32_t last_8block = last_sample / 4;
    static uint32_t last_2block = last_sample % 4; 

    uint32_t cur_raw_marker = num_finished_markers + cur_marker;
    uint64_t *cur_buf = buf + cur_marker * num_item_1geno;
    double af = AFA1[cur_raw_marker];
    double mu = 2.0 * af;
    double center_value = 0.0;
    if(center){
        center_value = mu;
    }
    double rdev = 1.0;
    if(std){
        rdev = 1.0 / sqrt(mu * (1.0 - af));
        //use RDev
    }

    double g1_lookup[4];
    g1_lookup[0] = (2.0 - center_value) * rdev;
    g1_lookup[1] = (mu - center_value) * rdev;
    g1_lookup[2] = (1.0 - center_value) * rdev;
    g1_lookup[3] = (0.0 - center_value) * rdev;

    
    double g_lookup[256][4];
    for(uint16_t i = 0; i <= 255; i++){
        for(uint16_t j = 0; j < 4; j++){
            g_lookup[i][j] = g1_lookup[(i >> (2 * j)) & 3];
        }
    }

    int sub_index = 0;
    uint32_t index = 0;
    for(; index < num_item_1geno - 1; index++){
        uint64_t geno = cur_buf[index];
        int move_bit = 0;
        for(int ri = 0; ri < 8; ri++){
            uint8_t geno_temp = (uint8_t) (geno >> move_bit);
            w_buf[sub_index] = g_lookup[geno_temp][0]; 
            w_buf[sub_index + 1] = g_lookup[geno_temp][1]; 
            w_buf[sub_index + 2] = g_lookup[geno_temp][2]; 
            w_buf[sub_index + 3] = g_lookup[geno_temp][3]; 
            move_bit += 8;
            sub_index += 4;
        }
    }
    //last geno
    uint64_t geno = cur_buf[index];
    int move_bit = 0;
    for(int ri = 0; ri < last_8block; ri++){
        uint8_t geno_temp = (uint8_t) (geno >> move_bit);
        w_buf[sub_index] = g_lookup[geno_temp][0]; 
        w_buf[sub_index + 1] = g_lookup[geno_temp][1]; 
        w_buf[sub_index + 2] = g_lookup[geno_temp][2]; 
        w_buf[sub_index + 3] = g_lookup[geno_temp][3]; 
        move_bit += 8;
        sub_index += 4;
    }
    //last 4
    uint8_t geno_temp = (uint8_t) (geno >> move_bit);
    for(int ri = 0; ri < last_2block; ri++){
        w_buf[sub_index + ri] = g_lookup[geno_temp][ri];
    }

}


void Geno::addOneFileOption(string key_store, string append_string, string key_name,
                                     map<string, vector<string>> options_in) {
    if(options_in.find(key_name) != options_in.end()){
        if(options_in[key_name].size() == 1){
            options[key_store] = options_in[key_name][0] + append_string;
        }else if(options_in[key_name].size() > 1){
            options[key_store] = options_in[key_name][0] + append_string;
            LOGGER.w(0, "Geno: multiple " + key_name + ", use the first one only" );
        }else{
            LOGGER.e(0, "no " + key_name + " parameter found");
        }
        std::ifstream f(options[key_store].c_str());
        if(!f.good()){
            LOGGER.e(0, key_name + " " + options[key_store] + " not found");
        }
        f.close();
    }
}

int Geno::registerOption(map<string, vector<string>>& options_in) {
    int return_value = 0;
    addOneFileOption("geno_file", ".bed","--bfile", options_in);
    addOneFileOption("bgen_file", "", "--bgen", options_in);
    options_in.erase("--bfile");
    if(options_in.find("m_file") != options_in.end()){
        for(auto & item : options_in["m_file"]){
            std::ifstream file_item((item + ".bed").c_str());
            if(file_item.fail()){
                LOGGER.e(0, "can't read BED file in [" + item + "].");
            }
            file_item.close();
        }
        options["m_file"] = boost::algorithm::join(options_in["m_file"], "\t");
        boost::replace_all(options["m_file"], "\r", "");
    }
    options_in.erase("m_file");

    options_d["min_maf"] = 0.0;
    options_d["max_maf"] = 0.5;
    options_d["hard_call_thresh"] = 0.9;

    if(options_in.find("--maf") != options_in.end()){
        auto option = options_in["--maf"];
        if(option.size() == 1){
            try{
                options_d["min_maf"] = std::stod(option[0]);
            }catch(std::invalid_argument&){
                LOGGER.e(0, "illegal value in --maf");
            }
            if(options_d["min_maf"]<0.0 || options_d["max_maf"]>0.5){
                LOGGER.e(0, "--maf can't be smaller than 0 or larger than 0.5");
            }

        }else{
            LOGGER.e(0, "multiple value in --maf, not supported currently");
        }
        options_in.erase("--maf");
    }

     if(options_in.find("--max-maf") != options_in.end()){
        auto option = options_in["--max-maf"];
        if(option.size() == 1){
            try{
                options_d["max_maf"] = std::stod(option[0]);
           }catch(std::invalid_argument&){
                LOGGER.e(0, "illegal value in --maf");
           }
           if(options_d["max_maf"] < 0.0 || options_d["max_maf"] > 0.5){
               LOGGER.e(0, "--max-maf can't be smaller than 0 or larger than 0.5");
           }
        }else{
            LOGGER.e(0, "multiple value in --maf, not supported currently");
        }
        options_in.erase("--max-maf");
    }

    if(options_d["min_maf"] > options_d["max_maf"]){
        LOGGER.e(0, "--maf can't be larger than --max-maf value");
    }

    if(options_in.find("--freq") != options_in.end()){
        processFunctions.push_back("freq");
        if(options_in["--freq"].size() != 0){
            LOGGER.w(0, "--freq should not follow by other parameters, if you want to calculate in founders only, "
                    "please specify by --founders option");
        }
        options_in.erase("--freq");

        options["out"] = options_in["--out"][0];

        return_value++;
    }

    if(options_in.find("--freqx") != options_in.end()){
        processFunctions.push_back("freqx");
        if(options_in["--freqx"].size() != 0){
            LOGGER.w(0, "--freq should not follow by other parameters, if you want to calculate in founders only, "
                    "please specify by --founders option");
        }
        options_in.erase("--freqx");

        options["out"] = options_in["--out"][0];

        return_value++;
    }

    if(options_in.find("--make-bed") != options_in.end()){
        if(options.find("bgen_file") == options.end()){
            processFunctions.push_back("make_bed");
        }else{
            processFunctions.push_back("make_bed_bgen");
        } 

        options_in.erase("--make-bed");
        options["out"] = options_in["--out"][0];

        return_value++;
    }

    addOneFileOption("update_freq_file", "", "--update-freq", options_in);

    if(options_in.find("--filter-sex") != options_in.end()){
        options["sex"] = "yes";
    }

    if(options_in.find("--sum-geno-x") != options_in.end()){
        processFunctions.push_back("sum_geno_x");
        options["sex"] = "yes";
        std::map<string, vector<string>> t_option;
        t_option["--chrx"] = {};
        t_option["--filter-sex"] = {};
        Pheno::registerOption(t_option);
        Marker::registerOption(t_option);
        options["out"] = options_in["--out"][0];
        return_value++;
    }

    return return_value;
}

void Geno::processMain() {
    //vector<function<void (uint8_t *, int)>> callBacks;
    vector<function<void (uint64_t *, int)>> callBacks;
    for(auto &process_function : processFunctions){
        if(process_function == "freq"){
            Pheno pheno;
            Marker marker;
            Geno geno(&pheno, &marker);
            if(geno.num_marker_freq == 0 ){
                LOGGER.i(0, "Computing allele frequencies...");
                callBacks.push_back(bind(&Geno::freq64, &geno, _1, _2));
                geno.loop_64block(marker.get_extract_index(), callBacks);
            }
            geno.out_freq(options["out"]);
            callBacks.clear();
        }

        if(process_function == "freqx"){
            std::map<string, vector<string>> t_option;
            t_option["--chrx"] = {};
            t_option["--filter-sex"] = {}; 
            Pheno::registerOption(t_option);
            Marker::registerOption(t_option);
            Geno::registerOption(t_option);

            Pheno pheno;
            Marker marker;
            Geno geno(&pheno, &marker);
            if(geno.num_marker_freq == 0 ){
                LOGGER.i(0, "Computing allele frequencies...");
                callBacks.push_back(bind(&Geno::freq64_x, &geno, _1, _2));
                geno.loop_64block(marker.get_extract_index(), callBacks);
            }
            geno.out_freq(options["out"]);
            callBacks.clear();
        }

        if(process_function == "make_bed"){
            Pheno pheno;
            Marker marker;
            Geno geno(&pheno, &marker);
            string filename = options["out"];
            pheno.save_pheno(filename + ".fam");
            marker.save_marker(filename + ".bim");
            LOGGER.i(0, "Saving genotype to PLINK format [" + filename + ".bed]...");
            callBacks.push_back(bind(&Geno::save_bed, &geno, _1, _2));
            geno.loop_64block(marker.get_extract_index(), callBacks);
            geno.closeOut();
            LOGGER.i(0, "Genotype has been saved.");
        }

        if(process_function == "make_bed_bgen"){
            Pheno pheno;
            Marker marker;
            Geno geno(&pheno, &marker);
            string filename = options["out"];
            pheno.save_pheno(filename + ".fam");
            marker.save_marker(filename + ".bim");
            LOGGER.i(0, "Converting bgen to PLINK format [" + filename + ".bed]...");
            geno.bgen2bed(marker.get_extract_index());
            LOGGER.i(0, "Genotype has been saved.");
        }


        if(process_function == "sum_geno_x"){
            Pheno pheno;
            Marker marker;
            Geno geno(&pheno, &marker);
            LOGGER.i(0, "Summing genotype in with sex"); 
            callBacks.push_back(bind(&Geno::sum_geno_x, &geno, _1, _2));
            geno.loop_64block(marker.get_extract_index(), callBacks);
            LOGGER.i(0, "Summary has bee saved.");
        }


    }

}
