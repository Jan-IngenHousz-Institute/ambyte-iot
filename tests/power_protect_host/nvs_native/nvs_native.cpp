/* The PINNED ESP-IDF NVS (framework-espidf@3.50500.0: nvs_storage.cpp,
 * nvs_page.cpp, nvs_pagemanager.cpp, nvs_types.cpp, nvs_item_hash_list.cpp,
 * compiled verbatim) on an in-memory 4-page partition, with fault injection
 * on the n-th raw entry-table write or the n-th read, bridged to C for
 * tests/power_protect_host (built with -DPP_NATIVE_NVS).
 *
 * Why (review of PR #113, round 4): what a failed write leaves behind cannot
 * be guessed from nvs.h. On this IDF a blob write can return
 * ESP_ERR_NVS_REMOVE_FAILED AFTER the new blob is on flash; an immediate read
 * then returns NOT_FOUND (Page::alterEntryState invalidated the page for this
 * boot) or the OLD value, and after a re-init (the next boot) the NEW one.
 * The production glue must be right against THAT, not against a fake. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "esp_partition.h"
#include "nvs_storage.hpp"

namespace {

class MemPartition : public nvs::Partition {
public:
    std::vector<uint8_t> bytes = std::vector<uint8_t>(4 * 4096, 0xff);
    int raw_writes = 0, fail_write_at = 0, reads = 0, fail_read_at = 0;
    const char *get_partition_name() override { return "nvs"; }
    esp_err_t read_raw(size_t off, void *dst, size_t n) override
    {
        memcpy(dst, bytes.data() + off, n);
        return ESP_OK;
    }
    esp_err_t read(size_t off, void *dst, size_t n) override
    {
        reads++;
        if (fail_read_at && reads == fail_read_at) return ESP_ERR_FLASH_OP_FAIL;
        memcpy(dst, bytes.data() + off, n);
        return ESP_OK;
    }
    esp_err_t write_raw(size_t off, const void *src, size_t n) override
    {
        raw_writes++;
        if (fail_write_at && raw_writes == fail_write_at) return ESP_ERR_FLASH_OP_FAIL;
        return write(off, src, n);
    }
    esp_err_t write(size_t off, const void *src, size_t n) override
    {
        for (size_t i = 0; i < n; i++) bytes[off + i] &= static_cast<const uint8_t *>(src)[i];   /* NOR: 1 -> 0 only */
        return ESP_OK;
    }
    esp_err_t erase_range(size_t off, size_t n) override
    {
        memset(bytes.data() + off, 0xff, n);
        return ESP_OK;
    }
    uint32_t get_address() override { return 0; }
    uint32_t get_size() override { return static_cast<uint32_t>(bytes.size()); }
    bool get_readonly() override { return false; }
};

MemPartition *g_part;
nvs::Storage *g_storage;
uint8_t g_ns;
bool g_ns_open;

}   // namespace

extern "C" {

/* A fresh, empty partition (a new device). */
void native_format(void)
{
    delete g_storage;
    g_storage = nullptr;
    delete g_part;
    g_part = new MemPartition();
    g_ns_open = false;
}

/* What a boot does: a new Storage over the same flash. */
void native_reinit(void)
{
    if (g_part == nullptr) native_format();
    delete g_storage;
    g_storage = new (std::nothrow) nvs::Storage(g_part);
    assert(g_storage != nullptr && g_storage->init(0, 4) == ESP_OK);
    g_ns_open = false;
}

/* Fail the n-th raw write / n-th read from now on (0 = none). */
void native_fault(int write_at, int read_at)
{
    g_part->raw_writes = 0;
    g_part->reads = 0;
    g_part->fail_write_at = write_at;
    g_part->fail_read_at = read_at;
}

int native_open(int readwrite)
{
    const esp_err_t e = g_storage->createOrOpenNamespace("pwr_protect", readwrite != 0, g_ns);
    g_ns_open = e == ESP_OK;
    return e;
}

int native_get_blob(void *out, size_t *len)
{
    size_t actual = 0;
    esp_err_t e = g_storage->getItemDataSize(g_ns, nvs::ItemType::BLOB, "state", actual);
    if (e != ESP_OK) return e;
    if (actual > *len) return ESP_ERR_NVS_INVALID_LENGTH;
    *len = actual;
    return g_storage->readItem(g_ns, nvs::ItemType::BLOB, "state", out, actual);
}

int native_set_blob(const void *src, size_t len)
{
    return g_storage->writeItem(g_ns, nvs::ItemType::BLOB, "state", src, len);
}

int native_get_u32(uint32_t *out)
{
    return g_storage->readItem(g_ns, nvs::ItemType::U32, "epoch", out, sizeof *out);
}

int native_set_u32(uint32_t value)
{
    return g_storage->writeItem(g_ns, nvs::ItemType::U32, "epoch", &value, sizeof value);
}

}   // extern "C"
