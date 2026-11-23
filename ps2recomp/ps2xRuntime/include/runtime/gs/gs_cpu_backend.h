#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>
#include <memory>
#include <mutex>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    // Which rows this rasteriser thread owns (8-row strips dealt round-robin) and the decoded
    // texture of the primitive being drawn. Passed down explicitly: thread_local reads cost a
    // function call per access on mingw (emulated TLS), and these are read per pixel.
    struct RasterBand
    {
        unsigned index = 0;
        unsigned count = 1;
        void *texture = nullptr;
        mutable uint64_t pixels = 0; // pixels this band wrote (graphics log)
        bool owns(int y) const { return count == 1u || ((static_cast<unsigned>(y) >> 3) % count) == index; }
    };

    void DrawPrimitive(const GSPrimitiveBatch &batch, const RasterBand &band);
    void DrawSprite(const GSPrimitiveBatch &batch, const RasterBand &band);
    void DrawTriangle(const GSPrimitiveBatch &batch, const RasterBand &band);
    void DrawLine(const GSPrimitiveBatch &batch, const RasterBand &band);
    void WritePixel(const GSDrawState &state, const RasterBand &band, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, const RasterBand &band, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);
    uint32_t ConvertTexel(const GSDrawState &state, uint32_t raw);

    // Decoded-texture cache: textures are converted to linear RGBA8888 (after CLUT / TEXA) in
    // 16x16 tiles on first use, and dropped when VRAM pages they live in are written or the CLUT
    // changes.
    struct DecodedTexture
    {
        uint32_t tbp0 = 0;
        uint8_t tbw = 0, psm = 0, tw = 0, th = 0, cpsm = 0, csm = 0, csa = 0;
        uint8_t ta0 = 0, ta1 = 0;
        bool aem = false;
        uint32_t width = 0, height = 0, tilesX = 0;
        uint32_t firstPage = 0, pageCount = 0;
        uint64_t validatedSerial = 0;
        uint64_t clutSerial = 0;
        uint64_t clutHash = 0;
        uint64_t lastUse = 0;
        std::vector<uint32_t> texels;
        std::unique_ptr<std::atomic<uint8_t>[]> tileValid;
        size_t tileCount = 0;
        std::mutex decodeMutex;
    };

    struct QueuedPrimitive
    {
        GSPrimitiveBatch batch;
        DecodedTexture *texture = nullptr;
    };

    // Fixed pool that runs one job per horizontal band set; the calling thread takes band 0.
    class WorkerPool
    {
    public:
        explicit WorkerPool(unsigned workers);
        ~WorkerPool();
        [[nodiscard]] unsigned bands() const { return static_cast<unsigned>(m_threads.size()) + 1u; }
        void run(const std::function<void(unsigned)> &job);

    private:
        void workerMain(unsigned band);
        std::vector<std::thread> m_threads;
        std::mutex m_mutex;
        std::condition_variable m_start;
        std::condition_variable m_done;
        const std::function<void(unsigned)> *m_job = nullptr;
        uint64_t m_generation = 0;
        unsigned m_pending = 0;
        bool m_stop = false;
    };

    void EnqueuePrimitive(const GSPrimitiveBatch &batch);
    void FlushQueueUnlocked(uint32_t reason = 0xFFFFFFFFu); // reason: ps2perf::Counter flush cause
    bool PrimitiveBounds(const GSPrimitiveBatch &batch, int &minX, int &minY, int &maxX, int &maxY) const;
    void MarkRegionPages(uint32_t psm, uint32_t baseBlock, uint32_t bw, int minX, int minY, int maxX, int maxY, bool pending);
    bool TexturePagesPending(const GSDrawState &state) const;
    uint64_t ClutHash(const DecodedTexture &texture) const;
    DecodedTexture *AcquireTexture(const GSDrawState &state);
    void DecodeTile(const GSDrawState &state, DecodedTexture &texture, uint32_t tile);
    void MarkVramWrite(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t wShift = m_pageWidthShift[psm & 0x3Fu];
        const uint32_t hShift = m_pageHeightShift[psm & 0x3Fu];
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, (bw * 64u) >> wShift);
        const uint32_t page = (base >> 5) + (y >> hShift) * pagesPerRow + (x >> wShift);
        const uint64_t serial = ++m_writeSerial;
        const bool lowOnly = psm == 0x01u /*CT24*/ || psm == 0x31u /*Z24*/;
        m_pageSerial[page & 511u] = serial;
        if (!lowOnly)
            m_pageSerialHigh[page & 511u] = serial;
        if (base & 31u)
        {
            m_pageSerial[(page + 1u) & 511u] = serial;
            if (!lowOnly)
                m_pageSerialHigh[(page + 1u) & 511u] = serial;
        }
    }

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;
    std::vector<std::unique_ptr<DecodedTexture>> m_textureCache;
    // Per-page write serials: m_pageSerial covers any byte; m_pageSerialHigh only writes that can
    // change bits 24-31 (CT24/Z24 writes leave them alone, which is where T8H/T4HL/T4HH textures live).
    std::array<uint64_t, 512> m_pageSerial{};
    std::array<uint64_t, 512> m_pageSerialHigh{};
    std::array<uint8_t, kPsmHandlerCount> m_pageWidthShift{};
    std::array<uint8_t, kPsmHandlerCount> m_pageHeightShift{};
    uint64_t m_writeSerial = 1;
    uint64_t m_clutSerial = 1;
    uint64_t m_textureUseCounter = 0;
    size_t m_textureCacheTexels = 0;
    std::vector<QueuedPrimitive> m_queue;
    std::array<uint8_t, 512> m_pendingPages{};
    uint64_t m_queuedPixels = 0;
    std::unique_ptr<WorkerPool> m_pool;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
