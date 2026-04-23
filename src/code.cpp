#include <bits/stdc++.h>
using namespace std;

// Persistent file-backed KV: index (string) -> sorted set of int values.
// Constraints: up to 1e5 entries, memory limit ~6 MiB. Avoid storing unnecessary data.
// Approach: Maintain an on-disk append-only log and a compact index file per key bucket.
// To keep implementation simple within limits, we'll store data in a single binary file
// with records and build minimal in-memory index during run, but only for keys touched
// in this session. For "continue based on previous run" we reuse the file content.
// We cannot load all data into memory; instead we scan lazily per find.
// Design:
// - Data file: records of type {op: uint8 (1=insert,2=delete), keyLen: uint8, key bytes, value: int32}.
// - File path: data.db in project root.
// - insert: append record; delete: append record.
// - find(key): stream through file and collect values for that key only, using unordered_set
//   to maintain current values, processing in order of records. Then output sorted.
// Complexity: O(total records) per find worst-case, but n<=1e5 and TL generous.
// To reduce work, we'll build a simple key->vector<offset> index file (keyidx.db) that maps
// keys to starting offsets where that key appears; but this requires extra memory to maintain.
// Given constraints and typical OJ, a full scan 1e5 records per find is acceptable.
// We'll also cache results of keys found in this session to avoid repeated scans.

static const char* DATA_FILE = "data.db";
static const char* DIR_FILE  = "dir.db"; // pairs of {hash:uint64, offset:uint64}
static const char* DIR2_FILE = "dir2.db"; // records: [klen:uint8][key][op:uint8][val:int32]

struct Rec {
    uint8_t op; // 1 insert, 2 delete
    string key;
    int32_t val;
};

static inline uint64_t fnv1a64(const string& s){
    const uint64_t FNV_OFFSET = 1469598103934665603ull;
    const uint64_t FNV_PRIME  = 1099511628211ull;
    uint64_t h=FNV_OFFSET;
    for(unsigned char c: s){ h ^= c; h *= FNV_PRIME; }
    return h;
}

// Append a record to data file and dir file
void append_record(FILE* fp_data, FILE* fp_dir, const string& key, uint8_t op, int32_t val) {
    uint8_t klen = (uint8_t)min<size_t>(key.size(), 255);
    long long pos = fp_data ? ftell(fp_data) : -1;
    if(fp_data){
        fwrite(&op, 1, 1, fp_data);
        fwrite(&klen, 1, 1, fp_data);
        fwrite(key.data(), 1, klen, fp_data);
        fwrite(&val, sizeof(val), 1, fp_data);
    }
    if(fp_dir && pos>=0){
        uint64_t h = fnv1a64(key);
        fwrite(&h, sizeof(h), 1, fp_dir);
        uint64_t off = (uint64_t)pos;
        fwrite(&off, sizeof(off), 1, fp_dir);
    }
    // Also append to dir2: exact key record for faster lookup without verifying data.db
    static FILE* fp_dir2 = nullptr;
    if(!fp_dir2) fp_dir2 = fopen(DIR2_FILE, "ab");
    if(fp_dir2){
        fwrite(&klen, 1, 1, fp_dir2);
        fwrite(key.data(), 1, klen, fp_dir2);
        fwrite(&op, 1, 1, fp_dir2);
        fwrite(&val, sizeof(val), 1, fp_dir2);
        fflush(fp_dir2);
    }
}

// Read record at a given offset
bool read_record_at(FILE* fp, uint64_t off, Rec& r){
    if(!fp) return false;
    if(fseek(fp, (long)off, SEEK_SET)!=0) return false;
    uint8_t op; uint8_t klen; int32_t val;
    if(fread(&op,1,1,fp)!=1) return false;
    if(fread(&klen,1,1,fp)!=1) return false;
    string key; key.resize(klen);
    if(klen>0){ if(fread(&key[0],1,klen,fp)!=klen) return false; }
    if(fread(&val,sizeof(val),1,fp)!=1) return false;
    r.op = op; r.key = std::move(key); r.val = val;
    return true;
}

// Iterate records streaming (used only for fallback or initial scan)
class RecIter {
    FILE* fp;
public:
    RecIter(const char* path) {
        fp = fopen(path, "rb");
    }
    ~RecIter(){ if(fp) fclose(fp);}
    bool ok() const { return fp != nullptr; }
    bool next(Rec& r){
        if(!fp) return false;
        uint8_t op; uint8_t klen; int32_t val;
        size_t rd = fread(&op,1,1,fp);
        if(rd!=1) return false;
        if(fread(&klen,1,1,fp)!=1) return false;
        string key; key.resize(klen);
        if(klen>0){ if(fread(&key[0],1,klen,fp)!=klen) return false; }
        if(fread(&val,sizeof(val),1,fp)!=1) return false;
        r.op = op; r.key = std::move(key); r.val = val;
        return true;
    }
};

static FILE* g_rf = nullptr;

int main(){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // Ensure data file exists
    {
        FILE* chk = fopen(DATA_FILE, "rb");
        if(!chk){ FILE* nf = fopen(DATA_FILE, "wb"); if(nf) fclose(nf); }
        else fclose(chk);
    }

    int n; if(!(cin>>n)) return 0;
    string cmd, key; long long vll;

    // Open append files once
    FILE* fp_data = fopen(DATA_FILE, "ab");
    FILE* fp_dir  = fopen(DIR_FILE,  "ab");
    if(!fp_data){ /* fallback: no persistence */ }

    // Simple session cache of computed finds to avoid rescans
    unordered_map<string, vector<int>> cache;
    unordered_map<uint64_t, vector<uint64_t>> dirCache; // hash -> offsets
    bool dirPreloaded = false;

    // Preload dir.db once to avoid repeated scans when many keys are queried
    {
        FILE* df = fopen(DIR_FILE, "rb");
        if(df){
            uint64_t hh, off;
            while(fread(&hh,sizeof(hh),1,df)==1 && fread(&off,sizeof(off),1,df)==1){
                dirCache[hh].push_back(off);
            }
            fclose(df);
            dirPreloaded = true;
        }
    }

    for(int i=0;i<n;++i){
        cin>>cmd;
        if(cmd=="insert"){
            cin>>key>>vll; int32_t val = (int32_t)vll;
            append_record(fp_data, fp_dir, key, 1, val);
            if(fp_data){ fflush(fp_data); }
            if(fp_dir){ fflush(fp_dir); }
            // Track new offset in dirCache for this key to avoid rereading dir file later
            if(fp_data){
                long long off = ftell(fp_data) - (long long)(1+1+min<size_t>(key.size(),255)+sizeof(int32_t));
                uint64_t h = fnv1a64(key);
                dirCache[h].push_back((uint64_t)off);
            }
            // If key is cached, update incrementally
            auto cit = cache.find(key);
            if(cit!=cache.end()){
                auto &vec = cit->second;
                auto itp = lower_bound(vec.begin(), vec.end(), (int)val);
                if(itp==vec.end() || *itp != (int)val) vec.insert(itp, (int)val);
            }
        } else if(cmd=="delete"){
            cin>>key>>vll; int32_t val = (int32_t)vll;
            append_record(fp_data, fp_dir, key, 2, val);
            if(fp_data){ fflush(fp_data); }
            if(fp_dir){ fflush(fp_dir); }
            if(fp_data){
                long long off = ftell(fp_data) - (long long)(1+1+min<size_t>(key.size(),255)+sizeof(int32_t));
                uint64_t h = fnv1a64(key);
                dirCache[h].push_back((uint64_t)off);
            }
            auto cit = cache.find(key);
            if(cit!=cache.end()){
                auto &vec = cit->second;
                auto itp = lower_bound(vec.begin(), vec.end(), (int)val);
                if(itp!=vec.end() && *itp==(int)val) vec.erase(itp);
            }
        } else if(cmd=="find"){
            cin>>key;
            auto it = cache.find(key);
            if(it==cache.end()){
                if(fp_data) fflush(fp_data);
                if(fp_dir) fflush(fp_dir);
                unordered_set<int> s; s.reserve(16);
                uint64_t h = fnv1a64(key);

                // Load offsets for this hash from dir.db
                vector<uint64_t> offs;
                auto dit = dirCache.find(h);
                if(dit!=dirCache.end()) offs = dit->second;

                if(!dirPreloaded){
                    FILE* df = fopen(DIR_FILE, "rb");
                    if(df){
                        uint64_t hh, off;
                        while(fread(&hh,sizeof(hh),1,df)==1 && fread(&off,sizeof(off),1,df)==1){
                            if(hh==h) offs.push_back(off);
                        }
                        fclose(df);
                    }
                    dirCache[h] = offs;
                }

                                // If no offsets found (likely older data before dir.db existed), do a one-time scan
                if(offs.empty()){
                    FILE* rf = fopen(DATA_FILE, "rb");
                    if(rf){
                        while(true){
                            uint64_t pos = (uint64_t)ftell(rf);
                            uint8_t op; uint8_t klen; int32_t val;
                            size_t rd = fread(&op,1,1,rf);
                            if(rd!=1) break;
                            if(fread(&klen,1,1,rf)!=1) break;
                            string k; k.resize(klen);
                            if(klen>0){ if(fread(&k[0],1,klen,rf)!=klen) break; }
                            if(fread(&val,sizeof(val),1,rf)!=1) break;
                            if(k==key){
                                offs.push_back(pos);
                                if(op==1) s.insert(val);
                                else if(op==2) s.erase(val);
                            }
                        }
                        fclose(rf);
                        // Backfill dir.db for this key
                        if(fp_dir && !offs.empty()){
                            FILE* df = fopen(DIR_FILE, "ab");
                            if(df){
                                uint64_t hh = h;
                                for(uint64_t off: offs){
                                    fwrite(&hh, sizeof(hh), 1, df);
                                    fwrite(&off, sizeof(off), 1, df);
                                }
                                fclose(df);
                            }
                        }
                        dirCache[h] = offs;
                    }
                } else {
                    // Read only records at these offsets and verify key matches (hash may collide)
                    sort(offs.begin(), offs.end());
                    if(!g_rf) g_rf = fopen(DATA_FILE, "rb");
                    if(g_rf){
                        Rec r;
                        for(uint64_t off : offs){
                            if(read_record_at(g_rf, off, r)){
                                if(r.key==key){
                                    if(r.op==1) s.insert(r.val);
                                    else if(r.op==2) s.erase(r.val);
                                }
                            }
                        }
                    }
                }

                vector<int> vals; vals.reserve(s.size());
                for(int x: s) vals.push_back(x);
                sort(vals.begin(), vals.end());
                cache[key] = vals;
                it = cache.find(key);
            }
            if(it->second.empty()){
                cout<<"null\n";
            } else {
                for(size_t j=0;j<it->second.size();++j){
                    if(j) cout<<' ';
                    cout<<it->second[j];
                }
                cout<<"\n";
            }
        }
    }

    if(fp_data) fclose(fp_data);
    if(fp_dir)  fclose(fp_dir);
    if(g_rf) fclose(g_rf);
    return 0;
}
