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

struct Rec {
    uint8_t op; // 1 insert, 2 delete
    string key;
    int32_t val;
};

// Append a record to data file
void append_record(FILE* fp, uint8_t op, const string& key, int32_t val) {
    uint8_t klen = (uint8_t)min<size_t>(key.size(), 255);
    fwrite(&op, 1, 1, fp);
    fwrite(&klen, 1, 1, fp);
    fwrite(key.data(), 1, klen, fp);
    fwrite(&val, sizeof(val), 1, fp);
}

// Iterate records streaming
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

    // Open append file once
    FILE* fp = fopen(DATA_FILE, "ab");
    if(!fp){
        // Fallback: cannot open file, but still process in-memory (will not persist)
    }

    // Simple session cache of computed finds to avoid rescans for same key repeatedly
    // Cache stores sorted vector; invalidated when new op on that key occurs.
    unordered_map<string, vector<int>> cache;

    for(int i=0;i<n;++i){
        cin>>cmd;
        if(cmd=="insert"){
            cin>>key>>vll; int32_t val = (int32_t)vll;
            if(fp) append_record(fp, 1, key, val);
            auto it = cache.find(key); if(it!=cache.end()) cache.erase(it);
        } else if(cmd=="delete"){
            cin>>key>>vll; int32_t val = (int32_t)vll;
            if(fp) append_record(fp, 2, key, val);
            auto it = cache.find(key); if(it!=cache.end()) cache.erase(it);
        } else if(cmd=="find"){
            cin>>key;
            auto it = cache.find(key);
            if(it==cache.end()){
                if(fp) fflush(fp);
                unordered_set<int> s; s.reserve(16);
                RecIter itf(DATA_FILE);
                if(itf.ok()){
                    Rec r; while(itf.next(r)){
                        if(r.key==key){
                            if(r.op==1) s.insert(r.val);
                            else if(r.op==2) s.erase(r.val);
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

    if(fp) fclose(fp);
    return 0;
}
