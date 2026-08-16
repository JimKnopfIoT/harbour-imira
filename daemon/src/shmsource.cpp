/*
 * harbour-imira — shared-memory frame source (see imira-comp.cpp for the
 * writer side and the seq protocol: odd = writing, even = complete).
 */
#include "shmsource.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace imira {

namespace {

struct ShmHeader {
    uint32_t magic;
    uint32_t seq;
    uint32_t width;
    uint32_t height;
};
constexpr uint32_t kMagic = 0x31464349; // "ICF1"
const char *kShmName = "/imira-comp-fb";

} // namespace

bool ShmFrameSource::start(int fps, const FrameCallback &cb)
{
    if (m_running)
        return true;
    m_running = true;

    m_thread = std::thread([this, fps, cb]() {
        const useconds_t interval = 1000000 / fps;
        // Outer loop: (re)attach to the frame buffer. A buffer whose seq
        // freezes is stale — a leftover from a dead imira-comp whose fresh
        // instance re-created the file — so we drop it and open again.
        while (m_running) {
            int fd = -1;
            for (int i = 0; i < 100 && m_running; ++i) {
                fd = shm_open(kShmName, O_RDONLY, 0);
                if (fd >= 0)
                    break;
                usleep(100000);
            }
            if (fd < 0) {
                fprintf(stderr, "imira-castd: imira-comp frame buffer never "
                                "appeared\n");
                return;
            }
            ShmHeader hdr;
            if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)
                    || hdr.magic != kMagic || hdr.width == 0) {
                close(fd);
                usleep(200000);
                continue;
            }
            const size_t frameBytes = (size_t)hdr.width * hdr.height * 4;
            auto *shm = static_cast<const uint8_t *>(
                mmap(nullptr, sizeof(ShmHeader) + frameBytes, PROT_READ,
                     MAP_SHARED, fd, 0));
            close(fd);
            if (shm == MAP_FAILED) {
                usleep(200000);
                continue;
            }
            const auto *header = reinterpret_cast<const ShmHeader *>(shm);
            const uint8_t *frame = shm + sizeof(ShmHeader);
            fprintf(stderr, "imira-castd: comp frames %ux%u\n", hdr.width,
                    hdr.height);

            std::vector<uint8_t> copy(frameBytes);
            uint32_t lastSeq = 0;
            int idleTicks = 0;
            bool stale = false;
            while (m_running && !stale) {
                uint32_t seq =
                    __atomic_load_n(&header->seq, __ATOMIC_ACQUIRE);
                if (seq != lastSeq && (seq & 1) == 0) {
                    memcpy(copy.data(), frame, frameBytes);
                    uint32_t after =
                        __atomic_load_n(&header->seq, __ATOMIC_ACQUIRE);
                    if (after == seq) { // no tear: frame was stable
                        lastSeq = seq;
                        idleTicks = 0;
                        cb(copy.data(), (int)hdr.width, (int)hdr.height,
                           (int)hdr.width * 4, 0, /*transform=*/2);
                    }
                } else if (++idleTicks > 2 * fps) {
                    // Two seconds without a new frame: the writer is gone
                    // (or we mapped a leftover). Reattach to the current
                    // file — the live compositor keeps its seq moving.
                    fprintf(stderr, "imira-castd: comp frames stalled, "
                                    "reattaching\n");
                    stale = true;
                }
                usleep(interval);
            }
            munmap(const_cast<uint8_t *>(shm),
                   sizeof(ShmHeader) + frameBytes);
        }
    });
    return true;
}

void ShmFrameSource::stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

} // namespace imira
