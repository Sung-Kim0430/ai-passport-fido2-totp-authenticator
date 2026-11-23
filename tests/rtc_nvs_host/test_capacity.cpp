#include "nvs_storage.hpp"
extern "C" {
#include "rtc_history_store.h"
}
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

class Nor final : public nvs::Partition {
public:
    std::vector<uint8_t> bytes;
    size_t programs = 0, erases = 0, mutations = 0, cut = 0, tear = 0;
    bool dead = false;
    std::vector<std::pair<bool, size_t>> trace;
    explicit Nor(size_t size = 65536) : bytes(size, 255) {}
    const char *get_partition_name() override { return bytes.size() == 65536 ? "rtc_hist" : "nvs"; }
    uint32_t get_address() override { return bytes.size() == 65536 ? 0x7f0000 : 0x9000; }
    uint32_t get_size() override { return static_cast<uint32_t>(bytes.size()); }
    bool get_readonly() override { return false; }
    bool bounds(size_t off, size_t n) { return off <= bytes.size() && n <= bytes.size() - off; }
    esp_err_t read_raw(size_t off, void *p, size_t n) override {
        assert(bounds(off,n)); if (dead) return ESP_FAIL;
        std::memcpy(p,bytes.data()+off,n); return ESP_OK;
    }
    esp_err_t read(size_t o, void *p, size_t n) override { return read_raw(o,p,n); }
    esp_err_t write_raw(size_t off,const void *p,size_t n) override {
        assert(bounds(off,n)); if(dead) return ESP_FAIL;
        ++programs; ++mutations; trace.emplace_back(false,n);
        bool fail = cut && mutations == cut;
        size_t count = fail ? std::min(n,tear) : n;
        auto input = static_cast<const uint8_t *>(p);
        for(size_t i=0;i<count;++i) bytes[off+i] &= input[i];
        if(fail) { dead=true; return ESP_FAIL; } return ESP_OK;
    }
    esp_err_t write(size_t o,const void *p,size_t n) override { return write_raw(o,p,n); }
    esp_err_t erase_range(size_t off,size_t n) override {
        assert(bounds(off,n) && off%4096==0 && n%4096==0);
        if(dead) return ESP_FAIL;
        ++erases; ++mutations; trace.emplace_back(true,n);
        bool fail = cut && mutations == cut;
        size_t count=fail ? std::min(n,tear) : n;
        std::fill_n(bytes.begin()+off,count,255);
        if(fail) { dead=true; return ESP_FAIL; } return ESP_OK;
    }
    void reset_fault() { cut=0; dead=false; mutations=0; trace.clear(); }
};

struct Engine {
    Nor &nor;
    nvs::Storage storage;
    uint8_t ns=0;
    int operation=0, fail=0;
    bool ack=false;
    explicit Engine(Nor &n) : nor(n), storage(&n) {}
    bool init(bool create=true) {
        if(storage.init(0,nor.get_size()/4096)!=ESP_OK) return false;
        return storage.createOrOpenNamespace("hist",create,ns)==ESP_OK;
    }
    bool fault() { return ++operation==fail; }
    static rtc_history_store_result_t read(void *u,const char *k,void *p,size_t *n) {
        auto &e=*static_cast<Engine *>(u);
        if(e.fault()) return RTC_HISTORY_STORE_IO_ERROR;
        size_t size=0;
        auto err=e.storage.getItemDataSize(e.ns,nvs::ItemType::BLOB,k,size);
        if(err==ESP_ERR_NVS_NOT_FOUND) return RTC_HISTORY_STORE_MISSING;
        if(err!=ESP_OK) return RTC_HISTORY_STORE_IO_ERROR;
        if(size>*n) return RTC_HISTORY_STORE_BLOCKED;
        *n=size;
        return e.storage.readItem(e.ns,nvs::ItemType::BLOB,k,p,size)==ESP_OK ?
            RTC_HISTORY_STORE_VALID : RTC_HISTORY_STORE_IO_ERROR;
    }
    static bool write(void *u,const char *k,const void *p,size_t n) {
        auto &e=*static_cast<Engine *>(u); bool fail=e.fault();
        if(fail && !e.ack) return false;
        auto err=e.storage.writeItem(e.ns,nvs::ItemType::BLOB,k,p,n);
        return err==ESP_OK && !fail;
    }
    static bool commit(void *u) { return !static_cast<Engine *>(u)->fault(); }
    rtc_history_store_io_t io() { return {this,read,write,commit}; }
};
static rtc_history_record_t sample(uint64_t n) {
    rtc_history_record_t r={};
    r.source=2; r.sequence=n; r.epoch=1+n/41;
    r.utc_end_us=UINT64_C(1700000000000000)+n*RTC_HISTORY_MIN_RAW_US;
    r.raw={RTC_HISTORY_MIN_RAW_US+n,static_cast<uint32_t>(n*71%RTC_DRIFT_Q19)};
    r.utc_delta_us=RTC_HISTORY_MIN_RAW_US;
    r.flags=15; r.temperature_centi_c=static_cast<int16_t>(n%8000);
    r.battery_mv=static_cast<uint16_t>(3000+n%1000);
    r.temp_age_s=static_cast<uint16_t>(n%65536); r.battery_age_s=static_cast<uint16_t>((n*3)%65536);
    r.combined_uncertainty_us=n%1000;
    return r;
}
static void verify(const rtc_history_t &h,uint64_t n,uint64_t epoch_offset=0) {
    assert(h.generation==n && h.count==(n>100?100:n) && h.next==n%100);
    uint64_t watermark=n>100 ? sample(n-100).utc_end_us : 0;
    assert(h.evicted_utc_end_us==watermark);
    for(size_t i=0;i<h.count;++i) {
        rtc_history_record_t got, want=sample(n-h.count+i+1);
        want.epoch+=epoch_offset;
        uint8_t a[80],b[80];
        assert(rtc_history_get(&h,i,&got));
        assert(rtc_history_encode(&got,a,80) && rtc_history_encode(&want,b,80));
        assert(!std::memcmp(a,b,80));
    }
}
static void recover(Nor &nor,uint64_t old,uint64_t next,size_t &blocked) {
    nor.reset_fault();
    Engine e(nor);
    if(!e.init()) { ++blocked; return; }
    rtc_history_t h; rtc_history_store_t s;
    auto r=rtc_history_store_open(&s,&h,e.io(),2);
    if(r==RTC_HISTORY_STORE_BLOCKED || r==RTC_HISTORY_STORE_IO_ERROR) { ++blocked; return; }
    assert(r==RTC_HISTORY_STORE_VALID || (old==0 && r==RTC_HISTORY_STORE_MISSING));
    assert(h.generation==old || h.generation==next);
    verify(h,h.generation);
    assert(s.checkpoint.accepted_sequence==h.generation && s.checkpoint.dropped==0);
}
static void checkpoints() {
    Nor nor;
    {
        Engine e(nor); assert(e.init()); rtc_history_t h; rtc_history_store_t s;
        assert(rtc_history_store_open(&s,&h,e.io(),2)==RTC_HISTORY_STORE_MISSING);
        assert(s.checkpoint.accepted_sequence==0 && s.checkpoint.dropped==0);
        auto r=sample(1);
        rtc_history_store_checkpoint_t cp={4,2};
        assert(rtc_history_store_append(&s,&r,&cp)==RTC_HISTORY_STORE_VALID);
        assert(s.checkpoint.accepted_sequence==4 && s.checkpoint.dropped==2);
    }
    {
        Engine e(nor); assert(e.init()); rtc_history_t h; rtc_history_store_t s;
        assert(rtc_history_store_open(&s,&h,e.io(),2)==RTC_HISTORY_STORE_VALID);
        assert(s.checkpoint.accepted_sequence==4 && s.checkpoint.dropped==2);
        rtc_history_record_t last;
        assert(rtc_history_get(&h,0,&last) && last.sequence==1 && h.generation==1);
        auto r=sample(2);
        assert(rtc_history_store_append(&s,&r,NULL)==RTC_HISTORY_STORE_VALID);
        assert(s.checkpoint.accepted_sequence==4 && s.checkpoint.dropped==2);
    }
    {
        Engine e(nor); assert(e.init()); rtc_history_t h; rtc_history_store_t s;
        assert(rtc_history_store_open(&s,&h,e.io(),2)==RTC_HISTORY_STORE_VALID);
        assert(s.checkpoint.accepted_sequence==4 && s.checkpoint.dropped==2);
        rtc_history_record_t last;
        assert(rtc_history_get(&h,1,&last) && last.sequence==2 && h.generation==2);
    }
}
static void mode_capacity() {
    Nor nor;
    {
        Engine e(nor); assert(e.init());
        uint8_t namespaces[2]={e.ns,0};
        assert(e.storage.createOrOpenNamespace("hist_freq",true,namespaces[1])==ESP_OK);
        rtc_history_t rings[2]; rtc_history_store_t stores[2];
        for(unsigned mode=0;mode<2;++mode) {
            e.ns=namespaces[mode];
            assert(rtc_history_store_open(&stores[mode],&rings[mode],e.io(),2)==RTC_HISTORY_STORE_MISSING);
        }
        for(uint64_t n=1;n<=650;++n) for(unsigned mode=0;mode<2;++mode) {
            e.ns=namespaces[mode];
            auto row=sample(n); row.epoch+=mode*100000;
            assert(rtc_history_store_append(&stores[mode],&row,NULL)==RTC_HISTORY_STORE_VALID);
            verify(rings[mode],n,mode*100000);
        }
        nvs_stats_t stats={}; assert(e.storage.fillStats(stats)==ESP_OK);
        std::printf("mode capacity: bytes=%zu rows=%zu+%zu used_entries=%zu free_entries=%zu namespaces=%zu erases=%zu\n",
            nor.bytes.size(),rings[0].count,rings[1].count,stats.used_entries,stats.free_entries,
            stats.namespace_count,nor.erases);
    }
    {
        Engine e(nor); assert(e.init(false));
        for(const char *name : {"hist", "hist_freq"}) {
            assert(e.storage.createOrOpenNamespace(name,false,e.ns)==ESP_OK);
            rtc_history_t ring; rtc_history_store_t store;
            assert(rtc_history_store_open(&store,&ring,e.io(),2)==RTC_HISTORY_STORE_VALID);
            verify(ring,650,!std::strcmp(name,"hist")?0:100000);
            assert(store.checkpoint.accepted_sequence==650 && store.checkpoint.dropped==0);
        }
    }
}

int main() {
    mode_capacity();
    checkpoints();
    Nor security(24576);
    {
        nvs::Storage storage(&security); assert(storage.init(0,6)==ESP_OK);
        for(const char *name : {"auth", "fido", "rtc_cal"}) {
            uint8_t ns; assert(storage.createOrOpenNamespace(name,true,ns)==ESP_OK);
            uint8_t fixture[256]; for(size_t i=0;i<sizeof fixture;++i) fixture[i]=static_cast<uint8_t>(i*17);
            assert(storage.writeItem(ns,nvs::ItemType::BLOB,"fixture",fixture,sizeof fixture)==ESP_OK);
        }
    }
    const auto security_original=security.bytes;
    Nor nor;
    std::vector<uint8_t> before_gc;
    uint64_t gc_generation=0;
    {
        Engine e(nor); assert(e.init());
        rtc_history_t h; rtc_history_store_t s;
        assert(rtc_history_store_open(&s,&h,e.io(),2)==RTC_HISTORY_STORE_MISSING);
        for(uint64_t n=1;n<=650;++n) {
            auto before=nor.bytes; auto erases=nor.erases;
            auto r=sample(n);
            assert(rtc_history_store_append(&s, &r, NULL)==RTC_HISTORY_STORE_VALID);
            verify(h,n);
            if(n>110 && nor.erases>erases && before_gc.empty()) {
                before_gc=std::move(before); gc_generation=n;
            }
        }
        nvs_stats_t stats={}; assert(e.storage.fillStats(stats)==ESP_OK);
        std::printf("capacity: bytes=%zu pages=16 rows=%zu programs=%zu erases=%zu used_entries=%zu free_entries=%zu namespaces=%zu\n",
            nor.bytes.size(),h.count,nor.programs,nor.erases,stats.used_entries,stats.free_entries,stats.namespace_count);
    }
    assert(nor.erases>10 && gc_generation>110);
    size_t blocked=0,cases=0;
    recover(nor,650,650,blocked); assert(blocked==0);
    for(bool empty : {false,true}) for(bool ack : {false,true}) for(int boundary=1;boundary<=7;++boundary) {
        Nor test;
        if(!empty) test.bytes=before_gc;
        {
            Engine e(test); assert(e.init());
            rtc_history_t h; rtc_history_store_t s;
            auto result=rtc_history_store_open(&s,&h,e.io(),2);
            assert(result==(empty?RTC_HISTORY_STORE_MISSING:RTC_HISTORY_STORE_VALID));
            e.operation=0; e.fail=boundary; e.ack=ack;
            auto r=sample(empty?1:gc_generation);
            result=rtc_history_store_append(&s, &r, NULL);
            assert(result==RTC_HISTORY_STORE_IO_ERROR || result==RTC_HISTORY_STORE_VALID);
            if(result==RTC_HISTORY_STORE_IO_ERROR) verify(h,empty?0:gc_generation-1);
        }
        recover(test,empty?0:gc_generation-1,empty?1:gc_generation,blocked); ++cases;
    }
    Nor probe; probe.bytes=before_gc;
    {
        Engine e(probe); assert(e.init()); rtc_history_t h; rtc_history_store_t s;
        assert(rtc_history_store_open(&s,&h,e.io(),2)==RTC_HISTORY_STORE_VALID);
        probe.reset_fault(); auto r=sample(gc_generation);
        assert(rtc_history_store_append(&s, &r, NULL)==RTC_HISTORY_STORE_VALID);
    }
    const auto trace=probe.trace;
    assert(std::any_of(trace.begin(),trace.end(),[](auto op){return op.first;}));
    for(size_t operation=1;operation<=trace.size();++operation) {
        size_t length=trace[operation-1].second;
        for(size_t tear : {size_t(0),size_t(1),length/2,length-1,length}) {
            Nor test; test.bytes=before_gc;
            {
                Engine e(test); assert(e.init()); rtc_history_t h; rtc_history_store_t s;
                assert(rtc_history_store_open(&s,&h,e.io(),2)==RTC_HISTORY_STORE_VALID);
                test.reset_fault(); test.cut=operation; test.tear=tear;
                auto r=sample(gc_generation);
                auto result=rtc_history_store_append(&s, &r, NULL);
                assert(result==RTC_HISTORY_STORE_IO_ERROR);
                verify(h,gc_generation-1);
            }
            recover(test,gc_generation-1,gc_generation,blocked); ++cases;
        }
    }
    assert(security.bytes==security_original);
    {
        nvs::Storage storage(&security); assert(storage.init(0,6)==ESP_OK);
        for(const char *name : {"auth", "fido", "rtc_cal"}) {
            uint8_t ns; assert(storage.createOrOpenNamespace(name,false,ns)==ESP_OK);
            uint8_t fixture[256]; assert(storage.readItem(ns,nvs::ItemType::BLOB,"fixture",fixture,sizeof fixture)==ESP_OK);
            for(size_t i=0;i<sizeof fixture;++i) assert(fixture[i]==static_cast<uint8_t>(i*17));
        }
    }
    assert(security.bytes==security_original);
    std::printf("PASS actual SDK 5.5.6 BLOB COW GC, all-fields reopen, security isolation; protocol/NOR fault cases=%zu blocked=%zu mutation_boundaries=%zu gc_generation=%llu\n",
        cases,blocked,trace.size(),static_cast<unsigned long long>(gc_generation));
}
